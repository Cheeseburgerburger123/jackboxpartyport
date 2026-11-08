// Wii U front end: game picker, GamePad touch as the mouse, buttons as keys, SDL2 audio, and the
// scripted tests used with Cemu (autotest.txt on the SD card, screenshots and log.txt next to it).
//
// SD card layout (sd:/wiiu/apps/flashplayeremu/):
//   swf/*.swf            games (sd:/swf/*.swf is listed too)
//   swf/<game>.swf.url   optional: the web address a sitelocked game expects (one line)
//   log.txt, shots/      written by the player
//   autotest.txt         optional test script, same commands as the PC host plus
//                        "<frame> movie <path>", "<frame> touch x y" and "<frame> quit"
#include "../rw_core.h"
#include "gfx_gx2.h"
#include "kbd.h"
#include "music.h"
#include "ui.h"
#include <SDL.h>
#include <coreinit/alarm.h>
#include <coreinit/context.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/filesystem.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>

void rw_call_on_stack(void (*fn)(void), void* stack_top); // stack_switch.S
#include <coreinit/time.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/systeminfo.h>
#include <sysapp/launch.h>
#include <dirent.h>
#include <unistd.h>
#include "pads.h"
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>

#define SD "fs:/vol/external01"
#define APP_DIR SD "/wiiu/apps/flashplayeremu"
// A build that starts one game straight away instead of the menu (CMakeLists.txt: the jackboxparty app).
// RW_DIRECT_GAME is the game's WASI path without its first slash, RW_DIRECT_TITLE its name on the loading screen.
#ifndef RW_LOG_NAME
#define RW_LOG_NAME "log.txt"
#endif
#ifdef RW_DIRECT_GAME
#define RW_DIRECT_PATH "/" RW_DIRECT_GAME // (the slash is added here: MSYS rewrites "/..." arguments into Windows paths)
static const char* g_direct_title = RW_DIRECT_TITLE;
#else
static const char* g_direct_title = NULL;
#endif
#define SCREEN_W 1280
#define SCREEN_H 720

static int g_log; // log.txt was created
static OSMutex g_log_lock;
static uint64_t g_start_ticks;

// log.txt goes through the system's FS calls directly: through stdio the console left an empty file after a
// freeze (the size on the SD card only updated on close), and stdio/fsync may allocate.
// plat_log only copies the line into a ring buffer; a writer thread on core 0 does the SD card work. Writing
// and flushing on the caller's thread stalled games for about a second per warning (one game logged an error
// every frame), and the audio thread waited on the log too.
static FSClient g_fs_client;
static FSCmdBlock g_fs_block;
static FSFileHandle g_log_fh;

#define LOG_RING (256 * 1024)
static char g_ring[LOG_RING];
static uint32_t g_ring_head, g_ring_tail; // under g_log_lock; bytes in the ring = head - tail
static uint32_t g_ring_dropped;
static volatile int g_log_urgent;   // an error is waiting: flush it soon
static volatile int g_log_stop;     // log_close: write the rest and stop
static volatile int g_log_stopped;
static char g_last_msg[256];        // repeated lines are counted instead of written
static uint32_t g_last_repeats;

static void ring_put(const char* s, uint32_t n) {
    if (n > LOG_RING - (g_ring_head - g_ring_tail)) {
        g_ring_dropped++;
        return;
    }
    for (uint32_t i = 0; i < n; i++) g_ring[(g_ring_head + i) % LOG_RING] = s[i];
    g_ring_head += n;
}

static int log_writer_main(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    static char chunk[16 * 1024] __attribute__((aligned(64))); // FS buffers must be 64-byte aligned
    uint64_t last_flush = OSGetSystemTime();
    int dirty = 0;
    for (;;) {
        OSLockMutex(&g_log_lock);
        uint32_t n = g_ring_head - g_ring_tail;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        for (uint32_t i = 0; i < n; i++) chunk[i] = g_ring[(g_ring_tail + i) % LOG_RING];
        g_ring_tail += n;
        int more = g_ring_head != g_ring_tail;
        OSUnlockMutex(&g_log_lock);
        if (n) {
            FSWriteFile(&g_fs_client, &g_fs_block, (uint8_t*)chunk, 1, n, g_log_fh, 0, FS_ERROR_FLAG_ALL);
            dirty = 1;
        }
        if (more) continue;
        // Committing to the SD card takes about a second on the console: every 2 s at most, or soon after
        // an error (the watchdog's reports are warnings and come within 5 s of a stop).
        uint32_t since = (uint32_t)OSTicksToMilliseconds(OSGetSystemTime() - last_flush);
        if (dirty && (since >= 2000 || (g_log_urgent && since >= 250) || g_log_stop)) {
            FSFlushFile(&g_fs_client, &g_fs_block, g_log_fh, FS_ERROR_FLAG_ALL);
            last_flush = OSGetSystemTime();
            g_log_urgent = 0;
            dirty = 0;
        }
        if (g_log_stop) break;
        OSSleepTicks(OSMillisecondsToTicks(50));
    }
    g_log_stopped = 1;
    return 0;
}

static void log_open(void) {
    OSInitMutex(&g_log_lock);
    FSInit();
    if (FSAddClient(&g_fs_client, FS_ERROR_FLAG_ALL) != FS_STATUS_OK) return;
    FSInitCmdBlock(&g_fs_block);
    if (FSOpenFile(&g_fs_client, &g_fs_block, "/vol/external01/wiiu/apps/flashplayeremu/" RW_LOG_NAME, "w", &g_log_fh,
                   FS_ERROR_FLAG_ALL) != FS_STATUS_OK)
        return;
    static OSThread thread __attribute__((aligned(16)));
    static uint8_t stack[32 * 1024] __attribute__((aligned(16)));
    if (!OSCreateThread(&thread, log_writer_main, 0, NULL, stack + sizeof(stack), sizeof(stack), 20,
                        OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_DETACHED))
        return;
    OSSetThreadName(&thread, "Flashplayer Emu log");
    OSResumeThread(&thread);
    g_log = 1;
}

// Waits (up to ms) until the writer has written and committed everything logged so far.
static void log_drain(uint32_t ms) {
    g_log_urgent = 1;
    for (uint32_t t = 0; t < ms; t += 10) {
        OSLockMutex(&g_log_lock);
        int empty = g_ring_head == g_ring_tail;
        OSUnlockMutex(&g_log_lock);
        if (empty && !g_log_urgent) return;
        OSSleepTicks(OSMillisecondsToTicks(10));
    }
}

static void log_close(void) {
    if (!g_log) return;
    g_log_stop = 1;
    for (int i = 0; i < 300 && !g_log_stopped; i++) OSSleepTicks(OSMillisecondsToTicks(10));
    g_log = 0;
    if (g_log_stopped) FSCloseFile(&g_fs_client, &g_fs_block, g_log_fh, FS_ERROR_FLAG_ALL);
    FSDelClient(&g_fs_client, FS_ERROR_FLAG_ALL);
}

// Lines carry the seconds since start, so a console log shows slow steps as well as stops.
void plat_log(int level, const char* msg) {
    static const char* tags[] = { "ERROR", "WARN", "INFO" };
    if (!g_log) return;
    uint32_t ms = g_start_ticks ? (uint32_t)OSTicksToMilliseconds(OSGetSystemTime() - g_start_ticks) : 0;
    char line[1024];
    OSLockMutex(&g_log_lock);
    if (strncmp(msg, g_last_msg, sizeof(g_last_msg) - 1) == 0) {
        g_last_repeats++;
        OSUnlockMutex(&g_log_lock);
        return;
    }
    if (g_last_repeats) {
        int n = snprintf(line, sizeof(line), "      (the line before repeated %u more times)\n", (unsigned)g_last_repeats);
        ring_put(line, (uint32_t)n);
        g_last_repeats = 0;
    }
    snprintf(g_last_msg, sizeof(g_last_msg), "%s", msg);
    if (g_ring_dropped) {
        int n = snprintf(line, sizeof(line), "      (%u lines dropped: log buffer full)\n", (unsigned)g_ring_dropped);
        g_ring_dropped = 0;
        ring_put(line, (uint32_t)n);
    }
    int n = snprintf(line, sizeof(line), "%s [%5u.%03u] %s\n", tags[level < 0 || level > 2 ? 2 : level],
                     (unsigned)(ms / 1000), (unsigned)(ms % 1000), msg);
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    if (n > 0) ring_put(line, (uint32_t)n);
    OSUnlockMutex(&g_log_lock);
    if (level == 0) g_log_urgent = 1;
}

// ---------------------------------------------------------------- console diagnostics
// Cemu runs things a real Wii U refuses (or does slowly), so a console run reports what it can:
// CPU exceptions with their addresses, and a watchdog that says where the main thread is when the
// app stops making progress. Map the addresses to functions with tools/addr2fn.sh and the same build's .elf.
static volatile uint32_t g_heartbeat; // bumped by every frame the app shows and every tick it runs
static char g_phase[160] = "starting";
static OSThread* g_main_thread;

static void set_phase(const char* fmt, const char* arg) {
    snprintf(g_phase, sizeof(g_phase), fmt, arg);
}

static void present(void) {
    g_heartbeat++;
    gfx_gx2_present();
}

// WHBProcIsRunning, and once it says no (HOME / power), the GPU is off limits (gfx_gx2_gpu_lost).
// Set once ProcUI says the app is closing (HOME > Close Software, power): from then on the system keyboard
// can't be shut down (that hung the app on its way out), so it's left for the system to clear.
static int g_closing;

static int running(void) {
    // Once it has said no, WHB has shut ProcUI down: asking again blocks forever (the console then sat on
    // "Wii U Menu" after HOME > Close Software).
    if (g_closing) return 0;
    if (WHBProcIsRunning()) return 1;
    g_closing = 1;
    gfx_gx2_gpu_lost();
    gfx_h264_abandon(); // (Close Software: the decoder hardware is already gone, calling into it crashed)
    return 0;
}

// "pc ... lr ... calls: a b c" from a context's registers and its stack back chain.
static void describe_context(const OSContext* c, char* out, size_t n) {
    int len = snprintf(out, n, "pc %08X lr %08X sp %08X, calls:", (unsigned)c->srr0, (unsigned)c->lr, (unsigned)c->gpr[1]);
    uint32_t sp = c->gpr[1];
    for (int i = 0; i < 16 && len > 0 && (size_t)len < n - 12; i++) {
        if (sp < 0x10000000 || sp >= 0x50000000 || (sp & 3)) break;
        sp = *(volatile uint32_t*)sp;
        if (sp < 0x10000000 || sp >= 0x50000000 || (sp & 3)) break;
        len += snprintf(out + len, n - (size_t)len, " %08X", (unsigned)*(volatile uint32_t*)(sp + 4));
    }
}

// File writes can't happen inside an exception callback (they froze the console with nothing logged), so
// the callback only saves the registers and sends the thread on to crash_report(), which logs and stops.
static OSContext g_crash_ctx;
static const char* volatile g_crash_kind;

static void crash_report(void) {
    char m[400];
    describe_context(&g_crash_ctx, m, sizeof(m));
    char line[700];
    snprintf(line, sizeof(line), "CRASH (%s) during: %s; %s, dar %08X", g_crash_kind, g_phase, m,
             (unsigned)g_crash_ctx.dar);
    plat_log(0, line);
    log_drain(5000);
    for (;;) OSSleepTicks(OSMillisecondsToTicks(1000));
}

static BOOL redirect_crash(const char* kind, OSContext* c) {
    if (g_crash_kind) return FALSE; // a second one (e.g. in crash_report): let the system have it
    g_crash_kind = kind;
    memcpy(&g_crash_ctx, c, sizeof(g_crash_ctx));
    c->srr0 = (uint32_t)crash_report;
    c->gpr[1] -= 1024; // a little room below the crashed frame
    return TRUE;
}
static BOOL on_dsi(OSContext* c) { return redirect_crash("DSI: bad memory access", c); }
static BOOL on_isi(OSContext* c) { return redirect_crash("ISI: bad code address", c); }
static BOOL on_program(OSContext* c) { return redirect_crash("program: illegal instruction or trap", c); }
static BOOL on_alignment(OSContext* c) { return redirect_crash("alignment: misaligned access", c); }

static int watchdog_main(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    uint32_t last = g_heartbeat;
    int still = 0, reports = 0;
    plat_log(2, "watchdog: running");
    for (;;) {
        OSSleepTicks(OSMillisecondsToTicks(1000));
        uint32_t now = g_heartbeat;
        if (now != last) {
            last = now;
            still = 0;
            continue;
        }
        if (++still % 5 != 0 || reports >= 12) continue;
        reports++;
        {
            extern char g_rw_grow_state[]; // rw_core.c
            char first[300];
            snprintf(first, sizeof(first), "watchdog: no progress for %d s during: %s (last: %s)", still, g_phase,
                     g_rw_grow_state);
            plat_log(1, first); // before touching the main thread, in case that blocks
        }
        // Pause the main thread so its saved registers are current, read them, let it go, then log.
        char m[400];
        OSSuspendThread(g_main_thread);
        for (int i = 0; i < 100 && !OSIsThreadSuspended(g_main_thread); i++) OSSleepTicks(OSMillisecondsToTicks(1));
        describe_context(&g_main_thread->context, m, sizeof(m));
        OSResumeThread(g_main_thread);
        char line[600];
        snprintf(line, sizeof(line), "watchdog: no progress for %d s during: %s; %s", still, g_phase, m);
        plat_log(1, line);
    }
    return 0;
}

