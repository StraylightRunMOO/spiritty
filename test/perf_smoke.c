/*
 * perf_smoke.c — pump a synthetic 10 MB ANSI stream through
 * sp_terminal_write and report MB/s + ns/byte.
 *
 * Standalone binary built when SPIRITTY_BUILD_TESTS=ON. Not run as part
 * of `ctest` — perf measurements are inherently flaky in a CI matrix and
 * we don't want them gating PRs. Invoke explicitly:
 *
 *     ./build/test/spiritty_perf_smoke
 *
 * Methodology:
 *   - Generate one 10 MB buffer up front (so allocation + RNG aren't in
 *     the timed region).
 *   - Pump in 64 KB chunks. The chunk size is bigger than the parser's
 *     internal scratch but small enough to exercise the fast path.
 *   - Time only sp_terminal_write. Skip the renderer entirely (it would
 *     drag in GL); we want to measure parser + buffer cost.
 *   - Run several iterations and report best/median/mean.
 *
 * Synthetic stream content: lines of ~80 printable ASCII chars, with
 * SGR color escapes injected every ~20 chars and a CR+LF every line.
 * This is a reasonable proxy for `ls --color`, build output, etc.
 */

#define _POSIX_C_SOURCE 200809L

#include "spiritty/spiritty.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STREAM_BYTES   (10ull * 1024 * 1024)  /* 10 MB */
#define CHUNK_BYTES    (64ull * 1024)         /* 64 KB */
#define ITERATIONS     5

/* Fast deterministic PRNG so runs are reproducible. */
static uint64_t xorshift64(uint64_t* s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static uint8_t* gen_stream(size_t n) {
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) return NULL;

    static const char* sgr_palette[] = {
        "\x1b[31m", "\x1b[32m", "\x1b[33m", "\x1b[34m", "\x1b[35m",
        "\x1b[36m", "\x1b[91m", "\x1b[92m", "\x1b[1;33m", "\x1b[0m",
    };
    const size_t palette_len = sizeof sgr_palette / sizeof sgr_palette[0];

    uint64_t rng = 0xC0FFEE5EEDull;
    size_t   off = 0;
    int      col = 0;

    while (off < n) {
        if ((col & 0x1F) == 0 && n - off >= 8) {
            const char* esc = sgr_palette[xorshift64(&rng) % palette_len];
            size_t l = strlen(esc);
            if (off + l < n) {
                memcpy(buf + off, esc, l);
                off += l;
            }
        }
        if (col >= 80 && n - off >= 2) {
            buf[off++] = '\r';
            buf[off++] = '\n';
            col = 0;
            continue;
        }
        /* Printable ASCII 33..126. */
        buf[off++] = (uint8_t)(33 + (xorshift64(&rng) % (126 - 33)));
        col++;
    }
    return buf;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void* a, const void* b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

int main(void) {
    fprintf(stderr, "spiritty perf smoke — %llu MB / iter, %d iters\n",
            (unsigned long long)(STREAM_BYTES / (1024 * 1024)), ITERATIONS);

    uint8_t* stream = gen_stream(STREAM_BYTES);
    if (!stream) {
        fprintf(stderr, "stream alloc failed\n");
        return 1;
    }

    double secs[ITERATIONS];
    for (int it = 0; it < ITERATIONS; ++it) {
        sp_options opts;
        sp_options_default(&opts);
        opts.cols = 80;
        opts.rows = 24;
        opts.scrollback_lines = 1000;

        sp_terminal* t = sp_terminal_create(&opts);
        if (!t) { fprintf(stderr, "terminal create failed\n"); free(stream); return 1; }

        /* No renderer attached — sp_terminal_render would no-op anyway,
         * and we never call it. Pure parser + buffer cost. */

        const double t0 = now_seconds();
        size_t off = 0;
        while (off < STREAM_BYTES) {
            size_t take = CHUNK_BYTES;
            if (take > STREAM_BYTES - off) take = STREAM_BYTES - off;
            sp_terminal_write(t, stream + off, take);
            off += take;
        }
        const double t1 = now_seconds();
        secs[it] = t1 - t0;

        sp_terminal_destroy(t);

        fprintf(stderr, "  iter %d: %.3f s  (%.1f MB/s, %.1f ns/byte)\n",
                it, secs[it],
                (double)STREAM_BYTES / (1024.0 * 1024.0) / secs[it],
                secs[it] * 1e9 / (double)STREAM_BYTES);
    }

    qsort(secs, ITERATIONS, sizeof secs[0], cmp_double);
    double best = secs[0];
    double median = secs[ITERATIONS / 2];
    double sum = 0;
    for (int i = 0; i < ITERATIONS; ++i) sum += secs[i];
    double mean = sum / ITERATIONS;

    printf("perf_smoke: best=%.1f MB/s  median=%.1f MB/s  mean=%.1f MB/s  (%llu MB)\n",
           (double)STREAM_BYTES / (1024.0 * 1024.0) / best,
           (double)STREAM_BYTES / (1024.0 * 1024.0) / median,
           (double)STREAM_BYTES / (1024.0 * 1024.0) / mean,
           (unsigned long long)(STREAM_BYTES / (1024 * 1024)));

    free(stream);
    return 0;
}
