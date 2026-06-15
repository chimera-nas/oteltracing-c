// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * oteltracing-c implementation.  See oteltracing.h for the model.
 *
 * Hot path (otel_span_start/attr/event/end): touches only the caller's embedded
 * span plus, at end(), the home thread's lock-free SPSC ring -- no malloc, no
 * locks.  Finished spans are copied (POD, attrs/events inline) into the ring.
 *
 * otel_drain() is the single consumer: it walks the registered thread contexts,
 * drains each ring, maps spans into a pre-allocated protobuf-c batch, packs the
 * OTLP ExportTraceServiceRequest, prepends the 5-byte gRPC frame header, and
 * hands the buffer to the registered transport callback.
 */

#define _GNU_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <uuid/uuid.h>

/* The library implementation always needs the full definitions, regardless of
 * any -DOTEL_TRACING=0 in the surrounding build. */
#undef OTEL_TRACING
#define OTEL_TRACING 1
#include "oteltracing.h"

#include "stopwatch.h"

#include "opentelemetry/proto/collector/trace/v1/trace_service.pb-c.h"
#include "opentelemetry/proto/trace/v1/trace.pb-c.h"
#include "opentelemetry/proto/common/v1/common.pb-c.h"
#include "opentelemetry/proto/resource/v1/resource.pb-c.h"

#ifndef SYMBOL_EXPORT
#define SYMBOL_EXPORT __attribute__((visibility("default")))
#endif

/* Per-thread ring capacity (spans).  Power of two.  Allocated once at thread
 * registration; ring-full simply drops + counts. */
#ifndef OTEL_RING_SIZE
#define OTEL_RING_SIZE 2048
#endif

/* Max spans packed into a single ExportTraceServiceRequest, and the size of the
 * shared protobuf key-value / event scratch pools the batch carves slices from.
 * A span's attrs and events now share its arena, so a single span can hold many
 * more than the old fixed caps (bounded only by the ~4 KiB arena, i.e. a couple
 * hundred records); the pools are sized to comfortably hold one batch's worth. */
#define OTEL_BATCH_SPANS  256
#define OTEL_BATCH_ATTRS  2048
#define OTEL_BATCH_EVENTS 1024

/* Generous upper bound on a single packed span; the request scratch is sized from
 * it.  A span's variable data is capped by its 4 KiB arena, so even a span packed
 * full of attributes serializes to well under this. */
#define OTEL_SPAN_SERIALIZATION 8192

typedef Opentelemetry__Proto__Collector__Trace__V1__ExportTraceServiceRequest otlp_request_t;
typedef Opentelemetry__Proto__Trace__V1__ResourceSpans                        otlp_rspans_t;
typedef Opentelemetry__Proto__Trace__V1__ScopeSpans                           otlp_sspans_t;
typedef Opentelemetry__Proto__Trace__V1__Span                                 otlp_span_t;
typedef Opentelemetry__Proto__Trace__V1__Status                               otlp_status_t;
typedef Opentelemetry__Proto__Trace__V1__Span__Event                          otlp_event_t;
typedef Opentelemetry__Proto__Resource__V1__Resource                          otlp_resource_t;
typedef Opentelemetry__Proto__Common__V1__InstrumentationScope                otlp_scope_t;
typedef Opentelemetry__Proto__Common__V1__KeyValue                            otlp_kv_t;
typedef Opentelemetry__Proto__Common__V1__AnyValue                            otlp_any_t;

#define OTLP_KIND_INTERNAL OPENTELEMETRY__PROTO__TRACE__V1__SPAN__SPAN_KIND__SPAN_KIND_INTERNAL
#define OTLP_KIND_SERVER   OPENTELEMETRY__PROTO__TRACE__V1__SPAN__SPAN_KIND__SPAN_KIND_SERVER
#define OTLP_KIND_CLIENT   OPENTELEMETRY__PROTO__TRACE__V1__SPAN__SPAN_KIND__SPAN_KIND_CLIENT
#define OTLP_KIND_PRODUCER OPENTELEMETRY__PROTO__TRACE__V1__SPAN__SPAN_KIND__SPAN_KIND_PRODUCER
#define OTLP_KIND_CONSUMER OPENTELEMETRY__PROTO__TRACE__V1__SPAN__SPAN_KIND__SPAN_KIND_CONSUMER

