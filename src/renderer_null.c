/*
 * renderer_null.c — no-GPU reference renderer.
 *
 * Used by tests to validate the vtable contract end-to-end without
 * dragging in a GL context. Records frame counts, last cell batch,
 * uploaded glyph count, and the most recent custom shader source so
 * tests can assert on observable side effects.
 *
 * Behavioral guarantees:
 *   - init/shutdown are idempotent within a single object.
 *   - draw_cells outside of begin_frame/end_frame is a no-op (recorded).
 *   - set_custom_shader copies the source into a heap buffer; passing
 *     NULL clears it.
 *   - upload_glyph deep-copies pixels.
 */

#define SPIRITTY_BUILDING 1

#include "spiritty/spiritty_renderer.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    sp_renderer_opts opts;
    int32_t fb_w, fb_h;

    bool     initialized;
    bool     in_frame;

    uint64_t begin_frame_count;
    uint64_t end_frame_count;
    uint64_t draw_cells_count;
    uint64_t resize_count;
    uint64_t glyph_upload_count;
    uint64_t draw_cells_outside_frame;

    /* Last batch metadata snapshot — cells pointer is NOT retained past
     * the draw_cells call, but counts and cursor state are. */
    size_t   last_cell_count;
    int32_t  last_cols, last_rows;
    int32_t  last_cursor_row, last_cursor_col;
    bool     last_cursor_visible;
    float    last_time_seconds;

    /* Optional deep copy of last batch's cells — gated on
     * sp_renderer_null_capture_cells(true). Off by default to keep the
     * happy path allocation-free. */
    bool     capture_cells;
    sp_cell* captured_cells;
    size_t   captured_capacity;

    char*    last_custom_shader;     /* NULL until set */
    size_t   last_custom_shader_len;
} sp_renderer_null;

static bool null_init(void* self, const sp_renderer_opts* opts) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    if (!opts) return false;
    n->opts = *opts;
    n->fb_w = opts->framebuffer_width;
    n->fb_h = opts->framebuffer_height;
    n->initialized = true;
    return true;
}

static void null_shutdown(void* self) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    n->initialized = false;
    n->in_frame    = false;
    free(n->captured_cells);    n->captured_cells = NULL; n->captured_capacity = 0;
    free(n->last_custom_shader); n->last_custom_shader = NULL; n->last_custom_shader_len = 0;
}

static void null_resize(void* self, int32_t fb_w, int32_t fb_h) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    n->fb_w = fb_w;
    n->fb_h = fb_h;
    ++n->resize_count;
}

static void null_begin_frame(void* self) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    n->in_frame = true;
    ++n->begin_frame_count;
}

static void null_draw_cells(void* self, const sp_cell_batch* batch) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    if (!n->in_frame) {
        ++n->draw_cells_outside_frame;
        return;
    }
    ++n->draw_cells_count;
    if (!batch) return;

    n->last_cell_count     = batch->cell_count;
    n->last_cols           = batch->cols;
    n->last_rows           = batch->rows;
    n->last_cursor_row     = batch->cursor_row;
    n->last_cursor_col     = batch->cursor_col;
    n->last_cursor_visible = batch->cursor_visible;
    n->last_time_seconds   = batch->time_seconds;

    if (n->capture_cells && batch->cells && batch->cell_count > 0) {
        if (batch->cell_count > n->captured_capacity) {
            sp_cell* p = (sp_cell*)realloc(n->captured_cells,
                                           batch->cell_count * sizeof(sp_cell));
            if (!p) return;
            n->captured_cells    = p;
            n->captured_capacity = batch->cell_count;
        }
        memcpy(n->captured_cells, batch->cells, batch->cell_count * sizeof(sp_cell));
    }
}

static void null_end_frame(void* self) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    n->in_frame = false;
    ++n->end_frame_count;
}

static bool null_set_custom_shader(void* self, const char* src) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    free(n->last_custom_shader);
    if (!src) {
        n->last_custom_shader     = NULL;
        n->last_custom_shader_len = 0;
        return true;
    }
    size_t len = strlen(src);
    char*  buf = (char*)malloc(len + 1);
    if (!buf) {
        n->last_custom_shader     = NULL;
        n->last_custom_shader_len = 0;
        return false;
    }
    memcpy(buf, src, len + 1);
    n->last_custom_shader     = buf;
    n->last_custom_shader_len = len;
    return true;
}

static void null_upload_glyph(void* self, const sp_glyph* g) {
    sp_renderer_null* n = (sp_renderer_null*)self;
    if (!g) return;
    ++n->glyph_upload_count;
    /* No texture to populate; we only count uploads. Pixel pointer is
     * not retained — the caller owns its lifetime. */
    (void)g;
}

static const sp_renderer_vtbl SP_NULL_VTBL = {
    .init              = null_init,
    .shutdown          = null_shutdown,
    .resize            = null_resize,
    .begin_frame       = null_begin_frame,
    .draw_cells        = null_draw_cells,
    .end_frame         = null_end_frame,
    .set_custom_shader = null_set_custom_shader,
    .upload_glyph      = null_upload_glyph,
};

/* -------- Public factory + introspection helpers -------- */

SPIRITTY_API sp_renderer* sp_renderer_null_create(void) {
    sp_renderer*      r = (sp_renderer*)calloc(1, sizeof(sp_renderer));
    sp_renderer_null* n = (sp_renderer_null*)calloc(1, sizeof(sp_renderer_null));
    if (!r || !n) { free(r); free(n); return NULL; }
    r->vt   = &SP_NULL_VTBL;
    r->self = n;
    return r;
}

SPIRITTY_API void sp_renderer_null_destroy(sp_renderer* r) {
    if (!r) return;
    if (r->self) {
        null_shutdown(r->self);
        free(r->self);
    }
    free(r);
}

/* Test-only inspection. These deliberately live in the same TU as the
 * implementation so the struct stays opaque to the rest of the core. */

SPIRITTY_API uint64_t sp_renderer_null_begin_frames(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->begin_frame_count;
}
SPIRITTY_API uint64_t sp_renderer_null_end_frames(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->end_frame_count;
}
SPIRITTY_API uint64_t sp_renderer_null_draw_calls(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->draw_cells_count;
}
SPIRITTY_API uint64_t sp_renderer_null_glyph_uploads(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->glyph_upload_count;
}
SPIRITTY_API uint64_t sp_renderer_null_resizes(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->resize_count;
}
SPIRITTY_API uint64_t sp_renderer_null_misordered_draws(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->draw_cells_outside_frame;
}
SPIRITTY_API size_t sp_renderer_null_last_cell_count(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->last_cell_count;
}
SPIRITTY_API const char* sp_renderer_null_last_custom_shader(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->last_custom_shader;
}
SPIRITTY_API void sp_renderer_null_capture_cells(sp_renderer* r, bool enable) {
    ((sp_renderer_null*)r->self)->capture_cells = enable;
}
SPIRITTY_API const sp_cell* sp_renderer_null_captured_cells(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->captured_cells;
}
SPIRITTY_API bool sp_renderer_null_initialized(const sp_renderer* r) {
    return ((const sp_renderer_null*)r->self)->initialized;
}
