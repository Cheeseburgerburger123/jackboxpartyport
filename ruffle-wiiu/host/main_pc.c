// PC test host: runs the same wasm2c Ruffle build as the Wii U with OpenGL, headless by default.
//   rwpc <root dir> <movie path under root> [-w 1280 -h 720] [-frames N] [-out dir] [-every N]
//        [-script file] [-show] [-wav out.wav]
// Script lines (sorted by frame): "<frame> move|down|up|click x y", "<frame> key <code>",
// "<frame> keydown|keyup <code>", "<frame> type <text>", "<frame> shot". Codes are ASCII
// characters or left/right/up/down/enter/escape/space/backspace/tab/shift/control.
#define SDL_MAIN_HANDLED
#include "rw_core.h"
#include "gfx_gl.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void plat_log(int level, const char* msg) {
    static const char* tags[] = { "ERROR", "WARN", "INFO" };
    fprintf(stderr, "%s %s\n", tags[level < 0 || level > 2 ? 2 : level], msg);
    fflush(stderr);
}

// No network on the PC test host (yet).
uint32_t plat_sock_open(const char* host, uint32_t port, int tls) { (void)host; (void)port; (void)tls; return 0; }
int plat_sock_state(uint32_t id) { (void)id; return 2; }
uint32_t plat_sock_send(uint32_t id, const uint8_t* data, uint32_t len) { (void)id; (void)data; (void)len; return 0; }
uint32_t plat_sock_recv(uint32_t id, uint8_t* buf, uint32_t cap) { (void)id; (void)buf; (void)cap; return 0; }
void plat_sock_close(uint32_t id) { (void)id; }

uint64_t plat_time_ns(void) {
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1e9 / (double)SDL_GetPerformanceFrequency());
}

void plat_random(uint8_t* buf, size_t n) {
    static uint32_t s = 0;
    if (!s) s = (uint32_t)time(NULL) | 1;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (uint8_t)s;
    }
}

static void write_bmp(const char* path, const uint8_t* rgba, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t* line = (uint8_t*)malloc((size_t)row + pad);
    memset(line, 0, (size_t)row + pad);
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

static uint32_t key_code(const char* s) {
    static const struct { const char* name; uint32_t code; } names[] = {
        { "left", RW_KEY_LEFT }, { "right", RW_KEY_RIGHT }, { "up", RW_KEY_UP }, { "down", RW_KEY_DOWN },
        { "enter", RW_KEY_ENTER }, { "escape", RW_KEY_ESCAPE }, { "space", ' ' },
        { "backspace", RW_KEY_BACKSPACE }, { "tab", RW_KEY_TAB }, { "shift", RW_KEY_SHIFT },
        { "control", RW_KEY_CONTROL },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(s, names[i].name)) return names[i].code;
    return (uint32_t)(unsigned char)s[0];
}

static uint32_t sdl_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_LEFT: return RW_KEY_LEFT;
    case SDLK_RIGHT: return RW_KEY_RIGHT;
    case SDLK_UP: return RW_KEY_UP;
    case SDLK_DOWN: return RW_KEY_DOWN;
    case SDLK_RETURN: return RW_KEY_ENTER;
    case SDLK_ESCAPE: return RW_KEY_ESCAPE;
    case SDLK_BACKSPACE: return RW_KEY_BACKSPACE;
    case SDLK_TAB: return RW_KEY_TAB;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return RW_KEY_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return RW_KEY_CONTROL;
    case SDLK_DELETE: return RW_KEY_DELETE;
    default: return (k > 0 && k < 0x80) ? (uint32_t)k : 0;
    }
}

