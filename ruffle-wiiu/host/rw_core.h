// Shared C side of Ruffle for Wii U: owns the wasm2c instance and WASI, and calls into a
// platform layer (gfx_* and plat_*) that is implemented once with OpenGL (PC test host) and
// once with GX2 (Wii U).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y;
    uint32_t rgba; // r << 24 | g << 16 | b << 8 | a, premultiplied
} RwVertex;

// Command list opcodes, same as wiiu/src/host.rs.
enum { RW_OP_CLEAR = 1, RW_OP_DRAW = 2, RW_OP_MASK = 3, RW_OP_BLEND = 4 };
enum { RW_PRIM_TRIANGLES = 0, RW_PRIM_LINES = 1, RW_PRIM_LINE_LOOP = 2 };
// RW_KIND_RAW: bitmap payload, drawn as raw RGB with alpha 1 (the Stage3D layer)
enum { RW_KIND_COLOR = 0, RW_KIND_BITMAP = 1, RW_KIND_GRADIENT = 2, RW_KIND_RAW = 3 };

// ---- implemented by the platform ----
uint32_t gfx_mesh_create(const RwVertex* v, uint32_t nverts, const uint32_t* idx, uint32_t nidx);
void gfx_mesh_free(uint32_t id);
// rgba: RGBA8 rows (stride bytes apart), or NULL for a transparent texture.
uint32_t gfx_tex_create(uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride);
void gfx_tex_update(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride);
#ifdef __WIIU__
uint8_t* gfx_tex_update_begin(uint32_t id, uint32_t x, uint32_t y, uint32_t* pitch);
void gfx_tex_update_end(uint32_t id, uint32_t y, uint32_t h);
#endif
void rw_core_collect(void);
// BitmapData.perlinNoise, natively (perlin.c). flags: stitch, fractal, grayscale, transparent, then the channels.
typedef struct PerlinJob PerlinJob;
PerlinJob* perlin_job_new(uint32_t w, uint32_t h, double base_x, double base_y, uint32_t octaves, int64_t seed,
                          uint32_t flags, const double* offsets, uint8_t* out);
void perlin_rows(const PerlinJob* job, uint32_t y0, uint32_t y1);
void perlin_job_free(PerlinJob* job);
void gfx_tex_mip(uint32_t id, uint32_t level, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride);
void gfx_tex_free(uint32_t id);
void gfx_tex_read(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out);
// Runs a command list into the screen (target 0) or a texture.
void gfx_frame(uint32_t target, const uint32_t* cmds, uint32_t n);
// Frees every mesh and texture (the player that made them is gone).
void gfx_reset(void);
// A Flash filter on the GPU: kind 1 blur (p: blur x, blur y, passes), 2 glow / drop shadow (p: blur x,
// blur y, passes, strength, offset x, offset y, premultiplied r, g, b, a, inner, knockout, hide object),
// from texels (sx, sy, sw, sh) of texture src into texture dst at (dx, dy). Returns 0 if not done (the
// player then filters on the CPU).
// H.264 through the platform's decoder (0 / nothing where there is none).
uint32_t gfx_h264_open(uint32_t w, uint32_t h);
int gfx_h264_decode(uint32_t id, uint32_t tex, const uint8_t* data, uint32_t len);
void gfx_h264_close(uint32_t id);
void gfx_h264_reset(void);
// Pixel Bender: a program (gfx_c3d_program) drawn over a whole target with up to 8 input textures. Float textures
// hold RGBA as 32-bit floats.
int gfx_pb_run(uint32_t program, uint32_t target, const uint32_t* textures, const float* floats, uint32_t nfloats,
               const int32_t* ints, uint32_t nints, int zero_outside, uint32_t linear_mask);
void gfx_tex_copy(uint32_t dst, uint32_t src); // src 0: what the game has drawn on the screen so far
uint32_t gfx_tex_create_float(uint32_t w, uint32_t h, const float* rgba); // rgba NULL: empty, usable as a target
void gfx_tex_read_float(uint32_t id, uint32_t w, uint32_t h, float* out);
// Draws a layer over the target with a blend mode needing the backdrop's colours (7, 13, 14). 0 if not done.
int gfx_blend_layer(uint32_t target, uint32_t layer, uint32_t mode, uint32_t mask_state, uint32_t mask_depth);
int gfx_filter(uint32_t kind, uint32_t src, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t dst, int32_t dx,
               int32_t dy, const float* p, uint32_t np);

