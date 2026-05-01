/*
 * spiritty.hpp — header-only C++17 RAII facade over the C11 core.
 *
 * Zero translation units; all bodies are inline. Throws nothing —
 * failures propagate as bool returns or empty optionals. Build the
 * core with `-fno-exceptions -fno-rtti` and consume from any C++17
 * project.
 */
#ifndef SPIRITTY_HPP
#define SPIRITTY_HPP

#include "spiritty.h"
#include "spiritty_renderer.h"

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace spiritty {

struct Options : public ::sp_options {
    Options() noexcept { ::sp_options_default(this); }
};

class Terminal {
public:
    using EventCallback = std::function<void(const ::sp_event&)>;

    Terminal() : Terminal(Options{}) {}

    explicit Terminal(const ::sp_options& opts)
        : handle_(::sp_terminal_create(&opts), &::sp_terminal_destroy) {}

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) noexcept        = default;
    Terminal& operator=(Terminal&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] ::sp_terminal* raw() const noexcept { return handle_.get(); }

    void write(std::string_view bytes) noexcept {
        ::sp_terminal_write(handle_.get(),
                            reinterpret_cast<const uint8_t*>(bytes.data()),
                            bytes.size());
    }

    void resize(int cols, int rows) noexcept {
        ::sp_terminal_resize(handle_.get(), cols, rows);
    }

    void clear() noexcept { ::sp_terminal_clear(handle_.get()); }
    void reset() noexcept { ::sp_terminal_reset(handle_.get()); }
    void render() noexcept { ::sp_terminal_render(handle_.get()); }

    [[nodiscard]] int cols() const noexcept { return ::sp_terminal_cols(handle_.get()); }
    [[nodiscard]] int rows() const noexcept { return ::sp_terminal_rows(handle_.get()); }

    void attach_renderer(::sp_renderer* r) noexcept {
        ::sp_terminal_attach_renderer(handle_.get(), r);
    }

    void on_event(EventCallback cb) {
        callback_ = std::move(cb);
        ::sp_terminal_set_event_cb(handle_.get(), &Terminal::trampoline_, this);
    }

    [[nodiscard]] std::string selection_text() const {
        char* raw = ::sp_terminal_selection_text(handle_.get());
        if (!raw) return {};
        std::string out(raw);
        ::sp_string_free(raw);
        return out;
    }

    bool set_custom_shader(std::string_view glsl) {
        std::string z(glsl);
        return ::sp_terminal_set_custom_shader(handle_.get(), z.c_str());
    }

private:
    static void trampoline_(const ::sp_event* ev, void* user) {
        auto* self = static_cast<Terminal*>(user);
        if (self && self->callback_) self->callback_(*ev);
    }

    using Handle = std::unique_ptr<::sp_terminal, decltype(&::sp_terminal_destroy)>;
    Handle        handle_{nullptr, &::sp_terminal_destroy};
    EventCallback callback_;
};

} // namespace spiritty

#endif // SPIRITTY_HPP
