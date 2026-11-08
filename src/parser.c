/*
 * parser.c — VT100/ANSI escape sequence FSM (C11 port).
 *
 * Ported from the legacy `src/ansi_parser.cpp`. The state-machine topology
 * follows Paul Williams' VT500 dispatch model; only the transitions and
 * dispatch *we actually use* are wired up here. Bytes that don't match a
 * known dispatch return to GROUND and are dropped silently — that is the
 * correct behaviour per the spec.
 *
 * Hot path: `process_byte()` is called once per input byte. The branches
 * are arranged so that the GROUND/PRINT path (the common case for plain
 * text) is the shortest.
 */
#define SPIRITTY_BUILDING 1

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Constants ---------------------------------------------------------- */

#define SP_MAX_PARAMS       16
#define SP_OSC_BUF_MAX      4096
#define SP_INTERMEDIATE_MAX 4

/* The live 256-color palette lives on sp_terminal now so it can be
 * mutated by OSC 4. Fetch via sp_terminal_palette_entry (internal.h). */

/* ---- States ------------------------------------------------------------- */

typedef enum {
    SP_PS_GROUND = 0,
    SP_PS_ESCAPE,
    SP_PS_ESCAPE_INTERMEDIATE,
    SP_PS_CSI_ENTRY,
    SP_PS_CSI_PARAM,
    SP_PS_CSI_INTERMEDIATE,
    SP_PS_CSI_IGNORE,
    SP_PS_OSC_STRING,
    SP_PS_DCS_ENTRY,
    SP_PS_DCS_PARAM,
    SP_PS_DCS_INTERMEDIATE,
    SP_PS_DCS_IGNORE,
    SP_PS_DCS_PASSTHROUGH,
    SP_PS_SOS_PM_APC
} sp_pstate;

/* ---- Parser handle ----------------------------------------------------- */

struct sp_parser {
    struct sp_terminal* owner;

    sp_pstate state;
    int32_t   params[SP_MAX_PARAMS];
    int32_t   nparams;
    bool      param_started;        /* did we see at least one digit since last CLEAR? */
    bool      private_csi;          /* '?' prefix */
    char      intermediates[SP_INTERMEDIATE_MAX + 1];
    int32_t   nintermediates;
    char      osc_buf[SP_OSC_BUF_MAX];
    int32_t   osc_len;
    bool      osc_saw_esc;

    /* UTF-8 reassembly */
    uint32_t  utf8_cp;
    int32_t   utf8_remaining;

    /* SGR state mirrored from legacy GraphicsRendition */
    sp_cell_attrs gr;
};

/* ---- Helpers ----------------------------------------------------------- */

static void sp_parser_clear_pi(sp_parser* p) {
    p->nparams = 0;
    p->param_started = false;
    p->private_csi = false;
    p->nintermediates = 0;
    p->intermediates[0] = '\0';
}

static int32_t sp_parser_param(const sp_parser* p, int32_t idx, int32_t dflt) {
    if (idx >= p->nparams) return dflt;
    int32_t v = p->params[idx];
    return v <= 0 ? dflt : v;
}

static void sp_parser_add_param_digit(sp_parser* p, char c) {
    if (!p->param_started) {
        if (p->nparams >= SP_MAX_PARAMS) return;
        p->params[p->nparams++] = 0;
        p->param_started = true;
    }
    int32_t* slot = &p->params[p->nparams - 1];
    if (*slot < 100000) *slot = (*slot * 10) + (c - '0');
}

static void sp_parser_param_sep(sp_parser* p) {
    /* If we never saw a digit between separators (e.g. ";;"), record an
     * implicit zero. Otherwise just close the current param so the next
     * digit opens a fresh slot via sp_parser_add_param_digit. */
    if (!p->param_started && p->nparams < SP_MAX_PARAMS) {
        p->params[p->nparams++] = 0;
    }
    p->param_started = false;
}

static void sp_parser_add_intermediate(sp_parser* p, char c) {
    if (p->nintermediates < SP_INTERMEDIATE_MAX) {
        p->intermediates[p->nintermediates++] = c;
        p->intermediates[p->nintermediates] = '\0';
    }
}

/* ---- Cursor / scroll plumbing ------------------------------------------ */

static int32_t sp_clamp_row(struct sp_terminal* t, int32_t row) {
    return sp_clamp_i32(row, 0, sp_buffer_rows(t->buf) - 1);
}
static int32_t sp_clamp_col(struct sp_terminal* t, int32_t col) {
    return sp_clamp_i32(col, 0, sp_buffer_cols(t->buf) - 1);
}

static void sp_cursor_set(struct sp_terminal* t, int32_t row, int32_t col) {
    t->cursor.row = sp_clamp_row(t, row);
    t->cursor.col = sp_clamp_col(t, col);
}