// Stage3D (see ruffle/wiiu/src/context3d.rs for the draw packet layout).
uint32_t gfx_c3d_flags(void); // bit 0: GLSL flips Y (OpenGL), bit 1: plain uniforms instead of blocks
uint32_t gfx_c3d_buf_create(uint32_t size, int vertex); // vertex: 32-bit values, else 16-bit indices
void gfx_c3d_buf_upload(uint32_t id, uint32_t offset, const uint8_t* data, uint32_t len); // little-endian bytes
void gfx_c3d_buf_free(uint32_t id);
uint32_t gfx_c3d_program(const char* vs, const char* fs); // 0 on failure
void gfx_c3d_program_free(uint32_t id);
void gfx_c3d_clear(uint32_t target, const float rgba[4], float depth, uint32_t stencil, uint32_t mask);
void gfx_c3d_draw(const uint32_t* words, uint32_t n);

void plat_log(int level, const char* msg);

// Network connections (TCP, TLS when `tls`), served by the platform. States: 0 connecting, 1 open,
// 2 failed, 3 closed by the other side (data already received can still be read).
uint32_t plat_sock_open(const char* host, uint32_t port, int tls);
int plat_sock_state(uint32_t id);
uint32_t plat_sock_send(uint32_t id, const uint8_t* data, uint32_t len);
uint32_t plat_sock_recv(uint32_t id, uint8_t* buf, uint32_t cap);
void plat_sock_close(uint32_t id);
uint64_t plat_time_ns(void);
void plat_random(uint8_t* buf, size_t n);

// ---- provided by rw_core.c ----
// root: host folder that the movie sees as "/" (WASI preopen).
int rw_core_init(const char* root);
// Throws the current player away (also after a trap) and starts a fresh wasm instance.
int rw_core_reset(void);
// url: e.g. "file:///swf/game.swf". Returns 0 on success.
int rw_core_start(const char* url, uint32_t width, uint32_t height);
// Where the player's ".content/" paths point (files bundled with the app).
void rw_core_set_content(const char* dir);
int rw_core_tick(double dt_ms);
void rw_core_render(void);
void rw_core_resize(uint32_t width, uint32_t height);
void rw_core_mouse(int kind, double x, double y, int button);
void rw_core_key(int down, uint32_t code);
void rw_core_text(uint32_t codepoint);
// Drawing statistics since the last call: filters run on the CPU, thousands of pixels filtered, us in
// filters, shapes tessellated, us tessellating, KB of texture updates, draws into textures, GPU filters.
void rw_core_render_stats(uint32_t out[8]);
// Non-zero while a focused text field wants an on-screen keyboard; changes with each new request.
uint32_t rw_core_keyboard(void);
// The focused text field's text (UTF-8, cut to cap - 1 bytes); returns its length, or -1 with no text
// field focused. info: max chars (0 = any) | 0x10000 password | 0x20000 multiline.
int rw_core_text_get(char* out, uint32_t cap, uint32_t* info);
// Replaces the focused text field's text, as if typed.
void rw_core_text_set(const char* text);
// Interleaved stereo s16.
void rw_core_audio(int16_t* out, uint32_t frames);
uint32_t rw_core_audio_rate(void);
// Root movie load progress 0..1000 (1000 = loaded).
uint32_t rw_core_load_permille(void);
// Stage size in pixels; 0 x 0 before the movie has loaded.
void rw_core_stage_size(uint32_t* w, uint32_t* h);
// Non-zero once the wasm code trapped (panic); the player is dead after that.
int rw_core_dead(void);

// Named key codes for rw_core_key (below 0x100 is ASCII).
enum {
    RW_KEY_LEFT = 0x100, RW_KEY_RIGHT, RW_KEY_UP, RW_KEY_DOWN, RW_KEY_ENTER, RW_KEY_ESCAPE,
    RW_KEY_BACKSPACE, RW_KEY_TAB, RW_KEY_SHIFT, RW_KEY_CONTROL, RW_KEY_DELETE, RW_KEY_PAGEUP,
    RW_KEY_PAGEDOWN
};

#ifdef __cplusplus
}
#endif
