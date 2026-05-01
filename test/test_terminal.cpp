#include "test_framework.h"
#include "spiritty/terminal.h"
#include "spiritty/buffer.h"

using namespace spiritty;

TEST(terminal, default_creation) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    ASSERT_EQ(term.cols(), 80);
    ASSERT_EQ(term.rows(), 24);
    ASSERT_TRUE(term.cursor().visible);
}

TEST(terminal, custom_options) {
    TerminalOptions opts;
    opts.cols = 100;
    opts.rows = 30;
    opts.font_size = 16;
    opts.gpu_acceleration = false;
    Terminal term(opts);
    ASSERT_EQ(term.cols(), 100);
    ASSERT_EQ(term.rows(), 30);
}

TEST(terminal, open) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test-container");
    // Should not crash
    ASSERT_TRUE(term.buffer() != nullptr);
}

TEST(terminal, write_text) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello");
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, U'H');
    ASSERT_EQ(term.buffer()->cell(0, 4).codepoint, U'o');
}

TEST(terminal, resize) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.resize(100, 50);
    ASSERT_EQ(term.cols(), 100);
    ASSERT_EQ(term.rows(), 50);
}

TEST(terminal, resize_invalid) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.resize(-1, 0);
    ASSERT_EQ(term.cols(), 80); // unchanged
    ASSERT_EQ(term.rows(), 24); // unchanged
}

TEST(terminal, clear) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello");
    term.clear();
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, ' ');
    ASSERT_EQ(term.cursor().row, 0);
    ASSERT_EQ(term.cursor().col, 0);
}

TEST(terminal, reset) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello");
    term.move_cursor(5, 5);
    term.reset();
    ASSERT_EQ(term.cursor().row, 0);
    ASSERT_EQ(term.cursor().col, 0);
}

TEST(terminal, move_cursor) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(10, 20);
    ASSERT_EQ(term.cursor().row, 10);
    ASSERT_EQ(term.cursor().col, 20);
}

TEST(terminal, move_cursor_out_of_bounds) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(100, 100);
    ASSERT_EQ(term.cursor().row, 0); // should be rejected
    ASSERT_EQ(term.cursor().col, 0);
}

TEST(terminal, save_restore_cursor) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(10, 20);
    term.save_cursor();
    term.move_cursor(0, 0);
    term.restore_cursor();
    ASSERT_EQ(term.cursor().row, 10);
    ASSERT_EQ(term.cursor().col, 20);
}

TEST(terminal, hide_show_cursor) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.hide_cursor();
    ASSERT_FALSE(term.cursor().visible);
    term.show_cursor();
    ASSERT_TRUE(term.cursor().visible);
}

TEST(terminal, focus_blur) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.focus();
    ASSERT_TRUE(term.cursor().visible);
    term.blur();
    ASSERT_TRUE(term.cursor().blink_state);
}

TEST(terminal, options_get_set) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.set_option("fontSize", "16");
    ASSERT_EQ(term.get_option("fontSize"), "16");
    term.set_option("cursorBlink", "false");
    ASSERT_EQ(term.get_option("cursorBlink"), "false");
}

TEST(terminal, selection_api) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello World");
    ASSERT_EQ(term.get_selection(), ""); // no selection yet
    term.select_all();
    ASSERT_NE(term.get_selection(), "");
    term.clear_selection();
    ASSERT_EQ(term.get_selection(), "");
}

TEST(terminal, event_callbacks) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    bool called = false;
    term.on_event("resize", [&called](const TerminalEvent&) {
        called = true;
    });
    term.resize(100, 50);
    ASSERT_TRUE(called);
}

TEST(terminal, scroll_to_methods) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.scroll_to_top();
    term.scroll_to_bottom();
    term.scroll_to_line(0);
    // Should not crash
}

TEST(terminal, terminal_type) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.set_terminal_type("vt100");
    // Should not crash
}

TEST(terminal, charset) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.set_charset("UTF-8");
    // Should not crash
}

TEST(terminal, destroy) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.destroy();
    // Should not crash; buffer should be cleared
    ASSERT_TRUE(term.buffer() == nullptr);
}

TEST(terminal, js_bridge) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.js_write("Hello");
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, U'H');
    term.js_resize(100, 50);
    ASSERT_EQ(term.cols(), 100);
    ASSERT_EQ(term.rows(), 50);
}

TEST(terminal, refresh) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("X");
    term.refresh();
    // Should not crash
}
