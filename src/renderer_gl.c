/*
 * renderer_gl.c — OpenGL 3.3 core backend.
 *
 * Scope (v1):
 *   - Loads GL function pointers via host-supplied loader (glfwGetProcAddress
 *     or equivalent). Zero external GL-loader dependency.
 *   - Globals via std140 UBO at binding 0 (matches shaders/common.glsl).
 *   - Cell background pass: instanced quads (one instance per cell with a
 *     non-zero color) sourced from a per-frame VBO.
 *   - Post-process pass: scene rendered into an FBO color attachment, then
 *     composited to the default framebuffer via the post-process program
 *     (default identity, hot-swappable via set_custom_shader).
 *   - Text + cursor passes are stubbed; full glyph atlas + shaper lands in
 *     a follow-up commit. The vtable contract is honored regardless.
 */

#define SPIRITTY_BUILDING 1

#include "spiritty/spiritty_renderer.h"
#include "internal.h"
#include "shaders_embed.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Minimal GL types & constants we depend on ---------------------- */

typedef unsigned int  GLenum;
typedef unsigned int  GLbitfield;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef char          GLchar;
typedef float         GLfloat;
typedef unsigned char GLboolean;
typedef ptrdiff_t     GLintptr;
typedef ptrdiff_t     GLsizeiptr;

#define GL_FALSE                  0
#define GL_TRUE                   1
#define GL_NO_ERROR               0
#define GL_TRIANGLE_STRIP         0x0005
#define GL_TRIANGLES              0x0004
#define GL_FLOAT                  0x1406
#define GL_UNSIGNED_BYTE          0x1401
#define GL_RGBA                   0x1908
#define GL_RGBA8                  0x8058
#define GL_RED                    0x1903
#define GL_R8                     0x8229
#define GL_TEXTURE_2D             0x0DE1
#define GL_TEXTURE0               0x84C0
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_LINEAR                 0x2601
#define GL_NEAREST                0x2600
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_FRAMEBUFFER            0x8D40
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_COLOR_BUFFER_BIT       0x00004000
#define GL_DEPTH_BUFFER_BIT       0x00000100
#define GL_BLEND                  0x0BE2
#define GL_SRC_ALPHA              0x0302
#define GL_ONE                    1
#define GL_ONE_MINUS_SRC_ALPHA    0x0303
#define GL_ARRAY_BUFFER           0x8892
#define GL_UNIFORM_BUFFER         0x8A11
#define GL_STATIC_DRAW            0x88E4
#define GL_DYNAMIC_DRAW           0x88E8
#define GL_VERTEX_SHADER          0x8B31
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_INFO_LOG_LENGTH        0x8B84

/* ---- Function pointer table ----------------------------------------- */

