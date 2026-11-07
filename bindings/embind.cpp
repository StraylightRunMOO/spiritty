/*
 * embind.cpp — JavaScript/TypeScript bindings for the Spiritty C ABI.
 *
 * This is the only C++ translation unit in the WASM build. Everything it
 * does is wrap C functions in spiritty.h with Embind machinery so the TS
 * wrapper in ts/src/spiritty.ts can drive them.
 *
 * Build flags (set in bindings/CMakeLists.txt):
 *   -sMODULARIZE=1 -sEXPORT_ES6=1 -sUSE_WEBGL2=1
 *   -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 -O3 -flto
 *   --emit-tsd spiritty.d.ts
 *
 * Design notes:
 *  - sp_options is a `value_object` — TS passes a plain object; Embind
 *    copies fields by name into the C struct.
 *  - sp_terminal is wrapped in a `SpTerminalHandle` RAII class so the
 *    JS GC reaping the handle calls sp_terminal_destroy automatically.
 *  - sp_terminal_write takes a Uint8Array via emscripten::val to avoid an
 *    extra JS-side malloc/free trip per chunk.
 *  - The renderer is created in-WASM (sp_renderer_webgl_create) and
 *    attached internally — TS never holds a renderer handle directly.
 */

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "spiritty/spiritty.h"
#include "spiritty/spiritty_renderer.h"
#include "font.h"

using emscripten::class_;
using emscripten::val;
using emscripten::value_object;
using emscripten::function;
using emscripten::register_vector;

namespace {

/* RAII handle that owns a terminal + its WebGL renderer. JS keeps a
 * reference; when the JS object is collected, ~SpTerminalHandle runs and
 * tears the C objects down in the right order. */
class SpTerminalHandle {
public:
    SpTerminalHandle(const sp_options& opts,
                     int32_t fb_w, int32_t fb_h, float dpr)
        : term_(sp_terminal_create(&opts))
        , renderer_(nullptr)
    {
        if (!term_) return;

        renderer_ = sp_renderer_webgl_create();
        if (!renderer_) {
            sp_terminal_destroy(term_);
            term_ = nullptr;
            return;
        }

        /* Rasterize IBM Plex Mono (or a later loadFont TTF) at the
         * framebuffer pixel size so glyphs are 1:1 with the backing store. */
        float px = (float)opts.font_size * (dpr > 0.0f ? dpr : 1.0f);
        if (px < 1.0f) px = 14.0f;
        float cw = 0.0f, ch = 0.0f;
        if (!sp_font_measure_builtin(px, &cw, &ch) || cw < 1.0f || ch < 1.0f) {
            cw = 10.0f * (dpr > 0.0f ? dpr : 1.0f);
            ch = 20.0f * (dpr > 0.0f ? dpr : 1.0f);
        }
        cell_w_ = cw;
        cell_h_ = ch;
        font_px_ = px;

        /* Bring the backend up. Without this, the vtable is attached
         * but every program / buffer handle in the renderer struct is
         * still zero, so the first render triggers
         *   WebGL: INVALID_OPERATION: no valid shader program in use
         *   WebGL: INVALID_OPERATION: bufferData: no buffer
         * The native imgui demo does this in terminal_widget.cpp. */
        sp_renderer_opts ropts{};
        ropts.framebuffer_width   = fb_w;
        ropts.framebuffer_height  = fb_h;
        ropts.device_pixel_ratio  = dpr;
        ropts.cell_width_px       = cw;
        ropts.cell_height_px      = ch;
        ropts.font_size_px        = px;
        ropts.use_linear_blending   = true;
        /* Coverage already matches the destination; the Ghostty
         * luminance correction fattens AA into gray boxes at 1:1. */
        ropts.use_linear_correction = false;

        if (!renderer_->vt->init(renderer_->self, &ropts)) {
            sp_renderer_webgl_destroy(renderer_);
            renderer_ = nullptr;
            sp_terminal_destroy(term_);
            term_ = nullptr;
            return;
        }

        sp_terminal_attach_renderer(term_, renderer_);
    }

    SpTerminalHandle(const SpTerminalHandle&)            = delete;
    SpTerminalHandle& operator=(const SpTerminalHandle&) = delete;

    ~SpTerminalHandle() {
        /* Detach before destroying renderer so the terminal's pointer
         * is cleared before we free the renderer's storage. */
        if (term_) {
            sp_terminal_attach_renderer(term_, nullptr);
        }
        if (renderer_) {
            sp_renderer_webgl_destroy(renderer_);
            renderer_ = nullptr;
        }
        if (term_) {
            sp_terminal_destroy(term_);
            term_ = nullptr;
        }
    }

    bool valid() const { return term_ != nullptr; }

    /* ---- I/O & state ---- */