static void sp_cursor_advance(struct sp_terminal* t) {
    int32_t cols = sp_buffer_cols(t->buf);
    int32_t rows = sp_buffer_rows(t->buf);
    int32_t bot  = sp_buffer_scroll_bot(t->buf);
    if (t->cursor.col + 1 < cols) {
        t->cursor.col++;
    } else if (t->auto_wrap) {
        t->cursor.col = 0;
        if (t->cursor.row == bot) {
            sp_buffer_scroll_up(t->buf, 1, t->default_attrs);
            /* cursor stays at the region's bottom */
        } else if (t->cursor.row + 1 < rows) {
            t->cursor.row++;
        }
    }
    /* else: pinned at right margin */
}

static void sp_newline(struct sp_terminal* t) {
    int32_t rows = sp_buffer_rows(t->buf);
    int32_t bot  = sp_buffer_scroll_bot(t->buf);
    if (t->cursor.row == bot) {
        sp_buffer_scroll_up(t->buf, 1, t->default_attrs);
    } else if (t->cursor.row + 1 < rows) {
        t->cursor.row++;
    }
    /* else: cursor outside region (below bot but on last screen row) —
     * don't scroll, don't move. Matches xterm. */
}

/* ---- Print handler ----------------------------------------------------- */

static void sp_handle_print_cp(sp_parser* p, uint32_t cp) {
    struct sp_terminal* t = p->owner;
    if (!t || !t->buf) return;
    sp_buffer_write_cp(t->buf, t->cursor.row, t->cursor.col, cp, p->gr);
    /* Tag the cell we just wrote with the current OSC 8 URI (if any).
     * Written before the cursor advance so the coordinates are right. */
    if (t->current_uri_id != 0) {
        sp_buffer_set_uri(t->buf, t->cursor.row, t->cursor.col, t->current_uri_id);
    }
    sp_cursor_advance(t);
}

static void sp_handle_print(sp_parser* p, uint8_t byte) {
    /* UTF-8 reassembly. */
    if (p->utf8_remaining > 0) {
        if ((byte & 0xC0) != 0x80) {
            /* Malformed continuation; restart. */
            p->utf8_remaining = 0;
            p->utf8_cp = 0;
        } else {
            p->utf8_cp = (p->utf8_cp << 6) | (uint32_t)(byte & 0x3F);
            if (--p->utf8_remaining == 0) {
                sp_handle_print_cp(p, p->utf8_cp);
                p->utf8_cp = 0;
            }
            return;
        }
    }
    if (byte < 0x80) {
        sp_handle_print_cp(p, (uint32_t)byte);
    } else if ((byte & 0xE0) == 0xC0) {
        p->utf8_cp = (uint32_t)(byte & 0x1F);
        p->utf8_remaining = 1;
    } else if ((byte & 0xF0) == 0xE0) {
        p->utf8_cp = (uint32_t)(byte & 0x0F);
        p->utf8_remaining = 2;
    } else if ((byte & 0xF8) == 0xF0) {
        p->utf8_cp = (uint32_t)(byte & 0x07);
        p->utf8_remaining = 3;
    } else {
        /* invalid lead — drop */
        p->utf8_cp = 0;
        p->utf8_remaining = 0;
    }
}

/* ---- C0 handlers ------------------------------------------------------- */

static void sp_handle_c0(sp_parser* p, uint8_t b) {
    struct sp_terminal* t = p->owner;
    switch (b) {
        case 0x07: /* BEL */ {
            sp_event ev = { .kind = SP_EV_BELL };
            sp_terminal_emit(t, &ev);
            break;
        }
        case 0x08: /* BS */
            if (t->cursor.col > 0) t->cursor.col--;
            break;
        case 0x09: /* HT */ {
            int32_t cols = sp_buffer_cols(t->buf);
            int32_t next = (t->cursor.col / 8 + 1) * 8;
            t->cursor.col = next < cols ? next : cols - 1;
            break;
        }
        case 0x0A: /* LF */
        case 0x0B: /* VT */
        case 0x0C: /* FF */
            sp_newline(t);
            break;
        case 0x0D: /* CR */
            t->cursor.col = 0;
            break;
        default:
            /* Drop silently — NUL, SO/SI charset shifts, etc. */
            break;
    }
}

/* ---- SGR --------------------------------------------------------------- */

