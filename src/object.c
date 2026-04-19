#include "object.h"
#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in postgres.c — tears down libpq connection without pulling libpq into this TU. */
void bowie_pg_conn_finish(void *conn);

Object *BOWIE_NULL     = NULL;
Object *BOWIE_TRUE     = NULL;
Object *BOWIE_FALSE    = NULL;
Object *BOWIE_BREAK    = NULL;
Object *BOWIE_CONTINUE = NULL;

void objects_init(void) {
    BOWIE_NULL        = calloc(1, sizeof(Object));
    BOWIE_NULL->type  = OBJ_NULL;
    BOWIE_NULL->refs  = 1000; /* permanent */

    BOWIE_TRUE           = calloc(1, sizeof(Object));
    BOWIE_TRUE->type     = OBJ_BOOL;
    BOWIE_TRUE->bool_val = 1;
    BOWIE_TRUE->refs     = 1000;

    BOWIE_FALSE           = calloc(1, sizeof(Object));
    BOWIE_FALSE->type     = OBJ_BOOL;
    BOWIE_FALSE->bool_val = 0;
    BOWIE_FALSE->refs     = 1000;

    BOWIE_BREAK        = calloc(1, sizeof(Object));
    BOWIE_BREAK->type  = OBJ_BREAK;
    BOWIE_BREAK->refs  = 1000;

    BOWIE_CONTINUE        = calloc(1, sizeof(Object));
    BOWIE_CONTINUE->type  = OBJ_CONTINUE;
    BOWIE_CONTINUE->refs  = 1000;
}

static Object *obj_new(ObjType type) {
    Object *o = calloc(1, sizeof(Object));
    o->type   = type;
    o->refs   = 1;
    return o;
}

Object *obj_int(long long val) {
    Object *o    = obj_new(OBJ_INT);
    o->int_val   = val;
    return o;
}

Object *obj_float(double val) {
    Object *o    = obj_new(OBJ_FLOAT);
    o->float_val = val;
    return o;
}

Object *obj_string(const char *str) {
    Object *o    = obj_new(OBJ_STRING);
    o->string.str = strdup(str);
    return o;
}

Object *obj_bool(int val) {
    return val ? BOWIE_TRUE : BOWIE_FALSE;
}

Object *obj_null(void) { return BOWIE_NULL; }

Object *obj_array(void) {
    Object *o = obj_new(OBJ_ARRAY);
    objlist_init(&o->array.elems);
    return o;
}

Object *obj_hash(void) {
    Object *o = obj_new(OBJ_HASH);
    memset(o->hash.buckets, 0, sizeof(o->hash.buckets));
    o->hash.size = 0;
    return o;
}

Object *obj_function(char **params, int pc, Node *body, Env *closure, const char *name) {
    Object *o        = obj_new(OBJ_FUNCTION);
    o->fn.params     = params;
    o->fn.param_count= pc;
    o->fn.body       = body;
    o->fn.closure    = closure;
    o->fn.name       = name ? strdup(name) : NULL;
    if (closure) env_retain(closure);
    return o;
}

Object *obj_builtin(BuiltinFn fn, const char *name) {
    Object *o       = obj_new(OBJ_BUILTIN);
    o->builtin.fn   = fn;
    o->builtin.name = name;
    return o;
}

Object *obj_return(Object *val) {
    Object *o   = obj_new(OBJ_RETURN);
    o->ret.value= val;
    obj_retain(val);
    return o;
}

Object *obj_break(void)    { return BOWIE_BREAK; }
Object *obj_continue(void) { return BOWIE_CONTINUE; }

Object *obj_errorf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Object *o     = obj_new(OBJ_ERROR);
    o->error.msg  = strdup(buf);
    return o;
}

Object *obj_http_server(int port, Object *listen_cb) {
    Object *o        = obj_new(OBJ_HTTP_SERVER);
    o->server.port   = port;
    o->server.fd     = -1;
    o->server.routes = NULL;
    o->server.listen_cb = listen_cb;
    if (listen_cb) obj_retain(listen_cb);
    return o;
}

Object *obj_pg_conn(void *pgconn) {
    Object *o   = obj_new(OBJ_PG_CONN);
    o->pg.conn = pgconn;
    return o;
}

void obj_retain(Object *o) {
    if (o && o->refs < 1000) o->refs++;
}

