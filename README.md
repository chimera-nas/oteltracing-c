<!--
SPDX-FileCopyrightText: 2026 Ben Jarvis
SPDX-License-Identifier: MIT
-->

# oteltracing-c

A lightweight OpenTelemetry **span** tracer for C, designed for tracing
operations that take *microseconds*, not milliseconds.

It emits spans in **OTLP/gRPC** format (protobuf, the same wire format the
OpenTelemetry Collector speaks), but it is **transport-agnostic**: the library
encodes finished spans into ready-to-send gRPC-framed buffers and hands them to
a callback you provide. You decide how they reach a collector — over an HTTP/2
client, curl, a file, a socket, anything. The library has no networking
dependency of its own.

## Why another tracer?

Most tracing libraries assume millisecond-scale work and freely allocate and
lock per span. This one is built for hot paths:

- **No heap allocation on the span hot path.** A span is an embeddable POD
  (`struct otel_span`) you place inside your own request/operation struct.
  Attributes and events are stored *inline* in that struct.
- **No locks on the hot path.** Finished spans are published into a per-thread
  lock-free SPSC ring; a single consumer drains them.
- **Cheap timestamps.** Time comes from a TSC-backed clock (the shared
  [`stopwatch`](https://github.com/chimera-nas/stopwatch) subproject) — an
  `rdtsc` plus arithmetic, no `clock_gettime` per span. Overridable.

## Threading model

- A span **starts and ends on the same thread** (its "home" thread).
- While in progress, the owning request may be handed to another thread, which
  may add attributes/events and hand it back. This is a **single-owner baton**
  (only one thread touches a span at a time); your handoff mechanism provides
  the memory barrier. No locks are needed because all mutable span data lives
  inline in the struct.
- Only threads that **start or end** spans need `otel_thread_register()`. A
  thread that merely annotates a handed-off span does not.
- `otel_drain()` is the **single consumer** — call it from one thread (e.g. a
  dedicated exporter thread or an event-loop hook).

## Building

Dependencies:

- `protobuf-c` + `protoc-c` (Debian/Ubuntu: `libprotobuf-c-dev`,
  `protobuf-c-compiler`) — used to generate the OTLP bindings.
- `libuuid` (`uuid-dev`) — for seeding trace/span IDs.
- `pthread`.

The [`stopwatch`](https://github.com/chimera-nas/stopwatch) submodule is vendored
under `ext/stopwatch`, so clone recursively:

```sh
git clone --recursive https://github.com/chimera-nas/oteltracing-c.git
# or, in an existing checkout:
git submodule update --init --recursive
```

Build with CMake:

```sh
cmake -S . -B build -GNinja
ninja -C build
ctest --test-dir build      # decode-verify test, no network required
```

This produces `liboteltracing-c.so`; link against it and `#include "oteltracing.h"`.

## Usage

### 1. Initialize, register a transport, register your threads

```c
#include "oteltracing.h"

/* Your transport: buf/len is a complete gRPC-framed OTLP
 * ExportTraceServiceRequest, ready to POST to
 *   <collector>/opentelemetry.proto.collector.trace.v1.TraceService/Export
 * with content-type: application/grpc. The buffer is only valid for the
 * duration of the call — copy it if you send asynchronously. */
static void my_transport(const void *buf, size_t len, void *priv)
{
    http_post_grpc(priv, buf, len);   /* your HTTP/2 client */
}

otel_init("my-service");              /* sets the OTLP service.name */
otel_set_transport(my_transport, my_http_client);

/* On every thread that will start/end spans: */
otel_thread_register();
```

Spans are only recorded once a transport is registered, so register it before
producing spans.

### 2. Create spans and annotate them

```c
struct otel_span span;                /* embed this in your request struct */

otel_span_start(&span, "handle-request", OTEL_SPAN_SERVER);
otel_span_attr_str(&span, "peer", "10.0.0.7");
otel_span_attr_u64(&span, "bytes", 4096);
otel_span_event(&span, "parsed-header");

/* ... do the work ... */

otel_span_end(&span);                 /* on the home thread */
```

Child spans inherit the parent's trace ID and link to it:

```c
struct otel_span child;
otel_span_start_child(&child, "vfs-read", OTEL_SPAN_INTERNAL, &parent);
/* ... */
otel_span_end(&child);
```

To continue a trace started elsewhere (distributed propagation), seed from a
remote context. `sampled` carries the upstream decision (e.g. the W3C
`traceparent` sampled flag):

```c
otel_span_start_remote(&span, "rpc", OTEL_SPAN_SERVER, trace_id /*16 bytes*/,
                       parent_span_id, sampled /* 0 or 1 */);
```

Record an error status:

```c
otel_span_set_status(&span, OTEL_STATUS_ERROR, "permission denied");
```

### 3. Drain (export) periodically

`otel_drain()` collects finished spans from all registered threads, encodes them
into OTLP batches, and pushes each through your transport. Call it from a single
thread on whatever cadence suits you:

```c
for (;;) {
    do_event_loop_iteration();
    otel_drain();                     /* or call from a loop/timer hook */
}
```

### Sampling

By default every trace is recorded. To sample probabilistically, set a ratio in
`[0,1]` — the chance that a new (root) trace is recorded:

```c
otel_set_sampler(0.01);   /* record ~1 in 100 traces */
```

The decision is made **once**, with a fast per-thread PRNG, when a root span
starts, and is inherited by the whole trace — every child span sees the same
recording bit. For traces that aren't sampled, the inline `otel_span_*` calls
in the header compile down to a flag test and an immediate return: no library
call, no id generation, no allocation. So you can leave instrumentation calls in
hot paths and pay almost nothing when a trace isn't being recorded. Set the
ratio once at startup (it's configuration, not a per-request control).

### 4. Shut down

```c
otel_thread_unregister();             /* on each registered thread */
otel_shutdown();                      /* drains remaining spans, frees state */
```

## API reference

| Function | Purpose |
| --- | --- |
| `otel_init(service)` | Initialize; set OTLP `service.name`. |
| `otel_shutdown()` | Drain and tear down. |
| `otel_thread_register()` / `otel_thread_unregister()` | Per-thread span staging (for threads that start/end spans). |
| `otel_set_transport(fn, priv)` | Register the callback that ships gRPC-framed OTLP buffers. |
| `otel_set_sampler(ratio)` | Probability `[0,1]` that a new trace is recorded (default 1.0). |
| `otel_set_clock(now_unix_ns)` | Override the wall-clock source (returns ns since the Unix epoch). |
| `otel_drain()` | Encode and export finished spans (single consumer). |
| `otel_dropped_spans()` | Count of spans dropped due to ring overflow. |
| `otel_span_start` / `_start_child` / `_start_remote` | Begin a span. |
| `otel_span_attr_str` / `_i64` / `_u64` / `_bool` | Add an attribute (safe mid-flight). |
| `otel_span_event(s, name)` | Add a timestamped event (safe mid-flight). |
| `otel_span_set_status(s, status, msg)` | Set span status. |
| `otel_span_end(s)` | End and stage the span (home thread). |
| `otel_span_recording(s)` | Whether the span is being recorded. |

Inline capacity per span is bounded by `OTEL_SPAN_MAX_ATTRS` and
`OTEL_SPAN_MAX_EVENTS`; overflow is dropped and counted on the span.

## License

MIT — see [LICENSE](LICENSE). The vendored OpenTelemetry protocol definitions
under `opentelemetry/proto/` are Copyright OpenTelemetry Authors and licensed
under Apache-2.0 (retained in their headers).
