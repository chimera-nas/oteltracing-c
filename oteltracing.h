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

/*
 * A span carries a single inline arena from which everything variable-length is
 * sub-allocated: the name, the status message, attribute records, event records,
 * and all of their strings (attr keys, attr string values, event names).  There
 * are therefore no fixed per-kind caps -- attributes, events and strings simply
 * share one byte budget, and whatever overflows the arena is dropped and counted
 * (see otel_dropped_*).  Attributes and events are chained as singly-linked lists
 * threaded through the arena.
 *
 * The arena is addressed by byte OFFSETS (uint16_t) rather than pointers, so a
 * span is position-independent: it can be copied by value (the tracer memcpy's it
 * into its export ring) with no pointer fix-up at all.  OTEL_NIL marks an empty
 * list / absent string.  Allocations are rounded up to OTEL_ARENA_ALIGN bytes so
 * that records (which contain 64-bit fields) stay naturally aligned.
 *
 * OTEL_SPAN_ARENA is chosen so the whole struct otel_span is exactly 4 KiB on a
 * 64-bit target (the fixed fields below occupy 72 bytes). */
#define OTEL_SPAN_ARENA   4024
#define OTEL_ARENA_ALIGN  8
#define OTEL_NIL          0xFFFF

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

/*
 * Attribute record.  Lives in the span arena and is reached through the span's
 * attr list (see otel_attr_first/next).  All references into the arena are byte
 * offsets, not pointers, so the span stays copyable by value.  Treat as opaque;
 * use the otel_attr_* accessors.
 */
struct otel_attr {
    uint16_t next;             /* arena offset of the next attr, or OTEL_NIL */
    uint16_t key;              /* arena offset of the key string */
    uint16_t val;             /* arena offset of the string value (STR), else OTEL_NIL */
    uint8_t  type;             /* enum otel_attr_type */
    uint8_t  pad;
    union {                    /* numeric value (string value lives at `val`) */
        int64_t  i;
        uint64_t u;
        double   d;
        int      b;
    } v;
};

/* Event record.  Lives in the span arena; reached through otel_event_first/next.
 * An event carries its own attribute list (OTLP allows per-event attributes),
 * built from the same struct otel_attr records and reached via
 * otel_event_attr_first/otel_attr_next. */
struct otel_event {
    uint16_t next;             /* arena offset of the next event, or OTEL_NIL */
    uint16_t name;             /* arena offset of the name string */
    uint16_t attr_head;        /* arena offset of the first event attr, or OTEL_NIL */
    uint16_t attr_tail;        /* arena offset of the last event attr */
    uint8_t  num_attrs;
    uint8_t  dropped_attrs;
    uint16_t pad;
    uint64_t time_unix_ns;
};

/*
 * Embeddable span.  Treat fields as opaque; use the otel_span_* helpers.
 *
 * The span is self-contained and position-independent: the name, status message,
 * attribute and event records, and every string they reference all live in the
 * inline `arena` and are addressed by byte offsets into it.  Because nothing
 * stores a pointer, a span can be copied by value (the tracer memcpy's it into
 * its ring) with no fix-up.  The fixed fields total 72 bytes; OTEL_SPAN_ARENA
 * makes the whole struct exactly 4 KiB.
 */
struct otel_span {
    uint8_t  trace_id[16];
    uint64_t span_id;
    uint64_t parent_id;
    uint64_t start_unix_ns;
    uint64_t end_unix_ns;
    uint16_t name;             /* arena offset of the name, or OTEL_NIL */
    uint16_t status_message;   /* arena offset of the status message, or OTEL_NIL */
    uint16_t attr_head;        /* arena offset of the first attr, or OTEL_NIL */
    uint16_t attr_tail;        /* arena offset of the last attr (O(1) append) */
    uint16_t event_head;       /* arena offset of the first event, or OTEL_NIL */
    uint16_t event_tail;       /* arena offset of the last event */
    uint16_t arena_used;       /* bytes consumed in arena */
    uint16_t num_attrs;        /* attrs in the list */
    uint16_t num_events;       /* events in the list */
    uint8_t  kind;             /* enum otel_span_kind */
    uint8_t  flags;            /* OTEL_FLAG_* */
    uint8_t  status;           /* enum otel_status */
    uint8_t  dropped_attrs;
    uint8_t  dropped_events;
    uint8_t  pad;
    char     arena[OTEL_SPAN_ARENA];
};

