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
    suspenders_hose_t   hose;          /* io_uring socket abstraction */
    struct buf          io_buf;        /* read scratch */
    telnetty_context_t* tn;

    char                uri[256];      /* "tcp://host:port" for reader cr */

    suspenders_cr_t*    reader_cr;
    suspenders_cr_t*    writer_cr;
    suspenders_chan_t*  send_chan;     /* keystrokes from GUI -> writer cr */

    net_on_bytes_fn     on_bytes;
    void*               on_bytes_user;

    /* NAWS state. The reader cr picks pending sizes up between read ticks. */
    _Atomic uint32_t    pending_size;
    bool                negotiated_naws;

    _Atomic bool        dial_done;     /* reader finished its dial step */
    _Atomic bool        stop;
    pthread_mutex_t     send_mu;       /* guards out-of-cr telnetty_send calls */
};

/* Module-wide one-time init for the suspenders runtime.
 *
 * suspenders 1.2.1 semantics: `suspenders_run()` blocks until every
 * spawned coroutine has finished (or `suspenders_request_stop()` is
 * called), and hose/channel blocking ops require coroutine context
 * (they return PERM/false if called from a non-coroutine thread).
 *
 * That makes a per-frame "tick from GUI" model impossible — the demo
 * now owns one dedicated worker thread that calls `suspenders_run()`
 * and stays there for the life of the process. A perpetual keepalive
 * coroutine parks on a never-signaled channel so `suspenders_run` never
 * drains to empty between sessions (which would let it return and race
 * us when the next session spawns). The GUI thread spawns coroutines
 * via `suspenders_spawn` (safe from any thread — foreign spawns go
 * through the global injector) and uses `suspenders_chan_try_send` for
 * outbound bytes (non-blocking ops don't need coroutine context). */
static atomic_bool g_runtime_inited = false;
static atomic_bool g_runtime_ready  = false;
static pthread_mutex_t g_runtime_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker_tid;
static suspenders_chan_t* g_keepalive;

static void keepalive_cr(void* arg) {
    (void)arg;
    uintptr_t dummy;
    /* Park forever — nothing ever sends on g_keepalive. */
    (void)suspenders_chan_recv(g_keepalive, &dummy);
}

static void* worker_main(void* arg) {
    (void)arg;
    /* suspenders_init sets a thread-local `initialized` flag that
     * suspenders_run checks, so it must be called on this thread.
     * suspenders_spawn / chan_create consult a global atomic
     * (s_runtime_up), so they're safe from the GUI thread once this
     * finishes. */
    suspenders_init(1, 4096);
    g_keepalive = suspenders_chan_create(sizeof(uintptr_t), 1);
    suspenders_spawn(keepalive_cr, NULL, SUSPENDERS_QOS_LOW);
    atomic_store_explicit(&g_runtime_ready, true, memory_order_release);
    suspenders_run();
    return NULL;
}

