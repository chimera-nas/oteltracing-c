// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * oteltracing-c -- a lightweight OpenTelemetry span tracer for C.
 *
 * Spans are designed to track microsecond-scale operations, so the begin/end/
 * annotate hot path performs no heap allocation and takes no locks.  A span is
 * an embeddable POD value (struct otel_span): callers place one inside their own
 * async/request structure rather than allocating it from the tracer.
 *
 * Threading model:
 *   - A span starts and ends on the same ("home") thread.
 *   - While in progress the owning request may be handed to another thread,
 *     which may add attributes/events and then hand it back.  Because all
 *     mutable span data lives inline in struct otel_span and ownership is a
 *     single-owner baton (only one thread touches the span at a time), this is
 *     safe without locks -- the caller's handoff mechanism provides the barrier.
 *   - Only threads that start/end spans need otel_thread_register(); a thread
 *     that merely annotates a handed-off span does not.
 *
 * Emission is OTLP/gRPC (protobuf-c).  The library is transport-agnostic: it
 * encodes finished spans into gRPC-framed ExportTraceServiceRequest buffers and
 * hands them to a transport callback the embedder registers (e.g. an HTTP/2 POST
 * driven by libevpl).  Draining is likewise driven by the embedder via
 * otel_drain() rather than an internal background thread.
 */

#ifndef OTELTRACING_H
#define OTELTRACING_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum attributes/events stored inline in a span.  Overflow beyond these is
 * dropped and counted (see otel_dropped_*).  Kept small because these structs
 * are embedded in pooled, numerous request objects. */
#define OTEL_SPAN_MAX_ATTRS  8
#define OTEL_SPAN_MAX_EVENTS 4

/* Span kind, mirrors OTLP SpanKind (INTERNAL is the default). */
enum otel_span_kind {
    OTEL_SPAN_INTERNAL = 0,
    OTEL_SPAN_SERVER   = 1,
    OTEL_SPAN_CLIENT   = 2,
    OTEL_SPAN_PRODUCER = 3,
    OTEL_SPAN_CONSUMER = 4,
};

/* Attribute value type. */
enum otel_attr_type {
    OTEL_ATTR_STR = 0,
    OTEL_ATTR_I64 = 1,
    OTEL_ATTR_U64 = 2,
    OTEL_ATTR_F64 = 3,
    OTEL_ATTR_BOOL = 4,
};

/* Span status code, mirrors OTLP StatusCode. */
enum otel_status {
    OTEL_STATUS_UNSET = 0,
    OTEL_STATUS_OK    = 1,
    OTEL_STATUS_ERROR = 2,
};

/* Per-span flags. */
#define OTEL_FLAG_RECORDING 0x01  /* span is sampled and will be emitted */

struct otel_attr {
    const char         *key;   /* borrowed; must outlive otel_drain() */
    uint8_t             type;   /* enum otel_attr_type */
    union {
        const char *s;         /* borrowed */
        int64_t     i;
        uint64_t    u;
        double      d;
        int         b;
    } v;
};

struct otel_event {
    const char *name;          /* borrowed; must outlive otel_drain() */
    uint64_t    time_unix_ns;
};

/*
 * Embeddable span.  Treat fields as opaque; use the otel_span_* helpers.
 *
 * String fields (name, attr keys/values, event names) are BORROWED pointers --
 * they must remain valid until the span has been drained.  In practice use
 * string literals or strings whose lifetime exceeds the request.
 */
struct otel_span {
    uint8_t  trace_id[16];
    uint64_t span_id;
    uint64_t parent_id;
    const char *name;
    uint64_t start_unix_ns;
    uint64_t end_unix_ns;
    uint8_t  kind;             /* enum otel_span_kind */
    uint8_t  flags;            /* OTEL_FLAG_* */
    uint8_t  status;           /* enum otel_status */
    uint8_t  num_attrs;
    uint8_t  num_events;
    uint8_t  dropped_attrs;
    uint8_t  dropped_events;
    const char *status_message;
    struct otel_attr  attrs[OTEL_SPAN_MAX_ATTRS];
    struct otel_event events[OTEL_SPAN_MAX_EVENTS];
};

