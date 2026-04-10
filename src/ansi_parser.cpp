#include "spiritty/ansi_parser.h"
#include "spiritty/terminal.h"
#include "spiritty/buffer.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace spiritty {

ANSIParser::ANSIParser(Terminal* terminal) 
    : terminal_(terminal), 
      buffer_(nullptr), // Will be set when terminal is fully initialized
      state_(ParserState::GROUND),
      current_gr_(),
      window_title_("Spiritty") {
    init_color_palette();
    reset();
}

void ANSIParser::reset() {
    state_ = ParserState::GROUND;
    saved_state_ = ParserState::GROUND;
    params_.clear();
    intermediates_.clear();
    osc_string_.clear();
    current_gr_.reset();
    modes_ = TerminalModes();
    saved_cursor_ = SavedCursor();
    saved_terminal_state_ = SavedTerminalState();
    cursor_stack_.clear();
}

void ANSIParser::parse(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = static_cast<uint8_t>(data[i]);
        process_byte(byte);
    }
}

void ANSIParser::process_byte(uint8_t byte) {
    ParserAction action = ParserAction::NONE;
    
    switch (state_) {
        case ParserState::GROUND:
            if (byte == 0x1B) { // ESC
                action = ParserAction::CLEAR;
                transition(ParserState::ESCAPE, action);
            } else if (byte >= 0x00 && byte <= 0x1F) { // C0
                action = ParserAction::EXECUTE;
                execute_action(action, byte);
            } else if (byte == 0x7F) { // DEL
                // Ignore
            } else { // Printable character
                action = ParserAction::PRINT;
                execute_action(action, byte);
            }
            break;
            
        case ParserState::ESCAPE:
            if (byte >= 0x40 && byte <= 0x5F) { // Final character
                action = ParserAction::ESC_DISPATCH;
                execute_action(action, byte);
                transition(ParserState::GROUND, action);
            } else if (byte >= 0x30 && byte <= 0x3F) { // Intermediate
                action = ParserAction::COLLECT;
                execute_action(action, byte);
                transition(ParserState::ESCAPE_INTERMEDIATE, action);
            } else if (byte == 0x5B) { // [
                action = ParserAction::CLEAR;
                transition(ParserState::CSI_ENTRY, action);
            } else if (byte == 0x5D) { // ]
                action = ParserAction::OSC_START;
                transition(ParserState::OSC_STRING, action);
            } else if (byte == 0x50) { // P
                action = ParserAction::CLEAR;
                transition(ParserState::DCS_ENTRY, action);
            } else if (byte >= 0x58 && byte <= 0x5E) { // X, Y, Z, [, \\, ], ^
                action = ParserAction::HOOK;
                transition(ParserState::SOS_PM_APC_STRING, action);
            } else {
                // Invalid sequence, return to ground
                transition(ParserState::GROUND, ParserAction::NONE);
            }
            break;
            
        case ParserState::ESCAPE_INTERMEDIATE:
            if (byte >= 0x40 && byte <= 0x5F) { // Final character
                action = ParserAction::ESC_DISPATCH;
                execute_action(action, byte);
                transition(ParserState::GROUND, action);
            } else if (byte >= 0x30 && byte <= 0x3F) { // Intermediate
                action = ParserAction::COLLECT;
                execute_action(action, byte);
            } else {
                // Invalid sequence
                transition(ParserState::GROUND, ParserAction::NONE);
            }
            break;
            
        case ParserState::CSI_ENTRY:
            if (byte >= 0x40 && byte <= 0x7E) { // Final character
                action = ParserAction::CSI_DISPATCH;
                execute_action(action, byte);
                transition(ParserState::GROUND, action);
            } else if (byte >= 0x30 && byte <= 0x3F) { // Parameter
                action = ParserAction::PARAM;
                execute_action(action, byte);
                transition(ParserState::CSI_PARAM, action);
            } else if (byte >= 0x20 && byte <= 0x2F) { // Intermediate
                action = ParserAction::COLLECT;
                execute_action(action, byte);
                transition(ParserState::CSI_INTERMEDIATE, action);
            } else if (byte == 0x3A) { // :
                transition(ParserState::CSI_IGNORE, ParserAction::NONE);
            } else {
                // Invalid sequence
                transition(ParserState::GROUND, ParserAction::NONE);
            }
            break;
            
        case ParserState::CSI_PARAM:
            if (byte >= 0x40 && byte <= 0x7E) { // Final character
                action = ParserAction::CSI_DISPATCH;
                execute_action(action, byte);
                transition(ParserState::GROUND, action);
            } else if (byte >= 0x30 && byte <= 0x39) { // Digit
                action = ParserAction::PARAM;
                execute_action(action, byte);
            } else if (byte == 0x3B) { // ;
                action = ParserAction::PARAM;
                execute_action(action, byte);
            } else if (byte >= 0x20 && byte <= 0x2F) { // Intermediate
                action = ParserAction::COLLECT;
                execute_action(action, byte);
                transition(ParserState::CSI_INTERMEDIATE, action);
            } else if (byte == 0x3A || (byte >= 0x3C && byte <= 0x3F)) {
                transition(ParserState::CSI_IGNORE, ParserAction::NONE);
            } else {
                // Invalid sequence
                transition(ParserState::GROUND, ParserAction::NONE);
            }
            break;
            
        case ParserState::CSI_INTERMEDIATE:
            if (byte >= 0x40 && byte <= 0x7E) { // Final character
                action = ParserAction::CSI_DISPATCH;
                execute_action(action, byte);
                transition(ParserState::GROUND, action);
            } else if (byte >= 0x20 && byte <= 0x2F) { // Intermediate
                action = ParserAction::COLLECT;
                execute_action(action, byte);
            } else {
                // Invalid sequence
                transition(ParserState::GROUND, ParserAction::NONE);
            }
            break;
            
        case ParserState::CSI_IGNORE:
            if (byte >= 0x40 && byte <= 0x7E) { // Final character
                transition(ParserState::GROUND, ParserAction::NONE);
            }
            // Ignore everything else
            break;
            
        case ParserState::OSC_STRING:
            if (byte == 0x07 || (byte == 0x5C && !osc_string_.empty() && osc_string_.back() == 0x1B)) {
                // OSC terminator (BEL or ESC\\)
                action = ParserAction::OSC_END;
                execute_action(action, byte);
                transition(ParserState::GROUND, action);
            } else if (byte == 0x1B) {
                // ESC might be part of ST
                osc_string_ += byte;
            } else {
                action = ParserAction::OSC_PUT;
                execute_action(action, byte);
            }
            break;
            
        case ParserState::DCS_ENTRY:
            if (byte >= 0x40 && byte <= 0x7E) { // Final character
                action = ParserAction::HOOK;
                execute_action(action, byte);
                transition(ParserState::DCS_PASSTHROUGH, action);
            } else if (byte >= 0x30 && byte <= 0x3F) { // Parameter
                action = ParserAction::PARAM;
                execute_action(action, byte);
                transition(ParserState::DCS_PARAM, action);
            } else if (byte >= 0x20 && byte <= 0x2F) { // Intermediate
                action = ParserAction::COLLECT;
                execute_action(action, byte);
                transition(ParserState::DCS_INTERMEDIATE, action);
            } else {
                // Invalid sequence
                transition(ParserState::GROUND, ParserAction::NONE);
            }
            break;
            
        case ParserState::DCS_PARAM:
        case ParserState::DCS_INTERMEDIATE:
            if (byte == 0x1B) { // ESC might start ST
                transition(ParserState::GROUND, ParserAction::NONE);
            } else {
                // Collect parameters/intermediates
                action = ParserAction::PUT;
                execute_action(action, byte);
            }
            break;
            
        case ParserState::DCS_PASSTHROUGH:
            if (byte == 0x1B) { // ESC might start ST
                transition(ParserState::GROUND, ParserAction::NONE);
            } else {
                action = ParserAction::PUT;
                execute_action(action, byte);
            }
            break;
            
        case ParserState::SOS_PM_APC_STRING:
            if (byte == 0x1B) { // ESC might start ST
                transition(ParserState::GROUND, ParserAction::NONE);
            } else {
                action = ParserAction::PUT;
                execute_action(action, byte);
            }
            break;
    }
}

