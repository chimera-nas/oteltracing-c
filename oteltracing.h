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
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile-time master switch.  Define OTEL_TRACING=0 (e.g. -DOTEL_TRACING=0) to
 * strip tracing entirely from a translation unit: struct otel_span becomes
 * zero-size and every API call below compiles to nothing, with no link
 * dependency on liboteltracing-c.  Defaults to enabled.
 */
#ifndef OTEL_TRACING
#define OTEL_TRACING 1
#endif

/* Maximum attributes/events stored inline in a span.  Overflow beyond these is
 * dropped and counted (see otel_dropped_*).  Kept small because these structs
 * are embedded in pooled, numerous request objects. */
#define OTEL_SPAN_MAX_ATTRS  8
#define OTEL_SPAN_MAX_EVENTS 4

/* Inline string arena size, chosen so that the whole struct otel_span is exactly
 * 4 KiB on a 64-bit target (the fixed fields below occupy 336 bytes).  All span
 * strings -- the name, attribute keys and string values, event names, and the
 * status message -- are copied into this arena so the span is self-contained: it
 * borrows no caller memory and can be copied by value.  When the arena fills, a
 * string is simply dropped (its pointer becomes NULL). */
#define OTEL_SPAN_STRBUF 3760

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

#if OTEL_TRACING

struct otel_attr {
    const char         *key;   /* into the span's strbuf (or NULL) */
    uint8_t             type;   /* enum otel_attr_type */
    union {
        const char *s;         /* into the span's strbuf (or NULL) */
        int64_t     i;
        uint64_t    u;
        double      d;
        int         b;
    } v;
};

struct otel_event {
    const char *name;          /* into the span's strbuf (or NULL) */
    uint64_t    time_unix_ns;
};

/*
 * Embeddable span.  Treat fields as opaque; use the otel_span_* helpers.
 *
 * The span is self-contained: every string (name, attr keys/values, event
 * names, status message) is copied into the inline strbuf arena, and the string
 * pointers point into that arena.  This means a span can be copied by value and
 * borrows no caller memory -- the helpers copy the strings in.  (On enqueue the
 * tracer rebases the arena pointers into the ring slot's own copy.)  The fixed
 * fields total 336 bytes; OTEL_SPAN_STRBUF makes the whole struct exactly 4 KiB.
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
    uint16_t str_used;         /* bytes consumed in strbuf */
    const char *status_message;
    struct otel_attr  attrs[OTEL_SPAN_MAX_ATTRS];
    struct otel_event events[OTEL_SPAN_MAX_EVENTS];
    char     strbuf[OTEL_SPAN_STRBUF];
};

#ifdef __cplusplus
static_assert(sizeof(struct otel_span) == 4096,
              "otel_span must be 4 KiB; adjust OTEL_SPAN_STRBUF");
#else
_Static_assert(sizeof(struct otel_span) == 4096,
               "otel_span must be 4 KiB; adjust OTEL_SPAN_STRBUF");
#endif

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

/*
 * Span sink: an alternative (or additional) consumer fed the raw finished spans
 * during otel_drain(), instead of (or alongside) the gRPC transport.  Unlike the
 * transport -- which receives an already-encoded protobuf buffer -- a sink sees
 * the native struct otel_span, so a sink (e.g. the optional SQLite backend) can
 * persist spans without a protobuf round-trip.
 *
 * The three callbacks bracket one drain pass: begin() once before any spans of a
 * drain (open a transaction), span() for each finished span, end() once after
 * (commit).  All run on the single drain/flusher thread, never on a span's home
 * (hot-path) thread, so a slow sink can never block span production -- it only
 * slows draining, which makes the per-thread rings fill and overflowing spans
 * drop (counted, see otel_dropped_spans).  begin/end may be NULL.
 *
 * Borrowed-pointer rules match the transport: a span's name/attr/event strings
 * are valid only for the duration of the span() call -- copy them if you persist.
 */
struct otel_span_sink {
    void (*begin)(void *priv);
    void (*span)(const struct otel_span *s, void *priv);
    void (*end)(void *priv);
    void  *priv;
};
void otel_set_span_sink(const struct otel_span_sink *sink);   /* NULL detaches */

