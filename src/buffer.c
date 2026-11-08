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
/* posix_memalign() and aligned_alloc() require feature-test macros on glibc. */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200112L
#endif

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
 *  Per-buffer line freelist
 *
 *  Hot-path allocator: scrolling a steady output stream calls sp_line_init
 *  per scrolled line. Each call reaches into posix_memalign for an aligned
 *  cols * sizeof(sp_cell) block — small but frequent enough to dominate
 *  perf at high write rates (the perf_smoke harness measured ~131k
 *  scrolls / 10 MB at 9 MB/s before this freelist landed).
 *
 *  Strategy: keep a singly-linked stack of recently-freed row buffers,
 *  keyed by `cols` (the only dimension that affects layout). sp_free_cells
 *  pushes; sp_alloc_cells pops. On buffer resize, the freelist is drained
 *  in sp_buffer_resize so we never hand out a stale-sized row.
 *
 *  The list is owned by sp_buffer (not global) — concurrent terminals
 *  don't share state, and a global freelist would need a mutex that
 *  defeats the purpose. Cap the list at a few hundred entries so a long
 *  burst of frees doesn't pin unbounded memory; anything past the cap is
 *  released back to libc.
 *
 *  This is morally a Memento pool, just in 30 lines. We can swap it for
 *  Memento later if profiling justifies the extra dep — see PLAN.md.
 * ========================================================================= */

#define SP_LINE_POOL_CAP 256

typedef struct sp_line_pool_node {
    struct sp_line_pool_node* next;
} sp_line_pool_node;

typedef struct {
    sp_line_pool_node* head;
    int32_t            cols;
    int32_t            count;
} sp_line_pool;

/* Per-thread current pool; see pool-swap helpers below. */
static _Thread_local sp_line_pool* sp_active_pool = NULL;

static sp_line_pool* sp_pool_swap_in(sp_line_pool* p) {
    sp_line_pool* prev = sp_active_pool;
    sp_active_pool = p;
    return prev;
}
static void sp_pool_swap_out(sp_line_pool* prev) {
    sp_active_pool = prev;
}

static void sp_pool_drain(sp_line_pool* p) {
    sp_line_pool_node* n = p->head;
    while (n) {
        sp_line_pool_node* next = n->next;
#if defined(_MSC_VER)
        _aligned_free(n);
#else
        free(n);
#endif
        n = next;
    }
    p->head  = NULL;
    p->count = 0;
}

/* ===========================================================================
 *  64-byte aligned cell-row allocation
 * ========================================================================= */

#define SP_LINE_ALIGN 64

static sp_cell* sp_alloc_cells(int32_t cols) {
    if (cols <= 0) return NULL;

#if 0  /* Pool fast path temporarily disabled — see Phase 2 bug note. */
    sp_line_pool* pool = sp_active_pool;
    if (pool && pool->head && pool->cols == cols) {
        sp_line_pool_node* n = pool->head;
        pool->head = n->next;
        pool->count--;
        memset(n, 0, (size_t)cols * sizeof(sp_cell));
        return (sp_cell*)n;
    }
#endif

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

#if 0  /* Pool fast path temporarily disabled — see Phase 2 bug note. */
    sp_line_pool* pool = sp_active_pool;
    if (pool && pool->cols > 0 && pool->count < SP_LINE_POOL_CAP) {
        sp_line_pool_node* n = (sp_line_pool_node*)cells;
        n->next = pool->head;
        pool->head = n;
        pool->count++;
        return;
    }
#endif

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
    sp_line_pool  pool;
    /* Viewport offset in lines ABOVE the live bottom. 0 = pinned to bottom
     * (the normal render path); scroll_offset ≤ sb.size caps at the top of
     * scrollback. Set by the host for wheel / PgUp; reset to 0 on any
     * write (xterm "snap to bottom on output"). */
    size_t        scroll_offset;
    /* Parallel per-cell URI intern-id grid. 0 == no URI. Co-resized with
     * the active screen; written by the parser's print path; shifted by
     * scroll_up/scroll_down; zeroed on clear/insert/delete. NOT carried
     * into scrollback — hit-tests on scrolled-up rows return 0. */
    uint16_t*     uri_grid;
    /* DECSTBM scroll region (inclusive, 0-based). Defaults to the full
     * screen. scroll_up/scroll_down operate within this range. */
    int32_t       scroll_top;
    int32_t       scroll_bot;
};