void ANSIParser::transition(ParserState new_state, ParserAction action) {
    state_ = new_state;
}

void ANSIParser::execute_action(ParserAction action, uint8_t byte) {
    switch (action) {
        case ParserAction::PRINT:
            handle_print(byte);
            break;
        case ParserAction::EXECUTE:
            handle_c0(byte);
            break;
        case ParserAction::CLEAR:
            clear_params();
            intermediates_.clear();
            break;
        case ParserAction::COLLECT:
            intermediates_ += static_cast<char>(byte);
            break;
        case ParserAction::PARAM:
            if (byte == ';') {
                add_param(0); // Default parameter
            } else if (std::isdigit(byte)) {
                if (params_.empty()) {
                    params_.push_back(0);
                }
                params_.back() = params_.back() * 10 + (byte - '0');
            }
            break;
        case ParserAction::ESC_DISPATCH:
            handle_esc_dispatch(byte);
            break;
        case ParserAction::CSI_DISPATCH:
            handle_csi_dispatch(byte);
            break;
        case ParserAction::HOOK:
            // Handle DCS/OSC start
            break;
        case ParserAction::PUT:
            // Handle DCS/OSC data
            if (state_ == ParserState::OSC_STRING) {
                osc_string_ += static_cast<char>(byte);
            }
            break;
        case ParserAction::UNHOOK:
            // Handle DCS/OSC end
            break;
        case ParserAction::OSC_START:
            osc_string_.clear();
            break;
        case ParserAction::OSC_PUT:
            osc_string_ += static_cast<char>(byte);
            break;
        case ParserAction::OSC_END:
            handle_osc();
            break;
        case ParserAction::NONE:
            break;
    }
}

