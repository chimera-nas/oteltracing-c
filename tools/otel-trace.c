// SPDX-FileCopyrightText: 2026 Ben Jarvis
//
// SPDX-License-Identifier: MIT

/*
 * otel-trace -- query a local oteltracing-c SQLite span store.
 *
 * The store is written by the optional SQLite sink (otel_sqlite.c).  This tool is
 * a read-only client: it only needs libsqlite3 and the shared schema header.  It
 * opens the database read-only and in WAL mode so it can be run against a file a
 * live process is still writing to.
 *
 * Commands:
 *   list   [filters]        recent traces (one row per trace)
 *   show   <trace_id>       full span tree for one trace
 *   spans  [filters]        individual spans
 *   stats                   per-name span counts and latency
 *   sql    "<query>"        run an arbitrary read-only query
 *
 * Filters: --service S  --name N  --status error  --since SECONDS
 *          --min-duration-us US  --limit N
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include <sqlite3.h>

#include "otel_sqlite_schema.h"

struct filters {
    const char    *service;
    const char    *name;
    int            status_error;
    long long      since_sec;
    long long      min_duration_us;
    int            limit;
};

static const char *
kind_name(int k)
{
    switch (k) {
        case 1:  return "server";
        case 2:  return "client";
        case 3:  return "producer";
        case 4:  return "consumer";
        default: return "internal";
    }
}

static const char *
status_name(int s)
{
    switch (s) {
        case 1:  return "ok";
        case 2:  return "ERROR";
        default: return "unset";
    }
}

static void
fmt_time(long long unix_ns, char *out, size_t outlen)
{
    time_t    secs = (time_t) (unix_ns / 1000000000LL);
    long      ms   = (long) ((unix_ns % 1000000000LL) / 1000000LL);
    struct tm tm;
    char      base[32];

    localtime_r(&secs, &tm);
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(out, outlen, "%s.%03ld", base, ms);
}

/* Render a ns duration into a compact human string. */
static void
fmt_dur(long long ns, char *out, size_t outlen)
{
    if (ns < 1000) {
        snprintf(out, outlen, "%lldns", ns);
    } else if (ns < 1000000) {
        snprintf(out, outlen, "%.1fus", ns / 1000.0);
    } else if (ns < 1000000000) {
        snprintf(out, outlen, "%.2fms", ns / 1000000.0);
    } else {
        snprintf(out, outlen, "%.2fs", ns / 1000000000.0);
    }
}

static sqlite3 *
db_open(const char *path)
{
    sqlite3 *db = NULL;

    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "otel-trace: cannot open %s: %s\n", path,
                db ? sqlite3_errmsg(db) : "?");
        if (db) {
            sqlite3_close(db);
        }
        return NULL;
    }
    sqlite3_busy_timeout(db, 2000);
    return db;
}

/* Append WHERE clauses for the common filters; bind happens by the caller after
 * prepare (returns the next bind index to use). */
static void
append_filters(char *sql, size_t cap, const struct filters *f)
{
    if (f->service) {
        strncat(sql, " AND service = ?", cap - strlen(sql) - 1);
    }
    if (f->name) {
        strncat(sql, " AND name = ?", cap - strlen(sql) - 1);
    }
    if (f->status_error) {
        strncat(sql, " AND status_code = 2", cap - strlen(sql) - 1);
    }
    if (f->since_sec) {
        strncat(sql, " AND start_unix_ns >= ?", cap - strlen(sql) - 1);
    }
    if (f->min_duration_us) {
        strncat(sql, " AND duration_ns >= ?", cap - strlen(sql) - 1);
    }
}

static int
bind_filters(sqlite3_stmt *st, int idx, const struct filters *f)
{
    struct timespec now;

    if (f->service) {
        sqlite3_bind_text(st, idx++, f->service, -1, SQLITE_TRANSIENT);
    }
    if (f->name) {
        sqlite3_bind_text(st, idx++, f->name, -1, SQLITE_TRANSIENT);
    }
    if (f->since_sec) {
        clock_gettime(CLOCK_REALTIME, &now);
        sqlite3_bind_int64(st, idx++,
            ((sqlite3_int64) now.tv_sec - f->since_sec) * 1000000000LL);
    }
    if (f->min_duration_us) {
        sqlite3_bind_int64(st, idx++, f->min_duration_us * 1000LL);
    }
    return idx;
}