// A sampling profiler for test scripts ("profile" in a game's lines): where the main thread is, 200 times a
// second, with its callers, written to profile.txt when the game ends. Addresses map with addr2line on the ELF.
#define PROF_DEPTH 6
#define PROF_MAX 40000
static uint32_t (*g_prof)[PROF_DEPTH];
static volatile int g_prof_on;
static volatile uint32_t g_prof_n;

static int prof_main(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    for (;;) {
        OSSleepTicks(OSMillisecondsToTicks(g_prof_on ? 5 : 200));
        if (!g_prof_on || g_prof_n >= PROF_MAX) continue;
        OSSuspendThread(g_main_thread);
        for (int i = 0; i < 20 && !OSIsThreadSuspended(g_main_thread); i++) OSSleepTicks(OSMicrosecondsToTicks(100));
        const OSContext* c = &g_main_thread->context;
        uint32_t* s = g_prof[g_prof_n];
        s[0] = c->srr0;
        s[1] = c->lr;
        uint32_t sp = c->gpr[1];
        for (int d = 2; d < PROF_DEPTH; d++) {
            if (sp < 0x10000000 || sp >= 0x50000000 || (sp & 3)) { s[d] = 0; continue; }
            sp = *(const uint32_t*)sp;
            s[d] = sp >= 0x10000000 && sp < 0x50000000 ? *(const uint32_t*)(sp + 4) : 0;
        }
        g_prof_n++;
        OSResumeThread(g_main_thread);
    }
    return 0;
}

// While a game's logic is slow, a light sample (100 a second) of where the main thread is goes in the log, as
// "slow spots": on the console that's the only way to see what takes the time (Cemu's saved registers don't say).
#define SPOT_SLOTS 512
typedef struct { uint32_t pc, n; } Spot;
static Spot g_spots[SPOT_SLOTS];   // the game function it was in
static Spot g_callers[SPOT_SLOTS]; // and the one that called that
static volatile int g_spots_on;

static void spot_add(Spot* table, uint32_t pc) {
    pc &= ~(uint32_t)0x3F;
    for (uint32_t i = (pc >> 6) % SPOT_SLOTS, tries = 0; tries < 16; i = (i + 1) % SPOT_SLOTS, tries++) {
        if (table[i].pc == pc || !table[i].pc) {
            table[i].pc = pc;
            table[i].n++;
            return;
        }
    }
}

// App code on the console, and in Cemu.
static int is_game_code(uint32_t a) {
    return (a >= 0x0C000000 && a < 0x0E000000) || (a >= 0x02000000 && a < 0x04000000);
}

// A timer interrupt on the game's core reads where the code really is. (Pausing the thread from another core only
// ever caught it inside system calls: every "slow spot" was an audio lock or the frame wait.)
static OSAlarm g_spot_alarm __attribute__((aligned(64)));

static uint32_t swap_if_needed(uint32_t a) { // (Cemu's saved pc is byte-swapped)
    return is_game_code(a) ? a : __builtin_bswap32(a);
}

static void on_spot_alarm(OSAlarm* alarm, OSContext* ctx) {
    (void)alarm;
    if (!g_spots_on) return;
    uint32_t pc = swap_if_needed(ctx->srr0);
    if (!is_game_code(pc)) return;
    spot_add(g_spots, pc);
    if (is_game_code(ctx->lr)) spot_add(g_callers, ctx->lr);
}
static void spots_log(const char* title, Spot* table) {
    char line[300];
    int len = snprintf(line, sizeof(line), "  %s:", title);
    for (int k = 0; k < 14 && len < (int)sizeof(line) - 20; k++) {
        int best = -1;
        for (int i = 0; i < SPOT_SLOTS; i++)
            if (table[i].n && (best < 0 || table[i].n > table[best].n)) best = i;
        if (best < 0) break;
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %08X*%u", (unsigned)table[best].pc, (unsigned)table[best].n);
        table[best].n = 0;
    }
    plat_log(2, line);
    memset(table, 0, sizeof(Spot) * SPOT_SLOTS);
}

// Called every 5 s: logs what was sampled, and samples the next stretch if this one was slow.
static void spots_window(int slow) {
    static int started;
    if (!started) { // (from the game's thread: an alarm runs on the core that set it)
        OSCreateAlarm(&g_spot_alarm);
        OSSetPeriodicAlarm(&g_spot_alarm, OSGetTime() + OSMillisecondsToTicks(5), OSMillisecondsToTicks(5), on_spot_alarm);
        started = 1;
    }
    if (g_spots_on) {
        g_spots_on = 0;
        OSSleepTicks(OSMillisecondsToTicks(25));
        spots_log("slow spots", g_spots);
        spots_log("called from", g_callers);
    }
    g_spots_on = slow;
}

static void prof_start(void) {
    static OSThread thread __attribute__((aligned(16)));
    static uint8_t stack[16 * 1024] __attribute__((aligned(16)));
    static int started;
    if (!g_prof) g_prof = (uint32_t(*)[PROF_DEPTH])malloc(PROF_MAX * sizeof(*g_prof));
    if (!g_prof) return;
    if (!started && OSCreateThread(&thread, prof_main, 0, NULL, stack + sizeof(stack), sizeof(stack), 5,
                                   OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_DETACHED)) {
        OSResumeThread(&thread);
        started = 1;
    }
    g_prof_n = 0;
    g_prof_on = 1;
}

static void prof_stop(void) {
    if (!g_prof_on) return;
    g_prof_on = 0;
    OSSleepTicks(OSMillisecondsToTicks(10));
    FILE* f = fopen(APP_DIR "/profile.txt", "w");
    if (!f) return;
    for (uint32_t i = 0; i < g_prof_n; i++) {
        for (int d = 0; d < PROF_DEPTH; d++) fprintf(f, "%08X ", (unsigned)g_prof[i][d]);
        fputc('\n', f);
    }
    fclose(f);
}

static void start_diagnostics(void) {
    g_main_thread = OSGetCurrentThread();
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, on_dsi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, on_isi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, on_program);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT, on_alignment);
    static OSThread thread __attribute__((aligned(16)));
    static uint8_t stack[64 * 1024] __attribute__((aligned(16)));
    if (OSCreateThread(&thread, watchdog_main, 0, NULL, stack + sizeof(stack), sizeof(stack), 10,
                       OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_DETACHED)) {
        OSSetThreadName(&thread, "Flashplayer Emu watchdog");
        OSResumeThread(&thread);
    }
}

static void logf_(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    plat_log(2, buf);
}

// Wasm memory is reserved in one block when a game starts: all the console can spare, less room for
// textures, sound and the host. Growing past a block needs the old and a bigger new block at once, which
// failed at 256 MB (Happy Wheels ran out of memory with plenty left in total).
// Big GPU blocks share this block from its bottom (gfx_gx2.c pool_alloc), so what's left here is for
// everything else that mallocs (audio, a big game's file while it's read, small GPU blocks, the shader
// compiler). The system keyboard is loaded before this.
#define MEM_HEADROOM ((uint64_t)256 << 20)
// The block is taken once, when the app starts, and kept: every game's memory reuses it. Taking it again
// for each game failed on the console (the old block freed, a 1 GB try worked, then 832 MB didn't) and
// shrank by 16 MB a game in Cemu.
static void* g_res;
static uint64_t g_res_size;
static int g_res_used;
static uint8_t* g_res_low; // the lowest byte the game's memory uses (it grows down from the end)

static void* take_reserve(uint64_t min_bytes, uint64_t max_bytes, uint64_t* size_out);
uintptr_t gfx_pool_top(void);

void* rw_mem_reserve(uint64_t min_bytes, uint64_t max_bytes, uint64_t* size_out) {
    if (!g_res) g_res = take_reserve(min_bytes, max_bytes, &g_res_size);
    if (!g_res) return NULL;
    if (!g_res_low) g_res_low = (uint8_t*)g_res + g_res_size;
    if (g_res_used || g_res_size < min_bytes) return NULL;
    uint8_t* low = (uint8_t*)g_res + g_res_size - min_bytes;
    if (low < (uint8_t*)gfx_pool_top()) return NULL; // (pictures of the menu are in the way: an ordinary block)
    g_res_low = low;
    g_res_used = 1;
    *size_out = g_res_size;
    return g_res;
}

// 1 if p is the kept block (now free for the next game), 0 if the caller should free it.
int rw_mem_release(void* p) {
    if (!p || p != g_res) return 0;
    g_res_used = 0;
    g_res_low = (uint8_t*)g_res + g_res_size;
    return 1;
}

// May the game's memory grow down to `spare` bytes above the start of its block?
int rw_mem_can_grow(void* block, uint64_t spare) {
    if (block != g_res) return 1;
    uint8_t* low = (uint8_t*)block + spare;
    if (low < (uint8_t*)gfx_pool_top()) return 0;
    g_res_low = low;
    return 1;
}

void rw_res_range(uint8_t** base, uint8_t** end, uint8_t** low) {
    *base = (uint8_t*)g_res;
    *end = (uint8_t*)g_res + g_res_size;
    *low = g_res_low;
}

static void* take_reserve(uint64_t min_bytes, uint64_t max_bytes, uint64_t* size_out) {
    // The biggest block malloc can give, found by trying (asking the system heap said 0: wut's malloc
    // has a heap of its own), less the headroom; then that block is taken, stepping down while it
    // can't be had. Nothing is touched, so a try costs little.
    // Only sizes under 1 GB are tried: malloc said yes to 1 GB and 1.8 GB (in Cemu and on the console)
    // while everything from 992 MB to 1.6 GB failed, so those answers can't be trusted (or freed).
    const uint64_t step = (uint64_t)16 << 20;
    uint64_t avail = 0;
    for (uint64_t size = (uint64_t)1008 << 20; size >= step; size -= step) {
        void* p = malloc((size_t)size);
        if (p) {
            free(p);
            avail = size;
            break;
        }
    }
    uint64_t want = avail > MEM_HEADROOM ? avail - MEM_HEADROOM : 0;
    if (want > max_bytes) want = max_bytes;
    want &= ~(uint64_t)0xFFFF;
    void* block = NULL;
    for (; want > min_bytes && !block; want = want > step ? want - step : 0) {
        block = malloc((size_t)want);
        if (block) break;
    }
    // (Measuring what's left afterwards by trying big mallocs crashed Cemu: such tries can't be trusted.)
    char m[160];
    snprintf(m, sizeof(m), "wasm memory: %u MB free, reserved %u MB (%u MB left for the rest)", (unsigned)(avail >> 20),
             block ? (unsigned)(want >> 20) : 0, block ? (unsigned)((avail - want) >> 20) : 0);
    plat_log(block ? 2 : 1, m);
    *size_out = block ? want : 0;
    return block;
}

uint64_t plat_time_ns(void) {
    return (uint64_t)OSTicksToNanoseconds(OSGetSystemTime());
}

void plat_random(uint8_t* buf, size_t n) {
    static uint32_t s = 0;
    if (!s) s = (uint32_t)OSGetSystemTime() | 1;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (uint8_t)s;
    }
}

// ---------------------------------------------------------------- game list

#define MAX_GAMES 256
static UiGame g_games[MAX_GAMES];
static int g_ngames;