void obj_release(Object *o) {
    if (!o || o->refs >= 1000) return;
    if (--o->refs > 0) return;

    switch (o->type) {
        case OBJ_STRING:
            free(o->string.str);
            break;
        case OBJ_ARRAY:
            for (int i = 0; i < o->array.elems.count; i++)
                obj_release(o->array.elems.items[i]);
            free(o->array.elems.items);
            break;
        case OBJ_HASH:
            for (int i = 0; i < 64; i++) {
                HashEntry *e = o->hash.buckets[i];
                while (e) {
                    HashEntry *next = e->next;
                    free(e->key);
                    obj_release(e->value);
                    free(e);
                    e = next;
                }
            }
            break;
        case OBJ_FUNCTION:
            for (int i = 0; i < o->fn.param_count; i++) free(o->fn.params[i]);
            free(o->fn.params);
            free(o->fn.name);
            if (o->fn.closure) env_release(o->fn.closure);
            break;
        case OBJ_RETURN:
            obj_release(o->ret.value);
            break;
        case OBJ_ERROR:
            free(o->error.msg);
            break;
        case OBJ_HTTP_SERVER: {
            if (o->server.listen_cb) obj_release(o->server.listen_cb);
            Route *r = o->server.routes;
            while (r) {
                Route *next = r->next;
                free(r->method);
                free(r->path);
                if (r->query_key) free(r->query_key);
                obj_release(r->handler);
                free(r);
                r = next;
            }
            break;
        }
        case OBJ_PG_CONN:
            if (o->pg.conn) {
                bowie_pg_conn_finish(o->pg.conn);
                o->pg.conn = NULL;
            }
            break;
        default: break;
    }
    free(o);
}

/* ---- Array ---- */
void objlist_init(ObjList *l) { l->items = NULL; l->count = l->cap = 0; }

void objlist_push(ObjList *l, Object *o) {
    if (l->count >= l->cap) {
        l->cap   = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, l->cap * sizeof(Object *));
    }
    l->items[l->count++] = o;
}

void objlist_free(ObjList *l) {
    for (int i = 0; i < l->count; i++) obj_release(l->items[i]);
    free(l->items);
}

void array_push(Object *arr, Object *val) {
    obj_retain(val);
    objlist_push(&arr->array.elems, val);
}

Object *array_get(Object *arr, int idx) {
    if (idx < 0) idx = arr->array.elems.count + idx;
    if (idx < 0 || idx >= arr->array.elems.count) return BOWIE_NULL;
    return arr->array.elems.items[idx];
}

/* ---- Hash ---- */
static unsigned int hash_key(const char *key) {
    unsigned int h = 5381;
    while (*key) h = ((h << 5) + h) + (unsigned char)*key++;
    return h % 64;
}

void hash_set(Object *h, const char *key, Object *val) {
    unsigned int idx = hash_key(key);
    HashEntry *e = h->hash.buckets[idx];
    while (e) {
        if (!strcmp(e->key, key)) {
            obj_release(e->value);
            e->value = val;
            obj_retain(val);
            return;
        }
        e = e->next;
    }
    e        = malloc(sizeof(HashEntry));
    e->key   = strdup(key);
    e->value = val;
    obj_retain(val);
    e->next  = h->hash.buckets[idx];
    h->hash.buckets[idx] = e;
    h->hash.size++;
}

Object *hash_get(Object *h, const char *key) {
    unsigned int idx = hash_key(key);
    HashEntry *e = h->hash.buckets[idx];
    while (e) {
        if (!strcmp(e->key, key)) return e->value;
        e = e->next;
    }
    return BOWIE_NULL;
}

char **hash_keys(Object *h, int *out_count) {
    char **keys = malloc(h->hash.size * sizeof(char *));
    int n = 0;
    for (int i = 0; i < 64; i++) {
        HashEntry *e = h->hash.buckets[i];
        while (e) { keys[n++] = e->key; e = e->next; }
    }
    *out_count = n;
    return keys;
}

