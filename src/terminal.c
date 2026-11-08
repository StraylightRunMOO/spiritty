/*
 * terminal.c — Spiritty terminal core composition.
 *
 * Owns the buffer, parser, and selection manager. Public API surface lives
 * in `include/spiritty/spiritty.h`; internal state lives in `internal.h`.
 */
#define SPIRITTY_BUILDING 1

#include "internal.h"

#include <stdio.h>
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

/* xterm/ANSI 256-color palette, same math as parser.c's old static table.
 * Populated once at create, then mutated via OSC 4 / sp_terminal_set_palette_*. */
static uint32_t sp_palette_default_entry(int idx) {
    static const uint32_t base16[16] = {
        0x000000FFu, 0xCD0000FFu, 0x00CD00FFu, 0xCDCD00FFu,
        0x0000EEFFu, 0xCD00CDFFu, 0x00CDCDFFu, 0xE5E5E5FFu,
        0x7F7F7FFFu, 0xFF0000FFu, 0x00FF00FFu, 0xFFFF00FFu,
        0x5C5CFFFFu, 0xFF00FFFFu, 0x00FFFFFFu, 0xFFFFFFFFu,
    };
    if (idx < 0 || idx > 255) return 0xFFFFFFFFu;
    if (idx < 16) return base16[idx];
    if (idx < 232) {
        int i = idx - 16;
        int r = (i / 36) % 6;
        int g = (i / 6)  % 6;
        int b =  i       % 6;
        uint32_t rv = r ? (uint32_t)(55 + 40 * r) : 0u;
        uint32_t gv = g ? (uint32_t)(55 + 40 * g) : 0u;
        uint32_t bv = b ? (uint32_t)(55 + 40 * b) : 0u;
        return (rv << 24) | (gv << 16) | (bv << 8) | 0xFFu;
    }
    uint32_t v = (uint32_t)(8 + 10 * (idx - 232));
    return (v << 24) | (v << 16) | (v << 8) | 0xFFu;
}

static void sp_palette_reset(sp_terminal* t) {
    for (int i = 0; i < 256; i++) t->palette[i] = sp_palette_default_entry(i);
    if (t->opts.palette && t->opts.palette_len > 0) {
        size_t n = t->opts.palette_len > 256 ? 256 : t->opts.palette_len;
        for (size_t i = 0; i < n; i++) t->palette[i] = t->opts.palette[i];
    }
}

uint32_t sp_terminal_palette_entry(const sp_terminal* t, int idx) {
    if (!t || idx < 0 || idx > 255) return 0xFFFFFFFFu;
    return t->palette[idx];
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
    t->bracketed_paste = false;
    t->mouse.last_button = -1;
    sp_palette_reset(t);

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
    if (t->alt_buf) sp_buffer_destroy(t->alt_buf);
    for (int i = 0; i < SP_URI_CAP; i++) free(t->uri_table.uris[i]);
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
    /* xterm convention: any new output snaps the viewport to the bottom.
     * Host can scroll back up with sp_terminal_scroll_* after this returns. */
    if (t->buf) sp_buffer_scroll_reset(t->buf);
    sp_parser_feed(t->parser, data, len);
}