/* The configured OTLP service.name (set via otel_init).  Useful to a span sink
 * that wants to denormalize the service onto each stored span. */
const char *otel_service_name(void);

/*
 * Per-thread ring capacity (finished-span backlog) in spans; must be a power of
 * two.  This is the buffer between the lock-free span hot path (producer) and the
 * drain/flusher (consumer): when it fills -- e.g. the sink can't keep up -- newly
 * ended spans are dropped and counted rather than blocking the producer.  Larger
 * capacity absorbs longer sink-latency spikes at the cost of memory per thread.
 * Takes effect for threads registered after the call; set it before
 * otel_thread_register().  Default OTEL_RING_SIZE (2048).
 */
void otel_set_ring_capacity(unsigned int spans);

/*
 * Head-sampling ratio in [0,1]: the probability that a newly started (root)
 * trace is recorded.  1.0 (the default) records every trace; 0.0 records none;
 * 0.01 records roughly 1 in 100.  The decision is made once, when a root span
 * starts, using a fast per-thread PRNG, and is inherited by the entire trace
 * (every child span).  Spans belonging to an unsampled trace are not recorded,
 * so the inline hot path below short-circuits to nothing.  Thread-safe to set at
 * startup; treat as configuration, not a per-request knob.
 */
void otel_set_sampler(double ratio);

/*
 * Runtime master switch.  Tracing is live only when a transport is registered
 * AND enabled here (default on).  Disabling stops new traces from being recorded
 * immediately and cheaply -- the inline hot path just tests a global and bails;
 * in-flight spans already recording still complete.  Re-enabling resumes new
 * traces.  Use this for a runtime on/off knob without rebuilding.
 */
void otel_set_enabled(int enabled);

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

/* ---- span hot path (no malloc, no locks) ----
 *
 * The recording (sampled) decision lives in the span's flags.  The inline
 * wrappers below test that bit first, so every operation on a span belonging to
 * an unsampled trace compiles to a flag test and an immediate return -- no call
 * into the library, no work.  Only the cases that actually touch library state
 * (deciding sampling for a root, generating ids, stamping the clock, staging a
 * finished span) fall through to the out-of-line slow paths.
 */

/* Internal: live (a transport is registered) and slow paths.  Do not call the
 * trailing-underscore symbols directly; use the inline wrappers. */
extern int otel_enabled_;
void otel_span_start_root_(struct otel_span *s, const char *name, uint8_t kind);
void otel_span_start_child_(struct otel_span *s, const char *name, uint8_t kind,
                            const struct otel_span *parent);
void otel_span_start_remote_(struct otel_span *s, const char *name, uint8_t kind,
                             const uint8_t trace_id[16], uint64_t parent_id);
void otel_span_event_(struct otel_span *s, const char *name);
void otel_span_end_(struct otel_span *s);

/* Copy up to `len` bytes of `str` (plus a NUL) into the span's string arena and
 * return a pointer to the copy, or NULL if it does not fit (or str is NULL).
 * Internal helper used by the string-bearing setters. */
static inline const char *
otel_strputn(struct otel_span *s, const char *str, size_t len)
{
    char  *dst;
    size_t avail;

    if (!str) {
        return (const char *) 0;
    }
    avail = (size_t) OTEL_SPAN_STRBUF - s->str_used;
    if (len + 1 > avail) {
        return (const char *) 0;   /* arena full: caller stores NULL */
    }
    dst = &s->strbuf[s->str_used];
    memcpy(dst, str, len);
    dst[len]     = '\0';
    s->str_used += (uint16_t) (len + 1);
    return dst;
}

static inline const char *
otel_strput(struct otel_span *s, const char *str)
{
    return str ? otel_strputn(s, str, strlen(str)) : (const char *) 0;
}

/* Start a new root span (begins a fresh trace; sampling is decided here). */
static inline void
otel_span_start(struct otel_span *s, const char *name, enum otel_span_kind kind)
{
    if (!otel_enabled_) {
        s->flags = 0;
        return;
    }
    otel_span_start_root_(s, name, (uint8_t) kind);
}

/* Replace the span name (copied into the arena).  Keeps the existing name if the
 * arena is full.  Use when the descriptive name is only known after start. */