void ANSIParser::handle_print(uint8_t byte) {
    if (!terminal_ || !buffer_) return;
    
    // Handle character sets
    char32_t codepoint = byte;
    
    // Apply current graphics rendition
    CellAttributes attrs = current_gr_.to_cell_attributes();
    
    // Write to buffer
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    
    buffer_->write_char(row, col, codepoint, attrs);
    
    // Move cursor
    terminal_->move_cursor(row, col + 1);
}

void ANSIParser::handle_c0(uint8_t ch) {
    if (!terminal_) return;
    
    switch (ch) {
        case 0x00: // NUL
            break;
        case 0x07: // BEL
            // Terminal bell
            break;
        case 0x08: // BS
            {
                int row = terminal_->cursor().row;
                int col = terminal_->cursor().col;
                terminal_->move_cursor(row, std::max(0, col - 1));
            }
            break;
        case 0x09: // HT
            {
                int row = terminal_->cursor().row;
                int col = terminal_->cursor().col;
                // Move to next tab stop (every 8 columns)
                int next_tab = ((col / 8) + 1) * 8;
                terminal_->move_cursor(row, std::min(next_tab, terminal_->cols() - 1));
            }
            break;
        case 0x0A: // LF
            {
                int row = terminal_->cursor().row;
                int col = terminal_->cursor().col;
                terminal_->move_cursor(row + 1, col);
            }
            break;
        case 0x0B: // VT
        case 0x0C: // FF
            // Treat as LF
            {
                int row = terminal_->cursor().row;
                int col = terminal_->cursor().col;
                terminal_->move_cursor(row + 1, col);
            }
            break;
        case 0x0D: // CR
            {
                int row = terminal_->cursor().row;
                terminal_->move_cursor(row, 0);
            }
            break;
        case 0x0E: // SO
            // Shift out - use G1 character set
            use_charset(1);
            break;
        case 0x0F: // SI
            // Shift in - use G0 character set
            use_charset(0);
            break;
        case 0x18: // CAN
        case 0x1A: // SUB
            // Cancel sequence
            transition(ParserState::GROUND, ParserAction::NONE);
            break;
        case 0x1B: // ESC
            // Handled in state machine
            break;
        case 0x1F: // US
            // Unit separator - ignore
            break;
    }
}

void ANSIParser::handle_esc_dispatch(uint8_t byte) {
    if (!terminal_) return;
    
    std::string sequence = "\x1B" + intermediates_ + static_cast<char>(byte);
    
    switch (byte) {
        case 'D': // IND - Index
            esc_index();
            break;
        case 'M': // RI - Reverse Index
            esc_reverse_index();
            break;
        case 'E': // NEL - Next Line
            esc_next_line();
            break;
        case 'H': // HTS - Horizontal Tab Set
            esc_horizontal_tab_set();
            break;
        case '7': // DECSC - Save Cursor
            save_cursor_state();
            break;
        case '8': // DECRC - Restore Cursor
            restore_cursor_state();
            break;
        case '=': // DECKPAM - Keypad Application Mode
            modes_.keypad_application_mode = true;
            break;
        case '>': // DECKPNM - Keypad Numeric Mode
            modes_.keypad_application_mode = false;
            break;
        case 'c': // RIS - Reset to Initial State
            terminal_->reset();
            break;
        case 'n': // LS2 - Locking Shift 2
            use_charset(2);
            break;
        case 'o': // LS3 - Locking Shift 3
            use_charset(3);
            break;
        case '|': // LS3R - Locking Shift 3 Right
            use_charset(3);
            break;
        case '}': // LS2R - Locking Shift 2 Right
            use_charset(2);
            break;
        case '~': // LS1R - Locking Shift 1 Right
            use_charset(1);
            break;
    }
    
    // Handle character set designators
    if (!intermediates_.empty()) {
        if (intermediates_[0] == '(') { // Designate G0
            esc_designate_g0(byte - 0x40);
        } else if (intermediates_[0] == ')') { // Designate G1
            esc_designate_g1(byte - 0x40);
        } else if (intermediates_[0] == '*') { // Designate G2
            esc_designate_g2(byte - 0x40);
        } else if (intermediates_[0] == '+') { // Designate G3
            esc_designate_g3(byte - 0x40);
        }
    }
}

