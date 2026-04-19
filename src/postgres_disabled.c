#include "postgres.h"
#include "env.h"
#include "object.h"

void bowie_pg_conn_finish(void *conn) {
    (void)conn;
}

static Object *bw_pg_disabled(ObjList *args) {
    (void)args;
    return obj_errorf(
        "PostgreSQL support was not compiled in (install libpq and rebuild)");
}

void postgres_register(Env *env) {
    Object *tmp;
#define REG(name, fn) env_set(env, name, (tmp = obj_builtin(fn, name))); obj_release(tmp)
    REG("pg_connect", bw_pg_disabled);
    REG("pg_close", bw_pg_disabled);
    REG("pg_query", bw_pg_disabled);
#undef REG
}
