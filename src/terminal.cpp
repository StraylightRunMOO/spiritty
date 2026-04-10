#include "spiritty/terminal.h"
#include "spiritty/buffer.h"
#include "spiritty/ansi_parser.h"
#include "spiritty/webgl_renderer.h"
#include "spiritty/selection_manager.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace spiritty {

Terminal::Terminal() : Terminal(TerminalOptions()) {}

Terminal::Terminal(const TerminalOptions& options) 
    : options_(options), 
      opened_(false), 
      focused_(false), 
      needs_render_(false),
      terminal_type_("xterm-256color") {
    initialize();
}

Terminal::~Terminal() {
    destroy();
}

void Terminal::initialize() {
    // Create components
    buffer_ = std::make_unique<TerminalBuffer>(this, options_.rows, options_.cols, options_.scrollback_lines);
    parser_ = std::make_unique<ANSIParser>(this);
    renderer_ = std::make_unique<WebGLRenderer>(this);
    selection_manager_ = std::make_unique<SelectionManager>(this);
    
    // Initialize cursor
    cursor_.row = 0;
    cursor_.col = 0;
    cursor_.visible = true;
    cursor_.blink_state = true;
    
    // Initialize saved cursor
    saved_cursor_ = cursor_;
    
    // Initialize selection
    selection_.active = false;
    
    // Initialize renderer
    if (options_.gpu_acceleration) {
        renderer_->initialize({});
    }
}

void Terminal::open(const std::string& container_id) {
    container_id_ = container_id;
    opened_ = true;
    
    // Create JavaScript bridge
    std::ostringstream js_code;
    js_code << "window.spiritty = window.spiritty || {};\n";
    js_code << "window.spiritty.terminals = window.spiritty.terminals || {};\n";
    js_code << "window.spiritty.terminals['" << container_id << "'] = {\n";
    js_code << "  write: function(data) { Module.ccall('spiritty_js_write', null, ['string', 'string'], ['" << container_id << "', data]); },\n";
    js_code << "  resize: function(cols, rows) { Module.ccall('spiritty_js_resize', null, ['string', 'number', 'number'], ['" << container_id << "', cols, rows]); },\n";
    js_code << "  keyEvent: function(key, pressed) { Module.ccall('spiritty_js_key_event', null, ['string', 'string', 'boolean'], ['" << container_id << "', key, pressed]); },\n";
    js_code << "  mouseEvent: function(row, col, button, pressed) { Module.ccall('spiritty_js_mouse_event', null, ['string', 'number', 'number', 'number', 'boolean'], ['" << container_id << "', row, col, button, pressed]); },\n";
    js_code << "  getSelection: function() { return Module.ccall('spiritty_js_get_selection', 'string', ['string'], ['" << container_id << "']); },\n";
    js_code << "  setSelection: function(start_row, start_col, end_row, end_col) { Module.ccall('spiritty_js_set_selection', null, ['string', 'number', 'number', 'number', 'number'], ['" << container_id << "', start_row, start_col, end_row, end_col]); }\n";
    js_code << "};\n";
    
    // In a real implementation, this would be injected into the page
    std::cout << "JavaScript bridge created for container: " << container_id << std::endl;
    std::cout << js_code.str() << std::endl;
    
    // Initial render
    render();
}

void Terminal::write(const std::string& data) {
    write(data.c_str(), data.length());
}

void Terminal::write(const char* data, size_t len) {
    if (!opened_) return;
    
    process_input(data, len);
    
    // Queue render if needed
    if (needs_render_) {
        render();
        needs_render_ = false;
    }
}

void Terminal::resize(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    
    // Save current content
    std::string buffer_content = buffer_->get_text();
    
    // Resize components
    options_.cols = cols;
    options_.rows = rows;
    
    buffer_->resize(cols, rows);
    
    if (renderer_) {
        renderer_->resize(cols * renderer_->cell_width(), rows * renderer_->cell_height());
    }
    
    // Ensure cursor is in bounds
    cursor_.row = std::min(cursor_.row, rows - 1);
    cursor_.col = std::min(cursor_.col, cols - 1);
    
    // Send resize event
    TerminalEvent event;
    event.type = TerminalEvent::RESIZE;
    event.rows = rows;
    event.cols = cols;
    dispatch_event(event);
    
    // Re-render
    render();
}

