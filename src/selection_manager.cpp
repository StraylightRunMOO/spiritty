#include "spiritty/selection_manager.h"
#include "spiritty/terminal.h"
#include "spiritty/buffer.h"
#include <algorithm>
#include <sstream>

namespace spiritty {

SelectionManager::SelectionManager(Terminal* terminal)
    : terminal_(terminal),
      buffer_(nullptr),
      selection_mode_(SelectionMode::NORMAL),
      selecting_(false),
      selection_start_row_(0),
      selection_start_col_(0),
      selection_end_row_(0),
      selection_end_col_(0),
      last_mouse_row_(0),
      last_mouse_col_(0),
      mouse_down_(false),
      mouse_down_row_(0),
      mouse_down_col_(0),
      mouse_down_button_(0),
      lazy_selection_(false),
      selection_dirty_(false) {
    
    selection_.active = false;
    selection_.start_row = 0;
    selection_.start_col = 0;
    selection_.end_row = 0;
    selection_.end_col = 0;
}

void SelectionManager::start_selection(int row, int col, SelectionMode mode) {
    selection_mode_ = mode;
    selecting_ = true;
    
    selection_start_row_ = row;
    selection_start_col_ = col;
    selection_end_row_ = row;
    selection_end_col_ = col;
    
    last_mouse_row_ = row;
    last_mouse_col_ = col;
    
    // Apply selection mode adjustments
    switch (mode) {
        case SelectionMode::WORD:
            normalize_word_selection();
            break;
        case SelectionMode::LINE:
            normalize_line_selection();
            break;
        default:
            break;
    }
    
    update_selection_highlight();
    notify_selection_changed();
}

void SelectionManager::update_selection(int row, int col) {
    if (!selecting_) return;
    
    selection_end_row_ = row;
    selection_end_col_ = col;
    
    last_mouse_row_ = row;
    last_mouse_col_ = col;
    
    // Apply selection mode adjustments
    switch (selection_mode_) {
        case SelectionMode::WORD:
            normalize_word_selection();
            break;
        case SelectionMode::LINE:
            normalize_line_selection();
            break;
        default:
            break;
    }
    
    update_selection_highlight();
    notify_selection_changed();
}

void SelectionManager::end_selection() {
    if (!selecting_) return;
    
    selecting_ = false;
    
    // Finalize selection
    selection_.active = true;
    selection_.start_row = selection_start_row_;
    selection_.start_col = selection_start_col_;
    selection_.end_row = selection_end_row_;
    selection_.end_col = selection_end_col_;
    
    ensure_selection_order();
    
    // Auto-copy if enabled
    if (config_.copy_on_select) {
        copy_selection();
    }
    
    notify_selection_changed();
}

void SelectionManager::clear_selection() {
    if (!selection_.active && !selecting_) return;
    
    selection_.active = false;
    selecting_ = false;
    
    clear_selection_highlight();
    notify_selection_cleared();
}

void SelectionManager::select_all() {
    if (!terminal_ || !buffer_) return;
    
    selection_.active = true;
    selection_.start_row = 0;
    selection_.start_col = 0;
    selection_.end_row = terminal_->rows() - 1;
    selection_.end_col = terminal_->cols() - 1;
    
    update_selection_highlight();
    notify_selection_changed();
}

void SelectionManager::select_word(int row, int col) {
    if (!terminal_ || !buffer_) return;
    
    auto boundaries = find_word_boundaries(row, col);
    
    selection_start_row_ = row;
    selection_start_col_ = boundaries.first;
    selection_end_row_ = row;
    selection_end_col_ = boundaries.second;
    
    selection_mode_ = SelectionMode::WORD;
    
    selection_.active = true;
    selection_.start_row = selection_start_row_;
    selection_.start_col = selection_start_col_;
    selection_.end_row = selection_end_row_;
    selection_.end_col = selection_end_col_;
    
    update_selection_highlight();
    notify_selection_changed();
}

void SelectionManager::select_line(int row) {
    if (!terminal_ || !buffer_) return;
    
    selection_start_row_ = row;
    selection_start_col_ = 0;
    selection_end_row_ = row;
    selection_end_col_ = terminal_->cols() - 1;
    
    selection_mode_ = SelectionMode::LINE;
    
    selection_.active = true;
    selection_.start_row = selection_start_row_;
    selection_.start_col = selection_start_col_;
    selection_.end_row = selection_end_row_;
    selection_.end_col = selection_end_col_;
    
    update_selection_highlight();
    notify_selection_changed();
}

bool SelectionManager::is_position_selected(int row, int col) const {
    if (!selection_.active) return false;
    
    Selection normalized = normalize_selection();
    
    if (row < normalized.start_row || row > normalized.end_row) {
        return false;
    }
    
    if (row == normalized.start_row && col < normalized.start_col) {
        return false;
    }
    
    if (row == normalized.end_row && col > normalized.end_col) {
        return false;
    }
    
    return true;
}

bool SelectionManager::is_cell_selected(int row, int col) const {
    return is_position_selected(row, col);
}

std::string SelectionManager::get_selected_text() const {
    return extract_selected_text(false);
}

std::string SelectionManager::get_selected_text_formatted() const {
    return extract_selected_text(true);
}

void SelectionManager::copy_selection() {
    std::string text = get_selected_text();
    // In a real implementation, this would copy to system clipboard
    std::cout << "Copied to clipboard: " << text << std::endl;
}

void SelectionManager::copy_selection_formatted() {
    std::string text = get_selected_text_formatted();
    // In a real implementation, this would copy formatted text to clipboard
    std::cout << "Copied formatted to clipboard: " << text << std::endl;
}

void SelectionManager::paste(const std::string& text) {
    // In a real implementation, this would paste text to terminal
    // For now, we'll just write it to the terminal
    if (terminal_) {
        terminal_->write(text);
    }
}

void SelectionManager::extend_selection(int row, int col) {
    if (!selection_.active) return;
    
    selection_end_row_ = row;
    selection_end_col_ = col;
    
    update_selection_highlight();
    notify_selection_changed();
}

void SelectionManager::shrink_selection(int row, int col) {
    // Shrink selection towards the start
    if (!selection_.active) return;
    
    Selection normalized = normalize_selection();
    
    if (row > normalized.end_row || (row == normalized.end_row && col > normalized.end_col)) {
        // Can't shrink beyond current end
        return;
    }
    
    selection_end_row_ = row;
    selection_end_col_ = col;
    
    update_selection_highlight();
    notify_selection_changed();
}

void SelectionManager::move_selection_start(int row, int col) {
    if (!selection_.active) return;
    
    selection_start_row_ = row;
    selection_start_col_ = col;
    
    update_selection_highlight();
    notify_selection_changed();
}

void SelectionManager::move_selection_end(int row, int col) {
    if (!selection_.active) return;
    
    selection_end_row_ = row;
    selection_end_col_ = col;
    
    update_selection_highlight();
    notify_selection_changed();
}

int SelectionManager::find_word_start(int row, int col) const {
    if (!terminal_ || !buffer_) return col;
    
    const TerminalLine& line = buffer_->line(row);
    
    // Find start of current word
    while (col > 0 && is_word_character(line.cell(col - 1).codepoint)) {
        col--;
    }
    
    return col;
}

int SelectionManager::find_word_end(int row, int col) const {
    if (!terminal_ || !buffer_) return col;
    
    const TerminalLine& line = buffer_->line(row);
    int cols = terminal_->cols();
    
    // Find end of current word
    while (col < cols - 1 && is_word_character(line.cell(col).codepoint)) {
        col++;
    }
    
    return col;
}

std::pair<int, int> SelectionManager::find_word_boundaries(int row, int col) const {
    if (!terminal_ || !buffer_) return {col, col};
    
    const TerminalLine& line = buffer_->line(row);
    int cols = terminal_->cols();
    
    // Skip non-word characters to find word start
    while (col < cols && !is_word_character(line.cell(col).codepoint)) {
        col++;
    }
    
    if (col >= cols) {
        // No word found at this position
        return {col, col};
    }
    
    int start = col;
    int end = col;
    
    // Find word start
    while (start > 0 && is_word_character(line.cell(start - 1).codepoint)) {
        start--;
    }
    
    // Find word end
    while (end < cols && is_word_character(line.cell(end).codepoint)) {
        end++;
    }
    
    return {start, end};
}

int SelectionManager::find_line_start(int row) const {
    return 0; // Lines always start at column 0
}

int SelectionManager::find_line_end(int row) const {
    if (!terminal_) return 0;
    return terminal_->cols() - 1;
}

void SelectionManager::render_selection() const {
    if (!selection_.active) return;
    
    // Render selection highlight
    // TODO: Implement selection rendering
}

void SelectionManager::highlight_selection() {
    // Apply selection highlighting to terminal cells
    if (!terminal_ || !buffer_) return;
    
    Selection normalized = normalize_selection();
    
    for (int row = normalized.start_row; row <= normalized.end_row; ++row) {
        int start_col = (row == normalized.start_row) ? normalized.start_col : 0;
        int end_col = (row == normalized.end_row) ? normalized.end_col : terminal_->cols() - 1;
        
        TerminalLine& line = buffer_->line(row);
        
        for (int col = start_col; col <= end_col; ++col) {
            Cell& cell = line.cell(col);
            // Apply selection background color
            cell.attrs.bg_color = config_.selection_background_color;
            line.mark_dirty();
        }
    }
}

void SelectionManager::unhighlight_selection() {
    // Remove selection highlighting from terminal cells
    if (!terminal_ || !buffer_) return;
    
    Selection normalized = normalize_selection();
    
    for (int row = normalized.start_row; row <= normalized.end_row; ++row) {
        int start_col = (row == normalized.start_row) ? normalized.start_col : 0;
        int end_col = (row == normalized.end_row) ? normalized.end_col : terminal_->cols() - 1;
        
        TerminalLine& line = buffer_->line(row);
        
        for (int col = start_col; col <= end_col; ++col) {
            Cell& cell = line.cell(col);
            // Restore original background color
            // TODO: Store original colors
            line.mark_dirty();
        }
    }
}

void SelectionManager::on_selection_changed() {
    save_to_history();
    
    if (on_selection_callback_) {
        on_selection_callback_();
    }
}

void SelectionManager::on_selection_cleared() {
    if (on_cleared_callback_) {
        on_cleared_callback_();
    }
}

void SelectionManager::save_selection() {
    // Save current selection state
    // TODO: Implement selection persistence
}

void SelectionManager::restore_selection() {
    // Restore saved selection state
    // TODO: Implement selection restoration
}

void SelectionManager::clear_saved_selection() {
    // Clear saved selection
    // TODO: Implement saved selection clearing
}

Selection SelectionManager::normalize_selection() const {
    Selection normalized = selection_;
    
    if (normalized.start_row > normalized.end_row || 
        (normalized.start_row == normalized.end_row && normalized.start_col > normalized.end_col)) {
        // Swap start and end
        std::swap(normalized.start_row, normalized.end_row);
        std::swap(normalized.start_col, normalized.end_col);
    }
    
    return normalized;
}

bool SelectionManager::is_selection_empty() const {
    if (!selection_.active) return true;
    
    Selection normalized = normalize_selection();
    return (normalized.start_row == normalized.end_row && 
            normalized.start_col == normalized.end_col);
}

int SelectionManager::selection_length() const {
    if (!selection_.active) return 0;
    
    Selection normalized = normalize_selection();
    int length = 0;
    
    for (int row = normalized.start_row; row <= normalized.end_row; ++row) {
        int start_col = (row == normalized.start_row) ? normalized.start_col : 0;
        int end_col = (row == normalized.end_row) ? normalized.end_col : terminal_->cols() - 1;
        length += end_col - start_col + 1;
        
        if (row < normalized.end_row) {
            length += 1; // Count newline
        }
    }
    
    return length;
}

SelectionDirection SelectionManager::selection_direction() const {
    if (!selection_.active) return SelectionDirection::FORWARD;
    
    if (selection_start_row_ < selection_end_row_ ||
        (selection_start_row_ == selection_end_row_ && selection_start_col_ < selection_end_col_)) {
        return SelectionDirection::FORWARD;
    } else {
        return SelectionDirection::BACKWARD;
    }
}

void SelectionManager::handle_mouse_down(int row, int col, int button, bool shift, bool ctrl, bool alt) {
    mouse_down_ = true;
    mouse_down_row_ = row;
    mouse_down_col_ = col;
    mouse_down_button_ = button;
    
    if (button == 0) { // Left button
        if (shift && selection_.active) {
            // Extend selection
            extend_selection(row, col);
        } else {
            // Start new selection
            SelectionMode mode = SelectionMode::NORMAL;
            
            if (config_.word_selection_on_double_click && is_double_click()) {
                mode = SelectionMode::WORD;
            } else if (config_.line_selection_on_triple_click && is_triple_click()) {
                mode = SelectionMode::LINE;
            }
            
            start_selection(row, col, mode);
        }
    } else if (button == 2) { // Right button
        if (config_.right_click_to_select) {
            start_selection(row, col, SelectionMode::NORMAL);
        }
    }
}

void SelectionManager::handle_mouse_move(int row, int col, bool shift, bool ctrl, bool alt) {
    if (!mouse_down_) return;
    
    if (mouse_down_button_ == 0) { // Left button
        update_selection(row, col);
    }
}

void SelectionManager::handle_mouse_up(int row, int col, int button) {
    if (!mouse_down_) return;
    
    mouse_down_ = false;
    
    if (button == 0) { // Left button
        end_selection();
    }
}

void SelectionManager::handle_key_event(const std::string& key, bool shift, bool ctrl, bool alt) {
    if (!selection_.active) return;
    
    if (key == "Escape") {
        clear_selection();
    } else if (key == "c" && ctrl) {
        copy_selection();
    } else if (key == "v" && ctrl) {
        // Paste would be handled by terminal
    }
}

void SelectionManager::save_to_history() {
    // Save current selection to history
    if (!selection_.active) return;
    
    history_.selections.push_back(selection_);
    history_.current_index = history_.selections.size() - 1;
    
    // Limit history size
    if (history_.selections.size() > 100) {
        history_.selections.erase(history_.selections.begin());
        history_.current_index--;
    }
}

void SelectionManager::undo_selection() {
    if (history_.selections.empty()) return;
    
    if (history_.current_index > 0) {
        history_.current_index--;
        selection_ = history_.selections[history_.current_index];
        update_selection_highlight();
        notify_selection_changed();
    }
}

void SelectionManager::redo_selection() {
    if (history_.selections.empty()) return;
    
    if (history_.current_index < history_.selections.size() - 1) {
        history_.current_index++;
        selection_ = history_.selections[history_.current_index];
        update_selection_highlight();
        notify_selection_changed();
    }
}

void SelectionManager::clear_history() {
    history_.selections.clear();
    history_.current_index = 0;
}

void SelectionManager::select_search_result(const SearchResult& result) {
    selection_.active = true;
    selection_.start_row = result.start_row;
    selection_.start_col = result.start_col;
    selection_.end_row = result.end_row;
    selection_.end_col = result.end_col;
    
    update_selection_highlight();
    notify_selection_changed();
}

std::vector<SelectionManager::SearchResult> SelectionManager::find_all(const std::string& text, bool case_sensitive, bool whole_word) {
    std::vector<SearchResult> results;
    
    if (!terminal_ || !buffer_ || text.empty()) return results;
    
    for (int row = 0; row < terminal_->rows(); ++row) {
        const TerminalLine& line = buffer_->line(row);
        std::string line_text = line.text();
        
        size_t pos = 0;
        while ((pos = line_text.find(text, pos)) != std::string::npos) {
            if (whole_word) {
                // Check if it's a whole word
                bool is_word_start = (pos == 0) || !is_word_character(line_text[pos - 1]);
                bool is_word_end = (pos + text.length() == line_text.length()) || 
                                   !is_word_character(line_text[pos + text.length()]);
                
                if (!is_word_start || !is_word_end) {
                    pos++;
                    continue;
                }
            }
            
            SearchResult result;
            result.start_row = row;
            result.start_col = pos;
            result.end_row = row;
            result.end_col = pos + text.length();
            result.text = text;
            
            results.push_back(result);
            pos += text.length();
        }
    }
    
    return results;
}

std::string SelectionManager::get_selection_description() const {
    if (!selection_.active) return "No selection";
    
    std::ostringstream oss;
    Selection normalized = normalize_selection();
    
    oss << "Selection from row " << normalized.start_row << ", col " << normalized.start_col <<
           " to row " << normalized.end_row << ", col " << normalized.end_col <<
           " (" << selection_length() << " characters)";
    
    return oss.str();
}

std::string SelectionManager::get_selection_at_cursor(int row, int col) const {
    if (!is_position_selected(row, col)) return "";
    
    // Get the word at cursor position
    auto boundaries = find_word_boundaries(row, col);
    
    if (!terminal_ || !buffer_) return "";
    
    const TerminalLine& line = buffer_->line(row);
    return line.text_range(boundaries.first, boundaries.second);
}

// Private helper methods
bool SelectionManager::is_word_character(char32_t ch) const {
    return std::isalnum(static_cast<char>(ch)) || ch == '_';
}

bool SelectionManager::is_line_break_character(char32_t ch) const {
    return ch == '\n' || ch == '\r';
}

bool SelectionManager::is_whitespace(char32_t ch) const {
    return std::isspace(static_cast<char>(ch));
}

void SelectionManager::normalize_word_selection() {
    if (!terminal_ || !buffer_) return;
    
    auto boundaries = find_word_boundaries(selection_end_row_, selection_end_col_);
    
    selection_end_col_ = boundaries.second;
    
    // If this is the start of selection, find word start
    if (selection_start_row_ == selection_end_row_ && 
        selection_start_col_ == selection_end_col_) {
        selection_start_col_ = boundaries.first;
    }
}

void SelectionManager::normalize_line_selection() {
    selection_start_col_ = 0;
    selection_end_col_ = terminal_->cols() - 1;
}

void SelectionManager::ensure_selection_order() {
    if (selection_start_row_ > selection_end_row_ ||
        (selection_start_row_ == selection_end_row_ && selection_start_col_ > selection_end_col_)) {
        std::swap(selection_start_row_, selection_end_row_);
        std::swap(selection_start_col_, selection_end_col_);
    }
}

std::string SelectionManager::extract_selected_text(bool formatted) const {
    if (!selection_.active || !terminal_ || !buffer_) return "";
    
    Selection normalized = normalize_selection();
    std::ostringstream oss;
    
    for (int row = normalized.start_row; row <= normalized.end_row; ++row) {
        const TerminalLine& line = buffer_->line(row);
        
        int start_col = (row == normalized.start_row) ? normalized.start_col : 0;
        int end_col = (row == normalized.end_row) ? normalized.end_col : terminal_->cols() - 1;
        
        std::string line_text = line.text_range(start_col, end_col + 1);
        
        if (formatted) {
            // Apply formatting based on cell attributes
            // TODO: Implement formatted text extraction
            oss << line_text;
        } else {
            oss << line_text;
        }
        
        if (row < normalized.end_row) {
            oss << "\n";
        }
    }
    
    return oss.str();
}

std::string SelectionManager::format_selection_text(const std::string& text) const {
    // Apply formatting to selection text
    // TODO: Implement text formatting
    return text;
}

void SelectionManager::update_selection_highlight() {
    unhighlight_selection();
    highlight_selection();
}

void SelectionManager::clear_selection_highlight() {
    unhighlight_selection();
}

void SelectionManager::notify_selection_changed() {
    if (on_selection_callback_) {
        on_selection_callback_();
    }
}

void SelectionManager::notify_selection_cleared() {
    if (on_cleared_callback_) {
        on_cleared_callback_();
    }
}

bool SelectionManager::is_double_click() const {
    // Check if this is a double click
    // TODO: Implement double click detection
    return false;
}

bool SelectionManager::is_triple_click() const {
    // Check if this is a triple click
    // TODO: Implement triple click detection
    return false;
}

} // namespace spiritty