/* ---- list ---- */

static int
cmd_list(sqlite3 *db, const struct filters *f)
{
    char          sql[1024];
    sqlite3_stmt *st;
    int           idx;

    /* Aggregate to one row per trace, picking the root span's name. */
    snprintf(sql, sizeof(sql),
        "SELECT trace_id,"
        "       MIN(start_unix_ns),"
        "       MAX(end_unix_ns) - MIN(start_unix_ns),"
        "       COUNT(*),"
        "       MAX(CASE WHEN status_code=2 THEN 1 ELSE 0 END),"
        "       (SELECT name FROM spans r WHERE r.trace_id=s.trace_id "
        "          AND r.parent_id IS NULL LIMIT 1),"
        "       (SELECT service FROM spans v WHERE v.trace_id=s.trace_id LIMIT 1)"
        " FROM spans s WHERE 1=1");
    append_filters(sql, sizeof(sql), f);
    strncat(sql, " GROUP BY trace_id ORDER BY MIN(start_unix_ns) DESC LIMIT ?",
            sizeof(sql) - strlen(sql) - 1);

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "otel-trace: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    idx = bind_filters(st, 1, f);
    sqlite3_bind_int(st, idx, f->limit > 0 ? f->limit : 50);

    printf("%-34s %-10s %-23s %10s %5s %s\n",
           "TRACE ID", "SERVICE", "START", "DURATION", "SPANS", "ROOT");
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *tid  = sqlite3_column_text(st, 0);
        long long            t0   = sqlite3_column_int64(st, 1);
        long long            dur  = sqlite3_column_int64(st, 2);
        int                  n    = sqlite3_column_int(st, 3);
        int                  err  = sqlite3_column_int(st, 4);
        const unsigned char *root = sqlite3_column_text(st, 5);
        const unsigned char *svc  = sqlite3_column_text(st, 6);
        char                 tbuf[40], dbuf[24];

        fmt_time(t0, tbuf, sizeof(tbuf));
        fmt_dur(dur, dbuf, sizeof(dbuf));
        printf("%-34s %-10s %-23s %10s %5d %s%s\n",
               tid ? (const char *) tid : "?",
               svc ? (const char *) svc : "-",
               tbuf, dbuf, n,
               root ? (const char *) root : "(no root)",
               err ? "  [ERROR]" : "");
    }
    sqlite3_finalize(st);
    return 0;
} /* cmd_list */

/* ---- show ---- */

struct snode {
    sqlite3_int64 id;
    char          span_id[20];
    char          parent_id[20];
    char         *name;
    int           kind;
    int           status;
    char         *status_msg;
    long long     start;
    long long     end;
    int           printed;
};

/* Build a padding string of `width` spaces (capped) into `pad` (>=64 bytes). */
static void
make_pad(char *pad, int width)
{
    if (width > 60) {
        width = 60;
    }
    if (width < 0) {
        width = 0;
    }
    memset(pad, ' ', width);
    pad[width] = '\0';
}

/* Print the rows of a prepared+bound statement selecting (key,type,s_val,i_val,
 * d_val), one "<pad>- key = value" line each, then finalize it.  Shared by span
 * and event attribute printing (identical column layout). */
static void
print_attr_rows(sqlite3_stmt *st, const char *pad)
{
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *k = sqlite3_column_text(st, 0);
        int                  t = sqlite3_column_int(st, 1);

        printf("%s- %s = ", pad, k ? (const char *) k : "?");
        switch (t) {
            case 0:  printf("\"%s\"\n", sqlite3_column_text(st, 2)); break;
            case 1:
            case 2:  printf("%lld\n", (long long) sqlite3_column_int64(st, 3));
                break;
            case 3:  printf("%g\n", sqlite3_column_double(st, 4)); break;
            case 4:  printf("%s\n",
                            sqlite3_column_int64(st, 3) ? "true" : "false");
                break;
            default: printf("?\n"); break;
        }
    }
    sqlite3_finalize(st);
}