void Terminal::clear() {
    buffer_->clear();
    cursor_.row = 0;
    cursor_.col = 0;
    mark_dirty();
}

void Terminal::reset() {
    // Reset terminal state
    clear();
    parser_->reset();
    selection_manager_->clear_selection();
    
    // Reset cursor
    cursor_ = Cursor();
    saved_cursor_ = Cursor();
    
    // Reset modes
    modes_ = TerminalModes();
    
    mark_dirty();
}

void Terminal::focus() {
    focused_ = true;
    cursor_.visible = true;
    mark_dirty();
}

void Terminal::blur() {
    focused_ = false;
    cursor_.blink_state = true; // Reset blink state
    mark_dirty();
}

void Terminal::destroy() {
    if (renderer_) {
        renderer_->shutdown();
    }
    
    buffer_.reset();
    parser_.reset();
    renderer_.reset();
    selection_manager_.reset();
    
    opened_ = false;
}

std::string Terminal::get_selection() const {
    return selection_manager_->get_selected_text();
}

void Terminal::clear_selection() {
    selection_manager_->clear_selection();
}

void Terminal::select_all() {
    selection_manager_->select_all();
}

void Terminal::scroll_to_top() {
    buffer_->set_viewport(0);
    mark_dirty();
}

void Terminal::scroll_to_bottom() {
    buffer_->set_viewport(buffer_->history_size());
    mark_dirty();
}

void Terminal::scroll_to_line(int line) {
    buffer_->set_viewport(line);
    mark_dirty();
}

void Terminal::move_cursor(int row, int col) {
    if (row >= 0 && row < options_.rows && col >= 0 && col < options_.cols) {
        cursor_.row = row;
        cursor_.col = col;
        mark_dirty();
    }
}

void Terminal::save_cursor() {
    saved_cursor_ = cursor_;
    saved_cursor_.attrs = parser_->modes().cursor_visible ? cursor_.attrs : CellAttributes();
}

void Terminal::restore_cursor() {
    cursor_ = saved_cursor_;
    mark_dirty();
}

void Terminal::hide_cursor() {
    cursor_.visible = false;
    mark_dirty();
}

void Terminal::show_cursor() {
    cursor_.visible = true;
    mark_dirty();
}

void Terminal::set_option(const std::string& key, const std::string& value) {
    // Handle various options
    if (key == "fontSize") {
        options_.font_size = std::stoi(value);
        if (renderer_) {
            renderer_->load_font(options_.font_family, options_.font_size);
        }
    } else if (key == "fontFamily") {
        options_.font_family = value;
        if (renderer_) {
            renderer_->load_font(options_.font_family, options_.font_size);
        }
    } else if (key == "theme") {
        options_.theme = value;
        // TODO: Apply theme
    } else if (key == "cursorBlink") {
        options_.cursor_blink = (value == "true");
    } else if (key == "wordWrap") {
        options_.word_wrap = (value == "true");
        buffer_->reflow_lines();
    }
    
    mark_dirty();
}

std::string Terminal::get_option(const std::string& key) const {
    if (key == "fontSize") {
        return std::to_string(options_.font_size);
    } else if (key == "fontFamily") {
        return options_.font_family;
    } else if (key == "theme") {
        return options_.theme;
    } else if (key == "cursorBlink") {
        return options_.cursor_blink ? "true" : "false";
    } else if (key == "wordWrap") {
        return options_.word_wrap ? "true" : "false";
    }
    return "";
}

void Terminal::refresh() {
    mark_dirty();
    render();
}

void Terminal::on_event(const std::string& event_type, EventCallback callback) {
    event_callbacks_[event_type].push_back(callback);
}