static inline void
otel_span_set_name(struct otel_span *s, const char *name)
{
    const char *n;

    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return;
    }
    n = otel_strput(s, name);
    if (n) {
        s->name = n;
    }
}

/* Start a child span: inherits the parent's trace and sampling decision.  If the
 * parent is not recording (unsampled trace, or tracing off), this is just a flag
 * clear -- the fast path. */
static inline void
otel_span_start_child(struct otel_span *s, const char *name,
                      enum otel_span_kind kind, const struct otel_span *parent)
{
    if (!parent || !(parent->flags & OTEL_FLAG_RECORDING)) {
        s->flags = 0;
        return;
    }
    otel_span_start_child_(s, name, (uint8_t) kind, parent);
}

/* Start a span from a propagated context.  `sampled` carries the upstream
 * sampling decision (e.g. the W3C traceparent sampled flag); when 0 the trace is
 * not recorded here either. */
static inline void
otel_span_start_remote(struct otel_span *s, const char *name,
                       enum otel_span_kind kind,
                       const uint8_t trace_id[16], uint64_t parent_id, int sampled)
{
    if (!otel_enabled_ || !sampled) {
        s->flags = 0;
        return;
    }
    otel_span_start_remote_(s, name, (uint8_t) kind, trace_id, parent_id);
}

/* True if the span is being recorded (callers may skip building attrs if not). */
static inline int
otel_span_recording(const struct otel_span *s)
{
    return (s->flags & OTEL_FLAG_RECORDING) != 0;
}

/* Internal: claim the next inline attribute slot, or NULL if not recording, full
 * (dropped + counted), or the key string did not fit in the arena.  The key is
 * copied into the arena. */
static inline struct otel_attr *
otel_span_attr_(struct otel_span *s, const char *key)
{
    struct otel_attr *a;
    const char       *k;

    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return (struct otel_attr *) 0;
    }
    if (s->num_attrs >= OTEL_SPAN_MAX_ATTRS) {
        if (s->dropped_attrs < 255) {
            s->dropped_attrs++;
        }
        return (struct otel_attr *) 0;
    }
    k = otel_strput(s, key);
    if (!k) {
        if (s->dropped_attrs < 255) {
            s->dropped_attrs++;   /* arena full: drop the whole attr */
        }
        return (struct otel_attr *) 0;
    }
    a      = &s->attrs[s->num_attrs++];
    a->key = k;
    return a;
}

/* Add attributes / events.  Safe to call from a thread the request was handed to
 * mid-flight (these touch only the embedded span).  No-op on an unsampled trace;
 * dropped + counted past the inline capacity.  String values are copied into the
 * span's arena; if the arena is full the value is stored as NULL. */
static inline void
otel_span_attr_str(struct otel_span *s, const char *key, const char *value)
{
    struct otel_attr *a = otel_span_attr_(s, key);

    if (a) {
        a->type = OTEL_ATTR_STR;
        a->v.s  = otel_strput(s, value);
    }
}

/* Like otel_span_attr_str but for a value that is not NUL-terminated (copies
 * exactly `len` bytes). */
static inline void
otel_span_attr_strn(struct otel_span *s, const char *key,
                    const char *value, size_t len)
{
    struct otel_attr *a = otel_span_attr_(s, key);

    if (a) {
        a->type = OTEL_ATTR_STR;
        a->v.s  = otel_strputn(s, value, len);
    }
}

static inline void
otel_span_attr_i64(struct otel_span *s, const char *key, int64_t value)
{
    struct otel_attr *a = otel_span_attr_(s, key);

    if (a) {
        a->type = OTEL_ATTR_I64;
        a->v.i  = value;
    }
}

static inline void
otel_span_attr_u64(struct otel_span *s, const char *key, uint64_t value)
{
    struct otel_attr *a = otel_span_attr_(s, key);

    if (a) {
        a->type = OTEL_ATTR_U64;
        a->v.u  = value;
    }
}

static inline void
otel_span_attr_bool(struct otel_span *s, const char *key, int value)
{
    struct otel_attr *a = otel_span_attr_(s, key);

    if (a) {
        a->type = OTEL_ATTR_BOOL;
        a->v.b  = value ? 1 : 0;
    }
}

