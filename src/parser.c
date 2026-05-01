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

/* xterm/ANSI 256-color palette (compressed from the legacy init_color_palette).
 * Index 0..15 = system colors, 16..231 = 6×6×6 cube, 232..255 = grayscale. */
static uint32_t sp_palette_default(int idx) {
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
    /* Grayscale */
    uint32_t v = (uint32_t)(8 + 10 * (idx - 232));
    return (v << 24) | (v << 16) | (v << 8) | 0xFFu;
}

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
    if (t->cursor.col + 1 < cols) {
        t->cursor.col++;
    } else if (t->auto_wrap) {
        t->cursor.col = 0;
        if (t->cursor.row + 1 < rows) {
            t->cursor.row++;
        } else {
            sp_buffer_scroll_up(t->buf, 1, t->default_attrs);
            /* row stays at last row */
        }
    }
    /* else: pinned at right margin */
}

static void sp_newline(struct sp_terminal* t) {
    int32_t rows = sp_buffer_rows(t->buf);
    if (t->cursor.row + 1 < rows) {
        t->cursor.row++;
    } else {
        sp_buffer_scroll_up(t->buf, 1, t->default_attrs);
    }
}

/* ---- Print handler ----------------------------------------------------- */

static void sp_handle_print_cp(sp_parser* p, uint32_t cp) {
    struct sp_terminal* t = p->owner;
    if (!t || !t->buf) return;
    sp_buffer_write_cp(t->buf, t->cursor.row, t->cursor.col, cp, p->gr);
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
            p->gr.fg = sp_palette_default(pv - 30);
        } else if (pv == 38) {
            /* 38;5;N or 38;2;R;G;B */
            if (i + 2 < p->nparams && p->params[i + 1] == 5) {
                p->gr.fg = sp_palette_default(p->params[i + 2]);
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
            p->gr.bg = sp_palette_default(pv - 40);
        } else if (pv == 48) {
            if (i + 2 < p->nparams && p->params[i + 1] == 5) {
                p->gr.bg = sp_palette_default(p->params[i + 2]);
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
            p->gr.fg = sp_palette_default(pv - 90 + 8);
        } else if (pv >= 100 && pv <= 107) {
            p->gr.bg = sp_palette_default(pv - 100 + 8);
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
        case 'r': /* DECSTBM (ignored — single-region for now) */
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
            if (t->cursor.row > 0) {
                t->cursor.row--;
            } else {
                sp_buffer_scroll_down(t->buf, 1, t->default_attrs);
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

    if (ps == 0 || ps == 1 || ps == 2) {
        /* Window title (and icon name for 0/1) */
        size_t n = (size_t)(end - i);
        if (n >= sizeof(t->window_title)) n = sizeof(t->window_title) - 1;
        memcpy(t->window_title, p->osc_buf + i, n);
        t->window_title[n] = '\0';
        sp_event ev = {
            .kind = SP_EV_TITLE,
            .as.title.text = t->window_title,
        };
        sp_terminal_emit(t, &ev);
    }
    /* Other Ps codes (color queries, etc.) — drop for now. */
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
