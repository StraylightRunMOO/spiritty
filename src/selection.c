/*
 * selection.c — selection manager (C11 port).
 *
 * Tracks one rectangular/linear/word/line selection over the active screen
 * buffer. Word/line boundary expansion ports the legacy behaviour from
 * `src/selection_manager.cpp`. Clipboard I/O is intentionally deferred to
 * the host (browser DOM clipboard, ImGui demo, Embind layer).
 */
#define SPIRITTY_BUILDING 1

#include "internal.h"

#include <stdlib.h>
#include <string.h>

struct sp_selmgr {
    struct sp_terminal* owner;
    sp_selection sel;
};

/* ---- Helpers ----------------------------------------------------------- */

/* Order endpoints so start <= end in row-major order. */
static sp_selection sp_sel_normalized(const sp_selection* s) {
    sp_selection n = *s;
    bool swap = (n.start_row > n.end_row) ||
                (n.start_row == n.end_row && n.start_col > n.end_col);
    if (swap) {
        int32_t tr = n.start_row, tc = n.start_col;
        n.start_row = n.end_row;
        n.start_col = n.end_col;
        n.end_row = tr;
        n.end_col = tc;
    }
    return n;
}

static void sp_word_boundaries(struct sp_terminal* t, int32_t row, int32_t col,
                               int32_t* out_start, int32_t* out_end) {
    const sp_line* ln = sp_buffer_line_const(t->buf, row);
    int32_t cols = sp_buffer_cols(t->buf);
    if (!ln || cols == 0 || col < 0 || col >= cols) {
        *out_start = col;
        *out_end   = col;
        return;
    }
    /* Walk forward past non-word chars to find a word. */
    int32_t c = col;
    while (c < cols && !sp_codepoint_is_word(ln->cells[c].codepoint)) c++;
    if (c >= cols) { *out_start = col; *out_end = col; return; }

    int32_t start = c, end = c;
    while (start > 0 && sp_codepoint_is_word(ln->cells[start - 1].codepoint)) start--;
    while (end + 1 < cols && sp_codepoint_is_word(ln->cells[end + 1].codepoint)) end++;
    *out_start = start;
    *out_end   = end;
}

static void sp_apply_mode(struct sp_terminal* t, sp_selection* s) {
    if (s->mode == SP_SEL_LINE) {
        int32_t cols = sp_buffer_cols(t->buf);
        s->start_col = 0;
        s->end_col   = cols > 0 ? cols - 1 : 0;
    } else if (s->mode == SP_SEL_WORD) {
        int32_t a, b;
        sp_word_boundaries(t, s->start_row, s->start_col, &a, &b);
        s->start_col = a;
        int32_t c, d;
        sp_word_boundaries(t, s->end_row, s->end_col, &c, &d);
        s->end_col = d;
    }
}

/* ---- Public API -------------------------------------------------------- */

sp_selmgr* sp_selmgr_create(struct sp_terminal* owner) {
    sp_selmgr* sm = (sp_selmgr*)calloc(1, sizeof *sm);
    if (!sm) return NULL;
    sm->owner = owner;
    return sm;
}

void sp_selmgr_destroy(sp_selmgr* sm) { free(sm); }

void sp_selmgr_begin(sp_selmgr* sm, int32_t row, int32_t col, sp_sel_mode mode) {
    if (!sm) return;
    sm->sel.active    = true;
    sm->sel.mode      = mode;
    sm->sel.start_row = row;
    sm->sel.start_col = col;
    sm->sel.end_row   = row;
    sm->sel.end_col   = col;
    sp_apply_mode(sm->owner, &sm->sel);
}

void sp_selmgr_extend(sp_selmgr* sm, int32_t row, int32_t col) {
    if (!sm || !sm->sel.active) return;
    sm->sel.end_row = row;
    sm->sel.end_col = col;
    sp_apply_mode(sm->owner, &sm->sel);
}

void sp_selmgr_clear(sp_selmgr* sm) {
    if (!sm) return;
    memset(&sm->sel, 0, sizeof sm->sel);
}

bool sp_selmgr_active(const sp_selmgr* sm) {
    return sm && sm->sel.active;
}

const sp_selection* sp_selmgr_selection(const sp_selmgr* sm) {
    return sm ? &sm->sel : NULL;
}

/* ---- Text extraction --------------------------------------------------- */

char* sp_selmgr_text(const sp_selmgr* sm) {
    if (!sm || !sm->sel.active || !sm->owner || !sm->owner->buf) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    sp_selection n = sp_sel_normalized(&sm->sel);
    int32_t cols = sp_buffer_cols(sm->owner->buf);

    /* Concatenate per-line ranges with newlines between rows. */
    size_t cap = 256, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    for (int32_t r = n.start_row; r <= n.end_row; ++r) {
        const sp_line* ln = sp_buffer_line_const(sm->owner->buf, r);
        if (!ln) continue;
        int32_t s = (r == n.start_row) ? n.start_col : 0;
        int32_t e = (r == n.end_row)   ? n.end_col   : cols - 1;
        /* sp_line_text_range is half-open [start, end); selection endpoints
         * are inclusive — bump end by one. */
        char* part = sp_line_text_range(ln, s, e + 1);
        if (!part) continue;
        size_t plen = strlen(part);
        size_t need = len + plen + 2;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char* tmp = (char*)realloc(out, cap);
            if (!tmp) { free(part); free(out); return NULL; }
            out = tmp;
        }
        memcpy(out + len, part, plen);
        len += plen;
        if (r < n.end_row) out[len++] = '\n';
        out[len] = '\0';
        free(part);
    }
    return out;
}