static int has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static int entry_is_dir(const struct dirent* e, const char* full) {
    if (e->d_type != DT_UNKNOWN) return e->d_type == DT_DIR;
    struct stat st;
    return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

static int file_exists(const char* wasi_path) {
    char full[500];
    struct stat st;
    snprintf(full, sizeof(full), SD "%s", wasi_path);
    return stat(full, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_web_page(const char* name) {
    static const char* const exts[] = { ".html", ".htm", ".xhtml", ".jsp", ".php", ".asp", ".aspx" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (has_ext(name, exts[i])) return 1;
    return 0;
}

// The Flash installer that web pages name next to the game.
static int is_flash_installer(const char* name) {
    return !strncasecmp(name, "playerProductInstall", 20) || !strncasecmp(name, "expressInstall", 14);
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// The SWF a saved web page embeds: the first quoted "....swf" on the site that isn't the installer. A page's links
// start at its own folder or, with a leading slash, at the top of the site (`root`). With `want`, only an SWF of that
// name counts. `out` gets the SWF's WASI path.
static int page_names_swf(const char* page, const char* page_dir, const char* root, const char* want, char* out, size_t n) {
    enum { MAX_PAGE = 512 * 1024 };
    char full[500];
    snprintf(full, sizeof(full), SD "%s", page);
    FILE* f = fopen(full, "rb");
    if (!f) return 0;
    char* text = (char*)malloc(MAX_PAGE);
    size_t len = text ? fread(text, 1, MAX_PAGE, f) : 0;
    fclose(f);
    int found = 0;
    for (size_t i = 0; i + 4 <= len && !found; i++) {
        if (strncasecmp(text + i, ".swf", 4)) continue;
        size_t start = i;
        while (start > 0 && text[start - 1] != '"' && text[start - 1] != '\'' && text[start - 1] != '\n' && i - start < 200) start--;
        if (start == 0 || text[start - 1] == '\n' || i - start >= 200) continue;
        char ref[220];
        snprintf(ref, sizeof(ref), "%.*s.swf", (int)(i - start), text + start);
        if (strstr(ref, "://") || is_flash_installer(base_name(ref))) continue;
        if (want && strcasecmp(base_name(ref), want)) continue;
        snprintf(out, n, "%s/%s", ref[0] == '/' ? root : page_dir, ref[0] == '/' ? ref + 1 : ref);
        found = file_exists(out);
        if (!found) {
            snprintf(out, n, "%s/%s", root, ref[0] == '/' ? ref + 1 : ref);
            found = file_exists(out);
        }
    }
    free(text);
    return found;
}

// Looks through a site's pages, its top folder first. `page_out` (optional) gets the page that named the SWF.
static int find_site_page(const char* dir, const char* root, const char* want, int depth, char* swf_out, size_t n, char* page_out, size_t page_n) {
    char full[500];
    snprintf(full, sizeof(full), SD "%s", dir);
    DIR* d = opendir(full);
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_web_page(e->d_name)) continue;
        char page[500];
        snprintf(page, sizeof(page), "%s/%s", dir, e->d_name);
        found = page_names_swf(page, dir, root, want, swf_out, n);
        if (found && page_out) snprintf(page_out, page_n, "%s", page);
    }
    if (!found && depth > 0) {
        rewinddir(d);
        while (!found && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char sub[500];
            snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
            snprintf(full, sizeof(full), SD "%s", sub);
            if (entry_is_dir(e, full)) found = find_site_page(sub, root, want, depth - 1, swf_out, n, page_out, page_n);
        }
    }
    closedir(d);
    return found;
}

// The biggest SWF anywhere below dir, for a site with no page that names one.
static void biggest_swf_below(const char* dir, int depth, char* best, size_t n, long* best_size) {
    char full[500];
    snprintf(full, sizeof(full), SD "%s", dir);
    DIR* d = opendir(full);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[500];
        snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
        snprintf(full, sizeof(full), SD "%s", sub);
        struct stat st;
        if (entry_is_dir(e, full)) {
            if (depth > 0) biggest_swf_below(sub, depth - 1, best, n, best_size);
        } else if (has_ext(e->d_name, ".swf") && !is_flash_installer(e->d_name) && stat(full, &st) == 0 && (long)st.st_size > *best_size) {
            *best_size = (long)st.st_size;
            snprintf(best, n, "%s", sub);
        }
    }
    closedir(d);
}

// A game folder (swf/<folder>/) is one game in the list, whatever else is in it (XML, sounds, extra SWFs
// the game loads): the SWF named like the folder, else its only SWF, else its biggest one. A folder with no SWF
// at its top is a saved website: the SWF its page embeds, else the biggest one below it.
static void scan_game_folder(const char* wasi_dir, const char* folder_name) {
    char dir[300];
    snprintf(dir, sizeof(dir), SD "%s", wasi_dir);
    DIR* d = opendir(dir);
    if (!d) return;
    char best[256] = "";
    long best_size = -1;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !has_ext(e->d_name, ".swf")) continue;
        size_t stem = strlen(e->d_name) - 4;
        if (stem == strlen(folder_name) && !strncasecmp(e->d_name, folder_name, stem)) {
            snprintf(best, sizeof(best), "%s", e->d_name);
            break;
        }
        char full[400];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        long size = stat(full, &st) == 0 ? (long)st.st_size : 0;
        if (size > best_size) {
            best_size = size;
            snprintf(best, sizeof(best), "%s", e->d_name);
        }
    }
    closedir(d);
    if (g_ngames >= MAX_GAMES) return;
    if (!best[0]) {
        char site_swf[256] = "";
        long size = -1;
        if (!find_site_page(wasi_dir, wasi_dir, NULL, 2, site_swf, sizeof(site_swf), NULL, 0))
            biggest_swf_below(wasi_dir, 3, site_swf, sizeof(site_swf), &size);
        if (!site_swf[0]) return;
        UiGame* g = &g_games[g_ngames++];
        snprintf(g->path, sizeof(g->path), "%s", site_swf);
        ui_game_info(g, SD);
        snprintf(g->title, sizeof(g->title), "%s", folder_name);
        return;
    }
    UiGame* g = &g_games[g_ngames++];
    snprintf(g->path, sizeof(g->path), "%s/%s", wasi_dir, best);
    ui_game_info(g, SD);
}

// Lists the .swf files in wasi_dir, plus one game per subfolder (scan_game_folder): a game that loads files
// of its own keeps them in a folder with it, e.g. swf/zuma/zuma2.swf with swf/zuma/levels.xml. Relative URLs
// load from the SWF's folder (navigator.rs).
static void scan(const char* wasi_dir, int depth) {
    char dir[300];
    snprintf(dir, sizeof(dir), SD "%s", wasi_dir);
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && g_ngames < MAX_GAMES) {
        if (e->d_name[0] == '.') continue;
        if (has_ext(e->d_name, ".swf")) {
            UiGame* g = &g_games[g_ngames++];
            snprintf(g->path, sizeof(g->path), "%s/%s", wasi_dir, e->d_name);
            ui_game_info(g, SD);
            continue;
        }
        if (depth <= 0) continue;
        char sub[300];
        snprintf(sub, sizeof(sub), "%s/%s", wasi_dir, e->d_name);
        int is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            char full[400];
            struct stat st;
            snprintf(full, sizeof(full), SD "%s", sub);
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) scan_game_folder(sub, e->d_name);
    }
    closedir(d);
}

static int cmp_games(const void* a, const void* b) {
    return strcasecmp(((const UiGame*)a)->title, ((const UiGame*)b)->title);
}

// ---------------------------------------------------------------- input

typedef struct { uint32_t button; uint32_t key; } KeyMap;
// Flash games mostly use arrows, space, Z/X/C, Enter and Shift.
static const KeyMap g_keymap[] = {
    { VPAD_BUTTON_LEFT, RW_KEY_LEFT }, { VPAD_BUTTON_RIGHT, RW_KEY_RIGHT },
    { VPAD_BUTTON_UP, RW_KEY_UP }, { VPAD_BUTTON_DOWN, RW_KEY_DOWN },
    // The right stick is a second d-pad; the left stick moves the mouse, with L and R as its buttons.
    { VPAD_STICK_R_EMULATION_LEFT, RW_KEY_LEFT }, { VPAD_STICK_R_EMULATION_RIGHT, RW_KEY_RIGHT },
    { VPAD_STICK_R_EMULATION_UP, RW_KEY_UP }, { VPAD_STICK_R_EMULATION_DOWN, RW_KEY_DOWN },
#ifdef RW_DIRECT_GAME
    // Jackbox: A selects (Enter), B goes back (Escape), like its PC keyboard controls.
    { VPAD_BUTTON_A, RW_KEY_ENTER }, { VPAD_BUTTON_B, RW_KEY_ESCAPE }, { VPAD_BUTTON_X, 'x' }, { VPAD_BUTTON_Y, 'c' },
#else
    { VPAD_BUTTON_A, ' ' }, { VPAD_BUTTON_B, 'z' }, { VPAD_BUTTON_X, 'x' }, { VPAD_BUTTON_Y, 'c' },
#endif
    { VPAD_BUTTON_PLUS, RW_KEY_ENTER },
    { VPAD_BUTTON_ZL, RW_KEY_SHIFT }, { VPAD_BUTTON_ZR, RW_KEY_CONTROL },
};

static uint32_t button_by_name(const char* s) {
    static const struct { const char* n; uint32_t b; } names[] = {
        { "a", VPAD_BUTTON_A }, { "b", VPAD_BUTTON_B }, { "x", VPAD_BUTTON_X }, { "y", VPAD_BUTTON_Y },
        { "left", VPAD_BUTTON_LEFT }, { "right", VPAD_BUTTON_RIGHT }, { "up", VPAD_BUTTON_UP },
        { "down", VPAD_BUTTON_DOWN }, { "plus", VPAD_BUTTON_PLUS }, { "minus", VPAD_BUTTON_MINUS },
        { "l", VPAD_BUTTON_L }, { "r", VPAD_BUTTON_R }, { "zl", VPAD_BUTTON_ZL }, { "zr", VPAD_BUTTON_ZR },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(s, names[i].n)) return names[i].b;
    return 0;
}

// ---------------------------------------------------------------- test script

typedef struct { int frame; char cmd[16]; char arg[256]; } ScriptLine;
static ScriptLine* g_script;
static int g_nscript, g_si;

static void load_script(void) {
    FILE* f = fopen(APP_DIR "/autotest.txt", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        ScriptLine l;
        memset(&l, 0, sizeof(l));
        int consumed = 0;
        if (sscanf(line, "%d %15s %n", &l.frame, l.cmd, &consumed) < 2) continue;
        snprintf(l.arg, sizeof(l.arg), "%s", line + consumed);
        l.arg[strcspn(l.arg, "\r\n")] = 0;
        g_script = (ScriptLine*)realloc(g_script, sizeof(ScriptLine) * (g_nscript + 1));
        g_script[g_nscript++] = l;
    }
    fclose(f);
    logf_("autotest: %d lines", g_nscript);
}

static void write_bmp(const char* path, const uint8_t* rgba, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int row = w * 3, pad = (4 - row % 4) % 4;
    uint32_t size = 54 + (uint32_t)(row + pad) * h;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    for (int i = 0; i < 4; i++) {
        hdr[2 + i] = (uint8_t)(size >> (8 * i));
        hdr[18 + i] = (uint8_t)((uint32_t)w >> (8 * i));
        hdr[22 + i] = (uint8_t)((uint32_t)h >> (8 * i));
    }
    hdr[10] = 54; hdr[14] = 40; hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t* line = (uint8_t*)calloc(1, (size_t)row + pad);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            line[x * 3] = src[x * 4 + 2];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4];
        }
        fwrite(line, 1, (size_t)row + pad, f);
    }
    free(line);
    fclose(f);
}

static void screenshot(int frame) {
    static int n;
    static uint8_t* buf;
    if (!buf) buf = (uint8_t*)malloc((size_t)SCREEN_W * SCREEN_H * 4);
    mkdir(APP_DIR "/shots", 0777);
    gfx_gx2_read_screen(buf);
    char path[300];
    snprintf(path, sizeof(path), APP_DIR "/shots/shot_%04d_f%05d.bmp", n++, frame);
    write_bmp(path, buf, SCREEN_W, SCREEN_H);
    logf_("shot %s", path);
}

// ---------------------------------------------------------------- audio

static SDL_AudioDeviceID g_audio;