void sp_terminal_resize(sp_terminal* t, int32_t cols, int32_t rows) {
    if (!t || cols <= 0 || rows <= 0) return;
    sp_buffer_resize(t->buf, rows, cols, t->default_attrs);
    /* Keep the standby buffer dimensionally matched so a swap doesn't
     * surprise the renderer with a mismatched screen size. */
    if (t->alt_buf)
        sp_buffer_resize(t->alt_buf, rows, cols, t->default_attrs);
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

void sp_terminal_alt_enter(struct sp_terminal* t, bool save_and_clear) {
    if (!t || t->on_alt) return;
    if (!t->alt_buf) {
        /* 0 = no scrollback on alt; TUIs own the screen and don't want
         * their frames to bleed into history. */
        t->alt_buf = sp_buffer_create(sp_buffer_rows(t->buf),
                                      sp_buffer_cols(t->buf), 0);
        if (!t->alt_buf) return;
    }
    if (save_and_clear) t->alt_saved_cursor = t->cursor;
    sp_buffer* tmp = t->buf;
    t->buf = t->alt_buf;
    t->alt_buf = tmp;
    t->on_alt = true;
    if (save_and_clear) {
        sp_buffer_clear(t->buf, t->default_attrs);
        t->cursor.row = 0;
        t->cursor.col = 0;
    }
}

void sp_terminal_alt_leave(struct sp_terminal* t, bool restore_cursor) {
    if (!t || !t->on_alt) return;
    sp_buffer* tmp = t->buf;
    t->buf = t->alt_buf;
    t->alt_buf = tmp;
    t->on_alt = false;
    if (restore_cursor) t->cursor = t->alt_saved_cursor;
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

/* Flatten the viewport (active screen, optionally offset into scrollback)
 * into t->render_scratch in row-major order. When sp_buffer_scroll_offset
 * is 0, this is the active screen as-is; when it's N > 0, the top rows of
 * the viewport come from the scrollback ring. Returns the cell count
 * (cols * rows) on success, 0 on allocation failure. */
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

    size_t offset = sp_buffer_scroll_offset(t->buf);
    size_t sb_size = sp_buffer_scrollback_size(t->buf);
    if (offset > sb_size) offset = sb_size;

    for (int32_t r = 0; r < rows; ++r) {
        sp_cell* dst = t->render_scratch + (size_t)r * (size_t)cols;
        const sp_line* ln = NULL;
        if ((size_t)r < offset) {
            /* Pull from scrollback: viewport row r shows sb entry
             * (sb_size - offset) + r, which is `offset - r` entries
             * before the live bottom. */
            size_t idx = sb_size - offset + (size_t)r;
            ln = sp_buffer_scrollback_at(t->buf, idx);
        } else {
            /* Pull from active screen, shifted up by offset. */
            int32_t vrow = r - (int32_t)offset;
            ln = sp_buffer_line_const(t->buf, vrow);
        }
        if (ln && ln->cells && ln->cols >= cols) {
            memcpy(dst, ln->cells, (size_t)cols * sizeof(sp_cell));
        } else if (ln && ln->cells && ln->cols > 0) {
            memcpy(dst, ln->cells, (size_t)ln->cols * sizeof(sp_cell));
            memset(dst + ln->cols, 0, (size_t)(cols - ln->cols) * sizeof(sp_cell));
        } else {
            memset(dst, 0, (size_t)cols * sizeof(sp_cell));
        }
    }

    /* Paint selection into the scratch bg so the renderer picks it up
     * through the normal cell_bg path. Overwriting the original bg is
     * standard terminal behaviour; proper alpha blending would need a
     * second bg pass. Selection coordinates are in active-screen rows;
     * translate by the scroll offset so a selection made at the live
     * bottom stays painted while the user scrolls up into history. */
    if (sp_selmgr_active(t->selmgr)) {
        const sp_selection* s = sp_selmgr_selection(t->selmgr);
        if (s) {
            int32_t sr = s->start_row, sc = s->start_col;
            int32_t er = s->end_row,   ec = s->end_col;
            if (sr > er || (sr == er && sc > ec)) {
                int32_t tr = sr, tc = sc; sr = er; sc = ec; er = tr; ec = tc;
            }
            sr += (int32_t)offset;
            er += (int32_t)offset;
            uint32_t sel_rgba = t->opts.color_selection;
            for (int32_t r = sr; r <= er && r < rows; ++r) {
                if (r < 0) continue;
                int32_t col_lo = (r == sr) ? sc : 0;
                int32_t col_hi = (r == er) ? ec : (cols - 1);
                if (col_lo < 0) col_lo = 0;
                if (col_hi >= cols) col_hi = cols - 1;
                sp_cell* row = t->render_scratch + (size_t)r * (size_t)cols;
                for (int32_t c = col_lo; c <= col_hi; ++c) {
                    row[c].attrs.bg = sel_rgba;
                }
            }
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
        /* When scrolled up, the cursor logically belongs at the bottom of
         * the active screen; translate it into viewport coords. If that
         * pushes it off the bottom of the viewport, hide it (xterm does
         * the same — no cursor is drawn while viewing history). */
        size_t offset = sp_buffer_scroll_offset(t->buf);
        int32_t cur_row = t->cursor.row + (int32_t)offset;
        bool cur_visible = t->cursor_visible && t->cursor.visible;
        if (cur_row >= sp_buffer_rows(t->buf)) cur_visible = false;
        sp_cell_batch batch = {
            .cells          = t->render_scratch,
            .cell_count     = n,
            .cols           = sp_buffer_cols(t->buf),
            .rows           = sp_buffer_rows(t->buf),
            .cursor_row     = cur_row,
            .cursor_col     = t->cursor.col,
            .cursor_visible = cur_visible,
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

/* Encode button + modifier mask into the low 7 bits xterm expects.
 *
 *   bits 0-1 : button (0=left,1=middle,2=right,3=release-in-X10)
 *   bit  2   : Shift
 *   bit  3   : Meta/Alt
 *   bit  4   : Control
 *   bit  5   : Motion flag (added when kind == motion)
 *   bit  6   : Wheel flag (buttons 64/65)
 *
 * `button` values the host sends:
 *   0,1,2 = left/middle/right (press/release decided by `pressed`)
 *   3     = motion with no button
 *   64,65 = wheel up/down (press-only)
 * `mods` is bit 0=Shift, bit 1=Alt, bit 2=Ctrl. */
static int sp_mouse_build_cb(int button, int mods, bool motion, bool released_x10) {
    int cb;
    if (button >= 64) {
        cb = button;          /* wheel codes include the 64/65 already */
    } else if (button == 3) {
        cb = 3;               /* motion no-button; X10 treats as release */
    } else {
        cb = released_x10 ? 3 : (button & 0x03);
    }
    if (mods & 1) cb |= 4;
    if (mods & 2) cb |= 8;
    if (mods & 4) cb |= 16;
    if (motion)   cb |= 32;
    return cb;
}

/* X10 fallback (CSI M <Cb+32> <Cx+32> <Cy+32>). Column + row are clamped to
 * the legacy 8-bit range (223), beyond which X10 simply cannot encode. */
static int sp_mouse_encode_x10(int cb, int col1, int row1, char out[8]) {
    if (col1 > 223) col1 = 223;
    if (row1 > 223) row1 = 223;
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = 'M';
    out[3] = (char)(cb + 32);
    out[4] = (char)(col1 + 32);
    out[5] = (char)(row1 + 32);
    return 6;
}

void sp_terminal_send_mouse(sp_terminal* t, int32_t row, int32_t col,
                            int32_t button, bool pressed) {
    if (!t) return;
    sp_mouse_state* m = &t->mouse;

    /* Nothing listening → nothing to send. */
    if (!(m->x10 || m->button_event || m->button_motion || m->any_motion)) return;

    /* Clamp to visible grid; terminals number cells 1-based. */
    int32_t rows = sp_buffer_rows(t->buf);
    int32_t cols = sp_buffer_cols(t->buf);
    if (rows <= 0 || cols <= 0) return;
    if (row < 0) row = 0; else if (row >= rows) row = rows - 1;
    if (col < 0) col = 0; else if (col >= cols) col = cols - 1;

    /* Motion: button == 3 is the "motion over no button" marker; otherwise
     * `pressed` tells us press vs release. */
    bool motion = (button == 3);
    bool is_wheel = (button == 64 || button == 65);

    /* Which mode flags gate what gets sent: */
    if (!motion) {
        if (!(m->button_event || m->button_motion || m->any_motion) && !m->x10) return;
        /* X10 is press-only. */
        if (m->x10 && !m->button_event && !m->button_motion && !m->any_motion && !pressed)
            return;
    } else {
        /* Motion events only if 1002 (while held) or 1003 (always). */
        if (!m->button_motion && !m->any_motion) return;
        if (m->button_motion && !m->any_motion) {
            /* only report motion while a button is held */
            if (m->last_button < 0 || m->last_button == 3) return;
        }
    }

    /* De-dup motion to the same cell so dragging doesn't drown the host. */
    if (motion && row == m->last_row && col == m->last_col) return;

    int cb;
    if (motion) {
        int held = (m->last_button >= 0 && m->last_button != 3) ? m->last_button : 3;
        cb = sp_mouse_build_cb(held, 0, true, false);
    } else {
        cb = sp_mouse_build_cb(button, 0, false, !pressed && !is_wheel);
    }

    char buf[32];
    int n = 0;
    if (m->encode_sgr) {
        /* CSI < Cb ; Cx ; Cy  M (press/motion) or m (release). */
        char final = (!motion && !pressed && !is_wheel) ? 'm' : 'M';
        int raw_b = cb;
        if (!motion && !pressed && !is_wheel) raw_b = cb & ~3; /* strip the X10 release marker */
        n = snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c",
                     raw_b, col + 1, row + 1, final);
    } else if (m->encode_urxvt) {
        /* CSI Cb+32 ; Cx ; Cy M */
        n = snprintf(buf, sizeof buf, "\x1b[%d;%d;%dM",
                     cb + 32, col + 1, row + 1);
    } else {
        n = sp_mouse_encode_x10(cb, col + 1, row + 1, buf);
    }
    if (n > 0 && n < (int)sizeof buf) {
        sp_event ev = {
            .kind = SP_EV_DATA,
            .as.data.bytes = (const uint8_t*)buf,
            .as.data.len   = (size_t)n,
        };
        sp_terminal_emit(t, &ev);
    }

    /* Remember state for motion fill-in. */
    if (!motion) {
        if (pressed && !is_wheel) m->last_button = button;
        else if (!pressed && !is_wheel) m->last_button = -1;
    }
    m->last_row = row;
    m->last_col = col;
}

/* ---- OSC 8 URI intern table ------------------------------------------- */

uint16_t sp_terminal_uri_intern(sp_terminal* t, const char* s, size_t n) {
    if (!t || !s || n == 0) return 0;
    /* Dedup: linear scan — 256 cap keeps it cheap and the common case
     * (same URI tagged on many cells) wins hugely. */
    for (int i = 0; i < SP_URI_CAP; i++) {
        const char* u = t->uri_table.uris[i];
        if (u && strlen(u) == n && memcmp(u, s, n) == 0) {
            return (uint16_t)(i + 1);
        }
    }
    /* Insert at the ring-advance slot, evicting what was there. Simpler
     * than true LRU; a chat session almost never recycles that many URIs. */
    int slot = t->uri_table.next % SP_URI_CAP;
    char* copy = (char*)malloc(n + 1);
    if (!copy) return 0;
    memcpy(copy, s, n);
    copy[n] = '\0';
    free(t->uri_table.uris[slot]);
    t->uri_table.uris[slot] = copy;
    t->uri_table.next = (uint16_t)((t->uri_table.next + 1) % SP_URI_CAP);
    if (t->uri_table.count < SP_URI_CAP) t->uri_table.count++;
    return (uint16_t)(slot + 1);
}

const char* sp_terminal_uri_lookup(const sp_terminal* t, uint16_t id) {
    if (!t || id == 0 || id > SP_URI_CAP) return NULL;
    return t->uri_table.uris[id - 1];
}

/* Public hit-test. Row/col are viewport coordinates; scroll_offset is
 * applied here so a hit on row 0 while scrolled up reports from the
 * scrolled-in historical line (which currently has no URI tags → NULL). */
const char* sp_terminal_hit_test_uri(const sp_terminal* t, int32_t row, int32_t col) {
    if (!t) return NULL;
    size_t off = sp_buffer_scroll_offset(t->buf);
    if ((size_t)row < off) {
        /* Historical row: scrollback doesn't carry URIs in v1. */
        return NULL;
    }
    int32_t active_row = row - (int32_t)off;
    uint16_t id = sp_buffer_get_uri(t->buf, active_row, col);
    return sp_terminal_uri_lookup(t, id);
}

void sp_terminal_set_mouse_mode(sp_terminal* t, int mode, bool set) {
    if (!t) return;
    switch (mode) {
        case 9:    t->mouse.x10          = set; break;
        case 1000: t->mouse.button_event = set; break;
        case 1002: t->mouse.button_motion = set; break;
        case 1003: t->mouse.any_motion   = set; break;
        case 1006: t->mouse.encode_sgr   = set; break;
        case 1015: t->mouse.encode_urxvt = set; break;
        default: break;
    }
    if (!set) {
        /* Any disable wipes remembered state so a later enable starts fresh. */
        t->mouse.last_button = -1;
    }
}

int sp_terminal_mouse_mode(const sp_terminal* t) {
    if (!t) return 0;
    int bits = 0;
    if (t->mouse.x10)           bits |= 1 << 0;
    if (t->mouse.button_event)  bits |= 1 << 1;
    if (t->mouse.button_motion) bits |= 1 << 2;
    if (t->mouse.any_motion)    bits |= 1 << 3;
    if (t->mouse.encode_sgr)    bits |= 1 << 4;
    if (t->mouse.encode_urxvt)  bits |= 1 << 5;
    return bits;
}

/* ---- Paste (bracketed if DECSET 2004 set) ----------------------------- */

void sp_terminal_send_paste(sp_terminal* t, const char* utf8, size_t len) {
    if (!t || !utf8 || len == 0) return;
    /* Strip embedded ESC from the paste body to avoid injected sequences
     * running when bracketed paste isn't active — xterm does this
     * unconditionally. Build the payload into a transient buffer. */
    size_t cap = len + 16; /* room for 200~/201~ wrap */
    char* buf = (char*)malloc(cap);
    if (!buf) return;
    size_t off = 0;
    if (t->bracketed_paste) {
        static const char lead[] = "\x1b[200~";
        memcpy(buf + off, lead, 6); off += 6;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char b = (unsigned char)utf8[i];
        if (b == 0x1B) continue;    /* drop ESC */
        buf[off++] = (char)b;
    }
    if (t->bracketed_paste) {
        static const char trail[] = "\x1b[201~";
        memcpy(buf + off, trail, 6); off += 6;
    }
    sp_event ev = {
        .kind = SP_EV_DATA,
        .as.data.bytes = (const uint8_t*)buf,
        .as.data.len   = off,
    };
    sp_terminal_emit(t, &ev);
    free(buf);
}

/* ---- Scrollback viewport ----------------------------------------------- */

size_t sp_terminal_scroll_offset(const sp_terminal* t) {
    return t ? sp_buffer_scroll_offset(t->buf) : 0;
}
size_t sp_terminal_scroll_max(const sp_terminal* t) {
    return t ? sp_buffer_scroll_max(t->buf) : 0;
}
void sp_terminal_scroll_to(sp_terminal* t, size_t offset) {
    if (t) sp_buffer_scroll_to(t->buf, offset);
}
void sp_terminal_scroll_by(sp_terminal* t, int64_t delta) {
    if (t) sp_buffer_scroll_by(t->buf, delta);
}
void sp_terminal_scroll_reset(sp_terminal* t) {
    if (t) sp_buffer_scroll_reset(t->buf);
}

/* ---- Palette & color ------------------------------------------------- */

void sp_terminal_set_palette_entry(sp_terminal* t, int idx, uint32_t rgba) {
    if (!t || idx < 0 || idx > 255) return;
    t->palette[idx] = rgba;
    sp_buffer_mark_all_dirty(t->buf);
}

uint32_t sp_terminal_get_palette_entry(const sp_terminal* t, int idx) {
    return sp_terminal_palette_entry(t, idx);
}

void sp_terminal_reset_palette_entry(sp_terminal* t, int idx) {
    if (!t || idx < 0 || idx > 255) return;
    t->palette[idx] = sp_palette_default_entry(idx);
    sp_buffer_mark_all_dirty(t->buf);
}

void sp_terminal_set_color(sp_terminal* t, int role, uint32_t rgba) {
    if (!t) return;
    switch (role) {
        case SP_COLOR_FG:        t->opts.color_fg       = rgba; break;
        case SP_COLOR_BG:        t->opts.color_bg       = rgba; break;
        case SP_COLOR_CURSOR:    t->opts.color_cursor   = rgba; break;
        case SP_COLOR_SELECTION: t->opts.color_selection = rgba; break;
        default: return;
    }
    /* Also refresh the default rendition the parser is emitting so new
     * writes pick up the new fg/bg immediately. */
    t->default_attrs = sp_default_attrs_from_opts(&t->opts);
    sp_buffer_mark_all_dirty(t->buf);
}

uint32_t sp_terminal_get_color(const sp_terminal* t, int role) {
    if (!t) return 0;
    switch (role) {
        case SP_COLOR_FG:        return t->opts.color_fg;
        case SP_COLOR_BG:        return t->opts.color_bg;
        case SP_COLOR_CURSOR:    return t->opts.color_cursor;
        case SP_COLOR_SELECTION: return t->opts.color_selection;
    }
    return 0;
}

void sp_terminal_reset_color(sp_terminal* t, int role) {
    /* Without a remembered original we just revert to the hard defaults —
     * the same values sp_options_default sets. Matches OSC 110/111/112
     * semantics closely enough for v1. */
    uint32_t v = 0;
    switch (role) {
        case SP_COLOR_FG:        v = 0xFFFFFFFFu; break;
        case SP_COLOR_BG:        v = 0x000000FFu; break;
        case SP_COLOR_CURSOR:    v = 0xFFFFFFFFu; break;
        case SP_COLOR_SELECTION: v = 0x80808080u; break;
        default: return;
    }
    sp_terminal_set_color(t, role, v);
}

bool sp_terminal_set_custom_shader(sp_terminal* t, const char* glsl_source) {
    if (!t || !t->renderer || !t->renderer->vt || !t->renderer->vt->set_custom_shader)
        return false;
    return t->renderer->vt->set_custom_shader(t->renderer->self, glsl_source);
}

const char* sp_version(void) { return SPIRITTY_VERSION_STRING; }
