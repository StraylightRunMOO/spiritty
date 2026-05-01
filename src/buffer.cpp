#include "spiritty/buffer.h"
#include "spiritty/terminal.h"
#include <algorithm>
#include <cstring>
#include <codecvt>
#include <locale>

namespace spiritty {

// TerminalLine implementation
TerminalLine::TerminalLine(int cols) : dirty_(true) {
    cells_.resize(cols);
    attrs_.reset();
}

void TerminalLine::set_attributes(const CellAttributes& attrs) {
    attrs_ = attrs;
    dirty_ = true;
}

void TerminalLine::resize(int cols) {
    if (cols == static_cast<int>(cells_.size())) return;
    
    cells_.resize(cols);
    dirty_ = true;
}

void TerminalLine::clear() {
    for (auto& cell : cells_) {
        cell.codepoint = ' ';
        cell.width = 1;
        cell.attrs = attrs_;
    }
    dirty_ = true;
}

void TerminalLine::clear_range(int start, int end) {
    start = std::max(0, start);
    end = std::min(static_cast<int>(cells_.size()), end);
    
    for (int i = start; i < end; ++i) {
        cells_[i].codepoint = ' ';
        cells_[i].width = 1;
        cells_[i].attrs = attrs_;
    }
    dirty_ = true;
}

std::string TerminalLine::text() const {
    std::u32string utf32_text;
    for (const auto& cell : cells_) {
        if (cell.codepoint != 0 && !cell.is_wide_continuation()) {
            utf32_text += cell.codepoint;
        }
    }
    
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
    return converter.to_bytes(utf32_text);
}

std::string TerminalLine::text_range(int start, int end) const {
    start = std::max(0, start);
    end = std::min(static_cast<int>(cells_.size()), end);
    
    std::u32string utf32_text;
    for (int i = start; i < end; ++i) {
        const auto& cell = cells_[i];
        if (cell.codepoint != 0 && !cell.is_wide_continuation()) {
            utf32_text += cell.codepoint;
        }
    }
    
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
    return converter.to_bytes(utf32_text);
}

int TerminalLine::find_word_boundary(int col, bool forward) const {
    if (col < 0 || col >= static_cast<int>(cells_.size())) {
        return col;
    }
    
    if (forward) {
        // Find end of current word or start of next word
        while (col < static_cast<int>(cells_.size()) - 1) {
            char32_t ch = cells_[col].codepoint;
            if (is_word_char(ch)) {
                // Skip to end of word
                while (col < static_cast<int>(cells_.size()) - 1 && is_word_char(cells_[col].codepoint)) {
                    col++;
                }
                break;
            }
            col++;
        }
    } else {
        // Find start of current word or end of previous word
        while (col > 0) {
            char32_t ch = cells_[col].codepoint;
            if (is_word_char(ch)) {
                // Skip to start of word
                while (col > 0 && is_word_char(cells_[col - 1].codepoint)) {
                    col--;
                }
                break;
            }
            col--;
        }
    }
    
    return col;
}

bool TerminalLine::is_word_char(char32_t ch) const {
    // Word characters are letters, digits, and underscore
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

// ScrollbackBuffer implementation
ScrollbackBuffer::ScrollbackBuffer(size_t max_lines) : max_lines_(max_lines) {}

void ScrollbackBuffer::push_line(std::unique_ptr<TerminalLine> line) {
    if (is_full()) {
        lines_.pop_front();
    }
    lines_.push_back(std::move(line));
}

std::unique_ptr<TerminalLine> ScrollbackBuffer::pop_line() {
    if (is_empty()) {
        return nullptr;
    }
    
    auto line = std::move(lines_.front());
    lines_.pop_front();
    return line;
}

std::unique_ptr<TerminalLine> ScrollbackBuffer::peek_line() const {
    if (is_empty()) {
        return nullptr;
    }
    
    // Create a copy
    auto* original = lines_.front().get();
    auto line = std::make_unique<TerminalLine>(original->width());
    
    // Copy cells
    for (int i = 0; i < original->width(); ++i) {
        line->cell(i) = original->cell(i);
    }
    
    // Copy attributes
    line->set_attributes(original->attributes());
    
    return line;
}

TerminalLine* ScrollbackBuffer::line(size_t index) {
    if (index >= lines_.size()) {
        return nullptr;
    }
    return lines_[index].get();
}

const TerminalLine* ScrollbackBuffer::line(size_t index) const {
    if (index >= lines_.size()) {
        return nullptr;
    }
    return lines_[index].get();
}

void ScrollbackBuffer::set_max_size(size_t max_lines) {
    max_lines_ = max_lines;
    
    // Remove excess lines if necessary
    while (lines_.size() > max_lines_) {
        lines_.pop_front();
    }
}

void ScrollbackBuffer::clear() {
    lines_.clear();
}

// TerminalBuffer implementation
TerminalBuffer::TerminalBuffer(Terminal* terminal, int rows, int cols, int scrollback_lines)
    : terminal_(terminal), cols_(cols), rows_(rows), viewport_top_(0),
      scrollback_(scrollback_lines) {
    create_lines();
}

void TerminalBuffer::create_lines() {
    lines_.clear();
    for (int i = 0; i < rows_; ++i) {
        lines_.push_back(std::make_unique<TerminalLine>(cols_));
    }
}

void TerminalBuffer::resize(int rows, int cols) {
    if (rows == rows_ && cols == cols_) return;
    
    // Handle column resize
    if (cols != cols_) {
        for (auto& line : lines_) {
            line->resize(cols);
        }
        cols_ = cols;
    }
    
    // Handle row resize
    if (rows != rows_) {
        if (rows > rows_) {
            // Add new lines
            for (int i = rows_; i < rows; ++i) {
                lines_.push_back(std::make_unique<TerminalLine>(cols_));
            }
        } else {
            // Remove lines, moving content to scrollback if needed
            int lines_to_remove = rows_ - rows;
            for (int i = 0; i < lines_to_remove; ++i) {
                scrollback_.push_line(std::move(lines_.front()));
                lines_.erase(lines_.begin());
            }
        }
        rows_ = rows;
    }
    
    // Ensure viewport is valid
    viewport_top_ = std::min(viewport_top_, static_cast<int>(scrollback_.size()));
}

void TerminalBuffer::scroll_up(int count) {
    count = std::min(count, static_cast<int>(scrollback_.size()) - viewport_top_);
    if (count > 0) {
        viewport_top_ += count;
        mark_all_dirty();
    }
}

void TerminalBuffer::scroll_down(int count) {
    count = std::min(count, viewport_top_);
    if (count > 0) {
        viewport_top_ -= count;
        mark_all_dirty();
    }
}

void TerminalBuffer::scroll_to(int row) {
    row = std::max(0, std::min(row, static_cast<int>(scrollback_.size())));
    if (row != viewport_top_) {
        viewport_top_ = row;
        mark_all_dirty();
    }
}

void TerminalBuffer::set_viewport(int top_line) {
    viewport_top_ = std::max(0, std::min(top_line, static_cast<int>(scrollback_.size())));
}

void TerminalBuffer::clear() {
    for (auto& line : lines_) {
        line->clear();
    }
    mark_all_dirty();
}

void TerminalBuffer::clear_line(int row) {
    if (row >= 0 && row < rows_) {
        lines_[row]->clear();
        lines_[row]->mark_dirty();
    }
}

void TerminalBuffer::clear_range(int start_row, int start_col, int end_row, int end_col) {
    start_row = std::max(0, start_row);
    end_row = std::min(rows_ - 1, end_row);
    start_col = std::max(0, start_col);
    end_col = std::min(cols_ - 1, end_col);
    
    for (int row = start_row; row <= end_row; ++row) {
        int line_start_col = (row == start_row) ? start_col : 0;
        int line_end_col = (row == end_row) ? end_col : cols_ - 1;
        
        lines_[row]->clear_range(line_start_col, line_end_col + 1);
        lines_[row]->mark_dirty();
    }
}

void TerminalBuffer::insert_line(int row, int count) {
    if (count <= 0) return;
    
    row = std::max(0, std::min(row, rows_ - 1));
    count = std::min(count, rows_ - row);
    
    // Move lines down
    for (int i = rows_ - 1; i >= row + count; --i) {
        lines_[i] = std::move(lines_[i - count]);
    }
    
    // Create new lines
    for (int i = row; i < row + count; ++i) {
        lines_[i] = std::make_unique<TerminalLine>(cols_);
    }
    
    mark_all_dirty();
}

void TerminalBuffer::delete_line(int row, int count) {
    if (count <= 0) return;
    
    row = std::max(0, std::min(row, rows_ - 1));
    count = std::min(count, rows_ - row);
    
    // Move lines up
    for (int i = row; i < rows_ - count; ++i) {
        lines_[i] = std::move(lines_[i + count]);
    }
    
    // Create new lines at bottom
    for (int i = rows_ - count; i < rows_; ++i) {
        lines_[i] = std::make_unique<TerminalLine>(cols_);
    }
    
    mark_all_dirty();
}

void TerminalBuffer::insert_chars(int row, int col, int count) {
    if (row < 0 || row >= rows_ || count <= 0) return;
    
    TerminalLine& line = *lines_[row];
    col = std::max(0, std::min(col, cols_ - 1));
    count = std::min(count, cols_ - col);
    
    // Shift characters to the right
    for (int i = cols_ - 1; i >= col + count; --i) {
        line.cell(i) = line.cell(i - count);
    }
    
    // Clear inserted positions
    for (int i = col; i < col + count; ++i) {
        line.cell(i).codepoint = ' ';
        line.cell(i).width = 1;
        line.cell(i).attrs = line.attributes();
    }
    
    line.mark_dirty();
}

void TerminalBuffer::delete_chars(int row, int col, int count) {
    if (row < 0 || row >= rows_ || count <= 0) return;
    
    TerminalLine& line = *lines_[row];
    col = std::max(0, std::min(col, cols_ - 1));
    count = std::min(count, cols_ - col);
    
    // Shift characters to the left
    for (int i = col; i < cols_ - count; ++i) {
        line.cell(i) = line.cell(i + count);
    }
    
    // Clear remaining positions
    for (int i = cols_ - count; i < cols_; ++i) {
        line.cell(i).codepoint = ' ';
        line.cell(i).width = 1;
        line.cell(i).attrs = line.attributes();
    }
    
    line.mark_dirty();
}

void TerminalBuffer::write_text(int row, int col, const std::u32string& text, const CellAttributes& attrs) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    
    TerminalLine& line = *lines_[row];
    
    for (char32_t ch : text) {
        if (col >= cols_) break;
        
        line.cell(col).codepoint = ch;
        line.cell(col).width = 1; // TODO: Handle wide characters
        line.cell(col).attrs = attrs;
        
        col++;
    }
    
    line.mark_dirty();
}

void TerminalBuffer::write_char(int row, int col, char32_t ch, const CellAttributes& attrs) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    
    TerminalLine& line = *lines_[row];
    line.cell(col).codepoint = ch;
    line.cell(col).width = 1; // TODO: Handle wide characters
    line.cell(col).attrs = attrs;
    