static void sp_apply_sgr(sp_parser* p) {
    if (p->nparams == 0) {
        /* Bare CSI m == reset */
        p->gr = (sp_cell_attrs){
            .fg = p->owner->opts.color_fg,
            .bg = p->owner->opts.color_bg,
            .flags = 0,
            .underline_style = 0,
            ._pad = 0,
        };
        return;
    }
    for (int32_t i = 0; i < p->nparams; ++i) {
        int32_t pv = p->params[i];
        if (pv == 0) {
            p->gr = (sp_cell_attrs){
                .fg = p->owner->opts.color_fg,
                .bg = p->owner->opts.color_bg,
                .flags = 0,
                .underline_style = 0,
                ._pad = 0,
            };
        } else if (pv == 1) {
            p->gr.flags |= SP_ATTR_BOLD;
        } else if (pv == 2) {
            p->gr.flags |= SP_ATTR_DIM;
        } else if (pv == 3) {
            p->gr.flags |= SP_ATTR_ITALIC;
        } else if (pv == 4) {
            p->gr.flags |= SP_ATTR_UNDERLINE;
            p->gr.underline_style = 1;
        } else if (pv == 5) {
            p->gr.flags |= SP_ATTR_BLINK;
        } else if (pv == 7) {
            p->gr.flags |= SP_ATTR_REVERSE;
        } else if (pv == 8) {
            p->gr.flags |= SP_ATTR_HIDDEN;
        } else if (pv == 9) {
            p->gr.flags |= SP_ATTR_STRIKETHROUGH;
        } else if (pv == 21) {
            p->gr.underline_style = 2; /* double underline */
            p->gr.flags |= SP_ATTR_UNDERLINE;
        } else if (pv == 22) {
            p->gr.flags &= (uint16_t)~(SP_ATTR_BOLD | SP_ATTR_DIM);
        } else if (pv == 23) {
            p->gr.flags &= (uint16_t)~SP_ATTR_ITALIC;
        } else if (pv == 24) {
            p->gr.flags &= (uint16_t)~SP_ATTR_UNDERLINE;
            p->gr.underline_style = 0;
        } else if (pv == 25) {
            p->gr.flags &= (uint16_t)~SP_ATTR_BLINK;
        } else if (pv == 27) {
            p->gr.flags &= (uint16_t)~SP_ATTR_REVERSE;
        } else if (pv == 28) {
            p->gr.flags &= (uint16_t)~SP_ATTR_HIDDEN;
        } else if (pv == 29) {
            p->gr.flags &= (uint16_t)~SP_ATTR_STRIKETHROUGH;
        } else if (pv >= 30 && pv <= 37) {
            p->gr.fg = sp_terminal_palette_entry(p->owner, pv - 30);
        } else if (pv == 38) {
            /* 38;5;N or 38;2;R;G;B */
            if (i + 2 < p->nparams && p->params[i + 1] == 5) {
                p->gr.fg = sp_terminal_palette_entry(p->owner, p->params[i + 2]);
                i += 2;
            } else if (i + 4 < p->nparams && p->params[i + 1] == 2) {
                uint32_t r = (uint32_t)(p->params[i + 2] & 0xFF);
                uint32_t g = (uint32_t)(p->params[i + 3] & 0xFF);
                uint32_t b = (uint32_t)(p->params[i + 4] & 0xFF);
                p->gr.fg = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
                i += 4;
            }
        } else if (pv == 39) {
            p->gr.fg = p->owner->opts.color_fg;
        } else if (pv >= 40 && pv <= 47) {
            p->gr.bg = sp_terminal_palette_entry(p->owner, pv - 40);
        } else if (pv == 48) {
            if (i + 2 < p->nparams && p->params[i + 1] == 5) {
                p->gr.bg = sp_terminal_palette_entry(p->owner, p->params[i + 2]);
                i += 2;
            } else if (i + 4 < p->nparams && p->params[i + 1] == 2) {
                uint32_t r = (uint32_t)(p->params[i + 2] & 0xFF);
                uint32_t g = (uint32_t)(p->params[i + 3] & 0xFF);
                uint32_t b = (uint32_t)(p->params[i + 4] & 0xFF);
                p->gr.bg = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
                i += 4;
            }
        } else if (pv == 49) {
            p->gr.bg = p->owner->opts.color_bg;
        } else if (pv >= 90 && pv <= 97) {
            p->gr.fg = sp_terminal_palette_entry(p->owner, pv - 90 + 8);
        } else if (pv >= 100 && pv <= 107) {
            p->gr.bg = sp_terminal_palette_entry(p->owner, pv - 100 + 8);
        }
        /* unknown SGR params: ignore */
    }
}

/* ---- CSI dispatch ------------------------------------------------------ */

