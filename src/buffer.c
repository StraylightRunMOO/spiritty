/*
 * buffer.c — active screen + scrollback.
 *
 * Active screen: 2-D grid of `sp_cell`. Each row's cell array is allocated
 * with 64-byte alignment so SIMD scans (e.g. parser fast paths in Phase 6)
 * can use aligned loads.
 *
 * Scrollback: ring of `sp_line` structs (NOT a rope yet; rope integration
 * is intentionally deferred to Phase 6 — Halyard is byte-oriented and the
 * byte->cell encoding belongs in a separate layer to avoid losing per-cell
 * SGR attributes).
 */
#define SPIRITTY_BUILDING 1

#include "internal.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 *  UTF-8 encoding helper
 * ========================================================================= */

static size_t sp_utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    } else if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    } else if (cp < 0x110000u) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    out[0] = '?';
    return 1;
}

bool sp_codepoint_is_word(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= '0' && cp <= '9') return true;
    if (cp == '_') return true;
    /* Common non-ASCII letters (Latin extended). Good enough for a v1; we
     * can swap in proper Unicode word-char tables when needed. */
    if (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7) return true;
    return false;
}

/* ===========================================================================
 *  64-byte aligned cell-row allocation
 * ========================================================================= */

#define SP_LINE_ALIGN 64

static sp_cell* sp_alloc_cells(int32_t cols) {
    if (cols <= 0) return NULL;
    size_t bytes = (size_t)cols * sizeof(sp_cell);
    /* Round up to alignment. */
    size_t rounded = (bytes + (SP_LINE_ALIGN - 1)) & ~(size_t)(SP_LINE_ALIGN - 1);
    void* p = NULL;
#if defined(_MSC_VER)
    p = _aligned_malloc(rounded, SP_LINE_ALIGN);
#else
    if (posix_memalign(&p, SP_LINE_ALIGN, rounded) != 0) p = NULL;
#endif
    if (!p) return NULL;
    memset(p, 0, rounded);
    return (sp_cell*)p;
}

static void sp_free_cells(sp_cell* cells) {
    if (!cells) return;
#if defined(_MSC_VER)
    _aligned_free(cells);
#else
    free(cells);
#endif
}

/* ===========================================================================
 *  Line
 * ========================================================================= */

void sp_line_init(sp_line* ln, int32_t cols) {
    ln->cells = sp_alloc_cells(cols);
    ln->cols  = ln->cells ? cols : 0;
    ln->dirty = true;
    /* default cell: blank, width=1 */
    for (int32_t i = 0; i < ln->cols; ++i) {
        ln->cells[i].codepoint = ' ';
        ln->cells[i].width     = 1;
    }
}

void sp_line_free(sp_line* ln) {
    if (!ln) return;
    sp_free_cells(ln->cells);
    ln->cells = NULL;
    ln->cols  = 0;
}

void sp_line_resize(sp_line* ln, int32_t cols, sp_cell_attrs def) {
    if (cols == ln->cols) return;
    sp_cell* nc = sp_alloc_cells(cols);
    if (!nc) return;
    int32_t copy = cols < ln->cols ? cols : ln->cols;
    for (int32_t i = 0; i < copy; ++i) nc[i] = ln->cells[i];
    for (int32_t i = copy; i < cols; ++i) {
        nc[i].codepoint = ' ';
        nc[i].width     = 1;
        nc[i].attrs     = def;
    }
    sp_free_cells(ln->cells);
    ln->cells = nc;
    ln->cols  = cols;
    ln->dirty = true;
}

void sp_line_clear(sp_line* ln, sp_cell_attrs def) {
    for (int32_t i = 0; i < ln->cols; ++i) {
        ln->cells[i].codepoint = ' ';
        ln->cells[i].width     = 1;
        ln->cells[i]._pad[0] = ln->cells[i]._pad[1] = ln->cells[i]._pad[2] = 0;
        ln->cells[i].attrs     = def;
    }
    ln->dirty = true;
}

void sp_line_clear_range(sp_line* ln, int32_t start, int32_t end,
                         sp_cell_attrs def) {
    if (start < 0)        start = 0;
    if (end   > ln->cols) end   = ln->cols;
    for (int32_t i = start; i < end; ++i) {
        ln->cells[i].codepoint = ' ';
        ln->cells[i].width     = 1;
        ln->cells[i].attrs     = def;
    }
    ln->dirty = true;
}

char* sp_line_text(const sp_line* ln) {
    return sp_line_text_range(ln, 0, ln->cols);
}

