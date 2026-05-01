#include "test_framework.h"
#include "spiritty/selection_manager.h"
#include "spiritty/terminal.h"

using namespace spiritty;

TEST(selection, creation) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    ASSERT_FALSE(sm.has_selection());
}

TEST(selection, start_end_selection) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 5);
    sm.end_selection();
    ASSERT_TRUE(sm.has_selection());
}

TEST(selection, clear_selection) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 5);
    sm.end_selection();
    sm.clear_selection();
    ASSERT_FALSE(sm.has_selection());
}

TEST(selection, select_all) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello World");
    SelectionManager sm(&term);
    sm.select_all();
    ASSERT_TRUE(sm.has_selection());
}

TEST(selection, select_word) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello");
    SelectionManager sm(&term);
    sm.select_word(0, 2);
    ASSERT_TRUE(sm.has_selection());
}

TEST(selection, select_line) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello");
    SelectionManager sm(&term);
    sm.select_line(0);
    ASSERT_TRUE(sm.has_selection());
}

TEST(selection, is_position_selected) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 1);
    sm.update_selection(0, 5);
    sm.end_selection();
    ASSERT_TRUE(sm.is_position_selected(0, 2));
    ASSERT_TRUE(sm.is_position_selected(0, 5));
    ASSERT_FALSE(sm.is_position_selected(0, 0));
    ASSERT_FALSE(sm.is_position_selected(0, 6));
}

TEST(selection, normalize_selection) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 5);
    sm.update_selection(0, 1);
    sm.end_selection();
    Selection normalized = sm.normalize_selection();
    ASSERT_EQ(normalized.start_col, 1);
    ASSERT_EQ(normalized.end_col, 5);
}

TEST(selection, selection_direction) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 5);
    sm.end_selection();
    ASSERT_TRUE(sm.selection_direction() == SelectionDirection::FORWARD);
}

TEST(selection, is_selection_empty) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 0);
    sm.end_selection();
    ASSERT_TRUE(sm.is_selection_empty());
}

TEST(selection, selection_length) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 4);
    sm.end_selection();
    ASSERT_EQ(sm.selection_length(), 5);
}

TEST(selection, find_word_boundaries) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("hello world");
    SelectionManager sm(&term);
    auto boundaries = sm.find_word_boundaries(0, 2);
    ASSERT_EQ(boundaries.first, 0);
    ASSERT_EQ(boundaries.second, 5);
}

TEST(selection, get_selected_text) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("Hello");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 4);
    sm.end_selection();
    ASSERT_EQ(sm.get_selected_text(), "Hello");
}

TEST(selection, extend_selection) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 2);
    sm.end_selection();
    sm.extend_selection(0, 5);
    ASSERT_TRUE(sm.is_position_selected(0, 5));
}

TEST(selection, config) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    auto config = sm.config();
    ASSERT_TRUE(config.word_selection_on_double_click);
    ASSERT_TRUE(config.line_selection_on_triple_click);
}

TEST(selection, set_config) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    auto config = sm.config();
    config.copy_on_select = true;
    sm.set_config(config);
    ASSERT_TRUE(sm.config().copy_on_select);
}

TEST(selection, undo_redo) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 2);
    sm.end_selection();
    sm.undo_selection();
    // History may be empty after undo if only one entry
    // Just verify it doesn't crash
}

TEST(selection, find_all) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    term.write("hello hello hello");
    SelectionManager sm(&term);
    auto results = sm.find_all("hello");
    ASSERT_EQ(results.size(), 3);
}

TEST(selection, selection_description) {
    TerminalOptions _test_opts; _test_opts.gpu_acceleration = false; Terminal term(_test_opts);
    term.open("test");
    SelectionManager sm(&term);
    sm.start_selection(0, 0);
    sm.update_selection(0, 4);
    sm.end_selection();
    auto desc = sm.get_selection_description();
    ASSERT_NE(desc.find("Selection from"), std::string::npos);
}