static void audio_init(void) {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        logf_("SDL audio: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = (int)rw_core_audio_rate();
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else logf_("SDL_OpenAudioDevice: %s", SDL_GetError());
}

// ---------------------------------------------------------------- main

static void spoof_for(const char* game, char* out, size_t n) {
    out[0] = 0;
    char path[320];
    snprintf(path, sizeof(path), SD "%s.url", game);
    FILE* f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

static void start_url(const char* game, uint32_t w, uint32_t h) {
    char spoof[256], url[1000], page[300] = "";
    spoof_for(game, spoof, sizeof(spoof));
    // A game in its own folder: the web page that embeds it holds its settings (FlashVars).
    const char* in_swf = strstr(game, "/swf/");
    const char* folder_end = in_swf ? strchr(in_swf + 5, '/') : NULL;
    if (folder_end) {
        char root[300], swf[300];
        snprintf(root, sizeof(root), "%.*s", (int)(folder_end - game), game);
        if (!find_site_page(root, root, base_name(game), 2, swf, sizeof(swf), page, sizeof(page)))
            find_site_page(root, root, NULL, 2, swf, sizeof(swf), page, sizeof(page));
    }
    snprintf(url, sizeof(url), "file://%s\n%s\n%s", game, spoof, page);
    if (rw_core_start(url, w, h) != 0) logf_("rw_start failed for %s", game);
}

// ---------------------------------------------------------------- game pictures
// Captured from the game itself the first time it is seen, cached in thumbs/<file>.thumb:
// "RWT2", the SWF's size (a changed file gets a new picture), then UI_THUMB_W x UI_THUMB_H RGBA8.
// (RWT1 pictures were taken at a fixed 5 s, often mid-loading; they are retaken.)

#define THUMB_BYTES (UI_THUMB_W * UI_THUMB_H * 4)
#define CAPTURE_W (UI_THUMB_W * 2)
#define CAPTURE_H (UI_THUMB_H * 2)
// The picture is taken AFTER_LOAD_TICKS after the SWF has fully loaded (past most "loading" bars),
// or after MAX_CAPTURE_TICKS for games that never report done.
#define AFTER_LOAD_TICKS 150
#define MAX_CAPTURE_TICKS 1800

static void thumb_path(const UiGame* g, char* out, size_t n) {
    const char* name = strrchr(g->path, '/');
    snprintf(out, n, APP_DIR "/thumbs/%s.thumb", name ? name + 1 : g->path);
}

static int load_thumb(UiGame* g) {
    char path[400];
    thumb_path(g, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[8];
    int ok = fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "RWT2", 4) &&
             (uint32_t)(hdr[4] << 24 | hdr[5] << 16 | hdr[6] << 8 | hdr[7]) == g->size;
    if (ok) {
        g->thumb = (uint8_t*)malloc(THUMB_BYTES);
        if (!g->thumb || fread(g->thumb, 1, THUMB_BYTES, f) != THUMB_BYTES) {
            free(g->thumb);
            g->thumb = NULL;
            ok = 0;
        }
    }
    fclose(f);
    return ok;
}

static void make_thumb(UiGame* g, int index, int total, uint8_t* canvas) {
    set_phase("taking the menu picture of %s", g->path);
    // The progress screen first: starting a game can take a while (and the boot screen stays up until
    // the app shows a frame).
    ui_draw_progress(canvas, index, total, g->title, 0.0f);
    gfx_tex_update_screen(canvas);
    present();
    rw_core_reset();
    start_url(g->path, CAPTURE_W, CAPTURE_H);
    logf_("picture of %s: game started", g->path);
    int left = -1; // ticks still to run once loaded
    uint64_t start = OSGetSystemTime(), last_shown = start;
    // The game only runs its logic here; it's drawn once at the end for the picture. (Drawing every tick
    // made pictures slow: ~22 ms a frame for some games, and nobody sees those frames.)
    for (int i = 0; i < MAX_CAPTURE_TICKS && left != 0 && !rw_core_dead() && running(); i++) {
        g_heartbeat++;
        rw_core_tick(1000.0 / 60.0);
        music_update();
        if (left > 0) left--;
        else if (left < 0 && rw_core_load_permille() >= 1000) left = AFTER_LOAD_TICKS;
        // Give up on games that never start (a file that can't be read) and on very slow ones.
        uint32_t spent = (uint32_t)OSTicksToMilliseconds(OSGetSystemTime() - start);
        uint32_t w = 0, h = 0;
        if (i == 300) rw_core_stage_size(&w, &h);
        if ((i == 300 && !w && !rw_core_load_permille()) || spent > 20000) {
            logf_("picture of %s: %s, giving up", g->path, spent > 20000 ? "too slow" : "the game didn't start");
            break;
        }
        // The progress screen, up to 30 times a second: showing a frame waits for the TV's refresh, so at
        // 60 the game could only run 60 ticks a second and pictures took longer.
        uint64_t now = OSGetSystemTime();
        if (left != 0 && i + 1 < MAX_CAPTURE_TICKS && OSTicksToMilliseconds(now - last_shown) >= 33) {
            last_shown = now;
            ui_draw_progress(canvas, index, total, g->title, (float)OSTicksToMilliseconds(now - start) / 1000.0f);
            gfx_tex_update_screen(canvas);
            present();
        }
    }
    if (!running()) return; // leaving the app: no picture (the GPU is going away)
    if (!rw_core_dead()) rw_core_render(); // the one frame the picture is taken from
    if (left != 0 && !rw_core_dead()) logf_("picture of %s: still loading, taken anyway", g->path);
    if (rw_core_dead()) logf_("picture of %s: the game crashed, keeping its last frame", g->path);
    uint8_t* big = (uint8_t*)malloc(CAPTURE_W * CAPTURE_H * 4);
    g->thumb = (uint8_t*)malloc(THUMB_BYTES);
    if (!big || !g->thumb) {
        free(big);
        free(g->thumb);
        g->thumb = NULL;
        return;
    }
    gfx_gx2_read_screen_rect(0, 0, CAPTURE_W, CAPTURE_H, big);
    // Halve with a 2x2 box filter.
    for (int y = 0; y < UI_THUMB_H; y++)
        for (int x = 0; x < UI_THUMB_W; x++)
            for (int c = 0; c < 4; c++) {
                const uint8_t* p = big + ((size_t)(y * 2) * CAPTURE_W + x * 2) * 4 + c;
                g->thumb[((size_t)y * UI_THUMB_W + x) * 4 + c] =
                    (uint8_t)((p[0] + p[4] + p[CAPTURE_W * 4] + p[CAPTURE_W * 4 + 4] + 2) / 4);
            }
    free(big);
    for (int i = 0; i < THUMB_BYTES; i += 4) g->thumb[i + 3] = 255;

    mkdir(APP_DIR "/thumbs", 0777);
    char path[400];
    thumb_path(g, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint8_t hdr[8] = { 'R', 'W', 'T', '2', (uint8_t)(g->size >> 24), (uint8_t)(g->size >> 16),
                       (uint8_t)(g->size >> 8), (uint8_t)g->size };
    fwrite(hdr, 1, 8, f);
    fwrite(g->thumb, 1, THUMB_BYTES, f);
    fclose(f);
    logf_("picture saved: %s", path);
}

// ---------------------------------------------------------------- menu and game

// The border art currently shown around the game (for screenshots of what the TV shows).
static const uint8_t* g_border_art;
static int g_border_rect[4];
// The FPS counter's picture while it is shown (HUD_W x HUD_H at 8, 8), also for screenshots.
#define HUD_W 112
#define HUD_H 28
static const uint8_t* g_hud_px;
static const uint8_t* g_cursor_px;
static int g_cursor_on, g_cursor_x, g_cursor_y;
static uint8_t* g_osk_px;
static int g_osk_open;

static void shot_full(int frame) {
    static uint8_t* buf;
    if (!g_border_art && !g_hud_px && !g_cursor_on && !g_osk_px) {
        screenshot(frame);
        return;
    }
    if (!buf) buf = (uint8_t*)malloc((size_t)SCREEN_W * SCREEN_H * 4);
    // Border art everywhere, the game's stage rect from the screen target.
    int x = 0, y = 0, w = SCREEN_W, h = SCREEN_H;
    if (g_border_art) {
        memcpy(buf, g_border_art, (size_t)SCREEN_W * SCREEN_H * 4);
        x = g_border_rect[0]; y = g_border_rect[1]; w = g_border_rect[2]; h = g_border_rect[3];
    }
    uint8_t* game = (uint8_t*)malloc((size_t)w * h * 4);
    if (game) {
        gfx_gx2_read_screen_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, game);
        for (int r = 0; r < h; r++)
            memcpy(buf + ((size_t)(y + r) * SCREEN_W + x) * 4, game + (size_t)r * w * 4, (size_t)w * 4);
        free(game);
    }
    if (g_hud_px)
        for (int r = 0; r < HUD_H; r++)
            memcpy(buf + ((size_t)(8 + r) * SCREEN_W + 8) * 4, g_hud_px + (size_t)r * HUD_W * 4, HUD_W * 4);
    if (g_osk_px) {
        for (int r = 0; r < UI_OSK_TAB_H; r++)
            memcpy(buf + ((size_t)(UI_OSK_TAB_Y + r) * SCREEN_W + UI_OSK_TAB_X) * 4,
                   g_osk_px + ((size_t)(UI_OSK_TAB_CANVAS_Y + r) * UI_W) * 4, UI_OSK_TAB_W * 4);
        if (g_osk_open)
            for (int r = 0; r < UI_OSK_H; r++)
                memcpy(buf + ((size_t)(UI_OSK_Y + r) * SCREEN_W + UI_OSK_X) * 4, g_osk_px + (size_t)r * UI_W * 4, UI_OSK_W * 4);
    }
    if (g_cursor_on && g_cursor_px)
        for (int r = 0; r < UI_CURSOR_H && g_cursor_y + r < SCREEN_H; r++)
            for (int c = 0; c < UI_CURSOR_W && g_cursor_x + c < SCREEN_W; c++)
                if (g_cursor_px[(r * UI_CURSOR_W + c) * 4 + 3])
                    memcpy(buf + ((size_t)(g_cursor_y + r) * SCREEN_W + g_cursor_x + c) * 4,
                           g_cursor_px + (r * UI_CURSOR_W + c) * 4, 4);
    static int n;
    mkdir(APP_DIR "/shots", 0777);
    char path[300];
    snprintf(path, sizeof(path), APP_DIR "/shots/game_%04d_f%05d.bmp", n++, frame);
    write_bmp(path, buf, SCREEN_W, SCREEN_H);
    logf_("shot %s", path);
}

// ---------------------------------------------------------------- on-screen keyboard in games
static uint8_t* g_osk_px; // its pictures (see ui.h)
static int g_osk_open;
static uint8_t g_osk_held[UI_OSK_KEYS];
static int g_osk_down = -1; // the key being pressed (shift and ctrl stay held until pressed again)

static void osk_redraw(void) {
    if (!g_osk_px) return;
    ui_draw_osk(g_osk_px, g_osk_held);
    gfx_gx2_set_panel(0, g_osk_px, UI_OSK_W, UI_OSK_H, UI_W * 4, UI_OSK_X, UI_OSK_Y);
}

static void osk_tab_redraw(void) {
    if (!g_osk_px) return;
#ifdef RW_DIRECT_GAME
    return;
#endif
    ui_draw_osk_tab(g_osk_px, g_osk_open);
    gfx_gx2_set_panel(1, g_osk_px + (size_t)UI_OSK_TAB_CANVAS_Y * UI_W * 4, UI_OSK_TAB_W, UI_OSK_TAB_H, UI_W * 4, UI_OSK_TAB_X,
                      UI_OSK_TAB_Y);
}

static void osk_set_open(int open) {
    g_osk_open = open;
    if (!open) // nothing stays held down in the game
        for (int i = 0; i < UI_OSK_KEYS; i++)
            if (g_osk_held[i]) {
                rw_core_key(0, ui_osk_code(i));
                g_osk_held[i] = 0;
            }
    if (open) osk_redraw();
    gfx_gx2_panel_on(0, open);
    osk_tab_redraw();
}

// A press at a screen position: 1 if it was on the tab or the keyboard, so it isn't the game's.
static int osk_press(int x, int y) {
#ifdef RW_DIRECT_GAME
    (void)x;
    (void)y;
    return 0;
#endif
    if (x >= UI_OSK_TAB_X && x < UI_OSK_TAB_X + UI_OSK_TAB_W && y >= UI_OSK_TAB_Y && y < UI_OSK_TAB_Y + UI_OSK_TAB_H) {
        osk_set_open(!g_osk_open);
        return 1;
    }
    if (!g_osk_open || x < UI_OSK_X || x >= UI_OSK_X + UI_OSK_W || y < UI_OSK_Y || y >= UI_OSK_Y + UI_OSK_H) return 0;
    int k = ui_osk_key_at(x, y);
    uint32_t code = ui_osk_code(k);
    if (k < 0 || code == 0xffffffffu) return 1;
    if (!code) {
        osk_set_open(0);
        return 1;
    }
    if (code == RW_KEY_SHIFT || code == RW_KEY_CONTROL) {
        g_osk_held[k] = !g_osk_held[k];
        rw_core_key(g_osk_held[k], code);
    } else {
        g_osk_held[k] = 1;
        g_osk_down = k;
        rw_core_key(1, code);
    }
    osk_redraw();
    return 1;
}

static void osk_release(void) {
    if (g_osk_down < 0) return;
    rw_core_key(0, ui_osk_code(g_osk_down));
    g_osk_held[g_osk_down] = 0;
    g_osk_down = -1;
    osk_redraw();
}

// ---------------------------------------------------------------- settings (settings.txt on the SD card)

typedef struct {
    int music;   // 0 off .. 4 max
    int borders; // side borders around 4:3 games
    int fps;     // frames-per-second counter in games (off unless turned on)
    int theme;   // how the menus look: UI_THEME_*
} Settings;

static Settings g_set = { 4, 1, 0, 0 };
static const char* const MUSIC_NAMES[] = { "Off", "Quiet", "Medium", "Loud", "Max" };
static const char* const THEME_NAMES[UI_THEME_COUNT] = { "Classic", "Adobe Flash", "Halloween" };
static const char* const THEME_MUSIC[UI_THEME_COUNT] = {
    "fs:/vol/content/music/cipher.mp3",
    "fs:/vol/content/music/sneaky_snitch.mp3",
    "fs:/vol/content/music/not_as_it_seems.mp3",
};

static void apply_settings(void) {
    static const float vol[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    music_set_volume(vol[g_set.music]);
    ui_set_theme(g_set.theme);
}

static void load_settings(void) {
    FILE* f = fopen(APP_DIR "/settings.txt", "r");
    if (f) {
        char line[64];
        int v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "music=%d", &v) == 1 && v >= 0 && v <= 4) g_set.music = v;
            if (sscanf(line, "borders=%d", &v) == 1) g_set.borders = v != 0;
            if (sscanf(line, "fps=%d", &v) == 1) g_set.fps = v != 0;
            if (sscanf(line, "theme=%d", &v) == 1 && v >= 0 && v < UI_THEME_COUNT) g_set.theme = v;
        }
        fclose(f);
    }
    apply_settings();
}

static void save_settings(void) {
    FILE* f = fopen(APP_DIR "/settings.txt", "w");
    if (!f) return;
    fprintf(f, "music=%d\nborders=%d\nfps=%d\ntheme=%d\n", g_set.music, g_set.borders, g_set.fps, g_set.theme);
    fclose(f);
}

// ---------------------------------------------------------------- menu layout (menu.txt on the SD card)
// The player's order of the main list, and folders made by dropping a game on another. menu.txt:
//   g <path>     a game in the main list
//   f <name>     a folder, followed by its games as "  <path>"
// Games not in it (new ones) go at the end; games no longer on the card are left out.
#define MAX_FOLDERS 64
typedef struct {
    char name[48];
    int* games; // indexes into g_games
    int n, cap;
} Folder;
static Folder g_folders[MAX_FOLDERS];
static int* g_top; // the main list: a game index, or -(folder + 1)
static int g_ntop, g_top_cap;
static int g_open = -1; // the folder shown, or -1 for the main list
static UiGame* g_view;  // the tiles of the list shown
static int g_nview;