void ANSIParser::handle_csi_dispatch(uint8_t byte) {
    if (!terminal_) return;
    
    switch (byte) {
        case '@': // ICH - Insert Character
            csi_ich(get_param(0, 1));
            break;
        case 'A': // CUU - Cursor Up
            csi_cuu(get_param(0, 1));
            break;
        case 'B': // CUD - Cursor Down
            csi_cud(get_param(0, 1));
            break;
        case 'C': // CUF - Cursor Forward
            csi_cuf(get_param(0, 1));
            break;
        case 'D': // CUB - Cursor Back
            csi_cub(get_param(0, 1));
            break;
        case 'E': // CNL - Cursor Next Line
            csi_cnl(get_param(0, 1));
            break;
        case 'F': // CPL - Cursor Previous Line
            csi_cpl(get_param(0, 1));
            break;
        case 'G': // CHA - Cursor Horizontal Absolute
            csi_cha(get_param(0, 1));
            break;
        case 'H': // CUP - Cursor Position
            csi_cup(get_param(0, 1), get_param(1, 1));
            break;
        case 'J': // ED - Erase in Display
            csi_ed(get_param(0, 0));
            break;
        case 'K': // EL - Erase in Line
            csi_el(get_param(0, 0));
            break;
        case 'L': // IL - Insert Line
            csi_il(get_param(0, 1));
            break;
        case 'M': // DL - Delete Line
            csi_dl(get_param(0, 1));
            break;
        case 'P': // DCH - Delete Character
            csi_dch(get_param(0, 1));
            break;
        case 'S': // SU - Scroll Up
            terminal_->buffer()->scroll_up(get_param(0, 1));
            break;
        case 'T': // SD - Scroll Down
            terminal_->buffer()->scroll_down(get_param(0, 1));
            break;
        case 'X': // ECH - Erase Character
            csi_ech(get_param(0, 1));
            break;
        case 'Z': // CBT - Cursor Backward Tabulation
            {
                int count = get_param(0, 1);
                int row = terminal_->cursor().row;
                int col = terminal_->cursor().col;
                int prev_tab = ((col / 8) - count) * 8;
                terminal_->move_cursor(row, std::max(0, prev_tab));
            }
            break;
        case 'c': // DA - Device Attributes
            // Send terminal identification
            break;
        case 'd': // VPA - Vertical Position Absolute
            csi_vpa(get_param(0, 1));
            break;
        case 'f': // HVP - Horizontal and Vertical Position
            csi_hvp(get_param(0, 1), get_param(1, 1));
            break;
        case 'g': // TBC - Tab Clear
            csi_tbc(get_param(0, 0));
            break;
        case 'h': // SM - Set Mode
            csi_sm(get_param(0, 0));
            break;
        case 'l': // RM - Reset Mode
            csi_rm(get_param(0, 0));
            break;
        case 'm': // SGR - Select Graphic Rendition
            csi_sgr(params_);
            break;
        case 'n': // DSR - Device Status Report
            csi_dsr(get_param(0, 0));
            break;
        case 'q': // DECLL - Load LEDs
            // Ignore - LEDs not supported
            break;
        case 'r': // DECSTBM - Set Top and Bottom Margins
            csi_decstbm(get_param(0, 1), get_param(1, -1));
            break;
        case 's': // DECSLRM - Set Left and Right Margins (or save cursor)
            if (!intermediates_.empty() && intermediates_[0] == '?') {
                // DEC private mode
                csi_decslrm(get_param(0, 1), get_param(1, -1));
            } else {
                // Save cursor
                save_cursor_state();
            }
            break;
        case 't': // Window manipulation
            csi_cts(get_param(0, 0));
            break;
        case 'u': // Restore cursor
            restore_cursor_state();
            break;
        case '`': // HPA - Horizontal Position Absolute
            csi_cha(get_param(0, 1));
            break;
        case '~': // Delete character under cursor
            csi_dch_top(get_param(0, 1));
            break;
    }
}

void ANSIParser::handle_osc() {
    if (osc_string_.empty()) return;
    
    // Parse OSC command
    size_t semicolon_pos = osc_string_.find(';');
    int command = 0;
    std::string data;
    
    if (semicolon_pos != std::string::npos) {
        command = std::stoi(osc_string_.substr(0, semicolon_pos));
        data = osc_string_.substr(semicolon_pos + 1);
    } else {
        command = std::stoi(osc_string_);
    }
    
    switch (command) {
        case 0: // Set window title and icon
        case 1: // Set icon title
        case 2: // Set window title
            set_window_title(data);
            break;
        case 4: // Set color
            // Format: 4;index;rgb:rrrr/gggg/bbbb
            break;
        case 5: // Set special color
            break;
        case 10: // Set foreground color
        case 11: // Set background color
        case 12: // Set cursor color
            set_dynamic_color(command - 10, parse_color_from_string(data));
            break;
        case 52: // Clipboard operation
            // Format: 52;clipboard;data
            break;
        case 104: // Reset color
            break;
        case 105: // Reset special color
            break;
        case 110: // Reset foreground color
        case 111: // Reset background color
        case 112: // Reset cursor color
            set_dynamic_color(command - 110, 0xFFFFFFFF); // Reset to default
            break;
        case 133: // Shell integration
            break;
        case 777: // Notification
            break;
    }
}

// CSI Command Implementations
void ANSIParser::csi_cup(int row, int col) {
    if (!terminal_) return;
    
    // Convert from 1-based to 0-based
    row = std::max(1, row) - 1;
    col = std::max(1, col) - 1;
    
    // Apply origin mode
    if (modes_.origin_mode) {
        // TODO: Apply scroll margins
    }
    
    terminal_->move_cursor(row, col);
}

