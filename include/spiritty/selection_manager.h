#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "terminal.h"

namespace spiritty {

// Forward declarations
class Terminal;
class TerminalBuffer;

// Selection mode
enum class SelectionMode {
    NONE,
    NORMAL,         // Character-by-character selection
    WORD,           // Word-based selection (double-click)
    LINE            // Line-based selection (triple-click)
};

// Selection direction
enum class SelectionDirection {
    FORWARD,        // Start to end
    BACKWARD        // End to start
};

// Selection manager - handles text selection, copying, and related operations
class SelectionManager {
public:
    explicit SelectionManager(Terminal* terminal);
    
    // Selection state
    bool has_selection() const { return selection_.active; }
    const Selection& selection() const { return selection_; }
    
    // Selection operations
    void start_selection(int row, int col, SelectionMode mode = SelectionMode::NORMAL);
    void update_selection(int row, int col);
    void end_selection();
    void clear_selection();
    void select_all();
    void select_word(int row, int col);
    void select_line(int row);
    
    // Selection queries
    bool is_position_selected(int row, int col) const;
    bool is_cell_selected(int row, int col) const;
    std::string get_selected_text() const;
    std::string get_selected_text_formatted() const;
    
    // Clipboard operations
    void copy_selection();
    void copy_selection_formatted();
    void paste(const std::string& text);
    
    // Selection manipulation
    void extend_selection(int row, int col);
    void shrink_selection(int row, int col);
    void move_selection_start(int row, int col);
    void move_selection_end(int row, int col);
    
    // Selection mode
    SelectionMode selection_mode() const { return selection_mode_; }
    void set_selection_mode(SelectionMode mode) { selection_mode_ = mode; }
    
    // Word selection helpers
    int find_word_start(int row, int col) const;
    int find_word_end(int row, int col) const;
    std::pair<int, int> find_word_boundaries(int row, int col) const;
    
    // Line selection helpers
    int find_line_start(int row) const;
    int find_line_end(int row) const;
    
    // Selection rendering
    void render_selection() const;
    void highlight_selection();
    void unhighlight_selection();
    
    // Selection events
    void on_selection_changed();
    void on_selection_cleared();
    
    // Selection persistence
    void save_selection();
    void restore_selection();
    void clear_saved_selection();
    
    // Selection utilities
    Selection normalize_selection() const;
    bool is_selection_empty() const;
    int selection_length() const;
    SelectionDirection selection_direction() const;
    
    // Mouse interaction
    void handle_mouse_down(int row, int col, int button, bool shift, bool ctrl, bool alt);
    void handle_mouse_move(int row, int col, bool shift, bool ctrl, bool alt);
    void handle_mouse_up(int row, int col, int button);
    
    // Keyboard interaction
    void handle_key_event(const std::string& key, bool shift, bool ctrl, bool alt);
    
    // Configuration
    struct Config {
        bool word_selection_on_double_click = true;
        bool line_selection_on_triple_click = true;
        bool select_to_copy = false; // Select text automatically copies to clipboard
        bool copy_on_select = false; // Copy when selection is made
        bool right_click_to_select = false;
        bool rectangular_selection = false;
        bool selection_wraps_lines = true;
        int selection_padding = 2;
        uint32_t selection_color = 0x80808080;
        uint32_t selection_background_color = 0x40404080;
    };
    
    const Config& config() const { return config_; }
    void set_config(const Config& config) { config_ = config; }
    
    // History and undo
    struct SelectionHistory {
        std::vector<Selection> selections;
        size_t current_index = 0;
    };
    
    void save_to_history();
    void undo_selection();
    void redo_selection();
    void clear_history();
    
    // Search integration
    struct SearchResult {
        int start_row, start_col;
        int end_row, end_col;
        std::string text;
    };
    
    void select_search_result(const SearchResult& result);
    std::vector<SearchResult> find_all(const std::string& text, bool case_sensitive = false, bool whole_word = false);
    
    // Accessibility
    std::string get_selection_description() const;
    std::string get_selection_at_cursor(int row, int col) const;
    
    // Performance optimization
    void set_lazy_selection(bool lazy) { lazy_selection_ = lazy; }
    bool is_lazy_selection() const { return lazy_selection_; }
    
    // Event callbacks
    using SelectionCallback = std::function<void()>;
    void on_selection(const SelectionCallback& callback) { on_selection_callback_ = callback; }
    void on_cleared(const SelectionCallback& callback) { on_cleared_callback_ = callback; }

private:
    Terminal* terminal_;
    TerminalBuffer* buffer_;
    
    Selection selection_;
    SelectionMode selection_mode_;
    Config config_;
    
    // Selection tracking
    bool selecting_;
    int selection_start_row_;
    int selection_start_col_;
    int selection_end_row_;
    int selection_end_col_;
    int last_mouse_row_;
    int last_mouse_col_;
    
    // Selection history
    SelectionHistory history_;
    
    // Mouse state
    bool mouse_down_;
    int mouse_down_row_;
    int mouse_down_col_;
    int mouse_down_button_;
    
    // Performance optimization
    bool lazy_selection_;
    bool selection_dirty_;
    
    // Callbacks
    SelectionCallback on_selection_callback_;
    SelectionCallback on_cleared_callback_;
    
    // Private methods
    bool is_word_character(char32_t ch) const;
    bool is_line_break_character(char32_t ch) const;
    bool is_whitespace(char32_t ch) const;
    bool is_double_click() const;
    bool is_triple_click() const;
    
    void normalize_word_selection();
    void normalize_line_selection();
    void ensure_selection_order();
    
    std::string extract_selected_text(bool formatted = false) const;
    std::string format_selection_text(const std::string& text) const;
    
    void update_selection_highlight();
    void clear_selection_highlight();
    
    void notify_selection_changed();
    void notify_selection_cleared();
    
    // Non-copyable
    SelectionManager(const SelectionManager&) = delete;
    SelectionManager& operator=(const SelectionManager&) = delete;
};

} // namespace spiritty