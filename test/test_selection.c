/*
 * test_selection.c — selection manager black-box tests via sp_terminal.
 */
#include "test_framework.h"
#include "spiritty/spiritty.h"

#include <stdlib.h>
#include <string.h>

static sp_terminal* mk(int32_t cols, int32_t rows) {
    sp_options o; sp_options_default(&o);
    o.cols = cols; o.rows = rows; o.scrollback_lines = 0;
    return sp_terminal_create(&o);
}

static void feed(sp_terminal* t, const char* s) {
    sp_terminal_write(t, (const uint8_t*)s, strlen(s));
}

SP_TEST(selection, normal_single_row) {
    sp_terminal* t = mk(10, 2);
    feed(t, "Hello");
    sp_terminal_selection_begin(t, 0, 0, SP_SEL_NORMAL);
    sp_terminal_selection_extend(t, 0, 4);
    char* s = sp_terminal_selection_text(t);
    ASSERT_TRUE(s != NULL);
    ASSERT_STR_EQ(s, "Hello");
    sp_string_free(s);
    sp_terminal_destroy(t);
}

SP_TEST(selection, normal_two_rows_concatenated_with_newline) {
    /* 10 cols so the 5-char strings don't trigger autowrap onto a third row.
     * Note "\n" is LF only (no implicit CR), so "FGHIJ" lands at row 1 cols
     * 5..9 (continuing from where the cursor was after "ABCDE"). */
    sp_terminal* t = mk(10, 3);
    feed(t, "ABCDE\r\nFGHIJ");
    sp_terminal_selection_begin(t, 0, 0, SP_SEL_NORMAL);
    sp_terminal_selection_extend(t, 1, 4);
    char* s = sp_terminal_selection_text(t);
    ASSERT_TRUE(s != NULL);
    /* Row 0 padded to full width since it isn't the end-row of the
     * selection; row 1 stops at col 4 (inclusive). */
    ASSERT_STR_EQ(s, "ABCDE     \nFGHIJ");
    sp_string_free(s);
    sp_terminal_destroy(t);
}

SP_TEST(selection, word_mode_expands_to_word_boundaries) {
    sp_terminal* t = mk(20, 2);
    feed(t, "  hello world  ");
    /* Click somewhere inside "hello" (col 3) */
    sp_terminal_selection_begin(t, 0, 3, SP_SEL_WORD);
    char* s = sp_terminal_selection_text(t);
    ASSERT_TRUE(s != NULL);
    ASSERT_STR_EQ(s, "hello");
    sp_string_free(s);
    sp_terminal_destroy(t);
}

SP_TEST(selection, line_mode_selects_full_row) {
    sp_terminal* t = mk(10, 3);
    feed(t, "Hello\nWorld");
    sp_terminal_selection_begin(t, 0, 2, SP_SEL_LINE);
    char* s = sp_terminal_selection_text(t);
    ASSERT_TRUE(s != NULL);
    /* SP_SEL_LINE expands cols to [0, cols-1] inclusive, so we get the
     * whole row, padded out with spaces from the blank tail. */
    ASSERT_STR_EQ(s, "Hello     ");
    sp_string_free(s);
    sp_terminal_destroy(t);
}

SP_TEST(selection, clear_returns_empty_text) {
    sp_terminal* t = mk(5, 1);
    feed(t, "Hello");
    sp_terminal_selection_begin(t, 0, 0, SP_SEL_NORMAL);
    sp_terminal_selection_extend(t, 0, 4);
    sp_terminal_selection_clear(t);
    char* s = sp_terminal_selection_text(t);
    ASSERT_TRUE(s != NULL);
    ASSERT_STR_EQ(s, "");
    sp_string_free(s);
    sp_terminal_destroy(t);
}

SP_TEST(selection, reverse_extension_normalised) {
    /* User clicks at col 4, drags back to col 0 — text should still come out
     * in reading order. */
    sp_terminal* t = mk(10, 1);
    feed(t, "Hello");
    sp_terminal_selection_begin(t, 0, 4, SP_SEL_NORMAL);
    sp_terminal_selection_extend(t, 0, 0);
    char* s = sp_terminal_selection_text(t);
    ASSERT_TRUE(s != NULL);
    ASSERT_STR_EQ(s, "Hello");
    sp_string_free(s);
    sp_terminal_destroy(t);
}