    line.mark_dirty();
}

void TerminalBuffer::scroll_line_into_scrollback(std::unique_ptr<TerminalLine> line) {
    scrollback_.push_line(std::move(line));
}

std::unique_ptr<TerminalLine> TerminalBuffer::get_line_from_scrollback() {
    return scrollback_.pop_line();
}

void TerminalBuffer::mark_all_dirty() {
    for (auto& line : lines_) {
        line->mark_dirty();
    }
}

void TerminalBuffer::mark_dirty(int row) {
    if (row >= 0 && row < rows_) {
        lines_[row]->mark_dirty();
    }
}

void TerminalBuffer::clear_dirty_flags() {
    for (auto& line : lines_) {
        line->set_dirty(false);
    }
}

std::string TerminalBuffer::get_text() const {
    std::string result;
    for (const auto& line : lines_) {
        if (!result.empty()) {
            result += "\n";
        }
        result += line->text();
    }
    return result;
}

std::string TerminalBuffer::get_text_range(int start_row, int start_col, int end_row, int end_col) const {
    start_row = std::max(0, start_row);
    end_row = std::min(rows_ - 1, end_row);
    start_col = std::max(0, start_col);
    end_col = std::max(0, end_col);
    
    std::string result;
    for (int row = start_row; row <= end_row; ++row) {
        if (!result.empty()) {
            result += "\n";
        }
        
        int line_start_col = (row == start_row) ? start_col : 0;
        int line_end_col = (row == end_row) ? end_col : cols_ - 1;
        
        result += lines_[row]->text_range(line_start_col, line_end_col + 1);
    }
    return result;
}

