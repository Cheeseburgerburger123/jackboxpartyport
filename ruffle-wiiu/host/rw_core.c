// wasm2c instance, wasm memory access and the WASI subset Ruffle uses.
//
// On big-endian hosts (WABT_BIG_ENDIAN) wasm2c stores linear memory byte-reversed: wasm address
// a..a+n lives at data_end - a - n, which makes multi-byte loads native. So 32-bit words read
// natively, but byte strings (paths, pixels, file data) must be copied in reverse.
#include "rw_core.h"
#include "flashplayer.h"
#include "wasm-rt-impl.h"
#include "wasm-rt-exceptions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#ifdef __WIIU__
#include "wiiu/fastmem.h"
#endif

struct w2c_rw { int unused; };
struct w2c_wasi__snapshot__preview1 { int unused; };

static w2c_flashplayer g_inst;
static struct w2c_rw g_rw;
static struct w2c_wasi__snapshot__preview1 g_wasi;
static int g_dead;
static char g_root[512];
static char g_content[128];

void rw_core_set_content(const char* dir) {
    snprintf(g_content, sizeof(g_content), "%s", dir);
}

// ---------------------------------------------------------------- memory

static wasm_rt_memory_t* mem(void) { return w2c_flashplayer_memory(&g_inst); }

static int in_bounds(uint32_t addr, uint32_t n) {
    return (uint64_t)addr + n <= mem()->size;
}

static uint32_t rd32(uint32_t addr) {
    uint32_t v;
#if WABT_BIG_ENDIAN
    memcpy(&v, mem()->data_end - addr - 4, 4);
#else
    memcpy(&v, mem()->data + addr, 4);
#endif
    return v;
}

static void wr32(uint32_t addr, uint32_t v) {
#if WABT_BIG_ENDIAN
    memcpy(mem()->data_end - addr - 4, &v, 4);
#else
    memcpy(mem()->data + addr, &v, 4);
#endif
}

static void wr64(uint32_t addr, uint64_t v) {
#if WABT_BIG_ENDIAN
    memcpy(mem()->data_end - addr - 8, &v, 8);
#else
    memcpy(mem()->data + addr, &v, 8);
#endif
}

static void wr8(uint32_t addr, uint8_t v) {
#if WABT_BIG_ENDIAN
    mem()->data_end[-(ptrdiff_t)addr - 1] = v;
#else
    mem()->data[addr] = v;
#endif
}

static void copy_out(void* dst, uint32_t addr, uint32_t n) {
#if WABT_BIG_ENDIAN
    const uint8_t* src = mem()->data_end - addr - n;
    uint8_t* d = (uint8_t*)dst;
    uint32_t i = 0;
    // A word at a time with a byte-swapped load (byte by byte took most of a big picture upload's time).
    for (; i + 4 <= n; i += 4) {
        uint32_t v;
        memcpy(&v, src + n - 4 - i, 4);
        v = __builtin_bswap32(v);
        memcpy(d + i, &v, 4);
    }
    for (; i < n; i++) d[i] = src[n - 1 - i];
#else
    memcpy(dst, mem()->data + addr, n);
#endif
}

static void copy_in(uint32_t addr, const void* src, uint32_t n) {
#if WABT_BIG_ENDIAN
    uint8_t* d = mem()->data_end - addr - n;
    const uint8_t* s = (const uint8_t*)src;
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        v = __builtin_bswap32(v);
        memcpy(d + n - 4 - i, &v, 4);
    }
    for (; i < n; i++) d[n - 1 - i] = s[i];
#else
    memcpy(mem()->data + addr, src, n);
#endif
}

// ---------------------------------------------------------------- rw imports

u32 w2c_rw_host_mesh_create(struct w2c_rw* rw, u32 verts, u32 nverts, u32 indices, u32 nidx) {
    (void)rw;
    if (!in_bounds(verts, nverts * 12) || !in_bounds(indices, nidx * 4)) return 0;
    RwVertex* v = (RwVertex*)malloc(sizeof(RwVertex) * (nverts ? nverts : 1));
    uint32_t* idx = (uint32_t*)malloc(4 * (nidx ? nidx : 1));
    if (!v || !idx) { // out of memory: this shape isn't drawn (writing through NULL froze the console)
        free(v);
        free(idx);
        return 0;
    }
    for (u32 i = 0; i < nverts; i++) {
        uint32_t x = rd32(verts + i * 12), y = rd32(verts + i * 12 + 4);
        memcpy(&v[i].x, &x, 4);
        memcpy(&v[i].y, &y, 4);
        v[i].rgba = rd32(verts + i * 12 + 8);
    }
    for (u32 i = 0; i < nidx; i++) idx[i] = rd32(indices + i * 4);
    uint32_t id = gfx_mesh_create(v, nverts, idx, nidx);
    free(v);
    free(idx);
    return id;
}

void w2c_rw_host_mesh_free(struct w2c_rw* rw, u32 id) { (void)rw; gfx_mesh_free(id); }

static uint8_t* pixels_out(u32 addr, u32 w, u32 h, u32 stride) {
    uint8_t* p = (uint8_t*)malloc((size_t)w * h * 4 + 1);
    if (!p) return NULL; // out of memory
    for (u32 y = 0; y < h; y++) copy_out(p + (size_t)y * w * 4, addr + y * stride, w * 4);
    return p;
}

u32 w2c_rw_host_tex_create(struct w2c_rw* rw, u32 w, u32 h, u32 rgba, u32 stride) {
    (void)rw;
    if (!rgba) return gfx_tex_create(w, h, NULL, w * 4);
    if (h && !in_bounds(rgba, stride * (h - 1) + w * 4)) return 0;
    uint8_t* p = pixels_out(rgba, w, h, stride);
    // (Out of memory: a blank texture of the right size, or texture 0 if there's no room for that either.)
    uint32_t id = gfx_tex_create(w, h, p, w * 4);
    free(p);
    return id;
}