typedef void          (*PFN_glClear)(GLbitfield);
typedef void          (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void          (*PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void          (*PFN_glEnable)(GLenum);
typedef void          (*PFN_glDisable)(GLenum);
typedef void          (*PFN_glBlendFunc)(GLenum, GLenum);
typedef void          (*PFN_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);

typedef void          (*PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void          (*PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void          (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void          (*PFN_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void          (*PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);
typedef void          (*PFN_glBindBufferBase)(GLenum, GLuint, GLuint);

typedef void          (*PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void          (*PFN_glDeleteVertexArrays)(GLsizei, const GLuint*);
typedef void          (*PFN_glBindVertexArray)(GLuint);
typedef void          (*PFN_glEnableVertexAttribArray)(GLuint);
typedef void          (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void          (*PFN_glVertexAttribDivisor)(GLuint, GLuint);

typedef GLuint        (*PFN_glCreateShader)(GLenum);
typedef void          (*PFN_glDeleteShader)(GLuint);
typedef void          (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void          (*PFN_glCompileShader)(GLuint);
typedef void          (*PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void          (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);

typedef GLuint        (*PFN_glCreateProgram)(void);
typedef void          (*PFN_glDeleteProgram)(GLuint);
typedef void          (*PFN_glAttachShader)(GLuint, GLuint);
typedef void          (*PFN_glLinkProgram)(GLuint);
typedef void          (*PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void          (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void          (*PFN_glUseProgram)(GLuint);
typedef GLuint        (*PFN_glGetUniformBlockIndex)(GLuint, const GLchar*);
typedef void          (*PFN_glUniformBlockBinding)(GLuint, GLuint, GLuint);
typedef GLint         (*PFN_glGetUniformLocation)(GLuint, const GLchar*);
typedef void          (*PFN_glUniform1i)(GLint, GLint);
typedef void          (*PFN_glUniform1f)(GLint, GLfloat);
typedef void          (*PFN_glUniform2f)(GLint, GLfloat, GLfloat);

typedef void          (*PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void          (*PFN_glDrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);

typedef void          (*PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void          (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void          (*PFN_glBindFramebuffer)(GLenum, GLuint);
typedef GLenum        (*PFN_glCheckFramebufferStatus)(GLenum);
typedef void          (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);

typedef void          (*PFN_glGenTextures)(GLsizei, GLuint*);
typedef void          (*PFN_glDeleteTextures)(GLsizei, const GLuint*);
typedef void          (*PFN_glBindTexture)(GLenum, GLuint);
typedef void          (*PFN_glActiveTexture)(GLenum);
typedef void          (*PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void          (*PFN_glTexParameteri)(GLenum, GLenum, GLint);

typedef GLenum        (*PFN_glGetError)(void);

typedef struct {
    PFN_glClear                  Clear;
    PFN_glClearColor             ClearColor;
    PFN_glViewport               Viewport;
    PFN_glEnable                 Enable;
    PFN_glDisable                Disable;
    PFN_glBlendFunc              BlendFunc;
    PFN_glBlendFuncSeparate      BlendFuncSeparate;
    PFN_glGenBuffers             GenBuffers;
    PFN_glDeleteBuffers          DeleteBuffers;
    PFN_glBindBuffer             BindBuffer;
    PFN_glBufferData             BufferData;
    PFN_glBufferSubData          BufferSubData;
    PFN_glBindBufferBase         BindBufferBase;
    PFN_glGenVertexArrays        GenVertexArrays;
    PFN_glDeleteVertexArrays     DeleteVertexArrays;
    PFN_glBindVertexArray        BindVertexArray;
    PFN_glEnableVertexAttribArray EnableVertexAttribArray;
    PFN_glVertexAttribPointer    VertexAttribPointer;
    PFN_glVertexAttribDivisor    VertexAttribDivisor;
    PFN_glCreateShader           CreateShader;
    PFN_glDeleteShader           DeleteShader;
    PFN_glShaderSource           ShaderSource;
    PFN_glCompileShader          CompileShader;
    PFN_glGetShaderiv            GetShaderiv;
    PFN_glGetShaderInfoLog       GetShaderInfoLog;
    PFN_glCreateProgram          CreateProgram;
    PFN_glDeleteProgram          DeleteProgram;
    PFN_glAttachShader           AttachShader;
    PFN_glLinkProgram            LinkProgram;
    PFN_glGetProgramiv           GetProgramiv;
    PFN_glGetProgramInfoLog      GetProgramInfoLog;
    PFN_glUseProgram             UseProgram;
    PFN_glGetUniformBlockIndex   GetUniformBlockIndex;
    PFN_glUniformBlockBinding    UniformBlockBinding;
    PFN_glGetUniformLocation     GetUniformLocation;
    PFN_glUniform1i              Uniform1i;
    PFN_glUniform1f              Uniform1f;
    PFN_glUniform2f              Uniform2f;
    PFN_glDrawArrays             DrawArrays;
    PFN_glDrawArraysInstanced    DrawArraysInstanced;
    PFN_glGenFramebuffers        GenFramebuffers;
    PFN_glDeleteFramebuffers     DeleteFramebuffers;
    PFN_glBindFramebuffer        BindFramebuffer;
    PFN_glCheckFramebufferStatus CheckFramebufferStatus;
    PFN_glFramebufferTexture2D   FramebufferTexture2D;
    PFN_glGenTextures            GenTextures;
    PFN_glDeleteTextures         DeleteTextures;
    PFN_glBindTexture            BindTexture;
    PFN_glActiveTexture          ActiveTexture;
    PFN_glTexImage2D             TexImage2D;
    PFN_glTexParameteri          TexParameteri;
    PFN_glGetError               GetError;
} sp_gl_api;

#define GL_LOAD(api, loader, name) \
    do { (api)->name = (PFN_gl##name)loader("gl" #name); } while (0)

static bool sp_gl_load(sp_gl_api* api, void* (*loader)(const char*)) {
    if (!loader) return false;
    GL_LOAD(api, loader, Clear);
    GL_LOAD(api, loader, ClearColor);
    GL_LOAD(api, loader, Viewport);
    GL_LOAD(api, loader, Enable);
    GL_LOAD(api, loader, Disable);
    GL_LOAD(api, loader, BlendFunc);
    GL_LOAD(api, loader, BlendFuncSeparate);
    GL_LOAD(api, loader, GenBuffers);
    GL_LOAD(api, loader, DeleteBuffers);
    GL_LOAD(api, loader, BindBuffer);
    GL_LOAD(api, loader, BufferData);
    GL_LOAD(api, loader, BufferSubData);
    GL_LOAD(api, loader, BindBufferBase);
    GL_LOAD(api, loader, GenVertexArrays);
    GL_LOAD(api, loader, DeleteVertexArrays);
    GL_LOAD(api, loader, BindVertexArray);
    GL_LOAD(api, loader, EnableVertexAttribArray);
    GL_LOAD(api, loader, VertexAttribPointer);
    GL_LOAD(api, loader, VertexAttribDivisor);
    GL_LOAD(api, loader, CreateShader);
    GL_LOAD(api, loader, DeleteShader);
    GL_LOAD(api, loader, ShaderSource);
    GL_LOAD(api, loader, CompileShader);
    GL_LOAD(api, loader, GetShaderiv);
    GL_LOAD(api, loader, GetShaderInfoLog);
    GL_LOAD(api, loader, CreateProgram);
    GL_LOAD(api, loader, DeleteProgram);
    GL_LOAD(api, loader, AttachShader);
    GL_LOAD(api, loader, LinkProgram);
    GL_LOAD(api, loader, GetProgramiv);
    GL_LOAD(api, loader, GetProgramInfoLog);
    GL_LOAD(api, loader, UseProgram);
    GL_LOAD(api, loader, GetUniformBlockIndex);
    GL_LOAD(api, loader, UniformBlockBinding);
    GL_LOAD(api, loader, GetUniformLocation);
    GL_LOAD(api, loader, Uniform1i);
    GL_LOAD(api, loader, Uniform1f);
    GL_LOAD(api, loader, Uniform2f);
    GL_LOAD(api, loader, DrawArrays);
    GL_LOAD(api, loader, DrawArraysInstanced);
    GL_LOAD(api, loader, GenFramebuffers);
    GL_LOAD(api, loader, DeleteFramebuffers);
    GL_LOAD(api, loader, BindFramebuffer);
    GL_LOAD(api, loader, CheckFramebufferStatus);
    GL_LOAD(api, loader, FramebufferTexture2D);
    GL_LOAD(api, loader, GenTextures);
    GL_LOAD(api, loader, DeleteTextures);
    GL_LOAD(api, loader, BindTexture);
    GL_LOAD(api, loader, ActiveTexture);
    GL_LOAD(api, loader, TexImage2D);
    GL_LOAD(api, loader, TexParameteri);
    GL_LOAD(api, loader, GetError);

    /* The handful we actually depend on for first-frame draw must resolve. */
    return api->Clear && api->CreateShader && api->CreateProgram &&
           api->GenBuffers && api->GenVertexArrays && api->DrawArraysInstanced &&
           api->GenFramebuffers && api->GenTextures;
}

/* ---- Globals UBO layout (matches shaders/common.glsl) -------------- */

typedef struct {
    float projection[16];   /* mat4 */
    float screen_size_px[2];
    float cell_size_px[2];
    float grid_size[2];
    float grid_padding[2];
    float default_bg[4];
    float default_fg[4];
    float cursor_color[4];
    float cursor_xy[2];
    int   cursor_style;
    int   cursor_visible;
    float min_contrast;
    float pad_a;
    float pad_b;
    float pad_c;
} sp_gl_globals;

/* Per-instance vertex layout for cell_bg. Matches cell_bg.v.glsl. */
typedef struct {
    float grid_x, grid_y;
    float r, g, b, a;
} sp_gl_bg_inst;

/* ---- Backend state -------------------------------------------------- */

typedef struct {
    sp_gl_api        gl;
    sp_renderer_opts opts;
    int32_t          fb_w, fb_h;

    bool             initialized;
    bool             in_frame;

    /* Programs */
    GLuint           prog_cell_bg;
    GLuint           prog_post;

    /* Geometry */
    GLuint           vao_cell_bg;
    GLuint           vbo_cell_bg;
    size_t           vbo_cell_bg_capacity;  /* bytes */

    GLuint           vao_post;              /* empty VAO; verts hardcoded */

    /* Globals UBO */
    GLuint           ubo_globals;

    /* Scene FBO + color texture */
    GLuint           fbo_scene;
    GLuint           tex_scene;
    int32_t          tex_scene_w, tex_scene_h;

    /* Custom post-process shader (replaces prog_post when set). */
    char*            custom_post_src;
} sp_renderer_gl;

/* ---- GLSL preamble assembly ----------------------------------------- */

static char* sp_strdup_(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* Assemble: #version + specialization defines + common.glsl + body. */
static char* sp_gl_assemble_source(const sp_renderer_gl* g, const char* body, bool include_common) {
    /* Worst-case bound: header + common + body + slack. */
    size_t cap = 4096 + strlen(body) + (include_common ? strlen(sp_shader_common) : 0);
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    int n = 0;
    n += snprintf(buf + n, cap - n, "#version 330 core\n");
    if (g->opts.use_linear_blending)
        n += snprintf(buf + n, cap - n, "#define SPIRITTY_USE_LINEAR_BLENDING 1\n");
    if (g->opts.use_linear_correction)
        n += snprintf(buf + n, cap - n, "#define SPIRITTY_USE_LINEAR_CORRECTION 1\n");
    if (g->opts.use_display_p3)
        n += snprintf(buf + n, cap - n, "#define SPIRITTY_USE_DISPLAY_P3 1\n");
    if (g->opts.use_msdf)
        n += snprintf(buf + n, cap - n, "#define SPIRITTY_USE_MSDF 1\n");
    if (g->opts.has_color_glyphs)
        n += snprintf(buf + n, cap - n, "#define SPIRITTY_HAS_COLOR_GLYPHS 1\n");
    if (include_common)
        n += snprintf(buf + n, cap - n, "%s\n", sp_shader_common);
    n += snprintf(buf + n, cap - n, "%s\n", body);
    (void)n;
    return buf;
}

static GLuint sp_gl_compile(const sp_gl_api* gl, GLenum stage, const char* src) {
    GLuint sh = gl->CreateShader(stage);
    if (!sh) return 0;
    const GLchar* p = (const GLchar*)src;
    gl->ShaderSource(sh, 1, &p, NULL);
    gl->CompileShader(sh);
    GLint ok = 0;
    gl->GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        gl->GetShaderInfoLog(sh, sizeof log, &n, log);
        fprintf(stderr, "spiritty[gl]: shader compile failed (stage=0x%x):\n%.*s\nsource:\n%s\n",
                stage, (int)n, log, src);
        gl->DeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint sp_gl_link(const sp_gl_api* gl, GLuint vs, GLuint fs) {
    GLuint prog = gl->CreateProgram();
    if (!prog) return 0;
    gl->AttachShader(prog, vs);
    gl->AttachShader(prog, fs);
    gl->LinkProgram(prog);
    GLint ok = 0;
    gl->GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        gl->GetProgramInfoLog(prog, sizeof log, &n, log);
        fprintf(stderr, "spiritty[gl]: program link failed:\n%.*s\n", (int)n, log);
        gl->DeleteProgram(prog);
        return 0;
    }
    return prog;
}

static GLuint sp_gl_build_program(sp_renderer_gl* g,
                                   const char* vsrc, const char* fsrc,
                                   bool include_common) {
    char* vfull = sp_gl_assemble_source(g, vsrc, include_common);
    char* ffull = sp_gl_assemble_source(g, fsrc, include_common);
    if (!vfull || !ffull) { free(vfull); free(ffull); return 0; }
    GLuint vs = sp_gl_compile(&g->gl, GL_VERTEX_SHADER, vfull);
    GLuint fs = sp_gl_compile(&g->gl, GL_FRAGMENT_SHADER, ffull);
    free(vfull); free(ffull);
    if (!vs || !fs) {
        if (vs) g->gl.DeleteShader(vs);
        if (fs) g->gl.DeleteShader(fs);
        return 0;
    }
    GLuint prog = sp_gl_link(&g->gl, vs, fs);
    g->gl.DeleteShader(vs);
    g->gl.DeleteShader(fs);
    if (!prog) return 0;

    /* Bind the Globals UBO to binding 0 in this program. */
    GLuint blk = g->gl.GetUniformBlockIndex(prog, "Globals");
    if (blk != (GLuint)-1) g->gl.UniformBlockBinding(prog, blk, 0);
    return prog;
}

/* ---- Projection matrix --------------------------------------------- */

static void sp_ortho2d(float* m, float w, float h) {
    /* Top-left origin, +Y down. Maps (0,0)..(w,h) -> NDC (-1,1)..(1,-1). */
    memset(m, 0, sizeof(float) * 16);
    m[0]  =  2.0f / w;
    m[5]  = -2.0f / h;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

/* ---- FBO management ------------------------------------------------ */

static void sp_gl_destroy_scene_fbo(sp_renderer_gl* g) {
    if (g->fbo_scene) g->gl.DeleteFramebuffers(1, &g->fbo_scene);
    if (g->tex_scene) g->gl.DeleteTextures(1, &g->tex_scene);
    g->fbo_scene = 0;
    g->tex_scene = 0;
    g->tex_scene_w = g->tex_scene_h = 0;
}

static bool sp_gl_ensure_scene_fbo(sp_renderer_gl* g, int32_t w, int32_t h) {
    if (g->fbo_scene && g->tex_scene_w == w && g->tex_scene_h == h) return true;
    sp_gl_destroy_scene_fbo(g);
    if (w <= 0 || h <= 0) return false;

    g->gl.GenTextures(1, &g->tex_scene);
    g->gl.BindTexture(GL_TEXTURE_2D, g->tex_scene);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g->gl.GenFramebuffers(1, &g->fbo_scene);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->fbo_scene);
    g->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g->tex_scene, 0);
    GLenum status = g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "spiritty[gl]: FBO incomplete (status=0x%x)\n", status);
        sp_gl_destroy_scene_fbo(g);
        return false;
    }
    g->tex_scene_w = w;
    g->tex_scene_h = h;
    return true;
}

/* ---- Cell BG geometry --------------------------------------------- */

static void sp_gl_init_cell_bg_geom(sp_renderer_gl* g) {
    g->gl.GenVertexArrays(1, &g->vao_cell_bg);
    g->gl.GenBuffers(1, &g->vbo_cell_bg);
    g->gl.BindVertexArray(g->vao_cell_bg);
    g->gl.BindBuffer(GL_ARRAY_BUFFER, g->vbo_cell_bg);
    /* Per-instance attribs: location 0 = grid_pos (vec2), 1 = color (vec4). */
    g->gl.EnableVertexAttribArray(0);
    g->gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(sp_gl_bg_inst),
                              (const void*)offsetof(sp_gl_bg_inst, grid_x));
    g->gl.VertexAttribDivisor(0, 1);
    g->gl.EnableVertexAttribArray(1);
    g->gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(sp_gl_bg_inst),
                              (const void*)offsetof(sp_gl_bg_inst, r));
    g->gl.VertexAttribDivisor(1, 1);
    g->gl.BindVertexArray(0);
}

/* Convert 0xRRGGBBAA -> floats. */
static void sp_color_unpack(uint32_t c, float out[4]) {
    out[0] = ((c >> 24) & 0xFF) / 255.0f;
    out[1] = ((c >> 16) & 0xFF) / 255.0f;
    out[2] = ((c >>  8) & 0xFF) / 255.0f;
    out[3] = ((c      ) & 0xFF) / 255.0f;
}

/* Build instance buffer from a cell batch. Returns instance count. */
static size_t sp_gl_pack_bg_instances(const sp_cell_batch* batch,
                                      uint32_t default_bg,
                                      sp_gl_bg_inst* out) {
    size_t n = 0;
    for (int32_t r = 0; r < batch->rows; ++r) {
        for (int32_t c = 0; c < batch->cols; ++c) {
            const sp_cell* cell = &batch->cells[(size_t)r * (size_t)batch->cols + (size_t)c];
            uint32_t bg = cell->attrs.bg;
            if (bg == default_bg) continue;
            sp_color_unpack(bg, &out[n].r);
            out[n].grid_x = (float)c;
            out[n].grid_y = (float)r;
            ++n;
        }
    }
    return n;
}

/* ---- vtable --------------------------------------------------------- */

static bool gl_init(void* self, const sp_renderer_opts* opts) {
    sp_renderer_gl* g = (sp_renderer_gl*)self;
    if (!opts) return false;
    g->opts = *opts;
    if (g->opts.cell_width_px  <= 0.0f) g->opts.cell_width_px  = 8.0f;
    if (g->opts.cell_height_px <= 0.0f) g->opts.cell_height_px = 16.0f;
    g->fb_w = opts->framebuffer_width;
    g->fb_h = opts->framebuffer_height;

    if (!sp_gl_load(&g->gl, opts->gl_loader)) {
        fprintf(stderr, "spiritty[gl]: failed to load GL functions\n");
        return false;
    }

    /* Compile programs. */
    g->prog_cell_bg = sp_gl_build_program(g, sp_shader_cell_bg_v, sp_shader_cell_bg_f, true);
    if (!g->prog_cell_bg) return false;
    g->prog_post    = sp_gl_build_program(g, sp_shader_post_v,    sp_shader_post_f,    false);
    if (!g->prog_post) return false;

    /* Geometry. */
    sp_gl_init_cell_bg_geom(g);
    g->gl.GenVertexArrays(1, &g->vao_post);

    /* Globals UBO. */
    g->gl.GenBuffers(1, &g->ubo_globals);
    g->gl.BindBuffer(GL_UNIFORM_BUFFER, g->ubo_globals);
    g->gl.BufferData(GL_UNIFORM_BUFFER, sizeof(sp_gl_globals), NULL, GL_DYNAMIC_DRAW);
    g->gl.BindBufferBase(GL_UNIFORM_BUFFER, 0, g->ubo_globals);

    /* Scene FBO. */
    if (!sp_gl_ensure_scene_fbo(g, g->fb_w, g->fb_h)) return false;

    /* Bind iChannel0 sampler in post program to texture unit 0. */
    g->gl.UseProgram(g->prog_post);
    GLint loc = g->gl.GetUniformLocation(g->prog_post, "iChannel0");
    if (loc >= 0) g->gl.Uniform1i(loc, 0);
    g->gl.UseProgram(0);

    g->initialized = true;
    return true;
}

static void gl_shutdown(void* self) {
    sp_renderer_gl* g = (sp_renderer_gl*)self;
    if (!g->initialized) return;
    if (g->prog_cell_bg) g->gl.DeleteProgram(g->prog_cell_bg);
    if (g->prog_post)    g->gl.DeleteProgram(g->prog_post);
    if (g->vbo_cell_bg)  g->gl.DeleteBuffers(1, &g->vbo_cell_bg);
    if (g->vao_cell_bg)  g->gl.DeleteVertexArrays(1, &g->vao_cell_bg);
    if (g->vao_post)     g->gl.DeleteVertexArrays(1, &g->vao_post);
    if (g->ubo_globals)  g->gl.DeleteBuffers(1, &g->ubo_globals);
    sp_gl_destroy_scene_fbo(g);
    free(g->custom_post_src);
    memset(g, 0, sizeof(*g));
}

static void gl_resize(void* self, int32_t fb_w, int32_t fb_h) {
    sp_renderer_gl* g = (sp_renderer_gl*)self;
    g->fb_w = fb_w;
    g->fb_h = fb_h;
    sp_gl_ensure_scene_fbo(g, fb_w, fb_h);
}

static void gl_begin_frame(void* self) {
    sp_renderer_gl* g = (sp_renderer_gl*)self;
    g->in_frame = true;
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->fbo_scene);
    g->gl.Viewport(0, 0, g->fb_w, g->fb_h);
    g->gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g->gl.Clear(GL_COLOR_BUFFER_BIT);
    g->gl.Enable(GL_BLEND);
    g->gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void gl_draw_cells(void* self, const sp_cell_batch* batch) {
    sp_renderer_gl* g = (sp_renderer_gl*)self;
    if (!g->in_frame || !batch || batch->cell_count == 0) return;

    /* Upload globals. */
    sp_gl_globals globals;
    memset(&globals, 0, sizeof globals);
    sp_ortho2d(globals.projection, (float)g->fb_w, (float)g->fb_h);
    globals.screen_size_px[0] = (float)g->fb_w;
    globals.screen_size_px[1] = (float)g->fb_h;
    globals.cell_size_px[0]   = g->opts.cell_width_px;
    globals.cell_size_px[1]   = g->opts.cell_height_px;
    globals.grid_size[0]      = (float)batch->cols;
    globals.grid_size[1]      = (float)batch->rows;
    globals.grid_padding[0]   = 0.0f;
    globals.grid_padding[1]   = 0.0f;
    globals.cursor_xy[0]      = (float)batch->cursor_col;
    globals.cursor_xy[1]      = (float)batch->cursor_row;
    globals.cursor_visible    = batch->cursor_visible ? 1 : 0;
    globals.min_contrast      = 1.0f;
    g->gl.BindBuffer(GL_UNIFORM_BUFFER, g->ubo_globals);
    g->gl.BufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(globals), &globals);

    /* Pack BG instances. */
    sp_gl_bg_inst* inst = (sp_gl_bg_inst*)malloc(batch->cell_count * sizeof(sp_gl_bg_inst));
    if (!inst) return;
    size_t n = sp_gl_pack_bg_instances(batch, /*default_bg=*/0x000000FFu, inst);
    if (n > 0) {
        size_t bytes = n * sizeof(sp_gl_bg_inst);
        g->gl.BindBuffer(GL_ARRAY_BUFFER, g->vbo_cell_bg);
        if (bytes > g->vbo_cell_bg_capacity) {
            g->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, inst, GL_DYNAMIC_DRAW);
            g->vbo_cell_bg_capacity = bytes;
        } else {
            g->gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, inst);
        }
        g->gl.UseProgram(g->prog_cell_bg);
        g->gl.BindVertexArray(g->vao_cell_bg);
        g->gl.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)n);
        g->gl.BindVertexArray(0);
        g->gl.UseProgram(0);
    }
    free(inst);
}

static void gl_end_frame(void* self) {
    sp_renderer_gl* g = (sp_renderer_gl*)self;
    if (!g->in_frame) return;
    g->in_frame = false;

    /* Composite scene -> default framebuffer via post-process program. */
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g->gl.Viewport(0, 0, g->fb_w, g->fb_h);
    g->gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g->gl.Clear(GL_COLOR_BUFFER_BIT);
    g->gl.UseProgram(g->prog_post);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->tex_scene);
    /* Update common shadertoy uniforms if present. */
    GLint loc;
    if ((loc = g->gl.GetUniformLocation(g->prog_post, "iResolution")) >= 0)
        g->gl.Uniform2f(loc, (float)g->fb_w, (float)g->fb_h);
    if ((loc = g->gl.GetUniformLocation(g->prog_post, "iTime")) >= 0)
        g->gl.Uniform1f(loc, 0.0f); /* host advances via set_custom_shader cycle */
    g->gl.BindVertexArray(g->vao_post);
    g->gl.DrawArrays(GL_TRIANGLES, 0, 3);
    g->gl.BindVertexArray(0);
    g->gl.UseProgram(0);
}

static bool gl_set_custom_shader(void* self, const char* glsl_source) {
    sp_renderer_gl* g = (sp_renderer_gl*)self;

    /* Cache the source so the host can re-fetch / re-link if specialization changes. */
    free(g->custom_post_src);
    g->custom_post_src = NULL;

    if (!glsl_source) {
        /* Restore default identity post pass. */
        if (g->prog_post) g->gl.DeleteProgram(g->prog_post);
        g->prog_post = sp_gl_build_program(g, sp_shader_post_v, sp_shader_post_f, false);
        return g->prog_post != 0;
    }

    g->custom_post_src = sp_strdup_(glsl_source);

    /* Build a fragment by prepending the shadertoy_prefix to the user body. */
    size_t plen = strlen(sp_shader_shadertoy_prefix);
    size_t blen = strlen(glsl_source);
    char* combined = (char*)malloc(plen + blen + 2);
    if (!combined) return false;
    memcpy(combined, sp_shader_shadertoy_prefix, plen);
    combined[plen] = '\n';
    memcpy(combined + plen + 1, glsl_source, blen + 1);

    GLuint new_prog = sp_gl_build_program(g, sp_shader_post_v, combined, false);
    free(combined);
    if (!new_prog) return false;

    if (g->prog_post) g->gl.DeleteProgram(g->prog_post);
    g->prog_post = new_prog;

    /* Re-bind sampler. */
    g->gl.UseProgram(g->prog_post);
    GLint loc = g->gl.GetUniformLocation(g->prog_post, "iChannel0");
    if (loc >= 0) g->gl.Uniform1i(loc, 0);
    g->gl.UseProgram(0);
    return true;
}

static void gl_upload_glyph(void* self, const sp_glyph* g_in) {
    /* Glyph atlas upload lands in the follow-up text-pass commit. */
    (void)self; (void)g_in;
}

static const sp_renderer_vtbl SP_GL_VTBL = {
    .init              = gl_init,
    .shutdown          = gl_shutdown,
    .resize            = gl_resize,
    .begin_frame       = gl_begin_frame,
    .draw_cells        = gl_draw_cells,
    .end_frame         = gl_end_frame,
    .set_custom_shader = gl_set_custom_shader,
    .upload_glyph      = gl_upload_glyph,
};

SPIRITTY_API sp_renderer* sp_renderer_gl_create(void) {
    sp_renderer*    r = (sp_renderer*)calloc(1, sizeof(sp_renderer));
    sp_renderer_gl* g = (sp_renderer_gl*)calloc(1, sizeof(sp_renderer_gl));
    if (!r || !g) { free(r); free(g); return NULL; }
    r->vt   = &SP_GL_VTBL;
    r->self = g;
    return r;
}

SPIRITTY_API void sp_renderer_gl_destroy(sp_renderer* r) {
    if (!r) return;
    if (r->self) {
        gl_shutdown(r->self);
        free(r->self);
    }
    free(r);
}
