/*
 * renderer_webgl.c — WebGL2 backend, compiled only under __EMSCRIPTEN__.
 *
 * Differences from renderer_gl.c:
 *   - Emscripten exposes GLES3 entry points directly as C symbols, so there
 *     is no host loader / function pointer table — we just call glFoo().
 *   - Shader preamble emits `#version 300 es` plus `precision mediump float;`
 *     in fragment stages. The shader bodies themselves are unchanged.
 *   - The WebGL2 context is created and made current on the JS side (TS
 *     wrapper calls emscripten_webgl_create_context + make_context_current
 *     on a #spiritty <canvas>), so init() neither asks for nor uses the
 *     gl_loader pointer in sp_renderer_opts.
 *
 * Everything else — std140 Globals UBO, instanced cell_bg quads, scene FBO
 * + post-process composite, custom shader hot-swap with the Shadertoy
 * prefix — is byte-for-byte identical to the GL backend's behavior.
 */

#if defined(__EMSCRIPTEN__)

#define SPIRITTY_BUILDING 1

#include "spiritty/spiritty_renderer.h"
#include "internal.h"
#include "shaders_embed.h"

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Globals UBO layout (matches shaders/common.glsl) ----------------- */

typedef struct {
    float projection[16];
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
} sp_webgl_globals;

typedef struct {
    float grid_x, grid_y;
    float r, g, b, a;
} sp_webgl_bg_inst;

typedef struct {
    sp_renderer_opts opts;
    int32_t          fb_w, fb_h;

    bool             initialized;
    bool             in_frame;

    GLuint           prog_cell_bg;
    GLuint           prog_post;

    GLuint           vao_cell_bg;
    GLuint           vbo_cell_bg;
    size_t           vbo_cell_bg_capacity;

    GLuint           vao_post;
    GLuint           ubo_globals;

    GLuint           fbo_scene;
    GLuint           tex_scene;
    int32_t          tex_scene_w, tex_scene_h;

    char*            custom_post_src;
} sp_renderer_webgl;

/* ---- GLSL preamble assembly ------------------------------------------ */

