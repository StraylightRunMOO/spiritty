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

struct sp_terminal {
    sp_options    opts;
    sp_renderer*  renderer;
    sp_event_cb   event_cb;
    void*         event_user;

    sp_buffer*    buf;
    sp_parser*    parser;
    sp_selmgr*    selmgr;

    sp_cursor     cursor;
    sp_cursor     saved_cursor;

    sp_cell_attrs default_attrs;  /* SGR rendition the parser is currently emitting */
    bool          insert_mode;
    bool          auto_wrap;
    bool          origin_mode;
    bool          cursor_visible;

    char          window_title[256];
};

/* Clamp helper used by parser/selmgr/terminal. */
static inline int32_t sp_clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void sp_terminal_emit(struct sp_terminal* t, const sp_event* ev) {
    if (t && t->event_cb) t->event_cb(ev, t->event_user);
}

#ifdef __cplusplus
}
#endif

#endif /* SPIRITTY_INTERNAL_H */