void w2c_rw_host_tex_update(struct w2c_rw* rw, u32 id, u32 x, u32 y, u32 w, u32 h, u32 rgba, u32 stride) {
    (void)rw;
    if (h && !in_bounds(rgba, stride * (h - 1) + w * 4)) return;
#ifdef __WIIU__
    // Straight into the picture: going through a copy cost as much again in games updating big pictures every frame.
    uint32_t pitch;
    uint8_t* d = gfx_tex_update_begin(id, x, y, &pitch);
    if (!d) return;
    for (u32 row = 0; row < h; row++) copy_out(d + (size_t)row * pitch, rgba + row * stride, w * 4);
    gfx_tex_update_end(id, y, h);
#else
    uint8_t* p = pixels_out(rgba, w, h, stride);
    if (!p) return;
    gfx_tex_update(id, x, y, w, h, p, w * 4);
    free(p);
#endif
}

void w2c_rw_host_tex_mip(struct w2c_rw* rw, u32 id, u32 level, u32 w, u32 h, u32 rgba, u32 stride) {
    (void)rw;
    if (!h || !in_bounds(rgba, stride * (h - 1) + w * 4)) return;
    uint8_t* p = pixels_out(rgba, w, h, stride);
    if (!p) return;
    gfx_tex_mip(id, level, w, h, p, w * 4);
    free(p);
}

#ifdef __WIIU__
#include <coreinit/thread.h>
typedef struct { const PerlinJob* job; uint32_t y0, y1; } PerlinWork;
static int perlin_thread_main(int argc, const char** argv) {
    (void)argc;
    const PerlinWork* w = (const PerlinWork*)argv;
    perlin_rows(w->job, w->y0, w->y1);
    return 0;
}
static OSThread g_perlin_threads[2] __attribute__((aligned(16)));
static uint8_t g_perlin_stacks[2][32 * 1024] __attribute__((aligned(16)));
#endif

// The rows of a noise picture, on every core the console has (the game's thread runs on core 1).
static void perlin_run(const PerlinJob* job, uint32_t h) {
#ifdef __WIIU__
    if (h >= 48) {
        static const uint32_t cores[2] = { OS_THREAD_ATTRIB_AFFINITY_CPU0, OS_THREAD_ATTRIB_AFFINITY_CPU2 };
        PerlinWork work[2];
        int started[2] = { 0, 0 };
        uint32_t third = h / 3;
        for (int i = 0; i < 2; i++) {
            work[i].job = job;
            work[i].y0 = (uint32_t)i * third;
            work[i].y1 = (uint32_t)(i + 1) * third;
            started[i] = OSCreateThread(&g_perlin_threads[i], perlin_thread_main, 0, (char*)&work[i],
                                        g_perlin_stacks[i] + sizeof(g_perlin_stacks[i]), sizeof(g_perlin_stacks[i]), 16, cores[i]);
            if (started[i]) OSResumeThread(&g_perlin_threads[i]);
        }
        perlin_rows(job, 2 * third, h);
        for (int i = 0; i < 2; i++) {
            if (started[i]) OSJoinThread(&g_perlin_threads[i], NULL);
            else perlin_rows(job, work[i].y0, work[i].y1);
        }
        return;
    }
#endif
    perlin_rows(job, 0, h);
}

u32 w2c_rw_host_perlin_noise(struct w2c_rw* rw, u32 pixels, u32 w, u32 h, f64 base_x, f64 base_y, u32 octaves, u64 seed,
                             u32 flags, u32 offsets, u32 offsets_len) {
    (void)rw;
    if (!w || !h || offsets_len < octaves * 2 || !in_bounds(pixels, w * h * 4) || !in_bounds(offsets, offsets_len * 8)) return 0;
    double* offs = (double*)malloc((size_t)offsets_len * 8 + 8);
    uint8_t* out = (uint8_t*)malloc((size_t)w * h * 4);
    PerlinJob* job = NULL;
    if (offs && out) {
        copy_out(offs, offsets, offsets_len * 8);
#if WABT_BIG_ENDIAN
        // (copy_out gives the bytes in wasm order, little-endian: each double needs turning round.)
        for (u32 i = 0; i < offsets_len; i++) {
            uint64_t v;
            memcpy(&v, &offs[i], 8);
            v = __builtin_bswap64(v);
            memcpy(&offs[i], &v, 8);
        }
#endif
        job = perlin_job_new(w, h, base_x, base_y, octaves, (int64_t)seed, flags, offs, out);
    }
    if (job) {
        perlin_run(job, h);
        copy_in(pixels, out, w * h * 4);
    }
    perlin_job_free(job);
    free(offs);
    free(out);
    return job != NULL;
}

// Fills `count` words of wasm memory with one value: as a wasm loop this was a large part of BitmapData.fillRect.
void w2c_rw_host_fill_words(struct w2c_rw* rw, u32 addr, u32 count, u32 value) {
    (void)rw;
    if (!in_bounds(addr, count * 4)) return;
#if WABT_BIG_ENDIAN
    uint32_t* d = (uint32_t*)(mem()->data_end - addr - count * 4);
#else
    uint32_t* d = (uint32_t*)(mem()->data + addr);
#endif
#ifdef __WIIU__
    if (((uintptr_t)d & 3) == 0) { rw_fill_words_fast(d, count, value); return; } // (a fifth of Moto X3M's time)
#endif
    for (u32 i = 0; i < count; i++) d[i] = value;
}

