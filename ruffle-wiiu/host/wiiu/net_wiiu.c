// Network connections for games (Flash Socket/SecureSocket, and HTTP built on top of them in navigator.rs).
//
// Each connection gets a thread of its own on core 0. It does the blocking work (DNS, connect, the TLS
// handshake with mbedTLS, reads and writes), so the game thread only ever moves bytes in and out of
// buffers. The console's own TLS isn't used: its root certificates are years out of date. Ours come
// from content/cacert.pem (Mozilla's list, as shipped by curl).
#include "../rw_core.h"
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <stdarg.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/x509_crt.h>

#define MAX_SOCKS 16
#define STACK_SIZE (128 * 1024)
#define CA_FILE "fs:/vol/content/cacert.pem"

enum { ST_CONNECTING = 0, ST_OPEN = 1, ST_FAILED = 2, ST_CLOSED = 3 };

typedef struct {
    uint8_t* data;
    uint32_t len, cap;
} Buf;

typedef struct {
    int used;          // slot taken (until the game closed it and the thread is gone)
    int game_closed;   // the game called close: the thread should stop
    int thread_done;   // the thread returned
    volatile int state;
    char host[256];
    uint32_t port;
    int tls;
    Buf in, out;       // received but not yet read by the game / given by the game but not yet sent
    OSMutex lock;
    OSThread* thread;
    uint8_t* stack;
} Sock;

static Sock g_socks[MAX_SOCKS];
static OSMutex g_ca_lock;
static int g_ca_lock_ready, g_ca_loaded;
static mbedtls_x509_crt g_ca;

// (from mbedtls/net_sockets.h, which isn't built here)
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif

static void logf_net(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_net(int level, const char* fmt, ...) {
    char m[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    plat_log(level, m);
}

static int buf_put(Buf* b, const uint8_t* p, uint32_t n) {
    if (b->len + n > b->cap) {
        uint32_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        uint8_t* d = (uint8_t*)realloc(b->data, cap);
        if (!d) return 0;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

static void buf_take(Buf* b, uint32_t n) {
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

// mbedTLS asks for random bytes (MBEDTLS_ENTROPY_HARDWARE_ALT). The console has no simple source of
// real randomness for homebrew, so this mixes the CPU's fast timer with a running state.
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    static uint64_t s;
    if (!s) s = (uint64_t)OSGetTime() ^ 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < len; i++) {
        s ^= (uint64_t)OSGetSystemTick() * 0xBF58476D1CE4E5B9ull;
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        output[i] = (unsigned char)(s >> 24);
    }
    *olen = len;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)OSTicksToMilliseconds(OSGetTime());
}

static void load_ca(void) {
    OSLockMutex(&g_ca_lock);
    if (!g_ca_loaded) {
        mbedtls_x509_crt_init(&g_ca);
        int r = mbedtls_x509_crt_parse_file(&g_ca, CA_FILE);
        if (r < 0) logf_net(1, "net: couldn't read %s (error -0x%x): certificates won't be checked", CA_FILE, (unsigned)-r);
        else logf_net(2, "net: certificate list loaded (%d skipped)", r);
        g_ca_loaded = 1;
    }
    OSUnlockMutex(&g_ca_lock);
}

static int net_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    int n = send(fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return n;
}

static int net_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    int n = recv(fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return n;
}

static int open_tcp(Sock* s) {
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)s->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(s->host, port, &hints, &res);
    if (r != 0 || !res) {
        logf_net(1, "net: can't find %s (error %d)", s->host, r);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        logf_net(1, "net: can't connect to %s:%u (errno %d)", s->host, (unsigned)s->port, errno);
        return -1;
    }
#ifdef TCP_NODELAY
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    return fd;
}

static int wait_readable(int fd, int ms) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = { 0, ms * 1000 };
    return select(fd + 1, &r, NULL, NULL, &tv) > 0;
}

