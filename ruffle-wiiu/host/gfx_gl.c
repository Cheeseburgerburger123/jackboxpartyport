// OpenGL 2.1 + FBO implementation of the gfx_* layer for the PC test host.
// All render targets (screen and textures) are stored top row first, like Flash bitmaps, so
// textures can be sampled and read back without flips; only the final window blit flips.
#include "rw_core.h"
#include "gfx_gl.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLF(ret, name, args) static ret (APIENTRY* name) args;
#include "gl_funcs.inc"
#undef GLF
// Declared by the system headers but not exported by opengl32.dll: load it under another name.
static void (APIENTRY* p_glActiveTexture)(GLenum);

typedef struct {
    GLuint vbo, ibo;
    uint32_t nidx;
    int used;
} Mesh;

typedef struct {
    GLuint tex, fbo, stencil;
    uint32_t w, h;
    int used;
} Tex;

static Mesh* g_meshes;
static uint32_t g_nmeshes;
static Tex* g_texs;
static uint32_t g_ntexs;

static GLuint g_prog;
static GLint u_view, u_world, u_mult, u_add, u_kind, u_uvmat, u_gtype, u_spread, u_focal, u_tex;
static GLint a_pos, a_color;
static Tex g_screen; // screen-sized render target (id 0)

static const char* VS =
    "#version 120\n"
    "uniform vec4 view;\n"          // pixels -> clip: (2/w, sy*2/h, -1, -sy)
    "uniform mat3 world;\n"
    "uniform mat3 uvmat;\n"
    "attribute vec2 pos;\n"
    "attribute vec4 color;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = (world * vec3(pos, 1.0)).xy;\n"
    "  gl_Position = vec4(p.x * view.x + view.z, p.y * view.y + view.w, 0.0, 1.0);\n"
    "  v_uv = (uvmat * vec3(pos, 1.0)).xy;\n"
    "  v_color = color.abgr;\n"
    "}\n";

static const char* FS =
    "#version 120\n"
    "uniform vec4 mult;\n"
    "uniform vec4 add;\n"
    "uniform int kind;\n"
    "uniform int gtype;\n"
    "uniform int spread;\n"
    "uniform float focal;\n"
    "uniform sampler2D tex;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec4 c;\n"
    "  if (kind == 3) {\n"                       // Stage3D layer: raw RGB, alpha ignored
    "    gl_FragColor = vec4(texture2D(tex, v_uv).rgb, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  if (kind == 0) {\n"
    "    c = v_color;\n"
    "  } else if (kind == 1) {\n"
    "    c = texture2D(tex, v_uv);\n"
    "  } else {\n"
    "    float t;\n"
    "    if (gtype == 0) t = v_uv.x;\n"
    "    else if (gtype == 1) t = length(v_uv * 2.0 - 1.0);\n"
    "    else {\n"
    "      vec2 uv = v_uv * 2.0 - 1.0;\n"
    "      vec2 d = vec2(focal, 0.0) - uv;\n"
    "      float l = length(d);\n"
    "      d /= l;\n"
    "      t = l / (sqrt(1.0 - focal * focal * d.y * d.y) + focal * d.x);\n"
    "    }\n"
    "    if (spread == 0) t = clamp(t, 0.0, 1.0);\n"
    "    else if (spread == 1) t = fract(t);\n"
    "    else { t = abs(t); t = mod(t, 2.0); if (t > 1.0) t = 2.0 - t; }\n"
    "    c = texture2D(tex, vec2(t * (255.0 / 256.0) + 0.5 / 256.0, 0.5));\n"
    "  }\n"
    "  if (c.a > 0.0) {\n"
    "    c.rgb /= c.a;\n"
    "    c = clamp(c * mult + add, 0.0, 1.0);\n"
    "    c.rgb *= c.a;\n"
    "  } else if (add.a > 0.0) {\n"
    "    c = clamp(add, 0.0, 1.0);\n"
    "    c.rgb *= c.a;\n"
    "  }\n"
    "  gl_FragColor = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        plat_log(0, log);
    }
    return s;
}

