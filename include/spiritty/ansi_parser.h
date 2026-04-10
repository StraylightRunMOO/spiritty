#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include "terminal.h"

namespace spiritty {

// Forward declarations
class Terminal;
class TerminalBuffer;

// ANSI Parser States
enum class ParserState {
    GROUND,             // Normal text
    ESCAPE,             // After ESC
    ESCAPE_INTERMEDIATE, // After ESC +
    CSI_ENTRY,          // After ESC [
    CSI_PARAM,          // Collecting CSI parameters
    CSI_INTERMEDIATE,   // After CSI params
    CSI_IGNORE,         // Invalid CSI sequence
    OSC_STRING,         // Operating System Command
    SOS_PM_APC_STRING,  // Other string types
    DCS_ENTRY,          // Device Control String
    DCS_PARAM,          // DCS parameters
    DCS_INTERMEDIATE,   // DCS intermediate
    DCS_IGNORE,         // Invalid DCS
    DCS_PASSTHROUGH     // DCS data
};

// ANSI Parser Actions
enum class ParserAction {
    NONE,
    PRINT,              // Print character
    EXECUTE,            // Execute C0/C1 control
    CLEAR,              // Clear parameters
    COLLECT,            // Collect intermediate chars
    PARAM,              // Collect parameter
    ESC_DISPATCH,       // Dispatch ESC sequence
    CSI_DISPATCH,       // Dispatch CSI sequence
    HOOK,               // Start DCS/OSC
    PUT,                // Add to string
    UNHOOK,             // End DCS/OSC
    OSC_START,          // Start OSC
    OSC_PUT,            // Add to OSC
    OSC_END             // End OSC
};

// VT100/ANSI modes
struct TerminalModes {
    bool insert_mode = false;
    bool auto_wrap = true;
    bool origin_mode = false;
    bool cursor_visible = true;
    bool cursor_blink = true;
    bool reverse_video = false;
    bool new_line_mode = false;
    bool auto_repeat = true;
    bool cursor_keys_mode = false;
    bool keypad_application_mode = false;
    bool bracketed_paste = false;
    bool focus_reporting = false;
    bool mouse_reporting = false;
    bool mouse_sgr_mode = false;
    bool mouse_utf8_mode = false;
    bool mouse_alt_scroll = false;
    
    // Character sets
    int g0_charset = 0;
    int g1_charset = 0;
    int g2_charset = 0;
    int g3_charset = 0;
    int current_charset = 0; // 0=G0, 1=G1, 2=G2, 3=G3
    bool single_shift = false;
};

// Graphics Rendition (SGR) attributes
struct GraphicsRendition {
    int fg_color_mode = 0; // 0=default, 38=256, 38;5=256, 38;2;R;G;B=24bit
    int bg_color_mode = 0; // 48=256, 48;5=256, 48;2;R;G;B=24bit
    uint32_t fg_color = 0xFFFFFFFF;
    uint32_t bg_color = 0x00000000;
    bool bold = false;
    bool faint = false;
    bool italic = false;
    bool underline = false;
    bool double_underline = false;
    bool curly_underline = false;
    bool strikethrough = false;
    bool blink = false;
    bool reverse = false;
    bool invisible = false;
    bool crossed_out = false;
    
    void reset() {
        fg_color_mode = 0;
        bg_color_mode = 0;
        fg_color = 0xFFFFFFFF;
        bg_color = 0x00000000;
        bold = false;
        faint = false;
        italic = false;
        underline = false;
        double_underline = false;
        curly_underline = false;
        strikethrough = false;
        blink = false;
        reverse = false;
        invisible = false;
        crossed_out = false;
    }
    
    CellAttributes to_cell_attributes() const;
};

// ANSI Parser - handles VT100/ANSI escape sequences
class ANSIParser {
public:
    explicit ANSIParser(Terminal* terminal);
    
    // Parse input data
    void parse(const char* data, size_t len);
    void parse(const std::string& data) { parse(data.c_str(), data.length()); }
    
    // Reset parser state
    void reset();
    
    // Get/set current modes
    const TerminalModes& modes() const { return modes_; }
    void set_modes(const TerminalModes& modes) { modes_ = modes; }
    
    // Character set handling
    void select_charset(int g, int charset);
    void use_charset(int g);
    void single_shift(int g);
    
    // Mouse reporting
    void set_mouse_reporting(bool enabled, bool sgr_mode = true);
    void set_mouse_utf8_mode(bool enabled);
    