#define OTLP_ANY_STR OPENTELEMETRY__PROTO__COMMON__V1__ANY_VALUE__VALUE_STRING_VALUE
#define OTLP_ANY_INT OPENTELEMETRY__PROTO__COMMON__V1__ANY_VALUE__VALUE_INT_VALUE
#define OTLP_ANY_DBL OPENTELEMETRY__PROTO__COMMON__V1__ANY_VALUE__VALUE_DOUBLE_VALUE
#define OTLP_ANY_BOOL OPENTELEMETRY__PROTO__COMMON__V1__ANY_VALUE__VALUE_BOOL_VALUE

#define OTLP_STATUS_OK    OPENTELEMETRY__PROTO__TRACE__V1__STATUS__STATUS_CODE__STATUS_CODE_OK
#define OTLP_STATUS_ERROR OPENTELEMETRY__PROTO__TRACE__V1__STATUS__STATUS_CODE__STATUS_CODE_ERROR
#define OTLP_STATUS_UNSET OPENTELEMETRY__PROTO__TRACE__V1__STATUS__STATUS_CODE__STATUS_CODE_UNSET

/* gRPC length-prefixed message framing: 1-byte compression flag + 4-byte
 * big-endian length, immediately followed by the protobuf payload. */
struct grpc_hdr {
    uint8_t  compressed;
    uint32_t length;
} __attribute__((packed));

/* Per-thread context: a lock-free SPSC ring of finished spans.  The home thread
 * is the sole producer (head); otel_drain() is the sole consumer (tail). */
struct otel_thread_ctx {
    struct otel_span        *ring;       /* `capacity` slots */
    uint32_t                 capacity;   /* ring slots (power of two) */
    uint32_t                 mask;       /* capacity - 1 */
    _Atomic uint64_t         head;       /* producer cursor */
    _Atomic uint64_t         tail;       /* consumer cursor */
    _Atomic uint64_t         dropped;    /* ring-full drops on this thread */
    __uint128_t              randstate;  /* PCG-style id generator state */
    struct otel_thread_ctx  *next;       /* global registry list */
};

/* Pre-allocated protobuf-c batch, reused across flushes by the single drainer. */
struct otel_otlp {
    otlp_request_t   request;
    otlp_resource_t  resource;
    otlp_rspans_t    rspans;
    otlp_rspans_t   *rspansp;
    otlp_sspans_t    sspans;
    otlp_sspans_t   *sspansp;
    otlp_scope_t     scope;

    otlp_kv_t        res_attr[2];
    otlp_kv_t       *res_attrp[2];
    otlp_any_t       res_attr_val[2];

    otlp_span_t      spans[OTEL_BATCH_SPANS];
    otlp_span_t     *spansp[OTEL_BATCH_SPANS];
    otlp_status_t    status[OTEL_BATCH_SPANS];

    otlp_kv_t        attr[OTEL_BATCH_ATTRS];
    otlp_kv_t       *attrp[OTEL_BATCH_ATTRS];
    otlp_any_t       attr_val[OTEL_BATCH_ATTRS];

    otlp_event_t     event[OTEL_BATCH_EVENTS];
    otlp_event_t    *eventp[OTEL_BATCH_EVENTS];

    size_t           num_attrs;
    size_t           num_events;

    uint8_t         *buf;       /* gRPC frame hdr + packed protobuf */
    size_t           buf_size;
};

static struct {
    int                       initialized;
    char                      service[128];
    char                      hostname[128];

    otel_transport_fn         transport;
    void                     *transport_priv;

    /* Optional span sink (e.g. SQLite): fed raw finished spans during drain,
     * alongside or instead of the protobuf transport. */
    const struct otel_span_sink *sink;
    int                       sink_present;

    uint64_t                (*clock)(void);

    /* Head sampling: record every root trace when sample_always, else record
     * when otel_rand() < sample_threshold (== ratio * 2^64). */
    int                       sample_always;
    uint64_t                  sample_threshold;

    /* Runtime enable: otel_enabled_ == initialized && transport_present &&
     * runtime_on.  runtime_on is the otel_set_enabled() knob (default 1). */
    int                       transport_present;
    int                       runtime_on;