void w2c_rw_host_tex_free(struct w2c_rw* rw, u32 id) { (void)rw; gfx_tex_free(id); }

u32 w2c_rw_host_h264_open(struct w2c_rw* rw, u32 w, u32 h) {
    (void)rw;
    return gfx_h264_open(w, h);
}

u32 w2c_rw_host_h264_decode(struct w2c_rw* rw, u32 id, u32 tex, u32 data, u32 len) {
    (void)rw;
    if (!in_bounds(data, len)) return 0;
    uint8_t* p = (uint8_t*)malloc(len + 1);
    if (!p) return 0;
    copy_out(p, data, len);
    u32 done = (u32)gfx_h264_decode(id, tex, p, len);
    free(p);
    return done;
}

void w2c_rw_host_h264_close(struct w2c_rw* rw, u32 id) {
    (void)rw;
    gfx_h264_close(id);
}

void w2c_rw_host_tex_copy(struct w2c_rw* rw, u32 dst, u32 src) {
    (void)rw;
    gfx_tex_copy(dst, src);
}

static float* floats_out(u32 addr, u32 n) {
    if (!in_bounds(addr, n * 4)) return NULL;
    float* f = (float*)malloc(n * 4 + 4);
    if (!f) return NULL;
    for (u32 i = 0; i < n; i++) {
        uint32_t v = rd32(addr + i * 4);
        memcpy(&f[i], &v, 4);
    }
    return f;
}

u32 w2c_rw_host_tex_create_float(struct w2c_rw* rw, u32 w, u32 h, u32 rgba) {
    (void)rw;
    float* f = floats_out(rgba, w * h * 4);
    if (!f) return 0;
    u32 id = gfx_tex_create_float(w, h, f);
    free(f);
    return id;
}

u32 w2c_rw_host_tex_create_float_target(struct w2c_rw* rw, u32 w, u32 h) {
    (void)rw;
    return gfx_tex_create_float(w, h, NULL);
}

void w2c_rw_host_tex_read_float(struct w2c_rw* rw, u32 id, u32 w, u32 h, u32 out) {
    (void)rw;
    u32 n = w * h * 4;
    if (!in_bounds(out, n * 4)) return;
    float* f = (float*)malloc(n * 4 + 4);
    if (!f) return;
    gfx_tex_read_float(id, w, h, f);
    for (u32 i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, &f[i], 4);
        wr32(out + i * 4, v);
    }
    free(f);
}

u32 w2c_rw_host_pb_run(struct w2c_rw* rw, u32 program, u32 target, u32 textures, u32 floats, u32 nfloats, u32 ints,
                       u32 nints, u32 zero_outside, u32 linear_mask) {
    (void)rw;
    uint32_t tex[8];
    if (!in_bounds(textures, 32)) return 0;
    for (u32 i = 0; i < 8; i++) tex[i] = rd32(textures + i * 4);
    float* f = floats_out(floats, nfloats);
    float* n = floats_out(ints, nints); // (just words: the host passes them on as they are)
    u32 done = 0;
    if (f && n) done = (u32)gfx_pb_run(program, target, tex, f, nfloats, (const int32_t*)n, nints, (int)zero_outside, linear_mask);
    free(f);
    free(n);
    return done;
}

u32 w2c_rw_host_blend_layer(struct w2c_rw* rw, u32 target, u32 layer, u32 mode, u32 mask_state, u32 mask_depth) {
    (void)rw;
    return (u32)gfx_blend_layer(target, layer, mode, mask_state, mask_depth);
}

u32 w2c_rw_host_filter(struct w2c_rw* rw, u32 kind, u32 src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dst, u32 dx, u32 dy,
                       u32 params, u32 nparams) {
    (void)rw;
    float p[24];
    if (nparams > 24 || !in_bounds(params, nparams * 4)) return 0;
    for (u32 i = 0; i < nparams; i++) {
        uint32_t bits = rd32(params + i * 4);
        memcpy(&p[i], &bits, 4);
    }
    return (u32)gfx_filter(kind, src, sx, sy, sw, sh, dst, (int32_t)dx, (int32_t)dy, p, nparams);
}

void w2c_rw_host_tex_read(struct w2c_rw* rw, u32 id, u32 x, u32 y, u32 w, u32 h, u32 out) {
    (void)rw;
    if (!in_bounds(out, w * h * 4)) return;
    uint8_t* p = (uint8_t*)malloc((size_t)w * h * 4 + 1);
    if (!p) return; // out of memory: the caller's buffer stays as it is
    gfx_tex_read(id, x, y, w, h, p);
    copy_in(out, p, w * h * 4);
    free(p);
}

void w2c_rw_host_frame(struct w2c_rw* rw, u32 target, u32 cmds, u32 nwords) {
    (void)rw;
    if (!in_bounds(cmds, nwords * 4)) return;
    uint32_t* c = (uint32_t*)malloc(4 * (nwords ? nwords : 1));
    if (!c) return;
    for (u32 i = 0; i < nwords; i++) c[i] = rd32(cmds + i * 4);
    gfx_frame(target, c, nwords);
    free(c);
}

// ---------------------------------------------------------------- Stage3D imports

u32 w2c_rw_host_c3d_flags(struct w2c_rw* rw) { (void)rw; return gfx_c3d_flags(); }

u32 w2c_rw_host_c3d_buf_create(struct w2c_rw* rw, u32 size, u32 vertex) {
    (void)rw;
    return gfx_c3d_buf_create(size, vertex != 0);
}

