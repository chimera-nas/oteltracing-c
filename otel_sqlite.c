// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * SQLite span sink for oteltracing-c.  See otel_sqlite.h for the model.
 *
 * Design notes:
 *   - All SQLite access happens on one dedicated flusher thread (the drain
 *     consumer).  The span hot path never touches SQLite; it only enqueues into
 *     the core's per-thread lock-free rings, which drop + count on overflow.  So
 *     a slow database degrades to dropped spans, never to blocked producers.
 *   - Writes are batched: the sink's begin()/end() bracket each drain in a single
 *     transaction, so one drain == one commit regardless of span count.
 *   - PRAGMAs are tuned for a single bulk writer with concurrent CLI readers
 *     (WAL, NORMAL sync, manual checkpointing).
 */

#define _GNU_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <sqlite3.h>

#include "oteltracing.h"
#include "otel_sqlite.h"
#include "otel_sqlite_schema.h"

#ifndef SYMBOL_EXPORT
#define SYMBOL_EXPORT __attribute__((visibility("default")))
#endif

/* Default drain cadence if the caller passes 0. */
#define OTEL_SQLITE_DEFAULT_INTERVAL_MS 250

/* Run a WAL checkpoint / retention prune every this many commits. */
#define OTEL_SQLITE_MAINT_EVERY 64

static struct {
    sqlite3      *db;

    sqlite3_stmt *ins_span;
    sqlite3_stmt *ins_attr;
    sqlite3_stmt *ins_event;

    sqlite3_int64 cur_span;        /* rowid of the span being inserted */
    int           in_txn;          /* a transaction is open (begin succeeded) */
    unsigned long commits;         /* commits since open, drives maintenance */
    unsigned long max_spans;       /* retention cap, 0 = unlimited */

    /* Flusher thread. */
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             running;
    int             have_thread;
    unsigned int    interval_ms;

    struct otel_span_sink sink;
} S;

/* ---- helpers ---- */

static void
otel_sqlite_hex(
    char          *out,    /* needs 2*len + 1 bytes */
    const void    *in,
    size_t         len)
{
    static const char hexd[] = "0123456789abcdef";
    const uint8_t    *b = in;
    size_t            i;

    for (i = 0; i < len; i++) {
        out[i * 2]     = hexd[b[i] >> 4];
        out[i * 2 + 1] = hexd[b[i] & 0xf];
    }
    out[len * 2] = '\0';
}

static int
otel_sqlite_exec(const char *sql)
{
    char *err = NULL;

    if (sqlite3_exec(S.db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "otel_sqlite: exec failed: %s (%s)\n",
                err ? err : "?", sql);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* ---- sink callbacks (run on the flusher thread, inside otel_drain) ---- */

static void
otel_sqlite_begin(void *priv)
{
    (void) priv;
    S.in_txn = (otel_sqlite_exec("BEGIN") == 0);
}

static void
otel_sqlite_span(
    const struct otel_span *s,
    void                   *priv)
{
    char  trace_hex[33];
    char  span_hex[17];
    char  parent_hex[17];
    sqlite3_stmt *st;
    int   i;

    (void) priv;

    if (!S.in_txn) {
        return;
    }

    otel_sqlite_hex(trace_hex, s->trace_id, 16);
    otel_sqlite_hex(span_hex, &s->span_id, 8);

    st = S.ins_span;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, trace_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, span_hex, -1, SQLITE_TRANSIENT);
    if (s->parent_id) {
        otel_sqlite_hex(parent_hex, &s->parent_id, 8);
        sqlite3_bind_text(st, 3, parent_hex, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 3);
    }
    sqlite3_bind_text(st, 4, otel_service_name(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, s->name ? s->name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, s->kind);
    sqlite3_bind_int(st, 7, s->status);
    sqlite3_bind_text(st, 8, s->status_message ? s->status_message : "",
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, (sqlite3_int64) s->start_unix_ns);
    sqlite3_bind_int64(st, 10, (sqlite3_int64) s->end_unix_ns);
    sqlite3_bind_int64(st, 11, (sqlite3_int64) (s->end_unix_ns - s->start_unix_ns));
    sqlite3_bind_int(st, 12, s->dropped_attrs);
    sqlite3_bind_int(st, 13, s->dropped_events);

    if (sqlite3_step(st) != SQLITE_DONE) {
        return;
    }
    S.cur_span = sqlite3_last_insert_rowid(S.db);

    /* Attributes. */
    for (i = 0; i < s->num_attrs; i++) {
        const struct otel_attr *a = &s->attrs[i];

        st = S.ins_attr;
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, S.cur_span);
        sqlite3_bind_text(st, 2, a->key ? a->key : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, a->type);
        sqlite3_bind_null(st, 4);
        sqlite3_bind_null(st, 5);
        sqlite3_bind_null(st, 6);
        switch (a->type) {
            case OTEL_ATTR_STR:
                sqlite3_bind_text(st, 4, a->v.s ? a->v.s : "", -1, SQLITE_TRANSIENT);
                break;
            case OTEL_ATTR_I64:
                sqlite3_bind_int64(st, 5, (sqlite3_int64) a->v.i);
                break;
            case OTEL_ATTR_U64:
                sqlite3_bind_int64(st, 5, (sqlite3_int64) a->v.u);
                break;
            case OTEL_ATTR_F64:
                sqlite3_bind_double(st, 6, a->v.d);
                break;
            case OTEL_ATTR_BOOL:
                sqlite3_bind_int64(st, 5, a->v.b ? 1 : 0);
                break;
            default:
                break;
        } /* switch */
        sqlite3_step(st);
    }

    /* Events. */
    for (i = 0; i < s->num_events; i++) {
        const struct otel_event *e = &s->events[i];

        st = S.ins_event;
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, S.cur_span);
        sqlite3_bind_text(st, 2, e->name ? e->name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, (sqlite3_int64) e->time_unix_ns);
        sqlite3_step(st);
    }
} /* otel_sqlite_span */

