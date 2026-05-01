/*
 * test_parser.c — black-box tests for the ANSI/VT100 parser through
 * sp_terminal_write(). Anything we can express as bytes goes here; the
 * parser is exercised via the public API the way real consumers will.
 */
#include "test_framework.h"
#include "spiritty/spiritty.h"

#include <string.h>

static sp_terminal* mk_term(int32_t cols, int32_t rows) {
    sp_options o; sp_options_default(&o);
    o.cols = cols; o.rows = rows; o.scrollback_lines = 100;
    return sp_terminal_create(&o);
}

static void feed(sp_terminal* t, const char* s) {
    sp_terminal_write(t, (const uint8_t*)s, strlen(s));
}

/* ---- printable text ---------------------------------------------------- */

SP_TEST(parser, prints_ascii_in_order) {
    sp_terminal* t = mk_term(10, 3);
    feed(t, "Hello");
    const sp_cell* c0 = sp_terminal_cell_at(t, 0, 0);
    const sp_cell* c4 = sp_terminal_cell_at(t, 0, 4);
    ASSERT_EQ_INT(c0->codepoint, (uint32_t)'H');
    ASSERT_EQ_INT(c4->codepoint, (uint32_t)'o');
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(c, 5);
    sp_terminal_destroy(t);
}

SP_TEST(parser, autowrap_to_next_row) {
    sp_terminal* t = mk_term(4, 3);
    feed(t, "ABCDE");
    /* Row 0 = "ABCD", row 1 = "E_..." */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 3)->codepoint, (uint32_t)'D');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 1, 0)->codepoint, (uint32_t)'E');
    sp_terminal_destroy(t);
}

SP_TEST(parser, lf_advances_row) {
    sp_terminal* t = mk_term(10, 3);
    feed(t, "A\nB");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 1, 1)->codepoint, (uint32_t)'B');
    sp_terminal_destroy(t);
}

SP_TEST(parser, cr_resets_column) {
    sp_terminal* t = mk_term(10, 3);
    feed(t, "ABC\rXY");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'X');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->codepoint, (uint32_t)'Y');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 2)->codepoint, (uint32_t)'C');
    sp_terminal_destroy(t);
}

SP_TEST(parser, backspace_decrements_column) {
    sp_terminal* t = mk_term(10, 3);
    feed(t, "AB\bC");
    /* B was at col 1; \b → col 1; C overwrites B. */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->codepoint, (uint32_t)'C');
    sp_terminal_destroy(t);
}

SP_TEST(parser, tab_advances_to_next_8col_stop) {
    sp_terminal* t = mk_term(20, 3);
    feed(t, "A\tB");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 8)->codepoint, (uint32_t)'B');
    sp_terminal_destroy(t);
}

/* ---- CSI cursor movement ---------------------------------------------- */

SP_TEST(parser, csi_cup_moves_cursor) {
    sp_terminal* t = mk_term(10, 5);
    feed(t, "\x1b[3;5H");
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 2);
    ASSERT_EQ_INT(c, 4);
    sp_terminal_destroy(t);
}

SP_TEST(parser, csi_cup_clamps_to_bounds) {
    sp_terminal* t = mk_term(10, 5);
    feed(t, "\x1b[99;99H");
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 4);
    ASSERT_EQ_INT(c, 9);
    sp_terminal_destroy(t);
}

SP_TEST(parser, csi_cuu_cud_cuf_cub) {
    sp_terminal* t = mk_term(10, 5);
    feed(t, "\x1b[3;5H");           /* row 2, col 4 */
    feed(t, "\x1b[2A");              /* up 2 */
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0); ASSERT_EQ_INT(c, 4);
    feed(t, "\x1b[3B");              /* down 3 */
    sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 3); ASSERT_EQ_INT(c, 4);
    feed(t, "\x1b[2C");              /* right 2 */
    sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 3); ASSERT_EQ_INT(c, 6);
    feed(t, "\x1b[5D");              /* left 5 */
    sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 3); ASSERT_EQ_INT(c, 1);
    sp_terminal_destroy(t);
}

/* ---- CSI erase --------------------------------------------------------- */

SP_TEST(parser, csi_ed_2_clears_screen) {
    sp_terminal* t = mk_term(5, 3);
    feed(t, "ABCDE\nFGHIJ");
    feed(t, "\x1b[2J");
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 5; ++c)
            ASSERT_EQ_INT(sp_terminal_cell_at(t, r, c)->codepoint, (uint32_t)' ');
    sp_terminal_destroy(t);
}

SP_TEST(parser, csi_el_0_clears_to_eol) {
    sp_terminal* t = mk_term(5, 3);
    feed(t, "ABCDE");
    feed(t, "\x1b[1;3H");          /* cursor to row 1, col 3 (i.e. 'C') */
    feed(t, "\x1b[K");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->codepoint, (uint32_t)'B');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 2)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 3)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 4)->codepoint, (uint32_t)' ');
    sp_terminal_destroy(t);
}

