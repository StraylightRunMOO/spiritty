/*
 * net.h — cross-platform network ABI for the Spiritty ImGui demo.
 *
 * Two backends implement this interface:
 *   - net_suspenders.c (Linux, when SPIRITTY_USE_SUSPENDERS=ON):
 *       suspenders.h coroutines + io_uring + Telnetty IAC parser.
 *   - net_thread.cpp (macOS, and Linux when suspenders is disabled):
 *       std::thread + blocking POSIX socket + Telnetty.
 *
 * Both implementations:
 *   - Own a Telnetty context internally; the caller never sees it.
 *   - Deliver decoded application bytes via the on_bytes callback. The
 *     callback runs on the network worker thread/coroutine — the host
 *     is responsible for posting them onto the GUI thread (the ImGui
 *     widget uses a lock-free SPSC queue for this).
 *   - Accept keystrokes via net_send(). Bytes are escaped (IAC -> IAC IAC)
 *     by Telnetty before they hit the wire.
 *   - Negotiate NAWS at connect time; net_resize() updates the peer.
 *
 * The on_bytes callback contract:
 *   - Called with NULL/0 when the connection closes cleanly (EOF). The
 *     handle becomes invalid for subsequent send/resize after this; only
 *     net_close() is allowed.
 *   - Never called after net_close() returns.
 */

#ifndef SPIRITTY_DEMO_NET_H
#define SPIRITTY_DEMO_NET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct net_session net_session;

/* Receives decoded application bytes from the peer. `bytes==NULL && len==0`
 * means EOF / clean close. Runs on the backend's worker context, not the
 * GUI thread — do not touch ImGui or GL state here. */
typedef void (*net_on_bytes_fn)(const uint8_t* bytes, size_t len, void* user);

/* Open a session against `uri` (only "tcp://host:port" is supported in v1).
 * Returns NULL on failure. The on_bytes callback fires asynchronously
 * once the connection is established and bytes arrive. */
net_session* net_open(const char* uri,
                      net_on_bytes_fn on_bytes,
                      void* user);

/* Send keystrokes / mouse / arbitrary input to the peer. Thread-safe with
 * respect to the backend worker; callers may invoke from the GUI thread. */
void net_send(net_session* s, const uint8_t* bytes, size_t len);

/* Update the peer with a new terminal size (NAWS subneg). Idempotent;
 * cheap to spam from a ResizeObserver / GLFW window-size callback. */
void net_resize(net_session* s, uint16_t cols, uint16_t rows);

/* Tear down the session. Blocks until the worker has acknowledged
 * shutdown and any in-flight on_bytes calls have completed. */
void net_close(net_session* s);

/* Returns a static string identifying the active backend
 * (e.g. "suspenders" or "thread") — used by the demo's status bar. */
const char* net_backend_name(void);

/* Tick the network runtime. Backends that own no event loop of their own
 * (the suspenders backend, when running in option-(a) GUI-pumped mode)
 * advance their coroutines here. The thread backend's implementation is a
 * no-op. The demo calls this once per rendered frame regardless of which
 * backend is linked, so the call site stays #ifdef-free. */
void net_pump(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPIRITTY_DEMO_NET_H */
