/*
 * net_suspenders.c — Linux backend: suspenders.h coroutines + io_uring +
 * Telnetty IAC parser.
 *
 * One session opens with a single SUSPENDERS_QOS_HIGH coroutine that
 * dials, then enters a read loop pumping bytes through Telnetty. A
 * second SUSPENDERS_QOS_NORMAL coroutine drains a keystroke channel
 * (suspenders_chan_t) into hose_write — keeping read latency unaffected
 * by send congestion.
 *
 * The GUI thread calls suspenders_run() once per frame in non-blocking
 * mode (Phase 5 design (a)). A future Phase 6 perf experiment will
 * compare against a dedicated runner thread.
 *
 * This file is the only TU that defines TELNETTY_IMPLEMENTATION in the
 * Linux+suspenders configuration.
 */

/* We compile in strict ISO C11 (no GNU extensions) for the rest of the
 * core, but this TU is unavoidably POSIX-bound (io_uring + pthread).
 * Request POSIX.1-2008 *before* any system header so glibc exposes
 * sigset_t (via signal.h) — liburing.h declares it in its prototypes
 * without including <signal.h> itself. */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include <signal.h>

/* Memento provides the thread-local heap that suspenders.h relies on
 * via BUF_MALLOC / BUF_FREE / arena calls. We are the single TU in the
 * demo that defines MEMENTO_IMPLEMENTATION; the rest of the codebase
 * stays Memento-free until Phase 6 starts wiring it through the core. */
#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#define TELNETTY_IMPLEMENTATION
#define TELNETTY_OPTIONS_IMPLEMENTATION  /* defines telnetty_enable_naws etc. */
#include "telnetty_core.h"
#include "telnetty_options.h"

#define SUSPENDERS_IMPLEMENTATION
#include "suspenders.h"

#include "net.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Per-session state. Lives on the heap, owned by net_session. */
struct net_session {
    hose_t              hose;          /* io_uring socket abstraction */
    struct buf          io_buf;        /* read scratch */
    telnetty_context_t* tn;

    suspenders_cr_t*    reader_cr;
    suspenders_cr_t*    writer_cr;
    suspenders_chan_t*  send_chan;     /* keystrokes from GUI -> writer cr */

    net_on_bytes_fn     on_bytes;
    void*               on_bytes_user;

    /* NAWS state. The reader cr picks pending sizes up between read ticks. */
    _Atomic uint32_t    pending_size;
    bool                negotiated_naws;

    _Atomic bool        stop;
    pthread_mutex_t     send_mu;       /* guards out-of-cr telnetty_send calls */
};

/* Module-wide one-time init for the suspenders runtime. The runtime is
 * shared across all sessions; the host pumps it via net_pump() (called
 * from the GUI render loop). */
static atomic_bool g_runtime_inited = false;
static pthread_mutex_t g_runtime_mu = PTHREAD_MUTEX_INITIALIZER;

static void ensure_runtime(void) {
    if (atomic_load_explicit(&g_runtime_inited, memory_order_acquire)) return;
    pthread_mutex_lock(&g_runtime_mu);
    if (!atomic_load_explicit(&g_runtime_inited, memory_order_acquire)) {
        /* 0 worker threads -> single-threaded run; we tick from the GUI. */
        suspenders_init(0, 4096);
        atomic_store_explicit(&g_runtime_inited, true, memory_order_release);
    }
    pthread_mutex_unlock(&g_runtime_mu);
}

/* ---- Telnetty event callback --------------------------------------- */

static void on_telnet_event(telnetty_context_t* ctx,
                            telnetty_event_type_t event,
                            const telnetty_event_data_union_t* data,
                            void* user) {
    (void)ctx;
    net_session* s = (net_session*)user;
    if (event == TELNETTY_EVENT_DATA && data && data->data.length > 0) {
        if (s->on_bytes) {
            s->on_bytes(data->data.data, data->data.length, s->on_bytes_user);
        }
    }
}

/* ---- Drain Telnetty's outbound buffer to the wire ------------------ */

static void drain_outbound(net_session* s) {
    telnetty_buffer_t* outb = telnetty_get_output_buffer(s->tn);
    if (!outb || outb->length == 0) return;
    /* hose_write suspends the calling cr until the io_uring CQE lands. */
    hose_write(&s->hose, outb->data, outb->length);
    telnetty_flush(s->tn);
}

/* ---- Reader coroutine --------------------------------------------- */

static void reader_main(void* arg) {
    net_session* s = (net_session*)arg;

    /* NAWS negotiation kicks off here so it lands inside Telnetty's
     * outbound buffer before our first drain. */
    telnetty_enable_naws(s->tn);
    drain_outbound(s);

    uint8_t buf[8192];
    while (!atomic_load_explicit(&s->stop, memory_order_acquire)) {
        /* Pick up any pending NAWS update. */
        uint32_t sz = atomic_exchange_explicit(&s->pending_size, 0,
                                               memory_order_acq_rel);
        if (sz != 0 && s->negotiated_naws) {
            uint16_t cols = (uint16_t)(sz >> 16);
            uint16_t rows = (uint16_t)(sz & 0xFFFF);
            telnetty_send_window_size(s->tn, cols, rows);
            drain_outbound(s);
        }

        /* hose_read suspends the cr; another runnable cr (e.g. the
         * writer) gets the CPU until the read completes. */
        if (!hose_read(&s->hose, buf, sizeof buf)) {
            /* EOF or error. */
            if (s->on_bytes) s->on_bytes(NULL, 0, s->on_bytes_user);
            return;
        }
        telnetty_process(s->tn, buf, sizeof buf);
        drain_outbound(s);
    }
}