static void list_insert(int** a, int* n, int* cap, int at, int v) {
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 32;
        int* grown = (int*)realloc(*a, (size_t)c * sizeof(int));
        if (!grown) return;
        *a = grown;
        *cap = c;
    }
    if (at < 0 || at > *n) at = *n;
    memmove(*a + at + 1, *a + at, (size_t)(*n - at) * sizeof(int));
    (*a)[at] = v;
    (*n)++;
}

static void list_remove(int* a, int* n, int at) {
    if (at < 0 || at >= *n) return;
    memmove(a + at, a + at + 1, (size_t)(*n - at - 1) * sizeof(int));
    (*n)--;
}

static int game_by_path(const char* path) {
    for (int i = 0; i < g_ngames; i++)
        if (!strcmp(g_games[i].path, path)) return i;
    return -1;
}

static void save_menu(void) {
    FILE* f = fopen(APP_DIR "/menu.txt", "w");
    if (!f) return;
    for (int i = 0; i < g_ntop; i++) {
        int v = g_top[i];
        if (v >= 0) {
            fprintf(f, "g %s\n", g_games[v].path);
        } else {
            Folder* fo = &g_folders[-v - 1];
            fprintf(f, "f %s\n", fo->name);
            for (int k = 0; k < fo->n; k++) fprintf(f, "  %s\n", g_games[fo->games[k]].path);
        }
    }
    fclose(f);
}

static void load_menu(void) {
    char* used = (char*)calloc((size_t)g_ngames + 1, 1);
    FILE* f = fopen(APP_DIR "/menu.txt", "r");
    if (f && used) {
        char line[400];
        int cur = -1; // the folder being read
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == 'f' && line[1] == ' ') {
                cur = -1;
                for (int k = 0; k < MAX_FOLDERS; k++)
                    if (!g_folders[k].n && !g_folders[k].name[0]) { cur = k; break; }
                if (cur < 0) continue;
                snprintf(g_folders[cur].name, sizeof(g_folders[cur].name), "%s", line + 2);
                list_insert(&g_top, &g_ntop, &g_top_cap, g_ntop, -cur - 1);
            } else if (line[0] == 'g' && line[1] == ' ') {
                cur = -1;
                int gi = game_by_path(line + 2);
                if (gi >= 0 && !used[gi]) {
                    used[gi] = 1;
                    list_insert(&g_top, &g_ntop, &g_top_cap, g_ntop, gi);
                }
            } else if (line[0] == ' ' && cur >= 0) {
                int gi = game_by_path(line + 2);
                if (gi >= 0 && !used[gi]) {
                    used[gi] = 1;
                    Folder* fo = &g_folders[cur];
                    list_insert(&fo->games, &fo->n, &fo->cap, fo->n, gi);
                }
            }
        }
    }
    if (f) fclose(f);
    // Folders whose games are all gone are dropped; new games go at the end.
    for (int i = g_ntop - 1; i >= 0; i--)
        if (g_top[i] < 0 && !g_folders[-g_top[i] - 1].n) {
            g_folders[-g_top[i] - 1].name[0] = 0;
            list_remove(g_top, &g_ntop, i);
        }
    for (int i = 0; i < g_ngames; i++)
        if (!used || !used[i]) list_insert(&g_top, &g_ntop, &g_top_cap, g_ntop, i);
    free(used);
}

// The tiles of the list shown (the main list or the open folder) into g_view.
static void build_view(void) {
    int n = g_open >= 0 ? g_folders[g_open].n : g_ntop;
    UiGame* v = (UiGame*)realloc(g_view, (size_t)(n ? n : 1) * sizeof(UiGame));
    if (!v) return;
    g_view = v;
    g_nview = n;
    for (int i = 0; i < n; i++) {
        int item = g_open >= 0 ? g_folders[g_open].games[i] : g_top[i];
        if (item >= 0) {
            g_view[i] = g_games[item];
            continue;
        }
        Folder* fo = &g_folders[-item - 1];
        UiGame* t = &g_view[i];
        memset(t, 0, sizeof(*t));
        t->folder = 1;
        t->count = fo->n;
        snprintf(t->title, sizeof(t->title), "%s", fo->name);
        snprintf(t->path, sizeof(t->path), "folder:%d", -item - 1);
        for (int k = 0; k < 4 && k < fo->n; k++) t->sub[k] = g_games[fo->games[k]].thumb;
    }
}

// The list shown, for moving things in it.
static int* view_list(int** cap_out, int** n_out) {
    static int dummy;
    if (g_open >= 0) {
        *n_out = &g_folders[g_open].n;
        *cap_out = &g_folders[g_open].cap;
        return g_folders[g_open].games;
    }
    *n_out = &g_ntop;
    *cap_out = &dummy;
    return g_top;
}

// Moves tile `from` of the list shown to position `to` (0..n, as before the move).
static void menu_move(int from, int to) {
    int *cap, *n;
    int* a = view_list(&cap, &n);
    if (from < 0 || from >= *n || to < 0 || to > *n || to == from || to == from + 1) return;
    int v = a[from];
    list_remove(a, n, from);
    if (to > from) to--;
    memmove(a + to + 1, a + to, (size_t)(*n - to) * sizeof(int));
    a[to] = v;
    (*n)++;
    save_menu();
}

// Main list: game tile `from` dropped on tile `onto` (a game: they become a new folder there; a folder:
// the game goes in it). Returns the new position of the folder.
static int menu_make_folder(int from, int onto) {
    if (g_open >= 0 || from == onto || g_top[from] < 0) return onto;
    int game = g_top[from];
    int f;
    if (g_top[onto] < 0) {
        f = -g_top[onto] - 1;
    } else {
        f = -1;
        for (int k = 0; k < MAX_FOLDERS; k++)
            if (!g_folders[k].n && !g_folders[k].name[0]) { f = k; break; }
        if (f < 0) return onto; // (64 folders)
        // "Folder", "Folder 2", ...: the first name not taken.
        for (int num = 1;; num++) {
            char name[48];
            if (num == 1) snprintf(name, sizeof(name), "Folder");
            else snprintf(name, sizeof(name), "Folder %d", num);
            int taken = 0;
            for (int k = 0; k < MAX_FOLDERS; k++) taken |= g_folders[k].n && !strcmp(g_folders[k].name, name);
            if (!taken) {
                snprintf(g_folders[f].name, sizeof(g_folders[f].name), "%s", name);
                break;
            }
        }
        list_insert(&g_folders[f].games, &g_folders[f].n, &g_folders[f].cap, 0, g_top[onto]);
        g_top[onto] = -f - 1;
    }
    list_insert(&g_folders[f].games, &g_folders[f].n, &g_folders[f].cap, g_folders[f].n, game);
    list_remove(g_top, &g_ntop, from);
    save_menu();
    return from < onto ? onto - 1 : onto;
}

// Open folder: game `from` goes back to the main list, just after the folder. An emptied folder goes.
static void menu_take_out(int from) {
    if (g_open < 0) return;
    Folder* fo = &g_folders[g_open];
    if (from < 0 || from >= fo->n) return;
    int game = fo->games[from];
    list_remove(fo->games, &fo->n, from);
    int at = 0;
    while (at < g_ntop && g_top[at] != -g_open - 1) at++;
    list_insert(&g_top, &g_ntop, &g_top_cap, at + 1, game);
    if (!fo->n) {
        list_remove(g_top, &g_ntop, at);
        fo->name[0] = 0;
        g_open = -1;
    }
    save_menu();
}

enum { SET_THEME, SET_MUSIC, SET_BORDERS, SET_FPS, SET_RETAKE, SET_CREDITS, SET_BACK, SET_COUNT };
static int g_theme_changed; // the menu behind the settings page needs drawing in the new theme

// Changes setting `row` by dir (-1 / +1; A acts like +1). Returns 1 when the settings panel should close.
static int settings_change(int row, int dir, uint8_t* canvas, int* mode) {
    switch (row) {
    case SET_THEME:
        g_set.theme = (g_set.theme + dir + UI_THEME_COUNT) % UI_THEME_COUNT;
        g_theme_changed = 1;
        if (music_switch(THEME_MUSIC[g_set.theme]) != 0) logf_("menu music failed to start");
        break;
    case SET_MUSIC: g_set.music = (g_set.music + dir + 5) % 5; break;
    case SET_BORDERS: g_set.borders = !g_set.borders; break;
    case SET_FPS: g_set.fps = !g_set.fps; break;
    case SET_RETAKE:
        if (dir > 0) {
            music_pause(); // it stutters while the games run for their pictures
            for (int i = 0; i < g_ngames && running(); i++) {
                free(g_games[i].thumb);
                g_games[i].thumb = NULL;
                make_thumb(&g_games[i], i, g_ngames, canvas);
            }
            music_resume(1.5f);
        }
        return 0;
    case SET_CREDITS: if (dir > 0) *mode = 2; return 0;
    case SET_BACK: return dir > 0;
    }
    apply_settings();
    save_settings();
    return 0;
}

static int folder_sel; // the chosen tile inside the open folder

// Back from a folder to the main list, with the folder chosen there.
static void close_folder(int* sel) {
    for (int i = 0; i < g_ntop; i++)
        if (g_top[i] == -g_open - 1) *sel = i;
    g_open = -1;
}