    pthread_rwlock_t          registry_lock;  /* guards the thread-ctx list */
    struct otel_thread_ctx   *threads;

    pthread_mutex_t           drain_lock;      /* serializes otel_drain + batch */
    struct otel_otlp          otlp;

    _Atomic uint64_t          dropped_spans;   /* aggregated, plus per-thread */

    /* TSC-backed wall clock (shared stopwatch subproject): wall-clock anchor
     * captured once at init, absolute time = anchor + elapsed ticks since.  Keeps
     * the per-span time path off clock_gettime; read-only after init. */
    struct stopwatch_context  sw_ctx;
    struct stopwatch          sw_base;
    uint64_t                  wall_base_ns;
} OT;

static __thread struct otel_thread_ctx *otel_tls;

/* Per-thread ring capacity used when a thread registers; tunable before any
 * registration via otel_set_ring_capacity().  Power of two. */
static unsigned int otel_ring_capacity = OTEL_RING_SIZE;

/* Live flag, read inline from the header hot path; set once a transport is
 * registered.  Exported so the inline wrappers in other TUs resolve it. */
SYMBOL_EXPORT int otel_enabled_ = 0;

/* ---- clock ---- */

static uint64_t
otel_default_clock(void)
{
    return OT.wall_base_ns + stopwatch_elapsed_ns(&OT.sw_ctx, &OT.sw_base);
}

SYMBOL_EXPORT void
otel_set_clock(uint64_t (*now_unix_ns)(void))
{
    OT.clock = now_unix_ns ? now_unix_ns : otel_default_clock;
}

SYMBOL_EXPORT void
otel_set_sampler(double ratio)
{
    if (ratio >= 1.0) {
        OT.sample_always    = 1;
        OT.sample_threshold = ~0ULL;
    } else if (ratio <= 0.0) {
        OT.sample_always    = 0;
        OT.sample_threshold = 0;
    } else {
        /* threshold = ratio * 2^64; record when otel_rand() < threshold. */
        OT.sample_always    = 0;
        OT.sample_threshold = (uint64_t) (ratio * 18446744073709551616.0);
    }
}

/* ---- id generation (PCG-style multiplicative, uuid-seeded per thread) ---- */

static inline uint64_t
otel_rand(struct otel_thread_ctx *ctx)
{
    ctx->randstate *= (__uint128_t) UINT64_C(0xda942042e4dd58b5);
    return (uint64_t) (ctx->randstate >> 64);
}

/* ---- process / thread lifecycle ---- */

