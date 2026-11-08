/*
 * internal.h — types shared across core translation units.
 *
 * NOT installed. Public API lives under include/spiritty/.
 */
#ifndef SPIRITTY_INTERNAL_H
#define SPIRITTY_INTERNAL_H

#include "spiritty/spiritty.h"
#include "spiritty/spiritty_renderer.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Cursor / saved cursor state -------------------------------------- */

typedef struct {
    int32_t       row, col;
    bool          visible;
    bool          blink_state;
    sp_cell_attrs attrs;
} sp_cursor;

/* ---- Selection -------------------------------------------------------- */

typedef struct {
    bool        active;
    sp_sel_mode mode;
    int32_t     start_row, start_col;
    int32_t     end_row,   end_col;
} sp_selection;

/* ---- Line ------------------------------------------------------------- */

typedef struct {
    sp_cell* cells;        /* alignas(64) row buffer */
    int32_t  cols;
    bool     dirty;
} sp_line;

/* Per-line ops */
void sp_line_init(sp_line* ln, int32_t cols);
void sp_line_free(sp_line* ln);
void sp_line_resize(sp_line* ln, int32_t cols, sp_cell_attrs default_attrs);
void sp_line_clear(sp_line* ln, sp_cell_attrs default_attrs);
void sp_line_clear_range(sp_line* ln, int32_t start, int32_t end,
                         sp_cell_attrs default_attrs);
/* UTF-8 text of the line (caller frees with sp_string_free). */
char* sp_line_text(const sp_line* ln);
char* sp_line_text_range(const sp_line* ln, int32_t start, int32_t end);
bool  sp_codepoint_is_word(uint32_t cp);

/* ---- Buffer (active screen + scrollback) ------------------------------ */

typedef struct sp_buffer sp_buffer;

sp_buffer* sp_buffer_create(int32_t rows, int32_t cols, int32_t scrollback_max);
void       sp_buffer_destroy(sp_buffer* b);

int32_t    sp_buffer_rows(const sp_buffer* b);
int32_t    sp_buffer_cols(const sp_buffer* b);

sp_line*       sp_buffer_line(sp_buffer* b, int32_t row);
const sp_line* sp_buffer_line_const(const sp_buffer* b, int32_t row);
sp_cell*       sp_buffer_cell(sp_buffer* b, int32_t row, int32_t col);
const sp_cell* sp_buffer_cell_const(const sp_buffer* b, int32_t row, int32_t col);

void sp_buffer_resize(sp_buffer* b, int32_t rows, int32_t cols,
                      sp_cell_attrs default_attrs);
void sp_buffer_clear(sp_buffer* b, sp_cell_attrs default_attrs);
void sp_buffer_clear_line(sp_buffer* b, int32_t row, sp_cell_attrs def);
void sp_buffer_clear_range(sp_buffer* b,
                           int32_t start_row, int32_t start_col,
                           int32_t end_row,   int32_t end_col,
                           sp_cell_attrs def);

/* Scroll the active region up by `count` lines; lines that fall off the
 * top get pushed into scrollback. New lines at the bottom are blank. */
void sp_buffer_scroll_up(sp_buffer* b, int32_t count, sp_cell_attrs def);
void sp_buffer_scroll_down(sp_buffer* b, int32_t count, sp_cell_attrs def);

/* DECSTBM scroll region. Rows are 0-based inclusive. The default region
 * is the whole screen (0, rows-1). scroll_up / scroll_down affect only
 * the region; a region that starts above row 0 also skips the scrollback
 * push (matches xterm — you don't preserve partial-region content). */
int32_t sp_buffer_scroll_top(const sp_buffer* b);
int32_t sp_buffer_scroll_bot(const sp_buffer* b);
void    sp_buffer_set_scroll_region(sp_buffer* b, int32_t top, int32_t bot);

void sp_buffer_insert_lines(sp_buffer* b, int32_t row, int32_t count,
                            sp_cell_attrs def);
void sp_buffer_delete_lines(sp_buffer* b, int32_t row, int32_t count,
                            sp_cell_attrs def);