static char* sp_webgl_strdup_(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* Assemble: #version 300 es + (frag-only) precision + specialization defines
 * + common.glsl + body. The fragment-precision floor is required by GLES3 —
 * desktop GL gets it for free. */
static char* sp_webgl_assemble_source(const sp_renderer_webgl* g,
                                      const char* body,
                                      bool include_common,
                                      bool is_fragment) {
    size_t cap = 4096 + strlen(body) + (include_common ? strlen(sp_shader_common) : 0);
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    int n = 0;
    n += snprintf(buf + n, cap - n, "#version 300 es\n");
    if (is_fragment) {
        n += snprintf(buf + n, cap - n, "precision highp float;\n");
        n += snprintf(buf + n, cap - n, "precision highp int;\n");
        n += snprintf(buf + n, cap - n, "precision highp sampler2D;\n");
    }
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

static GLuint sp_webgl_compile(GLenum stage, const char* src) {
    GLuint sh = glCreateShader(stage);
    if (!sh) return 0;
    const GLchar* p = (const GLchar*)src;
    glShaderSource(sh, 1, &p, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof log, &n, log);
        fprintf(stderr, "spiritty[webgl]: shader compile failed (stage=0x%x):\n%.*s\nsource:\n%s\n",
                stage, (int)n, log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint sp_webgl_link(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    if (!prog) return 0;
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetProgramInfoLog(prog, sizeof log, &n, log);
        fprintf(stderr, "spiritty[webgl]: program link failed:\n%.*s\n", (int)n, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static GLuint sp_webgl_build_program(sp_renderer_webgl* g,
                                     const char* vsrc,
                                     const char* fsrc,
                                     bool include_common) {
    char* vfull = sp_webgl_assemble_source(g, vsrc, include_common, false);
    char* ffull = sp_webgl_assemble_source(g, fsrc, include_common, true);
    if (!vfull || !ffull) { free(vfull); free(ffull); return 0; }
    GLuint vs = sp_webgl_compile(GL_VERTEX_SHADER, vfull);
    GLuint fs = sp_webgl_compile(GL_FRAGMENT_SHADER, ffull);
    free(vfull); free(ffull);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = sp_webgl_link(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!prog) return 0;

    GLuint blk = glGetUniformBlockIndex(prog, "Globals");
    if (blk != GL_INVALID_INDEX) glUniformBlockBinding(prog, blk, 0);
    return prog;
}

static void sp_webgl_ortho2d(float* m, float w, float h) {
    memset(m, 0, sizeof(float) * 16);
    m[0]  =  2.0f / w;
    m[5]  = -2.0f / h;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

/* ---- FBO management --------------------------------------------------- */

static void sp_webgl_destroy_scene_fbo(sp_renderer_webgl* g) {
    if (g->fbo_scene) glDeleteFramebuffers(1, &g->fbo_scene);
    if (g->tex_scene) glDeleteTextures(1, &g->tex_scene);
    g->fbo_scene = 0;
    g->tex_scene = 0;
    g->tex_scene_w = g->tex_scene_h = 0;
}

static bool sp_webgl_ensure_scene_fbo(sp_renderer_webgl* g, int32_t w, int32_t h) {
    if (g->fbo_scene && g->tex_scene_w == w && g->tex_scene_h == h) return true;
    sp_webgl_destroy_scene_fbo(g);
    if (w <= 0 || h <= 0) return false;

    glGenTextures(1, &g->tex_scene);
    glBindTexture(GL_TEXTURE_2D, g->tex_scene);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &g->fbo_scene);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_scene);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->tex_scene, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "spiritty[webgl]: FBO incomplete (status=0x%x)\n", status);
        sp_webgl_destroy_scene_fbo(g);
        return false;
    }
    g->tex_scene_w = w;
    g->tex_scene_h = h;
    return true;
}

/* ---- Cell BG geometry ------------------------------------------------- */

static void sp_webgl_init_cell_bg_geom(sp_renderer_webgl* g) {
    glGenVertexArrays(1, &g->vao_cell_bg);
    glGenBuffers(1, &g->vbo_cell_bg);
    glBindVertexArray(g->vao_cell_bg);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_cell_bg);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(sp_webgl_bg_inst),
                          (const void*)offsetof(sp_webgl_bg_inst, grid_x));
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(sp_webgl_bg_inst),
                          (const void*)offsetof(sp_webgl_bg_inst, r));
    glVertexAttribDivisor(1, 1);
    glBindVertexArray(0);
}

static void sp_webgl_color_unpack(uint32_t c, float out[4]) {
    out[0] = ((c >> 24) & 0xFF) / 255.0f;
    out[1] = ((c >> 16) & 0xFF) / 255.0f;
    out[2] = ((c >>  8) & 0xFF) / 255.0f;
    out[3] = ((c      ) & 0xFF) / 255.0f;
}

static size_t sp_webgl_pack_bg_instances(const sp_cell_batch* batch,
                                         uint32_t default_bg,
                                         sp_webgl_bg_inst* out) {
    size_t n = 0;
    for (int32_t r = 0; r < batch->rows; ++r) {
        for (int32_t c = 0; c < batch->cols; ++c) {
            const sp_cell* cell = &batch->cells[(size_t)r * (size_t)batch->cols + (size_t)c];
            uint32_t bg = cell->attrs.bg;
            if (bg == default_bg) continue;
            sp_webgl_color_unpack(bg, &out[n].r);
            out[n].grid_x = (float)c;
            out[n].grid_y = (float)r;
            ++n;
        }
    }
    return n;
}

/* ---- vtable ----------------------------------------------------------- */

