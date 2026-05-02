/*
 * terminal.c — Spiritty terminal core composition.
 *
 * Owns the buffer, parser, and selection manager. Public API surface lives
 * in `include/spiritty/spiritty.h`; internal state lives in `internal.h`.
 */
#define SPIRITTY_BUILDING 1

#include "internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef SPIRITTY_VERSION_STRING
#define SPIRITTY_VERSION_STRING "0.1.0-refactor"
#endif

/* ---- Defaults ---------------------------------------------------------- */

void sp_options_default(sp_options* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->cols              = 80;
    out->rows              = 24;
    out->scrollback_lines  = 10000;
    out->font_size         = 14;
    out->font_family       = NULL;
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

static sp_cell_attrs sp_default_attrs_from_opts(const sp_options* o) {
    sp_cell_attrs a = {
        .fg = o->color_fg,
        .bg = o->color_bg,
        .flags = 0,
        .underline_style = 0,
        ._pad = 0,
    };
    return a;
}

/* ---- Lifecycle --------------------------------------------------------- */

sp_terminal* sp_terminal_create(const sp_options* opts) {
    sp_terminal* t = (sp_terminal*)calloc(1, sizeof *t);
    if (!t) return NULL;
    if (opts) {
        t->opts = *opts;
    } else {
        sp_options_default(&t->opts);
    }
    if (t->opts.cols <= 0)              t->opts.cols = 80;
    if (t->opts.rows <= 0)              t->opts.rows = 24;
    if (t->opts.scrollback_lines < 0)   t->opts.scrollback_lines = 0;

    t->default_attrs = sp_default_attrs_from_opts(&t->opts);
    t->cursor.row = 0;
    t->cursor.col = 0;
    t->cursor.visible = true;
    t->cursor.blink_state = false;
    t->cursor.attrs = t->default_attrs;
    t->saved_cursor = t->cursor;
    t->insert_mode = false;
    t->auto_wrap = true;
    t->origin_mode = false;
    t->cursor_visible = true;

    t->buf = sp_buffer_create(t->opts.rows, t->opts.cols, t->opts.scrollback_lines);
    if (!t->buf) { free(t); return NULL; }
    t->parser = sp_parser_create(t);
    if (!t->parser) { sp_buffer_destroy(t->buf); free(t); return NULL; }
    t->selmgr = sp_selmgr_create(t);
    if (!t->selmgr) {
        sp_parser_destroy(t->parser);
        sp_buffer_destroy(t->buf);
        free(t);
        return NULL;
    }
    return t;
}

void sp_terminal_destroy(sp_terminal* t) {
    if (!t) return;
    sp_selmgr_destroy(t->selmgr);
    sp_parser_destroy(t->parser);
    sp_buffer_destroy(t->buf);
    free(t->render_scratch);
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

/* ---- I/O & state ------------------------------------------------------- */

void sp_terminal_write(sp_terminal* SPIRITTY_RESTRICT t,
                       const uint8_t* SPIRITTY_RESTRICT data, size_t len) {
    if (!t || !data || len == 0) return;
    sp_parser_feed(t->parser, data, len);
}

void sp_terminal_resize(sp_terminal* t, int32_t cols, int32_t rows) {
    if (!t || cols <= 0 || rows <= 0) return;
    sp_buffer_resize(t->buf, rows, cols, t->default_attrs);
    t->opts.cols = cols;
    t->opts.rows = rows;
    if (t->cursor.row >= rows) t->cursor.row = rows - 1;
    if (t->cursor.col >= cols) t->cursor.col = cols - 1;
    sp_event ev = {
        .kind = SP_EV_RESIZE,
        .as.resize.cols = cols,
        .as.resize.rows = rows,
    };
    sp_terminal_emit(t, &ev);
}

void sp_terminal_clear(sp_terminal* t) {
    if (!t) return;
    sp_buffer_clear(t->buf, t->default_attrs);
    t->cursor.row = 0;
    t->cursor.col = 0;
}

void sp_terminal_reset(sp_terminal* t) {
    if (!t) return;
    sp_parser_reset(t->parser);
    sp_selmgr_clear(t->selmgr);
    sp_buffer_clear(t->buf, t->default_attrs);
    t->cursor.row = 0;
    t->cursor.col = 0;
    t->cursor.visible = true;
    t->saved_cursor = t->cursor;
    t->insert_mode = false;
    t->auto_wrap = true;
    t->origin_mode = false;
    t->default_attrs = sp_default_attrs_from_opts(&t->opts);
}

/* Flatten the active screen into t->render_scratch in row-major order.
 * Returns the cell count (cols * rows) on success, 0 on allocation
 * failure. The returned pointer is owned by the terminal and stays
 * valid until the next render call or destroy. */
static size_t sp_terminal_flatten_active(sp_terminal* t) {
    int32_t cols = sp_buffer_cols(t->buf);
    int32_t rows = sp_buffer_rows(t->buf);
    if (cols <= 0 || rows <= 0) return 0;

    size_t needed = (size_t)cols * (size_t)rows;
    if (needed > t->render_scratch_capacity) {
        sp_cell* p = (sp_cell*)realloc(t->render_scratch, needed * sizeof(sp_cell));
        if (!p) return 0;
        t->render_scratch          = p;
        t->render_scratch_capacity = needed;
    }
    for (int32_t r = 0; r < rows; ++r) {
        const sp_line* ln = sp_buffer_line_const(t->buf, r);
        sp_cell* dst = t->render_scratch + (size_t)r * (size_t)cols;
        if (ln && ln->cells && ln->cols >= cols) {
            memcpy(dst, ln->cells, (size_t)cols * sizeof(sp_cell));
        } else if (ln && ln->cells && ln->cols > 0) {
            memcpy(dst, ln->cells, (size_t)ln->cols * sizeof(sp_cell));
            memset(dst + ln->cols, 0, (size_t)(cols - ln->cols) * sizeof(sp_cell));
        } else {
            memset(dst, 0, (size_t)cols * sizeof(sp_cell));
        }
    }
    return needed;
}

void sp_terminal_render(sp_terminal* t) {
    if (!t || !t->renderer || !t->renderer->vt) return;
    const sp_renderer_vtbl* v = t->renderer->vt;

    if (v->begin_frame) v->begin_frame(t->renderer->self);

    if (v->draw_cells) {
        size_t n = sp_terminal_flatten_active(t);
        sp_cell_batch batch = {
            .cells          = t->render_scratch,
            .cell_count     = n,
            .cols           = sp_buffer_cols(t->buf),
            .rows           = sp_buffer_rows(t->buf),
            .cursor_row     = t->cursor.row,
            .cursor_col     = t->cursor.col,
            .cursor_visible = t->cursor_visible && t->cursor.visible,
            .time_seconds   = t->time_seconds,
        };
        v->draw_cells(t->renderer->self, &batch);
    }

    if (v->end_frame) v->end_frame(t->renderer->self);
    sp_buffer_clear_dirty(t->buf);
}

void sp_terminal_set_time(sp_terminal* t, float seconds) {
    if (t) t->time_seconds = seconds;
}

int32_t sp_terminal_cols(const sp_terminal* t) {
    return t ? sp_buffer_cols(t->buf) : 0;
}
int32_t sp_terminal_rows(const sp_terminal* t) {
    return t ? sp_buffer_rows(t->buf) : 0;
}

/* ---- Cursor ------------------------------------------------------------ */

void sp_terminal_move_cursor(sp_terminal* t, int32_t row, int32_t col) {
    if (!t) return;
    int32_t rows = sp_buffer_rows(t->buf);
    int32_t cols = sp_buffer_cols(t->buf);
    t->cursor.row = sp_clamp_i32(row, 0, rows ? rows - 1 : 0);
    t->cursor.col = sp_clamp_i32(col, 0, cols ? cols - 1 : 0);
}

void sp_terminal_get_cursor(const sp_terminal* t, int32_t* row, int32_t* col) {
    if (row) *row = t ? t->cursor.row : 0;
    if (col) *col = t ? t->cursor.col : 0;
}

/* ---- Cell access ------------------------------------------------------- */

const sp_cell* sp_terminal_cell_at(const sp_terminal* t, int32_t row, int32_t col) {
    if (!t) return NULL;
    return sp_buffer_cell_const(t->buf, row, col);
}

/* ---- Selection --------------------------------------------------------- */

void sp_terminal_selection_begin(sp_terminal* t, int32_t row, int32_t col,
                                 sp_sel_mode mode) {
    if (t) sp_selmgr_begin(t->selmgr, row, col, mode);
}
void sp_terminal_selection_extend(sp_terminal* t, int32_t row, int32_t col) {
    if (t) sp_selmgr_extend(t->selmgr, row, col);
}
void sp_terminal_selection_clear(sp_terminal* t) {
    if (t) sp_selmgr_clear(t->selmgr);
}
char* sp_terminal_selection_text(const sp_terminal* t) {
    if (!t) {
        char* s = (char*)malloc(1);
        if (s) s[0] = '\0';
        return s;
    }
    return sp_selmgr_text(t->selmgr);
}
void sp_string_free(char* s) { free(s); }

/* ---- Input forwarding -------------------------------------------------- */

void sp_terminal_send_key(sp_terminal* t, const char* utf8, size_t len) {
    if (!t || !utf8 || len == 0) return;
    /* Forward upstream as data event — the host (telnet/PTY) decides what
     * to do with it. This matches xterm's "the program reads from stdin"
     * model: keystrokes echo only when the program echoes them back. */
    sp_event ev = {
        .kind = SP_EV_DATA,
        .as.data.bytes = (const uint8_t*)utf8,
        .as.data.len   = len,
    };
    sp_terminal_emit(t, &ev);
}

void sp_terminal_send_mouse(sp_terminal* t, int32_t row, int32_t col,
                            int32_t button, bool pressed) {
    (void)t; (void)row; (void)col; (void)button; (void)pressed;
    /* Mouse-reporting (X10/SGR) lives behind DECSET 1000/1006; we'll wire
     * the encoder up when those modes flip on. For now: drop. */
}

bool sp_terminal_set_custom_shader(sp_terminal* t, const char* glsl_source) {
    if (!t || !t->renderer || !t->renderer->vt || !t->renderer->vt->set_custom_shader)
        return false;
    return t->renderer->vt->set_custom_shader(t->renderer->self, glsl_source);
}

const char* sp_version(void) { return SPIRITTY_VERSION_STRING; }