static int sock_main(int argc, const char** argv) {
    (void)argv;
    Sock* s = &g_socks[argc];
    int fd = -1, tls_ready = 0;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    if (s->tls) {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    OSTime t0 = OSGetSystemTime();
    fd = open_tcp(s);
    if (fd < 0) goto fail;

    if (s->tls) {
        load_ca();
        int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)"rw", 2);
        if (!r) r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (!r) {
            // Optional: a console with a wrong clock would otherwise fail every check. Problems are logged.
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
            mbedtls_ssl_conf_ca_chain(&conf, &g_ca, NULL);
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
            r = mbedtls_ssl_setup(&ssl, &conf);
        }
        if (!r) r = mbedtls_ssl_set_hostname(&ssl, s->host);
        if (!r) {
            mbedtls_ssl_set_bio(&ssl, &fd, net_send, net_recv, NULL);
            while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                if (s->game_closed) break;
        }
        if (r) {
            char e[100];
            mbedtls_strerror(r, e, sizeof(e));
            logf_net(1, "net: TLS with %s failed: %s (-0x%x)", s->host, e, (unsigned)-r);
            goto fail;
        }
        tls_ready = 1;
        uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
        if (flags) {
            char info[200];
            mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
            for (char* c = info; *c; c++)
                if (*c == '\n') *c = ' ';
            logf_net(1, "net: %s's certificate didn't check out (continuing): %s", s->host, info);
        }
    }
    logf_net(2, "net: connected to %s:%u%s in %u ms", s->host, (unsigned)s->port, s->tls ? " (TLS)" : "",
             (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
    s->state = ST_OPEN;

    uint8_t* chunk = (uint8_t*)malloc(16384);
    if (!chunk) goto fail;
    while (!s->game_closed) {
        // send what the game gave
        OSLockMutex(&s->lock);
        uint32_t n = s->out.len < 16384 ? s->out.len : 16384;
        if (n) memcpy(chunk, s->out.data, n);
        OSUnlockMutex(&s->lock);
        if (n) {
            int w;
            if (s->tls) {
                while ((w = mbedtls_ssl_write(&ssl, chunk, n)) == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ) {}
            } else {
                w = send(fd, chunk, n, 0);
            }
            if (w <= 0) {
                logf_net(1, "net: sending to %s failed (%d)", s->host, w);
                break;
            }
            OSLockMutex(&s->lock);
            buf_take(&s->out, (uint32_t)w);
            OSUnlockMutex(&s->lock);
        }
        // receive
        int pending = s->tls && mbedtls_ssl_get_bytes_avail(&ssl) > 0;
        if (!pending && !wait_readable(fd, n ? 0 : 4)) continue;
        int got;
        if (s->tls) {
            got = mbedtls_ssl_read(&ssl, chunk, 16384);
            if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
            if (got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        } else {
            got = recv(fd, chunk, 16384, 0);
        }
        if (got <= 0) {
            if (got != 0 && got != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) logf_net(1, "net: %s: connection lost (%d)", s->host, got);
            break;
        }
        OSLockMutex(&s->lock);
        int ok = buf_put(&s->in, chunk, (uint32_t)got);
        OSUnlockMutex(&s->lock);
        if (!ok) {
            logf_net(0, "net: out of memory receiving from %s", s->host);
            break;
        }
    }
    free(chunk);
    s->state = ST_CLOSED;
    goto done;

fail:
    s->state = ST_FAILED;
done:
    if (s->tls) {
        if (tls_ready && s->state == ST_CLOSED && s->game_closed) mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
    if (fd >= 0) close(fd);
    s->thread_done = 1;
    return 0;
}

// Frees slots whose game side and thread are both finished.
static void reap(void) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        Sock* s = &g_socks[i];
        if (!s->used || !s->game_closed || !s->thread_done) continue;
        int result;
        OSJoinThread(s->thread, &result);
        free(s->thread);
        free(s->stack);
        free(s->in.data);
        free(s->out.data);
        memset(s, 0, sizeof(*s));
    }
}

uint32_t plat_sock_open(const char* host, uint32_t port, int tls) {
    if (!g_ca_lock_ready) {
        OSInitMutex(&g_ca_lock);
        g_ca_lock_ready = 1;
    }
    reap();
    int i = 0;
    while (i < MAX_SOCKS && g_socks[i].used) i++;
    if (i == MAX_SOCKS) {
        logf_net(1, "net: too many connections open");
        return 0;
    }
    Sock* s = &g_socks[i];
    memset(s, 0, sizeof(*s));
    snprintf(s->host, sizeof(s->host), "%s", host);
    s->port = port;
    s->tls = tls;
    s->state = ST_CONNECTING;
    OSInitMutex(&s->lock);
    s->thread = (OSThread*)memalign(16, sizeof(OSThread));
    s->stack = (uint8_t*)memalign(16, STACK_SIZE);
    if (!s->thread || !s->stack) {
        free(s->thread);
        free(s->stack);
        memset(s, 0, sizeof(*s));
        return 0;
    }
    memset(s->thread, 0, sizeof(OSThread));
    if (!OSCreateThread(s->thread, sock_main, i, NULL, s->stack + STACK_SIZE, STACK_SIZE, 16, OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
        free(s->thread);
        free(s->stack);
        memset(s, 0, sizeof(*s));
        return 0;
    }
    s->used = 1;
    logf_net(2, "net: connecting to %s:%u%s", host, (unsigned)port, tls ? " (TLS)" : "");
    OSResumeThread(s->thread);
    return (uint32_t)i + 1;
}

static Sock* get(uint32_t id) {
    if (!id || id > MAX_SOCKS || !g_socks[id - 1].used || g_socks[id - 1].game_closed) return NULL;
    return &g_socks[id - 1];
}

int plat_sock_state(uint32_t id) {
    Sock* s = get(id);
    return s ? s->state : ST_FAILED;
}

uint32_t plat_sock_send(uint32_t id, const uint8_t* data, uint32_t len) {
    Sock* s = get(id);
    if (!s || s->state == ST_FAILED || s->state == ST_CLOSED) return 0;
    OSLockMutex(&s->lock);
    int ok = buf_put(&s->out, data, len);
    OSUnlockMutex(&s->lock);
    return ok ? len : 0;
}

uint32_t plat_sock_recv(uint32_t id, uint8_t* buf, uint32_t cap) {
    Sock* s = get(id);
    if (!s) return 0;
    OSLockMutex(&s->lock);
    uint32_t n = s->in.len < cap ? s->in.len : cap;
    if (n) {
        memcpy(buf, s->in.data, n);
        buf_take(&s->in, n);
    }
    OSUnlockMutex(&s->lock);
    return n;
}

void plat_sock_close(uint32_t id) {
    if (!id || id > MAX_SOCKS || !g_socks[id - 1].used) return;
    g_socks[id - 1].game_closed = 1;
    reap();
}