sp_buffer* sp_buffer_create(int32_t rows, int32_t cols, int32_t sb_max) {
    if (rows <= 0 || cols <= 0) return NULL;
    sp_buffer* b = (sp_buffer*)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->rows = rows;
    b->cols = cols;
    b->pool.head  = NULL;
    b->pool.cols  = cols;
    b->pool.count = 0;
    b->visible = (sp_line*)calloc((size_t)rows, sizeof(sp_line));
    if (!b->visible) { free(b); return NULL; }

    sp_line_pool* prev = sp_pool_swap_in(&b->pool);
    for (int32_t i = 0; i < rows; ++i) {
        sp_line_init(&b->visible[i], cols);
    }
    sp_pool_swap_out(prev);

    sb_init(&b->sb, (size_t)(sb_max < 0 ? 0 : sb_max));
    b->uri_grid = (uint16_t*)calloc((size_t)rows * (size_t)cols, sizeof(uint16_t));
    /* uri_grid is best-effort — we don't fail buffer creation on OOM here;
     * hit-tests just return 0 when the grid is absent. */
    b->scroll_top = 0;
    b->scroll_bot = rows - 1;
    return b;
}

int32_t sp_buffer_scroll_top(const sp_buffer* b) { return b ? b->scroll_top : 0; }
int32_t sp_buffer_scroll_bot(const sp_buffer* b) { return b ? b->scroll_bot : 0; }

void sp_buffer_set_scroll_region(sp_buffer* b, int32_t top, int32_t bot) {
    if (!b) return;
    if (top < 0) top = 0;
    if (bot >= b->rows) bot = b->rows - 1;
    /* Degenerate (top > bot or single-row region) → reset to full screen,
     * matching xterm: an invalid DECSTBM homes the scroll region. */
    if (top >= bot) { top = 0; bot = b->rows - 1; }
    b->scroll_top = top;
    b->scroll_bot = bot;
}

void sp_buffer_destroy(sp_buffer* b) {
    if (!b) return;
    sp_line_pool* prev = sp_pool_swap_in(&b->pool);
    if (b->visible) {
        for (int32_t i = 0; i < b->rows; ++i) sp_line_free(&b->visible[i]);
        free(b->visible);
    }
    sb_free(&b->sb);
    sp_pool_swap_out(prev);
    sp_pool_drain(&b->pool);
    free(b->uri_grid);
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

    sp_line_pool* prev = sp_pool_swap_in(&b->pool);

    /* Column changes invalidate every pooled row — drain before resizing. */
    if (cols != b->cols) {
        sp_pool_drain(&b->pool);
        b->pool.cols = cols;
        for (int32_t i = 0; i < b->rows; ++i) {
            sp_line_resize(&b->visible[i], cols, def);
        }
        b->cols = cols;
    }

    if (rows == b->rows) { sp_pool_swap_out(prev); return; }

    if (rows < b->rows) {
        /* Lose lines off the bottom (don't push to scrollback — these are
         * in-progress prompt rows; matches xterm behaviour). */
        for (int32_t i = rows; i < b->rows; ++i) sp_line_free(&b->visible[i]);
        sp_line* nv = (sp_line*)realloc(b->visible, (size_t)rows * sizeof(sp_line));
        if (nv) b->visible = nv;
    } else {
        sp_line* nv = (sp_line*)realloc(b->visible, (size_t)rows * sizeof(sp_line));
        if (!nv) { sp_pool_swap_out(prev); return; }
        b->visible = nv;
        for (int32_t i = b->rows; i < rows; ++i) {
            sp_line_init(&b->visible[i], cols);
        }
    }
    b->rows = rows;
    sp_pool_swap_out(prev);

    /* URI grid: reallocate to new dimensions (dropping all tags on resize
     * — the old content is reflowed anyway so coordinates would be stale). */
    free(b->uri_grid);
    b->uri_grid = (uint16_t*)calloc((size_t)b->rows * (size_t)b->cols, sizeof(uint16_t));
    /* Scroll region: reset to the full new screen. An explicitly set
     * region from before the resize is stale either way (xterm does
     * the same — apps re-send DECSTBM after SIGWINCH). Always resetting
     * also keeps `scroll_up` able to push into scrollback when the
     * caller never touched DECSTBM. */
    b->scroll_top = 0;
    b->scroll_bot = b->rows - 1;
}

