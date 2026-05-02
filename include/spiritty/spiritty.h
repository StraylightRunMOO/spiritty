/*
 * spiritty.h — public C11 API for the Spiritty terminal core.
 *
 * This is the only stable surface. The C++17 facade in spiritty.hpp,
 * the Embind layer in bindings/embind.cpp, and the renderer backends
 * all sit on top of these declarations.
 *
 * Layout invariants for hot structs are enforced via _Static_assert.
 */
#ifndef SPIRITTY_H
#define SPIRITTY_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- C/C++ compatibility shims ------------------------------------------ */

/* `restrict` is C99+; C++ has no standard equivalent. Compilers offer
 * extensions: GCC/Clang `__restrict__`, MSVC `__restrict`. Pick whichever
 * is available; fall back to nothing for unknown compilers. */
#ifndef SPIRITTY_RESTRICT
#  if defined(__cplusplus)
#    if defined(__GNUC__) || defined(__clang__)
#      define SPIRITTY_RESTRICT __restrict__
#    elif defined(_MSC_VER)
#      define SPIRITTY_RESTRICT __restrict
#    else
#      define SPIRITTY_RESTRICT
#    endif
#  else
#    define SPIRITTY_RESTRICT restrict
#  endif
#endif

/* In C++ mode, _Static_assert / _Alignof are flagged by -Wpedantic. Use the
 * native C++11 keywords so the C++17 RAII facade compiles cleanly. */
#if defined(__cplusplus)
#  define SPIRITTY_STATIC_ASSERT(expr, msg) static_assert((expr), msg)
#  define SPIRITTY_ALIGNOF(T)               alignof(T)
#else
#  define SPIRITTY_STATIC_ASSERT(expr, msg) _Static_assert((expr), msg)
#  define SPIRITTY_ALIGNOF(T)               _Alignof(T)
#endif

/* ---- Visibility / linkage ----------------------------------------------- */

#if defined(_WIN32) || defined(__CYGWIN__)
#  define SPIRITTY_EXPORT __declspec(dllexport)
#  define SPIRITTY_IMPORT __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#  define SPIRITTY_EXPORT __attribute__((visibility("default")))
#  define SPIRITTY_IMPORT __attribute__((visibility("default")))
#else
#  define SPIRITTY_EXPORT
#  define SPIRITTY_IMPORT
#endif

#if defined(SPIRITTY_BUILDING)
#  define SPIRITTY_API SPIRITTY_EXPORT
#else
#  define SPIRITTY_API SPIRITTY_IMPORT
#endif

/* ---- Forward-declared opaque handles ------------------------------------ */

typedef struct sp_terminal sp_terminal;
typedef struct sp_renderer sp_renderer;

/* ---- Cell attributes (packed; matches GPU vertex layout) ---------------- */

enum {
    SP_ATTR_BOLD          = 1u << 0,
    SP_ATTR_ITALIC        = 1u << 1,
    SP_ATTR_UNDERLINE     = 1u << 2,
    SP_ATTR_STRIKETHROUGH = 1u << 3,
    SP_ATTR_DIM           = 1u << 4,
    SP_ATTR_REVERSE       = 1u << 5,
    SP_ATTR_HIDDEN        = 1u << 6,
    SP_ATTR_BLINK         = 1u << 7
};

struct sp_cell_attrs {
    uint32_t fg;              /* 0xRRGGBBAA */
    uint32_t bg;
    uint16_t flags;           /* SP_ATTR_* bitfield */
    uint8_t  underline_style; /* 0=none, 1=single, 2=double, 3=curly */
    uint8_t  _pad;
};
typedef struct sp_cell_attrs sp_cell_attrs;
SPIRITTY_STATIC_ASSERT(sizeof(struct sp_cell_attrs) == 12, "sp_cell_attrs layout drift");

struct sp_cell {
    uint32_t             codepoint;
    uint8_t              width;   /* 1 or 2 (CJK / wide) */
    uint8_t              _pad[3];
    struct sp_cell_attrs attrs;
};
typedef struct sp_cell sp_cell;
/* 4 + 1 + 3 + 12 = 20 → struct trail pads to 4-byte align == 20.
 * Row buffers in buffer.c carry alignas(64) for cache-line discipline; we
 * deliberately do NOT pad individual cells to 16/32 bytes — that would
 * waste 50%+ of D-cache for a per-character payload. */
SPIRITTY_STATIC_ASSERT(sizeof(struct sp_cell) == 20, "sp_cell layout drift");
SPIRITTY_STATIC_ASSERT(SPIRITTY_ALIGNOF(struct sp_cell) >= 4, "sp_cell alignment drift");

/* ---- Configuration ----------------------------------------------------- */