// Shows the game picker until a game is chosen (returns 1, path in `chosen`) or the app should quit (0).
// The menu music fades out after the choice, before the game takes the audio device.
static int run_picker(uint8_t* canvas, int* sel, char* chosen, size_t chosen_len) {
    set_phase("%s", "the menu");
    chosen[0] = 0;
    int frame = 0, was_touched = 0, fade_frames = -1;
    uint64_t menu_busy = 0, upload_busy = 0, menu_span = OSGetSystemTime();
    ui_forget_screen(); // a game or the picture taking drew over the screen texture
    int mode = 0, set_sel = 0; // mode 0: games, 1: settings, 2: credits
    uint32_t prev = ~0u, script_hold = 0; // ~0: buttons held when the menu opens don't count as presses
    // A touch on a tile, and dragging it (see the games page below).
    int touch_down = 0, down_item = -1, down_x = 0, down_y = 0, down_frame = 0, dragging = 0;
    int hover = -1, hover_frames = 0, drop_target = -1, drop_insert = -1, drop_back = 0, scroll_frame = 0;
    int last_x = 0, last_y = 0;
    int script_touch = 0, script_x = 0, script_y = 0; // test scripts: "down x y", "move x y", "up"
    while (running()) {
        VPADStatus vs;
        VPADReadError err;
        memset(&vs, 0, sizeof(vs));
        VPADRead(VPAD_CHAN_0, &vs, 1, &err);
        int want_shot = 0;
        // (not while the music fades out: the next lines are the game's)
        for (; fade_frames < 0 && g_si < g_nscript && g_script[g_si].frame <= frame; g_si++) {
            ScriptLine* l = &g_script[g_si];
            if (!strcmp(l->cmd, "movie")) {
                snprintf(chosen, chosen_len, "%s", l->arg);
                fade_frames = 0;
                g_si++;
                break; // later lines belong to the game, whose frames count from 0
            }
            else if (!strcmp(l->cmd, "press")) script_hold |= button_by_name(l->arg);
            else if (!strcmp(l->cmd, "release")) script_hold &= ~button_by_name(l->arg);
            else if (!strcmp(l->cmd, "down") || !strcmp(l->cmd, "move")) {
                script_touch = 1;
                sscanf(l->arg, "%d %d", &script_x, &script_y);
            }
            else if (!strcmp(l->cmd, "up")) script_touch = 0;
            else if (!strcmp(l->cmd, "settext")) { kbd_cancel(); if (g_open >= 0) { snprintf(g_folders[g_open].name, sizeof(g_folders[g_open].name), "%s", l->arg); save_menu(); } }
            else if (!strcmp(l->cmd, "shot")) want_shot = 1;
            else if (!strcmp(l->cmd, "quit")) { logf_("autotest menu %d: quit", frame); return 0; }
        }
        Pads pads;
        pads_read(&vs, err, &pads);
        uint32_t hold = pads.hold | script_hold;
        uint32_t pressed = prev == ~0u ? 0 : hold & ~prev;
        prev = hold;
        // A new touch this frame, in 1280x720 coordinates.
        int touched = (err == VPAD_READ_SUCCESS && vs.tpNormal.touched) || script_touch;
        int tap = touched && !was_touched, tx = 0, ty = 0;
        was_touched = touched;
        if (script_touch) {
            tx = script_x;
            ty = script_y;
        } else if (touched) { // (held touches drag the scroll bar and tiles)
            VPADTouchData tp;
            VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &vs.tpNormal);
            tx = tp.x;
            ty = tp.y;
        }
        const uint32_t UP = VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP | VPAD_STICK_R_EMULATION_UP;
        const uint32_t DOWN = VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN | VPAD_STICK_R_EMULATION_DOWN;
        const uint32_t LEFT = VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT | VPAD_STICK_R_EMULATION_LEFT;
        const uint32_t RIGHT = VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT | VPAD_STICK_R_EMULATION_RIGHT;
        if (fade_frames >= 0) {
            // a game was chosen: ignore input while the music fades
        } else if (mode == 2) {
            if ((pressed & (VPAD_BUTTON_A | VPAD_BUTTON_B)) || tap) mode = 1;
        } else if (mode == 1) {
            if ((pressed & UP) && set_sel > 0) set_sel--;
            if ((pressed & DOWN) && set_sel + 1 < SET_COUNT) set_sel++;
            int dir = (pressed & LEFT) ? -1 : (pressed & (RIGHT | VPAD_BUTTON_A)) ? 1 : 0;
            if (tap) {
                int r = ui_settings_row_at(SET_COUNT, tx, ty);
                if (r >= 0) { set_sel = r; dir = 1; }
            }
            if (dir && settings_change(set_sel, dir, canvas, &mode)) mode = 0;
            if (pressed & (VPAD_BUTTON_B | VPAD_BUTTON_PLUS)) mode = 0;
        } else if (kbd_busy()) {
            // Renaming a folder on the system keyboard.
            static char name[128];
            if (kbd_update(err == VPAD_READ_SUCCESS ? &vs : NULL, name, sizeof(name)) == 1 && g_open >= 0 && name[0]) {
                snprintf(g_folders[g_open].name, sizeof(g_folders[g_open].name), "%s", name);
                save_menu();
            }
            touch_down = 0;
            prev = ~0u; // buttons held when it closes (the A that pressed OK) don't count
        } else {
            int* s_ptr = g_open >= 0 ? &folder_sel : sel;
            build_view();
            int n = g_nview;
            if (*s_ptr >= n) *s_ptr = n ? n - 1 : 0;
            int s = *s_ptr;
            int start = 0;
            if (g_open < 0 && ((pressed & VPAD_BUTTON_PLUS) || (tap && ui_settings_button_at(tx, ty)))) {
                mode = 1;
                set_sel = 0;
                touch_down = 0;
            } else if (g_open >= 0 && ((pressed & VPAD_BUTTON_X) || (tap && ui_settings_button_at(tx, ty)))) {
                kbd_open(g_folders[g_open].name, 40, 0, 0); // rename
                touch_down = 0;
            } else if (g_open >= 0 && ((pressed & VPAD_BUTTON_B) || (tap && ui_back_button_at(tx, ty)))) {
                close_folder(sel);
                touch_down = 0;
            } else if (n) {
                if ((pressed & RIGHT) && s + 1 < n) s++;
                if ((pressed & LEFT) && s > 0) s--;
                if (pressed & DOWN) s = s + UI_COLS < n ? s + UI_COLS : n - 1;
                if ((pressed & UP) && s >= UI_COLS) s -= UI_COLS;
                // L / R: a page (the rows on screen) up or down.
                if (pressed & VPAD_BUTTON_L) s = s >= UI_COLS * UI_ROWS ? s - UI_COLS * UI_ROWS : s % UI_COLS;
                if (pressed & VPAD_BUTTON_R) s = s + UI_COLS * UI_ROWS < n ? s + UI_COLS * UI_ROWS : n - 1;
                start = (pressed & VPAD_BUTTON_A) != 0;
                int scroll_row = touched && !dragging ? ui_scroll_at(n, tx, ty, tap) : -1;
                if (scroll_row >= 0) {
                    // On the scroll bar: jump (or drag) to that row, keeping the column.
                    s = scroll_row * UI_COLS + s % UI_COLS;
                    if (s >= n) s = n - 1;
                    touch_down = 0;
                } else if (tap) {
                    // A touch on a tile: a tap (let go soon, not moved) starts it; moving it or holding it
                    // picks it up.
                    down_item = ui_tile_at(n, tx, ty);
                    down_x = tx;
                    down_y = ty;
                    down_frame = frame;
                    touch_down = down_item >= 0;
                }
                if (touch_down && touched) {
                    int dx = tx - down_x, dy = ty - down_y;
                    if (!dragging && (dx * dx + dy * dy > 24 * 24 || frame - down_frame > 30)) {
                        dragging = 1;
                        s = down_item;
                        hover = -1;
                    }
                    if (dragging) {
                        // Held over the middle of another tile for a moment: drop target for a folder
                        // (nothing moves meanwhile). Otherwise it goes between tiles (the red bar).
                        int mid = ui_tile_middle_at(n, tx, ty);
                        int can_fold = g_open < 0 && mid >= 0 && mid != down_item && !g_view[down_item].folder;
                        if (can_fold && mid == hover) hover_frames++;
                        else { hover = can_fold ? mid : -1; hover_frames = 0; }
                        drop_target = hover >= 0 && hover_frames >= 20 ? hover : -1;
                        drop_back = ui_back_button_at(tx, ty);
                        drop_insert = drop_target < 0 && !drop_back ? ui_insert_at(n, tx, ty) : -1;
                        // Near the top or bottom of the grid: scroll a row every half second.
                        int edge = ty < 225 ? -1 : ty > 590 ? 1 : 0;
                        if (edge && frame - scroll_frame > 30) {
                            ui_set_top_row(ui_top_row() + edge, n);
                            scroll_frame = frame;
                        }
                        last_x = tx;
                        last_y = ty;
                    }
                } else if (touch_down && !touched) {
                    // Let go (the release sample has no position: where the finger last was).
                    if (dragging) {
                        if (drop_target >= 0) s = menu_make_folder(down_item, drop_target);
                        else if (drop_back) { menu_take_out(down_item); s = 0; }
                        else if (drop_insert >= 0) {
                            menu_move(down_item, drop_insert);
                            s = drop_insert > down_item ? drop_insert - 1 : drop_insert;
                        }
                        build_view();
                        n = g_nview;
                        if (s >= n) s = n ? n - 1 : 0;
                    } else if (frame - down_frame <= 30) {
                        s = down_item;
                        start = 1;
                    }
                    touch_down = dragging = 0;
                    drop_target = drop_insert = -1;
                    drop_back = 0;
                }
            }
            if (g_open < 0 && s_ptr != sel) s_ptr = sel; // (a folder closed by moving its last game out)
            *s_ptr = s;
            if (start && s < g_nview) {
                if (g_view[s].folder) {
                    g_open = atoi(g_view[s].path + 7); // "folder:<n>"
                    folder_sel = 0;
                    build_view();
                } else {
                    snprintf(chosen, chosen_len, "%s", g_view[s].path);
                    fade_frames = 0;
                }
            }
        }
        if (mode == 0) {
            ui_set_folder(g_open >= 0 ? g_folders[g_open].name : NULL);
            ui_set_drag(dragging ? down_item : -1, last_x, last_y, drop_target, drop_insert, drop_back);
        }
        if (fade_frames == 0) {
            music_sfx_launch();
            music_fade_to(0.0f, 0.8f);
        }
        // Launch animation (tile pop + fade to black, ~0.9 s), or the menu fading in when it opens.
        float launch = fade_frames >= 0 ? fminf((float)(fade_frames + 1) / 54.0f, 1.0f) : 0.0f;
        gfx_gx2_set_fade(fade_frames >= 0 ? 1.0f - launch : fminf((float)frame / 20.0f, 1.0f));
        if (fade_frames >= 0) {
            fade_frames++;
            // Done once the screen is black and the music silent (at most ~1.5 s, in case audio isn't running).
            if ((music_faded_out() && fade_frames >= 54) || fade_frames > 90) {
                music_pause();
                return 1;
            }
        }
        music_update();
        float t = (float)frame / 60.0f;
        uint64_t d0 = OSGetSystemTime();
        // Under a settings or credits page the menu isn't drawn: the page keeps a dimmed copy of it.
        if (mode == 0) {
            build_view();
            ui_draw_picker(canvas, g_view, g_nview, g_open >= 0 ? folder_sel : *sel, t, music_level(), launch);
        }
        if (mode >= 1 && g_theme_changed) {
            build_view();
            ui_draw_picker(canvas, g_view, g_nview, g_open >= 0 ? folder_sel : *sel, t, music_level(), 0.0f);
        }
        g_theme_changed = 0;
        if (mode == 1) {
            const char* labels[SET_COUNT] = { "Theme", "Menu music", "Side borders", "FPS counter",
                                              "Retake game pictures", "Credits", "Back" };
            const char* values[SET_COUNT] = { THEME_NAMES[g_set.theme], MUSIC_NAMES[g_set.music],
                                              g_set.borders ? "On" : "Off", g_set.fps ? "On" : "Off", "", "", "" };
            ui_draw_settings(canvas, labels, values, SET_COUNT, set_sel, t);
        } else if (mode == 2) {
            ui_draw_credits(canvas, t);
        }
        uint64_t d1 = OSGetSystemTime();
        int rects[32][4];
        int nrects = ui_take_dirty(rects, 32);
        gfx_tex_update_screen_rects(canvas, (const int (*)[4])rects, nrects);
        menu_busy += d1 - d0;
        upload_busy += OSGetSystemTime() - d1;
        if (want_shot) screenshot(frame);
        present();
        frame++;
        if (frame % 120 == 0) {
            // The menu is drawn on the CPU: this must stay well under 16.7 ms for 60 fps.
            uint64_t now = OSGetSystemTime();
            uint32_t span = (uint32_t)OSTicksToMilliseconds(now - menu_span);
            uint32_t us = (uint32_t)OSTicksToMicroseconds(menu_busy) / 120;
            uint32_t up = (uint32_t)OSTicksToMicroseconds(upload_busy) / 120;
            logf_("menu (%s): %u fps, drawing %u.%u ms, upload %u.%u ms per frame, music level %d%%",
                  mode == 0 ? "games" : mode == 1 ? "settings" : "credits", (unsigned)(120000 / (span ? span : 1)),
                  (unsigned)(us / 1000), (unsigned)(us % 1000 / 100), (unsigned)(up / 1000), (unsigned)(up % 1000 / 100),
                  (int)(music_level() * 100.0f));
            menu_busy = upload_busy = 0;
            menu_span = now;
        }
    }
    return 0;
}

// The FPS counter (settings): "NN FPS" in yellow 3x5 pixel digits, 4x size, on a black box.
static void show_fps(int fps) {
    static const uint16_t glyphs[13] = {
        0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF, // 0-9
        0x79A4, 0x7BE4, 0x79CF, // F P S
    };
    static uint8_t px[HUD_W * HUD_H * 4];
    for (int i = 0; i < HUD_W * HUD_H; i++) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
        px[i * 4 + 3] = 255;
    }
    char s[8];
    snprintf(s, sizeof(s), "%d", fps > 999 ? 999 : fps < 0 ? 0 : fps);
    int idx[8], n = 0;
    for (const char* p = s; *p; p++) idx[n++] = *p - '0';
    idx[n++] = -1; // space
    idx[n++] = 10;
    idx[n++] = 11;
    idx[n++] = 12;
    int x = 4;
    for (int c = 0; c < n; c++, x += 16) {
        if (idx[c] < 0) { x -= 8; continue; }
        uint16_t g = glyphs[idx[c]];
        for (int gy = 0; gy < 5; gy++)
            for (int gx = 0; gx < 3; gx++) {
                if (!(g >> (14 - gy * 3 - gx) & 1)) continue;
                for (int yy = 0; yy < 4; yy++)
                    for (int xx = 0; xx < 4; xx++) {
                        int X = x + gx * 4 + xx, Y = 4 + gy * 4 + yy;
                        if (X >= HUD_W) continue;
                        uint8_t* q = &px[(Y * HUD_W + X) * 4];
                        q[0] = 255; q[1] = 221; q[2] = 0;
                    }
            }
    }
    gfx_gx2_set_hud(px, HUD_W, HUD_H);
    g_hud_px = px;
}

