/*
 * test_buffer.c — exercises sp_line and sp_buffer.
 *
 * These tests reach into `internal.h` to manipulate sp_line/sp_buffer
 * directly because the public API only exposes these via sp_terminal.
 * Once Phase 3 lands, equivalent black-box tests live in test_terminal.c.
 */
#include "test_framework.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- sp_line ----------------------------------------------------------- */

static sp_cell_attrs default_attrs(void) {
    sp_cell_attrs a = { .fg = 0xFFFFFFFFu, .bg = 0x000000FFu, .flags = 0,
                        .underline_style = 0, ._pad = 0 };
    return a;
}

SP_TEST(buffer, line_init_sets_width_and_dirty) {
    sp_line line;
    sp_line_init(&line, 80);
    ASSERT_EQ_INT(line.cols, 80);
    ASSERT_TRUE(line.dirty);
    ASSERT_TRUE(line.cells != NULL);
    sp_line_free(&line);
}

SP_TEST(buffer, line_clear_fills_with_spaces) {
    sp_line line;
    sp_line_init(&line, 10);
    sp_line_clear(&line, default_attrs());
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ_INT(line.cells[i].codepoint, (uint32_t)' ');
    }
    sp_line_free(&line);
}

SP_TEST(buffer, line_resize_preserves_existing) {
    sp_line line;
    sp_line_init(&line, 5);
    sp_line_clear(&line, default_attrs());
    line.cells[0].codepoint = (uint32_t)'A';
    line.cells[4].codepoint = (uint32_t)'E';
    sp_line_resize(&line, 8, default_attrs());
    ASSERT_EQ_INT(line.cols, 8);
    ASSERT_EQ_INT(line.cells[0].codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(line.cells[4].codepoint, (uint32_t)'E');
    /* New cells initialised to space */
    ASSERT_EQ_INT(line.cells[5].codepoint, (uint32_t)' ');
    sp_line_free(&line);
}

SP_TEST(buffer, line_text_round_trips_ascii) {
    sp_line line;
    sp_line_init(&line, 5);
    sp_line_clear(&line, default_attrs());
    const char* hello = "Hello";
    for (int i = 0; i < 5; ++i) line.cells[i].codepoint = (uint32_t)hello[i];
    char* s = sp_line_text(&line);
    ASSERT_TRUE(s != NULL);
    ASSERT_STR_EQ(s, "Hello");
    free(s);
    sp_line_free(&line);
}

SP_TEST(buffer, line_text_range_subsets_correctly) {
    sp_line line;
    sp_line_init(&line, 5);
    sp_line_clear(&line, default_attrs());
    const char* h = "Hello";
    for (int i = 0; i < 5; ++i) line.cells[i].codepoint = (uint32_t)h[i];
    /* Half-open [start, end): cells 1..3 → "ell". */
    char* s = sp_line_text_range(&line, 1, 4);
    ASSERT_TRUE(s != NULL);
    ASSERT_STR_EQ(s, "ell");
    free(s);
    sp_line_free(&line);
}

SP_TEST(buffer, line_clear_range_zaps_only_specified_cells) {
    sp_line line;
    sp_line_init(&line, 5);
    sp_line_clear(&line, default_attrs());
    const char* h = "ABCDE";
    for (int i = 0; i < 5; ++i) line.cells[i].codepoint = (uint32_t)h[i];
    /* Half-open [1, 4): clears cells 1, 2, 3. */
    sp_line_clear_range(&line, 1, 4, default_attrs());
    ASSERT_EQ_INT(line.cells[0].codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(line.cells[1].codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(line.cells[2].codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(line.cells[3].codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(line.cells[4].codepoint, (uint32_t)'E');
    sp_line_free(&line);
}

SP_TEST(buffer, codepoint_is_word_classifies_correctly) {
    ASSERT_TRUE(sp_codepoint_is_word('a'));
    ASSERT_TRUE(sp_codepoint_is_word('Z'));
    ASSERT_TRUE(sp_codepoint_is_word('0'));
    ASSERT_TRUE(sp_codepoint_is_word('_'));
    ASSERT_FALSE(sp_codepoint_is_word(' '));
    ASSERT_FALSE(sp_codepoint_is_word('.'));
    ASSERT_FALSE(sp_codepoint_is_word(','));
}

/* ---- sp_buffer --------------------------------------------------------- */

SP_TEST(buffer, buffer_create_has_correct_dims) {
    sp_buffer* b = sp_buffer_create(24, 80, 1000);
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ_INT(sp_buffer_rows(b), 24);
    ASSERT_EQ_INT(sp_buffer_cols(b), 80);
    sp_buffer_destroy(b);
}

SP_TEST(buffer, buffer_lines_are_initialised_blank) {
    sp_buffer* b = sp_buffer_create(3, 4, 0);
    sp_buffer_clear(b, default_attrs());
    for (int r = 0; r < 3; ++r) {
        const sp_line* ln = sp_buffer_line_const(b, r);
        ASSERT_TRUE(ln != NULL);
        for (int c = 0; c < 4; ++c) {
            ASSERT_EQ_INT(ln->cells[c].codepoint, (uint32_t)' ');
        }
    }
    sp_buffer_destroy(b);
}

SP_TEST(buffer, buffer_write_cp_lands_in_cell) {
    sp_buffer* b = sp_buffer_create(3, 4, 0);
    sp_buffer_clear(b, default_attrs());
    sp_buffer_write_cp(b, 1, 2, (uint32_t)'X', default_attrs());
    ASSERT_EQ_INT(sp_buffer_cell(b, 1, 2)->codepoint, (uint32_t)'X');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 0)->codepoint, (uint32_t)' ');
    sp_buffer_destroy(b);
}

SP_TEST(buffer, buffer_scroll_up_pushes_to_scrollback) {
    sp_buffer* b = sp_buffer_create(3, 4, 100);
    sp_buffer_clear(b, default_attrs());
    /* Row 0 = "AAAA" so we can recognise it in scrollback. */
    for (int c = 0; c < 4; ++c)
        sp_buffer_write_cp(b, 0, c, (uint32_t)'A', default_attrs());
    ASSERT_EQ_INT((int)sp_buffer_scrollback_size(b), 0);
    sp_buffer_scroll_up(b, 1, default_attrs());
    ASSERT_EQ_INT((int)sp_buffer_scrollback_size(b), 1);
    /* Top row should now be blank */
    const sp_line* row0 = sp_buffer_line_const(b, 0);
    ASSERT_EQ_INT(row0->cells[0].codepoint, (uint32_t)' ');
    /* Scrollback entry 0 (most recent) should be the AAAA row */
    const sp_line* sb = sp_buffer_scrollback_at(b, 0);
    ASSERT_TRUE(sb != NULL);
    ASSERT_EQ_INT(sb->cells[0].codepoint, (uint32_t)'A');
    sp_buffer_destroy(b);
}

SP_TEST(buffer, buffer_scrollback_caps_at_max) {
    sp_buffer* b = sp_buffer_create(2, 2, 3);
    sp_buffer_clear(b, default_attrs());
    /* Push 5 rows out — should keep only 3 most recent. */
    for (int i = 0; i < 5; ++i) {
        sp_buffer_write_cp(b, 0, 0, (uint32_t)('0' + i), default_attrs());
        sp_buffer_scroll_up(b, 1, default_attrs());
    }
    ASSERT_EQ_INT((int)sp_buffer_scrollback_size(b), 3);
    sp_buffer_destroy(b);
}

SP_TEST(buffer, buffer_resize_grows_and_shrinks) {
    sp_buffer* b = sp_buffer_create(3, 4, 0);
    sp_buffer_clear(b, default_attrs());
    sp_buffer_resize(b, 5, 6, default_attrs());
    ASSERT_EQ_INT(sp_buffer_rows(b), 5);
    ASSERT_EQ_INT(sp_buffer_cols(b), 6);
    /* New rows initialised */
    ASSERT_EQ_INT(sp_buffer_cell(b, 4, 5)->codepoint, (uint32_t)' ');
    sp_buffer_resize(b, 2, 3, default_attrs());
    ASSERT_EQ_INT(sp_buffer_rows(b), 2);
    ASSERT_EQ_INT(sp_buffer_cols(b), 3);
    sp_buffer_destroy(b);
}

SP_TEST(buffer, buffer_insert_chars_shifts_right) {
    sp_buffer* b = sp_buffer_create(1, 5, 0);
    sp_buffer_clear(b, default_attrs());
    const char* s = "ABCDE";
    for (int c = 0; c < 5; ++c)
        sp_buffer_write_cp(b, 0, c, (uint32_t)s[c], default_attrs());
    sp_buffer_insert_chars(b, 0, 1, 2, default_attrs());
    /* Expect "A  BC" — D and E pushed off the right margin. */
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 1)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 2)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 3)->codepoint, (uint32_t)'B');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 4)->codepoint, (uint32_t)'C');
    sp_buffer_destroy(b);
}

SP_TEST(buffer, buffer_delete_chars_shifts_left) {
    sp_buffer* b = sp_buffer_create(1, 5, 0);
    sp_buffer_clear(b, default_attrs());
    const char* s = "ABCDE";
    for (int c = 0; c < 5; ++c)
        sp_buffer_write_cp(b, 0, c, (uint32_t)s[c], default_attrs());
    sp_buffer_delete_chars(b, 0, 1, 2, default_attrs());
    /* Expect "ADE  " — BC removed, DE shifted, blanks at end. */
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 0)->codepoint, (uint32_t)'A');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 1)->codepoint, (uint32_t)'D');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 2)->codepoint, (uint32_t)'E');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 3)->codepoint, (uint32_t)' ');
    ASSERT_EQ_INT(sp_buffer_cell(b, 0, 4)->codepoint, (uint32_t)' ');
    sp_buffer_destroy(b);
}