static void make_target(Tex* t) {
    if (t->fbo) return;
    glGenFramebuffers(1, &t->fbo);
    glGenRenderbuffers(1, &t->stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, t->stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)t->w, (GLsizei)t->h);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->stencil);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->stencil);
}

static void free_target(Tex* t) {
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->stencil) glDeleteRenderbuffers(1, &t->stencil);
    t->fbo = t->stencil = 0;
}

int gfx_gl_init(uint32_t width, uint32_t height) {
#define GLF(ret, name, args) name = (ret (APIENTRY*) args)SDL_GL_GetProcAddress(#name); if (!name) { plat_log(0, "missing GL function " #name); return -1; }
#include "gl_funcs.inc"
#undef GLF
    p_glActiveTexture = (void (APIENTRY*)(GLenum))SDL_GL_GetProcAddress("glActiveTexture");
    if (!p_glActiveTexture) { plat_log(0, "missing GL function glActiveTexture"); return -1; }
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(g_prog, sizeof(log), NULL, log);
        plat_log(0, log);
        return -1;
    }
    u_view = glGetUniformLocation(g_prog, "view");
    u_world = glGetUniformLocation(g_prog, "world");
    u_mult = glGetUniformLocation(g_prog, "mult");
    u_add = glGetUniformLocation(g_prog, "add");
    u_kind = glGetUniformLocation(g_prog, "kind");
    u_uvmat = glGetUniformLocation(g_prog, "uvmat");
    u_gtype = glGetUniformLocation(g_prog, "gtype");
    u_spread = glGetUniformLocation(g_prog, "spread");
    u_focal = glGetUniformLocation(g_prog, "focal");
    u_tex = glGetUniformLocation(g_prog, "tex");
    a_pos = glGetAttribLocation(g_prog, "pos");
    a_color = glGetAttribLocation(g_prog, "color");
    gfx_gl_resize(width, height);
    return 0;
}

void gfx_gl_resize(uint32_t width, uint32_t height) {
    if (g_screen.tex) {
        free_target(&g_screen);
        glDeleteTextures(1, &g_screen.tex);
    }
    g_screen.w = width;
    g_screen.h = height;
    glGenTextures(1, &g_screen.tex);
    glBindTexture(GL_TEXTURE_2D, g_screen.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    make_target(&g_screen);
}

void gfx_gl_present(int win_w, int win_h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_screen.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_STENCIL_TEST);
    glBlitFramebuffer(0, 0, (GLint)g_screen.w, (GLint)g_screen.h, 0, win_h, win_w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void gfx_gl_read_screen(uint8_t* rgba) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_screen.fbo);
    glReadPixels(0, 0, (GLsizei)g_screen.w, (GLsizei)g_screen.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

// ---------------------------------------------------------------- resources

uint32_t gfx_mesh_create(const RwVertex* v, uint32_t nverts, const uint32_t* idx, uint32_t nidx) {
    uint32_t id = 1;
    while (id < g_nmeshes && g_meshes[id].used) id++;
    if (id >= g_nmeshes) {
        uint32_t n = g_nmeshes ? g_nmeshes * 2 : 1024;
        g_meshes = (Mesh*)realloc(g_meshes, n * sizeof(Mesh));
        memset(g_meshes + g_nmeshes, 0, (n - g_nmeshes) * sizeof(Mesh));
        if (id < 1) id = 1;
        g_nmeshes = n;
    }
    Mesh* m = &g_meshes[id];
    m->used = 1;
    m->nidx = nidx;
    glGenBuffers(1, &m->vbo);
    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nverts * sizeof(RwVertex)), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(nidx * 4), idx, GL_STATIC_DRAW);
    return id;
}

void gfx_mesh_free(uint32_t id) {
    if (id >= g_nmeshes || !g_meshes[id].used) return;
    glDeleteBuffers(1, &g_meshes[id].vbo);
    glDeleteBuffers(1, &g_meshes[id].ibo);
    memset(&g_meshes[id], 0, sizeof(Mesh));
}