#ifdef __cplusplus
static_assert(sizeof(struct otel_span) == 4096,
              "otel_span must be 4 KiB; adjust OTEL_SPAN_ARENA");
#else
_Static_assert(sizeof(struct otel_span) == 4096,
               "otel_span must be 4 KiB; adjust OTEL_SPAN_ARENA");
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
struct otel_event *otel_span_event_(struct otel_span *s, const char *name);
void otel_span_end_(struct otel_span *s);

/* Reserve `n` bytes from the span's arena (rounded up to OTEL_ARENA_ALIGN so the
 * next allocation stays aligned) and return the byte offset, or OTEL_NIL if it
 * does not fit.  Internal building block for strings and records. */
static inline uint16_t
otel_arena_alloc(struct otel_span *s, size_t n)
{
    size_t off = ((size_t) s->arena_used + (OTEL_ARENA_ALIGN - 1)) &
        ~(size_t) (OTEL_ARENA_ALIGN - 1);
    size_t end = off + ((n + (OTEL_ARENA_ALIGN - 1)) & ~(size_t) (OTEL_ARENA_ALIGN - 1));

    if (end > OTEL_SPAN_ARENA) {
        return (uint16_t) OTEL_NIL;
    }
    s->arena_used = (uint16_t) end;
    return (uint16_t) off;
}

/* Copy up to `len` bytes of `str` (plus a NUL) into the span's arena and return
 * the byte offset of the copy, or OTEL_NIL if it does not fit (or str is NULL). */
static inline uint16_t
otel_arena_putn(struct otel_span *s, const char *str, size_t len)
{
    uint16_t off;
    char    *dst;

    if (!str) {
        return (uint16_t) OTEL_NIL;
    }
    off = otel_arena_alloc(s, len + 1);
    if (off == (uint16_t) OTEL_NIL) {
        return (uint16_t) OTEL_NIL;   /* arena full */
    }
    dst = &s->arena[off];
    memcpy(dst, str, len);
    dst[len] = '\0';
    return off;
}

static inline uint16_t
otel_arena_put(struct otel_span *s, const char *str)
{
    return str ? otel_arena_putn(s, str, strlen(str)) : (uint16_t) OTEL_NIL;
}

/* ---- read accessors (used by the exporter / a span sink) ----
 *
 * Resolve arena offsets back to C strings and walk the attr/event lists.  All
 * returned pointers point into the span's own arena and are valid for as long as
 * the span is. */
static inline const char *
otel_span_strv(const struct otel_span *s, uint16_t off)
{
    return off == (uint16_t) OTEL_NIL ? "" : &s->arena[off];
}

static inline const char *
otel_span_name(const struct otel_span *s)
{
    return otel_span_strv(s, s->name);
}

static inline const char *
otel_span_status_message(const struct otel_span *s)
{
    return otel_span_strv(s, s->status_message);
}

static inline const struct otel_attr *
otel_attr_first(const struct otel_span *s)
{
    return s->attr_head == (uint16_t) OTEL_NIL ?
           (const struct otel_attr *) 0 :
           (const struct otel_attr *) &s->arena[s->attr_head];
}

static inline const struct otel_attr *
otel_attr_next(const struct otel_span *s, const struct otel_attr *a)
{
    return a->next == (uint16_t) OTEL_NIL ?
           (const struct otel_attr *) 0 :
           (const struct otel_attr *) &s->arena[a->next];
}

static inline const char *
otel_attr_key(const struct otel_span *s, const struct otel_attr *a)
{
    return otel_span_strv(s, a->key);
}

