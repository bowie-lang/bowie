#include "postgres.h"
#include "env.h"
#include "object.h"
#include "event_loop.h"
#include "coro.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARG(n)    (args->count > (n) ? args->items[n] : BOWIE_NULL)
#define NARGS     (args->count)
#define REQUIRE(n, name) \
    if (NARGS < (n)) return obj_errorf(name "() requires %d argument(s)", n)

static Object *pg_err_result(PGresult *res) {
    const char *msg = PQresultErrorMessage(res);
    char buf[640];
    snprintf(buf, sizeof(buf), "pg_query: %s", msg ? msg : "unknown error");
    return obj_errorf("%s", buf);
}

/* ---- PG connection pool ---- */

#define PG_POOL_MAX 64

typedef struct {
    PGconn *conn;
    int     in_use;
} PgPoolEntry;

typedef struct PgPoolWaiter {
    Coro                *coro;
    PGconn              *conn;  /* set by pool_release; read by pool_acquire after yield */
    struct PgPoolWaiter *next;
} PgPoolWaiter;

typedef struct {
    PgPoolEntry  entries[PG_POOL_MAX];
    int          size;     /* current number of open connections */
    int          max_size; /* hard cap — never open more than this */
    char        *conninfo;
    PgPoolWaiter *waiters_head;
    PgPoolWaiter *waiters_tail;
} PgPool;

static PgPool *pool_from_obj(Object *o) {
    return (PgPool *)o->pg.conn;
}

/* Try to get a free connection; return NULL if all are busy (no growth). */
static PGconn *pool_try_acquire(PgPool *pool) {
    for (int i = 0; i < pool->size; i++) {
        if (!pool->entries[i].in_use) {
            if (PQstatus(pool->entries[i].conn) != CONNECTION_OK) {
                PQfinish(pool->entries[i].conn);
                pool->entries[i].conn = PQconnectdb(pool->conninfo);
            }
            pool->entries[i].in_use = 1;
            return pool->entries[i].conn;
        }
    }
    return NULL;
}

/*
 * Acquire a connection. If the pool is exhausted and we are inside a coroutine,
 * yield until another coroutine releases one. Outside a coroutine, fail immediately.
 */
static PGconn *pool_acquire(PgPool *pool) {
    PGconn *c = pool_try_acquire(pool);
    if (c) return c;

    if (!g_running_coro) return NULL; /* sync context — can't wait */

    /* Enqueue this coroutine as a waiter */
    PgPoolWaiter *w = malloc(sizeof(PgPoolWaiter));
    w->coro = g_running_coro;
    w->conn = NULL;
    w->next = NULL;
    if (pool->waiters_tail) {
        pool->waiters_tail->next = w;
        pool->waiters_tail = w;
    } else {
        pool->waiters_head = pool->waiters_tail = w;
    }
    coro_yield(); /* suspend until a connection is released */
    /* pool_release set w->conn and kept in_use=1 — no other coro can steal it */
    PGconn *assigned = w->conn;
    free(w);
    return assigned;
}

static void pool_release(PgPool *pool, PGconn *c) {
    for (int i = 0; i < pool->size; i++) {
        if (pool->entries[i].conn == c) {
            if (pool->waiters_head) {
                /* Hand connection directly to the first waiter.
                 * Keep in_use=1 so no other coroutine can steal it before
                 * the waiter runs. pool_acquire reads w->conn and frees w. */
                PgPoolWaiter *w = pool->waiters_head;
                pool->waiters_head = w->next;
                if (!pool->waiters_head) pool->waiters_tail = NULL;
                w->conn = c;
                event_loop_enqueue(w->coro);
                /* do NOT free w here — pool_acquire frees it after reading conn */
            } else {
                pool->entries[i].in_use = 0;
            }
            return;
        }
    }
}

void bowie_pg_conn_finish(void *conn) {
    PgPool *pool = (PgPool *)conn;
    if (!pool) return;
    for (int i = 0; i < pool->size; i++) {
        if (pool->entries[i].conn)
            PQfinish(pool->entries[i].conn);
    }
    free(pool->conninfo);
    free(pool);
}

static Object *bw_pg_connect(ObjList *args) {
    REQUIRE(1, "pg_connect");
    if (ARG(0)->type != OBJ_STRING)
        return obj_errorf("pg_connect(): connection info must be a string");

    const char *conninfo = ARG(0)->string.str;

    /* Determine pool size from env var, default 4 */
    int pool_size = 4;
    const char *ps = getenv("BOWIE_PG_POOL_SIZE");
    if (ps) { int v = atoi(ps); if (v > 0 && v <= PG_POOL_MAX) pool_size = v; }

    PgPool *pool = calloc(1, sizeof(PgPool));
    pool->conninfo  = strdup(conninfo);
    pool->size      = 0;
    pool->max_size  = pool_size;

    /* Open connections eagerly */
    for (int i = 0; i < pool_size; i++) {
        PGconn *c = PQconnectdb(conninfo);
        if (!c || PQstatus(c) != CONNECTION_OK) {
            char buf[640];
            snprintf(buf, sizeof(buf), "pg_connect: %s", c ? PQerrorMessage(c) : "PQconnectdb returned null");
            if (c) PQfinish(c);
            /* Release already-opened connections */
            for (int j = 0; j < i; j++) PQfinish(pool->entries[j].conn);
            free(pool->conninfo);
            free(pool);
            return obj_errorf("%s", buf);
        }
        pool->entries[i].conn   = c;
        pool->entries[i].in_use = 0;
        pool->size++;
    }

    return obj_pg_conn(pool);
}

