/*
 * spiritty_renderer.h — pluggable renderer vtable.
 *
 * Two backends ship with the project today:
 *   - renderer_gl.c     (OpenGL 3.3 core, native)
 *   - renderer_webgl.c  (WebGL2, compiled under EMSCRIPTEN)
 *
 * Both consume the same GLSL sources from the src/shaders directory.
 */
#ifndef SPIRITTY_RENDERER_H
#define SPIRITTY_RENDERER_H

#include "spiritty.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t  framebuffer_width;
    int32_t  framebuffer_height;
    float    device_pixel_ratio;
    uint32_t atlas_size;          /* 0 → backend default */
    bool     premultiplied_alpha;
    bool     antialias;
} sp_renderer_opts;

/* A glyph the core asks the renderer to upload into its atlas. */
typedef struct {
    uint32_t       codepoint;
    uint16_t       width_px;
    uint16_t       height_px;
    int16_t        bearing_x;
    int16_t        bearing_y;
    float          advance;
    const uint8_t* pixels;        /* R8 grayscale, width_px * height_px */
} sp_glyph;

/* A batched draw call worth of cells. The renderer is free to copy or
 * keep the pointer alive only for the duration of draw_cells(). */
typedef struct {
    const sp_cell* SPIRITTY_RESTRICT cells;
    size_t                  cell_count;
    int32_t                 cols;
    int32_t                 rows;
    int32_t                 cursor_row;
    int32_t                 cursor_col;
    bool                    cursor_visible;
    float                   time_seconds;  /* for shader iTime */
} sp_cell_batch;

typedef struct sp_renderer_vtbl {
    bool (*init)(void* self, const sp_renderer_opts* opts);
    void (*shutdown)(void* self);
    void (*resize)(void* self, int32_t fb_w, int32_t fb_h);
    void (*begin_frame)(void* self);
    void (*draw_cells)(void* self, const sp_cell_batch* batch);
    void (*end_frame)(void* self);
    bool (*set_custom_shader)(void* self, const char* glsl_source);
    void (*upload_glyph)(void* self, const sp_glyph* glyph);
} sp_renderer_vtbl;

struct sp_renderer {
    const sp_renderer_vtbl* vt;
    void*                   self;
};

/* Concrete backend factories. Each lives in its own translation unit and
 * is only linked when the corresponding option is enabled. */

/* Null renderer — always available. Records vtable activity for tests
 * and serves as a reference implementation of the contract. */
SPIRITTY_API sp_renderer* sp_renderer_null_create(void);
SPIRITTY_API void         sp_renderer_null_destroy(sp_renderer* r);
SPIRITTY_API bool         sp_renderer_null_initialized(const sp_renderer* r);
SPIRITTY_API uint64_t     sp_renderer_null_begin_frames(const sp_renderer* r);
SPIRITTY_API uint64_t     sp_renderer_null_end_frames(const sp_renderer* r);
SPIRITTY_API uint64_t     sp_renderer_null_draw_calls(const sp_renderer* r);
SPIRITTY_API uint64_t     sp_renderer_null_glyph_uploads(const sp_renderer* r);
SPIRITTY_API uint64_t     sp_renderer_null_resizes(const sp_renderer* r);
SPIRITTY_API uint64_t     sp_renderer_null_misordered_draws(const sp_renderer* r);
SPIRITTY_API size_t       sp_renderer_null_last_cell_count(const sp_renderer* r);
SPIRITTY_API const char*  sp_renderer_null_last_custom_shader(const sp_renderer* r);
SPIRITTY_API void         sp_renderer_null_capture_cells(sp_renderer* r, bool enable);
SPIRITTY_API const sp_cell* sp_renderer_null_captured_cells(const sp_renderer* r);

#if defined(SPIRITTY_HAVE_GL)
SPIRITTY_API sp_renderer* sp_renderer_gl_create(void);
SPIRITTY_API void         sp_renderer_gl_destroy(sp_renderer* r);
#endif

#if defined(SPIRITTY_HAVE_WEBGL) || defined(__EMSCRIPTEN__)
SPIRITTY_API sp_renderer* sp_renderer_webgl_create(void);
SPIRITTY_API void         sp_renderer_webgl_destroy(sp_renderer* r);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPIRITTY_RENDERER_H */
