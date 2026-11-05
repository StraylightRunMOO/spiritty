/*
 * terminal_widget.cpp — implementation. Owns the spiritty handle and the
 * GL renderer; bridges decoded telnet bytes from the network worker into
 * sp_terminal_write on the GUI thread.
 */

#include "terminal_widget.h"

#include <cstring>

TerminalWidget::TerminalWidget(int cols, int rows, float cell_w, float cell_h,
                               void* (*gl_loader)(const char*))
    : cols_(cols)
    , rows_(rows)
    , fb_w_(static_cast<int>(cols * cell_w))
    , fb_h_(static_cast<int>(rows * cell_h))
{
    sp_options opts;
    sp_options_default(&opts);
    opts.cols  = cols;
    opts.rows  = rows;
    opts.scrollback_lines = 1000;

    term_ = sp_terminal_create(&opts);
    if (!term_) return;

#if defined(SPIRITTY_HAVE_GL)
    renderer_ = sp_renderer_gl_create();
#else
    renderer_ = sp_renderer_null_create();
#endif
    if (!renderer_) return;

    sp_renderer_opts ropts{};
    ropts.framebuffer_width  = fb_w_;
    ropts.framebuffer_height = fb_h_;
    ropts.device_pixel_ratio = 1.0f;
    ropts.cell_width_px      = cell_w;
    ropts.cell_height_px     = cell_h;
    ropts.use_linear_blending   = true;
    ropts.use_linear_correction = true;
    ropts.gl_loader          = gl_loader;
    if (!renderer_->vt->init(renderer_->self, &ropts)) {
#if defined(SPIRITTY_HAVE_GL)
        sp_renderer_gl_destroy(renderer_);
#else
        sp_renderer_null_destroy(renderer_);
#endif
        renderer_ = nullptr;
        return;
    }

    sp_terminal_attach_renderer(term_, renderer_);
}

TerminalWidget::~TerminalWidget() {
    disconnect();
    if (term_) {
        sp_terminal_attach_renderer(term_, nullptr);
        sp_terminal_destroy(term_);
        term_ = nullptr;
    }
    if (renderer_) {
#if defined(SPIRITTY_HAVE_GL)
        sp_renderer_gl_destroy(renderer_);
#else
        sp_renderer_null_destroy(renderer_);
#endif
        renderer_ = nullptr;
    }
}

bool TerminalWidget::connect(const std::string& uri) {
    if (session_) return false;
    session_ = net_open(uri.c_str(), &TerminalWidget::on_bytes_thunk, this);
    if (session_) {
        net_resize(session_, static_cast<uint16_t>(cols_),
                              static_cast<uint16_t>(rows_));
    }
    return session_ != nullptr;
}

void TerminalWidget::disconnect() {
    if (session_) {
        net_close(session_);
        session_ = nullptr;
    }
}

void TerminalWidget::send_input(const char* utf8, size_t len) {
    if (!session_ || !utf8 || len == 0) return;
    net_send(session_, reinterpret_cast<const uint8_t*>(utf8), len);
}

void TerminalWidget::resize(int cols, int rows, int fb_w, int fb_h) {
    cols_ = cols;
    rows_ = rows;
    fb_w_ = fb_w;
    fb_h_ = fb_h;
    if (term_)     sp_terminal_resize(term_, cols, rows);
    if (renderer_) renderer_->vt->resize(renderer_->self, fb_w, fb_h);
    if (session_)  net_resize(session_, static_cast<uint16_t>(cols),
                                         static_cast<uint16_t>(rows));
}

void TerminalWidget::tick(float time_seconds) {
    if (!term_) return;

    /* Drain anything the network worker has accumulated since last tick.
     * We swap into a local buffer so the worker can keep pushing while
     * we feed bytes to the parser. */
    std::vector<uint8_t> drained;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        drained.swap(pending_bytes_);
    }
    if (!drained.empty()) {
        sp_terminal_write(term_, drained.data(), drained.size());
    }

    sp_terminal_set_time(term_, time_seconds);
    sp_terminal_render(term_);
}

void TerminalWidget::on_bytes_thunk(const uint8_t* bytes, size_t len, void* user) {
    auto* self = static_cast<TerminalWidget*>(user);
    if (!bytes || len == 0) return;  /* EOF — leave session_ for explicit close. */
    {
        std::lock_guard<std::mutex> lk(self->pending_mu_);
        self->pending_bytes_.insert(self->pending_bytes_.end(), bytes, bytes + len);
        self->bytes_received_ += len;
    }
}

int TerminalWidget::cols() const { return term_ ? sp_terminal_cols(term_) : 0; }
int TerminalWidget::rows() const { return term_ ? sp_terminal_rows(term_) : 0; }
