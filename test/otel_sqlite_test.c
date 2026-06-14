// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * SQLite sink round-trip test: open a temp-file span store, emit a parent + child
 * span with attributes, an event, and an error status, let the sink persist them,
 * then reopen the database read-only and assert the rows, the parent/child
 * linkage, the attributes/event, and the status round-trip correctly.  No network
 * or collector is involved, so this runs in CI.
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "oteltracing.h"
#include "otel_sqlite.h"

static int g_failures;

#define CHECK(cond) do {                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
} while (0)

static sqlite3_int64
scalar_i64(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    sqlite3_int64 v = -1;

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            v = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    return v;
}

static char *
scalar_text(sqlite3 *db, const char *sql, char *out, size_t outlen)
{
    sqlite3_stmt *st;

    out[0] = '\0';
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            snprintf(out, outlen, "%s", t ? (const char *) t : "");
        }
        sqlite3_finalize(st);
    }
    return out;
}

int
main(void)
{
    char             path[] = "/tmp/otel_sqlite_test_XXXXXX";
    int              fd;
    sqlite3         *db;
    struct otel_span parent, child;
    char             buf[128];
    char             buf2[128];

    fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);
    unlink(path);   /* sqlite_open will recreate it */

    otel_init("sqlite-test");
    /* Long flush interval so we control timing via close()'s final drain. */
    if (otel_sqlite_open(path, 60000) != 0) {
        fprintf(stderr, "otel_sqlite_open failed\n");
        return 1;
    }
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

    otel_thread_unregister();
    otel_sqlite_close();   /* stops flusher, final drain, commit, close */
    otel_shutdown();

    /* ---- verify ---- */
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "reopen failed\n");
        unlink(path);
        return 1;
    }

    CHECK(scalar_i64(db, "SELECT COUNT(*) FROM spans") == 2);
    CHECK(scalar_i64(db,
        "SELECT COUNT(*) FROM spans WHERE parent_id IS NULL") == 1);  /* root */
    CHECK(scalar_i64(db, "SELECT service='sqlite-test' FROM spans LIMIT 1") == 1);

    /* Shared trace id between parent and child. */
    CHECK(scalar_i64(db,
        "SELECT COUNT(DISTINCT trace_id) FROM spans") == 1);

    /* Child links to parent: child.parent_id == parent.span_id. */
    scalar_text(db,
        "SELECT span_id FROM spans WHERE name='parent-op'", buf, sizeof(buf));
    scalar_text(db,
        "SELECT parent_id FROM spans WHERE name='child-op'", buf2, sizeof(buf2));
    CHECK(strcmp(buf, buf2) == 0 && buf[0] != '\0');

    /* Parent attributes (2) + event (1). */
    CHECK(scalar_i64(db,
        "SELECT COUNT(*) FROM span_attrs a JOIN spans s ON a.span=s.id "
        "WHERE s.name='parent-op'") == 2);
    CHECK(scalar_i64(db,
        "SELECT COUNT(*) FROM span_events e JOIN spans s ON e.span=s.id "
        "WHERE s.name='parent-op'") == 1);
    scalar_text(db,
        "SELECT e.name FROM span_events e JOIN spans s ON e.span=s.id "
        "WHERE s.name='parent-op'", buf, sizeof(buf));
    CHECK(strcmp(buf, "received") == 0);

    /* String attr value round-trips. */
    scalar_text(db,
        "SELECT s_val FROM span_attrs a JOIN spans s ON a.span=s.id "
        "WHERE s.name='parent-op' AND a.key='peer'", buf, sizeof(buf));
    CHECK(strcmp(buf, "1.2.3.4") == 0);

    /* Child status + message. */
    CHECK(scalar_i64(db,
        "SELECT status_code FROM spans WHERE name='child-op'") == 2);
    scalar_text(db,
        "SELECT status_message FROM spans WHERE name='child-op'",
        buf, sizeof(buf));
    CHECK(strcmp(buf, "boom") == 0);

    /* Durations are non-negative and timestamps populated. */
    CHECK(scalar_i64(db,
        "SELECT COUNT(*) FROM spans WHERE duration_ns >= 0 "
        "AND start_unix_ns > 0 AND end_unix_ns >= start_unix_ns") == 2);

    sqlite3_close(db);
    unlink(path);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("otel_sqlite_test: OK\n");
    return 0;
} /* main */