static inline void
otel_span_event(struct otel_span *s, const char *name)
{
    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return;
    }
    otel_span_event_(s, name);   /* slow path: needs the clock */
}

/* Set the span status (e.g. on error).  message is copied into the span's arena
 * (NULL if it does not fit). */
static inline void
otel_span_set_status(struct otel_span *s, enum otel_status status,
                     const char *message)
{
    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return;
    }
    s->status         = (uint8_t) status;
    s->status_message = otel_strput(s, message);
}

/* End the span on its home thread: stamps end time and stages it for emission. */
static inline void
otel_span_end(struct otel_span *s)
{
    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return;
    }
    otel_span_end_(s);   /* slow path: stamps end time, stages for export */
}

#else  /* OTEL_TRACING == 0 : tracing compiled out */

/*
 * Tracing is disabled at compile time.  struct otel_span is zero-size so the
 * member you embed costs nothing, and every API call is a no-op the compiler
 * elides.  No symbol from liboteltracing-c is referenced, so the library need
 * not be linked.
 */
struct otel_span { };

typedef void (*otel_transport_fn)(const void *buf, size_t len, void *priv);

struct otel_span_sink {
    void (*begin)(void *priv);
    void (*span)(const struct otel_span *s, void *priv);
    void (*end)(void *priv);
    void  *priv;
};

static inline int  otel_init(const char *service) { (void) service; return 0; }
static inline void otel_shutdown(void) { }
static inline void otel_thread_register(void) { }
static inline void otel_thread_unregister(void) { }
static inline void otel_set_transport(otel_transport_fn fn, void *priv) { (void) fn; (void) priv; }
static inline void otel_set_span_sink(const struct otel_span_sink *sink) { (void) sink; }
static inline const char *otel_service_name(void) { return ""; }
static inline void otel_set_ring_capacity(unsigned int spans) { (void) spans; }
static inline void otel_set_sampler(double ratio) { (void) ratio; }
static inline void otel_set_enabled(int enabled) { (void) enabled; }
static inline void otel_set_clock(uint64_t (*now_unix_ns)(void)) { (void) now_unix_ns; }
static inline int  otel_drain(void) { return 0; }
static inline uint64_t otel_dropped_spans(void) { return 0; }

static inline void
otel_span_start(struct otel_span *s, const char *name, enum otel_span_kind kind)
{ (void) s; (void) name; (void) kind; }

static inline void
otel_span_start_child(struct otel_span *s, const char *name,
                      enum otel_span_kind kind, const struct otel_span *parent)
{ (void) s; (void) name; (void) kind; (void) parent; }

static inline void
otel_span_start_remote(struct otel_span *s, const char *name,
                       enum otel_span_kind kind,
                       const uint8_t trace_id[16], uint64_t parent_id, int sampled)
{ (void) s; (void) name; (void) kind; (void) trace_id; (void) parent_id; (void) sampled; }

static inline int  otel_span_recording(const struct otel_span *s) { (void) s; return 0; }

static inline void otel_span_set_name(struct otel_span *s, const char *name)
{ (void) s; (void) name; }
static inline void otel_span_attr_str(struct otel_span *s, const char *key, const char *value)
{ (void) s; (void) key; (void) value; }
static inline void otel_span_attr_strn(struct otel_span *s, const char *key, const char *value, size_t len)
{ (void) s; (void) key; (void) value; (void) len; }
static inline void otel_span_attr_i64(struct otel_span *s, const char *key, int64_t value)
{ (void) s; (void) key; (void) value; }
static inline void otel_span_attr_u64(struct otel_span *s, const char *key, uint64_t value)
{ (void) s; (void) key; (void) value; }
static inline void otel_span_attr_bool(struct otel_span *s, const char *key, int value)
{ (void) s; (void) key; (void) value; }
static inline void otel_span_event(struct otel_span *s, const char *name)
{ (void) s; (void) name; }
static inline void otel_span_set_status(struct otel_span *s, enum otel_status status,
                                        const char *message)
{ (void) s; (void) status; (void) message; }
static inline void otel_span_end(struct otel_span *s) { (void) s; }

#endif /* OTEL_TRACING */

#ifdef __cplusplus
}
#endif

#endif /* OTELTRACING_H */