// Plays a game until the player holds Minus (returns 1: back to the menu) or the app quits (0).
static int run_game(const char* path, uint8_t* canvas) {
    logf_("starting %s", path);
    // A loading screen until the game draws: unpacking a big file holds up the first tick for many
    // seconds on the console (DinoRun: 13 s), which used to look like a freeze.
    int gi = game_by_path(path);
    const char* slash = strrchr(path, '/');
#ifdef RW_DIRECT_GAME
    (void)gi;
    (void)slash;
    memset(canvas, 0, (size_t)SCREEN_W * SCREEN_H * 4); // plain black until the game draws
    for (size_t i = 3; i < (size_t)SCREEN_W * SCREEN_H * 4; i += 4) canvas[i] = 255;
#else
    ui_draw_loading(canvas, gi >= 0 ? g_games[gi].title : g_direct_title ? g_direct_title : slash ? slash + 1 : path);
#endif
    gfx_tex_update_screen(canvas);
    gfx_gx2_set_fade(1.0f);
    present();
    audio_init();
    // A fresh player: the picture captures or the last game may have left one running (or crashed).
    rw_core_reset();
    set_phase("playing %s", path);
    start_url(path, SCREEN_W, SCREEN_H);
    // Script frames count from the game's start.
    int gframe = 0, quit = 0, leave = 0, touching = 0, border_done = 0, minus_armed = 0;
    float cursor_x = SCREEN_W / 2, cursor_y = SCREEN_H / 2, cursor_idle = 99.0f, pointer_x = -1, pointer_y = -1;
    float script_sx = 0, script_sy = 0;
    int click_l_prev = 0, click_r_prev = 0, script_stick = 0;
    uint64_t cursor_last = OSGetSystemTime();
    {
        static uint8_t arrow[UI_CURSOR_W * UI_CURSOR_H * 4];
        ui_cursor_pixels(arrow);
        g_cursor_px = arrow;
        gfx_gx2_set_cursor(arrow, UI_CURSOR_W, UI_CURSOR_H);
    }
    if (!g_osk_px) g_osk_px = (uint8_t*)calloc((size_t)UI_W * UI_H, 4);
    memset(g_osk_held, 0, sizeof(g_osk_held));
    g_osk_down = -1;
    int osk_touch = 0, script_touch = 0, script_tx = 0, script_ty = 0;
    osk_set_open(0);
#ifdef RW_DIRECT_GAME
    gfx_gx2_panel_on(1, 0); // no on-screen keyboard button: the game never needs typing
#else
    gfx_gx2_panel_on(1, g_osk_px != NULL);
#endif
    double touch_x = 0, touch_y = 0;
    double audio_acc = 0;
    static int16_t audio_buf[48000 / 30 * 2];
    double audio_frame_ms = 16.7;
    int audio_started = 0, audio_peak = 0;
    uint32_t audio_underruns = 0;
    uint64_t last = OSGetSystemTime(), busy = 0, tick_busy = 0, present_busy = 0, span_start = last;
    uint32_t hold_prev = 0, script_hold = 0;
    uint32_t kb_last = 0, kb_ignore = 0;
    int kb_swallow = 0, kb_touch_ignore = 0, script_realtime = 0;
    uint32_t fps_frames = 0, game_frames = 0, win_frames = 0;
    uint64_t win_start = OSGetSystemTime(), win_tick = 0;
    uint64_t fps_start = OSGetSystemTime();
    while (running() && !quit && !leave) {
        VPADStatus vs;
        VPADReadError err;
        memset(&vs, 0, sizeof(vs));
        VPADRead(VPAD_CHAN_0, &vs, 1, &err);
        // A text field got focus (or was tapped again): the system keyboard, filled with its text. While it
        // is up the game gets no GamePad input; held keys and the touch are let go first.
        uint32_t kb_req = rw_core_keyboard();
        if (kb_req && kb_req != kb_last && !kbd_busy()) {
            static char text[1024];
            uint32_t info = 0;
            if (rw_core_text_get(text, sizeof(text), &info) >= 0) {
                kbd_open(text, (int)(info & 0xffff), (info & 0x10000) != 0, (info & 0x20000) != 0);
                logf_("keyboard opened for a text field (max %u chars)", (unsigned)(info & 0xffff));
            }
        }
        kb_last = kb_req;
        if (kbd_busy()) {
            static char text[1024];
            int r = kbd_update(err == VPAD_READ_SUCCESS ? &vs : NULL, text, sizeof(text));
            if (r == 1) rw_core_text_set(text);
            if (touching == 2) rw_core_mouse(2, touch_x, touch_y, 0);
            touching = 0;
            kb_swallow = 1;
        }
        Pads pads;
        pads_read(&vs, err, &pads);
        // Buttons still held when the keyboard goes away (the A that pressed OK) don't reach the game.
        if (kb_swallow && !kbd_busy()) {
            kb_ignore = pads.game_hold;
            kb_touch_ignore = 1; // until the finger is lifted
            kb_swallow = 0;
        }
        kb_ignore &= pads.game_hold;
        uint32_t hold = kbd_busy() ? script_hold : ((pads.game_hold & ~kb_ignore) | script_hold);
        for (size_t i = 0; i < sizeof(g_keymap) / sizeof(g_keymap[0]); i++) {
            uint32_t b = g_keymap[i].button;
            if ((hold & b) && !(hold_prev & b)) rw_core_key(1, g_keymap[i].key);
            if (!(hold & b) && (hold_prev & b)) rw_core_key(0, g_keymap[i].key);
        }
        // Minus goes back to the menu (once it has been seen up: the press that started things doesn't count).
        if (!(hold & VPAD_BUTTON_MINUS)) minus_armed = 1;
        else if (minus_armed && !(hold_prev & VPAD_BUTTON_MINUS)) leave = 1;
        hold_prev = hold;
        // The mouse on the left stick (or a Wii Remote's pointer): it shows when moved and hides when left alone.
        {
            uint64_t cnow = OSGetSystemTime();
            float cdt = (float)OSTicksToMicroseconds(cnow - cursor_last) / 1000000.0f;
            cursor_last = cnow;
            if (cdt > 0.05f) cdt = 0.05f;
            float lx = kbd_busy() ? 0 : (script_stick ? script_sx : pads.lx);
            float ly = kbd_busy() ? 0 : (script_stick ? script_sy : pads.ly);
            float mag = sqrtf(lx * lx + ly * ly);
            int moved = 0;
            if (mag > 0.18f) {
                float m = mag > 1.0f ? 1.0f : mag;
                m = (m - 0.18f) / 0.82f;
                float speed = (0.08f + 0.92f * m * m) * 1000.0f * cdt / mag; // pixels a second at full tilt
                cursor_x += lx * speed;
                cursor_y -= ly * speed;
                moved = 1;
            }
            if (pads.pointer_valid && !kbd_busy() &&
                (fabsf(pads.px - pointer_x) > 1.5f || fabsf(pads.py - pointer_y) > 1.5f)) {
                pointer_x = cursor_x = pads.px;
                pointer_y = cursor_y = pads.py;
                moved = 1;
            }
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_x > SCREEN_W - 1) cursor_x = SCREEN_W - 1;
            if (cursor_y > SCREEN_H - 1) cursor_y = SCREEN_H - 1;
            if (moved) {
                rw_core_mouse(0, cursor_x, cursor_y, 0);
                cursor_idle = 0;
            }
            int cl = !kbd_busy() && (pads.click_l || (script_hold & VPAD_BUTTON_L));
            int cr = !kbd_busy() && (pads.click_r || (script_hold & VPAD_BUTTON_R));
            if (cl != click_l_prev || cr != click_r_prev) {
                rw_core_mouse(0, cursor_x, cursor_y, 0);
                if (cl != click_l_prev) rw_core_mouse(cl ? 1 : 2, cursor_x, cursor_y, 0);
                if (cr != click_r_prev) rw_core_mouse(cr ? 1 : 2, cursor_x, cursor_y, 2);
                click_l_prev = cl;
                click_r_prev = cr;
                cursor_idle = 0;
            }
            if (cl || cr) cursor_idle = 0;
            cursor_idle += cdt;
            if (err == VPAD_READ_SUCCESS && vs.tpNormal.touched) cursor_idle = 99.0f; // the finger is the mouse now
            g_cursor_on = cursor_idle < 3.0f && !kbd_busy();
            g_cursor_x = (int)cursor_x;
            g_cursor_y = (int)cursor_y;
            gfx_gx2_set_cursor_pos(g_cursor_on, cursor_x, cursor_y);
        }
        if (err == VPAD_READ_SUCCESS && !vs.tpNormal.touched) kb_touch_ignore = 0;
        // A touch that starts on the on-screen keyboard or its tab stays there until the finger is lifted.
        {
            int t_on = script_touch, tx = script_tx, ty = script_ty;
            if (!script_touch && err == VPAD_READ_SUCCESS && vs.tpNormal.touched && !kbd_busy()) {
                VPADTouchData tp;
                VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &vs.tpNormal);
                t_on = tp.validity == VPAD_VALID;
                tx = tp.x;
                ty = tp.y;
            }
            if (t_on && !touching && !osk_touch && osk_press(tx, ty)) osk_touch = 1;
            if (osk_touch && !script_touch && (err != VPAD_READ_SUCCESS ? 0 : !vs.tpNormal.touched)) {
                osk_release();
                osk_touch = 0;
                kb_touch_ignore = 0;
            }
        }
        if (err == VPAD_READ_SUCCESS && !kbd_busy() && !kb_touch_ignore && !osk_touch) {
            VPADTouchData tp;
            VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &vs.tpNormal);
            if (vs.tpNormal.touched && tp.validity == VPAD_VALID) {
                // A new touch moves the mouse there first and presses on the next frame: games that aim at
                // the mouse each frame (Zuma's frog) otherwise fired toward where it was before the tap.
                rw_core_mouse(0, tp.x, tp.y, 0);
                if (touching == 1) {
                    rw_core_mouse(1, tp.x, tp.y, 0);
                    touching = 2;
                } else if (!touching) {
                    touching = 1;
                }
                touch_x = tp.x;
                touch_y = tp.y;
            } else if (touching && !vs.tpNormal.touched) {
                if (touching == 1) rw_core_mouse(1, touch_x, touch_y, 0); // a tap shorter than a frame
                // The release sample has no position: let go where the finger last was.
                rw_core_mouse(2, touch_x, touch_y, 0);
                touching = 0;
            }
        }
        int want_shot = 0;
        for (; g_si < g_nscript && g_script[g_si].frame <= gframe; g_si++) {
            ScriptLine* l = &g_script[g_si];
            double x = 0, y = 0;
            sscanf(l->arg, "%lf %lf", &x, &y);
            if (!strcmp(l->cmd, "move")) rw_core_mouse(0, x, y, 0);
            else if (!strcmp(l->cmd, "down")) rw_core_mouse(1, x, y, 0);
            else if (!strcmp(l->cmd, "up")) rw_core_mouse(2, x, y, 0);
            else if (!strcmp(l->cmd, "click") || !strcmp(l->cmd, "touch")) {
                rw_core_mouse(0, x, y, 0); rw_core_mouse(1, x, y, 0); rw_core_mouse(2, x, y, 0);
            }
            else if (!strcmp(l->cmd, "press")) script_hold |= button_by_name(l->arg);
            else if (!strcmp(l->cmd, "release")) script_hold &= ~button_by_name(l->arg);
            else if (!strcmp(l->cmd, "tdown")) { script_touch = 1; script_tx = (int)x; script_ty = (int)y; }
            else if (!strcmp(l->cmd, "tup")) { script_touch = 0; if (osk_touch) { osk_release(); osk_touch = 0; } }
            else if (!strcmp(l->cmd, "stick")) { script_stick = 1; script_sx = (float)x; script_sy = (float)y; }
            else if (!strcmp(l->cmd, "type")) { for (const char* p = l->arg; *p; p++) rw_core_text((unsigned char)*p); }
            else if (!strcmp(l->cmd, "settext")) { kbd_cancel(); rw_core_text_set(l->arg); } // as if typed on the keyboard
            else if (!strcmp(l->cmd, "cpufilters")) gfx_gx2_filters_on_cpu(1); // compare with the GPU versions
            else if (!strcmp(l->cmd, "realtime")) script_realtime = 1; else if (!strcmp(l->cmd, "profile")) prof_start(); else if (!strcmp(l->cmd, "profstop")) prof_stop(); // (the real clock, like normal play)
            else if (!strcmp(l->cmd, "shot")) want_shot = 1;
            else if (!strcmp(l->cmd, "quit")) { logf_("autotest %d: quit", gframe); quit = 1; }
            else if (!strcmp(l->cmd, "leave")) { leave = 1; g_si++; break; } // same as holding Minus;
            // later lines belong to the menu, whose frames count from 0 again
        }

        uint64_t now = OSGetSystemTime();
        double dt = (double)OSTicksToMicroseconds(now - last) / 1000.0;
        double raw_dt = dt;
        last = now;
        // A long frame (loading, the first use of a picture) isn't made up all at once: catching up 100 ms
        // ran several game frames in one, making the next frame long too (games stuttered as they started).
        if (dt > 50) dt = 50;
        if (g_nscript && !script_realtime) dt = 1000.0 / 60.0; // scripted runs: fixed steps, same as the PC host
        uint64_t t0 = OSGetSystemTime();
        int changed = rw_core_tick(dt);
        // The engine frees what a game dropped a little at a time, which is too slow for a game loading a new level
        // with the old one's pictures still around (Happy Wheels ran out of memory). When the block is nearly full,
        // and has filled further since the last time, everything unused is freed at once.
        {
            static uintptr_t collected_at;
            uint8_t* res_base;
            uint8_t* res_end;
            uint8_t* res_low;
            rw_res_range(&res_base, &res_end, &res_low);
            if (res_base && gfx_pool_top()) {
                uintptr_t used = (uintptr_t)(res_end - res_low) + (gfx_pool_top() - (uintptr_t)res_base);
                uintptr_t size = (uintptr_t)(res_end - res_base);
                if (used + ((uintptr_t)112 << 20) > size && used > collected_at + ((uintptr_t)8 << 20)) {
                    rw_core_collect();
                    logf_("nearly out of memory (%u MB): freed what the game no longer uses", (unsigned)(used >> 20));
                    collected_at = used;
                } else if (used + ((uintptr_t)32 << 20) < collected_at) {
                    collected_at = used;
                }
            }
        }        if (changed) { // what the player sees: a game made for 30 fps shows 30 here when it's on speed
            game_frames++;
            fps_frames++;
        }
        uint64_t t1 = OSGetSystemTime();
        // Drawn only when the game has a new picture: most Flash games run at 24-30 fps, and drawing the
        // same frame again at 60 doubled the CPU and GPU work. The last picture stays on screen.
        if (changed || gframe < 2 || want_shot) rw_core_render();
        busy += OSGetSystemTime() - t0;
        tick_busy += t1 - t0;

        // Border art for games that leave bars at the sides, made once the stage size is known.
        if (!border_done) {
            uint32_t w, h;
            rw_core_stage_size(&w, &h);
            if (w && h) {
                border_done = 1;
                float scale = (float)SCREEN_W / w < (float)SCREEN_H / h ? (float)SCREEN_W / w : (float)SCREEN_H / h;
                int rw = (int)(w * scale + 0.5f), rh = (int)(h * scale + 0.5f);
                int x = (SCREEN_W - rw) / 2, y = (SCREEN_H - rh) / 2;
                if (x >= 14 && g_set.borders) { // (the art scales to fit even narrow bars)
                    logf_("side art: game %ux%u, shown at x %d, %d wide", (unsigned)w, (unsigned)h, x, rw);
                    ui_draw_border(canvas, x, y, rw, rh);
                    gfx_gx2_set_border(canvas, (uint32_t)x, (uint32_t)y, (uint32_t)rw, (uint32_t)rh);
                    g_border_art = canvas;
                    g_border_rect[0] = x; g_border_rect[1] = y; g_border_rect[2] = rw; g_border_rect[3] = rh;
                }
            }
        }

        if (g_audio && !(g_nscript && !script_realtime)) {
            // Keep enough queued to last through the next frames, however slow they are. Going by the frame's
            // time gave at most 33 ms of sound a frame: any game under 30 fps ran short and sounded chopped up,
            // and under 15 fps it was mostly silence.
            audio_frame_ms += (raw_dt - audio_frame_ms) * 0.1;
            double ahead_ms = audio_frame_ms * 2.5;
            if (ahead_ms < 120) ahead_ms = 120; // (80 still ran dry a few times every 5 s)
            if (ahead_ms > 400) ahead_ms = 400;
            uint32_t target = (uint32_t)(rw_core_audio_rate() * ahead_ms / 1000.0);
            uint32_t queued = SDL_GetQueuedAudioSize(g_audio) / 4;
            if (!queued && audio_started) audio_underruns++;
            while (queued < target) {
                uint32_t n = target - queued;
                if (n > sizeof(audio_buf) / 4) n = sizeof(audio_buf) / 4;
                rw_core_audio(audio_buf, n);
                for (uint32_t i = 0; i < n * 2; i++) {
                    int v = audio_buf[i] < 0 ? -(int)audio_buf[i] : audio_buf[i];
                    if (v > audio_peak) audio_peak = v;
                }
                SDL_QueueAudio(g_audio, audio_buf, n * 4);
                queued += n;
            }
            audio_started = 1;
        } else {
            audio_acc += rw_core_audio_rate() * dt / 1000.0;
            uint32_t n = (uint32_t)audio_acc;
            if (n > sizeof(audio_buf) / 4) n = sizeof(audio_buf) / 4;
            audio_acc -= n;
            rw_core_audio(audio_buf, n);
            if (g_audio && SDL_GetQueuedAudioSize(g_audio) < rw_core_audio_rate() / 2 * 4)
                SDL_QueueAudio(g_audio, audio_buf, n * 4);
        }

        if (want_shot) shot_full(gframe);
        uint64_t p0 = OSGetSystemTime();
        present();
        present_busy += OSGetSystemTime() - p0;
        gframe++;
        // FPS counter: frames shown in the last half second, updated twice a second.
        if (g_set.fps) {
            uint32_t ms = (uint32_t)OSTicksToMilliseconds(OSGetSystemTime() - fps_start);
            if (ms >= 500) {
                show_fps((int)((fps_frames * 1000 + ms / 2) / ms));
                fps_frames = 0;
                fps_start = OSGetSystemTime();
            }
        }
        // Every 5 s whatever the frame count: a game at 5 fps takes a minute to reach 300 frames, and those are the
        // ones to look at.
        win_tick += t1 - t0;
        win_frames++;
        {
            uint64_t wn = OSGetSystemTime();
            uint32_t wms = (uint32_t)OSTicksToMilliseconds(wn - win_start);
            if (wms >= 5000) {
                uint32_t pct = (uint32_t)(OSTicksToMilliseconds(win_tick) * 100 / wms);
                logf_("  last %u.%u s: %u frames, logic %u%% of the time", (unsigned)(wms / 1000), (unsigned)(wms % 1000 / 100),
                      (unsigned)win_frames, (unsigned)pct);
                spots_window(pct > 40);
                win_start = wn;
                win_tick = 0;
                win_frames = 0;
            }
        }
        if (gframe % 300 == 0) {
            // What to look at for speed on the console: frames per second shown, and per frame the time in
            // the Flash player's logic (tick) and in building its drawing (render), in tenths of a ms.
            uint64_t now2 = OSGetSystemTime();
            uint32_t span_us = (uint32_t)OSTicksToMicroseconds(now2 - span_start);
            uint32_t tick_us = (uint32_t)OSTicksToMicroseconds(tick_busy) / 300;
            uint32_t render_us = (uint32_t)OSTicksToMicroseconds(busy - tick_busy) / 300;
            uint32_t present_us = (uint32_t)OSTicksToMicroseconds(present_busy) / 300; // GPU + vsync waits
            logf_("frame %d: %u.%u fps, logic %u.%u ms, render %u.%u ms, present %u.%u ms per frame", gframe,
                  (unsigned)(3000000000ull / (span_us ? span_us : 1) / 10), (unsigned)(3000000000ull / (span_us ? span_us : 1) % 10),
                  (unsigned)(tick_us / 1000), (unsigned)(tick_us % 1000 / 100), (unsigned)(render_us / 1000),
                  (unsigned)(render_us % 1000 / 100), (unsigned)(present_us / 1000), (unsigned)(present_us % 1000 / 100));
            // Where the drawing time went over those 300 frames (in total, not per frame).
            uint32_t st[8], gpu_mb, gpu_blocks;
            rw_core_render_stats(st);
            gfx_gx2_memory(&gpu_mb, &gpu_blocks);
            logf_("  movie loaded: %u.%u%%", (unsigned)(rw_core_load_permille() / 10), (unsigned)(rw_core_load_permille() % 10));
            logf_("  drawing: %u GPU filters, %u CPU filters (%u K pixels, %u ms), %u shapes built (%u ms), "
                  "%u KB of picture updates, %u draws into pictures; GPU memory %u MB in %u blocks",
                  (unsigned)st[7], (unsigned)st[0], (unsigned)st[1], (unsigned)(st[2] / 1000), (unsigned)st[3],
                  (unsigned)(st[4] / 1000), (unsigned)st[5], (unsigned)st[6], (unsigned)gpu_mb, (unsigned)gpu_blocks);
            logf_("  game speed: %u pictures a second (compare with the SWF's fps above)",
                  (unsigned)((uint64_t)game_frames * 1000000 / (span_us ? span_us : 1)));
            game_frames = 0;
            logf_("  audio: peak level %d of 32767, ran dry %u times, %u ms ahead", audio_peak, (unsigned)audio_underruns,
                  g_audio ? (unsigned)(SDL_GetQueuedAudioSize(g_audio) / 4 * 1000 / rw_core_audio_rate()) : 0);
            audio_peak = 0;
            audio_underruns = 0;
            {
                struct mallinfo mi = mallinfo();
                uint8_t* base;
                uint8_t* end;
                uint8_t* low;
                rw_res_range(&base, &end, &low);
                logf_("  memory: game %u MB, shared GPU %u MB, heap %u MB used of %u MB", (unsigned)((end - low) >> 20),
                      base ? (unsigned)((gfx_pool_top() ? gfx_pool_top() - (uintptr_t)base : 0) >> 20) : 0,
                      (unsigned)((mi.uordblks - (base ? g_res_size : 0)) >> 20), (unsigned)((mi.arena - (base ? g_res_size : 0)) >> 20));
            }
            busy = tick_busy = present_busy = 0;
            span_start = now2;
        }
        if (rw_core_dead() && !quit && !leave) {
            logf_("player crashed (wasm trap) at frame %d; back to the menu", gframe);
            leave = 1;
        }
    }
    gfx_gx2_set_border(NULL, 0, 0, 0, 0);
    g_border_art = NULL;
    gfx_gx2_set_hud(NULL, 0, 0);
    prof_stop(); g_spots_on = 0; gfx_gx2_set_cursor_pos(0, 0, 0);
    osk_set_open(0);
    gfx_gx2_panel_on(1, 0);
    g_cursor_on = 0;
    g_hud_px = NULL;
    kbd_cancel();
    if (g_audio) {
        SDL_CloseAudioDevice(g_audio);
        g_audio = 0;
    }
    rw_core_reset(); // stop the game and free everything it used
    if (leave) logf_("back to the menu");
    return leave && !quit && running();
}