static void sp_csi_dispatch(sp_parser* p, uint8_t final_byte) {
    struct sp_terminal* t = p->owner;
    if (!t || !t->buf) return;

    int32_t rows = sp_buffer_rows(t->buf);
    int32_t cols = sp_buffer_cols(t->buf);

    switch (final_byte) {
        case 'A': { /* CUU */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, t->cursor.row - n, t->cursor.col);
            break;
        }
        case 'B': { /* CUD */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, t->cursor.row + n, t->cursor.col);
            break;
        }
        case 'C': { /* CUF */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, t->cursor.row, t->cursor.col + n);
            break;
        }
        case 'D': { /* CUB */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, t->cursor.row, t->cursor.col - n);
            break;
        }
        case 'E': { /* CNL */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, t->cursor.row + n, 0);
            break;
        }
        case 'F': { /* CPL */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, t->cursor.row - n, 0);
            break;
        }
        case 'G': { /* CHA */
            int32_t c = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, t->cursor.row, c - 1);
            break;
        }
        case 'H': /* CUP */
        case 'f': /* HVP */ {
            int32_t r = sp_parser_param(p, 0, 1);
            int32_t c = sp_parser_param(p, 1, 1);
            sp_cursor_set(t, r - 1, c - 1);
            break;
        }
        case 'J': { /* ED */
            int32_t mode = (p->nparams == 0) ? 0 : p->params[0];
            if (mode == 0) {
                sp_buffer_clear_range(t->buf, t->cursor.row, t->cursor.col,
                                      rows - 1, cols - 1, t->default_attrs);
            } else if (mode == 1) {
                sp_buffer_clear_range(t->buf, 0, 0,
                                      t->cursor.row, t->cursor.col,
                                      t->default_attrs);
            } else if (mode == 2 || mode == 3) {
                sp_buffer_clear(t->buf, t->default_attrs);
                if (mode == 2) sp_cursor_set(t, 0, 0);
            }
            break;
        }
        case 'K': { /* EL */
            int32_t mode = (p->nparams == 0) ? 0 : p->params[0];
            sp_line* ln = sp_buffer_line(t->buf, t->cursor.row);
            if (!ln) break;
            /* sp_line_clear_range is half-open [start, end). */
            if (mode == 0) {
                sp_line_clear_range(ln, t->cursor.col, cols, t->default_attrs);
            } else if (mode == 1) {
                sp_line_clear_range(ln, 0, t->cursor.col + 1, t->default_attrs);
            } else if (mode == 2) {
                sp_line_clear(ln, t->default_attrs);
            }
            break;
        }
        case 'L': { /* IL */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_buffer_insert_lines(t->buf, t->cursor.row, n, t->default_attrs);
            break;
        }
        case 'M': { /* DL */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_buffer_delete_lines(t->buf, t->cursor.row, n, t->default_attrs);
            break;
        }
        case 'P': { /* DCH */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_buffer_delete_chars(t->buf, t->cursor.row, t->cursor.col, n,
                                   t->default_attrs);
            break;
        }
        case 'X': { /* ECH — erase n chars at cursor without moving */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_line* ln = sp_buffer_line(t->buf, t->cursor.row);
            if (ln) {
                int32_t end = t->cursor.col + n;          /* half-open */
                if (end > cols) end = cols;
                sp_line_clear_range(ln, t->cursor.col, end, t->default_attrs);
            }
            break;
        }
        case '@': { /* ICH */
            int32_t n = sp_parser_param(p, 0, 1);
            sp_buffer_insert_chars(t->buf, t->cursor.row, t->cursor.col, n,
                                   t->default_attrs);
            break;
        }
        case 'd': { /* VPA */
            int32_t r = sp_parser_param(p, 0, 1);
            sp_cursor_set(t, r - 1, t->cursor.col);
            break;
        }
        case 'h': /* SM / DECSET */
        case 'l': /* RM / DECRST */ {
            bool set = (final_byte == 'h');
            for (int32_t i = 0; i < p->nparams; ++i) {
                int32_t v = p->params[i];
                if (p->private_csi) {
                    switch (v) {
                        case 7:    t->auto_wrap     = set; break;
                        case 25:   t->cursor_visible = set;
                                   t->cursor.visible = set; break;
                        case 6:    t->origin_mode   = set; break;
                        case 2004: t->bracketed_paste = set; break;
                        case 9:
                        case 1000:
                        case 1002:
                        case 1003:
                        case 1006:
                        case 1015:
                            sp_terminal_set_mouse_mode(t, v, set);
                            break;
                        case 1047:
                            /* Switch screen buffers; cursor state untouched.
                             * On entering alt, clear it (xterm). */
                            if (set && !t->on_alt)      sp_terminal_alt_enter(t, false);
                            else if (!set && t->on_alt) sp_terminal_alt_leave(t, false);
                            break;
                        case 1048:
                            /* Save/restore cursor (DECSC/DECRC equivalent). */
                            if (set) t->alt_saved_cursor = t->cursor;
                            else     t->cursor = t->alt_saved_cursor;
                            break;
                        case 1049:
                            /* DECSET 1049 = 1048 (save cursor) + 1047 with
                             * clear-on-enter. DECRST 1049 = swap back +
                             * restore cursor. What most TUIs actually use. */
                            if (set && !t->on_alt)      sp_terminal_alt_enter(t, true);
                            else if (!set && t->on_alt) sp_terminal_alt_leave(t, true);
                            break;
                        default:   break;
                    }
                } else {
                    switch (v) {
                        case 4:    t->insert_mode = set; break;
                        default:   break;
                    }
                }
            }
            break;
        }
        case 'm': /* SGR */
            sp_apply_sgr(p);
            t->default_attrs = p->gr;
            break;
        case 'r': { /* DECSTBM — set top & bottom scrolling margins */
            int32_t nrows = sp_buffer_rows(t->buf);
            int32_t top   = sp_parser_param(p, 0, 1);
            int32_t bot   = sp_parser_param(p, 1, nrows);
            if (top < 1)     top = 1;
            if (bot > nrows) bot = nrows;
            /* Invalid pair → full-screen region (already what set_region
             * does for top >= bot). DECSTBM also homes the cursor. */
            sp_buffer_set_scroll_region(t->buf, top - 1, bot - 1);
            t->cursor.row = 0;
            t->cursor.col = 0;
            break;
        }
        case 's': /* DECSLRM / save cursor (ignored for left/right margin) */
            break;
        case 'u': /* restore cursor (ANSI.SYS) */
            t->cursor = t->saved_cursor;
            break;
        case 'n': { /* DSR */
            if (p->nparams > 0 && p->params[0] == 6) {
                /* CPR — Cursor Position Report */
                char rpt[32];
                int len = snprintf(rpt, sizeof rpt, "\x1b[%d;%dR",
                                   t->cursor.row + 1, t->cursor.col + 1);
                if (len > 0) {
                    sp_event ev = {
                        .kind = SP_EV_DATA,
                        .as.data.bytes = (const uint8_t*)rpt,
                        .as.data.len   = (size_t)len,
                    };
                    sp_terminal_emit(t, &ev);
                }
            }
            break;
        }
        default:
            /* Unhandled CSI final byte — drop. */
            break;
    }
}