static void sp_buf_uri_zero_row(sp_buffer* b, int32_t row) {
    if (!b || !b->uri_grid || row < 0 || row >= b->rows) return;
    memset(b->uri_grid + (size_t)row * (size_t)b->cols, 0,
           (size_t)b->cols * sizeof(uint16_t));
}

static void sp_buf_uri_zero_cells(sp_buffer* b, int32_t row,
                                  int32_t col_start, int32_t col_end) {
    if (!b || !b->uri_grid || row < 0 || row >= b->rows) return;
    if (col_start < 0) col_start = 0;
    if (col_end > b->cols) col_end = b->cols;
    if (col_end <= col_start) return;
    memset(b->uri_grid + (size_t)row * (size_t)b->cols + col_start, 0,
           (size_t)(col_end - col_start) * sizeof(uint16_t));
}

void sp_buffer_clear(sp_buffer* b, sp_cell_attrs def) {
    if (!b) return;
    for (int32_t i = 0; i < b->rows; ++i) sp_line_clear(&b->visible[i], def);
    if (b->uri_grid)
        memset(b->uri_grid, 0,
               (size_t)b->rows * (size_t)b->cols * sizeof(uint16_t));
}

void sp_buffer_clear_line(sp_buffer* b, int32_t row, sp_cell_attrs def) {
    sp_line* ln = sp_buffer_line(b, row);
    if (ln) sp_line_clear(ln, def);
    sp_buf_uri_zero_row(b, row);
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
        sp_buf_uri_zero_cells(b, r, s, e);
    }
}

void sp_buffer_scroll_up(sp_buffer* b, int32_t count, sp_cell_attrs def) {
    if (!b || count <= 0) return;
    int32_t top = b->scroll_top, bot = b->scroll_bot;
    int32_t region_rows = bot - top + 1;
    if (count > region_rows) count = region_rows;
    /* xterm: only full-screen scroll carries into the scrollback. A region
     * that starts below row 0 drops the evicted lines on the floor. */
    const bool full = (top == 0 && bot == b->rows - 1);
    for (int32_t i = 0; i < count; ++i) {
        if (full) {
            sp_line out = b->visible[0];
            sb_push(&b->sb, out);
        } else {
            sp_line_free(&b->visible[top]);
        }
        for (int32_t r = top + 1; r <= bot; ++r) {
            b->visible[r - 1] = b->visible[r];
        }
        sp_line nl;
        sp_line_init(&nl, b->cols);
        for (int32_t c = 0; c < b->cols; ++c) nl.cells[c].attrs = def;
        b->visible[bot] = nl;
    }
    if (b->uri_grid) {
        size_t stride = (size_t)b->cols;
        size_t keep_rows = (size_t)(region_rows - count);
        memmove(b->uri_grid + (size_t)top * stride,
                b->uri_grid + (size_t)(top + count) * stride,
                keep_rows * stride * sizeof(uint16_t));
        memset(b->uri_grid + (size_t)(bot + 1 - count) * stride, 0,
               (size_t)count * stride * sizeof(uint16_t));
    }
    sp_buffer_mark_all_dirty(b);
}

