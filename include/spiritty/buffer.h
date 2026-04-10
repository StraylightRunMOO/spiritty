#pragma once

#include <vector>
#include <memory>
#include <string>
#include <deque>
#include <cstdint>
#include "terminal.h"

namespace spiritty {

// Forward declaration
class Terminal;

// Terminal line - represents a single row in the terminal
class TerminalLine {
public:
    TerminalLine(int cols);
    
    // Cell access
    Cell& cell(int col) { return cells_[col]; }
    const Cell& cell(int col) const { return cells_[col]; }
    
    // Line properties
    bool is_dirty() const { return dirty_; }
    void set_dirty(bool dirty) { dirty_ = dirty; }
    void mark_dirty() { dirty_ = true; }
    
    // Attributes
    void set_attributes(const CellAttributes& attrs);
    const CellAttributes& attributes() const { return attrs_; }
    
    // Width tracking
    int width() const { return static_cast<int>(cells_.size()); }
    void resize(int cols);
    
    // Clear line
    void clear();
    void clear_range(int start, int end);
    
    // Text extraction
    std::string text() const;
    std::string text_range(int start, int end) const;
    
    // Dirty flag management
    bool is_line_dirty() const { return dirty_; }
    void set_line_dirty(bool dirty) { dirty_ = dirty; }
    
    // Word wrapping support
    int find_word_boundary(int col, bool forward) const;
    bool is_word_char(char32_t ch) const;

private:
    std::vector<Cell> cells_;
    CellAttributes attrs_;
    bool dirty_;
    
    friend class TerminalBuffer;
};

// Scrollback buffer - stores lines that have scrolled off screen
class ScrollbackBuffer {
public:
    explicit ScrollbackBuffer(size_t max_lines);
    
    // Line management
    void push_line(std::unique_ptr<TerminalLine> line);
    std::unique_ptr<TerminalLine> pop_line();
    std::unique_ptr<TerminalLine> peek_line() const;
    
    // Access
    TerminalLine* line(size_t index);
    const TerminalLine* line(size_t index) const;
    
    // Size management
    size_t size() const { return lines_.size(); }
    size_t max_size() const { return max_lines_; }
    void set_max_size(size_t max_lines);
    bool is_full() const { return lines_.size() >= max_lines_; }
    bool is_empty() const { return lines_.empty(); }
    
    // Clear
    void clear();
    
    // Iterator support
    using iterator = std::deque<std::unique_ptr<TerminalLine>>::iterator;
    using const_iterator = std::deque<std::unique_ptr<TerminalLine>>::const_iterator;
    
    iterator begin() { return lines_.begin(); }
    iterator end() { return lines_.end(); }
    const_iterator begin() const { return lines_.begin(); }
    const_iterator end() const { return lines_.end(); }
    const_iterator cbegin() const { return lines_.cbegin(); }
    const_iterator cend() const { return lines_.cend(); }

private:
    std::deque<std::unique_ptr<TerminalLine>> lines_;
    size_t max_lines_;
};

// Main terminal buffer - manages visible lines and scrollback
class TerminalBuffer {
public:
    TerminalBuffer(Terminal* terminal, int rows, int cols, int scrollback_lines);
    
    // Screen access
    TerminalLine& line(int row) { return *lines_[row]; }
    const TerminalLine& line(int row) const { return *lines_[row]; }
    
    // Cell access
    Cell& cell(int row, int col) { return lines_[row]->cell(col); }
    const Cell& cell(int row, int col) const { return lines_[row]->cell(col); }
    
    // Buffer dimensions
    int rows() const { return static_cast<int>(lines_.size()); }
    int cols() const { return cols_; }
    
    // Resize buffer
    void resize(int rows, int cols);
    
    // Scroll operations
    void scroll_up(int count = 1);
    void scroll_down(int count = 1);
    void scroll_to(int row);
    
    // Clear operations
    void clear();
    void clear_line(int row);
    void clear_range(int start_row, int start_col, int end_row, int end_col);
    
    // Insert/delete operations
    void insert_line(int row, int count = 1);
    void delete_line(int row, int count = 1);
    void insert_chars(int row, int col, int count = 1);
    void delete_chars(int row, int col, int count = 1);
    
    // Text operations
    void write_text(int row, int col, const std::u32string& text, const CellAttributes& attrs);
    void write_char(int row, int col, char32_t ch, const CellAttributes& attrs);
    
    // Scrollback
    void scroll_line_into_scrollback(std::unique_ptr<TerminalLine> line);
    std::unique_ptr<TerminalLine> get_line_from_scrollback();
    
    // Viewport management
    void set_viewport(int top_line);
    int viewport_top() const { return viewport_top_; }
    int viewport_bottom() const { return viewport_top_ + rows() - 1; }
    
    // Dirty tracking
    void mark_all_dirty();
    void mark_dirty(int row);
    bool is_dirty(int row) const { return lines_[row]->is_dirty(); }
    void clear_dirty_flags();
    
    // Text extraction for selection/copy
    std::string get_text() const;
    std::string get_text_range(int start_row, int start_col, int end_row, int end_col) const;
    
    // Word wrapping
    void reflow_lines();
    
    // Save/restore state
    void save_state();
    void restore_state();
    
    // History management
    size_t history_size() const { return scrollback_.size(); }
    size_t max_history_size() const { return scrollback_.max_size(); }
    void clear_history();

private:
    Terminal* terminal_;
    int cols_;
    int rows_;
    int viewport_top_;
    
    std::vector<std::unique_ptr<TerminalLine>> lines_;
    ScrollbackBuffer scrollback_;
    
    // Saved state
    struct SavedState {
        std::vector<std::unique_ptr<TerminalLine>> lines;
        ScrollbackBuffer scrollback;
    };
    std::unique_ptr<SavedState> saved_state_;
    
    // Helper methods
    void create_lines();
    void move_lines_up(int start, int end, int count);
    void move_lines_down(int start, int end, int count);
    void copy_line_attributes(TerminalLine* dest, const TerminalLine* src);
};

} // namespace spiritty