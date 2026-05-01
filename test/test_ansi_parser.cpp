#include "test_framework.h"
#include "spiritty/ansi_parser.h"
#include "spiritty/terminal.h"
#include "spiritty/buffer.h"

using namespace spiritty;

TEST(ansi_parser, creation) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    ANSIParser parser(&term);
    ASSERT_TRUE(parser.modes().cursor_visible);
    ASSERT_TRUE(parser.modes().auto_wrap);
}

TEST(ansi_parser, reset) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    ANSIParser parser(&term);
    parser.set_mouse_reporting(true);
    parser.reset();
    ASSERT_FALSE(parser.modes().mouse_reporting);
}

TEST(ansi_parser, print_text) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("Hello");
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, U'H');
    ASSERT_EQ(term.buffer()->cell(0, 1).codepoint, U'e');
    ASSERT_EQ(term.buffer()->cell(0, 4).codepoint, U'o');
}

TEST(ansi_parser, cursor_up) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(5, 5);
    ANSIParser parser(&term);
    parser.parse("\x1B[3A");
    ASSERT_EQ(term.cursor().row, 2);
    ASSERT_EQ(term.cursor().col, 5);
}

TEST(ansi_parser, cursor_down) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(5, 5);
    ANSIParser parser(&term);
    parser.parse("\x1B[2B");
    ASSERT_EQ(term.cursor().row, 7);
    ASSERT_EQ(term.cursor().col, 5);
}

TEST(ansi_parser, cursor_forward) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(5, 5);
    ANSIParser parser(&term);
    parser.parse("\x1B[4C");
    ASSERT_EQ(term.cursor().row, 5);
    ASSERT_EQ(term.cursor().col, 9);
}

TEST(ansi_parser, cursor_back) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(5, 5);
    ANSIParser parser(&term);
    parser.parse("\x1B[2D");
    ASSERT_EQ(term.cursor().row, 5);
    ASSERT_EQ(term.cursor().col, 3);
}

TEST(ansi_parser, cursor_position) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[10;20H");
    ASSERT_EQ(term.cursor().row, 9);
    ASSERT_EQ(term.cursor().col, 19);
}

TEST(ansi_parser, erase_display) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("Hello\x1B[2J");
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, ' ');
}

TEST(ansi_parser, erase_line) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("Hello\x1B[2K");
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, ' ');
}

TEST(ansi_parser, sgr_reset) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[1m\x1B[31m\x1B[0mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_FALSE(attrs.bold);
}

TEST(ansi_parser, sgr_bold) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[1mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_TRUE(attrs.bold);
}

TEST(ansi_parser, sgr_italic) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[3mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_TRUE(attrs.italic);
}

TEST(ansi_parser, sgr_underline) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[4mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_TRUE(attrs.underline);
    ASSERT_EQ(attrs.underline_style, 1);
}

TEST(ansi_parser, sgr_strikethrough) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[9mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_TRUE(attrs.strikethrough);
}

TEST(ansi_parser, sgr_foreground_color) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[31mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_EQ(attrs.fg_color, 0xFF800000);
}

TEST(ansi_parser, sgr_background_color) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[42mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_EQ(attrs.bg_color, 0xFF008000);
}

TEST(ansi_parser, sgr_256_color) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[38;5;196mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_EQ(attrs.fg_color, parser.get_color_palette(196));
}

TEST(ansi_parser, sgr_true_color) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[38;2;255;128;64mA");
    CellAttributes attrs = term.buffer()->cell(0, 0).attrs;
    ASSERT_EQ(attrs.fg_color, (0xFF << 24) | (255 << 16) | (128 << 8) | 64);
}

TEST(ansi_parser, save_restore_cursor) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(5, 10);
    ANSIParser parser(&term);
    parser.parse("\x1B\x37"); // ESC 7 - save cursor
    term.move_cursor(0, 0);
    parser.parse("\x1B\x38"); // ESC 8 - restore cursor
    ASSERT_EQ(term.cursor().row, 5);
    ASSERT_EQ(term.cursor().col, 10);
}

TEST(ansi_parser, tab) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\t");
    ASSERT_EQ(term.cursor().col, 8);
}

TEST(ansi_parser, carriage_return) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(5, 10);
    ANSIParser parser(&term);
    parser.parse("\r");
    ASSERT_EQ(term.cursor().row, 5);
    ASSERT_EQ(term.cursor().col, 0);
}

TEST(ansi_parser, backspace) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(0, 5);
    ANSIParser parser(&term);
    parser.parse("\x08");
    ASSERT_EQ(term.cursor().col, 4);
}

TEST(ansi_parser, bell_ignored) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x07");
    // Should not crash or move cursor
    ASSERT_EQ(term.cursor().row, 0);
    ASSERT_EQ(term.cursor().col, 0);
}

TEST(ansi_parser, next_line) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.move_cursor(0, 5);
    ANSIParser parser(&term);
    parser.parse("\x1B\x45"); // NEL
    ASSERT_EQ(term.cursor().row, 1);
    ASSERT_EQ(term.cursor().col, 0);
}

TEST(ansi_parser, decset_cursor_visible) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B[?25l"); // hide cursor
    ASSERT_FALSE(parser.modes().cursor_visible);
    parser.parse("\x1B[?25h"); // show cursor
    ASSERT_TRUE(parser.modes().cursor_visible);
}

TEST(ansi_parser, window_title_osc) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("\x1B]2;My Title\x07");
    ASSERT_EQ(parser.window_title(), "My Title");
}

TEST(ansi_parser, insert_character) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("ABC\x1B[1D\x1B[@X");
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, U'A');
    ASSERT_EQ(term.buffer()->cell(0, 1).codepoint, U'B');
    ASSERT_EQ(term.buffer()->cell(0, 2).codepoint, U'X');
    ASSERT_EQ(term.buffer()->cell(0, 3).codepoint, U'C');
}

TEST(ansi_parser, delete_character) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    ANSIParser parser(&term);
    parser.parse("ABC\x1B[2D\x1B[PX");
    ASSERT_EQ(term.buffer()->cell(0, 0).codepoint, U'A');
    ASSERT_EQ(term.buffer()->cell(0, 1).codepoint, U'X');
    ASSERT_EQ(term.buffer()->cell(0, 2).codepoint, 0); // shifted from empty cell
}