/* ---- ESC dispatch ------------------------------------------------------ */

static void sp_esc_dispatch(sp_parser* p, uint8_t b) {
    struct sp_terminal* t = p->owner;
    switch (b) {
        case '7': /* DECSC — save cursor */
            t->saved_cursor = t->cursor;
            break;
        case '8': /* DECRC — restore cursor */
            t->cursor = t->saved_cursor;
            break;
        case 'D': /* IND — index */
            sp_newline(t);
            break;
        case 'E': /* NEL — next line */
            t->cursor.col = 0;
            sp_newline(t);
            break;
        case 'M': /* RI — reverse index */
            if (t->cursor.row == sp_buffer_scroll_top(t->buf)) {
                sp_buffer_scroll_down(t->buf, 1, t->default_attrs);
            } else if (t->cursor.row > 0) {
                t->cursor.row--;
            }
            break;
        case 'c': /* RIS — full reset */
            sp_buffer_clear(t->buf, t->default_attrs);
            t->cursor.row = 0;
            t->cursor.col = 0;
            t->cursor.visible = true;
            t->saved_cursor = t->cursor;
            t->insert_mode = false;
            t->auto_wrap = true;
            t->origin_mode = false;
            break;
        default:
            break;
    }
}

/* ---- OSC dispatch ------------------------------------------------------ */

/* Parse a single hex nibble, -1 on failure. */
static int sp_hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Parse "#RRGGBB", "#RGB", or xterm "rgb:RR/GG/BB" / "rgb:RRRR/GGGG/BBBB"
 * into a 0xRRGGBBAA uint32, alpha always 0xFF. Returns false on any error.
 * The input buffer need not be NUL-terminated past `len`. */
static bool sp_parse_color(const char* s, size_t len, uint32_t* out) {
    if (!s || len == 0) return false;
    if (s[0] == '#') {
        if (len == 7) {   /* #RRGGBB */
            int r1=sp_hex_nib(s[1]),r2=sp_hex_nib(s[2]);
            int g1=sp_hex_nib(s[3]),g2=sp_hex_nib(s[4]);
            int b1=sp_hex_nib(s[5]),b2=sp_hex_nib(s[6]);
            if ((r1|r2|g1|g2|b1|b2) < 0) return false;
            uint32_t r = (uint32_t)((r1<<4)|r2);
            uint32_t g = (uint32_t)((g1<<4)|g2);
            uint32_t b = (uint32_t)((b1<<4)|b2);
            *out = (r<<24)|(g<<16)|(b<<8)|0xFFu;
            return true;
        }
        if (len == 4) {   /* #RGB */
            int r=sp_hex_nib(s[1]),g=sp_hex_nib(s[2]),b=sp_hex_nib(s[3]);
            if ((r|g|b) < 0) return false;
            uint32_t rr=(uint32_t)((r<<4)|r);
            uint32_t gg=(uint32_t)((g<<4)|g);
            uint32_t bb=(uint32_t)((b<<4)|b);
            *out = (rr<<24)|(gg<<16)|(bb<<8)|0xFFu;
            return true;
        }
        return false;
    }
    /* rgb:RR/GG/BB or rgb:RRRR/GGGG/BBBB */
    if (len >= 4 && (s[0]=='r'||s[0]=='R') && (s[1]=='g'||s[1]=='G') &&
        (s[2]=='b'||s[2]=='B') && s[3] == ':') {
        const char* p = s + 4;
        size_t remain = len - 4;
        uint32_t ch[3] = {0,0,0};
        for (int k = 0; k < 3; ++k) {
            size_t j = 0;
            uint32_t v = 0;
            while (j < remain && p[j] != '/' && j < 4) {
                int n = sp_hex_nib(p[j]);
                if (n < 0) return false;
                v = (v << 4) | (uint32_t)n;
                j++;
            }
            if (j == 0) return false;
            /* Scale down to 8-bit depending on width (xterm style). */
            if (j == 4) v >>= 8;
            else if (j == 3) v >>= 4;
            else if (j == 1) v = v * 0x11;
            /* j == 2: already 8-bit, no scale. */
            ch[k] = v & 0xFFu;
            if (k < 2) {
                if (j >= remain || p[j] != '/') return false;
                p += j + 1;
                remain -= j + 1;
            } else {
                p += j;
                remain -= j;
            }
        }
        *out = (ch[0]<<24)|(ch[1]<<16)|(ch[2]<<8)|0xFFu;
        return true;
    }
    return false;
}