void sp_buffer_scroll_down(sp_buffer* b, int32_t count, sp_cell_attrs def) {
    if (!b || count <= 0) return;
    int32_t top = b->scroll_top, bot = b->scroll_bot;
    int32_t region_rows = bot - top + 1;
    if (count > region_rows) count = region_rows;
    for (int32_t i = 0; i < count; ++i) {
        sp_line_free(&b->visible[bot]);
        for (int32_t r = bot; r > top; --r) {
            b->visible[r] = b->visible[r - 1];
        }
        sp_line nl;
        sp_line_init(&nl, b->cols);
        for (int32_t c = 0; c < b->cols; ++c) nl.cells[c].attrs = def;
        b->visible[top] = nl;
    }
    if (b->uri_grid) {
        size_t stride = (size_t)b->cols;
        size_t keep_rows = (size_t)(region_rows - count);
        memmove(b->uri_grid + (size_t)(top + count) * stride,
                b->uri_grid + (size_t)top * stride,
                keep_rows * stride * sizeof(uint16_t));
        memset(b->uri_grid + (size_t)top * stride, 0,
               (size_t)count * stride * sizeof(uint16_t));
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
    if (b->uri_grid) {
        size_t stride = (size_t)b->cols;
        size_t moved = (size_t)(b->rows - row - count) * stride;
        memmove(b->uri_grid + (size_t)(row + count) * stride,
                b->uri_grid + (size_t)row * stride,
                moved * sizeof(uint16_t));
        memset(b->uri_grid + (size_t)row * stride, 0,
               (size_t)count * stride * sizeof(uint16_t));
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
    if (b->uri_grid) {
        size_t stride = (size_t)b->cols;
        size_t moved = (size_t)(b->rows - row - count) * stride;
        memmove(b->uri_grid + (size_t)row * stride,
                b->uri_grid + (size_t)(row + count) * stride,
                moved * sizeof(uint16_t));
        memset(b->uri_grid + (size_t)(b->rows - count) * stride, 0,
               (size_t)count * stride * sizeof(uint16_t));
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
    if (b->uri_grid) {
        uint16_t* r = b->uri_grid + (size_t)row * (size_t)b->cols;
        for (int32_t i = ln->cols - 1; i >= col + count; --i) r[i] = r[i - count];
        for (int32_t i = col; i < col + count; ++i) r[i] = 0;
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
    if (b->uri_grid) {
        uint16_t* r = b->uri_grid + (size_t)row * (size_t)b->cols;
        for (int32_t i = col; i < ln->cols - count; ++i) r[i] = r[i + count];
        for (int32_t i = ln->cols - count; i < ln->cols; ++i) r[i] = 0;
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

void sp_buffer_set_uri(sp_buffer* b, int32_t row, int32_t col, uint16_t id) {
    if (!b || !b->uri_grid || row < 0 || row >= b->rows ||
        col < 0 || col >= b->cols) return;
    b->uri_grid[(size_t)row * (size_t)b->cols + (size_t)col] = id;
}

uint16_t sp_buffer_get_uri(const sp_buffer* b, int32_t row, int32_t col) {
    if (!b || !b->uri_grid || row < 0 || row >= b->rows ||
        col < 0 || col >= b->cols) return 0;
    return b->uri_grid[(size_t)row * (size_t)b->cols + (size_t)col];
}

void sp_buffer_clear_uris(sp_buffer* b) {
    if (!b || !b->uri_grid) return;
    memset(b->uri_grid, 0, (size_t)b->rows * (size_t)b->cols * sizeof(uint16_t));
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

size_t sp_buffer_scroll_offset(const sp_buffer* b) {
    return b ? b->scroll_offset : 0;
}
size_t sp_buffer_scroll_max(const sp_buffer* b) {
    return b ? b->sb.size : 0;
}
void sp_buffer_scroll_to(sp_buffer* b, size_t offset) {
    if (!b) return;
    if (offset > b->sb.size) offset = b->sb.size;
    if (b->scroll_offset != offset) {
        b->scroll_offset = offset;
        sp_buffer_mark_all_dirty(b);
    }
}
void sp_buffer_scroll_by(sp_buffer* b, int64_t delta) {
    if (!b) return;
    int64_t cur = (int64_t)b->scroll_offset + delta;
    if (cur < 0) cur = 0;
    if ((uint64_t)cur > (uint64_t)b->sb.size) cur = (int64_t)b->sb.size;
    sp_buffer_scroll_to(b, (size_t)cur);
}
void sp_buffer_scroll_reset(sp_buffer* b) {
    sp_buffer_scroll_to(b, 0);
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