SYMBOL_EXPORT int
otel_init(const char *service)
{
    if (OT.initialized) {
        return 0;
    }

    memset(&OT, 0, sizeof(OT));

    snprintf(OT.service, sizeof(OT.service), "%s", service ? service : "unknown");
    if (gethostname(OT.hostname, sizeof(OT.hostname)) != 0) {
        snprintf(OT.hostname, sizeof(OT.hostname), "unknown");
    }
    OT.hostname[sizeof(OT.hostname) - 1] = '\0';

    /* Anchor the TSC-backed wall clock: capture CLOCK_REALTIME once, then track
     * absolute time as that anchor plus monotonic elapsed ns from the stopwatch. */
    {
        struct timespec ts;
        stopwatch_context_init(&OT.sw_ctx);
        clock_gettime(CLOCK_REALTIME, &ts);
        OT.wall_base_ns = (uint64_t) ts.tv_sec * 1000000000ULL +
            (uint64_t) ts.tv_nsec;
        stopwatch_start(&OT.sw_ctx, &OT.sw_base);
    }

    OT.clock = otel_default_clock;

    /* Default: sample every trace.  Override with otel_set_sampler(). */
    OT.sample_always    = 1;
    OT.sample_threshold = ~0ULL;

    /* Enabled at runtime by default; goes live once a transport is registered. */
    OT.runtime_on       = 1;
    OT.transport_present = 0;
    otel_enabled_       = 0;

    pthread_rwlock_init(&OT.registry_lock, NULL);
    pthread_mutex_init(&OT.drain_lock, NULL);

    /* One-time init of the reused protobuf-c batch scaffolding. */
    struct otel_otlp *o = &OT.otlp;
    size_t i;

    opentelemetry__proto__collector__trace__v1__export_trace_service_request__init(&o->request);
    opentelemetry__proto__trace__v1__resource_spans__init(&o->rspans);
    opentelemetry__proto__trace__v1__scope_spans__init(&o->sspans);
    opentelemetry__proto__resource__v1__resource__init(&o->resource);
    opentelemetry__proto__common__v1__instrumentation_scope__init(&o->scope);

    o->scope.name    = OT.service;
    o->scope.version = "0.1.0";

    for (i = 0; i < 2; i++) {
        opentelemetry__proto__common__v1__key_value__init(&o->res_attr[i]);
        opentelemetry__proto__common__v1__any_value__init(&o->res_attr_val[i]);
        o->res_attr[i].value = &o->res_attr_val[i];
        o->res_attr_val[i].value_case = OTLP_ANY_STR;
        o->res_attrp[i] = &o->res_attr[i];
    }
    o->res_attr[0].key = "service.name";
    o->res_attr_val[0].string_value = OT.service;
    o->res_attr[1].key = "host.name";
    o->res_attr_val[1].string_value = OT.hostname;

    o->resource.n_attributes = 2;
    o->resource.attributes   = o->res_attrp;
    o->rspans.resource       = &o->resource;

    o->scope.name = OT.service;
    o->sspans.scope = &o->scope;

    o->rspans.n_scope_spans = 1;
    o->sspansp = &o->sspans;
    o->rspans.scope_spans = &o->sspansp;

    o->request.n_resource_spans = 1;
    o->rspansp = &o->rspans;
    o->request.resource_spans = &o->rspansp;

    o->sspans.spans = o->spansp;

    for (i = 0; i < OTEL_BATCH_SPANS; i++) {
        opentelemetry__proto__trace__v1__span__init(&o->spans[i]);
        opentelemetry__proto__trace__v1__status__init(&o->status[i]);
        o->spans[i].status = &o->status[i];
        o->spansp[i] = &o->spans[i];
    }
    for (i = 0; i < OTEL_BATCH_ATTRS; i++) {
        opentelemetry__proto__common__v1__key_value__init(&o->attr[i]);
        opentelemetry__proto__common__v1__any_value__init(&o->attr_val[i]);
        o->attr[i].value = &o->attr_val[i];
        o->attrp[i] = &o->attr[i];
    }
    for (i = 0; i < OTEL_BATCH_EVENTS; i++) {
        opentelemetry__proto__trace__v1__span__event__init(&o->event[i]);
        o->eventp[i] = &o->event[i];
    }

    o->buf_size = sizeof(struct grpc_hdr) + OTEL_BATCH_SPANS * OTEL_SPAN_SERIALIZATION;
    o->buf = malloc(o->buf_size);

    OT.initialized = 1;
    return o->buf ? 0 : -1;
}

/* Recompute the inline-visible live flag from its inputs.  Tracing is live when
 * a destination exists -- a transport or a span sink -- and it is enabled. */
static void
otel_recompute_enabled(void)
{
    otel_enabled_ = OT.initialized &&
        (OT.transport_present || OT.sink_present) &&
        OT.runtime_on;
}

SYMBOL_EXPORT void
otel_set_transport(
    otel_transport_fn fn,
    void             *priv)
{
    OT.transport         = fn;
    OT.transport_priv    = priv;
    OT.transport_present  = (fn != NULL);
    otel_recompute_enabled();
}

SYMBOL_EXPORT void
otel_set_span_sink(const struct otel_span_sink *sink)
{
    OT.sink         = sink;
    OT.sink_present = (sink != NULL);
    otel_recompute_enabled();
}

SYMBOL_EXPORT const char *
otel_service_name(void)
{
    return OT.service;
}

SYMBOL_EXPORT void
otel_set_ring_capacity(unsigned int spans)
{
    /* Round up to a power of two (mask arithmetic requires it); clamp to a sane
     * minimum.  Threads registered after this use the new capacity. */
    unsigned int cap = 1;

    if (spans < 2) {
        spans = 2;
    }
    while (cap < spans) {
        cap <<= 1;
    }
    otel_ring_capacity = cap;
}

SYMBOL_EXPORT void
otel_set_enabled(int enabled)
{
    OT.runtime_on = enabled ? 1 : 0;
    otel_recompute_enabled();
}

