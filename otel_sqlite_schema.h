// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * Shared SQLite schema for the oteltracing-c local span store.  Both the writer
 * (otel_sqlite.c) and the reader (tools/otel-trace.c) include this so the table
 * layout cannot drift between them.
 *
 * trace_id and span_id are stored as lowercase hex TEXT (32 and 16 chars): this
 * matches the OTLP / Jaeger UI convention and avoids the unsigned-64 -> SQLite
 * signed-INTEGER representation problem.  Each span row is denormalized with its
 * service and a precomputed duration_ns so the CLI can do slow-span / per-service
 * queries with simple indexed scans.
 */

#ifndef OTEL_SQLITE_SCHEMA_H
#define OTEL_SQLITE_SCHEMA_H

/* Bump when the table layout changes; stored in PRAGMA user_version. */
#define OTEL_SQLITE_SCHEMA_VERSION 2

/* Attribute value types stored in span_attrs.type (mirror enum otel_attr_type).
 * The CLI uses these to pick which value column to read. */

__attribute__((unused))
static const char OTEL_SQLITE_SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS spans ("
    "  id              INTEGER PRIMARY KEY,"
    "  trace_id        TEXT NOT NULL,"
    "  span_id         TEXT NOT NULL,"
    "  parent_id       TEXT,"
    "  service         TEXT,"
    "  name            TEXT,"
    "  kind            INTEGER,"
    "  status_code     INTEGER,"
    "  status_message  TEXT,"
    "  start_unix_ns   INTEGER,"
    "  end_unix_ns     INTEGER,"
    "  duration_ns     INTEGER,"
    "  dropped_attrs   INTEGER,"
    "  dropped_events  INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS span_attrs ("
    "  span    INTEGER NOT NULL,"
    "  key     TEXT,"
    "  type    INTEGER,"
    "  s_val   TEXT,"
    "  i_val   INTEGER,"
    "  d_val   REAL"
    ");"
    "CREATE TABLE IF NOT EXISTS span_events ("
    "  id           INTEGER PRIMARY KEY,"
    "  span         INTEGER NOT NULL,"
    "  name         TEXT,"
    "  time_unix_ns INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS span_event_attrs ("
    "  event   INTEGER NOT NULL,"
    "  key     TEXT,"
    "  type    INTEGER,"
    "  s_val   TEXT,"
    "  i_val   INTEGER,"
    "  d_val   REAL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_spans_trace    ON spans(trace_id);"
    "CREATE INDEX IF NOT EXISTS idx_spans_start    ON spans(start_unix_ns);"
    "CREATE INDEX IF NOT EXISTS idx_spans_name     ON spans(name);"
    "CREATE INDEX IF NOT EXISTS idx_spans_duration ON spans(duration_ns);"
    "CREATE INDEX IF NOT EXISTS idx_attrs_span     ON span_attrs(span);"
    "CREATE INDEX IF NOT EXISTS idx_events_span    ON span_events(span);"
    "CREATE INDEX IF NOT EXISTS idx_event_attrs_event ON span_event_attrs(event);";

#endif /* OTEL_SQLITE_SCHEMA_H */