void sp_buffer_insert_chars(sp_buffer* b, int32_t row, int32_t col,
                            int32_t count, sp_cell_attrs def);
void sp_buffer_delete_chars(sp_buffer* b, int32_t row, int32_t col,
                            int32_t count, sp_cell_attrs def);

/* Write a single codepoint at (row,col); does not advance. */
void sp_buffer_write_cp(sp_buffer* b, int32_t row, int32_t col,
                        uint32_t cp, sp_cell_attrs attrs);

size_t sp_buffer_scrollback_size(const sp_buffer* b);
size_t sp_buffer_scrollback_max (const sp_buffer* b);
const sp_line* sp_buffer_scrollback_at(const sp_buffer* b, size_t idx);

/* Scrollback viewport: `scroll_offset` is lines above the live bottom.
 * 0 = pinned to bottom (normal); scrollback_size = fully scrolled up. The
 * renderer reads scroll_offset via sp_buffer_scroll_offset and flattens
 * historical lines in for any scrolled-up portion of the viewport. */
size_t sp_buffer_scroll_offset(const sp_buffer* b);
size_t sp_buffer_scroll_max   (const sp_buffer* b);
void   sp_buffer_scroll_to    (sp_buffer* b, size_t offset);
void   sp_buffer_scroll_by    (sp_buffer* b, int64_t delta);
void   sp_buffer_scroll_reset (sp_buffer* b);

/* Per-cell URI intern id (0 = no URI). Parallel grid co-resized with the
 * active screen; cleared on buffer_clear / clear_line / clear_range; shifted
 * alongside cells on scroll_up / scroll_down. Scrollback does not carry
 * URIs (deliberate v1 simplification). */
void     sp_buffer_set_uri    (sp_buffer* b, int32_t row, int32_t col, uint16_t id);
uint16_t sp_buffer_get_uri    (const sp_buffer* b, int32_t row, int32_t col);
void     sp_buffer_clear_uris (sp_buffer* b);

/* Mark all visible rows dirty. */
void sp_buffer_mark_all_dirty(sp_buffer* b);
void sp_buffer_clear_dirty   (sp_buffer* b);

/* Concatenated UTF-8 text of the visible screen (caller frees). */
char* sp_buffer_text(const sp_buffer* b);

/* ---- Parser ----------------------------------------------------------- */

typedef struct sp_parser sp_parser;

sp_parser* sp_parser_create(struct sp_terminal* owner);
void       sp_parser_destroy(sp_parser* p);
void       sp_parser_reset  (sp_parser* p);
void       sp_parser_feed   (sp_parser* p, const uint8_t* data, size_t len);

/* ---- Selection manager ------------------------------------------------ */

typedef struct sp_selmgr sp_selmgr;

sp_selmgr* sp_selmgr_create(struct sp_terminal* owner);
void       sp_selmgr_destroy(sp_selmgr* sm);
void       sp_selmgr_begin  (sp_selmgr* sm, int32_t row, int32_t col, sp_sel_mode m);
void       sp_selmgr_extend (sp_selmgr* sm, int32_t row, int32_t col);
void       sp_selmgr_clear  (sp_selmgr* sm);
bool       sp_selmgr_active (const sp_selmgr* sm);
char*      sp_selmgr_text   (const sp_selmgr* sm);
const sp_selection* sp_selmgr_selection(const sp_selmgr* sm);

/* ---- Terminal internals (visible to parser/selmgr) -------------------- */

/* Mouse-reporting mode flags; each flipped by DECSET/DECRST private 1000
 * family. `encode_sgr` wins over `encode_urxvt` wins over the X10 fallback. */
typedef struct {
    bool x10;               /* DECSET 9  — press-only legacy X10 */
    bool button_event;      /* DECSET 1000 — press + release */
    bool button_motion;     /* DECSET 1002 — press/release + motion while held */
    bool any_motion;        /* DECSET 1003 — press/release + any motion */
    bool encode_sgr;        /* DECSET 1006 — CSI < Cb;Cx;Cy M/m */
    bool encode_urxvt;      /* DECSET 1015 — CSI Cb;Cx;Cy M */
    int32_t last_button;    /* last pressed button, for motion fill-in */
    int32_t last_row, last_col;
} sp_mouse_state;