SYMBOL_EXPORT void
otel_thread_register(void)
{
    struct otel_thread_ctx *ctx;

    if (otel_tls) {
        return;
    }

    ctx = calloc(1, sizeof(*ctx));
    ctx->capacity = otel_ring_capacity;
    ctx->mask     = ctx->capacity - 1;
    ctx->ring     = calloc(ctx->capacity, sizeof(struct otel_span));
    atomic_store_explicit(&ctx->head, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->tail, 0, memory_order_relaxed);

    /* Seed the per-thread id generator with a high-quality random value. */
    uuid_generate((unsigned char *) &ctx->randstate);
    if (ctx->randstate == 0) {
        ctx->randstate = (__uint128_t) (uintptr_t) ctx ^ OT.clock();
    }

    pthread_rwlock_wrlock(&OT.registry_lock);
    ctx->next   = OT.threads;
    OT.threads  = ctx;
    pthread_rwlock_unlock(&OT.registry_lock);

    otel_tls = ctx;
}

/* Drain a single context's ring into the batch / transport.  Caller holds
 * OT.drain_lock.  Defined below otlp_add_span. */
static int  otel_drain_ctx(struct otel_thread_ctx *ctx);
static void otlp_flush(void);

SYMBOL_EXPORT void
otel_thread_unregister(void)
{
    struct otel_thread_ctx *ctx = otel_tls;
    struct otel_thread_ctx **pp;

    if (!ctx) {
        return;
    }

    /* Take the write lock (excludes any concurrent drain) so we can act as the
     * sole consumer for this ring one last time, unlink, and free. */
    pthread_rwlock_wrlock(&OT.registry_lock);

    if (otel_enabled_) {
        pthread_mutex_lock(&OT.drain_lock);
        if (OT.sink_present && OT.sink->begin) {
            OT.sink->begin(OT.sink->priv);
        }
        otel_drain_ctx(ctx);
        otlp_flush();
        if (OT.sink_present && OT.sink->end) {
            OT.sink->end(OT.sink->priv);
        }
        pthread_mutex_unlock(&OT.drain_lock);
    }

    for (pp = &OT.threads; *pp; pp = &(*pp)->next) {
        if (*pp == ctx) {
            *pp = ctx->next;
            break;
        }
    }
    pthread_rwlock_unlock(&OT.registry_lock);

    free(ctx->ring);
    free(ctx);
    otel_tls = NULL;
}

SYMBOL_EXPORT void
otel_shutdown(void)
{
    if (!OT.initialized) {
        return;
    }

    if (otel_enabled_) {
        otel_drain();
    }

    /* Threads are expected to unregister themselves; free any stragglers. */
    pthread_rwlock_wrlock(&OT.registry_lock);
    while (OT.threads) {
        struct otel_thread_ctx *ctx = OT.threads;
        OT.threads = ctx->next;
        free(ctx->ring);
        free(ctx);
    }
    pthread_rwlock_unlock(&OT.registry_lock);

    free(OT.otlp.buf);
    pthread_rwlock_destroy(&OT.registry_lock);
    pthread_mutex_destroy(&OT.drain_lock);

    OT.initialized = 0;
    otel_enabled_  = 0;
}

/* ---- span hot path: slow paths only ----
 *
 * The inline wrappers and the unsampled fast path live in oteltracing.h; these
 * out-of-line functions run only when a span is actually being recorded (or to
 * decide sampling for a root span). */

static inline void
otel_span_init(
    struct otel_span *s,
    const char       *name,
    uint8_t           kind)
{
    s->kind           = (uint8_t) kind;
    s->status         = OTEL_STATUS_UNSET;
    s->status_message = (uint16_t) OTEL_NIL;
    s->num_attrs      = 0;
    s->num_events     = 0;
    s->dropped_attrs  = 0;
    s->dropped_events = 0;
    s->end_unix_ns    = 0;
    s->start_unix_ns  = OT.clock();
    s->flags          = OTEL_FLAG_RECORDING;
    /* Reset the arena (empty attr/event lists) and copy the name in. */
    s->arena_used     = 0;
    s->attr_head      = (uint16_t) OTEL_NIL;
    s->attr_tail      = (uint16_t) OTEL_NIL;
    s->event_head     = (uint16_t) OTEL_NIL;
    s->event_tail     = (uint16_t) OTEL_NIL;
    s->name           = otel_arena_put(s, name);
}