typedef struct {
    int frame;
    char cmd[16];
    char arg[256];
} ScriptLine;

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: rwpc <root> <movie> [-w W -h H] [-frames N] [-out dir] [-every N] [-script f] [-show] [-wav f]\n");
        return 2;
    }
    const char* root = argv[1];
    const char* movie = argv[2];
    int w = 1280, h = 720, frames = 0, every = 0, show = 0, realtime = 0;
    const char* out = ".";
    const char* script_path = NULL;
    const char* wav_path = NULL;
    const char* spoof = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-script") && i + 1 < argc) script_path = argv[++i];
        else if (!strcmp(argv[i], "-wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "-show")) show = 1;
        else if (!strcmp(argv[i], "-realtime")) realtime = 1; // 60 frames a real second (games that wait on the clock)
        else if (!strcmp(argv[i], "-spoof") && i + 1 < argc) spoof = argv[++i];
    }

    ScriptLine* script = NULL;
    int nscript = 0;
    if (script_path) {
        FILE* f = fopen(script_path, "r");
        if (!f) { fprintf(stderr, "cannot open %s\n", script_path); return 2; }
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            ScriptLine l = { 0 };
            int consumed = 0;
            if (sscanf(line, "%d %15s %n", &l.frame, l.cmd, &consumed) < 2) continue;
            snprintf(l.arg, sizeof(l.arg), "%s", line + consumed);
            l.arg[strcspn(l.arg, "\r\n")] = 0;
            script = (ScriptLine*)realloc(script, sizeof(ScriptLine) * (nscript + 1));
            script[nscript++] = l;
        }
        fclose(f);
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | (show ? SDL_INIT_AUDIO : 0)) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* win = SDL_CreateWindow("Ruffle (wasm2c host)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                       SDL_WINDOW_OPENGL | (show ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN));
    if (!win || !SDL_GL_CreateContext(win)) {
        fprintf(stderr, "window/GL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(show ? 1 : 0);
    if (gfx_gl_init((uint32_t)w, (uint32_t)h) != 0) return 1;

    SDL_AudioDeviceID audio = 0;
    if (rw_core_init(root) != 0) return 1;
    uint32_t rate = rw_core_audio_rate();
    if (show) {
        SDL_AudioSpec want = { 0 }, have;
        want.freq = (int)rate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
    }

    char url[600];
    snprintf(url, sizeof(url), "file:///%s%s%s", movie[0] == '/' ? movie + 1 : movie, spoof ? "\n" : "", spoof ? spoof : "");
    if (rw_core_start(url, (uint32_t)w, (uint32_t)h) != 0) {
        fprintf(stderr, "rw_start failed\n");
        return 1;
    }

    FILE* wav = wav_path ? fopen(wav_path, "wb") : NULL;
    uint32_t wav_bytes = 0;
    if (wav) { uint8_t z[44] = { 0 }; fwrite(z, 1, 44, wav); }

    uint8_t* pixels = (uint8_t*)malloc((size_t)w * h * 4);
    int16_t audio_buf[48000 / 60 * 2 + 64];
    double audio_acc = 0;
    int si = 0, frame = 0, running = 1, shot_no = 0;
    uint64_t last = SDL_GetPerformanceCounter();
    uint64_t busy_ticks = 0;
    while (running && (frames == 0 || frame < frames) && !rw_core_dead()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_MOUSEMOTION: rw_core_mouse(0, ev.motion.x, ev.motion.y, 0); break;
            case SDL_MOUSEBUTTONDOWN: rw_core_mouse(1, ev.button.x, ev.button.y, ev.button.button == SDL_BUTTON_RIGHT ? 2 : 0); break;
            case SDL_MOUSEBUTTONUP: rw_core_mouse(2, ev.button.x, ev.button.y, ev.button.button == SDL_BUTTON_RIGHT ? 2 : 0); break;
            case SDL_KEYDOWN: if (!ev.key.repeat && sdl_key(ev.key.keysym.sym)) rw_core_key(1, sdl_key(ev.key.keysym.sym)); break;
            case SDL_KEYUP: if (sdl_key(ev.key.keysym.sym)) rw_core_key(0, sdl_key(ev.key.keysym.sym)); break;
            default: break;
            }
        }
        int want_shot = every > 0 && frame > 0 && frame % every == 0;
        for (; si < nscript && script[si].frame <= frame; si++) {
            ScriptLine* l = &script[si];
            double x = 0, y = 0;
            sscanf(l->arg, "%lf %lf", &x, &y);
            if (!strcmp(l->cmd, "move")) rw_core_mouse(0, x, y, 0);
            else if (!strcmp(l->cmd, "down")) rw_core_mouse(1, x, y, 0);
            else if (!strcmp(l->cmd, "up")) rw_core_mouse(2, x, y, 0);
            else if (!strcmp(l->cmd, "click")) { rw_core_mouse(0, x, y, 0); rw_core_mouse(1, x, y, 0); rw_core_mouse(2, x, y, 0); }
            else if (!strcmp(l->cmd, "keydown")) rw_core_key(1, key_code(l->arg));
            else if (!strcmp(l->cmd, "keyup")) rw_core_key(0, key_code(l->arg));
            else if (!strcmp(l->cmd, "key")) { rw_core_key(1, key_code(l->arg)); rw_core_key(0, key_code(l->arg)); }
            else if (!strcmp(l->cmd, "type")) { for (const char* p = l->arg; *p; p++) rw_core_text((unsigned char)*p); }
            else if (!strcmp(l->cmd, "shot")) want_shot = 1;
            else if (!strcmp(l->cmd, "movie")) {
                // Same as the Wii U menu starting another game: a fresh player in the same process.
                char next[600];
                snprintf(next, sizeof(next), "file:///%s", l->arg[0] == '/' ? l->arg + 1 : l->arg);
                rw_core_reset();
                if (rw_core_start(next, (uint32_t)w, (uint32_t)h) != 0) fprintf(stderr, "rw_start failed: %s\n", next);
            }
        }

        uint64_t t0 = SDL_GetPerformanceCounter();
        double dt = 1000.0 / 60.0;
        if (show) {
            uint64_t now = SDL_GetPerformanceCounter();
            dt = (double)(now - last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            last = now;
            if (dt > 100) dt = 100;
        }
        if (realtime) SDL_Delay(16);
        rw_core_tick(dt);
        rw_core_render();
        busy_ticks += SDL_GetPerformanceCounter() - t0;

        audio_acc += rate * dt / 1000.0;
        uint32_t n = (uint32_t)audio_acc;
        if (n > sizeof(audio_buf) / 4) n = sizeof(audio_buf) / 4;
        audio_acc -= n;
        rw_core_audio(audio_buf, n);
        if (audio && SDL_GetQueuedAudioSize(audio) < rate) SDL_QueueAudio(audio, audio_buf, n * 4);
        if (wav) { fwrite(audio_buf, 4, n, wav); wav_bytes += n * 4; }

        if (want_shot) {
            gfx_gl_read_screen(pixels);
            char path[700];
            snprintf(path, sizeof(path), "%s/shot_%04d_f%05d.bmp", out, shot_no++, frame);
            write_bmp(path, pixels, w, h);
            fprintf(stderr, "INFO shot %s\n", path);
        }
        if (show) {
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            gfx_gl_present(ww, wh);
            SDL_GL_SwapWindow(win);
        }
        frame++;
        if (frame % 300 == 0) {
            fprintf(stderr, "INFO frame %d, avg busy %.2f ms/frame\n", frame,
                    (double)busy_ticks * 1000.0 / (double)SDL_GetPerformanceFrequency() / 300.0);
            busy_ticks = 0;
        }
    }

    if (wav) {
        uint8_t hdr[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
        uint32_t riff = wav_bytes + 36, byte_rate = rate * 4;
        memcpy(hdr + 4, &riff, 4);
        memcpy(hdr + 24, &rate, 4);
        memcpy(hdr + 28, &byte_rate, 4);
        hdr[32] = 4; hdr[34] = 16;
        memcpy(hdr + 36, "data", 4);
        memcpy(hdr + 40, &wav_bytes, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(hdr, 1, 44, wav);
        fclose(wav);
    }
    int dead = rw_core_dead();
    fprintf(stderr, "INFO done after %d frames%s\n", frame, dead ? " (wasm trapped)" : "");
    SDL_Quit();
    return dead ? 3 : 0;
}