void w2c_rw_host_c3d_buf_upload(struct w2c_rw* rw, u32 id, u32 offset, u32 data, u32 len) {
    (void)rw;
    if (!in_bounds(data, len)) return;
    uint8_t* tmp = (uint8_t*)malloc(len + 1);
    if (!tmp) return;
    copy_out(tmp, data, len); // byte order as Flash wrote it (little-endian values)
    gfx_c3d_buf_upload(id, offset, tmp, len);
    free(tmp);
}

void w2c_rw_host_c3d_buf_free(struct w2c_rw* rw, u32 id) { (void)rw; gfx_c3d_buf_free(id); }

static char* string_out(u32 addr, u32 len) {
    if (!in_bounds(addr, len)) return NULL;
    char* s = (char*)malloc(len + 1);
    if (!s) return NULL;
    copy_out(s, addr, len);
    s[len] = 0;
    return s;
}

u32 w2c_rw_host_c3d_program(struct w2c_rw* rw, u32 vs, u32 vs_len, u32 fs, u32 fs_len) {
    (void)rw;
    char* v = string_out(vs, vs_len);
    char* f = string_out(fs, fs_len);
    u32 id = (v && f) ? gfx_c3d_program(v, f) : 0;
    free(v);
    free(f);
    return id;
}

void w2c_rw_host_c3d_program_free(struct w2c_rw* rw, u32 id) { (void)rw; gfx_c3d_program_free(id); }