static void
otel_sqlite_prune(void)
{
    sqlite3_stmt *st = NULL;
    sqlite3_int64 maxid, cutoff;
    char          buf[256];

    if (S.max_spans == 0) {
        return;
    }

    if (sqlite3_prepare_v2(S.db, "SELECT MAX(id) FROM spans", -1, &st, NULL)
        != SQLITE_OK) {
        return;
    }
    maxid = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);

    cutoff = maxid - (sqlite3_int64) S.max_spans;
    if (cutoff <= 0) {
        return;
    }

    snprintf(buf, sizeof(buf),
             "DELETE FROM span_attrs WHERE span <= %lld;"
             "DELETE FROM span_events WHERE span <= %lld;"
             "DELETE FROM spans WHERE id <= %lld;",
             (long long) cutoff, (long long) cutoff, (long long) cutoff);
    otel_sqlite_exec(buf);
}

static void
otel_sqlite_end(void *priv)
{
    (void) priv;

    if (!S.in_txn) {
        return;
    }

    otel_sqlite_exec("COMMIT");
    S.in_txn = 0;
    S.commits++;

    /* Periodic maintenance off the per-span path: checkpoint the WAL so it does
     * not grow without bound, and apply retention. */
    if (S.commits % OTEL_SQLITE_MAINT_EVERY == 0) {
        otel_sqlite_prune();
        sqlite3_wal_checkpoint_v2(S.db, NULL, SQLITE_CHECKPOINT_PASSIVE,
                                  NULL, NULL);
    }
}

/* ---- flusher thread ---- */

static void *
otel_sqlite_flusher(void *arg)
{
    (void) arg;

    pthread_mutex_lock(&S.lock);
    while (S.running) {
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long) (S.interval_ms % 1000) * 1000000L;
        ts.tv_sec  += (time_t) (S.interval_ms / 1000) + ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;

        pthread_cond_timedwait(&S.cond, &S.lock, &ts);

        /* Drain outside the lock: it may take a while and must not block close(). */
        pthread_mutex_unlock(&S.lock);
        otel_drain();
        pthread_mutex_lock(&S.lock);
    }
    pthread_mutex_unlock(&S.lock);
    return NULL;
}

/* ---- lifecycle ---- */

SYMBOL_EXPORT void
otel_sqlite_set_max_spans(unsigned long max_spans)
{
    S.max_spans = max_spans;
}