static bool webgl_init(void* self, const sp_renderer_opts* opts) {
    sp_renderer_webgl* g = (sp_renderer_webgl*)self;
    if (!opts) return false;
    g->opts = *opts;
    if (g->opts.cell_width_px  <= 0.0f) g->opts.cell_width_px  = 8.0f;
    if (g->opts.cell_height_px <= 0.0f) g->opts.cell_height_px = 16.0f;
    g->fb_w = opts->framebuffer_width;
    g->fb_h = opts->framebuffer_height;

    /* The TS wrapper is responsible for creating + making current the
     * WebGL2 context before any vtable method runs. We don't need a
     * loader — Emscripten links GLES3 calls directly. */

    g->prog_cell_bg = sp_webgl_build_program(g, sp_shader_cell_bg_v, sp_shader_cell_bg_f, true);
    if (!g->prog_cell_bg) return false;
    g->prog_post    = sp_webgl_build_program(g, sp_shader_post_v,    sp_shader_post_f,    false);
    if (!g->prog_post) return false;

    sp_webgl_init_cell_bg_geom(g);
    glGenVertexArrays(1, &g->vao_post);

    glGenBuffers(1, &g->ubo_globals);
    glBindBuffer(GL_UNIFORM_BUFFER, g->ubo_globals);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(sp_webgl_globals), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g->ubo_globals);

    if (!sp_webgl_ensure_scene_fbo(g, g->fb_w, g->fb_h)) return false;

    glUseProgram(g->prog_post);
    GLint loc = glGetUniformLocation(g->prog_post, "iChannel0");
    if (loc >= 0) glUniform1i(loc, 0);
    glUseProgram(0);

    g->initialized = true;
    return true;
}

static void webgl_shutdown(void* self) {
    sp_renderer_webgl* g = (sp_renderer_webgl*)self;
    if (!g->initialized) return;
    if (g->prog_cell_bg) glDeleteProgram(g->prog_cell_bg);
    if (g->prog_post)    glDeleteProgram(g->prog_post);
    if (g->vbo_cell_bg)  glDeleteBuffers(1, &g->vbo_cell_bg);
    if (g->vao_cell_bg)  glDeleteVertexArrays(1, &g->vao_cell_bg);
    if (g->vao_post)     glDeleteVertexArrays(1, &g->vao_post);
    if (g->ubo_globals)  glDeleteBuffers(1, &g->ubo_globals);
    sp_webgl_destroy_scene_fbo(g);
    free(g->custom_post_src);
    memset(g, 0, sizeof(*g));
}

static void webgl_resize(void* self, int32_t fb_w, int32_t fb_h) {
    sp_renderer_webgl* g = (sp_renderer_webgl*)self;
    g->fb_w = fb_w;
    g->fb_h = fb_h;
    sp_webgl_ensure_scene_fbo(g, fb_w, fb_h);
}

static void webgl_begin_frame(void* self) {
    sp_renderer_webgl* g = (sp_renderer_webgl*)self;
    g->in_frame = true;
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_scene);
    glViewport(0, 0, g->fb_w, g->fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void webgl_draw_cells(void* self, const sp_cell_batch* batch) {
    sp_renderer_webgl* g = (sp_renderer_webgl*)self;
    if (!g->in_frame || !batch || batch->cell_count == 0) return;

    sp_webgl_globals globals;
    memset(&globals, 0, sizeof globals);
    sp_webgl_ortho2d(globals.projection, (float)g->fb_w, (float)g->fb_h);
    globals.screen_size_px[0] = (float)g->fb_w;
    globals.screen_size_px[1] = (float)g->fb_h;
    globals.cell_size_px[0]   = g->opts.cell_width_px;
    globals.cell_size_px[1]   = g->opts.cell_height_px;
    globals.grid_size[0]      = (float)batch->cols;
    globals.grid_size[1]      = (float)batch->rows;
    globals.cursor_xy[0]      = (float)batch->cursor_col;
    globals.cursor_xy[1]      = (float)batch->cursor_row;
    globals.cursor_visible    = batch->cursor_visible ? 1 : 0;
    globals.min_contrast      = 1.0f;
    glBindBuffer(GL_UNIFORM_BUFFER, g->ubo_globals);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(globals), &globals);

    sp_webgl_bg_inst* inst = (sp_webgl_bg_inst*)malloc(batch->cell_count * sizeof(sp_webgl_bg_inst));
    if (!inst) return;
    size_t n = sp_webgl_pack_bg_instances(batch, /*default_bg=*/0x000000FFu, inst);
    if (n > 0) {
        size_t bytes = n * sizeof(sp_webgl_bg_inst);
        glBindBuffer(GL_ARRAY_BUFFER, g->vbo_cell_bg);
        if (bytes > g->vbo_cell_bg_capacity) {
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, inst, GL_DYNAMIC_DRAW);
            g->vbo_cell_bg_capacity = bytes;
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, inst);
        }
        glUseProgram(g->prog_cell_bg);
        glBindVertexArray(g->vao_cell_bg);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)n);
        glBindVertexArray(0);
        glUseProgram(0);
    }
    free(inst);
}