char* sp_line_text_range(const sp_line* ln, int32_t start, int32_t end) {
    if (start < 0)        start = 0;
    if (end   > ln->cols) end   = ln->cols;
    /* Worst-case 4 UTF-8 bytes per cell + NUL. */
    size_t cap = (size_t)(end - start) * 4 + 1;
    if (cap < 1) cap = 1;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t off = 0;
    for (int32_t i = start; i < end; ++i) {
        uint32_t cp = ln->cells[i].codepoint;
        if (cp == 0) cp = ' ';
        char b[4];
        size_t n = sp_utf8_encode(cp, b);
        memcpy(out + off, b, n);
        off += n;
    }
    out[off] = '\0';
    return out;
}

/* ===========================================================================
 *  Scrollback ring
 * ========================================================================= */

typedef struct {
    sp_line* lines;     /* ring of `cap` slots */
    size_t   cap;
    size_t   head;      /* index of oldest valid slot */
    size_t   size;      /* number of valid lines */
} sp_scrollback;

static void sb_init(sp_scrollback* s, size_t cap) {
    s->cap   = cap;
    s->head  = 0;
    s->size  = 0;
    s->lines = cap ? (sp_line*)calloc(cap, sizeof(sp_line)) : NULL;
}

static void sb_free(sp_scrollback* s) {
    if (!s->lines) return;
    for (size_t i = 0; i < s->size; ++i) {
        size_t idx = (s->head + i) % s->cap;
        sp_line_free(&s->lines[idx]);
    }
    free(s->lines);
    s->lines = NULL;
    s->cap = s->head = s->size = 0;
}

static void sb_push(sp_scrollback* s, sp_line line) {
    if (s->cap == 0) {
        sp_line_free(&line);
        return;
    }
    if (s->size == s->cap) {
        /* Drop oldest. */
        sp_line_free(&s->lines[s->head]);
        s->lines[s->head] = line;
        s->head = (s->head + 1) % s->cap;
    } else {
        size_t idx = (s->head + s->size) % s->cap;
        s->lines[idx] = line;
        s->size++;
    }
}

static const sp_line* sb_at(const sp_scrollback* s, size_t i) {
    if (i >= s->size) return NULL;
    return &s->lines[(s->head + i) % s->cap];
}

/* ===========================================================================
 *  Buffer
 * ========================================================================= */

struct sp_buffer {
    int32_t       rows, cols;
    sp_line*      visible;        /* rows-long array of lines */
    sp_scrollback sb;
};

sp_buffer* sp_buffer_create(int32_t rows, int32_t cols, int32_t sb_max) {
    if (rows <= 0 || cols <= 0) return NULL;
    sp_buffer* b = (sp_buffer*)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->rows = rows;
    b->cols = cols;
    b->visible = (sp_line*)calloc((size_t)rows, sizeof(sp_line));
    if (!b->visible) { free(b); return NULL; }
    for (int32_t i = 0; i < rows; ++i) {
        sp_line_init(&b->visible[i], cols);
    }
    sb_init(&b->sb, (size_t)(sb_max < 0 ? 0 : sb_max));
    return b;
}

void sp_buffer_destroy(sp_buffer* b) {
    if (!b) return;
    if (b->visible) {
        for (int32_t i = 0; i < b->rows; ++i) sp_line_free(&b->visible[i]);
        free(b->visible);
    }
    sb_free(&b->sb);
    free(b);
}

int32_t sp_buffer_rows(const sp_buffer* b) { return b ? b->rows : 0; }
int32_t sp_buffer_cols(const sp_buffer* b) { return b ? b->cols : 0; }

sp_line* sp_buffer_line(sp_buffer* b, int32_t row) {
    if (!b || row < 0 || row >= b->rows) return NULL;
    return &b->visible[row];
}
const sp_line* sp_buffer_line_const(const sp_buffer* b, int32_t row) {
    if (!b || row < 0 || row >= b->rows) return NULL;
    return &b->visible[row];
}
sp_cell* sp_buffer_cell(sp_buffer* b, int32_t row, int32_t col) {
    sp_line* ln = sp_buffer_line(b, row);
    if (!ln || col < 0 || col >= ln->cols) return NULL;
    return &ln->cells[col];
}
const sp_cell* sp_buffer_cell_const(const sp_buffer* b, int32_t row, int32_t col) {
    const sp_line* ln = sp_buffer_line_const(b, row);
    if (!ln || col < 0 || col >= ln->cols) return NULL;
    return &ln->cells[col];
}

