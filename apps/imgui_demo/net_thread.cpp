/*
 * net_thread.cpp — std::thread + blocking POSIX socket + Telnetty.
 *
 * Used on macOS (no io_uring) and on Linux when SPIRITTY_USE_SUSPENDERS=OFF.
 * One thread per session; the thread blocks in recv(), feeds bytes through
 * Telnetty's IAC parser, and forwards decoded application bytes via the
 * on_bytes callback. Outbound bytes (keystrokes, NAWS subneg) are queued
 * from the GUI thread and drained by the same worker via a wakeup pipe.
 *
 * This file is the only TU that defines TELNETTY_IMPLEMENTATION.
 */

#define TELNETTY_IMPLEMENTATION
#define TELNETTY_OPTIONS_IMPLEMENTATION  /* defines telnetty_enable_naws etc. */
#include "telnetty_core.h"
#include "telnetty_options.h"

#include "net.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

/* Parse "tcp://host:port" → (host, port). Returns false on malformed input. */
bool parse_tcp_uri(const char* uri, std::string& host, std::string& port) {
    constexpr const char* prefix = "tcp://";
    constexpr size_t      prefix_len = 6;
    if (!uri || std::strncmp(uri, prefix, prefix_len) != 0) return false;
    const char* rest  = uri + prefix_len;
    const char* colon = std::strrchr(rest, ':');
    if (!colon || colon == rest) return false;
    host.assign(rest, static_cast<size_t>(colon - rest));
    port.assign(colon + 1);
    return !host.empty() && !port.empty();
}

int dial_blocking(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return -1;

    int fd = -1;
    for (addrinfo* rp = res; rp; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd >= 0) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

} /* namespace */

struct net_session {
    int                 sock_fd       = -1;
    int                 wake_pipe[2]  = { -1, -1 }; /* read, write */
    std::thread         worker;
    std::atomic<bool>   stop{false};

    telnetty_context_t* tn            = nullptr;

    net_on_bytes_fn     on_bytes      = nullptr;
    void*               on_bytes_user = nullptr;

    std::mutex             out_mu;
    std::vector<uint8_t>   out_pending; /* keystrokes queued from GUI thread */

    /* NAWS state — pushed by net_resize, picked up by the worker. */
    std::atomic<uint32_t>  pending_size{0}; /* (cols<<16) | rows; 0 = none */
    bool                   negotiated_naws = false;
};

/* ---- Telnetty event callback ---------------------------------------- */

static void on_telnet_event(telnetty_context_t* /*ctx*/,
                            telnetty_event_type_t event,
                            const telnetty_event_data_union_t* data,
                            void* user) {
    auto* s = static_cast<net_session*>(user);
    if (event == TELNETTY_EVENT_DATA && data && data->data.length > 0) {
        if (s->on_bytes) {
            s->on_bytes(data->data.data, data->data.length, s->on_bytes_user);
        }
    }
    /* Other events (WILL/WONT/DO/DONT/SB/SE) are handled by the option
     * handlers we registered up front — Telnetty drives the Q-Method
     * negotiation FSM internally and writes responses to ctx->out_buffer.
     * We just need to drain that buffer back to the wire. */
}

/* ---- Wake the worker out of poll() so it picks up new outbound data. */
static void wake_worker(net_session* s) {
    if (s->wake_pipe[1] >= 0) {
        const uint8_t b = 1;
        ssize_t n = ::write(s->wake_pipe[1], &b, 1);
        (void)n;
    }
}

/* ---- Worker loop ---------------------------------------------------- */