void TerminalBuffer::reflow_lines() {
    // TODO: Implement word wrapping reflow
    // This is complex and would need to handle:
    // - Preserving attributes across wrapped lines
    // - Updating cursor position
    // - Managing scrollback
    // - Handling wide characters
}

void TerminalBuffer::save_state() {
    if (!saved_state_) {
        saved_state_ = std::make_unique<SavedState>();
    }
    
    // Save current lines
    saved_state_->lines.clear();
    for (const auto& line : lines_) {
        auto saved_line = std::make_unique<TerminalLine>(line->width());
        for (int i = 0; i < line->width(); ++i) {
            saved_line->cell(i) = line->cell(i);
        }
        saved_line->set_attributes(line->attributes());
        saved_state_->lines.push_back(std::move(saved_line));
    }
    
    // Save scrollback (shallow copy for now)
    // In a full implementation, we'd deep copy the scrollback
}

void TerminalBuffer::restore_state() {
    if (!saved_state_) return;
    
    // Restore lines
    lines_.clear();
    for (const auto& saved_line : saved_state_->lines) {
        auto line = std::make_unique<TerminalLine>(saved_line->width());
        for (int i = 0; i < saved_line->width(); ++i) {
            line->cell(i) = saved_line->cell(i);
        }
        line->set_attributes(saved_line->attributes());
        lines_.push_back(std::move(line));
    }
    
    // Restore scrollback
    // In a full implementation, we'd restore the scrollback
}

void TerminalBuffer::clear_history() {
    scrollback_.clear();
}

} // namespace spiritty