static void ensure_runtime(void) {
    if (atomic_load_explicit(&g_runtime_inited, memory_order_acquire)) return;
    pthread_mutex_lock(&g_runtime_mu);
    if (!atomic_load_explicit(&g_runtime_inited, memory_order_acquire)) {
        pthread_create(&g_worker_tid, NULL, worker_main, NULL);
        /* Wait for the worker to finish init before the GUI thread tries
         * to spawn / create channels. */
        while (!atomic_load_explicit(&g_runtime_ready, memory_order_acquire)) {
            struct timespec ts = {0, 1 * 1000 * 1000};  /* 1ms */
            nanosleep(&ts, NULL);
        }
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
    /* suspenders_hose_write suspends the calling cr until the io_uring
     * CQE lands. Return value is ignored — telnetty will re-queue on the
     * next drain if the peer is slow. */
    (void)suspenders_hose_write(&s->hose, outb->data, outb->length);
    telnetty_flush(s->tn);
}

/* ---- Reader coroutine --------------------------------------------- */

static void reader_main(void* arg) {
    net_session* s = (net_session*)arg;

    /* Dial from inside the coroutine — hose_dial suspends on io_uring
     * and requires coroutine context (fails PERM on a raw thread). */
    suspenders_hose_init(&s->hose, &s->io_buf);
    bool ok = suspenders_hose_dial(&s->hose, s->uri);
    if (!ok) {
        if (s->on_bytes) s->on_bytes(NULL, 0, s->on_bytes_user);
        atomic_store_explicit(&s->dial_done, true, memory_order_release);
        return;
    }
    atomic_store_explicit(&s->dial_done, true, memory_order_release);

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

        /* suspenders_hose_read suspends the cr; another runnable cr
         * (e.g. the writer) gets the CPU until the read completes.
         * Returns bytes read; 0 = EOF, negative = error. */
        ssize_t n = suspenders_hose_read(&s->hose, buf, sizeof buf);
        if (n <= 0) {
            if (s->on_bytes) s->on_bytes(NULL, 0, s->on_bytes_user);
            return;
        }
        telnetty_process(s->tn, buf, (size_t)n);
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

/* ---- URI parsing + DNS resolve ------------------------------------- */

#include <netdb.h>
#include <arpa/inet.h>

/* Resolve `tcp://host:port` to `tcp://A.B.C.D:port`. suspenders 1.2.1's
 * tcp_dial calls inet_pton directly (no DNS), so we have to do the
 * lookup ourselves. getaddrinfo is synchronous — fine on the GUI thread
 * before we hand the URI to the reader coroutine. */
static bool resolve_tcp_uri(const char* uri, char* out, size_t out_cap) {
    static const char prefix[] = "tcp://";
    const size_t plen = sizeof prefix - 1;
    if (!uri || strncmp(uri, prefix, plen) != 0) return false;

    const char* rest = uri + plen;
    const char* colon = strrchr(rest, ':');
    if (!colon || colon == rest) return false;

    char host[256];
    size_t hlen = (size_t)(colon - rest);
    if (hlen >= sizeof host) return false;
    memcpy(host, rest, hlen);
    host[hlen] = '\0';

    const char* port = colon + 1;
    if (*port == '\0') return false;

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;

    char ip[INET_ADDRSTRLEN];
    struct sockaddr_in* sin = (struct sockaddr_in*)res->ai_addr;
    if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip)) {
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    int n = snprintf(out, out_cap, "tcp://%s:%s", ip, port);
    return n > 0 && (size_t)n < out_cap;
}

/* ---- Public API ---------------------------------------------------- */

net_session* net_open(const char* uri, net_on_bytes_fn on_bytes, void* user) {
    char uri_copy[256];
    if (!resolve_tcp_uri(uri, uri_copy, sizeof uri_copy)) return NULL;

    ensure_runtime();

    net_session* s = (net_session*)calloc(1, sizeof(net_session));
    if (!s) return NULL;

    s->on_bytes      = on_bytes;
    s->on_bytes_user = user;
    pthread_mutex_init(&s->send_mu, NULL);
    atomic_init(&s->pending_size, 0);
    atomic_init(&s->dial_done, false);
    atomic_init(&s->stop, false);

    s->tn = telnetty_create(on_telnet_event, s);
    if (!s->tn) { free(s); return NULL; }

    s->io_buf.data = NULL;
    s->io_buf.len  = 0;
    s->io_buf.cap  = 0;
    memcpy(s->uri, uri_copy, sizeof s->uri);

    /* Channel for outbound chunks (heap-allocated send_chunk_t copies). */
    s->send_chan = suspenders_chan_create(sizeof(send_chunk_t), 64);
    if (!s->send_chan) {
        telnetty_destroy(s->tn);
        free(s);
        return NULL;
    }

    /* Dial happens inside reader_main — hose_dial needs coroutine context.
     * net_open returns a live session optimistically; a connection
     * failure surfaces as an EOF callback (on_bytes with NULL/0). */
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
    /* try_send is the non-blocking variant — safe to call from the GUI
     * thread (blocking chan ops fail PERM outside coroutine context). */
    if (suspenders_chan_try_send(s->send_chan, &chunk) != SUSPENDERS_OK) {
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
    /* Close the hose first so any in-flight hose_read returns promptly;
     * then poison the send channel so the writer cr's chan_recv unblocks.
     * The dedicated worker thread drives both crs to completion — we do
     * not call suspenders_run from the GUI (it blocks forever). */
    suspenders_hose_close(&s->hose);
    send_chunk_t poison = { NULL, 0 };
    (void)suspenders_chan_try_send(s->send_chan, &poison);
    /* Give the worker a moment to drain; coroutine cleanup is best-effort
     * here (teardown races on free would need refcounting). For the demo
     * one-connect-at-a-time flow this is sufficient. */
    struct timespec ts = {0, 20 * 1000 * 1000};  /* 20ms */
    nanosleep(&ts, NULL);
    telnetty_destroy(s->tn);
    free(s->io_buf.data);
    pthread_mutex_destroy(&s->send_mu);
    free(s);
}

const char* net_backend_name(void) { return "suspenders"; }

/* No-op in the dedicated-worker model. Kept so main.cpp's per-frame
 * call compiles for both backends. */
void net_pump(void) { }