/* ---- SGR --------------------------------------------------------------- */

SP_TEST(parser, csi_sgr_bold_sets_attr_flag) {
    sp_terminal* t = mk_term(5, 1);
    feed(t, "\x1b[1mA");
    const sp_cell* c = sp_terminal_cell_at(t, 0, 0);
    ASSERT_TRUE((c->attrs.flags & SP_ATTR_BOLD) != 0);
    sp_terminal_destroy(t);
}

SP_TEST(parser, csi_sgr_reset_clears_attrs) {
    sp_terminal* t = mk_term(5, 1);
    feed(t, "\x1b[1;4mA\x1b[0mB");
    const sp_cell* a = sp_terminal_cell_at(t, 0, 0);
    const sp_cell* b = sp_terminal_cell_at(t, 0, 1);
    ASSERT_TRUE((a->attrs.flags & SP_ATTR_BOLD) != 0);
    ASSERT_EQ_INT(b->attrs.flags, 0);
    sp_terminal_destroy(t);
}

SP_TEST(parser, csi_sgr_truecolor_fg) {
    sp_terminal* t = mk_term(5, 1);
    feed(t, "\x1b[38;2;200;100;50mA");
    const sp_cell* c = sp_terminal_cell_at(t, 0, 0);
    /* 0xRRGGBBAA: 200=0xC8, 100=0x64, 50=0x32, alpha=0xFF */
    ASSERT_EQ_INT(c->attrs.fg, 0xC86432FFu);
    sp_terminal_destroy(t);
}

SP_TEST(parser, csi_sgr_256color_fg) {
    sp_terminal* t = mk_term(5, 1);
    /* index 196 in xterm 256 = bright red-ish (cube 5,0,0) = 0xFF0000FF */
    feed(t, "\x1b[38;5;196mA");
    const sp_cell* c = sp_terminal_cell_at(t, 0, 0);
    ASSERT_EQ_INT(c->attrs.fg, 0xFF0000FFu);
    sp_terminal_destroy(t);
}

/* ---- DECSET / DECRST --------------------------------------------------- */

SP_TEST(parser, decset_25_makes_cursor_visible) {
    sp_terminal* t = mk_term(5, 1);
    feed(t, "\x1b[?25l");           /* hide */
    /* No public introspection; just verify it doesn't crash & DECSET 25h restores. */
    feed(t, "\x1b[?25h");
    sp_terminal_destroy(t);
}

/* ---- OSC --------------------------------------------------------------- */

static const char* g_last_title = NULL;
static void title_cb(const sp_event* ev, void* user) {
    (void)user;
    if (ev->kind == SP_EV_TITLE) g_last_title = ev->as.title.text;
}

SP_TEST(parser, osc_2_sets_title_via_bel) {
    sp_terminal* t = mk_term(10, 3);
    g_last_title = NULL;
    sp_terminal_set_event_cb(t, title_cb, NULL);
    feed(t, "\x1b]2;Hello World\x07");
    ASSERT_TRUE(g_last_title != NULL);
    ASSERT_STR_EQ(g_last_title, "Hello World");
    sp_terminal_destroy(t);
}

SP_TEST(parser, osc_2_sets_title_via_st) {
    sp_terminal* t = mk_term(10, 3);
    g_last_title = NULL;
    sp_terminal_set_event_cb(t, title_cb, NULL);
    feed(t, "\x1b]0;Title\x1b\\");
    ASSERT_TRUE(g_last_title != NULL);
    ASSERT_STR_EQ(g_last_title, "Title");
    sp_terminal_destroy(t);
}

/* ---- ESC dispatch ----------------------------------------------------- */

SP_TEST(parser, esc_save_restore_cursor) {
    sp_terminal* t = mk_term(10, 5);
    feed(t, "\x1b[3;5H");          /* row 2, col 4 */
    feed(t, "\x1b""7");              /* DECSC */
    feed(t, "\x1b[1;1H");          /* origin */
    feed(t, "\x1b""8");              /* DECRC */
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 2);
    ASSERT_EQ_INT(c, 4);
    sp_terminal_destroy(t);
}

/* ---- UTF-8 ------------------------------------------------------------- */

SP_TEST(parser, utf8_two_byte_decoded) {
    sp_terminal* t = mk_term(5, 1);
    feed(t, "\xC3\xA9");             /* é = U+00E9 */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, 0xE9u);
    sp_terminal_destroy(t);
}

SP_TEST(parser, utf8_three_byte_decoded) {
    sp_terminal* t = mk_term(5, 1);
    feed(t, "\xE2\x98\x83");         /* ☃ = U+2603 */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, 0x2603u);
    sp_terminal_destroy(t);
}

/* ---- Robustness ------------------------------------------------------- */

SP_TEST(parser, malformed_csi_returns_to_ground) {
    sp_terminal* t = mk_term(10, 1);
    feed(t, "\x1b[\x05X");           /* CSI then bogus byte → bail to ground; X printed */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'X');
    sp_terminal_destroy(t);
}
