#include "test_framework.h"
#include "spiritty/buffer.h"
#include "spiritty/terminal.h"

using namespace spiritty;

// ---------------------------------------------------------------------------
// TerminalLine tests
// ---------------------------------------------------------------------------

TEST(buffer, terminal_line_creation) {
    TerminalLine line(80);
    ASSERT_EQ(line.width(), 80);
    ASSERT_TRUE(line.is_dirty());
}

TEST(buffer, terminal_line_clear) {
    TerminalLine line(10);
    line.clear();
    ASSERT_TRUE(line.is_dirty());
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(line.cell(i).codepoint, ' ');
    }
}

TEST(buffer, terminal_line_resize) {
    TerminalLine line(10);
    line.resize(20);
    ASSERT_EQ(line.width(), 20);
    ASSERT_TRUE(line.is_dirty());
}

TEST(buffer, terminal_line_text) {
    TerminalLine line(5);
    line.cell(0).codepoint = U'H';
    line.cell(1).codepoint = U'e';
    line.cell(2).codepoint = U'l';
    line.cell(3).codepoint = U'l';
    line.cell(4).codepoint = U'o';
    ASSERT_EQ(line.text(), "Hello");
}

TEST(buffer, terminal_line_text_range) {
    TerminalLine line(5);
    line.cell(0).codepoint = U'H';
    line.cell(1).codepoint = U'e';
    line.cell(2).codepoint = U'l';
    line.cell(3).codepoint = U'l';
    line.cell(4).codepoint = U'o';
    ASSERT_EQ(line.text_range(1, 4), "ell");
}

// ---------------------------------------------------------------------------
// ScrollbackBuffer tests
// ---------------------------------------------------------------------------

TEST(buffer, scrollback_creation) {
    ScrollbackBuffer sb(100);
    ASSERT_EQ(sb.max_size(), 100);
    ASSERT_TRUE(sb.is_empty());
    ASSERT_FALSE(sb.is_full());
}

TEST(buffer, scrollback_push_and_pop) {
    ScrollbackBuffer sb(10);
    auto line = std::make_unique<TerminalLine>(5);
    line->cell(0).codepoint = U'A';
    sb.push_line(std::move(line));
    
    ASSERT_EQ(sb.size(), 1);
    ASSERT_FALSE(sb.is_empty());
    
    auto popped = sb.pop_line();
    ASSERT_TRUE(popped != nullptr);
    ASSERT_EQ(popped->cell(0).codepoint, U'A');
    ASSERT_TRUE(sb.is_empty());
}

TEST(buffer, scrollback_max_size) {
    ScrollbackBuffer sb(2);
    for (int i = 0; i < 5; ++i) {
        auto line = std::make_unique<TerminalLine>(1);
        line->cell(0).codepoint = static_cast<char32_t>(U'A' + i);
        sb.push_line(std::move(line));
    }
    ASSERT_EQ(sb.size(), 2);
    ASSERT_TRUE(sb.is_full());
    
    auto popped = sb.pop_line();
    ASSERT_EQ(popped->cell(0).codepoint, U'D'); // oldest of the 2 kept
}

// ---------------------------------------------------------------------------
// TerminalBuffer tests
// ---------------------------------------------------------------------------

TEST(buffer, terminal_buffer_creation) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 24, 80, 1000);
    ASSERT_EQ(buf.rows(), 24);
    ASSERT_EQ(buf.cols(), 80);
}

TEST(buffer, terminal_buffer_cell_access) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 10, 10, 100);
    buf.cell(0, 0).codepoint = U'X';
    ASSERT_EQ(buf.cell(0, 0).codepoint, U'X');
}

TEST(buffer, terminal_buffer_clear) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 5, 5, 100);
    buf.cell(0, 0).codepoint = U'X';
    buf.clear();
    ASSERT_EQ(buf.cell(0, 0).codepoint, ' ');
}

TEST(buffer, terminal_buffer_write_char) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 10, 10, 100);
    CellAttributes attrs;
    buf.write_char(0, 0, U'Z', attrs);
    ASSERT_EQ(buf.cell(0, 0).codepoint, U'Z');
}

TEST(buffer, terminal_buffer_write_text) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 10, 10, 100);
    CellAttributes attrs;
    buf.write_text(0, 0, U"Hello", attrs);
    ASSERT_EQ(buf.cell(0, 0).codepoint, U'H');
    ASSERT_EQ(buf.cell(0, 1).codepoint, U'e');
    ASSERT_EQ(buf.cell(0, 4).codepoint, U'o');
}

