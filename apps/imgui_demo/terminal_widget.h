/*
 * terminal_widget.h — owns a Spiritty terminal + GL renderer + the bridge
 * to the network backend. The demo's main.cpp creates one of these per
 * session and ticks it from the GLFW main loop.
 *
 * v1 layout: the terminal renders into the GLFW default framebuffer
 * directly. ImGui draws a status bar + connect dialog on top.
 */

#ifndef SPIRITTY_DEMO_TERMINAL_WIDGET_H
#define SPIRITTY_DEMO_TERMINAL_WIDGET_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "spiritty/spiritty.h"
#include "spiritty/spiritty_renderer.h"
#include "net.h"

class TerminalWidget {
public:
    TerminalWidget(int cols, int rows, float cell_w, float cell_h,
                   void* (*gl_loader)(const char*));
    ~TerminalWidget();

    /** Open a network session against `uri` (e.g. "tcp://towel.blinkenlights.nl:23"). */
    bool connect(const std::string& uri);
    void disconnect();
    bool connected() const { return session_ != nullptr; }

    /** Forward a UTF-8 string to the peer. */
    void send_input(const char* utf8, size_t len);

    /** Update grid + framebuffer size; pushes NAWS to the peer and resizes
     *  the renderer's scene FBO. */
    void resize(int cols, int rows, int fb_w, int fb_h);

    /** Per-frame tick: drains pending inbound bytes into the terminal,
     *  then renders one frame to the current GL framebuffer. */
    void tick(float time_seconds);

    /** Snapshot of the most recent decoded byte count for diagnostics. */
    uint64_t bytes_received() const { return bytes_received_; }

    int cols() const;
    int rows() const;

    /** Backend name for the status bar ("suspenders" / "thread"). */
    const char* backend_name() const { return ::net_backend_name(); }

private:
    static void on_bytes_thunk(const uint8_t* bytes, size_t len, void* user);

    sp_terminal*    term_       = nullptr;
    sp_renderer*    renderer_   = nullptr;
    net_session*    session_    = nullptr;

    int             cols_       = 0;
    int             rows_       = 0;
    int             fb_w_       = 0;
    int             fb_h_       = 0;

    /* SPSC-ish queue: net worker pushes, GUI thread drains. Mutex is fine
     * for v1 — bytes-per-second is tiny relative to GUI tick rate. */
    std::mutex             pending_mu_;
    std::vector<uint8_t>   pending_bytes_;
    uint64_t               bytes_received_ = 0;
};

#endif /* SPIRITTY_DEMO_TERMINAL_WIDGET_H */