static float bits_to_float(u32 b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

void w2c_rw_host_c3d_clear(struct w2c_rw* rw, u32 target, u32 r, u32 g, u32 b, u32 a, u32 depth, u32 stencil, u32 mask) {
    (void)rw;
    float rgba[4] = { bits_to_float(r), bits_to_float(g), bits_to_float(b), bits_to_float(a) };
    gfx_c3d_clear(target, rgba, bits_to_float(depth), stencil, mask);
}

void w2c_rw_host_c3d_draw(struct w2c_rw* rw, u32 words, u32 n) {
    (void)rw;
    if (!in_bounds(words, n * 4)) return;
    uint32_t* w = (uint32_t*)malloc(4 * (n ? n : 1));
    if (!w) return;
    for (u32 i = 0; i < n; i++) w[i] = rd32(words + i * 4);
    gfx_c3d_draw(w, n);
    free(w);
}

// ---------------------------------------------------------------- network

u32 w2c_rw_host_sock_open(struct w2c_rw* rw, u32 host, u32 host_len, u32 port, u32 tls) {
    (void)rw;
    if (!host_len || host_len > 255 || !in_bounds(host, host_len)) return 0;
    char h[256];
    copy_out(h, host, host_len);
    h[host_len] = 0;
    return plat_sock_open(h, port, (int)tls);
}

u32 w2c_rw_host_sock_state(struct w2c_rw* rw, u32 id) {
    (void)rw;
    return (u32)plat_sock_state(id);
}

u32 w2c_rw_host_sock_send(struct w2c_rw* rw, u32 id, u32 data, u32 len) {
    (void)rw;
    if (!len || !in_bounds(data, len)) return 0;
    uint8_t* p = (uint8_t*)malloc(len);
    if (!p) return 0;
    copy_out(p, data, len);
    u32 n = plat_sock_send(id, p, len);
    free(p);
    return n;
}

u32 w2c_rw_host_sock_recv(struct w2c_rw* rw, u32 id, u32 buf, u32 cap) {
    (void)rw;
    if (!cap || !in_bounds(buf, cap)) return 0;
    uint8_t* p = (uint8_t*)malloc(cap);
    if (!p) return 0;
    u32 n = plat_sock_recv(id, p, cap);
    if (n) copy_in(buf, p, n);
    free(p);
    return n;
}

void w2c_rw_host_sock_close(struct w2c_rw* rw, u32 id) {
    (void)rw;
    plat_sock_close(id);
}

void w2c_rw_host_log(struct w2c_rw* rw, u32 level, u32 msg, u32 len) {
    (void)rw;
    if (!in_bounds(msg, len)) return;
    char* s = (char*)malloc(len + 1);
    if (!s) return;
    copy_out(s, msg, len);
    s[len] = 0;
    plat_log((int)level, s);
    free(s);
}

// ---------------------------------------------------------------- WASI

enum {
    WASI_ESUCCESS = 0, WASI_EBADF = 8, WASI_EEXIST = 20, WASI_EINVAL = 28, WASI_EIO = 29,
    WASI_EISDIR = 31, WASI_ENOENT = 44, WASI_ENOSYS = 52, WASI_ENOTDIR = 54
};
enum { FT_DIRECTORY = 3, FT_REGULAR = 4 };
enum { O_CREAT_ = 1, O_DIRECTORY_ = 2, O_EXCL_ = 4, O_TRUNC_ = 8 };
#define RIGHT_FD_WRITE (1ull << 6)
#define PREOPEN_FD 3
#define MAX_FDS 64

typedef struct {
    int used;
    int is_dir;
    FILE* f;
    char path[512];
} Fd;

static Fd g_fds[MAX_FDS];

static Fd* get_fd(u32 fd) {
    if (fd >= MAX_FDS || !g_fds[fd].used) return NULL;
    return &g_fds[fd];
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_get(struct w2c_wasi__snapshot__preview1* w, u32 fd, u32 buf) {
    (void)w;
    if (fd != PREOPEN_FD) return WASI_EBADF;
    wr8(buf, 0);       // preopen type: directory
    wr32(buf + 4, 1);  // name length of "/"
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_dir_name(struct w2c_wasi__snapshot__preview1* w, u32 fd, u32 path, u32 len) {
    (void)w;
    if (fd != PREOPEN_FD) return WASI_EBADF;
    if (len >= 1) wr8(path, '/');
    return WASI_ESUCCESS;
}

// A ".." path part (going up a folder), which isn't allowed. Dots inside a name are fine: a game named
// "Impossible_Quiz_Deluxe_NG..swf" couldn't be opened while any ".." was refused.
static int has_parent_part(const char* p) {
    for (const char* s = p; (s = strstr(s, "..")) != NULL; s += 2)
        if ((s == p || s[-1] == '/') && (s[2] == 0 || s[2] == '/')) return 1;
    return 0;
}

// Games made on Windows or a Mac often ask for "games/triviadeath2/..." when the folder is "TriviaDeath2":
// their disks ignore letter case, the console's SD card here doesn't. When a path isn't there as written,
// each part after `base` is looked up ignoring case, and the corrected path replaces it (in place).
#define CASE_CACHE 64
static char g_case_from[CASE_CACHE][512], g_case_to[CASE_CACHE][512];
static int g_case_next, g_case_logged;

static void fix_case(char* path, size_t n, const char* base) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    for (int i = 0; i < CASE_CACHE; i++)
        if (g_case_from[i][0] && !strcmp(g_case_from[i], path)) {
            snprintf(path, n, "%s", g_case_to[i]);
            return;
        }
    size_t blen = strlen(base);
    if (strncmp(path, base, blen) != 0 || path[blen] != '/') return;
    char fixed[512];
    snprintf(fixed, sizeof(fixed), "%s", base);
    const char* rest = path + blen + 1;
    int changed = 0;
    while (*rest) {
        const char* slash = strchr(rest, '/');
        size_t plen = slash ? (size_t)(slash - rest) : strlen(rest);
        char part[256];
        if (plen == 0 || plen >= sizeof(part)) return;
        memcpy(part, rest, plen);
        part[plen] = 0;
        size_t flen = strlen(fixed);
        if (flen + 1 + plen + 1 > sizeof(fixed)) return;
        char tryp[512];
        snprintf(tryp, sizeof(tryp), "%s/%s", fixed, part);
        if (stat(tryp, &st) != 0) {
            DIR* d = opendir(fixed);
            if (!d) break;
            struct dirent* e;
            int found = 0;
            while ((e = readdir(d)) != NULL)
                if (!strcasecmp(e->d_name, part)) {
                    snprintf(part, sizeof(part), "%s", e->d_name);
                    found = changed = 1;
                    break;
                }
            closedir(d);
            if (!found) break; // (a new file being created: the folders before it are still fixed)
        }
        snprintf(fixed + flen, sizeof(fixed) - flen, "/%s", part);
        rest += plen;
        if (*rest == '/') rest++;
    }
    if (!changed) return;
    if (*rest) { // the parts that weren't found stay as they were
        size_t flen = strlen(fixed);
        snprintf(fixed + flen, sizeof(fixed) - flen, "/%s", rest);
    }
    if (g_case_logged < 20) {
        g_case_logged++;
        char m[1100];
        snprintf(m, sizeof(m), "file name case fixed: %s -> %s", path, fixed);
        plat_log(2, m);
    }
    snprintf(g_case_from[g_case_next], sizeof(g_case_from[0]), "%s", path);
    snprintf(g_case_to[g_case_next], sizeof(g_case_to[0]), "%s", fixed);
    g_case_next = (g_case_next + 1) % CASE_CACHE;
    snprintf(path, n, "%s", fixed);
}

u32 w2c_wasi__snapshot__preview1_path_open(struct w2c_wasi__snapshot__preview1* w, u32 dirfd, u32 dirflags,
                                           u32 path, u32 path_len, u32 oflags, u64 rights, u64 rights_inh,
                                           u32 fdflags, u32 fd_out) {
    (void)w; (void)dirflags; (void)rights_inh; (void)fdflags;
    Fd* dir = get_fd(dirfd);
    if (!dir || !dir->is_dir) return WASI_EBADF;
    if (path_len > 400 || !in_bounds(path, path_len)) return WASI_EINVAL;
    char rel[512];
    copy_out(rel, path, path_len);
    rel[path_len] = 0;
    if (has_parent_part(rel)) return WASI_ENOENT;

    u32 slot = 0;
    for (u32 i = PREOPEN_FD + 1; i < MAX_FDS; i++) {
        if (!g_fds[i].used) { slot = i; break; }
    }
    if (!slot) return WASI_EIO;
    Fd* fd = &g_fds[slot];
    memset(fd, 0, sizeof(*fd));
    // Files bundled with the app (the fonts), which aren't under the SD card root.
    if (g_content[0] && !strncmp(rel, ".content/", 9)) snprintf(fd->path, sizeof(fd->path), "%s/%s", g_content, rel + 9);
    else snprintf(fd->path, sizeof(fd->path), "%s/%s", dir->path, rel);
    size_t plen = strlen(fd->path);
    while (plen > 1 && (fd->path[plen - 1] == '/' || fd->path[plen - 1] == '.')) fd->path[--plen] = 0;
    fix_case(fd->path, sizeof(fd->path), dir->path);

    struct stat st;
    int exists = stat(fd->path, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        fd->is_dir = 1;
    } else if (oflags & O_DIRECTORY_) {
        return exists ? WASI_ENOTDIR : WASI_ENOENT;
    } else {
        if (exists && (oflags & O_EXCL_)) return WASI_EEXIST;
        int write = (rights & RIGHT_FD_WRITE) != 0;
        const char* mode = "rb";
        if (write) mode = (!exists || (oflags & O_TRUNC_)) ? ((oflags & O_CREAT_) || exists ? "wb" : NULL) : "r+b";
        if (!mode) return WASI_ENOENT;
        fd->f = fopen(fd->path, mode);
        if (!fd->f) return WASI_ENOENT;
    }
    fd->used = 1;
    wr32(fd_out, slot);
    return WASI_ESUCCESS;
}

// Host path for (directory fd, wasm path); 0 if invalid.
static int host_path(u32 dirfd, u32 path, u32 path_len, char* out, size_t n) {
    Fd* dir = get_fd(dirfd);
    if (!dir || !dir->is_dir || path_len > 400 || !in_bounds(path, path_len)) return 0;
    char rel[512];
    copy_out(rel, path, path_len);
    rel[path_len] = 0;
    if (has_parent_part(rel)) return 0;
    snprintf(out, n, "%s/%s", dir->path, rel);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
    fix_case(out, n, dir->path);
    return 1;
}

u32 w2c_wasi__snapshot__preview1_path_create_directory(struct w2c_wasi__snapshot__preview1* w, u32 dirfd, u32 path, u32 path_len) {
    (void)w;
    char p[600];
    if (!host_path(dirfd, path, path_len, p, sizeof(p))) return WASI_EINVAL;
    struct stat st;
    if (stat(p, &st) == 0) return WASI_EEXIST;
#ifdef _WIN32
    int r = mkdir(p);
#else
    int r = mkdir(p, 0777);
#endif
    return r == 0 ? WASI_ESUCCESS : WASI_ENOENT;
}

u32 w2c_wasi__snapshot__preview1_path_unlink_file(struct w2c_wasi__snapshot__preview1* w, u32 dirfd, u32 path, u32 path_len) {
    (void)w;
    char p[600];
    if (!host_path(dirfd, path, path_len, p, sizeof(p))) return WASI_EINVAL;
    return remove(p) == 0 ? WASI_ESUCCESS : WASI_ENOENT;
}

u32 w2c_wasi__snapshot__preview1_path_remove_directory(struct w2c_wasi__snapshot__preview1* w, u32 dirfd, u32 path, u32 path_len) {
    (void)w;
    char p[600];
    if (!host_path(dirfd, path, path_len, p, sizeof(p))) return WASI_EINVAL;
    return rmdir(p) == 0 ? WASI_ESUCCESS : WASI_ENOENT;
}

// (Imported since jpegxr's C code links WASI libc's stdio; Ruffle itself doesn't seek.)
u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1* w, u32 fd, u64 offset, u32 whence, u32 newoffset) {
    (void)w;
    Fd* f = get_fd(fd);
    if (!f || !f->f) return WASI_EBADF;
    int how = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
    if (fseek(f->f, (long)(int64_t)offset, how) != 0) return WASI_EINVAL;
    long pos = ftell(f->f);
    wr32(newoffset, (u32)pos);
    wr32(newoffset + 4, 0);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(struct w2c_wasi__snapshot__preview1* w, u32 fd, u32 flags) {
    (void)w;
    (void)flags;
    return get_fd(fd) ? WASI_ESUCCESS : WASI_EBADF;
}

u32 w2c_wasi__snapshot__preview1_path_filestat_get(struct w2c_wasi__snapshot__preview1* w, u32 dirfd, u32 flags,
                                                   u32 path, u32 path_len, u32 buf) {
    (void)w; (void)flags;
    char p[600];
    if (!host_path(dirfd, path, path_len, p, sizeof(p))) return WASI_EINVAL;
    struct stat st;
    if (stat(p, &st) != 0) return WASI_ENOENT;
    for (u32 i = 0; i < 64; i++) wr8(buf + i, 0);
    wr8(buf + 16, S_ISDIR(st.st_mode) ? FT_DIRECTORY : FT_REGULAR);
    wr64(buf + 24, 1);
    wr64(buf + 32, (u64)st.st_size);
    u64 t = (u64)st.st_mtime * 1000000000ull;
    wr64(buf + 40, t);
    wr64(buf + 48, t);
    wr64(buf + 56, t);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1* w, u32 fd) {
    (void)w;
    Fd* f = get_fd(fd);
    if (!f || fd == PREOPEN_FD) return WASI_EBADF;
    if (f->f) fclose(f->f);
    memset(f, 0, sizeof(*f));
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1* w, u32 fd, u32 iovs, u32 iovs_len, u32 nread) {
    (void)w;
    Fd* f = get_fd(fd);
    if (!f) return WASI_EBADF;
    if (f->is_dir) return WASI_EISDIR;
    if (!f->f) return WASI_EBADF;
    u32 total = 0;
    static uint8_t chunk[64 * 1024];
    for (u32 i = 0; i < iovs_len; i++) {
        u32 buf = rd32(iovs + i * 8), len = rd32(iovs + i * 8 + 4);
        if (!in_bounds(buf, len)) return WASI_EINVAL;
        u32 done = 0;
        while (done < len) {
            u32 want = len - done < sizeof(chunk) ? len - done : (u32)sizeof(chunk);
            size_t got = fread(chunk, 1, want, f->f);
            copy_in(buf + done, chunk, (u32)got);
            done += (u32)got;
            if (got < want) break;
        }
        total += done;
        if (done < len) break;
    }
    wr32(nread, total);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1* w, u32 fd, u32 iovs, u32 iovs_len, u32 nwritten) {
    (void)w;
    u32 total = 0;
    for (u32 i = 0; i < iovs_len; i++) {
        u32 buf = rd32(iovs + i * 8), len = rd32(iovs + i * 8 + 4);
        if (!in_bounds(buf, len)) return WASI_EINVAL;
        char* tmp = (char*)malloc(len + 1);
        if (!tmp) return WASI_EIO;
        copy_out(tmp, buf, len);
        tmp[len] = 0;
        if (fd == 1 || fd == 2) {
            plat_log(fd == 2 ? 1 : 2, tmp);
        } else {
            Fd* f = get_fd(fd);
            if (!f || !f->f) { free(tmp); return WASI_EBADF; }
            fwrite(tmp, 1, len, f->f);
        }
        free(tmp);
        total += len;
    }
    wr32(nwritten, total);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1* w, u32 fd, u32 buf) {
    (void)w;
    u8 type;
    if (fd <= 2) type = 2; // character device
    else {
        Fd* f = get_fd(fd);
        if (!f) return WASI_EBADF;
        type = f->is_dir ? FT_DIRECTORY : FT_REGULAR;
    }
    for (u32 i = 0; i < 24; i++) wr8(buf + i, 0);
    wr8(buf, type);
    wr64(buf + 8, ~0ull);
    wr64(buf + 16, ~0ull);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_get(struct w2c_wasi__snapshot__preview1* w, u32 fd, u32 buf) {
    (void)w;
    Fd* f = get_fd(fd);
    if (!f) return WASI_EBADF;
    struct stat st;
    if (stat(f->path, &st) != 0) return WASI_EIO;
    for (u32 i = 0; i < 64; i++) wr8(buf + i, 0);
    wr8(buf + 16, f->is_dir ? FT_DIRECTORY : FT_REGULAR);
    wr64(buf + 24, 1);
    wr64(buf + 32, (u64)st.st_size);
    u64 t = (u64)st.st_mtime * 1000000000ull;
    wr64(buf + 40, t);
    wr64(buf + 48, t);
    wr64(buf + 56, t);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1* w, u32 count, u32 size) {
    (void)w;
    wr32(count, 0);
    wr32(size, 0);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1* w, u32 env, u32 buf) {
    (void)w; (void)env; (void)buf;
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1* w, u32 id, u64 precision, u32 out) {
    (void)w; (void)id; (void)precision;
    wr64(out, plat_time_ns());
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1* w, u32 buf, u32 len) {
    (void)w;
    if (!in_bounds(buf, len)) return WASI_EINVAL;
    uint8_t* tmp = (uint8_t*)malloc(len + 1);
    if (!tmp) return WASI_EIO;
    plat_random(tmp, len);
    copy_in(buf, tmp, len);
    free(tmp);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_sched_yield(struct w2c_wasi__snapshot__preview1* w) {
    (void)w;
    return WASI_ESUCCESS;
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1* w, u32 code) {
    (void)w;
    char msg[64];
    snprintf(msg, sizeof(msg), "proc_exit(%u)", code);
    plat_log(0, msg);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
}

// ---------------------------------------------------------------- exports

#if defined(__WIIU__)
extern uint32_t g_wasm_rt_trap_trace[8];
#endif

// What wasm memory growth last did, for the Wii U watchdog's report.
char g_rw_grow_state[96] = "no growth yet";

void rw_grow_log(uint64_t old_size, uint64_t new_size, int ok) {
    if (ok == 2) {
        char m[120];
        snprintf(m, sizeof(m), "wasm memory: %u MB reserved (most it may grow to: %u MB)", (unsigned)(new_size >> 20),
                 (unsigned)(old_size >> 20));
        plat_log(2, m);
        return;
    }
    snprintf(g_rw_grow_state, sizeof(g_rw_grow_state), "%s %u -> %u KB",
             ok < 0 ? "in realloc for wasm memory" : ok ? "wasm memory grew" : "wasm memory could not grow",
             (unsigned)(old_size >> 10), (unsigned)(new_size >> 10));
    if (ok < 0) return;
    // One line per 16 MB crossed.
    if (ok && (old_size >> 24) == (new_size >> 24)) return;
    char m[120];
    snprintf(m, sizeof(m), "wasm memory %s: %u -> %u KB", ok ? "grew" : "COULD NOT GROW",
             (unsigned)(old_size >> 10), (unsigned)(new_size >> 10));
    plat_log(ok ? 2 : 0, m);
}

static void log_trap(wasm_rt_trap_t trap) {
    char m[200];
    snprintf(m, sizeof(m), "wasm trap: %s (memory %u KB)", wasm_rt_strerror(trap), (unsigned)(mem()->size >> 10));
    plat_log(0, m);
#if defined(__WIIU__)
    snprintf(m, sizeof(m), "trap callers: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)g_wasm_rt_trap_trace[0],
             (unsigned)g_wasm_rt_trap_trace[1], (unsigned)g_wasm_rt_trap_trace[2], (unsigned)g_wasm_rt_trap_trace[3],
             (unsigned)g_wasm_rt_trap_trace[4], (unsigned)g_wasm_rt_trap_trace[5], (unsigned)g_wasm_rt_trap_trace[6],
             (unsigned)g_wasm_rt_trap_trace[7]);
    plat_log(0, m);
#endif
}

#define TRY(fail)                                                    \
    if (g_dead) return fail;                                         \
    {                                                                \
        wasm_rt_trap_t trap_ = (wasm_rt_trap_t)wasm_rt_impl_try();   \
        if (trap_) {                                                 \
            log_trap(trap_);                                         \
            g_dead = 1;                                              \
            return fail;                                             \
        }                                                            \
    }

static int g_have_inst;
// Audio buffer inside wasm memory: an address in the current instance only, so a reset forgets it.
static uint32_t g_audio_buf, g_audio_buf_frames;

static void reset_fds(void) {
    for (int i = PREOPEN_FD + 1; i < MAX_FDS; i++)
        if (g_fds[i].used && g_fds[i].f) fclose(g_fds[i].f);
    memset(g_fds, 0, sizeof(g_fds));
    for (int i = 0; i < 3; i++) g_fds[i].used = 1;
    g_fds[PREOPEN_FD].used = 1;
    g_fds[PREOPEN_FD].is_dir = 1;
    snprintf(g_fds[PREOPEN_FD].path, sizeof(g_fds[PREOPEN_FD].path), "%s", g_root);
}

int rw_core_init(const char* root) {
    snprintf(g_root, sizeof(g_root), "%s", root);
    size_t n = strlen(g_root);
    while (n > 0 && g_root[n - 1] == '/') g_root[--n] = 0;
    wasm_rt_init();
    return rw_core_reset();
}

int rw_core_reset(void) {
    if (g_have_inst) {
        wasm2c_flashplayer_free(&g_inst);
        g_have_inst = 0;
        gfx_reset();
        gfx_h264_reset();
    }
    reset_fds();
    g_audio_buf = g_audio_buf_frames = 0;
    g_dead = 0;
    TRY(-1);
    wasm2c_flashplayer_instantiate(&g_inst, &g_rw, &g_wasi);
    g_have_inst = 1;
    return 0;
}

int rw_core_start(const char* url, uint32_t width, uint32_t height) {
    TRY(-1);
    uint32_t len = (uint32_t)strlen(url);
    uint32_t p = w2c_flashplayer_rw_alloc(&g_inst, len + 1);
    copy_in(p, url, len);
    int r = (int)w2c_flashplayer_rw_start(&g_inst, p, len, width, height);
    w2c_flashplayer_rw_free(&g_inst, p, len + 1);
    return r;
}

int rw_core_tick(double dt_ms) {
    TRY(0);
    return (int)w2c_flashplayer_rw_tick(&g_inst, dt_ms);
}

void rw_core_render(void) {
    TRY();
    w2c_flashplayer_rw_render(&g_inst);
}

void rw_core_resize(uint32_t width, uint32_t height) {
    TRY();
    w2c_flashplayer_rw_resize(&g_inst, width, height);
}

void rw_core_mouse(int kind, double x, double y, int button) {
    TRY();
    w2c_flashplayer_rw_mouse(&g_inst, (u32)kind, x, y, (u32)button);
}

void rw_core_key(int down, uint32_t code) {
    TRY();
    w2c_flashplayer_rw_key(&g_inst, (u32)down, code);
}

void rw_core_text(uint32_t codepoint) {
    TRY();
    w2c_flashplayer_rw_text(&g_inst, codepoint);
}

void rw_core_render_stats(uint32_t out[8]) {
    memset(out, 0, 8 * sizeof(uint32_t));
    TRY();
    uint32_t p = w2c_flashplayer_rw_alloc(&g_inst, 32);
    w2c_flashplayer_rw_render_stats(&g_inst, p);
    for (int i = 0; i < 8; i++) out[i] = rd32(p + (uint32_t)i * 4);
    w2c_flashplayer_rw_free(&g_inst, p, 32);
}

uint32_t rw_core_keyboard(void) {
    TRY(0);
    return w2c_flashplayer_rw_keyboard(&g_inst);
}

int rw_core_text_get(char* out, uint32_t cap, uint32_t* info) {
    TRY(-1);
    uint32_t p = w2c_flashplayer_rw_alloc(&g_inst, cap + 4);
    uint32_t n = w2c_flashplayer_rw_text_get(&g_inst, p, cap, p + cap);
    int r = -1;
    if (n != 0xffffffffu) {
        if (n >= cap) n = cap - 1;
        copy_out(out, p, n);
        out[n] = 0;
        *info = rd32(p + cap);
        r = (int)n;
    }
    w2c_flashplayer_rw_free(&g_inst, p, cap + 4);
    return r;
}

void rw_core_text_set(const char* text) {
    TRY();
    uint32_t len = (uint32_t)strlen(text);
    uint32_t p = w2c_flashplayer_rw_alloc(&g_inst, len + 1);
    copy_in(p, text, len);
    w2c_flashplayer_rw_text_set(&g_inst, p, len);
    w2c_flashplayer_rw_free(&g_inst, p, len + 1);
}

void rw_core_audio(int16_t* out, uint32_t frames) {
    uint32_t buf = g_audio_buf;
    uint32_t buf_frames = g_audio_buf_frames;
    memset(out, 0, (size_t)frames * 4);
    TRY();
    if (frames > buf_frames) {
        if (buf) w2c_flashplayer_rw_free(&g_inst, buf, buf_frames * 4);
        buf = g_audio_buf = w2c_flashplayer_rw_alloc(&g_inst, frames * 4);
        buf_frames = g_audio_buf_frames = frames;
    }
    w2c_flashplayer_rw_audio_mix(&g_inst, buf, frames);
    // i16 samples are 2-byte wasm values: read them as halves of native 32-bit words.
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t v = rd32(buf + i * 4);
        out[i * 2] = (int16_t)(v & 0xffff);
        out[i * 2 + 1] = (int16_t)(v >> 16);
    }
}

void rw_core_collect(void) {
    TRY();
    w2c_flashplayer_rw_collect(&g_inst);
}

uint32_t rw_core_audio_rate(void) {
    TRY(48000);
    return w2c_flashplayer_rw_audio_rate(&g_inst);
}

uint32_t rw_core_load_permille(void) {
    TRY(0);
    return w2c_flashplayer_rw_load_permille(&g_inst);
}

void rw_core_stage_size(uint32_t* w, uint32_t* h) {
    *w = *h = 0;
    TRY();
    uint32_t s = w2c_flashplayer_rw_stage_size(&g_inst);
    *w = s >> 16;
    *h = s & 0xffff;
}

int rw_core_dead(void) { return g_dead; }