SYMBOL_EXPORT void
otel_span_start_root_(
    struct otel_span *s,
    const char       *name,
    uint8_t           kind)
{
    struct otel_thread_ctx *ctx = otel_tls;

    if (!ctx) {
        s->flags = 0;
        return;
    }

    /* Head sampling: record every trace when sample_always is set, otherwise
     * draw the per-thread PRNG once and compare against the configured
     * threshold (threshold = ratio * 2^64). */
    if (!OT.sample_always && otel_rand(ctx) >= OT.sample_threshold) {
        s->flags = 0;
        return;
    }

    *(uint64_t *) &s->trace_id[0] = otel_rand(ctx);
    *(uint64_t *) &s->trace_id[8] = otel_rand(ctx);
    s->span_id   = otel_rand(ctx);
    s->parent_id = 0;
    otel_span_init(s, name, kind);
}

SYMBOL_EXPORT void
otel_span_start_child_(
    struct otel_span       *s,
    const char             *name,
    uint8_t                 kind,
    const struct otel_span *parent)
{
    struct otel_thread_ctx *ctx = otel_tls;

    if (!ctx) {
        s->flags = 0;
        return;
    }

    memcpy(s->trace_id, parent->trace_id, 16);
    s->span_id   = otel_rand(ctx);
    s->parent_id = parent->span_id;
    otel_span_init(s, name, kind);
}

SYMBOL_EXPORT void
otel_span_start_remote_(
    struct otel_span *s,
    const char       *name,
    uint8_t           kind,
    const uint8_t     trace_id[16],
    uint64_t          parent_id)
{
    struct otel_thread_ctx *ctx = otel_tls;

    if (!ctx) {
        s->flags = 0;
        return;
    }

    memcpy(s->trace_id, trace_id, 16);
    s->span_id   = otel_rand(ctx);
    s->parent_id = parent_id;
    otel_span_init(s, name, kind);
}

SYMBOL_EXPORT struct otel_event *
otel_span_event_(
    struct otel_span *s,
    const char       *name)
{
    struct otel_event *e;
    uint16_t           noff, roff;

    noff = otel_arena_put(s, name);
    roff = otel_arena_alloc(s, sizeof(struct otel_event));
    if (roff == (uint16_t) OTEL_NIL) {
        if (s->dropped_events < 255) {
            s->dropped_events++;   /* arena full: drop the event */
        }
        return (struct otel_event *) 0;
    }

    e                = (struct otel_event *) &s->arena[roff];
    e->next          = (uint16_t) OTEL_NIL;
    e->name          = noff;
    e->attr_head     = (uint16_t) OTEL_NIL;
    e->attr_tail     = (uint16_t) OTEL_NIL;
    e->num_attrs     = 0;
    e->dropped_attrs = 0;
    e->time_unix_ns  = OT.clock();

    if (s->event_tail == (uint16_t) OTEL_NIL) {
        s->event_head = roff;
    } else {
        ((struct otel_event *) &s->arena[s->event_tail])->next = roff;
    }
    s->event_tail = roff;
    s->num_events++;
    return e;
}

SYMBOL_EXPORT void
otel_span_end_(struct otel_span *s)
{
    struct otel_thread_ctx *ctx = otel_tls;
    struct otel_span       *slot;
    uint64_t                head, tail;

    s->end_unix_ns = OT.clock();

    if (!ctx) {
        atomic_fetch_add_explicit(&OT.dropped_spans, 1, memory_order_relaxed);
        return;
    }

    /* SPSC enqueue: we are the sole producer. */
    head = atomic_load_explicit(&ctx->head, memory_order_relaxed);
    tail = atomic_load_explicit(&ctx->tail, memory_order_acquire);

    if (head - tail >= ctx->capacity) {
        atomic_fetch_add_explicit(&ctx->dropped, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&OT.dropped_spans, 1, memory_order_relaxed);
        return;
    }

    /* The span is position-independent: everything variable-length lives in its
     * arena addressed by offsets, so a plain value copy is fully self-contained --
     * no pointer rebasing needed. */
    slot  = &ctx->ring[head & ctx->mask];
    *slot = *s;
    atomic_store_explicit(&ctx->head, head + 1, memory_order_release);
}

