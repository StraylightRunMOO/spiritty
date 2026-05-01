/*
 * terminal.c — Phase 1 stub.
 *
 * The full implementation lands in Phase 2 (port from terminal.cpp +
 * buffer.cpp). For now we provide just enough symbols for the library
 * to link, so the new build system can be exercised end-to-end.
 *
 * NOTE: Functions return safe defaults; behaviour is intentionally
 *       inert. Do NOT promote tests against this until Phase 2.
 */
#define SPIRITTY_BUILDING 1

#include "spiritty/spiritty.h"
#include "spiritty/spiritty_renderer.h"

#include <stdlib.h>
#include <string.h>

#ifndef SPIRITTY_VERSION_STRING
#define SPIRITTY_VERSION_STRING "0.1.0-refactor"
#endif

struct sp_terminal {
    sp_options   opts;
    sp_renderer* renderer;
    sp_event_cb  event_cb;
    void*        event_user;
    int32_t      cursor_row;
    int32_t      cursor_col;
};

void sp_options_default(sp_options* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->cols              = 80;
    out->rows              = 24;
    out->scrollback_lines  = 10000;
    out->font_size         = 14;
    out->font_family       = NULL; /* core picks "monospace" */
    out->color_fg          = 0xFFFFFFFFu;
    out->color_bg          = 0x000000FFu;
    out->color_cursor      = 0xFFFFFFFFu;
    out->color_selection   = 0x80808080u;
    out->palette           = NULL;
    out->palette_len       = 0;
    out->gpu_acceleration  = true;
    out->cursor_blink      = true;
    out->cursor_style      = 0;
    out->custom_shader_path = NULL;
}

sp_terminal* sp_terminal_create(const sp_options* opts) {
    sp_terminal* t = (sp_terminal*)calloc(1, sizeof *t);
    if (!t) return NULL;
    if (opts) {
        t->opts = *opts;
    } else {
        sp_options_default(&t->opts);
    }
    return t;
}

void sp_terminal_destroy(sp_terminal* t) {
    free(t);
}

void sp_terminal_attach_renderer(sp_terminal* t, sp_renderer* r) {
    if (t) t->renderer = r;
}

void sp_terminal_set_event_cb(sp_terminal* t, sp_event_cb cb, void* user) {
    if (!t) return;
    t->event_cb   = cb;
    t->event_user = user;
}

void sp_terminal_write(sp_terminal* SPIRITTY_RESTRICT t,
                       const uint8_t* SPIRITTY_RESTRICT data, size_t len) {
    (void)t; (void)data; (void)len;
    /* Phase 2: feed parser. */
}

void sp_terminal_resize(sp_terminal* t, int32_t cols, int32_t rows) {
    if (!t) return;
    t->opts.cols = cols;
    t->opts.rows = rows;
}

void sp_terminal_clear(sp_terminal* t)  { (void)t; }
void sp_terminal_reset(sp_terminal* t)  { (void)t; }
void sp_terminal_render(sp_terminal* t) { (void)t; }

int32_t sp_terminal_cols(const sp_terminal* t) { return t ? t->opts.cols : 0; }
int32_t sp_terminal_rows(const sp_terminal* t) { return t ? t->opts.rows : 0; }

void sp_terminal_move_cursor(sp_terminal* t, int32_t row, int32_t col) {
    if (!t) return;
    t->cursor_row = row;
    t->cursor_col = col;
}

void sp_terminal_get_cursor(const sp_terminal* t, int32_t* row, int32_t* col) {
    if (row) *row = t ? t->cursor_row : 0;
    if (col) *col = t ? t->cursor_col : 0;
}

const sp_cell* sp_terminal_cell_at(const sp_terminal* t,
                                   int32_t row, int32_t col) {
    (void)t; (void)row; (void)col;
    return NULL;
}

void sp_terminal_selection_begin(sp_terminal* t,
                                 int32_t row, int32_t col,
                                 sp_sel_mode mode) {
    (void)t; (void)row; (void)col; (void)mode;
}
void sp_terminal_selection_extend(sp_terminal* t, int32_t row, int32_t col) {
    (void)t; (void)row; (void)col;
}
void sp_terminal_selection_clear(sp_terminal* t) { (void)t; }

char* sp_terminal_selection_text(const sp_terminal* t) {
    (void)t;
    char* s = (char*)malloc(1);
    if (s) s[0] = '\0';
    return s;
}
void sp_string_free(char* s) { free(s); }

void sp_terminal_send_key(sp_terminal* t, const char* utf8, size_t len) {
    (void)t; (void)utf8; (void)len;
}
void sp_terminal_send_mouse(sp_terminal* t,
                            int32_t row, int32_t col,
                            int32_t button, bool pressed) {
    (void)t; (void)row; (void)col; (void)button; (void)pressed;
}

bool sp_terminal_set_custom_shader(sp_terminal* t, const char* glsl_source) {
    if (!t || !t->renderer || !t->renderer->vt || !t->renderer->vt->set_custom_shader)
        return false;
    return t->renderer->vt->set_custom_shader(t->renderer->self, glsl_source);
}

const char* sp_version(void) { return SPIRITTY_VERSION_STRING; }