void Terminal::set_terminal_type(const std::string& term_type) {
    terminal_type_ = term_type;
    // Set TERM environment variable equivalent
}

void Terminal::set_window_size(int width, int height) {
    // Calculate cols/rows based on pixel dimensions
    if (renderer_) {
        int cols = width / renderer_->cell_width();
        int rows = height / renderer_->cell_height();
        resize(cols, rows);
    }
}

void Terminal::set_charset(const std::string& charset) {
    // Handle character set selection
    parser_->select_charset(0, 0); // UTF-8 default
}

void Terminal::render() {
    if (!opened_ || !renderer_) return;
    
    renderer_->begin_frame();
    renderer_->render();
    renderer_->end_frame();
    
    needs_render_ = false;
}

void Terminal::update_cursor() {
    // Update cursor blink state
    static auto last_blink_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_blink_time).count();
    
    if (elapsed > 500) { // Blink every 500ms
        cursor_.blink_state = !cursor_.blink_state;
        last_blink_time = now;
        mark_dirty();
    }
}

void Terminal::queue_render() {
    needs_render_ = true;
}

void Terminal::process_input(const char* data, size_t len) {
    parser_->parse(data, len);
}

void Terminal::update_scroll_region() {
    // Update scroll region based on current modes
    // This would handle scroll margins
}

void Terminal::handle_mouse_event(int row, int col, int button, bool pressed) {
    selection_manager_->handle_mouse_down(row, col, button, false, false, false);
    
    // Send mouse report if enabled
    if (parser_->modes().mouse_reporting) {
        // Generate mouse report sequence
    }
}

void Terminal::dispatch_event(const TerminalEvent& event) {
    auto it = event_callbacks_.find(event.type);
    if (it != event_callbacks_.end()) {
        for (const auto& callback : it->second) {
            callback(event);
        }
    }
}

void Terminal::mark_dirty() {
    needs_render_ = true;
    if (buffer_) {
        buffer_->mark_all_dirty();
    }
}

// JavaScript bridge implementations
void Terminal::js_write(const std::string& data) {
    write(data);
}

void Terminal::js_resize(int cols, int rows) {
    resize(cols, rows);
}

void Terminal::js_key_event(const std::string& key, bool pressed) {
    // Handle key events
    if (pressed) {
        // Generate appropriate escape sequence based on current modes
        std::string sequence;
        
        if (key == "Enter") {
            sequence = "\r";
        } else if (key == "Backspace") {
            sequence = "\x7F";
        } else if (key == "Tab") {
            sequence = "\t";
        } else if (key == "Escape") {
            sequence = "\x1B";
        } else if (key == "ArrowUp") {
            if (parser_->modes().cursor_keys_mode) {
                sequence = "\x1BOA";
            } else {
                sequence = "\x1B[A";
            }
        } else if (key == "ArrowDown") {
            if (parser_->modes().cursor_keys_mode) {
                sequence = "\x1BOB";
            } else {
                sequence = "\x1B[B";
            }
        } else if (key == "ArrowRight") {
            if (parser_->modes().cursor_keys_mode) {
                sequence = "\x1BOC";
            } else {
                sequence = "\x1B[C";
            }
        } else if (key == "ArrowLeft") {
            if (parser_->modes().cursor_keys_mode) {
                sequence = "\x1BOD";
            } else {
                sequence = "\x1B[D";
            }
        } else if (key.length() == 1) {
            sequence = key;
        }
        
        if (!sequence.empty()) {
            TerminalEvent event;
            event.type = TerminalEvent::DATA;
            event.data = sequence;
            dispatch_event(event);
        }
    }
}

void Terminal::js_mouse_event(int row, int col, int button, bool pressed) {
    handle_mouse_event(row, col, button, pressed);
}

std::string Terminal::js_get_selection() const {
    return get_selection();
}

void Terminal::js_set_selection(int start_row, int start_col, int end_row, int end_col) {
    selection_manager_->start_selection(start_row, start_col);
    selection_manager_->update_selection(end_row, end_col);
    selection_manager_->end_selection();
}

} // namespace spiritty