/* ---- drain / OTLP encoding (single consumer) ---- */

static Opentelemetry__Proto__Trace__V1__Span__SpanKind
otlp_kind(uint8_t kind)
{
    switch (kind) {
        case OTEL_SPAN_SERVER:   return OTLP_KIND_SERVER;
        case OTEL_SPAN_CLIENT:   return OTLP_KIND_CLIENT;
        case OTEL_SPAN_PRODUCER: return OTLP_KIND_PRODUCER;
        case OTEL_SPAN_CONSUMER: return OTLP_KIND_CONSUMER;
        default:                 return OTLP_KIND_INTERNAL;
    } /* switch */
} /* otlp_kind */

/* Encode one finished attr into the next free slot of the shared KeyValue pool
 * (o->attrp[o->num_attrs]) and advance the cursor.  Used for both span-level and
 * event-level attributes, which draw from the same pool. */
static void
otlp_add_attr(
    struct otel_otlp       *o,
    const struct otel_span *s,
    const struct otel_attr *a)
{
    otlp_kv_t  *kv = o->attrp[o->num_attrs];
    otlp_any_t *av = kv->value;

    kv->key = (char *) otel_attr_key(s, a);
    switch (a->type) {
        case OTEL_ATTR_STR:
            av->value_case   = OTLP_ANY_STR;
            av->string_value = (char *) otel_attr_strval(s, a);
            break;
        case OTEL_ATTR_I64:
            av->value_case = OTLP_ANY_INT;
            av->int_value  = a->v.i;
            break;
        case OTEL_ATTR_U64:
            av->value_case = OTLP_ANY_INT;
            av->int_value  = (int64_t) a->v.u;
            break;
        case OTEL_ATTR_F64:
            av->value_case   = OTLP_ANY_DBL;
            av->double_value = a->v.d;
            break;
        case OTEL_ATTR_BOOL:
            av->value_case = OTLP_ANY_BOOL;
            av->bool_value = a->v.b;
            break;
        default:
            av->value_case   = OTLP_ANY_STR;
            av->string_value = "";
            break;
    } /* switch */
    o->num_attrs++;
} /* otlp_add_attr */

/* Map one finished span into the protobuf-c batch, flushing first if full. */
static void
otlp_add_span(const struct otel_span *s)
{
    struct otel_otlp        *o = &OT.otlp;
    otlp_span_t             *span;
    const struct otel_attr  *sa;
    const struct otel_event *se;
    size_t                   event_attrs = 0;

    /* Event attributes draw from the same KeyValue pool as span attributes, so
     * the flush decision must account for them too. */
    for (se = otel_event_first(s); se; se = otel_event_next(s, se)) {
        event_attrs += se->num_attrs;
    }

    if (o->sspans.n_spans == OTEL_BATCH_SPANS ||
        o->num_attrs + s->num_attrs + event_attrs > OTEL_BATCH_ATTRS ||
        o->num_events + s->num_events > OTEL_BATCH_EVENTS) {
        otlp_flush();
    }

    span = &o->spans[o->sspans.n_spans];

    /* IDs.  The id bytes are opaque to the collector; we ship them raw. */
    span->trace_id.data = (uint8_t *) s->trace_id;
    span->trace_id.len  = 16;
    span->span_id.data  = (uint8_t *) &s->span_id;
    span->span_id.len   = 8;
    if (s->parent_id) {
        span->parent_span_id.data = (uint8_t *) &s->parent_id;
        span->parent_span_id.len  = 8;
    } else {
        span->parent_span_id.data = NULL;
        span->parent_span_id.len  = 0;
    }

    span->name                = (char *) otel_span_name(s);
    span->kind                = otlp_kind(s->kind);
    span->start_time_unix_nano = s->start_unix_ns;
    span->end_time_unix_nano   = s->end_unix_ns;

    span->status->code    = (s->status == OTEL_STATUS_ERROR) ? OTLP_STATUS_ERROR :
                            (s->status == OTEL_STATUS_OK)    ? OTLP_STATUS_OK :
                            OTLP_STATUS_UNSET;
    span->status->message = (char *) otel_span_status_message(s);

    span->dropped_attributes_count = s->dropped_attrs;
    span->dropped_events_count     = s->dropped_events;

    /* Attributes: walk the arena list, carving a slice from the shared attr pool. */
    span->attributes   = &o->attrp[o->num_attrs];
    span->n_attributes = 0;
    for (sa = otel_attr_first(s); sa; sa = otel_attr_next(s, sa)) {
        otlp_add_attr(o, s, sa);
        span->n_attributes++;
    }

    /* Events: walk the arena list, carving a slice from the shared event pool;
     * each event's own attributes carve from the same KeyValue pool. */
    span->events   = &o->eventp[o->num_events];
    span->n_events = 0;
    for (se = otel_event_first(s); se; se = otel_event_next(s, se)) {
        otlp_event_t           *ev = o->eventp[o->num_events];
        const struct otel_attr *ea;

        ev->time_unix_nano = se->time_unix_ns;
        ev->name           = (char *) otel_event_name(s, se);

        ev->attributes   = &o->attrp[o->num_attrs];
        ev->n_attributes = 0;
        for (ea = otel_event_attr_first(s, se); ea; ea = otel_attr_next(s, ea)) {
            otlp_add_attr(o, s, ea);
            ev->n_attributes++;
        }
        ev->dropped_attributes_count = se->dropped_attrs;

        o->num_events++;
        span->n_events++;
    }

    o->sspans.n_spans++;
} /* otlp_add_span */

