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

/* ---- Alt-screen (DECSET 1049) ----------------------------------------- */

SP_TEST(parser, decset_1049_saves_and_restores_cursor) {
    sp_terminal* t = mk_term(10, 10);
    feed(t, "\x1b[5;7H");            /* row 4, col 6 (0-based 4,6) */
    feed(t, "\x1b[?1049h");          /* enter alt, save cursor, home */
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(c, 0);
    feed(t, "\x1b[?1049l");          /* leave alt, restore cursor */
    sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 4);
    ASSERT_EQ_INT(c, 6);
    sp_terminal_destroy(t);
}

SP_TEST(parser, decset_1049_isolates_screens) {
    sp_terminal* t = mk_term(10, 3);
    feed(t, "AAA");                  /* primary row 0 = "AAA" */
    feed(t, "\x1b[?1049h");          /* enter alt; alt is blank */
    /* Alt cell (0,0) should be empty (space or zero), not 'A'. */
    ASSERT_NE_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'A');
    feed(t, "BBB");                  /* alt row 0 = "BBB" */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'B');
    feed(t, "\x1b[?1049l");          /* leave alt */
    /* Primary unchanged — "AAA" still at row 0. */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 2)->codepoint, (uint32_t)'A');
    sp_terminal_destroy(t);
}

/* ---- DECSTBM scroll region -------------------------------------------- */

SP_TEST(parser, decstbm_homes_cursor) {
    sp_terminal* t = mk_term(10, 10);
    feed(t, "\x1b[5;5H");
    feed(t, "\x1b[3;8r");            /* region rows 3..8, cursor homes */
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(c, 0);
    sp_terminal_destroy(t);
}

SP_TEST(parser, decstbm_scrolls_only_region) {
    sp_terminal* t = mk_term(5, 5);
    /* Fill rows 0..4 with row-marker letters A..E. */
    feed(t, "A\r\nB\r\nC\r\nD\r\nE");
    /* Region rows 3..5 (1-based) = internal rows 2..4. */
    feed(t, "\x1b[3;5r");
    /* Move to last row of region, feed LF → region scrolls up; rows 0/1
     * (A, B) must remain intact. */
    feed(t, "\x1b[5;1H\n");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 1, 0)->codepoint, (uint32_t)'B');
    /* Row 2 of screen (top of region) was 'C' — now 'D' after scroll. */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 2, 0)->codepoint, (uint32_t)'D');
    sp_terminal_destroy(t);
}

/* ---- CJK wide cells --------------------------------------------------- */

SP_TEST(parser, cjk_hiragana_occupies_two_cells) {
    sp_terminal* t = mk_term(10, 1);
    /* あ = U+3042 (Hiragana A) — 3 UTF-8 bytes. */
    feed(t, "\xE3\x81\x82");
    const sp_cell* c0 = sp_terminal_cell_at(t, 0, 0);
    const sp_cell* c1 = sp_terminal_cell_at(t, 0, 1);
    ASSERT_EQ_INT(c0->codepoint, 0x3042u);
    ASSERT_EQ_INT(c0->width,     2);
    /* Continuation cell: width 0, codepoint 0. */
    ASSERT_EQ_INT(c1->codepoint, 0u);
    ASSERT_EQ_INT(c1->width,     0);
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(c, 2);         /* cursor advanced by TWO */
    sp_terminal_destroy(t);
}

SP_TEST(parser, cjk_wide_wraps_at_right_margin) {
    sp_terminal* t = mk_term(3, 3);
    /* 2 narrow chars at cols 0 and 1 leave only col 2. A wide char at
     * col 2 doesn't fit, must wrap. Row 0 col 2 becomes blank, wide
     * lands at row 1 col 0. */
    feed(t, "ab");
    feed(t, "\xE3\x81\x82");     /* あ */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'a');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->codepoint, (uint32_t)'b');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 2)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 1, 0)->codepoint, 0x3042u);
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 1, 0)->width,     2);
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 1, 1)->width,     0);
    sp_terminal_destroy(t);
}

SP_TEST(parser, cjk_cjk_unified_ideograph) {
    sp_terminal* t = mk_term(5, 1);
    /* 美 = U+7F8E (CJK Unified) — 3 UTF-8 bytes 0xE7 0xBE 0x8E */
    feed(t, "\xE7\xBE\x8E");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, 0x7F8Eu);
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->width,     2);
    sp_terminal_destroy(t);
}

SP_TEST(parser, cjk_narrow_over_wide_left_clears_continuation) {
    sp_terminal* t = mk_term(5, 1);
    /* Write 'あ' at col 0 — occupies cols 0..1. */
    feed(t, "\xE3\x81\x82");
    /* Cursor is now at col 2. Move to col 0 and write 'X'. The
     * continuation at col 1 must become a plain space, not stay as
     * a width-0 orphan. */
    feed(t, "\x1b[1;1HX");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)'X');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->width,     1);
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->width,     1);
    sp_terminal_destroy(t);
}

SP_TEST(parser, cjk_narrow_over_wide_right_clears_leader) {
    sp_terminal* t = mk_term(5, 1);
    /* Write 'あ' at col 0 — occupies cols 0..1. */
    feed(t, "\xE3\x81\x82");
    /* Move to col 1 (the continuation cell) and write 'X'. The leader
     * 'あ' at col 0 is now orphaned — must become a plain space. */
    feed(t, "\x1b[1;2HX");
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->width,     1);
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->codepoint, (uint32_t)'X');
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 1)->width,     1);
    sp_terminal_destroy(t);
}