/* Small interned URI table for OSC 8 hyperlinks. Cap keeps a long session
 * from unbounded growth; when full new URIs reuse the oldest slot (ring). */
#define SP_URI_CAP 256
typedef struct {
    char*   uris[SP_URI_CAP];  /* malloc'd NUL-terminated; NULL when empty */
    uint16_t count;             /* total unique uris ever interned (saturates) */
    uint16_t next;              /* next slot index for LRU replacement */
} sp_uri_table;

struct sp_terminal {
    sp_options    opts;
    sp_renderer*  renderer;
    sp_event_cb   event_cb;
    void*         event_user;

    sp_buffer*    buf;
    /* Alt-screen (DECSET 1049 / 1047). Created on first toggle; `on_alt`
     * is true while `buf` points at it. `alt_saved_cursor` stashes the
     * primary cursor across 1048/1049. Both buffers resize together. */
    sp_buffer*    alt_buf;
    bool          on_alt;
    sp_cursor     alt_saved_cursor;
    sp_parser*    parser;
    sp_selmgr*    selmgr;

    sp_cursor     cursor;
    sp_cursor     saved_cursor;

    sp_cell_attrs default_attrs;  /* SGR rendition the parser is currently emitting */
    bool          insert_mode;
    bool          auto_wrap;
    bool          origin_mode;
    bool          cursor_visible;
    bool          bracketed_paste;   /* DECSET 2004 — wrap pastes in ESC[200~/201~ */

    /* Live 256-entry palette (ANSI 0-15, xterm 6×6×6 cube, 24 greys). OSC 4
     * mutates entries; OSC 104 resets. SGR 30-37/40-47/90-97/100-107 and
     * 38;5;N / 48;5;N look up through here. */
    uint32_t      palette[256];

    char          window_title[256];

    /* Mouse reporting mode + last-reported state. */
    sp_mouse_state mouse;

    /* OSC 8 hyperlinks. The parser pushes/pops the current URI id via
     * `current_uri_id`; the print path reads it and tags each cell it
     * writes into sp_buffer_set_uri. */
    sp_uri_table   uri_table;
    uint16_t       current_uri_id;    /* 0 = no active link */

    /* Render scratch — flattened active screen rebuilt each render call. */
    sp_cell*      render_scratch;
    size_t        render_scratch_capacity;

    /* Wall-clock seconds since terminal creation, advanced by the host
     * via sp_terminal_set_time(). Forwarded to the renderer for iTime. */
    float         time_seconds;
};

/* Intern a URI string. Returns an opaque id > 0 on success, or 0 if the
 * string is empty/NULL. Duplicate lookups return the existing id. */
uint16_t    sp_terminal_uri_intern(struct sp_terminal* t, const char* s, size_t n);
const char* sp_terminal_uri_lookup(const struct sp_terminal* t, uint16_t id);

/* Mouse-reporting mode toggles. Flipped by DECSET/DECRST in parser.c; also
 * exposed publicly via sp_terminal_mouse_mode() for host introspection. */
void sp_terminal_set_mouse_mode(struct sp_terminal* t, int mode, bool set);

/* Alt-screen swap. Enter creates alt_buf on demand, saves cursor, swaps
 * `t->buf` to point at the alt, and optionally clears it (DECSET 1049).
 * Leave restores the saved cursor on 1049 and swaps back; no-op if the
 * terminal is not currently on alt. */
void sp_terminal_alt_enter(struct sp_terminal* t, bool save_and_clear);
void sp_terminal_alt_leave(struct sp_terminal* t, bool restore_cursor);

/* Clamp helper used by parser/selmgr/terminal. */
static inline int32_t sp_clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void sp_terminal_emit(struct sp_terminal* t, const sp_event* ev) {
    if (t && t->event_cb) t->event_cb(ev, t->event_user);
}

/* Palette lookup used by the parser's SGR dispatch. idx 0-255. */
uint32_t sp_terminal_palette_entry(const sp_terminal* t, int idx);

#ifdef __cplusplus
}
#endif

#endif /* SPIRITTY_INTERNAL_H */