    void write(val data) {
        /* `data` is a Uint8Array. Vector<uint8_t> is the cheapest copy
         * that Embind exposes; for hot paths consumers can use writeRaw
         * with a byte offset + length into the WASM heap directly. */
        if (!term_) return;
        const auto bytes = emscripten::convertJSArrayToNumberVector<uint8_t>(data);
        sp_terminal_write(term_, bytes.data(), bytes.size());
    }

    /* Direct heap path: caller writes into Module.HEAPU8 themselves and
     * passes the (offset, length) pair. Zero-copy. */
    void writeHeap(uintptr_t offset, size_t len) {
        if (!term_) return;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(offset);
        sp_terminal_write(term_, base, len);
    }

    void resize(int32_t cols, int32_t rows) {
        if (term_) sp_terminal_resize(term_, cols, rows);
    }

    /* Framebuffer resize — the <canvas> backing store changed. Must be
     * paired with a `resize` call if cols/rows also changed. */
    void resizeFramebuffer(int32_t fb_w, int32_t fb_h) {
        if (renderer_ && renderer_->vt && renderer_->vt->resize) {
            renderer_->vt->resize(renderer_->self, fb_w, fb_h);
        }
    }

    void clear() { if (term_) sp_terminal_clear(term_); }
    void reset() { if (term_) sp_terminal_reset(term_); }

    void render() { if (term_) sp_terminal_render(term_); }

    void setTime(float seconds) {
        if (term_) sp_terminal_set_time(term_, seconds);
    }

    int32_t cols() const { return term_ ? sp_terminal_cols(term_) : 0; }
    int32_t rows() const { return term_ ? sp_terminal_rows(term_) : 0; }

    /* ---- Cursor ---- */

    void moveCursor(int32_t row, int32_t col) {
        if (term_) sp_terminal_move_cursor(term_, row, col);
    }

    val getCursor() const {
        int32_t r = 0, c = 0;
        if (term_) sp_terminal_get_cursor(term_, &r, &c);
        val out = val::object();
        out.set("row", r);
        out.set("col", c);
        return out;
    }

    /* ---- Selection ---- */

    void selectionBegin(int32_t row, int32_t col, int32_t mode) {
        if (term_) {
            sp_terminal_selection_begin(term_, row, col,
                                        static_cast<sp_sel_mode>(mode));
        }
    }
    void selectionExtend(int32_t row, int32_t col) {
        if (term_) sp_terminal_selection_extend(term_, row, col);
    }
    void selectionClear() {
        if (term_) sp_terminal_selection_clear(term_);
    }
    std::string selectionText() const {
        if (!term_) return {};
        char* raw = sp_terminal_selection_text(term_);
        if (!raw) return {};
        std::string out(raw);
        sp_string_free(raw);
        return out;
    }

    /* ---- Input forwarding ---- */

    void sendKey(const std::string& utf8) {
        if (term_) sp_terminal_send_key(term_, utf8.data(), utf8.size());
    }

    void sendMouse(int32_t row, int32_t col, int32_t button, bool pressed) {
        if (term_) sp_terminal_send_mouse(term_, row, col, button, pressed);
    }

    /* ---- Shader hot-swap ---- */

    bool setCustomShader(const std::string& glsl) {
        if (!term_) return false;
        /* Empty string from JS means "restore the default identity pass."
         * The C side reads a NULL pointer as that signal, so map it here
         * rather than asking every caller to pass null/undefined. */
        const char* arg = glsl.empty() ? nullptr : glsl.c_str();
        return sp_terminal_set_custom_shader(term_, arg);
    }

    float cellWidth() const { return cell_w_; }
    float cellHeight() const { return cell_h_; }