static void app_main(void) {
    OSReport("flashplayeremu: app_main\n");
    WHBProcInit();
    mkdir(SD "/wiiu", 0777);
    mkdir(SD "/wiiu/apps", 0777);
    mkdir(APP_DIR, 0777);
    log_open();
    g_start_ticks = OSGetSystemTime();
    logf_("Flashplayer Emu starting");
    start_diagnostics();
    WHBGfxInit();
    VPADInit();
    pads_init();
    // Order matters for memory: the GPU pool (gfx_gx2_init) and the system keyboard take theirs before
    // rw_core_init reserves the games' memory out of what's left.
    int gfx_ok = gfx_gx2_init(SCREEN_W, SCREEN_H) == 0;
    if (gfx_ok && kbd_init()) gfx_gx2_set_overlay(kbd_draw);
    rw_core_set_content("fs:/vol/content");
    if (!gfx_ok || rw_core_init(SD) != 0) {
        logf_("init failed");
        while (running()) gfx_gx2_present();
        WHBProcShutdown();
        return;
    }
    load_script();

    scan("/wiiu/apps/flashplayeremu/swf", 1);
    scan("/swf", 1);
    qsort(g_games, (size_t)g_ngames, sizeof(g_games[0]), cmp_games);
    logf_("%d games found", g_ngames);
    load_menu(); // the player's order and folders

    uint8_t* canvas = (uint8_t*)malloc((size_t)SCREEN_W * SCREEN_H * 4);
    load_settings();

#ifdef RW_DIRECT_PATH
    {
        struct stat st;
        if (stat(SD RW_DIRECT_PATH, &st) != 0) {
            logf_("%s is missing", RW_DIRECT_PATH);
            ui_draw_loading(canvas, "Game files missing: copy them to sd:" RW_DIRECT_PATH);
            gfx_tex_update_screen(canvas);
            gfx_gx2_set_fade(1.0f);
            for (int i = 0; i < 60 * 15 && running(); i++) present();
        } else {
            run_game(RW_DIRECT_PATH, canvas);
        }
        logf_("leaving to the Wii U Menu");
        // Ask for the Wii U Menu, then only let ProcUI run (no drawing: the GPU goes away on the way out).
        // (Not when the system is already closing the app: it's on its way to the Wii U Menu by itself.)
        if (running()) {
            SYSLaunchMenu();
            while (running()) OSSleepTicks(OSMillisecondsToTicks(16));
        }
        gfx_gx2_gpu_lost();
        free(canvas);
        gfx_gx2_set_overlay(NULL);
        if (!g_closing) kbd_shutdown();
        log_close();
        WHBGfxShutdown();
        WHBProcShutdown();
        return;
    }
#endif

    // ---- game pictures (only for games not seen before)
    int missing = 0;
    for (int i = 0; i < g_ngames; i++)
        if (!load_thumb(&g_games[i])) missing++;
    for (int i = 0, done = 0; i < g_ngames && running(); i++)
        if (!g_games[i].thumb) make_thumb(&g_games[i], done++, missing, canvas);

    // The menu only ever plays its own bundled track (packed in the .wuhb, not read from the SD card).
    // Started after the game pictures: while a game runs for its picture the music isn't fed in time and
    // stutters.
    if (music_start(THEME_MUSIC[g_set.theme]) != 0) logf_("menu music failed to start");

    // ---- menu <-> game
    int sel = 0;
    char chosen[256];
    while (run_picker(canvas, &sel, chosen, sizeof(chosen))) {
        if (!run_game(chosen, canvas)) break;
        music_resume(1.5f); // back in the menu: the music fades back in where it left off
    }
    music_stop();
    free(canvas);
    logf_("exiting");
    gfx_gx2_set_overlay(NULL);
    if (!g_closing) kbd_shutdown();
    log_close();
    WHBGfxShutdown();
    WHBProcShutdown();
}

// Ruffle's wasm2c code recurses far deeper than the main thread's stack allows, so everything runs on
// a big heap stack (WASM_RT_MAX_CALL_STACK_DEPTH in CMakeLists.txt is sized for it).
#define STACK_SIZE (16 * 1024 * 1024)

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    OSReport("flashplayeremu: main\n");
    uint8_t* stack = (uint8_t*)MEMAllocFromDefaultHeapEx(STACK_SIZE, 64);
    OSReport("flashplayeremu: stack %p\n", stack);
    if (!stack) {
        OSReport("flashplayeremu: no memory for the %u byte stack\n", STACK_SIZE);
        return 1;
    }
    // Tell coreinit about the stack we run on (its stack checks use these bounds).
    OSThread* self = OSGetCurrentThread();
    void* old_start = self->stackStart;
    void* old_end = self->stackEnd;
    self->stackStart = stack + STACK_SIZE;
    self->stackEnd = stack;
    rw_call_on_stack(app_main, stack + STACK_SIZE);
    self->stackStart = old_start;
    self->stackEnd = old_end;
    MEMFreeToDefaultHeap(stack);
    return 0;
}