/* ---- Utilities ---- */
char *obj_inspect(Object *o) {
    if (!o) return strdup("null");
    char buf[256];
    switch (o->type) {
        case OBJ_INT:
            snprintf(buf, sizeof(buf), "%lld", o->int_val);
            return strdup(buf);
        case OBJ_FLOAT:
            snprintf(buf, sizeof(buf), "%g", o->float_val);
            return strdup(buf);
        case OBJ_STRING:
            return strdup(o->string.str);
        case OBJ_BOOL:
            return strdup(o->bool_val ? "true" : "false");
        case OBJ_NULL:
            return strdup("null");
        case OBJ_ARRAY: {
            /* build "[a, b, c]" */
            int   sz  = 2;
            char *out = malloc(sz);
            out[0]    = '['; out[1] = '\0';
            for (int i = 0; i < o->array.elems.count; i++) {
                char *s = obj_inspect(o->array.elems.items[i]);
                int  sl = strlen(s);
                out = realloc(out, sz + sl + 3);
                strcat(out, s);
                free(s);
                sz += sl + 3;
                if (i < o->array.elems.count - 1) strcat(out, ", ");
            }
            strcat(out, "]");
            return out;
        }
        case OBJ_HASH: {
            int   sz  = 2;
            char *out = malloc(sz);
            out[0]    = '{'; out[1] = '\0';
            int first = 1;
            for (int i = 0; i < 64; i++) {
                HashEntry *e = o->hash.buckets[i];
                while (e) {
                    char *v = obj_inspect(e->value);
                    int   l = strlen(e->key) + strlen(v) + 6;
                    out = realloc(out, sz + l);
                    if (!first) strcat(out, ", ");
                    strcat(out, "\"");
                    strcat(out, e->key);
                    strcat(out, "\": ");
                    strcat(out, v);
                    free(v);
                    sz += l; first = 0;
                    e = e->next;
                }
            }
            strcat(out, "}");
            return out;
        }
        case OBJ_FUNCTION:
            snprintf(buf, sizeof(buf), "<fn %s>", o->fn.name ? o->fn.name : "anonymous");
            return strdup(buf);
        case OBJ_BUILTIN:
            snprintf(buf, sizeof(buf), "<builtin %s>", o->builtin.name);
            return strdup(buf);
        case OBJ_RETURN: {
            char *inner = obj_inspect(o->ret.value);
            snprintf(buf, sizeof(buf), "<return %s>", inner);
            free(inner);
            return strdup(buf);
        }
        case OBJ_ERROR:
            snprintf(buf, sizeof(buf), "Error: %s", o->error.msg);
            return strdup(buf);
        case OBJ_HTTP_SERVER:
            snprintf(buf, sizeof(buf), "<server port=%d>", o->server.port);
            return strdup(buf);
        case OBJ_PG_CONN:
            return strdup(o->pg.conn ? "<pg_conn>" : "<pg_conn closed>");
        case OBJ_BREAK:
            return strdup("break");
        case OBJ_CONTINUE:
            return strdup("continue");
    }
    return strdup("unknown");
}

const char *obj_type_name(ObjType t) {
    switch (t) {
        case OBJ_INT:         return "int";
        case OBJ_FLOAT:       return "float";
        case OBJ_STRING:      return "string";
        case OBJ_BOOL:        return "bool";
        case OBJ_NULL:        return "null";
        case OBJ_ARRAY:       return "array";
        case OBJ_HASH:        return "hash";
        case OBJ_FUNCTION:    return "function";
        case OBJ_BUILTIN:     return "builtin";
        case OBJ_RETURN:      return "return";
        case OBJ_BREAK:       return "break";
        case OBJ_CONTINUE:    return "continue";
        case OBJ_ERROR:       return "error";
        case OBJ_HTTP_SERVER: return "server";
        case OBJ_PG_CONN:     return "pg_conn";
    }
    return "unknown";
}

int obj_is_truthy(Object *o) {
    if (!o || o->type == OBJ_NULL)  return 0;
    if (o->type == OBJ_BOOL)        return o->bool_val;
    if (o->type == OBJ_INT)         return o->int_val != 0;
    if (o->type == OBJ_FLOAT)       return o->float_val != 0.0;
    if (o->type == OBJ_STRING)      return o->string.str[0] != '\0';
    if (o->type == OBJ_PG_CONN)     return o->pg.conn != NULL;
    return 1;
}

int obj_equals(Object *a, Object *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) {
        /* int == float comparison */
        if (a->type == OBJ_INT && b->type == OBJ_FLOAT)
            return (double)a->int_val == b->float_val;
        if (a->type == OBJ_FLOAT && b->type == OBJ_INT)
            return a->float_val == (double)b->int_val;
        return 0;
    }
    switch (a->type) {
        case OBJ_INT:    return a->int_val == b->int_val;
        case OBJ_FLOAT:  return a->float_val == b->float_val;
        case OBJ_STRING: return !strcmp(a->string.str, b->string.str);
        case OBJ_BOOL:   return a->bool_val == b->bool_val;
        case OBJ_NULL:   return 1;
        default:         return a == b;
    }
}
