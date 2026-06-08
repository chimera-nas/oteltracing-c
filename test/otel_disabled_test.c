// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * Compile-time-disabled build: with OTEL_TRACING=0 the whole API must compile to
 * nothing via the header alone -- struct otel_span is zero-size and every call
 * is an elided no-op.  This test is deliberately NOT linked against
 * liboteltracing-c: if any call still referenced a library symbol it would fail
 * to link, proving the strip is complete.
 */

#define OTEL_TRACING 0
#include "oteltracing.h"

#include <stdio.h>

int
main(void)
{
    struct otel_span s;          /* zero-size; the embedded member costs nothing */
    uint8_t          trace_id[16] = { 0 };

    if (sizeof(struct otel_span) != 0) {
        fprintf(stderr, "otel_disabled_test: struct otel_span is %zu bytes, "
                "expected 0\n", sizeof(struct otel_span));
        return 1;
    }

    /* Every call below is a no-op the compiler elides; none reference the lib. */
    otel_init("disabled-build");
    otel_set_sampler(0.5);
    otel_set_enabled(1);
    otel_set_clock(0);
    otel_thread_register();

    otel_span_start(&s, "op", OTEL_SPAN_SERVER);
    otel_span_start_remote(&s, "rpc", OTEL_SPAN_SERVER, trace_id, 0, 1);
    otel_span_attr_str(&s, "k", "v");
    otel_span_attr_i64(&s, "i", -1);
    otel_span_attr_u64(&s, "u", 1);
    otel_span_attr_bool(&s, "b", 1);
    otel_span_event(&s, "e");
    otel_span_set_status(&s, OTEL_STATUS_ERROR, "x");

    if (otel_span_recording(&s)) {
        fprintf(stderr, "otel_disabled_test: recording should be 0\n");
        return 1;
    }

    otel_span_end(&s);

    if (otel_drain() != 0 || otel_dropped_spans() != 0) {
        fprintf(stderr, "otel_disabled_test: drain/dropped should be 0\n");
        return 1;
    }

    otel_thread_unregister();
    otel_shutdown();

    printf("otel_disabled_test: OK (tracing compiled out, struct otel_span is "
           "%zu bytes)\n", sizeof(struct otel_span));
    return 0;
}