void sp_buffer_resize(sp_buffer* b, int32_t rows, int32_t cols, sp_cell_attrs def) {
    if (!b || rows <= 0 || cols <= 0) return;

    /* Adjust columns first — every existing row resizes in place. */
    if (cols != b->cols) {
        for (int32_t i = 0; i < b->rows; ++i) {
            sp_line_resize(&b->visible[i], cols, def);
        }
        b->cols = cols;
    }

    if (rows == b->rows) return;

    if (rows < b->rows) {
        /* Lose lines off the bottom (don't push to scrollback — these are
         * in-progress prompt rows; matches xterm behaviour). */
        for (int32_t i = rows; i < b->rows; ++i) sp_line_free(&b->visible[i]);
        sp_line* nv = (sp_line*)realloc(b->visible, (size_t)rows * sizeof(sp_line));
        if (nv) b->visible = nv;
    } else {
        sp_line* nv = (sp_line*)realloc(b->visible, (size_t)rows * sizeof(sp_line));
        if (!nv) return;
        b->visible = nv;
        for (int32_t i = b->rows; i < rows; ++i) {
            sp_line_init(&b->visible[i], cols);
        }
    }
    b->rows = rows;
}

void sp_buffer_clear(sp_buffer* b, sp_cell_attrs def) {
    if (!b) return;
    for (int32_t i = 0; i < b->rows; ++i) sp_line_clear(&b->visible[i], def);
}

void sp_buffer_clear_line(sp_buffer* b, int32_t row, sp_cell_attrs def) {
    sp_line* ln = sp_buffer_line(b, row);
    if (ln) sp_line_clear(ln, def);
}

void sp_buffer_clear_range(sp_buffer* b,
                           int32_t r0, int32_t c0,
                           int32_t r1, int32_t c1,
                           sp_cell_attrs def) {
    if (!b) return;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int32_t tr = r0; r0 = r1; r1 = tr;
        int32_t tc = c0; c0 = c1; c1 = tc;
    }
    for (int32_t r = r0; r <= r1 && r < b->rows; ++r) {
        sp_line* ln = &b->visible[r];
        int32_t s = (r == r0) ? c0 : 0;
        int32_t e = (r == r1) ? c1 + 1 : ln->cols;
        sp_line_clear_range(ln, s, e, def);
    }
}

void sp_buffer_scroll_up(sp_buffer* b, int32_t count, sp_cell_attrs def) {
    if (!b || count <= 0) return;
    if (count > b->rows) count = b->rows;
    for (int32_t i = 0; i < count; ++i) {
        /* Push topmost line to scrollback (transfer ownership). */
        sp_line out = b->visible[0];
        sb_push(&b->sb, out);
        /* Shift remaining rows up. */
        for (int32_t r = 1; r < b->rows; ++r) {
            b->visible[r - 1] = b->visible[r];
        }
        /* New blank at the bottom. */
        sp_line nl;
        sp_line_init(&nl, b->cols);
        for (int32_t c = 0; c < b->cols; ++c) nl.cells[c].attrs = def;
        b->visible[b->rows - 1] = nl;
    }
    sp_buffer_mark_all_dirty(b);
}

void sp_buffer_scroll_down(sp_buffer* b, int32_t count, sp_cell_attrs def) {
    if (!b || count <= 0) return;
    if (count > b->rows) count = b->rows;
    for (int32_t i = 0; i < count; ++i) {
        sp_line_free(&b->visible[b->rows - 1]);
        for (int32_t r = b->rows - 1; r > 0; --r) {
            b->visible[r] = b->visible[r - 1];
        }
        sp_line nl;
        sp_line_init(&nl, b->cols);
        for (int32_t c = 0; c < b->cols; ++c) nl.cells[c].attrs = def;
        b->visible[0] = nl;
    }
    sp_buffer_mark_all_dirty(b);
}

void sp_buffer_insert_lines(sp_buffer* b, int32_t row, int32_t count,
                            sp_cell_attrs def) {
    if (!b || row < 0 || row >= b->rows || count <= 0) return;
    if (count > b->rows - row) count = b->rows - row;
    for (int32_t i = 0; i < count; ++i) {
        sp_line_free(&b->visible[b->rows - 1]);
        for (int32_t r = b->rows - 1; r > row; --r) {
            b->visible[r] = b->visible[r - 1];
        }
        sp_line nl;
        sp_line_init(&nl, b->cols);
        for (int32_t c = 0; c < b->cols; ++c) nl.cells[c].attrs = def;
        b->visible[row] = nl;
    }
    sp_buffer_mark_all_dirty(b);
}