    // Color palette
    void set_color_palette(int index, uint32_t color);
    uint32_t get_color_palette(int index) const;
    
    // Window title
    void set_window_title(const std::string& title);
    std::string window_title() const { return window_title_; }
    
    // Dynamic colors
    void set_dynamic_color(int color_type, uint32_t color);
    
    // Save/restore state
    void save_cursor_state();
    void restore_cursor_state();
    void save_terminal_state();
    void restore_terminal_state();

private:
    Terminal* terminal_;
    TerminalBuffer* buffer_;
    
    // Parser state machine
    ParserState state_;
    ParserState saved_state_;
    
    // Parameters and intermediates
    std::vector<int> params_;
    std::string intermediates_;
    std::string osc_string_;
    
    // Current modes
    TerminalModes modes_;
    GraphicsRendition current_gr_;
    
    // Color palette (256 colors)
    std::vector<uint32_t> color_palette_;
    
    // Window title
    std::string window_title_;
    
    // Saved states
    struct SavedCursor {
        int row = 0;
        int col = 0;
        GraphicsRendition gr;
        TerminalModes modes;
    };
    
    struct SavedTerminalState {
        TerminalModes modes;
        int top_margin = 0;
        int bottom_margin = 0;
        int left_margin = 0;
        int right_margin = 0;
    };
    
    SavedCursor saved_cursor_;
    SavedTerminalState saved_terminal_state_;
    std::vector<SavedCursor> cursor_stack_;
    
    // Character sets
    std::array<std::string, 4> charsets_;
    
    // Parser helpers
    void transition(ParserState new_state, ParserAction action);
    void execute_action(ParserAction action, uint8_t byte);
    
    // Sequence handlers
    void handle_c0(uint8_t ch);
    void handle_c1(uint8_t ch);
    void handle_esc();
    void handle_csi();
    void handle_dcs();
    void handle_osc();
    void handle_sos_pm_apc();
    
    // CSI command handlers
    void csi_cup(int row = 1, int col = 1);
    void csi_ed(int param = 0);
    void csi_el(int param = 0);
    void csi_sgr(const std::vector<int>& params);
    void csi_decset(int param);
    void csi_decrst(int param);
    void csi_sm(int param);
    void csi_rm(int param);
    void csi_dsr(int param);
    void csi_tbc(int param = 0);
    void csi_cts(int param);
    void csi_decstbm(int top = 1, int bottom = -1);
    void csi_decslrm(int left = 1, int right = -1);
    void csi_cha(int col = 1);
    void csi_vpa(int row = 1);
    void csi_hvp(int row = 1, int col = 1);
    void csi_cuu(int count = 1);
    void csi_cud(int count = 1);
    void csi_cuf(int count = 1);
    void csi_cub(int count = 1);
    void csi_cnl(int count = 1);
    void csi_cpl(int count = 1);
    void csi_ich(int count = 1);
    void csi_dch(int count = 1);
    void csi_il(int count = 1);
    void csi_dl(int count = 1);
    void csi_ech(int count = 1);
    void csi_dch_top(int count = 1);
    void csi_decscusr(int style = 1);
    void csi_decscusr_rgb(int style, uint32_t color);
    
    // ESC command handlers
    void esc_designate_g0(int charset);
    void esc_designate_g1(int charset);
    void esc_designate_g2(int charset);
    void esc_designate_g3(int charset);
    void esc_index();
    void esc_reverse_index();
    void esc_next_line();
    void esc_horizontal_tab_set();
    void esc_ri();
    void esc_decaln();
    void esc_decdwl(int dir);
    void esc_decbi();
    
    // Graphics rendition helpers
    void apply_sgr_param(int param);
    uint32_t parse_color(const std::vector<int>& params, size_t& i);
    
    // Mouse reporting helpers
    void send_mouse_report(int button, int row, int col, bool pressed);
    void send_mouse_sgr_report(int button, int row, int col, bool pressed);
    void send_mouse_utf8_report(int button, int row, int col, bool pressed);
    
    // DEC private mode helpers
    void set_dec_mode(int param, bool value);
    void set_mode(int param, bool value);
    
    // Helper functions
    int get_param(size_t index, int default_val = 0) const;
    void clear_params();
    void add_param(int value);
    
    // Color palette initialization
    void init_color_palette();
};

} // namespace spiritty