typedef struct {
    int32_t  cols;
    int32_t  rows;
    int32_t  scrollback_lines;
    int32_t  font_size;
    const char* font_family;     /* may be NULL → "monospace" */
    uint32_t color_fg;
    uint32_t color_bg;
    uint32_t color_cursor;
    uint32_t color_selection;
    const uint32_t* palette;     /* may be NULL → built-in xterm 256-color */
    size_t          palette_len;
    bool     gpu_acceleration;
    bool     cursor_blink;
    int32_t  cursor_style;       /* 0=block, 1=underline, 2=bar */
    const char* custom_shader_path; /* may be NULL */
} sp_options;

SPIRITTY_API void sp_options_default(sp_options* out);

/* ---- Events ------------------------------------------------------------ */

typedef enum {
    SP_EV_DATA       = 0,  /* terminal wants to send bytes upstream */
    SP_EV_RESIZE     = 1,
    SP_EV_TITLE      = 2,
    SP_EV_BELL       = 3,
    SP_EV_CURSOR     = 4,
    SP_EV_SCROLL     = 5,
    SP_EV_SELECTION  = 6
} sp_event_kind;

typedef struct {
    sp_event_kind kind;
    union {
        struct { const uint8_t* bytes; size_t len; } data;
        struct { int32_t cols, rows; }               resize;
        struct { const char* text; }                 title;
        struct { int32_t row, col; bool visible; }   cursor;
        struct { int32_t top; }                      scroll;
    } as;
} sp_event;

typedef void (*sp_event_cb)(const sp_event* ev, void* user);

/* ---- Lifecycle --------------------------------------------------------- */

SPIRITTY_API sp_terminal* sp_terminal_create(const sp_options* opts);
SPIRITTY_API void         sp_terminal_destroy(sp_terminal* t);

SPIRITTY_API void         sp_terminal_attach_renderer(sp_terminal* t, sp_renderer* r);
SPIRITTY_API void         sp_terminal_set_event_cb(sp_terminal* t, sp_event_cb cb, void* user);

/* ---- I/O & state ------------------------------------------------------- */

SPIRITTY_API void sp_terminal_write(sp_terminal* SPIRITTY_RESTRICT t,
                                    const uint8_t* SPIRITTY_RESTRICT data,
                                    size_t len);
SPIRITTY_API void sp_terminal_resize(sp_terminal* t, int32_t cols, int32_t rows);
SPIRITTY_API void sp_terminal_clear(sp_terminal* t);
SPIRITTY_API void sp_terminal_reset(sp_terminal* t);
SPIRITTY_API void sp_terminal_render(sp_terminal* t);
SPIRITTY_API void sp_terminal_set_time(sp_terminal* t, float seconds);

SPIRITTY_API int32_t sp_terminal_cols(const sp_terminal* t);
SPIRITTY_API int32_t sp_terminal_rows(const sp_terminal* t);

/* ---- Cursor ------------------------------------------------------------ */

SPIRITTY_API void sp_terminal_move_cursor(sp_terminal* t, int32_t row, int32_t col);
SPIRITTY_API void sp_terminal_get_cursor(const sp_terminal* t,
                                         int32_t* row, int32_t* col);

/* ---- Cell access (read-only) ------------------------------------------ */

SPIRITTY_API const sp_cell* sp_terminal_cell_at(const sp_terminal* t,
                                                int32_t row, int32_t col);

/* ---- Selection & clipboard -------------------------------------------- */

typedef enum { SP_SEL_NONE = 0, SP_SEL_NORMAL, SP_SEL_WORD, SP_SEL_LINE } sp_sel_mode;

SPIRITTY_API void   sp_terminal_selection_begin(sp_terminal* t,
                                                int32_t row, int32_t col,
                                                sp_sel_mode mode);
SPIRITTY_API void   sp_terminal_selection_extend(sp_terminal* t,
                                                 int32_t row, int32_t col);
SPIRITTY_API void   sp_terminal_selection_clear(sp_terminal* t);

/* Returns NUL-terminated UTF-8 owned by the caller; free with sp_string_free. */
SPIRITTY_API char*  sp_terminal_selection_text(const sp_terminal* t);
SPIRITTY_API void   sp_string_free(char* s);

/* ---- Input forwarding (UTF-8 keys, mouse) ----------------------------- */

SPIRITTY_API void sp_terminal_send_key(sp_terminal* t,
                                       const char* utf8, size_t len);
SPIRITTY_API void sp_terminal_send_mouse(sp_terminal* t,
                                         int32_t row, int32_t col,
                                         int32_t button, bool pressed);

/* ---- Shader hot-swap (forwards to attached renderer) ------------------ */

SPIRITTY_API bool sp_terminal_set_custom_shader(sp_terminal* t,
                                                const char* glsl_source);

/* ---- Version ---------------------------------------------------------- */

SPIRITTY_API const char* sp_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPIRITTY_H */
