#ifndef BOWIE_OBJECT_H
#define BOWIE_OBJECT_H

#include "ast.h"
#include <stdarg.h>

typedef struct Object    Object;
typedef struct Env       Env;
typedef struct ObjList   ObjList;
typedef struct HashEntry HashEntry;

typedef enum {
    OBJ_INT,
    OBJ_FLOAT,
    OBJ_STRING,
    OBJ_BOOL,
    OBJ_NULL,
    OBJ_ARRAY,
    OBJ_HASH,
    OBJ_FUNCTION,
    OBJ_BUILTIN,
    OBJ_RETURN,
    OBJ_BREAK,
    OBJ_CONTINUE,
    OBJ_ERROR,
    OBJ_HTTP_SERVER,
    OBJ_PG_CONN,
    OBJ_PROMISE,
} ObjType;

struct ObjList {
    Object **items;
    int      count;
    int      cap;
};

struct HashEntry {
    char        *key;
    Object      *value;
    HashEntry   *next;
};

typedef Object *(*BuiltinFn)(ObjList *args);

typedef struct Route {
    char        *method;
    char        *path;
    char        *query_key; /* NULL: match path only. Else path is base (e.g. /users) and this key must appear in ?query */
    Object      *handler;
    struct Route *next;
} Route;

struct Object {
    ObjType type;
    int     refs;

    union {
        long long int_val;
        double    float_val;
        int       bool_val;

        struct { char *str; }                                   string;
        struct { ObjList elems; }                               array;
        struct { HashEntry *buckets[64]; int size; }            hash;
        struct {
            char  **params;
            int     param_count;
            Node   *body;
            Env    *closure;
            char   *name;
            int     is_async;
            int     has_rest;
        } fn;
        struct { BuiltinFn fn; const char *name; }             builtin;
        struct { Object *value; }                               ret;
        struct { char *type; char *msg; }                       error;
        struct { int port; int fd; Route *routes; Object *listen_cb; } server;
        struct { void *conn; /* PGconn * when libpq is linked */ } pg;
        struct {
            int    state;          /* 0=PENDING 1=FULFILLED 2=REJECTED */
            Object *value;
            void  **waiters;       /* Coro * array (opaque to avoid circular dep) */
            int     waiter_count;
            int     waiter_cap;
        } promise;
    };
};

/* Constructors */
Object *obj_int(long long val);
Object *obj_float(double val);
Object *obj_string(const char *str);
Object *obj_bool(int val);
Object *obj_null(void);
Object *obj_array(void);
Object *obj_hash(void);
Object *obj_function(char **params, int pc, Node *body, Env *closure, const char *name, int is_async, int has_rest);
Object *obj_promise(void);
Object *obj_builtin(BuiltinFn fn, const char *name);
Object *obj_return(Object *val);
Object *obj_break(void);
Object *obj_continue(void);
Object *obj_errorf(const char *fmt, ...);
Object *obj_error_typef(const char *type, const char *fmt, ...);
Object *obj_http_server(int port, Object *listen_cb);
Object *obj_pg_conn(void *pgconn);

/* Reference counting */
void obj_retain(Object *o);
void obj_release(Object *o);

/* Array helpers */
void    array_push(Object *arr, Object *val);
Object *array_get(Object *arr, int idx);

/* Hash helpers */
void    hash_set(Object *h, const char *key, Object *val);
Object *hash_get(Object *h, const char *key);
char  **hash_keys(Object *h, int *out_count);

/* Utilities */
char       *obj_inspect(Object *o);
const char *obj_type_name(ObjType t);
int         obj_is_truthy(Object *o);
int         obj_equals(Object *a, Object *b);

/* ObjList helpers */
void objlist_init(ObjList *l);
void objlist_push(ObjList *l, Object *o);
void objlist_free(ObjList *l);

/* Singletons */
extern Object *BOWIE_NULL;
extern Object *BOWIE_TRUE;
extern Object *BOWIE_FALSE;
extern Object *BOWIE_BREAK;
extern Object *BOWIE_CONTINUE;

void objects_init(void);

#endif