TEST(buffer, terminal_buffer_resize_cols) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 5, 5, 100);
    buf.resize(5, 10);
    ASSERT_EQ(buf.cols(), 10);
    ASSERT_EQ(buf.rows(), 5);
}

TEST(buffer, terminal_buffer_resize_rows_up) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 5, 5, 100);
    buf.resize(10, 5);
    ASSERT_EQ(buf.rows(), 10);
    ASSERT_EQ(buf.cols(), 5);
}

TEST(buffer, terminal_buffer_resize_rows_down) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 5, 5, 100);
    buf.cell(0, 0).codepoint = U'A';
    buf.resize(3, 5);
    ASSERT_EQ(buf.rows(), 3);
    ASSERT_EQ(buf.cols(), 5);
    // Lines scrolled off should be in scrollback
    ASSERT_EQ(buf.history_size(), 2);
}

TEST(buffer, terminal_buffer_insert_chars) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 3, 5, 100);
    CellAttributes attrs;
    buf.write_text(0, 0, U"ABCDE", attrs);
    buf.insert_chars(0, 1, 2);
    ASSERT_EQ(buf.cell(0, 0).codepoint, U'A');
    ASSERT_EQ(buf.cell(0, 1).codepoint, U' ');
    ASSERT_EQ(buf.cell(0, 2).codepoint, U' ');
    ASSERT_EQ(buf.cell(0, 3).codepoint, U'B');
}

TEST(buffer, terminal_buffer_delete_chars) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 3, 5, 100);
    CellAttributes attrs;
    buf.write_text(0, 0, U"ABCDE", attrs);
    buf.delete_chars(0, 1, 2);
    ASSERT_EQ(buf.cell(0, 0).codepoint, U'A');
    ASSERT_EQ(buf.cell(0, 1).codepoint, U'D');
    ASSERT_EQ(buf.cell(0, 2).codepoint, U'E');
}

TEST(buffer, terminal_buffer_insert_line) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 3, 3, 100);
    CellAttributes attrs;
    buf.write_text(0, 0, U"AAA", attrs);
    buf.write_text(1, 0, U"BBB", attrs);
    buf.write_text(2, 0, U"CCC", attrs);
    buf.insert_line(1, 1);
    ASSERT_EQ(buf.cell(0, 0).codepoint, U'A');
    ASSERT_EQ(buf.cell(1, 0).codepoint, 0); // new line has empty cells
    ASSERT_EQ(buf.cell(2, 0).codepoint, U'B');
}

TEST(buffer, terminal_buffer_delete_line) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 3, 3, 100);
    CellAttributes attrs;
    buf.write_text(0, 0, U"AAA", attrs);
    buf.write_text(1, 0, U"BBB", attrs);
    buf.write_text(2, 0, U"CCC", attrs);
    buf.delete_line(1, 1);
    ASSERT_EQ(buf.cell(0, 0).codepoint, U'A');
    ASSERT_EQ(buf.cell(1, 0).codepoint, U'C');
    ASSERT_EQ(buf.cell(2, 0).codepoint, 0); // new line has empty cells
}

TEST(buffer, terminal_buffer_get_text) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 2, 3, 100);
    CellAttributes attrs;
    buf.write_text(0, 0, U"Hi", attrs);
    buf.write_text(1, 0, U"Bye", attrs);
    ASSERT_EQ(buf.get_text(), "Hi\nBye");
}

TEST(buffer, terminal_buffer_mark_dirty) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 3, 3, 100);
    buf.clear_dirty_flags();
    ASSERT_FALSE(buf.is_dirty(0));
    buf.mark_dirty(0);
    ASSERT_TRUE(buf.is_dirty(0));
}

TEST(buffer, terminal_buffer_mark_all_dirty) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 3, 3, 100);
    buf.clear_dirty_flags();
    buf.mark_all_dirty();
    ASSERT_TRUE(buf.is_dirty(0));
    ASSERT_TRUE(buf.is_dirty(1));
    ASSERT_TRUE(buf.is_dirty(2));
}

TEST(buffer, terminal_buffer_viewport) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    TerminalBuffer buf(&term, 5, 5, 100);
    buf.set_viewport(0);
    ASSERT_EQ(buf.viewport_top(), 0);
}