static void sp_osc_set_color_pair(struct sp_terminal* t, int role,
                                  const char* s, size_t len) {
    /* xterm "?" queries return the current color; we don't echo — no-op. */
    if (len > 0 && s[0] == '?') return;
    uint32_t v = 0;
    if (sp_parse_color(s, len, &v)) sp_terminal_set_color(t, role, v);
}

static void sp_osc_dispatch(sp_parser* p) {
    struct sp_terminal* t = p->owner;
    /* Format: <Ps>;<Pt> */
    if (p->osc_len == 0) return;
    /* Trim trailing ESC if present (we got ST as ESC \). */
    int32_t end = p->osc_len;
    if (end > 0 && (uint8_t)p->osc_buf[end - 1] == 0x1B) end--;
    p->osc_buf[end] = '\0';

    /* Parse Ps */
    int32_t ps = 0;
    int32_t i = 0;
    while (i < end && p->osc_buf[i] >= '0' && p->osc_buf[i] <= '9') {
        ps = ps * 10 + (p->osc_buf[i] - '0');
        i++;
    }
    if (i < end && p->osc_buf[i] == ';') i++;

    const char* body = p->osc_buf + i;
    size_t body_len = (size_t)(end - i);

    switch (ps) {
    case 0: case 1: case 2: {
        size_t n = body_len;
        if (n >= sizeof(t->window_title)) n = sizeof(t->window_title) - 1;
        memcpy(t->window_title, body, n);
        t->window_title[n] = '\0';
        sp_event ev = {
            .kind = SP_EV_TITLE,
            .as.title.text = t->window_title,
        };
        sp_terminal_emit(t, &ev);
        break;
    }
    case 4: {
        /* OSC 4;<idx>;<spec>[;<idx>;<spec>...]. Walk the pairs. */
        size_t pos = 0;
        while (pos < body_len) {
            int idx = 0;
            size_t ns = pos;
            while (pos < body_len && body[pos] >= '0' && body[pos] <= '9') {
                idx = idx * 10 + (body[pos] - '0');
                pos++;
            }
            if (ns == pos || pos >= body_len || body[pos] != ';') break;
            pos++;
            size_t vs = pos;
            while (pos < body_len && body[pos] != ';') pos++;
            size_t vlen = pos - vs;
            /* "?" queries return the current color; we don't echo back. */
            if (vlen > 0 && body[vs] != '?' && idx >= 0 && idx <= 255) {
                uint32_t v = 0;
                if (sp_parse_color(body + vs, vlen, &v))
                    sp_terminal_set_palette_entry(t, idx, v);
            }
            if (pos < body_len && body[pos] == ';') pos++;
        }
        break;
    }
    case 8: {
        /* OSC 8 ; params ; URI  — begin hyperlink (empty URI closes it).
         * `params` is semicolon-less (comma-separated key=value pairs); we
         * don't surface those to the host yet, just skip past. */
        size_t pos = 0;
        /* Skip params up to the next ';'. */
        while (pos < body_len && body[pos] != ';') pos++;
        if (pos < body_len) pos++;  /* step over the ';' */
        const char* uri = body + pos;
        size_t ulen = body_len - pos;
        /* Trim trailing whitespace (xterm tolerates \r / spaces). */
        while (ulen > 0 && (uri[ulen - 1] == ' ' || uri[ulen - 1] == '\r' ||
                            uri[ulen - 1] == '\n' || uri[ulen - 1] == '\t'))
            ulen--;
        /* Reject control chars anywhere in the URI — defends against
         * injected ESC inside the hyperlink payload (xterm does the same). */
        bool safe = true;
        for (size_t k = 0; k < ulen; k++) {
            unsigned char ch = (unsigned char)uri[k];
            if (ch < 0x20 || ch == 0x7F) { safe = false; break; }
        }
        if (!safe || ulen == 0) {
            t->current_uri_id = 0;      /* close any active link */
        } else {
            t->current_uri_id = sp_terminal_uri_intern(t, uri, ulen);
        }
        break;
    }
    case 10: sp_osc_set_color_pair(t, SP_COLOR_FG,     body, body_len); break;
    case 11: sp_osc_set_color_pair(t, SP_COLOR_BG,     body, body_len); break;
    case 12: sp_osc_set_color_pair(t, SP_COLOR_CURSOR, body, body_len); break;
    case 104: {
        /* No args resets the entire 256-color palette. Else reset each
         * named index. */
        if (body_len == 0) {
            for (int k = 0; k < 256; ++k) sp_terminal_reset_palette_entry(t, k);
        } else {
            size_t pos = 0;
            while (pos < body_len) {
                int idx = 0;
                size_t ns = pos;
                while (pos < body_len && body[pos] >= '0' && body[pos] <= '9') {
                    idx = idx * 10 + (body[pos] - '0');
                    pos++;
                }
                if (ns != pos && idx >= 0 && idx <= 255)
                    sp_terminal_reset_palette_entry(t, idx);
                if (pos < body_len && body[pos] == ';') pos++;
                else break;
            }
        }
        break;
    }
    case 110: sp_terminal_reset_color(t, SP_COLOR_FG);     break;
    case 111: sp_terminal_reset_color(t, SP_COLOR_BG);     break;
    case 112: sp_terminal_reset_color(t, SP_COLOR_CURSOR); break;
    case 52: {
        /* OSC 52 ; Pc ; Pd ST — set clipboard. Pc is a target list
         * (c=CLIPBOARD, p=PRIMARY, s=SELECTION, 0-7=xterm cut-buffers,
         * empty = c+s0). Pd is base64, or "?" for a query. Queries
         * aren't honored: echoing clipboard contents back to the TTY
         * is the attack vector OSC 52 is most known for, and the host
         * can't read the system clipboard in a trusted way anyway. */
        size_t pos = 0;
        size_t targets_s = pos;
        while (pos < body_len && body[pos] != ';') pos++;
        size_t targets_e = pos;
        if (pos < body_len) pos++;  /* skip ';' */
        const char* payload = body + pos;
        size_t plen = body_len - pos;
        /* Query form — drop silently. */
        if (plen == 1 && payload[0] == '?') break;
        /* RFC 4648 base64 decode, inline. Reject any byte outside the
         * alphabet so a malformed payload never writes the clipboard. */
        size_t effective = plen;
        while (effective > 0 && payload[effective - 1] == '=') effective--;
        /* Cap decoded length: xterm's default is 1 MiB; we match. */
        if (effective > 1u << 20) break;
        size_t out_cap = (effective / 4) * 3 + 3;
        char* decoded = (char*)malloc(out_cap + 1);
        if (!decoded) break;
        size_t out_len = 0;
        uint32_t acc = 0;
        int bits = 0;
        bool bad = false;
        for (size_t k = 0; k < effective; k++) {
            unsigned char c = (unsigned char)payload[k];
            if (c == '\r' || c == '\n' || c == ' ') continue;
            int v = -1;
            if (c >= 'A' && c <= 'Z')      v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = 26 + (c - 'a');
            else if (c >= '0' && c <= '9') v = 52 + (c - '0');
            else if (c == '+')             v = 62;
            else if (c == '/')             v = 63;
            if (v < 0) { bad = true; break; }
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                decoded[out_len++] = (char)((acc >> bits) & 0xFF);
            }
        }
        if (bad) { free(decoded); break; }
        decoded[out_len] = '\0';
        /* Build a NUL-terminated targets string on the stack. */
        char targets[16];
        size_t tn = targets_e - targets_s;
        if (tn >= sizeof(targets)) tn = sizeof(targets) - 1;
        memcpy(targets, body + targets_s, tn);
        targets[tn] = '\0';
        sp_event ev = {
            .kind = SP_EV_CLIPBOARD,
            .as.clipboard = { .text = decoded, .len = out_len,
                              .targets = targets },
        };
        sp_terminal_emit(t, &ev);
        free(decoded);
        break;
    }
    default: break;
    }
}