static Object *bw_pg_close(ObjList *args) {
    REQUIRE(1, "pg_close");
    if (ARG(0)->type != OBJ_PG_CONN)
        return obj_errorf("pg_close(): expected pg_conn");
    Object *o = ARG(0);
    if (o->pg.conn) {
        bowie_pg_conn_finish(o->pg.conn); /* frees the PgPool */
        o->pg.conn = NULL;
    }
    return obj_null();
}

static Object *result_to_object(PGresult *res) {
    ExecStatusType st = PQresultStatus(res);
    if (st == PGRES_FATAL_ERROR || st == PGRES_NONFATAL_ERROR) {
        Object *e = pg_err_result(res);
        PQclear(res);
        return e;
    }
    if (st == PGRES_COMMAND_OK || st == PGRES_EMPTY_QUERY) {
        Object *h = obj_hash();
        Object *_cmd = obj_string(PQcmdStatus(res));
        hash_set(h, "command", _cmd);
        obj_release(_cmd);
        const char *t = PQcmdTuples(res);
        long long n   = 0;
        if (t && t[0]) {
            char *end;
            n = strtoll(t, &end, 10);
            if (end == t) n = 0;
        }
        Object *_rows = obj_int(n);
        hash_set(h, "rows", _rows);
        obj_release(_rows);
        PQclear(res);
        return h;
    }
    if (st == PGRES_TUPLES_OK) {
        int nrows   = PQntuples(res);
        int nfields = PQnfields(res);
        Object *arr = obj_array();
        for (int i = 0; i < nrows; i++) {
            Object *row = obj_hash();
            for (int j = 0; j < nfields; j++) {
                const char *name = PQfname(res, j);
                if (!name) name = "";
                if (PQgetisnull(res, i, j)) {
                    hash_set(row, name, obj_null());
                } else {
                    const char *val = PQgetvalue(res, i, j);
                    Object *_v = obj_string(val ? val : "");
                    hash_set(row, name, _v);
                    obj_release(_v);
                }
            }
            array_push(arr, row);
            obj_release(row);
        }
        PQclear(res);
        return arr;
    }
    PQclear(res);
    return obj_errorf("pg_query(): unsupported or unexpected result status");
}

/* FdCallback: called when the PG socket is readable; re-enqueues the waiting coro */
static void on_pg_ready(int fd, void *ctx) {
    Coro *c = (Coro *)ctx;
    /* Drain network data into libpq's internal buffer */
    PgPool *pool = NULL; /* context not needed here — coro will call PQgetResult */
    (void)fd;
    (void)pool;
    /* Unwatch and re-enqueue the coroutine to run */
    event_loop_unwatch_fd(fd);
    event_loop_enqueue(c);
}

static Object *bw_pg_query(ObjList *args) {
    REQUIRE(2, "pg_query");
    if (ARG(0)->type != OBJ_PG_CONN)
        return obj_errorf("pg_query(): first argument must be pg_conn");
    if (ARG(1)->type != OBJ_STRING)
        return obj_errorf("pg_query(): SQL must be a string");

    PgPool *pool = pool_from_obj(ARG(0));
    if (!pool)
        return obj_errorf("pg_query(): connection is closed");

    const char *sql = ARG(1)->string.str;

    PGconn *c = pool_acquire(pool);
    if (!c) /* only reachable in sync (non-coro) context at PG_POOL_MAX */
        return obj_errorf("pg_query(): connection pool exhausted");

    if (PQstatus(c) != CONNECTION_OK) {
        pool_release(pool, c);
        return obj_errorf("pg_query(): connection is not OK: %s", PQerrorMessage(c));
    }

    Object *result;

    if (g_running_coro != NULL) {
        /* Async path: send query, yield, resume when result is ready */
        if (!PQsendQuery(c, sql)) {
            pool_release(pool, c);
            return obj_errorf("pg_query(): PQsendQuery failed: %s", PQerrorMessage(c));
        }
        int pg_sock = PQsocket(c);

        /* Flush write buffer, consume any data already in socket buffer */
        PQflush(c);
        PQconsumeInput(c);

        /* Yield while result is not yet ready */
        while (PQisBusy(c)) {
            event_loop_watch_fd(pg_sock, on_pg_ready, g_running_coro);
            coro_yield();
            /* on_pg_ready already unwatched the fd */
            PQconsumeInput(c);
        }

        PGresult *res = PQgetResult(c);
        /* Drain any remaining results */
        PGresult *extra;
        while ((extra = PQgetResult(c))) PQclear(extra);
        result = res ? result_to_object(res) :
                 obj_errorf("pg_query(): no result from server");
    } else {
        /* Sync path: used outside of HTTP coroutines (scripts, etc.) */
        PGresult *res = PQexec(c, sql);
        if (!res) {
            pool_release(pool, c);
            return obj_errorf("pg_query(): PQexec failed: %s", PQerrorMessage(c));
        }
        result = result_to_object(res);
    }

    pool_release(pool, c);
    return result;
}

void postgres_register(Env *env) {
    Object *tmp;
#define REG(name, fn) env_set(env, name, (tmp = obj_builtin(fn, name))); obj_release(tmp)

    REG("pg_connect", bw_pg_connect);
    REG("pg_close", bw_pg_close);
    REG("pg_query", bw_pg_query);
#undef REG
}
