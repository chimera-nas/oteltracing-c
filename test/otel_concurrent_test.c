// SPDX-FileCopyrightText: 2026 Ben Jarvis
// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include "oteltracing.h"
#include "otel_platform.h"
#include "otel_random.h"
#ifdef NDEBUG
#error Tests require assertions
#endif
static atomic_int done;
static unsigned int received;
static void span_sink(const struct otel_span *span, void *priv)
{
    (void) priv;
    assert(span->span_id != 0);
    assert(span->end_unix_ns >= span->start_unix_ns);
    received++;
}
static void *producer(void *arg)
{
    (void) arg;
    for (int round = 0; round < 20; ++round) {
        otel_thread_register();
        for (int i = 0; i < 100; ++i) {
            struct otel_span span;
            otel_span_start(&span, "concurrent", OTEL_SPAN_INTERNAL);
            otel_span_end(&span);
        }
        otel_thread_unregister();
    }
    return NULL;
}
static void *drainer(void *arg)
{
    (void) arg;
    while (!atomic_load(&done)) otel_drain();
    otel_drain();
    return NULL;
}
int main(void)
{
    struct otel_random_state state = {UINT64_C(0xfedcba9876543210), UINT64_C(0x123456789abcdef0)};
    const uint64_t expected[] = {UINT64_C(0x89934c906e585824), UINT64_C(0x33385589f1f29a72), UINT64_C(0xc41201d68f028d63), UINT64_C(0x4f086e8b9efeffda)};
    for (unsigned int i = 0; i < 4; ++i) assert(otel_random_next(&state) == expected[i]);
    struct timespec now;
    otel_realtime(&now);
    assert(now.tv_sec > 1700000000);
    struct otel_span_sink sink = {NULL, span_sink, NULL, NULL};
    otel_thread threads[4], drain;
    assert(otel_init("concurrent-test") == 0);
    otel_set_span_sink(&sink);
    assert(otel_thread_create(&drain, drainer, NULL) == 0);
    for (int i = 0; i < 4; ++i) assert(otel_thread_create(&threads[i], producer, NULL) == 0);
    for (int i = 0; i < 4; ++i) otel_thread_join(threads[i]);
    atomic_store(&done, 1);
    otel_thread_join(drain);
    assert(received == 8000);
    assert(otel_dropped_spans() == 0);
    otel_shutdown();
    return 0;
}