/* ---- Writer coroutine ---------------------------------------------- */

typedef struct {
    uint8_t* data;
    size_t   len;
} send_chunk_t;

static void writer_main(void* arg) {
    net_session* s = (net_session*)arg;
    send_chunk_t chunk;
    while (!atomic_load_explicit(&s->stop, memory_order_acquire)) {
        if (!suspenders_chan_recv(s->send_chan, &chunk)) return;
        if (!chunk.data || chunk.len == 0) {
            free(chunk.data);
            continue;
        }
        pthread_mutex_lock(&s->send_mu);
        telnetty_send(s->tn, chunk.data, chunk.len);
        pthread_mutex_unlock(&s->send_mu);
        drain_outbound(s);
        free(chunk.data);
    }
}

/* ---- URI parsing --------------------------------------------------- */

static bool parse_tcp_uri_inplace(const char* uri, char* out, size_t out_cap) {
    static const char prefix[] = "tcp://";
    if (!uri || strncmp(uri, prefix, sizeof prefix - 1) != 0) return false;
    if (strlen(uri) >= out_cap) return false;
    memcpy(out, uri, strlen(uri) + 1);
    return strrchr(out + sizeof prefix - 1, ':') != NULL;
}

/* ---- Public API ---------------------------------------------------- */

net_session* net_open(const char* uri, net_on_bytes_fn on_bytes, void* user) {
    char uri_copy[256];
    if (!parse_tcp_uri_inplace(uri, uri_copy, sizeof uri_copy)) return NULL;

    ensure_runtime();

    net_session* s = (net_session*)calloc(1, sizeof(net_session));
    if (!s) return NULL;

    s->on_bytes      = on_bytes;
    s->on_bytes_user = user;
    pthread_mutex_init(&s->send_mu, NULL);
    atomic_init(&s->pending_size, 0);
    atomic_init(&s->stop, false);

    s->tn = telnetty_create(on_telnet_event, s);
    if (!s->tn) { free(s); return NULL; }

    /* hose_init wants a struct buf; supply our scratch. */
    s->io_buf.data = NULL;
    s->io_buf.len  = 0;
    s->io_buf.cap  = 0;
    hose_init(&s->hose, suspenders_ring(), &s->io_buf);

    if (!hose_dial(&s->hose, uri_copy)) {
        telnetty_destroy(s->tn);
        free(s);
        return NULL;
    }

    /* Channel for outbound chunks (heap-allocated send_chunk_t copies). */
    s->send_chan = suspenders_chan_create(sizeof(send_chunk_t), 64);
    if (!s->send_chan) {
        hose_close(&s->hose);
        telnetty_destroy(s->tn);
        free(s);
        return NULL;
    }

    s->reader_cr = suspenders_spawn(reader_main, s, SUSPENDERS_QOS_HIGH);
    s->writer_cr = suspenders_spawn(writer_main, s, SUSPENDERS_QOS_NORMAL);
    return s;
}

void net_send(net_session* s, const uint8_t* bytes, size_t len) {
    if (!s || !bytes || len == 0) return;
    /* Copy into a heap chunk so the GUI caller's buffer can be reused
     * before the writer cr drains the channel. */
    send_chunk_t chunk;
    chunk.data = (uint8_t*)malloc(len);
    if (!chunk.data) return;
    memcpy(chunk.data, bytes, len);
    chunk.len = len;
    if (!suspenders_chan_send(s->send_chan, &chunk)) {
        free(chunk.data);
    }
}

void net_resize(net_session* s, uint16_t cols, uint16_t rows) {
    if (!s) return;
    uint32_t packed = ((uint32_t)cols << 16) | (uint32_t)rows;
    atomic_store_explicit(&s->pending_size, packed, memory_order_release);
    s->negotiated_naws = true;
}

void net_close(net_session* s) {
    if (!s) return;
    atomic_store_explicit(&s->stop, true, memory_order_release);
    /* Send a poison empty chunk so the writer cr unblocks from chan_recv. */
    send_chunk_t poison = { NULL, 0 };
    suspenders_chan_send(s->send_chan, &poison);
    /* Wait for both crs to wind down by spinning the runtime until they
     * have observed `stop`. The GUI's main loop normally pumps suspenders
     * for us, but during teardown we drive it ourselves. */
    for (int i = 0; i < 64; ++i) {
        suspenders_run();
    }
    hose_close(&s->hose);
    telnetty_destroy(s->tn);
    free(s->io_buf.data);
    pthread_mutex_destroy(&s->send_mu);
    free(s);
}

const char* net_backend_name(void) { return "suspenders"; }

/* Tick the suspenders runtime in non-blocking mode. The GUI main loop
 * calls this once per rendered frame so coroutines get CPU. Exposed
 * via a non-net.h symbol so the demo can pump regardless of backend
 * (the thread backend's net_pump is a no-op stub). */
void net_pump(void) {
    if (atomic_load_explicit(&g_runtime_inited, memory_order_acquire)) {
        suspenders_run();
    }
}