static void worker_main(net_session* s) {
    uint8_t buf[8192];

    /* Kick off NAWS — Telnetty queues a WILL NAWS into the out buffer. */
    telnetty_enable_naws(s->tn);

    while (!s->stop.load(std::memory_order_acquire)) {
        pollfd pfds[2];
        pfds[0] = { s->sock_fd,        POLLIN, 0 };
        pfds[1] = { s->wake_pipe[0],   POLLIN, 0 };

        int nready = ::poll(pfds, 2, -1);
        if (nready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Drain wake pipe so it doesn't fire forever. */
        if (pfds[1].revents & POLLIN) {
            uint8_t drain[64];
            while (::read(s->wake_pipe[0], drain, sizeof drain) > 0) { /* spin */ }
        }

        /* Pump any pending NAWS update (size changed since last loop). */
        uint32_t sz = s->pending_size.exchange(0, std::memory_order_acq_rel);
        if (sz != 0) {
            uint16_t cols = static_cast<uint16_t>(sz >> 16);
            uint16_t rows = static_cast<uint16_t>(sz & 0xFFFF);
            if (s->negotiated_naws) {
                telnetty_send_window_size(s->tn, cols, rows);
            }
            /* If NAWS hasn't completed yet the size is buffered for after
             * the WILL/DO handshake settles; we'll re-fire on next loop. */
        }

        /* Drain inbound socket bytes. */
        if (pfds[0].revents & POLLIN) {
            ssize_t n = ::recv(s->sock_fd, buf, sizeof buf, 0);
            if (n > 0) {
                telnetty_process(s->tn, buf, static_cast<size_t>(n));
            } else if (n == 0) {
                /* EOF. */
                if (s->on_bytes) s->on_bytes(nullptr, 0, s->on_bytes_user);
                break;
            } else if (errno != EAGAIN && errno != EINTR) {
                if (s->on_bytes) s->on_bytes(nullptr, 0, s->on_bytes_user);
                break;
            }
        }

        /* Append GUI-queued keystrokes into Telnetty (which IAC-escapes). */
        {
            std::lock_guard<std::mutex> lk(s->out_mu);
            if (!s->out_pending.empty()) {
                telnetty_send(s->tn, s->out_pending.data(), s->out_pending.size());
                s->out_pending.clear();
            }
        }

        /* Drain Telnetty's outbound buffer to the wire. */
        telnetty_buffer_t* outb = telnetty_get_output_buffer(s->tn);
        if (outb && outb->length > 0) {
            const uint8_t* p   = outb->data;
            size_t         rem = outb->length;
            while (rem > 0) {
                ssize_t w = ::send(s->sock_fd, p, rem, MSG_NOSIGNAL);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    rem = 0; /* socket dead; bail */
                    break;
                }
                p   += w;
                rem -= static_cast<size_t>(w);
            }
            telnetty_flush(s->tn);
        }

        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            if (s->on_bytes) s->on_bytes(nullptr, 0, s->on_bytes_user);
            break;
        }
    }
}

/* ---- Public API ----------------------------------------------------- */

net_session* net_open(const char* uri, net_on_bytes_fn on_bytes, void* user) {
    std::string host, port;
    if (!parse_tcp_uri(uri, host, port)) return nullptr;

    int fd = dial_blocking(host, port);
    if (fd < 0) return nullptr;

    auto* s = new net_session();
    s->sock_fd       = fd;
    s->on_bytes      = on_bytes;
    s->on_bytes_user = user;

    if (::pipe(s->wake_pipe) < 0) {
        ::close(fd);
        delete s;
        return nullptr;
    }
    /* Non-blocking wake pipe so the worker's drain loop terminates. */
    ::fcntl(s->wake_pipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(s->wake_pipe[1], F_SETFL, O_NONBLOCK);

    s->tn = telnetty_create(on_telnet_event, s);
    if (!s->tn) {
        ::close(fd);
        ::close(s->wake_pipe[0]);
        ::close(s->wake_pipe[1]);
        delete s;
        return nullptr;
    }

    s->worker = std::thread(worker_main, s);
    return s;
}

void net_send(net_session* s, const uint8_t* bytes, size_t len) {
    if (!s || !bytes || len == 0) return;
    {
        std::lock_guard<std::mutex> lk(s->out_mu);
        s->out_pending.insert(s->out_pending.end(), bytes, bytes + len);
    }
    wake_worker(s);
}

void net_resize(net_session* s, uint16_t cols, uint16_t rows) {
    if (!s) return;
    uint32_t packed = (static_cast<uint32_t>(cols) << 16)
                    |  static_cast<uint32_t>(rows);
    s->pending_size.store(packed, std::memory_order_release);
    /* Once the peer has agreed via DO NAWS, the worker will start
     * sending these. We optimistically mark NAWS negotiated after the
     * first non-zero size update so subsequent updates flow. */
    s->negotiated_naws = true;
    wake_worker(s);
}

void net_close(net_session* s) {
    if (!s) return;
    s->stop.store(true, std::memory_order_release);
    wake_worker(s);
    if (s->worker.joinable()) s->worker.join();
    if (s->tn) telnetty_destroy(s->tn);
    if (s->sock_fd >= 0) ::close(s->sock_fd);
    if (s->wake_pipe[0] >= 0) ::close(s->wake_pipe[0]);
    if (s->wake_pipe[1] >= 0) ::close(s->wake_pipe[1]);
    delete s;
}

const char* net_backend_name(void) { return "thread"; }

void net_pump(void) { /* worker thread drives itself; nothing to do. */ }