/* ---- process / thread lifecycle ---- */

/*
 * Initialize the tracer with a service name (used as the OTLP service.name
 * resource attribute).  Tracing only actually emits once a transport has been
 * registered via otel_set_transport().  Returns 0 on success.
 */
int  otel_init(const char *service);

/* Tear down the tracer.  Drains any remaining spans through the transport. */
void otel_shutdown(void);

/* Register/unregister the calling thread.  Required on any thread that starts
 * or ends spans (it owns a per-thread, lock-free span staging area). */
void otel_thread_register(void);
void otel_thread_unregister(void);

/* ---- exporter wiring (called by the embedder, e.g. libevpl) ---- */

/*
 * Transport callback.  buf/len is a complete gRPC-framed OTLP
 * ExportTraceServiceRequest (5-byte gRPC frame header + packed protobuf), ready
 * to POST to <endpoint>/opentelemetry.proto.collector.trace.v1.TraceService/Export
 * with content-type application/grpc.  The buffer is owned by the tracer and is
 * only valid for the duration of the call -- copy it if you send asynchronously.
 */
typedef void (*otel_transport_fn)(const void *buf, size_t len, void *priv);
void otel_set_transport(otel_transport_fn fn, void *priv);

/* Override the wall-clock source (default clock_gettime(CLOCK_REALTIME)).  Lets
 * the embedder inject a TSC-backed clock.  Must return nanoseconds since the
 * Unix epoch. */
void otel_set_clock(uint64_t (*now_unix_ns)(void));

/*
 * Collect finished spans from all registered threads, encode them into
 * gRPC-framed OTLP buffers, and push each to the registered transport.  Called
 * by the embedder's exporter driver (an event-loop hook or dedicated thread).
 * Returns the number of spans emitted.
 */
int  otel_drain(void);

/* Diagnostics: monotonically-increasing counters. */
uint64_t otel_dropped_spans(void);  /* spans dropped due to staging overflow */

/* ---- span hot path (no malloc, no locks) ---- */

/* Start a new root span (begins a fresh trace). */
void otel_span_start(struct otel_span *s, const char *name, enum otel_span_kind kind);

/* Start a child span: inherits parent's trace_id, parent_id = parent's span_id. */
void otel_span_start_child(struct otel_span *s, const char *name,
                           enum otel_span_kind kind, const struct otel_span *parent);

/* Start a span from a remote/propagated context (16-byte trace id + parent span id). */
void otel_span_start_remote(struct otel_span *s, const char *name,
                            enum otel_span_kind kind,
                            const uint8_t trace_id[16], uint64_t parent_id);

/* Add attributes / events.  Safe to call from a thread the request was handed
 * to mid-flight.  Silently dropped (and counted) past the inline capacity, or
 * if the span is not recording. */
void otel_span_attr_str(struct otel_span *s, const char *key, const char *value);
void otel_span_attr_i64(struct otel_span *s, const char *key, int64_t value);
void otel_span_attr_u64(struct otel_span *s, const char *key, uint64_t value);
void otel_span_attr_bool(struct otel_span *s, const char *key, int value);
void otel_span_event(struct otel_span *s, const char *name);

/* Set the span status (e.g. on error).  message is borrowed. */
void otel_span_set_status(struct otel_span *s, enum otel_status status,
                          const char *message);

/* End the span on its home thread: stamps end time and stages it for emission. */
void otel_span_end(struct otel_span *s);

/* True if the span is being recorded (callers may skip building attrs if not). */
int  otel_span_recording(const struct otel_span *s);

#ifdef __cplusplus
}
#endif

#endif /* OTELTRACING_H */
