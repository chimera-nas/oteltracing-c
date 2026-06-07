// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: Apache-2.0

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
    otel_span_event(&parent, "received");

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

            /* Parent attributes + event. */
            CHECK(sp_parent->n_attributes == 2);
            CHECK(sp_parent->n_events == 1);
            CHECK(strcmp(sp_parent->events[0]->name, "received") == 0);

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

    otel_thread_unregister();
    otel_shutdown();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("oteltracing_test: OK (%d spans round-tripped)\n", n);
    return 0;
}