uint32_t gfx_tex_create(uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride) {
    (void)stride;
    uint32_t id = 1;
    while (id < g_ntexs && g_texs[id].used) id++;
    if (id >= g_ntexs) {
        uint32_t n = g_ntexs ? g_ntexs * 2 : 1024;
        g_texs = (Tex*)realloc(g_texs, n * sizeof(Tex));
        memset(g_texs + g_ntexs, 0, (n - g_ntexs) * sizeof(Tex));
        g_ntexs = n;
    }
    Tex* t = &g_texs[id];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    uint8_t* zero = NULL;
    if (!rgba) {
        zero = (uint8_t*)calloc((size_t)w * h, 4);
        rgba = zero;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(zero);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}

void gfx_tex_mip(uint32_t id, uint32_t level, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride) {
    (void)id; (void)level; (void)w; (void)h; (void)rgba; (void)stride; // (the PC test host samples the full-size picture)
}

void gfx_tex_update(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride) {
    (void)stride;
    if (id >= g_ntexs || !g_texs[id].used) return;
    glBindTexture(GL_TEXTURE_2D, g_texs[id].tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void gfx_tex_free(uint32_t id) {
    if (id >= g_ntexs || !g_texs[id].used) return;
    free_target(&g_texs[id]);
    glDeleteTextures(1, &g_texs[id].tex);
    memset(&g_texs[id], 0, sizeof(Tex));
}

void gfx_tex_read(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out) {
    if (id >= g_ntexs || !g_texs[id].used) {
        memset(out, 0, (size_t)w * h * 4);
        return;
    }
    make_target(&g_texs[id]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_texs[id].fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

void gfx_reset(void) {
    for (uint32_t i = 0; i < g_nmeshes; i++) gfx_mesh_free(i);
    for (uint32_t i = 0; i < g_ntexs; i++) gfx_tex_free(i);
}

// The PC build filters on the CPU (the Wii U's GPU version is in wiiu/gfx_gx2.c).
int gfx_pb_run(uint32_t program, uint32_t target, const uint32_t* textures, const float* floats, uint32_t nfloats,
               const int32_t* ints, uint32_t nints, int zero_outside, uint32_t linear_mask) {
    (void)program; (void)target; (void)textures; (void)floats; (void)nfloats; (void)ints; (void)nints;
    (void)zero_outside; (void)linear_mask;
    return 0;
}

uint32_t gfx_h264_open(uint32_t w, uint32_t h) {
    (void)w; (void)h;
    return 0;
}

int gfx_h264_decode(uint32_t id, uint32_t tex, const uint8_t* data, uint32_t len) {
    (void)id; (void)tex; (void)data; (void)len;
    return 0;
}

void gfx_h264_close(uint32_t id) {
    (void)id;
}

void gfx_h264_reset(void) {
}

void gfx_h264_abandon(void) {
}

void gfx_tex_copy(uint32_t dst, uint32_t src) {
    (void)dst; (void)src;
}

uint32_t gfx_tex_create_float(uint32_t w, uint32_t h, const float* rgba) {
    (void)rgba;
    return gfx_tex_create(w, h, NULL, w * 4);
}

void gfx_tex_read_float(uint32_t id, uint32_t w, uint32_t h, float* out) {
    (void)id;
    memset(out, 0, (size_t)w * h * 16);
}

int gfx_blend_layer(uint32_t target, uint32_t layer, uint32_t mode, uint32_t mask_state, uint32_t mask_depth) {
    (void)target; (void)layer; (void)mode; (void)mask_state; (void)mask_depth;
    return 0;
}

int gfx_filter(uint32_t kind, uint32_t src, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t dst, int32_t dx,
               int32_t dy, const float* p, uint32_t np) {
    (void)kind; (void)src; (void)sx; (void)sy; (void)sw; (void)sh; (void)dst; (void)dx; (void)dy; (void)p; (void)np;
    return 0;
}

// ---------------------------------------------------------------- drawing

static float f(uint32_t w) {
    float v;
    memcpy(&v, &w, 4);
    return v;
}

static void set_mask(uint32_t state, uint32_t depth) {
    glDisable(GL_DEPTH_TEST); // Stage3D draws may have left it on
    switch (state) {
    case 0:
        glDisable(GL_STENCIL_TEST);
        glColorMask(1, 1, 1, 1);
        break;
    case 1:
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, (GLint)depth - 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        glColorMask(0, 0, 0, 0);
        break;
    case 2:
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, (GLint)depth, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glColorMask(1, 1, 1, 1);
        break;
    default:
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, (GLint)depth, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
        glColorMask(0, 0, 0, 0);
        break;
    }
}

static void set_blend(uint32_t mode) {
    GLenum eq = GL_FUNC_ADD, src = GL_ONE, dst = GL_ONE_MINUS_SRC_ALPHA;
    switch (mode) {
    case 3: src = GL_DST_COLOR; break;                                  // multiply
    case 4: dst = GL_ONE_MINUS_SRC_COLOR; break;                        // screen
    case 5: eq = GL_MAX; dst = GL_ONE; break;                           // lighten
    case 6: eq = GL_MIN; dst = GL_ONE; break;                           // darken
    case 8: dst = GL_ONE; break;                                        // add
    case 9: eq = GL_FUNC_REVERSE_SUBTRACT; dst = GL_ONE; break;         // subtract
    case 10: src = GL_ONE_MINUS_DST_COLOR; dst = GL_ONE_MINUS_SRC_COLOR; break; // invert (what's see-through stays)
    case 11: src = GL_ZERO; dst = GL_SRC_ALPHA; break;                  // alpha
    case 12: src = GL_ZERO; dst = GL_ONE_MINUS_SRC_ALPHA; break;        // erase
    case 255:                                                           // opaque copy (Stage3D layer)
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        return;
    default: break;                                                     // normal, layer, others
    }
    glBlendEquationSeparate(eq, GL_FUNC_ADD);
    glBlendFuncSeparate(src, dst, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void gfx_frame(uint32_t target, const uint32_t* c, uint32_t n) {
    Tex* t = &g_screen;
    if (target) {
        if (target >= g_ntexs || !g_texs[target].used) return;
        t = &g_texs[target];
        make_target(t);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    glUseProgram(g_prog);
    // Row 0 of every target is the top of the image: y pixel 0 -> clip -1.
    glUniform4f(u_view, 2.0f / (float)t->w, 2.0f / (float)t->h, -1.0f, -1.0f);
    glUniform1i(u_tex, 0);
    glEnable(GL_BLEND);
    set_blend(0);
    set_mask(0, 0);
    glStencilMask(0xff);

    uint32_t i = 0;
    while (i < n) {
        uint32_t op = c[i++];
        if (op == RW_OP_CLEAR) {
            glClearColor(f(c[i]), f(c[i + 1]), f(c[i + 2]), f(c[i + 3]));
            glClearStencil(0);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            i += 4;
        } else if (op == RW_OP_MASK) {
            set_mask(c[i], c[i + 1]);
            i += 2;
        } else if (op == RW_OP_BLEND) {
            set_blend(c[i]);
            i += 1;
        } else if (op == RW_OP_DRAW) {
            uint32_t mesh = c[i], first = c[i + 1], count = c[i + 2], primitive = c[i + 3];
            const uint32_t* w = &c[i + 4];
            GLfloat world[9] = { f(w[0]), f(w[1]), 0, f(w[2]), f(w[3]), 0, f(w[4]), f(w[5]), 1 };
            glUniformMatrix3fv(u_world, 1, GL_FALSE, world);
            glUniform4f(u_mult, f(w[6]), f(w[7]), f(w[8]), f(w[9]));
            glUniform4f(u_add, f(w[10]), f(w[11]), f(w[12]), f(w[13]));
            i += 4 + 14;
            uint32_t kind = c[i++];
            glUniform1i(u_kind, (GLint)kind);
            if (kind == RW_KIND_BITMAP || kind == RW_KIND_GRADIENT || kind == RW_KIND_RAW) {
                uint32_t tex = c[i];
                GLuint gltex = (tex < g_ntexs && g_texs[tex].used) ? g_texs[tex].tex : 0;
                glBindTexture(GL_TEXTURE_2D, gltex);
                const uint32_t* uv;
                if (kind != RW_KIND_GRADIENT) {
                    uint32_t flags = c[i + 1];
                    GLint filter = (flags & 1) ? GL_LINEAR : GL_NEAREST;
                    GLint wrap = (flags & 2) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
                    uv = &c[i + 2];
                    i += 2 + 6;
                } else {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glUniform1i(u_gtype, (GLint)c[i + 1]);
                    glUniform1i(u_spread, (GLint)c[i + 2]);
                    glUniform1f(u_focal, f(c[i + 3]));
                    uv = &c[i + 4];
                    i += 4 + 6;
                }
                GLfloat m[9] = { f(uv[0]), f(uv[1]), 0, f(uv[2]), f(uv[3]), 0, f(uv[4]), f(uv[5]), 1 };
                glUniformMatrix3fv(u_uvmat, 1, GL_FALSE, m);
            }
            if (mesh >= g_nmeshes || !g_meshes[mesh].used) continue;
            Mesh* m = &g_meshes[mesh];
            glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
            glEnableVertexAttribArray((GLuint)a_pos);
            glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, sizeof(RwVertex), (void*)0);
            if (a_color >= 0) {
                glEnableVertexAttribArray((GLuint)a_color);
                glVertexAttribPointer((GLuint)a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RwVertex), (void*)8);
            }
            GLenum mode = primitive == RW_PRIM_LINES ? GL_LINES : primitive == RW_PRIM_LINE_LOOP ? GL_LINE_LOOP : GL_TRIANGLES;
            glDrawElements(mode, (GLsizei)count, GL_UNSIGNED_INT, (void*)(uintptr_t)(first * 4));
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "bad render op %u", op);
            plat_log(0, msg);
            break;
        }
    }
    glDisable(GL_STENCIL_TEST);
    glColorMask(1, 1, 1, 1);
    if (getenv("RW_GFXLOG")) {
        uint32_t draws = 0;
        for (uint32_t k = 0; k < n; k++) draws += c[k] == RW_OP_DRAW; // rough (payload words can match)
        fprintf(stderr, "INFO gfx_frame target=%u words=%u ~draws=%u\n", target, n, draws);
    }
}

// ---------------------------------------------------------------- Stage3D

typedef struct {
    GLuint buf;
    uint32_t size;
    int used;
} C3dBuf;

static C3dBuf* g_c3d_bufs;
static uint32_t g_c3d_nbufs;
static GLuint g_c3d_ubo[2];

uint32_t gfx_c3d_flags(void) {
    // flip Y, keep uniform blocks (RW_C3D_FLAGS overrides, to test the Wii U's GLSL variant here)
    const char* e = getenv("RW_C3D_FLAGS");
    return e ? (uint32_t)atoi(e) : 1;
}

uint32_t gfx_c3d_buf_create(uint32_t size, int vertex) {
    (void)vertex; // OpenGL reads Flash's little-endian data as it is
    uint32_t id = 1;
    while (id < g_c3d_nbufs && g_c3d_bufs[id].used) id++;
    if (id >= g_c3d_nbufs) {
        uint32_t n = g_c3d_nbufs ? g_c3d_nbufs * 2 : 256;
        g_c3d_bufs = (C3dBuf*)realloc(g_c3d_bufs, n * sizeof(C3dBuf));
        memset(g_c3d_bufs + g_c3d_nbufs, 0, (n - g_c3d_nbufs) * sizeof(C3dBuf));
        g_c3d_nbufs = n;
    }
    C3dBuf* b = &g_c3d_bufs[id];
    b->used = 1;
    b->size = size;
    glGenBuffers(1, &b->buf);
    glBindBuffer(GL_ARRAY_BUFFER, b->buf);
    glBufferData(GL_ARRAY_BUFFER, size, NULL, GL_DYNAMIC_DRAW);
    return id;
}

void gfx_c3d_buf_upload(uint32_t id, uint32_t offset, const uint8_t* data, uint32_t len) {
    if (id >= g_c3d_nbufs || !g_c3d_bufs[id].used || offset + len > g_c3d_bufs[id].size) return;
    glBindBuffer(GL_ARRAY_BUFFER, g_c3d_bufs[id].buf);
    glBufferSubData(GL_ARRAY_BUFFER, offset, len, data);
}

void gfx_c3d_buf_free(uint32_t id) {
    if (id >= g_c3d_nbufs || !g_c3d_bufs[id].used) return;
    glDeleteBuffers(1, &g_c3d_bufs[id].buf);
    memset(&g_c3d_bufs[id], 0, sizeof(C3dBuf));
}

uint32_t gfx_c3d_program(const char* vs, const char* fs) {
    if (getenv("RW_GLSLLOG")) fprintf(stderr, "---- vertex\n%s\n---- fragment\n%s\n", vs, fs);
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        plat_log(0, log);
        plat_log(0, vs);
        plat_log(0, fs);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void gfx_c3d_program_free(uint32_t id) {
    if (id) glDeleteProgram(id);
}

static Tex* c3d_target(uint32_t id) {
    if (id >= g_ntexs || !g_texs[id].used) return NULL;
    make_target(&g_texs[id]);
    return &g_texs[id];
}

void gfx_c3d_clear(uint32_t target, const float rgba[4], float depth, uint32_t stencil, uint32_t mask) {
    Tex* t = c3d_target(target);
    if (!t) return;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    GLbitfield bits = 0;
    if (mask & 1) { glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]); bits |= GL_COLOR_BUFFER_BIT; }
    if (mask & 2) { glClearDepth(depth); bits |= GL_DEPTH_BUFFER_BIT; }
    if (mask & 4) { glClearStencil((GLint)stencil); bits |= GL_STENCIL_BUFFER_BIT; }
    if (bits) glClear(bits);
}

static GLenum c3d_blend(uint32_t f, int alpha) {
    static const GLenum color[] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                    GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                                    GL_ONE_MINUS_DST_COLOR };
    // The alpha channel uses the alpha version of color factors (as Ruffle's wgpu backend does).
    static const GLenum alpha_f[] = { GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA,
                                      GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA,
                                      GL_ONE_MINUS_DST_ALPHA };
    if (f > 9) f = 1;
    return alpha ? alpha_f[f] : color[f];
}

static GLenum c3d_stencil_op(uint32_t op) {
    static const GLenum ops[] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return ops[op & 7];
}

static GLenum c3d_face(uint32_t f) {
    return f == 1 ? GL_BACK : f == 2 ? GL_FRONT : GL_FRONT_AND_BACK;
}

void gfx_c3d_draw(const uint32_t* w, uint32_t n) {
    enum { HEAD = 27, ATTRS = 27, TEXS = ATTRS + 32, CONSTS = TEXS + 32, VC = 128 * 4, FC = 64 * 4 };
    if (n < CONSTS + VC + FC) return;
    Tex* t = c3d_target(w[0]);
    uint32_t ib = w[5];
    if (!t || !w[4] || ib >= g_c3d_nbufs || !g_c3d_bufs[ib].used) return;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    glUseProgram(w[4]);

    // Program constants: one uniform buffer per stage, at bindings 0 (vertex) and 1 (fragment).
    if (!g_c3d_ubo[0]) glGenBuffers(2, g_c3d_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_c3d_ubo[0]);
    glBufferData(GL_UNIFORM_BUFFER, VC * 4, &w[CONSTS], GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_c3d_ubo[0]);
    glBindBuffer(GL_UNIFORM_BUFFER, g_c3d_ubo[1]);
    glBufferData(GL_UNIFORM_BUFFER, FC * 4, &w[CONSTS + VC], GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_c3d_ubo[1]);

    // Vertex attributes va0..va7.
    static const GLint sizes[] = { 1, 2, 3, 4, 4 };
    for (GLuint i = 0; i < 8; i++) {
        const uint32_t* a = &w[ATTRS + i * 4];
        if (!a[0] || a[0] >= g_c3d_nbufs || !g_c3d_bufs[a[0]].used) {
            glDisableVertexAttribArray(i);
            continue;
        }
        glBindBuffer(GL_ARRAY_BUFFER, g_c3d_bufs[a[0]].buf);
        glEnableVertexAttribArray(i);
        int bytes4 = a[2] == 4;
        glVertexAttribPointer(i, sizes[a[2] > 4 ? 3 : a[2]], bytes4 ? GL_UNSIGNED_BYTE : GL_FLOAT, bytes4 ? GL_TRUE : GL_FALSE,
                              (GLsizei)a[3], (void*)(uintptr_t)a[1]);
    }

    // Textures fs0..fs7 (cube maps aren't supported yet).
    for (int i = 0; i < 8; i++) {
        const uint32_t* tx = &w[TEXS + i * 4];
        p_glActiveTexture(GL_TEXTURE0 + i);
        // (tx[1] marks a cube texture: a 2D strip of 6 faces that the GLSL samples itself, see context3d.rs)
        if (!tx[0] || tx[0] >= g_ntexs || !g_texs[tx[0]].used) {
            glBindTexture(GL_TEXTURE_2D, 0);
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, g_texs[tx[0]].tex);
        GLint filter = (tx[3] & 1) ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        int ru = tx[2] == 1 || tx[2] == 3, rv = tx[2] == 1 || tx[2] == 2;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ru ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, rv ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    }
    p_glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(c3d_blend(w[8], 0), c3d_blend(w[9], 0), c3d_blend(w[8], 1), c3d_blend(w[9], 1));
    glColorMask(w[13] & 1, (w[13] >> 1) & 1, (w[13] >> 2) & 1, (w[13] >> 3) & 1);

    if (w[3]) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_NEVER + (w[11] & 7));
        glDepthMask(w[10] ? GL_TRUE : GL_FALSE);
        // Stencil: the actions apply to the chosen faces; the others keep the stencil as it is.
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT_AND_BACK, GL_ALWAYS, 0, 0xff);
        glStencilOpSeparate(GL_FRONT_AND_BACK, GL_KEEP, GL_KEEP, GL_KEEP);
        if (w[19]) {
            GLenum f = c3d_face(w[19]);
            glStencilFuncSeparate(f, GL_NEVER + (w[20] & 7), (GLint)w[24], w[25]);
            glStencilOpSeparate(f, c3d_stencil_op(w[23]), c3d_stencil_op(w[22]), c3d_stencil_op(w[21]));
        }
        glStencilMask(w[26]);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
    }

    // The GLSL flips Y for OpenGL, which reverses the winding: Flash's clockwise front faces become CCW.
    if (w[12]) {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glCullFace(c3d_face(w[12]));
    } else {
        glDisable(GL_CULL_FACE);
    }

    if (w[14]) {
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)w[15], (GLint)w[16], (GLsizei)w[17], (GLsizei)w[18]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_c3d_bufs[ib].buf);
    glDrawElements(GL_TRIANGLES, (GLsizei)w[7], GL_UNSIGNED_SHORT, (void*)(uintptr_t)(w[6] * 2));

    // Back to the state the 2D renderer expects.
    for (GLuint i = 0; i < 8; i++) glDisableVertexAttribArray(i);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glColorMask(1, 1, 1, 1);
}
