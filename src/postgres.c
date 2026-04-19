#include "postgres.h"
#include "env.h"
#include "object.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bowie_pg_conn_finish(void *conn) {
    if (conn)
        PQfinish((PGconn *)conn);
}

#define ARG(n)    (args->count > (n) ? args->items[n] : BOWIE_NULL)
#define NARGS     (args->count)
#define REQUIRE(n, name) \
    if (NARGS < (n)) return obj_errorf(name "() requires %d argument(s)", n)

static Object *pg_err_conn(PGconn *c) {
    char buf[640];
    snprintf(buf, sizeof(buf), "pg_connect: %s", PQerrorMessage(c));
    return obj_errorf("%s", buf);
}

static Object *pg_err_result(PGresult *res) {
    const char *msg = PQresultErrorMessage(res);
    char buf[640];
    snprintf(buf, sizeof(buf), "pg_query: %s", msg ? msg : "unknown error");
    return obj_errorf("%s", buf);
}

static Object *bw_pg_connect(ObjList *args) {
    REQUIRE(1, "pg_connect");
    if (ARG(0)->type != OBJ_STRING)
        return obj_errorf("pg_connect(): connection info must be a string");
    PGconn *c = PQconnectdb(ARG(0)->string.str);
    if (!c)
        return obj_errorf("pg_connect: PQconnectdb returned null");
    if (PQstatus(c) != CONNECTION_OK) {
        Object *e = pg_err_conn(c);
        PQfinish(c);
        return e;
    }
    return obj_pg_conn(c);
}

static Object *bw_pg_close(ObjList *args) {
    REQUIRE(1, "pg_close");
    if (ARG(0)->type != OBJ_PG_CONN)
        return obj_errorf("pg_close(): expected pg_conn");
    Object *o = ARG(0);
    if (o->pg.conn) {
        bowie_pg_conn_finish(o->pg.conn);
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
        hash_set(h, "command", obj_string(PQcmdStatus(res)));
        const char *t = PQcmdTuples(res);
        long long n   = 0;
        if (t && t[0]) {
            char *end;
            n = strtoll(t, &end, 10);
            if (end == t) n = 0;
        }
        hash_set(h, "rows", obj_int(n));
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
                if (PQgetisnull(res, i, j))
                    hash_set(row, name, obj_null());
                else {
                    const char *val = PQgetvalue(res, i, j);
                    hash_set(row, name, obj_string(val ? val : ""));
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

static Object *bw_pg_query(ObjList *args) {
    REQUIRE(2, "pg_query");
    if (ARG(0)->type != OBJ_PG_CONN)
        return obj_errorf("pg_query(): first argument must be pg_conn");
    if (ARG(1)->type != OBJ_STRING)
        return obj_errorf("pg_query(): SQL must be a string");
    PGconn *c = (PGconn *)ARG(0)->pg.conn;
    if (!c)
        return obj_errorf("pg_query(): connection is closed");
    if (PQstatus(c) != CONNECTION_OK)
        return obj_errorf("pg_query(): connection is not OK: %s", PQerrorMessage(c));

    PGresult *res = PQexec(c, ARG(1)->string.str);
    if (!res)
        return obj_errorf("pg_query(): PQexec failed: %s", PQerrorMessage(c));
    return result_to_object(res);
}

void postgres_register(Env *env) {
    Object *tmp;
#define REG(name, fn) env_set(env, name, (tmp = obj_builtin(fn, name))); obj_release(tmp)

    REG("pg_connect", bw_pg_connect);
    REG("pg_close", bw_pg_close);
    REG("pg_query", bw_pg_query);
#undef REG
}
