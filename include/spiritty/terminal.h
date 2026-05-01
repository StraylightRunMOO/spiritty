#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <map>

namespace spiritty {

// Forward declarations
class TerminalBuffer;
class ANSIParser;
class WebGLRenderer;
class SelectionManager;

// Terminal configuration
struct TerminalOptions {
    int cols = 80;
    int rows = 24;
    int scrollback_lines = 10000;
    bool allow_bold = true;
    bool allow_italic = true;
    bool allow_underline = true;
    bool allow_strikethrough = true;
    bool word_wrap = true;
    bool cursor_blink = true;
    int cursor_style = 1; // 0=block, 1=underline, 2=bar
    std::string font_family = "monospace";
    int font_size = 14;
    std::string theme = "default";
    bool gpu_acceleration = true;
    
    // Colors (24-bit RGB)
    uint32_t color_foreground = 0xFFFFFFFF;
    uint32_t color_background = 0x00000000;
    uint32_t color_cursor = 0xFFFFFFFF;
    uint32_t color_selection = 0x80808080;
    
    // ANSI color palette (16 colors + 256 color support)
    std::vector<uint32_t> color_palette = {
        0xFF000000, // black
        0xFF800000, // red
        0xFF008000, // green
        0xFF808000, // yellow
        0xFF000080, // blue
        0xFF800080, // magenta
        0xFF008080, // cyan
        0xFFC0C0C0, // white
        0xFF808080, // bright black
        0xFFFF0000, // bright red
        0xFF00FF00, // bright green
        0xFFFFFF00, // bright yellow
        0xFF0000FF, // bright blue
        0xFFFF00FF, // bright magenta
        0xFF00FFFF, // bright cyan
        0xFFFFFFFF  // bright white
    };
};

// Cell attributes
struct CellAttributes {
    uint32_t fg_color = 0xFFFFFFFF;
    uint32_t bg_color = 0x00000000;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool dim = false;
    bool reverse = false;
    bool hidden = false;
    int underline_style = 0; // 0=none, 1=single, 2=double, 3=curly
    
    bool operator==(const CellAttributes& other) const {
        return fg_color == other.fg_color &&
               bg_color == other.bg_color &&
               bold == other.bold &&
               italic == other.italic &&
               underline == other.underline &&
               strikethrough == other.strikethrough &&
               dim == other.dim &&
               reverse == other.reverse &&
               hidden == other.hidden &&
               underline_style == other.underline_style;
    }
    
    bool operator!=(const CellAttributes& other) const {
        return !(*this == other);
    }
    
    void reset() {
        fg_color = 0xFFFFFFFF;
        bg_color = 0x00000000;
        bold = false;
        italic = false;
        underline = false;
        strikethrough = false;
        dim = false;
        reverse = false;
        hidden = false;
        underline_style = 0;
    }
};

// Terminal cell
struct Cell {
    char32_t codepoint = 0;
    uint32_t width = 1; // 1 for normal, 2 for wide chars
    CellAttributes attrs;
    
    bool is_wide_continuation() const { return codepoint == 0; }
    bool is_empty() const { return codepoint == 0 || codepoint == ' '; }
};

// Cursor position
struct Cursor {
    int row = 0;
    int col = 0;
    bool visible = true;
    bool blink_state = true;
    CellAttributes attrs;
};

// Selection range
struct Selection {
    int start_row = 0;
    int start_col = 0;
    int end_row = 0;
    int end_col = 0;
    bool active = false;
};

// Terminal events
struct TerminalEvent {
    enum Type {
        DATA,
        RESIZE,
        TITLE,
        BELL,
        CURSOR_MOVE,
        SCROLL,
        SELECTION_CHANGE,
        MOUSE_EVENT
    } type;
    
    std::string data;
    int rows = 0;
    int cols = 0;
    int row = 0;
    int col = 0;
};

// Main terminal class
class Terminal {
public:
    Terminal();
    explicit Terminal(const TerminalOptions& options);
    ~Terminal();
    
    // Core API - similar to xterm.js but cleaner
    void open(const std::string& container_id);
    void write(const std::string& data);
    void write(const char* data, size_t len);
    void resize(int cols, int rows);
    void clear();
    void reset();
    void focus();
    void blur();
    void destroy();
    
    // Buffer operations
    std::string get_selection() const;
    void clear_selection();
    void select_all();
    void scroll_to_top();
    void scroll_to_bottom();
    void scroll_to_line(int line);
    
    // Cursor operations
    void move_cursor(int row, int col);
    void save_cursor();
    void restore_cursor();
    void hide_cursor();
    void show_cursor();
    
    // Options management
    void set_option(const std::string& key, const std::string& value);
    std::string get_option(const std::string& key) const;
    void refresh();
    
    // Event handling
    using EventCallback = std::function<void(const TerminalEvent&)>;
    void on_event(const std::string& event_type, EventCallback callback);
    
    // Telnet/PTY support
    void set_terminal_type(const std::string& term_type);
    void set_window_size(int width, int height);
    void set_charset(const std::string& charset);
    
    // Internal methods (called by implementation)
    void render();
    void update_cursor();
    void queue_render();
    void dispatch_event(const TerminalEvent& event);
    
    // Getters
    int cols() const { return options_.cols; }
    int rows() const { return options_.rows; }
    const Cursor& cursor() const { return cursor_; }
    const Selection& selection() const { return selection_; }
    TerminalOptions& options() { return options_; }
    const TerminalOptions& options() const { return options_; }
    
    // JavaScript bridge methods
    void js_write(const std::string& data);
    void js_resize(int cols, int rows);
    void js_key_event(const std::string& key, bool pressed);
    void js_mouse_event(int row, int col, int button, bool pressed);
    std::string js_get_selection() const;
    void js_set_selection(int start_row, int start_col, int end_row, int end_col);
    
private:
    TerminalOptions options_;
    std::unique_ptr<TerminalBuffer> buffer_;
    std::unique_ptr<ANSIParser> parser_;
    
public:
    TerminalBuffer* buffer() { return buffer_.get(); }
    const TerminalBuffer* buffer() const { return buffer_.get(); }
    
private:
    std::unique_ptr<WebGLRenderer> renderer_;
    std::unique_ptr<SelectionManager> selection_manager_;
    
    Cursor cursor_;
    Cursor saved_cursor_;
    Selection selection_;
    
    bool opened_;
    bool focused_;
    bool needs_render_;
    
    std::string container_id_;
    std::string terminal_type_;
    
    // Event callbacks
    std::map<std::string, std::vector<EventCallback>> event_callbacks_;
    
    // Internal methods
    void initialize();
    void process_input(const char* data, size_t len);
    void update_scroll_region();
    void handle_mouse_event(int row, int col, int button, bool pressed);
    
    // Dirty tracking
    void mark_dirty();
    
    // Non-copyable
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
};

} // namespace spiritty