SYMBOL_EXPORT int
otel_sqlite_open(
    const char  *path,
    unsigned int flush_interval_ms)
{
    char buf[64];

    if (S.db) {
        fprintf(stderr, "otel_sqlite: already open\n");
        return -1;
    }

    if (sqlite3_open(path, &S.db) != SQLITE_OK) {
        fprintf(stderr, "otel_sqlite: open %s failed: %s\n", path,
                S.db ? sqlite3_errmsg(S.db) : "?");
        otel_sqlite_close();
        return -1;
    }

    /* Write-optimized for a single bulk writer + concurrent CLI readers.
     * page_size must be set before the schema is created. */
    otel_sqlite_exec("PRAGMA page_size=8192");
    otel_sqlite_exec("PRAGMA journal_mode=WAL");
    otel_sqlite_exec("PRAGMA synchronous=NORMAL");
    otel_sqlite_exec("PRAGMA temp_store=MEMORY");
    otel_sqlite_exec("PRAGMA cache_size=-16384");   /* ~16 MiB page cache */
    otel_sqlite_exec("PRAGMA busy_timeout=5000");
    otel_sqlite_exec("PRAGMA wal_autocheckpoint=0"); /* we checkpoint manually */

    if (otel_sqlite_exec(OTEL_SQLITE_SCHEMA) != 0) {
        otel_sqlite_close();
        return -1;
    }
    snprintf(buf, sizeof(buf), "PRAGMA user_version=%d",
             OTEL_SQLITE_SCHEMA_VERSION);
    otel_sqlite_exec(buf);

    if (sqlite3_prepare_v2(S.db,
            "INSERT INTO spans(trace_id,span_id,parent_id,service,name,kind,"
            "status_code,status_message,start_unix_ns,end_unix_ns,duration_ns,"
            "dropped_attrs,dropped_events) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &S.ins_span, NULL)
        != SQLITE_OK ||
        sqlite3_prepare_v2(S.db,
            "INSERT INTO span_attrs(span,key,type,s_val,i_val,d_val) "
            "VALUES(?,?,?,?,?,?)", -1, &S.ins_attr, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(S.db,
            "INSERT INTO span_events(span,name,time_unix_ns) "
            "VALUES(?,?,?)", -1, &S.ins_event, NULL) != SQLITE_OK) {
        fprintf(stderr, "otel_sqlite: prepare failed: %s\n",
                sqlite3_errmsg(S.db));
        otel_sqlite_close();
        return -1;
    }

    /* Register the sink with the core tracer. */
    S.sink.begin = otel_sqlite_begin;
    S.sink.span  = otel_sqlite_span;
    S.sink.end   = otel_sqlite_end;
    S.sink.priv  = NULL;
    otel_set_span_sink(&S.sink);

    /* Spawn the dedicated flusher thread. */
    S.interval_ms = flush_interval_ms ? flush_interval_ms
                                      : OTEL_SQLITE_DEFAULT_INTERVAL_MS;
    pthread_mutex_init(&S.lock, NULL);
    pthread_cond_init(&S.cond, NULL);
    S.running = 1;
    if (pthread_create(&S.thread, NULL, otel_sqlite_flusher, NULL) != 0) {
        fprintf(stderr, "otel_sqlite: flusher thread create failed\n");
        S.running = 0;
        otel_sqlite_close();
        return -1;
    }
    S.have_thread = 1;
    return 0;
} /* otel_sqlite_open */

SYMBOL_EXPORT void
otel_sqlite_close(void)
{
    /* Stop the flusher thread first. */
    if (S.have_thread) {
        pthread_mutex_lock(&S.lock);
        S.running = 0;
        pthread_cond_signal(&S.cond);
        pthread_mutex_unlock(&S.lock);
        pthread_join(S.thread, NULL);
        S.have_thread = 0;
        pthread_cond_destroy(&S.cond);
        pthread_mutex_destroy(&S.lock);
    }

    /* Final drain to flush any spans still in the rings (sink still attached). */
    otel_drain();

    /* Detach the sink before tearing down the database. */
    otel_set_span_sink(NULL);

    if (S.ins_span) {
        sqlite3_finalize(S.ins_span);
    }
    if (S.ins_attr) {
        sqlite3_finalize(S.ins_attr);
    }
    if (S.ins_event) {
        sqlite3_finalize(S.ins_event);
    }
    if (S.db) {
        /* Fold the WAL back into the main db so the file is self-contained. */
        sqlite3_wal_checkpoint_v2(S.db, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                                  NULL, NULL);
        sqlite3_close(S.db);
    }
    memset(&S, 0, sizeof(S));
} /* otel_sqlite_close */