static void webgl_end_frame(void* self) {
    sp_renderer_webgl* g = (sp_renderer_webgl*)self;
    if (!g->in_frame) return;
    g->in_frame = false;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g->fb_w, g->fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g->prog_post);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex_scene);
    GLint loc;
    if ((loc = glGetUniformLocation(g->prog_post, "iResolution")) >= 0)
        glUniform2f(loc, (float)g->fb_w, (float)g->fb_h);
    if ((loc = glGetUniformLocation(g->prog_post, "iTime")) >= 0)
        glUniform1f(loc, 0.0f);
    glBindVertexArray(g->vao_post);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

static bool webgl_set_custom_shader(void* self, const char* glsl_source) {
    sp_renderer_webgl* g = (sp_renderer_webgl*)self;

    free(g->custom_post_src);
    g->custom_post_src = NULL;

    if (!glsl_source) {
        if (g->prog_post) glDeleteProgram(g->prog_post);
        g->prog_post = sp_webgl_build_program(g, sp_shader_post_v, sp_shader_post_f, false);
        return g->prog_post != 0;
    }

    g->custom_post_src = sp_webgl_strdup_(glsl_source);

    size_t plen = strlen(sp_shader_shadertoy_prefix);
    size_t blen = strlen(glsl_source);
    char* combined = (char*)malloc(plen + blen + 2);
    if (!combined) return false;
    memcpy(combined, sp_shader_shadertoy_prefix, plen);
    combined[plen] = '\n';
    memcpy(combined + plen + 1, glsl_source, blen + 1);

    GLuint new_prog = sp_webgl_build_program(g, sp_shader_post_v, combined, false);
    free(combined);
    if (!new_prog) return false;

    if (g->prog_post) glDeleteProgram(g->prog_post);
    g->prog_post = new_prog;

    glUseProgram(g->prog_post);
    GLint loc = glGetUniformLocation(g->prog_post, "iChannel0");
    if (loc >= 0) glUniform1i(loc, 0);
    glUseProgram(0);
    return true;
}

static void webgl_upload_glyph(void* self, const sp_glyph* g_in) {
    (void)self; (void)g_in;
}

static const sp_renderer_vtbl SP_WEBGL_VTBL = {
    .init              = webgl_init,
    .shutdown          = webgl_shutdown,
    .resize            = webgl_resize,
    .begin_frame       = webgl_begin_frame,
    .draw_cells        = webgl_draw_cells,
    .end_frame         = webgl_end_frame,
    .set_custom_shader = webgl_set_custom_shader,
    .upload_glyph      = webgl_upload_glyph,
};

SPIRITTY_API sp_renderer* sp_renderer_webgl_create(void) {
    sp_renderer*       r = (sp_renderer*)calloc(1, sizeof(sp_renderer));
    sp_renderer_webgl* g = (sp_renderer_webgl*)calloc(1, sizeof(sp_renderer_webgl));
    if (!r || !g) { free(r); free(g); return NULL; }
    r->vt   = &SP_WEBGL_VTBL;
    r->self = g;
    return r;
}

SPIRITTY_API void sp_renderer_webgl_destroy(sp_renderer* r) {
    if (!r) return;
    if (r->self) {
        webgl_shutdown(r->self);
        free(r->self);
    }
    free(r);
}

#endif /* __EMSCRIPTEN__ */
