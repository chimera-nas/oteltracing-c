// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * Optional SQLite span sink for oteltracing-c.
 *
 * This is a turnkey local-debug backend: instead of (or in addition to) shipping
 * spans to an OTLP collector over gRPC, persist them to a SQLite database that
 * can be queried offline with the bundled `otel-trace` CLI.
 *
 * otel_sqlite_open() opens the database, applies write-optimized PRAGMAs, creates
 * the schema (see otel_sqlite_schema.h), registers a span sink with the core
 * tracer, and spawns a single dedicated flusher thread that periodically calls
 * otel_drain().  All SQLite access happens only on that flusher thread, so it is
 * never on a span's hot path: span production only ever enqueues into a per-thread
 * lock-free ring, and if the flusher falls behind, the ring fills and spans are
 * dropped + counted rather than blocking the producer.  Tune the backlog before
 * registering threads with otel_set_ring_capacity().
 *
 * Call otel_init() before otel_sqlite_open().
 */

#ifndef OTEL_SQLITE_H
#define OTEL_SQLITE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open `path` as the span store and start the flusher thread.  `flush_interval_ms`
 * is the drain cadence (0 selects a sensible default).  Each drain is written as
 * one large transaction.  Returns 0 on success, -1 on failure.
 */
int otel_sqlite_open(const char *path, unsigned int flush_interval_ms);

/*
 * Optional retention cap: keep at most ~`max_spans` span rows, pruning the oldest
 * traces as new ones arrive (0, the default, disables pruning).  Primary volume
 * control remains head sampling (otel_set_sampler).
 */
void otel_sqlite_set_max_spans(unsigned long max_spans);

/*
 * Stop the flusher thread, drain and commit any remaining spans, detach the sink,
 * and close the database.  Safe to call even if otel_sqlite_open() failed.
 */
void otel_sqlite_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OTEL_SQLITE_H */