/* ---- State machine ----------------------------------------------------- */

static void sp_transition(sp_parser* p, sp_pstate s) { p->state = s; }

static void sp_process_byte(sp_parser* p, uint8_t b) {
    /* GROUND has its own fast path (most bytes are printable) */
    if (p->state == SP_PS_GROUND) {
        if (b >= 0x20 && b != 0x7F) {
            sp_handle_print(p, b);
            return;
        }
        if (b == 0x1B) { sp_parser_clear_pi(p); sp_transition(p, SP_PS_ESCAPE); return; }
        if (b <= 0x1F) { sp_handle_c0(p, b); return; }
        return; /* DEL */
    }

    switch (p->state) {
        case SP_PS_ESCAPE:
            if (b == 0x5B)              { sp_parser_clear_pi(p); sp_transition(p, SP_PS_CSI_ENTRY); }
            else if (b == 0x5D)         { p->osc_len = 0; p->osc_saw_esc = false;
                                           sp_transition(p, SP_PS_OSC_STRING); }
            else if (b == 0x50)         { sp_parser_clear_pi(p); sp_transition(p, SP_PS_DCS_ENTRY); }
            else if (b >= 0x58 && b <= 0x5E) { sp_transition(p, SP_PS_SOS_PM_APC); }
            else if (b >= 0x30 && b <= 0x7E) { sp_esc_dispatch(p, b); sp_transition(p, SP_PS_GROUND); }
            else if (b >= 0x20 && b <= 0x2F) { sp_parser_add_intermediate(p, (char)b);
                                                sp_transition(p, SP_PS_ESCAPE_INTERMEDIATE); }
            else { sp_transition(p, SP_PS_GROUND); }
            break;

        case SP_PS_ESCAPE_INTERMEDIATE:
            if (b >= 0x40 && b <= 0x5F) { sp_esc_dispatch(p, b); sp_transition(p, SP_PS_GROUND); }
            else if (b >= 0x30 && b <= 0x3F) { sp_parser_add_intermediate(p, (char)b); }
            else { sp_transition(p, SP_PS_GROUND); }
            break;

        case SP_PS_CSI_ENTRY:
            if (b >= 0x40 && b <= 0x7E) { sp_csi_dispatch(p, b); sp_transition(p, SP_PS_GROUND); }
            else if (b == 0x3F)         { p->private_csi = true; sp_transition(p, SP_PS_CSI_PARAM); }
            else if (b >= 0x30 && b <= 0x39) { sp_parser_add_param_digit(p, (char)b);
                                                sp_transition(p, SP_PS_CSI_PARAM); }
            else if (b == 0x3B)         { sp_parser_param_sep(p); sp_transition(p, SP_PS_CSI_PARAM); }
            else if (b >= 0x20 && b <= 0x2F) { sp_parser_add_intermediate(p, (char)b);
                                                sp_transition(p, SP_PS_CSI_INTERMEDIATE); }
            else if (b == 0x3A)         { sp_transition(p, SP_PS_CSI_IGNORE); }
            else                        { sp_transition(p, SP_PS_GROUND); }
            break;

        case SP_PS_CSI_PARAM:
            if (b >= 0x40 && b <= 0x7E) { sp_csi_dispatch(p, b); sp_transition(p, SP_PS_GROUND); }
            else if (b >= 0x30 && b <= 0x39) { sp_parser_add_param_digit(p, (char)b); }
            else if (b == 0x3B)         { sp_parser_param_sep(p); }
            else if (b >= 0x20 && b <= 0x2F) { sp_parser_add_intermediate(p, (char)b);
                                                sp_transition(p, SP_PS_CSI_INTERMEDIATE); }
            else if (b == 0x3A)         { sp_transition(p, SP_PS_CSI_IGNORE); }
            else                        { sp_transition(p, SP_PS_GROUND); }
            break;

        case SP_PS_CSI_INTERMEDIATE:
            if (b >= 0x40 && b <= 0x7E) { sp_csi_dispatch(p, b); sp_transition(p, SP_PS_GROUND); }
            else if (b >= 0x20 && b <= 0x2F) { sp_parser_add_intermediate(p, (char)b); }
            else                        { sp_transition(p, SP_PS_GROUND); }
            break;

        case SP_PS_CSI_IGNORE:
            if (b >= 0x40 && b <= 0x7E) sp_transition(p, SP_PS_GROUND);
            break;

        case SP_PS_OSC_STRING:
            if (b == 0x07) {                      /* BEL terminator */
                sp_osc_dispatch(p);
                sp_transition(p, SP_PS_GROUND);
            } else if (p->osc_saw_esc) {
                if (b == 0x5C) {                  /* ST = ESC \\ */
                    sp_osc_dispatch(p);
                } /* else: malformed */
                sp_transition(p, SP_PS_GROUND);
            } else if (b == 0x1B) {
                p->osc_saw_esc = true;
            } else if (p->osc_len < SP_OSC_BUF_MAX - 1) {
                p->osc_buf[p->osc_len++] = (char)b;
            }
            break;

        case SP_PS_DCS_ENTRY:
        case SP_PS_DCS_PARAM:
        case SP_PS_DCS_INTERMEDIATE:
        case SP_PS_DCS_IGNORE:
        case SP_PS_DCS_PASSTHROUGH:
        case SP_PS_SOS_PM_APC:
            /* We don't act on DCS/SOS/PM/APC sequences yet — just consume
             * until ESC (which abandons), letting the ground-state ESC handler
             * pick up the next sequence. */
            if (b == 0x1B) {
                sp_parser_clear_pi(p);
                sp_transition(p, SP_PS_ESCAPE);
            }
            break;

        case SP_PS_GROUND:
            /* unreachable — handled at top */
            break;
    }
}