/* Print one event's attributes, indented `width` spaces. */
static void
print_event_attrs(sqlite3 *db, sqlite3_int64 event_id, int width)
{
    sqlite3_stmt *st;
    char          pad[64];

    make_pad(pad, width);
    if (sqlite3_prepare_v2(db,
            "SELECT key,type,s_val,i_val,d_val FROM span_event_attrs WHERE event=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, event_id);
        print_attr_rows(st, pad);
    }
}

static void
print_attrs_events(sqlite3 *db, sqlite3_int64 span_id, int depth)
{
    sqlite3_stmt *st;
    char          pad[64];
    int           p = depth * 2 + 2;

    make_pad(pad, p);
    if (p > 60) {
        p = 60;
    }

    if (sqlite3_prepare_v2(db,
            "SELECT key,type,s_val,i_val,d_val FROM span_attrs WHERE span=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, span_id);
        print_attr_rows(st, pad);
    }

    if (sqlite3_prepare_v2(db,
            "SELECT id,name,time_unix_ns FROM span_events WHERE span=? "
            "ORDER BY time_unix_ns", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, span_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            sqlite3_int64 event_id = sqlite3_column_int64(st, 0);

            printf("%s* event: %s\n", pad,
                   sqlite3_column_text(st, 1));
            print_event_attrs(db, event_id, p + 2);
        }
        sqlite3_finalize(st);
    }
}

static void
print_subtree(
    sqlite3            *db,
    struct snode       *nodes,
    int                 n,
    const char         *parent_span,
    long long           trace_t0,
    int                 depth)
{
    int i;

    for (i = 0; i < n; i++) {
        struct snode *s = &nodes[i];
        char          dbuf[24], obuf[24], sbuf[160];
        int           pad = depth * 2;

        if (s->printed) {
            continue;
        }
        /* Match children of `parent_span`; treat unknown parents as roots. */
        if (parent_span) {
            if (strcmp(s->parent_id, parent_span) != 0) {
                continue;
            }
        } else if (s->parent_id[0] != '\0') {
            int found = 0, j;
            for (j = 0; j < n; j++) {
                if (strcmp(nodes[j].span_id, s->parent_id) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                continue;   /* will be printed under its real parent */
            }
        }

        s->printed = 1;
        fmt_dur(s->end - s->start, dbuf, sizeof(dbuf));
        fmt_dur(s->start - trace_t0, obuf, sizeof(obuf));

        sbuf[0] = '\0';
        if (s->status == 2) {
            if (s->status_msg && s->status_msg[0]) {
                snprintf(sbuf, sizeof(sbuf), "  status=ERROR: %s", s->status_msg);
            } else {
                snprintf(sbuf, sizeof(sbuf), "  status=ERROR");
            }
        }

        printf("%*s%s [%s] %s (+%s)%s\n",
               pad, "", s->name ? s->name : "?", kind_name(s->kind),
               dbuf, obuf, sbuf);

        print_attrs_events(db, s->id, depth);
        print_subtree(db, nodes, n, s->span_id, trace_t0, depth + 1);
    }
} /* print_subtree */

static int
cmd_show(sqlite3 *db, const char *trace_id)
{
    sqlite3_stmt *st;
    struct snode *nodes = NULL;
    int           n = 0, cap = 0;
    long long     t0 = 0;

    if (sqlite3_prepare_v2(db,
            "SELECT id,span_id,parent_id,name,kind,status_code,status_message,"
            "start_unix_ns,end_unix_ns FROM spans WHERE trace_id=? "
            "ORDER BY start_unix_ns", -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "otel-trace: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_bind_text(st, 1, trace_id, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        struct snode *s;
        const unsigned char *txt;

        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            nodes = realloc(nodes, cap * sizeof(*nodes));
        }
        s = &nodes[n++];
        memset(s, 0, sizeof(*s));
        s->id = sqlite3_column_int64(st, 0);
        snprintf(s->span_id, sizeof(s->span_id), "%s",
                 (txt = sqlite3_column_text(st, 1)) ? (const char *) txt : "");
        snprintf(s->parent_id, sizeof(s->parent_id), "%s",
                 (txt = sqlite3_column_text(st, 2)) ? (const char *) txt : "");
        txt = sqlite3_column_text(st, 3);
        s->name = txt ? strdup((const char *) txt) : NULL;
        s->kind = sqlite3_column_int(st, 4);
        s->status = sqlite3_column_int(st, 5);
        txt = sqlite3_column_text(st, 6);
        s->status_msg = txt ? strdup((const char *) txt) : NULL;
        s->start = sqlite3_column_int64(st, 7);
        s->end = sqlite3_column_int64(st, 8);
        if (n == 1 || s->start < t0) {
            t0 = s->start;
        }
    }
    sqlite3_finalize(st);

    if (n == 0) {
        fprintf(stderr, "otel-trace: no spans for trace %s\n", trace_id);
        free(nodes);
        return 1;
    }

    printf("trace %s  (%d spans)\n", trace_id, n);
    print_subtree(db, nodes, n, NULL, t0, 0);

    for (int i = 0; i < n; i++) {
        free(nodes[i].name);
        free(nodes[i].status_msg);
    }
    free(nodes);
    return 0;
} /* cmd_show */

/* ---- spans ---- */

static int
cmd_spans(sqlite3 *db, const struct filters *f)
{
    char          sql[1024];
    sqlite3_stmt *st;
    int           idx;

    snprintf(sql, sizeof(sql),
        "SELECT start_unix_ns,duration_ns,service,name,kind,status_code,"
        "trace_id FROM spans WHERE 1=1");
    append_filters(sql, sizeof(sql), f);
    strncat(sql, " ORDER BY start_unix_ns DESC LIMIT ?",
            sizeof(sql) - strlen(sql) - 1);

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "otel-trace: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    idx = bind_filters(st, 1, f);
    sqlite3_bind_int(st, idx, f->limit > 0 ? f->limit : 50);

    printf("%-23s %10s %-10s %-8s %-6s %-24s %s\n",
           "START", "DURATION", "SERVICE", "KIND", "STATUS", "NAME", "TRACE");
    while (sqlite3_step(st) == SQLITE_ROW) {
        char tbuf[40], dbuf[24];

        fmt_time(sqlite3_column_int64(st, 0), tbuf, sizeof(tbuf));
        fmt_dur(sqlite3_column_int64(st, 1), dbuf, sizeof(dbuf));
        printf("%-23s %10s %-10s %-8s %-6s %-24s %s\n",
               tbuf, dbuf,
               sqlite3_column_text(st, 2) ? (const char *) sqlite3_column_text(st, 2) : "-",
               kind_name(sqlite3_column_int(st, 4)),
               status_name(sqlite3_column_int(st, 5)),
               sqlite3_column_text(st, 3) ? (const char *) sqlite3_column_text(st, 3) : "?",
               sqlite3_column_text(st, 6) ? (const char *) sqlite3_column_text(st, 6) : "?");
    }
    sqlite3_finalize(st);
    return 0;
} /* cmd_spans */

/* ---- stats ---- */

static int
cmd_stats(sqlite3 *db)
{
    sqlite3_stmt *st;

    if (sqlite3_prepare_v2(db,
            "SELECT name, COUNT(*), "
            "       CAST(AVG(duration_ns) AS INTEGER), "
            "       MIN(duration_ns), MAX(duration_ns), "
            "       SUM(CASE WHEN status_code=2 THEN 1 ELSE 0 END) "
            "FROM spans GROUP BY name ORDER BY COUNT(*) DESC LIMIT 50",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "otel-trace: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    printf("%-28s %8s %10s %10s %10s %7s\n",
           "NAME", "COUNT", "AVG", "MIN", "MAX", "ERRORS");
    while (sqlite3_step(st) == SQLITE_ROW) {
        char abuf[24], nbuf[24], xbuf[24];

        fmt_dur(sqlite3_column_int64(st, 2), abuf, sizeof(abuf));
        fmt_dur(sqlite3_column_int64(st, 3), nbuf, sizeof(nbuf));
        fmt_dur(sqlite3_column_int64(st, 4), xbuf, sizeof(xbuf));
        printf("%-28s %8d %10s %10s %10s %7d\n",
               sqlite3_column_text(st, 0) ? (const char *) sqlite3_column_text(st, 0) : "?",
               sqlite3_column_int(st, 1), abuf, nbuf, xbuf,
               sqlite3_column_int(st, 5));
    }
    sqlite3_finalize(st);
    return 0;
} /* cmd_stats */

/* ---- sql ---- */

static int
sql_print_row(void *arg, int ncol, char **vals, char **names)
{
    int *first = arg;
    int  i;

    if (*first) {
        for (i = 0; i < ncol; i++) {
            printf("%s%s", i ? "|" : "", names[i]);
        }
        printf("\n");
        *first = 0;
    }
    for (i = 0; i < ncol; i++) {
        printf("%s%s", i ? "|" : "", vals[i] ? vals[i] : "NULL");
    }
    printf("\n");
    return 0;
}

static int
cmd_sql(sqlite3 *db, const char *query)
{
    char *err = NULL;
    int   first = 1;

    if (sqlite3_exec(db, query, sql_print_row, &first, &err) != SQLITE_OK) {
        fprintf(stderr, "otel-trace: %s\n", err ? err : "?");
        sqlite3_free(err);
        return 1;
    }
    return 0;
}

/* ---- main ---- */

static void
usage(void)
{
    fprintf(stderr,
        "usage: otel-trace [--db PATH] <command> [args]\n"
        "\n"
        "commands:\n"
        "  list                 recent traces (one row per trace)\n"
        "  show <trace_id>      full span tree for one trace\n"
        "  spans                individual spans\n"
        "  stats                per-name span counts and latency\n"
        "  sql \"<query>\"        run a read-only SQL query\n"
        "\n"
        "filters (list, spans):\n"
        "  --service S          only this service\n"
        "  --name N             only spans named N\n"
        "  --status error       only traces/spans with an error status\n"
        "  --since SECONDS      only the last SECONDS\n"
        "  --min-duration-us US only spans/traces at least US microseconds\n"
        "  --limit N            cap rows (default 50)\n"
        "\n"
        "The database path defaults to $OTEL_TRACE_DB or ./traces.db.\n");
}

int
main(int argc, char **argv)
{
    const char     *db_path = getenv("OTEL_TRACE_DB");
    struct filters  f;
    const char     *cmd = NULL;
    const char     *arg = NULL;
    sqlite3        *db;
    int             i, rc;

    memset(&f, 0, sizeof(f));
    if (!db_path) {
        db_path = "traces.db";
    }

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--db") && i + 1 < argc) {
            db_path = argv[++i];
        } else if (!strcmp(argv[i], "--service") && i + 1 < argc) {
            f.service = argv[++i];
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            f.name = argv[++i];
        } else if (!strcmp(argv[i], "--status") && i + 1 < argc) {
            f.status_error = (strcmp(argv[++i], "error") == 0);
        } else if (!strcmp(argv[i], "--since") && i + 1 < argc) {
            f.since_sec = strtoll(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--min-duration-us") && i + 1 < argc) {
            f.min_duration_us = strtoll(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
            f.limit = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "otel-trace: unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else if (!cmd) {
            cmd = argv[i];
        } else if (!arg) {
            arg = argv[i];
        }
    }

    if (!cmd) {
        usage();
        return 2;
    }

    db = db_open(db_path);
    if (!db) {
        return 1;
    }

    if (!strcmp(cmd, "list")) {
        rc = cmd_list(db, &f);
    } else if (!strcmp(cmd, "show")) {
        if (!arg) {
            fprintf(stderr, "otel-trace: show needs a trace_id\n");
            rc = 2;
        } else {
            rc = cmd_show(db, arg);
        }
    } else if (!strcmp(cmd, "spans")) {
        rc = cmd_spans(db, &f);
    } else if (!strcmp(cmd, "stats")) {
        rc = cmd_stats(db);
    } else if (!strcmp(cmd, "sql")) {
        if (!arg) {
            fprintf(stderr, "otel-trace: sql needs a query\n");
            rc = 2;
        } else {
            rc = cmd_sql(db, arg);
        }
    } else {
        fprintf(stderr, "otel-trace: unknown command %s\n", cmd);
        usage();
        rc = 2;
    }

    sqlite3_close(db);
    return rc;
} /* main */