static inline const char *
otel_attr_strval(const struct otel_span *s, const struct otel_attr *a)
{
    return otel_span_strv(s, a->val);
}

static inline const struct otel_event *
otel_event_first(const struct otel_span *s)
{
    return s->event_head == (uint16_t) OTEL_NIL ?
           (const struct otel_event *) 0 :
           (const struct otel_event *) &s->arena[s->event_head];
}

static inline const struct otel_event *
otel_event_next(const struct otel_span *s, const struct otel_event *e)
{
    return e->next == (uint16_t) OTEL_NIL ?
           (const struct otel_event *) 0 :
           (const struct otel_event *) &s->arena[e->next];
}

static inline const char *
otel_event_name(const struct otel_span *s, const struct otel_event *e)
{
    return otel_span_strv(s, e->name);
}

/* First attribute of an event (walk onward with otel_attr_next; resolve with
 * otel_attr_key / otel_attr_strval, exactly like span attributes). */
static inline const struct otel_attr *
otel_event_attr_first(const struct otel_span *s, const struct otel_event *e)
{
    return e->attr_head == (uint16_t) OTEL_NIL ?
           (const struct otel_attr *) 0 :
           (const struct otel_attr *) &s->arena[e->attr_head];
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
    uint16_t n;

    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return;
    }
    n = otel_arena_put(s, name);
    if (n != (uint16_t) OTEL_NIL) {
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

/* Internal: allocate an attribute record from the arena, copy its key in, and
 * append it to the span's attr list.  Returns the record, or NULL if not
 * recording or the arena could not fit the key + record (dropped + counted).
 * The returned record's value field is left for the caller to fill. */
static inline struct otel_attr *
otel_span_attr_(struct otel_span *s, const char *key)
{
    struct otel_attr *a;
    uint16_t          koff, roff;

    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return (struct otel_attr *) 0;
    }
    koff = otel_arena_put(s, key);
    roff = otel_arena_alloc(s, sizeof(struct otel_attr));
    if (koff == (uint16_t) OTEL_NIL || roff == (uint16_t) OTEL_NIL) {
        if (s->dropped_attrs < 255) {
            s->dropped_attrs++;   /* arena full: drop the whole attr */
        }
        return (struct otel_attr *) 0;
    }

    a       = (struct otel_attr *) &s->arena[roff];
    a->next = (uint16_t) OTEL_NIL;
    a->key  = koff;
    a->val  = (uint16_t) OTEL_NIL;
    a->type = OTEL_ATTR_STR;

    if (s->attr_tail == (uint16_t) OTEL_NIL) {
        s->attr_head = roff;
    } else {
        ((struct otel_attr *) &s->arena[s->attr_tail])->next = roff;
    }
    s->attr_tail = roff;
    s->num_attrs++;
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
        a->val  = otel_arena_put(s, value);
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
        a->val  = otel_arena_putn(s, value, len);
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

/* Add a timestamped event and return a handle to it, or NULL if the span is not
 * recording (or the arena was full).  Pass the handle to otel_event_attr_* to
 * attach per-event attributes; ignore it if the event needs none.  The handle
 * stays valid until the span ends (the arena never relocates existing records). */
static inline struct otel_event *
otel_span_event(struct otel_span *s, const char *name)
{
    if (!(s->flags & OTEL_FLAG_RECORDING)) {
        return (struct otel_event *) 0;
    }
    return otel_span_event_(s, name);   /* slow path: needs the clock */
}

/* Internal: allocate an attribute record from the arena, copy its key in, and
 * append it to an EVENT's attr list.  Returns the record (value left to the
 * caller) or NULL if `e` is NULL or the arena could not fit it (dropped+counted
 * on the event). */
static inline struct otel_attr *
otel_event_attr_(struct otel_span *s, struct otel_event *e, const char *key)
{
    struct otel_attr *a;
    uint16_t          koff, roff;

    if (!e) {
        return (struct otel_attr *) 0;
    }
    koff = otel_arena_put(s, key);
    roff = otel_arena_alloc(s, sizeof(struct otel_attr));
    if (koff == (uint16_t) OTEL_NIL || roff == (uint16_t) OTEL_NIL) {
        if (e->dropped_attrs < 255) {
            e->dropped_attrs++;
        }
        return (struct otel_attr *) 0;
    }

    a       = (struct otel_attr *) &s->arena[roff];
    a->next = (uint16_t) OTEL_NIL;
    a->key  = koff;
    a->val  = (uint16_t) OTEL_NIL;
    a->type = OTEL_ATTR_STR;

    if (e->attr_tail == (uint16_t) OTEL_NIL) {
        e->attr_head = roff;
    } else {
        ((struct otel_attr *) &s->arena[e->attr_tail])->next = roff;
    }
    e->attr_tail = roff;
    e->num_attrs++;
    return a;
}

/* Attach attributes to an event handle returned by otel_span_event.  No-op if
 * the handle is NULL (unsampled trace) or the arena is full.  String values are
 * copied into the span's arena. */
static inline void
otel_event_attr_str(struct otel_span *s, struct otel_event *e,
                    const char *key, const char *value)
{
    struct otel_attr *a = otel_event_attr_(s, e, key);

    if (a) {
        a->type = OTEL_ATTR_STR;
        a->val  = otel_arena_put(s, value);
    }
}

static inline void
otel_event_attr_strn(struct otel_span *s, struct otel_event *e,
                     const char *key, const char *value, size_t len)
{
    struct otel_attr *a = otel_event_attr_(s, e, key);

    if (a) {
        a->type = OTEL_ATTR_STR;
        a->val  = otel_arena_putn(s, value, len);
    }
}

static inline void
otel_event_attr_i64(struct otel_span *s, struct otel_event *e,
                    const char *key, int64_t value)
{
    struct otel_attr *a = otel_event_attr_(s, e, key);

    if (a) {
        a->type = OTEL_ATTR_I64;
        a->v.i  = value;
    }
}

static inline void
otel_event_attr_u64(struct otel_span *s, struct otel_event *e,
                    const char *key, uint64_t value)
{
    struct otel_attr *a = otel_event_attr_(s, e, key);

    if (a) {
        a->type = OTEL_ATTR_U64;
        a->v.u  = value;
    }
}

static inline void
otel_event_attr_bool(struct otel_span *s, struct otel_event *e,
                     const char *key, int value)
{
    struct otel_attr *a = otel_event_attr_(s, e, key);

    if (a) {
        a->type = OTEL_ATTR_BOOL;
        a->v.b  = value ? 1 : 0;
    }
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
    s->status_message = otel_arena_put(s, message);
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
struct otel_event;   /* opaque event handle; never dereferenced in this mode */

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
static inline struct otel_event *otel_span_event(struct otel_span *s, const char *name)
{ (void) s; (void) name; return (struct otel_event *) 0; }
static inline void otel_event_attr_str(struct otel_span *s, struct otel_event *e, const char *key, const char *value)
{ (void) s; (void) e; (void) key; (void) value; }
static inline void otel_event_attr_strn(struct otel_span *s, struct otel_event *e, const char *key, const char *value, size_t len)
{ (void) s; (void) e; (void) key; (void) value; (void) len; }
static inline void otel_event_attr_i64(struct otel_span *s, struct otel_event *e, const char *key, int64_t value)
{ (void) s; (void) e; (void) key; (void) value; }
static inline void otel_event_attr_u64(struct otel_span *s, struct otel_event *e, const char *key, uint64_t value)
{ (void) s; (void) e; (void) key; (void) value; }
static inline void otel_event_attr_bool(struct otel_span *s, struct otel_event *e, const char *key, int value)
{ (void) s; (void) e; (void) key; (void) value; }
static inline void otel_span_set_status(struct otel_span *s, enum otel_status status,
                                        const char *message)
{ (void) s; (void) status; (void) message; }
static inline void otel_span_end(struct otel_span *s) { (void) s; }

#endif /* OTEL_TRACING */

#ifdef __cplusplus
}
#endif

#endif /* OTELTRACING_H */