void ANSIParser::csi_ed(int param) {
    if (!terminal_ || !buffer_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    
    switch (param) {
        case 0: // Clear from cursor to end of screen
            buffer_->clear_range(row, col, rows_ - 1, cols_ - 1);
            break;
        case 1: // Clear from start of screen to cursor
            buffer_->clear_range(0, 0, row, col);
            break;
        case 2: // Clear entire screen
            buffer_->clear();
            terminal_->move_cursor(0, 0);
            break;
        case 3: // Clear scrollback
            buffer_->clear_history();
            break;
    }
}

void ANSIParser::csi_el(int param) {
    if (!terminal_ || !buffer_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    
    switch (param) {
        case 0: // Clear from cursor to end of line
            buffer_->clear_range(row, col, row, cols_ - 1);
            break;
        case 1: // Clear from start of line to cursor
            buffer_->clear_range(row, 0, row, col);
            break;
        case 2: // Clear entire line
            buffer_->clear_line(row);
            break;
    }
}

void ANSIParser::csi_sgr(const std::vector<int>& params) {
    if (params.empty()) {
        current_gr_.reset();
        return;
    }
    
    for (size_t i = 0; i < params.size(); ++i) {
        int param = params[i];
        
        if (param == 0) {
            current_gr_.reset();
        } else if (param == 1) {
            current_gr_.bold = true;
        } else if (param == 2) {
            current_gr_.faint = true;
        } else if (param == 3) {
            current_gr_.italic = true;
        } else if (param == 4) {
            if (i + 1 < params.size() && params[i + 1] == 3) {
                current_gr_.curly_underline = true;
                i++; // Skip the 3
            } else {
                current_gr_.underline = true;
            }
        } else if (param == 5 || param == 6) {
            current_gr_.blink = true;
        } else if (param == 7) {
            current_gr_.reverse = true;
        } else if (param == 8) {
            current_gr_.invisible = true;
        } else if (param == 9) {
            current_gr_.strikethrough = true;
        } else if (param == 21) {
            current_gr_.double_underline = true;
        } else if (param == 22) {
            current_gr_.bold = false;
            current_gr_.faint = false;
        } else if (param == 23) {
            current_gr_.italic = false;
        } else if (param == 24) {
            current_gr_.underline = false;
            current_gr_.double_underline = false;
            current_gr_.curly_underline = false;
        } else if (param == 25) {
            current_gr_.blink = false;
        } else if (param == 27) {
            current_gr_.reverse = false;
        } else if (param == 28) {
            current_gr_.invisible = false;
        } else if (param == 29) {
            current_gr_.strikethrough = false;
        } else if (param >= 30 && param <= 37) {
            // Foreground color (8 colors)
            current_gr_.fg_color_mode = 0;
            current_gr_.fg_color = terminal_->options().color_palette[param - 30];
        } else if (param == 38) {
            // Extended foreground color
            if (i + 2 < params.size() && params[i + 1] == 5) {
                // 256-color
                int color_index = params[i + 2];
                current_gr_.fg_color_mode = 38;
                current_gr_.fg_color = get_color_palette(color_index);
                i += 2;
            } else if (i + 4 < params.size() && params[i + 1] == 2) {
                // 24-bit color
                uint8_t r = params[i + 2];
                uint8_t g = params[i + 3];
                uint8_t b = params[i + 4];
                current_gr_.fg_color_mode = 38;
                current_gr_.fg_color = (0xFF << 24) | (r << 16) | (g << 8) | b;
                i += 4;
            }
        } else if (param == 39) {
            // Default foreground color
            current_gr_.fg_color_mode = 0;
            current_gr_.fg_color = terminal_->options().color_foreground;
        } else if (param >= 40 && param <= 47) {
            // Background color (8 colors)
            current_gr_.bg_color_mode = 0;
            current_gr_.bg_color = terminal_->options().color_palette[param - 40];
        } else if (param == 48) {
            // Extended background color
            if (i + 2 < params.size() && params[i + 1] == 5) {
                // 256-color
                int color_index = params[i + 2];
                current_gr_.bg_color_mode = 48;
                current_gr_.bg_color = get_color_palette(color_index);
                i += 2;
            } else if (i + 4 < params.size() && params[i + 1] == 2) {
                // 24-bit color
                uint8_t r = params[i + 2];
                uint8_t g = params[i + 3];
                uint8_t b = params[i + 4];
                current_gr_.bg_color_mode = 48;
                current_gr_.bg_color = (0xFF << 24) | (r << 16) | (g << 8) | b;
                i += 4;
            }
        } else if (param == 49) {
            // Default background color
            current_gr_.bg_color_mode = 0;
            current_gr_.bg_color = terminal_->options().color_background;
        } else if (param >= 90 && param <= 97) {
            // Bright foreground colors
            current_gr_.fg_color_mode = 0;
            current_gr_.fg_color = terminal_->options().color_palette[param - 90 + 8];
        } else if (param >= 100 && param <= 107) {
            // Bright background colors
            current_gr_.bg_color_mode = 0;
            current_gr_.bg_color = terminal_->options().color_palette[param - 100 + 8];
        }
    }
}

void ANSIParser::csi_decset(int param) {
    set_dec_mode(param, true);
}

void ANSIParser::csi_decrst(int param) {
    set_dec_mode(param, false);
}

void ANSIParser::csi_sm(int param) {
    set_mode(param, true);
}

void ANSIParser::csi_rm(int param) {
    set_mode(param, false);
}

void ANSIParser::csi_dsr(int param) {
    // Device status report
    std::string response;
    
    switch (param) {
        case 5: // Status report
            response = "\x1B[0n"; // OK
            break;
        case 6: // Cursor position report
            if (terminal_) {
                int row = terminal_->cursor().row + 1;
                int col = terminal_->cursor().col + 1;
                response = "\x1B[" + std::to_string(row) + ";" + std::to_string(col) + "R";
            }
            break;
    }
    
    if (!response.empty()) {
        TerminalEvent event;
        event.type = TerminalEvent::DATA;
        event.data = response;
        terminal_->dispatch_event(event);
    }
}

void ANSIParser::csi_tbc(int param) {
    // Tab clear - not implemented
}

void ANSIParser::csi_cts(int param) {
    // Window manipulation
    if (!terminal_) return;
    
    switch (param) {
        case 8: // Resize window
            // CSI 8 ; height ; width t
            if (params_.size() >= 3) {
                int height = params_[1];
                int width = params_[2];
                terminal_->set_window_size(width, height);
            }
            break;
    }
}

void ANSIParser::csi_decstbm(int top, int bottom) {
    if (!terminal_) return;
    
    int rows = terminal_->rows();
    
    if (top < 1) top = 1;
    if (bottom < 0 || bottom > rows) bottom = rows;
    
    // Set scroll margins
    // TODO: Implement scroll margins in buffer
}

void ANSIParser::csi_decslrm(int left, int right) {
    if (!terminal_) return;
    
    int cols = terminal_->cols();
    
    if (left < 1) left = 1;
    if (right < 0 || right > cols) right = cols;
    
    // Set horizontal margins
    // TODO: Implement horizontal margins in buffer
}

void ANSIParser::csi_cha(int col) {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    terminal_->move_cursor(row, std::max(0, col - 1));
}

void ANSIParser::csi_vpa(int row) {
    if (!terminal_) return;
    
    int col = terminal_->cursor().col;
    terminal_->move_cursor(std::max(0, row - 1), col);
}

void ANSIParser::csi_hvp(int row, int col) {
    csi_cup(row, col); // Same as CUP
}

void ANSIParser::csi_cuu(int count) {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    terminal_->move_cursor(std::max(0, row - count), col);
}

void ANSIParser::csi_cud(int count) {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    int max_row = terminal_->rows() - 1;
    terminal_->move_cursor(std::min(max_row, row + count), col);
}

void ANSIParser::csi_cuf(int count) {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    int max_col = terminal_->cols() - 1;
    terminal_->move_cursor(row, std::min(max_col, col + count));
}

void ANSIParser::csi_cub(int count) {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    terminal_->move_cursor(row, std::max(0, col - count));
}

void ANSIParser::csi_cnl(int count) {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    int max_row = terminal_->rows() - 1;
    terminal_->move_cursor(std::min(max_row, row + count), 0);
}

void ANSIParser::csi_cpl(int count) {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    terminal_->move_cursor(std::max(0, row - count), 0);
}

void ANSIParser::csi_ich(int count) {
    if (!terminal_ || !buffer_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    buffer_->insert_chars(row, col, count);
}

void ANSIParser::csi_dch(int count) {
    if (!terminal_ || !buffer_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    buffer_->delete_chars(row, col, count);
}

void ANSIParser::csi_il(int count) {
    if (!terminal_ || !buffer_) return;
    
    int row = terminal_->cursor().row;
    buffer_->insert_line(row, count);
}

void ANSIParser::csi_dl(int count) {
    if (!terminal_ || !buffer_) return;
    
    int row = terminal_->cursor().row;
    buffer_->delete_line(row, count);
}

void ANSIParser::csi_ech(int count) {
    if (!terminal_ || !buffer_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    buffer_->clear_range(row, col, row, col + count);
}

void ANSIParser::csi_dch_top(int count) {
    csi_dch(count); // Same as DCH
}

void ANSIParser::csi_decscusr(int style) {
    if (!terminal_) return;
    
    // Set cursor style
    // 0, 1 = blinking block
    // 2 = steady block
    // 3 = blinking underline
    // 4 = steady underline
    // 5 = blinking bar
    // 6 = steady bar
    
    terminal_->options().cursor_style = (style == 0 || style == 1) ? 0 : 
                                       (style == 2) ? 0 :
                                       (style == 3 || style == 4) ? 1 : 2;
    terminal_->options().cursor_blink = (style == 0 || style == 1 || style == 3 || style == 5);
}

void ANSIParser::csi_decscusr_rgb(int style, uint32_t color) {
    // Set cursor style with color
    csi_decscusr(style);
    terminal_->options().color_cursor = color;
}

// ESC command implementations
void ANSIParser::esc_designate_g0(int charset) {
    select_charset(0, charset);
}

void ANSIParser::esc_designate_g1(int charset) {
    select_charset(1, charset);
}

void ANSIParser::esc_designate_g2(int charset) {
    select_charset(2, charset);
}

void ANSIParser::esc_designate_g3(int charset) {
    select_charset(3, charset);
}

void ANSIParser::esc_index() {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    terminal_->move_cursor(row + 1, col);
}

void ANSIParser::esc_reverse_index() {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    int col = terminal_->cursor().col;
    terminal_->move_cursor(row - 1, col);
}

void ANSIParser::esc_next_line() {
    if (!terminal_) return;
    
    int row = terminal_->cursor().row;
    terminal_->move_cursor(row + 1, 0);
}

void ANSIParser::esc_horizontal_tab_set() {
    // Set tab stop at current column
    // TODO: Implement tab stops
}

void ANSIParser::esc_ri() {
    esc_reverse_index();
}

void ANSIParser::esc_decaln() {
    // Screen alignment test - fill screen with E's
    if (!terminal_ || !buffer_) return;
    
    for (int row = 0; row < terminal_->rows(); ++row) {
        for (int col = 0; col < terminal_->cols(); ++col) {
            buffer_->write_char(row, col, 'E', CellAttributes());
        }
    }
}

void ANSIParser::esc_decdwl(int dir) {
    // Double width line - not implemented
}

void ANSIParser::esc_decbi() {
    // Back index - not implemented
}

// Helper methods
void ANSIParser::set_dec_mode(int param, bool value) {
    switch (param) {
        case 1: // DECCKM - Application Cursor Keys
            modes_.cursor_keys_mode = value;
            break;
        case 3: // DECCOLM - Column Mode
            // 80 or 132 columns
            break;
        case 5: // DECSCNM - Screen Mode
            modes_.reverse_video = value;
            break;
        case 6: // DECOM - Origin Mode
            modes_.origin_mode = value;
            break;
        case 7: // DECAWM - Auto Wrap Mode
            modes_.auto_wrap = value;
            break;
        case 8: // DECARM - Auto Repeat Mode
            modes_.auto_repeat = value;
            break;
        case 9: // DECINLM - Interlace
            break;
        case 12: // ATT610 - Start blinking cursor
            modes_.cursor_blink = value;
            break;
        case 25: // DECTCEM - Text Cursor Enable Mode
            modes_.cursor_visible = value;
            break;
        case 47: // Use alternate screen buffer
            // TODO: Implement alternate screen buffer
            break;
        case 1000: // Mouse reporting
            modes_.mouse_reporting = value;
            break;
        case 1002: // Mouse button event reporting
            modes_.mouse_reporting = value;
            break;
        case 1003: // Mouse any event reporting
            modes_.mouse_reporting = value;
            break;
        case 1004: // Focus reporting
            modes_.focus_reporting = value;
            break;
        case 1005: // Mouse UTF8 mode
            modes_.mouse_utf8_mode = value;
            break;
        case 1006: // Mouse SGR mode
            modes_.mouse_sgr_mode = value;
            break;
        case 1015: // Mouse urxvt mode
            break;
        case 1034: // 8-bit input
            break;
        case 1039: // Alternate send/receive
            break;
        case 1047: // Use alternate screen buffer
            // TODO: Implement alternate screen buffer
            break;
        case 1048: // Save cursor
            if (value) {
                save_cursor_state();
            } else {
                restore_cursor_state();
            }
            break;
        case 1049: // Save cursor and use alternate screen
            if (value) {
                save_cursor_state();
                // TODO: Switch to alternate screen
            } else {
                // TODO: Switch back to normal screen
                restore_cursor_state();
            }
            break;
        case 2004: // Bracketed paste
            modes_.bracketed_paste = value;
            break;
    }
}

void ANSIParser::set_mode(int param, bool value) {
    switch (param) {
        case 2: // KAM - Keyboard Action Mode
            break;
        case 4: // IRM - Insert Mode
            modes_.insert_mode = value;
            break;
        case 20: // LNM - Line Feed/New Line Mode
            modes_.new_line_mode = value;
            break;
    }
}

void ANSIParser::save_cursor_state() {
    if (!terminal_) return;
    
    saved_cursor_.row = terminal_->cursor().row;
    saved_cursor_.col = terminal_->cursor().col;
    saved_cursor_.gr = current_gr_;
    saved_cursor_.modes = modes_;
}

void ANSIParser::restore_cursor_state() {
    if (!terminal_) return;
    
    terminal_->move_cursor(saved_cursor_.row, saved_cursor_.col);
    current_gr_ = saved_cursor_.gr;
    modes_ = saved_cursor_.modes;
}

void ANSIParser::save_terminal_state() {
    // Save terminal state (modes, margins, etc.)
    saved_terminal_state_.modes = modes_;
    // TODO: Save margins
}

void ANSIParser::restore_terminal_state() {
    // Restore terminal state
    modes_ = saved_terminal_state_.modes;
    // TODO: Restore margins
}

void ANSIParser::select_charset(int g, int charset) {
    switch (g) {
        case 0: modes_.g0_charset = charset; break;
        case 1: modes_.g1_charset = charset; break;
        case 2: modes_.g2_charset = charset; break;
        case 3: modes_.g3_charset = charset; break;
    }
}

void ANSIParser::use_charset(int g) {
    modes_.current_charset = g;
}

void ANSIParser::single_shift(int g) {
    modes_.single_shift = true;
    modes_.current_charset = g;
}

void ANSIParser::set_mouse_reporting(bool enabled, bool sgr_mode) {
    modes_.mouse_reporting = enabled;
    modes_.mouse_sgr_mode = sgr_mode;
}

void ANSIParser::set_mouse_utf8_mode(bool enabled) {
    modes_.mouse_utf8_mode = enabled;
}

void ANSIParser::set_color_palette(int index, uint32_t color) {
    if (index >= 0 && index < 256) {
        if (static_cast<int>(color_palette_.size()) <= index) {
            color_palette_.resize(index + 1);
        }
        color_palette_[index] = color;
    }
}

uint32_t ANSIParser::get_color_palette(int index) const {
    if (index >= 0 && index < static_cast<int>(color_palette_.size())) {
        return color_palette_[index];
    }
    
    // Default colors
    if (index >= 0 && index < 16) {
        return terminal_->options().color_palette[index];
    }
    
    // Extended colors (16-231 are 6x6x6 color cube, 232-255 are grayscale)
    if (index >= 16 && index <= 231) {
        // 6x6x6 color cube
        int idx = index - 16;
        int r = (idx / 36) * 51;
        int g = ((idx / 6) % 6) * 51;
        int b = (idx % 6) * 51;
        return (0xFF << 24) | (r << 16) | (g << 8) | b;
    } else if (index >= 232 && index <= 255) {
        // Grayscale
        int gray = 8 + (index - 232) * 10;
        return (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
    }
    
    return 0xFFFFFFFF; // Default to white
}

void ANSIParser::set_window_title(const std::string& title) {
    window_title_ = title;
    // TODO: Notify terminal of title change
}

void ANSIParser::set_dynamic_color(int color_type, uint32_t color) {
    switch (color_type) {
        case 0: // Foreground
            terminal_->options().color_foreground = color;
            break;
        case 1: // Background
            terminal_->options().color_background = color;
            break;
        case 2: // Cursor
            terminal_->options().color_cursor = color;
            break;
    }
}

void ANSIParser::init_color_palette() {
    color_palette_.resize(16);
    for (int i = 0; i < 16; ++i) {
        color_palette_[i] = terminal_->options().color_palette[i];
    }
}

int ANSIParser::get_param(size_t index, int default_val) const {
    if (index < params_.size()) {
        return params_[index];
    }
    return default_val;
}

void ANSIParser::clear_params() {
    params_.clear();
}

void ANSIParser::add_param(int value) {
    params_.push_back(value);
}

uint32_t ANSIParser::parse_color_from_string(const std::string& str) {
    // Parse color from various formats
    // rgb:rrrr/gggg/bbbb
    // #rrggbb
    // color names
    
    if (str.substr(0, 4) == "rgb:") {
        // X11 rgb format
        size_t r_end = str.find('/', 4);
        size_t g_end = str.find('/', r_end + 1);
        
        if (r_end != std::string::npos && g_end != std::string::npos) {
            int r = std::stoi(str.substr(4, r_end - 4), nullptr, 16) >> 8;
            int g = std::stoi(str.substr(r_end + 1, g_end - r_end - 1), nullptr, 16) >> 8;
            int b = std::stoi(str.substr(g_end + 1), nullptr, 16) >> 8;
            
            return (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
    
    return 0xFFFFFFFF; // Default
}

CellAttributes GraphicsRendition::to_cell_attributes() const {
    CellAttributes attrs;
    
    attrs.fg_color = fg_color;
    attrs.bg_color = bg_color;
    attrs.bold = bold;
    attrs.italic = italic;
    attrs.underline = underline || double_underline || curly_underline;
    attrs.strikethrough = strikethrough;
    attrs.dim = faint;
    attrs.reverse = reverse;
    attrs.hidden = invisible;
    
    // Handle underline style
    if (double_underline) {
        attrs.underline_style = 2;
    } else if (curly_underline) {
        attrs.underline_style = 3;
    } else if (underline) {
        attrs.underline_style = 1;
    } else {
        attrs.underline_style = 0;
    }
    
    return attrs;
}

} // namespace spiritty