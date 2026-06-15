// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * Standalone decode-verify test: register a stub transport that captures the
 * emitted gRPC-framed OTLP buffer, emit a parent + child span with attributes
 * and an event, drain, then unpack the captured ExportTraceServiceRequest with
 * protobuf-c and assert the structure round-trips correctly.  No collector or
 * network is involved, so this runs in CI.
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

#include "oteltracing.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb-c.h"

struct grpc_hdr {
    uint8_t  compressed;
    uint32_t length;
} __attribute__((packed));

static uint8_t  g_buf[1 << 20];
static size_t   g_len;
static int      g_calls;

static void
stub_transport(
    const void *buf,
    size_t      len,
    void       *priv)
{
    (void) priv;
    g_calls++;
    if (len <= sizeof(g_buf)) {
        memcpy(g_buf, buf, len);
        g_len = len;
    }
}

static int g_failures;

#define CHECK(cond) do {                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                           \
        }                                                           \
} while (0)

int
main(void)
{
    struct otel_span parent, child;

    otel_init("oteltracing-test");
    otel_set_transport(stub_transport, NULL);
    otel_thread_register();

    otel_span_start(&parent, "parent-op", OTEL_SPAN_SERVER);
    CHECK(otel_span_recording(&parent));
    otel_span_attr_str(&parent, "peer", "1.2.3.4");
    otel_span_attr_u64(&parent, "bytes", 4096);
    struct otel_event *ev = otel_span_event(&parent, "received");
    otel_event_attr_str(&parent, ev, "queue", "rx0");
    otel_event_attr_u64(&parent, ev, "len", 512);

    otel_span_start_child(&child, "child-op", OTEL_SPAN_INTERNAL, &parent);
    otel_span_attr_i64(&child, "depth", 1);
    otel_span_set_status(&child, OTEL_STATUS_ERROR, "boom");
    otel_span_end(&child);

    otel_span_end(&parent);

    int n = otel_drain();
    CHECK(n == 2);
    CHECK(g_calls == 1);
    CHECK(g_len > sizeof(struct grpc_hdr));

    /* Decode the captured gRPC-framed payload. */
    struct grpc_hdr *hdr = (struct grpc_hdr *) g_buf;
    CHECK(hdr->compressed == 0);
    uint32_t plen = ntohl(hdr->length);
    CHECK(plen == g_len - sizeof(*hdr));

    Opentelemetry__Proto__Collector__Trace__V1__ExportTraceServiceRequest *req =
        opentelemetry__proto__collector__trace__v1__export_trace_service_request__unpack(
            NULL, plen, g_buf + sizeof(*hdr));
    CHECK(req != NULL);

    if (req) {
        CHECK(req->n_resource_spans == 1);
        Opentelemetry__Proto__Trace__V1__ResourceSpans *rs = req->resource_spans[0];

        /* Resource carries service.name. */
        int found_service = 0;
        for (size_t i = 0; i < rs->resource->n_attributes; i++) {
            if (strcmp(rs->resource->attributes[i]->key, "service.name") == 0) {
                found_service = 1;
                CHECK(strcmp(rs->resource->attributes[i]->value->string_value,
                             "oteltracing-test") == 0);
            }
        }
        CHECK(found_service);

        CHECK(rs->n_scope_spans == 1);
        Opentelemetry__Proto__Trace__V1__ScopeSpans *ss = rs->scope_spans[0];
        CHECK(ss->n_spans == 2);

        /* Identify spans by name (batch order is drain order: child then parent). */
        Opentelemetry__Proto__Trace__V1__Span *sp_parent = NULL, *sp_child = NULL;
        for (size_t i = 0; i < ss->n_spans; i++) {
            if (strcmp(ss->spans[i]->name, "parent-op") == 0) {
                sp_parent = ss->spans[i];
            } else if (strcmp(ss->spans[i]->name, "child-op") == 0) {
                sp_child = ss->spans[i];
            }
        }
        CHECK(sp_parent != NULL);
        CHECK(sp_child != NULL);

        if (sp_parent && sp_child) {
            /* IDs and parent linkage. */
            CHECK(sp_parent->trace_id.len == 16);
            CHECK(sp_parent->span_id.len == 8);
            CHECK(sp_parent->parent_span_id.len == 0);   /* root */
            CHECK(sp_parent->kind ==
                  OPENTELEMETRY__PROTO__TRACE__V1__SPAN__SPAN_KIND__SPAN_KIND_SERVER);

            CHECK(sp_child->trace_id.len == 16);
            /* Shared trace id. */
            CHECK(memcmp(sp_child->trace_id.data, sp_parent->trace_id.data, 16) == 0);
            /* child.parent_span_id == parent.span_id */
            CHECK(sp_child->parent_span_id.len == 8);
            CHECK(memcmp(sp_child->parent_span_id.data, sp_parent->span_id.data, 8) == 0);

            /* Timestamps present and ordered. */
            CHECK(sp_parent->start_time_unix_nano > 0);
            CHECK(sp_parent->end_time_unix_nano >= sp_parent->start_time_unix_nano);

            /* Parent attributes + event (with its own attributes). */
            CHECK(sp_parent->n_attributes == 2);
            CHECK(sp_parent->n_events == 1);
            CHECK(strcmp(sp_parent->events[0]->name, "received") == 0);
            CHECK(sp_parent->events[0]->n_attributes == 2);
            {
                int found_queue = 0, found_len = 0;
                for (size_t i = 0; i < sp_parent->events[0]->n_attributes; i++) {
                    Opentelemetry__Proto__Common__V1__KeyValue *kv =
                        sp_parent->events[0]->attributes[i];
                    if (strcmp(kv->key, "queue") == 0) {
                        found_queue = (strcmp(kv->value->string_value, "rx0") == 0);
                    } else if (strcmp(kv->key, "len") == 0) {
                        found_len = (kv->value->int_value == 512);
                    }
                }
                CHECK(found_queue);
                CHECK(found_len);
            }

            /* Child status + attr. */
            CHECK(sp_child->n_attributes == 1);
            CHECK(sp_child->status != NULL);
            CHECK(sp_child->status->code ==
                  OPENTELEMETRY__PROTO__TRACE__V1__STATUS__STATUS_CODE__STATUS_CODE_ERROR);
            CHECK(strcmp(sp_child->status->message, "boom") == 0);
        }

        opentelemetry__proto__collector__trace__v1__export_trace_service_request__free_unpacked(
            req, NULL);
    }

    /* ---- sampling ---- */

    /* ratio 0.0: no trace is recorded; children of an unsampled root inherit
     * that, and operations short-circuit (end is a no-op). */
    otel_set_sampler(0.0);
    struct otel_span s0, c0;
    otel_span_start(&s0, "unsampled", OTEL_SPAN_INTERNAL);
    CHECK(!otel_span_recording(&s0));
    otel_span_start_child(&c0, "unsampled-child", OTEL_SPAN_INTERNAL, &s0);
    CHECK(!otel_span_recording(&c0));
    otel_span_attr_str(&c0, "k", "v");           /* no-op, must not crash */
    otel_span_end(&c0);
    otel_span_end(&s0);
    CHECK(otel_drain() == 0);                     /* nothing was staged */

    /* ratio 1.0: every trace is recorded. */
    otel_set_sampler(1.0);
    struct otel_span s1;
    otel_span_start(&s1, "sampled", OTEL_SPAN_INTERNAL);
    CHECK(otel_span_recording(&s1));
    otel_span_end(&s1);
    CHECK(otel_drain() == 1);

    /* ratio 0.5: roughly half of independent traces are recorded. */
    otel_set_sampler(0.5);
    int rec = 0, N = 4000;
    for (int k = 0; k < N; k++) {
        struct otel_span sp;
        otel_span_start(&sp, "p", OTEL_SPAN_INTERNAL);
        if (otel_span_recording(&sp)) {
            rec++;
        }
        otel_span_end(&sp);
        otel_drain();                            /* keep the ring from filling */
    }
    CHECK(rec > N / 4 && rec < (3 * N) / 4);     /* loose bound around N/2 */

    otel_set_sampler(1.0);

    /* ---- runtime enable/disable ---- */
    otel_set_enabled(0);
    struct otel_span sd;
    otel_span_start(&sd, "disabled", OTEL_SPAN_INTERNAL);
    CHECK(!otel_span_recording(&sd));            /* new traces not recorded */
    otel_span_end(&sd);
    CHECK(otel_drain() == 0);

    otel_set_enabled(1);
    struct otel_span se;
    otel_span_start(&se, "reenabled", OTEL_SPAN_INTERNAL);
    CHECK(otel_span_recording(&se));
    otel_span_end(&se);
    CHECK(otel_drain() == 1);

    /* ---- string arena: name/key/value are copied, so they survive the caller
     * mutating (or freeing) the source strings before the span is drained. ---- */
    {
        char nbuf[16], kbuf[16], vbuf[32];

        strcpy(nbuf, "dyn-op");
        strcpy(kbuf, "dyn-key");
        strcpy(vbuf, "dyn-value");

        struct otel_span ms;
        otel_span_start(&ms, nbuf, OTEL_SPAN_INTERNAL);
        otel_span_attr_str(&ms, kbuf, vbuf);
        otel_span_attr_strn(&ms, "five", "abcdefghij", 5);   /* -> "abcde" */

        /* Clobber the caller's buffers before the span is drained. */
        memset(nbuf, 'Z', sizeof(nbuf));
        memset(kbuf, 'Z', sizeof(kbuf));
        memset(vbuf, 'Z', sizeof(vbuf));

        otel_span_end(&ms);
        g_len = 0;
        CHECK(otel_drain() == 1);

        struct grpc_hdr *mh = (struct grpc_hdr *) g_buf;
        Opentelemetry__Proto__Collector__Trace__V1__ExportTraceServiceRequest *mreq =
            opentelemetry__proto__collector__trace__v1__export_trace_service_request__unpack(
                NULL, ntohl(mh->length), g_buf + sizeof(*mh));
        CHECK(mreq != NULL);
        if (mreq) {
            Opentelemetry__Proto__Trace__V1__Span *msp =
                mreq->resource_spans[0]->scope_spans[0]->spans[0];
            CHECK(strcmp(msp->name, "dyn-op") == 0);
            CHECK(msp->n_attributes == 2);
            int found_kv = 0, found_five = 0;
            for (size_t i = 0; i < msp->n_attributes; i++) {
                if (strcmp(msp->attributes[i]->key, "dyn-key") == 0) {
                    found_kv = (strcmp(msp->attributes[i]->value->string_value,
                                       "dyn-value") == 0);
                } else if (strcmp(msp->attributes[i]->key, "five") == 0) {
                    found_five = (strcmp(msp->attributes[i]->value->string_value,
                                         "abcde") == 0);
                }
            }
            CHECK(found_kv);
            CHECK(found_five);
            opentelemetry__proto__collector__trace__v1__export_trace_service_request__free_unpacked(
                mreq, NULL);
        }
    }

    /* ---- shared arena: attrs and events have no fixed per-kind cap; they share
     * one byte budget with the strings.  Add far more of each than the old inline
     * arrays (8 attrs / 4 events) held, and confirm they all round-trip. ---- */
    {
        const int NA = 40, NE = 20;
        char      key[32];
        struct otel_span big;

        otel_span_start(&big, "big-op", OTEL_SPAN_INTERNAL);
        for (int k = 0; k < NA; k++) {
            snprintf(key, sizeof(key), "k%d", k);
            otel_span_attr_i64(&big, key, k);
        }
        for (int k = 0; k < NE; k++) {
            snprintf(key, sizeof(key), "ev%d", k);
            otel_span_event(&big, key);
        }
        otel_span_end(&big);

        g_len = 0;
        CHECK(otel_drain() == 1);

        struct grpc_hdr *bh = (struct grpc_hdr *) g_buf;
        Opentelemetry__Proto__Collector__Trace__V1__ExportTraceServiceRequest *breq =
            opentelemetry__proto__collector__trace__v1__export_trace_service_request__unpack(
                NULL, ntohl(bh->length), g_buf + sizeof(*bh));
        CHECK(breq != NULL);
        if (breq) {
            Opentelemetry__Proto__Trace__V1__Span *bsp =
                breq->resource_spans[0]->scope_spans[0]->spans[0];
            CHECK(bsp->n_attributes == (size_t) NA);
            CHECK(bsp->n_events == (size_t) NE);
            CHECK(bsp->dropped_attributes_count == 0);
            CHECK(bsp->dropped_events_count == 0);
            /* Spot-check that values survived in list order. */
            CHECK(strcmp(bsp->attributes[0]->key, "k0") == 0);
            CHECK(bsp->attributes[NA - 1]->value->int_value == NA - 1);
            CHECK(strcmp(bsp->events[NE - 1]->name, "ev19") == 0);
            opentelemetry__proto__collector__trace__v1__export_trace_service_request__free_unpacked(
                breq, NULL);
        }
    }

    otel_thread_unregister();
    otel_shutdown();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("oteltracing_test: OK (%d spans round-tripped)\n", n);
    return 0;
}