    /* Replace the TTF. `data` is a Uint8Array of font bytes; empty
     * reloads the builtin IBM Plex Mono. pixelHeight is framebuffer px. */
    bool loadFont(val data, float pixel_height) {
        if (!renderer_ || !renderer_->vt || !renderer_->vt->load_font)
            return false;
        float px = pixel_height > 0.0f ? pixel_height : font_px_;
        if (px < 1.0f) px = 16.0f;
        std::vector<uint8_t> bytes;
        if (!data.isUndefined() && !data.isNull())
            bytes = emscripten::convertJSArrayToNumberVector<uint8_t>(data);
        const uint8_t* ptr = bytes.empty() ? nullptr : bytes.data();
        if (!renderer_->vt->load_font(renderer_->self, ptr, bytes.size(), px))
            return false;
        font_px_ = px;
        if (!bytes.empty()) {
            sp_font* tmp = sp_font_create_from_memory(ptr, bytes.size(), px);
            if (tmp) {
                sp_font_cell_metrics(tmp, &cell_w_, &cell_h_, nullptr);
                sp_font_destroy(tmp);
            }
        } else {
            (void)sp_font_measure_builtin(px, &cell_w_, &cell_h_);
        }
        return true;
    }

private:
    sp_terminal* term_;
    sp_renderer* renderer_;
    float        cell_w_  = 0.0f;
    float        cell_h_  = 0.0f;
    float        font_px_ = 0.0f;
};

/* sp_options has a NULL-defaulted const char* font_family and a
 * pointer+length palette pair. Embind value_object can't introspect raw
 * pointers, so we expose a flattened POD plus a small marshaller. */
struct SpOptionsJS {
    int32_t  cols             = 80;
    int32_t  rows             = 24;
    int32_t  scrollback_lines = 1000;
    int32_t  font_size        = 14;
    std::string font_family   = "monospace";
    uint32_t color_fg         = 0xE0E0E0FFu;
    uint32_t color_bg         = 0x101010FFu;
    uint32_t color_cursor     = 0xE0E0E0FFu;
    uint32_t color_selection  = 0x3060A0AAu;
    bool     gpu_acceleration = true;
    bool     cursor_blink     = true;
    int32_t  cursor_style     = 0;
    std::string custom_shader_path; /* "" → none */
};

SpTerminalHandle* createTerminal(const SpOptionsJS& js,
                                 int32_t fb_w, int32_t fb_h, float dpr) {
    sp_options opts;
    sp_options_default(&opts);
    opts.cols              = js.cols;
    opts.rows              = js.rows;
    opts.scrollback_lines  = js.scrollback_lines;
    opts.font_size         = js.font_size;
    opts.font_family       = js.font_family.empty() ? nullptr : js.font_family.c_str();
    opts.color_fg          = js.color_fg;
    opts.color_bg          = js.color_bg;
    opts.color_cursor      = js.color_cursor;
    opts.color_selection   = js.color_selection;
    opts.gpu_acceleration  = js.gpu_acceleration;
    opts.cursor_blink      = js.cursor_blink;
    opts.cursor_style      = js.cursor_style;
    opts.custom_shader_path = js.custom_shader_path.empty()
                              ? nullptr : js.custom_shader_path.c_str();
    /* palette stays NULL → core uses built-in xterm 256-color. TS can
     * mutate the palette later via a dedicated API once we add one. */
    return new SpTerminalHandle(opts, fb_w, fb_h, dpr);
}

void destroyTerminal(SpTerminalHandle* h) { delete h; }

std::string version() { return std::string(sp_version()); }

} /* namespace */

EMSCRIPTEN_BINDINGS(spiritty) {
    value_object<SpOptionsJS>("SpOptions")
        .field("cols",             &SpOptionsJS::cols)
        .field("rows",             &SpOptionsJS::rows)
        .field("scrollbackLines",  &SpOptionsJS::scrollback_lines)
        .field("fontSize",         &SpOptionsJS::font_size)
        .field("fontFamily",       &SpOptionsJS::font_family)
        .field("colorFg",          &SpOptionsJS::color_fg)
        .field("colorBg",          &SpOptionsJS::color_bg)
        .field("colorCursor",      &SpOptionsJS::color_cursor)
        .field("colorSelection",   &SpOptionsJS::color_selection)
        .field("gpuAcceleration",  &SpOptionsJS::gpu_acceleration)
        .field("cursorBlink",      &SpOptionsJS::cursor_blink)
        .field("cursorStyle",      &SpOptionsJS::cursor_style)
        .field("customShaderPath", &SpOptionsJS::custom_shader_path);

    class_<SpTerminalHandle>("SpTerminal")
        .function("valid",          &SpTerminalHandle::valid)
        .function("write",          &SpTerminalHandle::write)
        .function("writeHeap",      &SpTerminalHandle::writeHeap)
        .function("resize",         &SpTerminalHandle::resize)
        .function("resizeFramebuffer", &SpTerminalHandle::resizeFramebuffer)
        .function("clear",          &SpTerminalHandle::clear)
        .function("reset",          &SpTerminalHandle::reset)
        .function("render",         &SpTerminalHandle::render)
        .function("setTime",        &SpTerminalHandle::setTime)
        .function("cols",           &SpTerminalHandle::cols)
        .function("rows",           &SpTerminalHandle::rows)
        .function("moveCursor",     &SpTerminalHandle::moveCursor)
        .function("getCursor",      &SpTerminalHandle::getCursor)
        .function("selectionBegin", &SpTerminalHandle::selectionBegin)
        .function("selectionExtend",&SpTerminalHandle::selectionExtend)
        .function("selectionClear", &SpTerminalHandle::selectionClear)
        .function("selectionText",  &SpTerminalHandle::selectionText)
        .function("sendKey",        &SpTerminalHandle::sendKey)
        .function("sendMouse",      &SpTerminalHandle::sendMouse)
        .function("setCustomShader",&SpTerminalHandle::setCustomShader)
        .function("cellWidth",      &SpTerminalHandle::cellWidth)
        .function("cellHeight",     &SpTerminalHandle::cellHeight)
        .function("loadFont",       &SpTerminalHandle::loadFont);

    function("createTerminal",  &createTerminal, emscripten::allow_raw_pointers());
    function("destroyTerminal", &destroyTerminal, emscripten::allow_raw_pointers());
    function("version",         &version);
}
