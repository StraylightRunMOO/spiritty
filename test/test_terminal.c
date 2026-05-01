/*
 * test_terminal.c — public-API surface tests for sp_terminal.
 */
#include "test_framework.h"
#include "spiritty/spiritty.h"

#include <string.h>

SP_TEST(terminal, options_default_sane) {
    sp_options o;
    sp_options_default(&o);
    ASSERT_EQ_INT(o.cols, 80);
    ASSERT_EQ_INT(o.rows, 24);
    ASSERT_TRUE(o.scrollback_lines >= 0);
    ASSERT_TRUE(o.font_size > 0);
}

SP_TEST(terminal, create_with_null_opts_uses_defaults) {
    sp_terminal* t = sp_terminal_create(NULL);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ_INT(sp_terminal_cols(t), 80);
    ASSERT_EQ_INT(sp_terminal_rows(t), 24);
    sp_terminal_destroy(t);
}

SP_TEST(terminal, create_with_custom_dims) {
    sp_options o; sp_options_default(&o);
    o.cols = 132; o.rows = 50;
    sp_terminal* t = sp_terminal_create(&o);
    ASSERT_EQ_INT(sp_terminal_cols(t), 132);
    ASSERT_EQ_INT(sp_terminal_rows(t), 50);
    sp_terminal_destroy(t);
}

SP_TEST(terminal, resize_changes_dims) {
    sp_terminal* t = sp_terminal_create(NULL);
    sp_terminal_resize(t, 100, 30);
    ASSERT_EQ_INT(sp_terminal_cols(t), 100);
    ASSERT_EQ_INT(sp_terminal_rows(t), 30);
    sp_terminal_destroy(t);
}

SP_TEST(terminal, move_cursor_clamps_to_bounds) {
    sp_terminal* t = sp_terminal_create(NULL);
    sp_terminal_move_cursor(t, 9999, 9999);
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 23);
    ASSERT_EQ_INT(c, 79);
    sp_terminal_move_cursor(t, -5, -5);
    sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(c, 0);
    sp_terminal_destroy(t);
}

SP_TEST(terminal, clear_resets_cursor_to_origin) {
    sp_terminal* t = sp_terminal_create(NULL);
    sp_terminal_write(t, (const uint8_t*)"Hello\nWorld", 11);
    sp_terminal_clear(t);
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(c, 0);
    /* All cells should be blanked. */
    ASSERT_EQ_INT(sp_terminal_cell_at(t, 0, 0)->codepoint, (uint32_t)' ');
    sp_terminal_destroy(t);
}

SP_TEST(terminal, version_returns_nonempty_string) {
    const char* v = sp_version();
    ASSERT_TRUE(v != NULL);
    ASSERT_TRUE(strlen(v) > 0);
}

SP_TEST(terminal, cell_at_returns_null_for_out_of_bounds) {
    sp_terminal* t = sp_terminal_create(NULL);
    ASSERT_TRUE(sp_terminal_cell_at(t, -1, 0) == NULL);
    ASSERT_TRUE(sp_terminal_cell_at(t, 0, -1) == NULL);
    ASSERT_TRUE(sp_terminal_cell_at(t, 9999, 0) == NULL);
    ASSERT_TRUE(sp_terminal_cell_at(t, 0, 9999) == NULL);
    sp_terminal_destroy(t);
}

SP_TEST(terminal, write_null_data_is_safe) {
    sp_terminal* t = sp_terminal_create(NULL);
    sp_terminal_write(t, NULL, 100);
    sp_terminal_write(t, (const uint8_t*)"", 0);
    int32_t r, c; sp_terminal_get_cursor(t, &r, &c);
    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(c, 0);
    sp_terminal_destroy(t);
}