/* ---- Public API -------------------------------------------------------- */

sp_parser* sp_parser_create(struct sp_terminal* owner) {
    sp_parser* p = (sp_parser*)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->owner = owner;
    p->state = SP_PS_GROUND;
    p->gr = (sp_cell_attrs){
        .fg = owner ? owner->opts.color_fg : 0xFFFFFFFFu,
        .bg = owner ? owner->opts.color_bg : 0x000000FFu,
        .flags = 0,
        .underline_style = 0,
        ._pad = 0,
    };
    return p;
}

void sp_parser_destroy(sp_parser* p) {
    free(p);
}

void sp_parser_reset(sp_parser* p) {
    if (!p) return;
    p->state = SP_PS_GROUND;
    sp_parser_clear_pi(p);
    p->osc_len = 0;
    p->osc_saw_esc = false;
    p->utf8_cp = 0;
    p->utf8_remaining = 0;
    p->gr = (sp_cell_attrs){
        .fg = p->owner ? p->owner->opts.color_fg : 0xFFFFFFFFu,
        .bg = p->owner ? p->owner->opts.color_bg : 0x000000FFu,
        .flags = 0,
        .underline_style = 0,
        ._pad = 0,
    };
}

void sp_parser_feed(sp_parser* p, const uint8_t* data, size_t len) {
    if (!p || !data) return;
    for (size_t i = 0; i < len; ++i) sp_process_byte(p, data[i]);
}