void sp_buffer_delete_lines(sp_buffer* b, int32_t row, int32_t count,
                            sp_cell_attrs def) {
    if (!b || row < 0 || row >= b->rows || count <= 0) return;
    if (count > b->rows - row) count = b->rows - row;
    for (int32_t i = 0; i < count; ++i) {
        sp_line_free(&b->visible[row]);
        for (int32_t r = row; r < b->rows - 1; ++r) {
            b->visible[r] = b->visible[r + 1];
        }
        sp_line nl;
        sp_line_init(&nl, b->cols);
        for (int32_t c = 0; c < b->cols; ++c) nl.cells[c].attrs = def;
        b->visible[b->rows - 1] = nl;
    }
    sp_buffer_mark_all_dirty(b);
}

void sp_buffer_insert_chars(sp_buffer* b, int32_t row, int32_t col,
                            int32_t count, sp_cell_attrs def) {
    sp_line* ln = sp_buffer_line(b, row);
    if (!ln || col < 0 || col >= ln->cols || count <= 0) return;
    if (count > ln->cols - col) count = ln->cols - col;
    /* Shift right. */
    for (int32_t i = ln->cols - 1; i >= col + count; --i) {
        ln->cells[i] = ln->cells[i - count];
    }
    /* Blank inserted region. */
    for (int32_t i = col; i < col + count; ++i) {
        ln->cells[i].codepoint = ' ';
        ln->cells[i].width     = 1;
        ln->cells[i].attrs     = def;
    }
    ln->dirty = true;
}

void sp_buffer_delete_chars(sp_buffer* b, int32_t row, int32_t col,
                            int32_t count, sp_cell_attrs def) {
    sp_line* ln = sp_buffer_line(b, row);
    if (!ln || col < 0 || col >= ln->cols || count <= 0) return;
    if (count > ln->cols - col) count = ln->cols - col;
    /* Shift left. */
    for (int32_t i = col; i < ln->cols - count; ++i) {
        ln->cells[i] = ln->cells[i + count];
    }
    /* Blank trailing region. */
    for (int32_t i = ln->cols - count; i < ln->cols; ++i) {
        ln->cells[i].codepoint = ' ';
        ln->cells[i].width     = 1;
        ln->cells[i].attrs     = def;
    }
    ln->dirty = true;
}

void sp_buffer_write_cp(sp_buffer* b, int32_t row, int32_t col,
                        uint32_t cp, sp_cell_attrs attrs) {
    sp_cell* c = sp_buffer_cell(b, row, col);
    if (!c) return;
    c->codepoint = cp;
    c->width     = 1;
    c->attrs     = attrs;
    if (row >= 0 && row < b->rows) b->visible[row].dirty = true;
}

size_t sp_buffer_scrollback_size(const sp_buffer* b) {
    return b ? b->sb.size : 0;
}
size_t sp_buffer_scrollback_max(const sp_buffer* b) {
    return b ? b->sb.cap : 0;
}
const sp_line* sp_buffer_scrollback_at(const sp_buffer* b, size_t idx) {
    if (!b) return NULL;
    return sb_at(&b->sb, idx);
}

void sp_buffer_mark_all_dirty(sp_buffer* b) {
    if (!b) return;
    for (int32_t i = 0; i < b->rows; ++i) b->visible[i].dirty = true;
}
void sp_buffer_clear_dirty(sp_buffer* b) {
    if (!b) return;
    for (int32_t i = 0; i < b->rows; ++i) b->visible[i].dirty = false;
}

char* sp_buffer_text(const sp_buffer* b) {
    if (!b) {
        char* z = (char*)malloc(1);
        if (z) z[0] = '\0';
        return z;
    }
    /* Worst case: rows * (cols * 4 + 1 newline) + NUL */
    size_t cap = (size_t)b->rows * ((size_t)b->cols * 4 + 1) + 1;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t off = 0;
    for (int32_t r = 0; r < b->rows; ++r) {
        const sp_line* ln = &b->visible[r];
        for (int32_t i = 0; i < ln->cols; ++i) {
            uint32_t cp = ln->cells[i].codepoint;
            if (cp == 0) cp = ' ';
            char tmp[4];
            size_t n = sp_utf8_encode(cp, tmp);
            memcpy(out + off, tmp, n);
            off += n;
        }
        if (r < b->rows - 1) out[off++] = '\n';
    }
    out[off] = '\0';
    return out;
}