static void
otlp_flush(void)
{
    struct otel_otlp *o = &OT.otlp;
    struct grpc_hdr  *hdr;
    size_t            len;

    if (o->sspans.n_spans == 0) {
        return;
    }

    hdr = (struct grpc_hdr *) o->buf;
    len = opentelemetry__proto__collector__trace__v1__export_trace_service_request__pack(
        &o->request, o->buf + sizeof(*hdr));

    hdr->compressed = 0;
    hdr->length     = htonl((uint32_t) len);

    if (OT.transport) {
        OT.transport(o->buf, sizeof(*hdr) + len, OT.transport_priv);
    }

    /* Reset batch cursors for the next fill. */
    o->sspans.n_spans = 0;
    o->num_attrs      = 0;
    o->num_events     = 0;
} /* otlp_flush */

/* Emit one finished span to every registered consumer: the protobuf transport
 * batch (if a transport is set) and the raw-span sink (if one is set). */
static void
otel_emit_span(const struct otel_span *s)
{
    if (OT.transport_present) {
        otlp_add_span(s);
    }
    if (OT.sink_present && OT.sink->span) {
        OT.sink->span(s, OT.sink->priv);
    }
}

static int
otel_drain_ctx(struct otel_thread_ctx *ctx)
{
    uint64_t tail, head;
    int      n = 0;

    /* SPSC dequeue: we are the sole consumer. */
    tail = atomic_load_explicit(&ctx->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ctx->head, memory_order_acquire);

    for (; tail != head; tail++) {
        otel_emit_span(&ctx->ring[tail & ctx->mask]);
        n++;
    }

    atomic_store_explicit(&ctx->tail, tail, memory_order_release);
    return n;
}

SYMBOL_EXPORT int
otel_drain(void)
{
    struct otel_thread_ctx *ctx;
    int                     n = 0;

    if (!otel_enabled_) {
        return 0;
    }

    pthread_mutex_lock(&OT.drain_lock);
    pthread_rwlock_rdlock(&OT.registry_lock);

    if (OT.sink_present && OT.sink->begin) {
        OT.sink->begin(OT.sink->priv);
    }

    for (ctx = OT.threads; ctx; ctx = ctx->next) {
        n += otel_drain_ctx(ctx);
    }

    otlp_flush();

    if (OT.sink_present && OT.sink->end) {
        OT.sink->end(OT.sink->priv);
    }

    pthread_rwlock_unlock(&OT.registry_lock);
    pthread_mutex_unlock(&OT.drain_lock);
    return n;
}

SYMBOL_EXPORT uint64_t
otel_dropped_spans(void)
{
    return atomic_load_explicit(&OT.dropped_spans, memory_order_relaxed);
}
