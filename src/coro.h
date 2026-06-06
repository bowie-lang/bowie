#ifndef BOWIE_CORO_H
#define BOWIE_CORO_H

#ifndef _WIN32

#include <ucontext.h>
#include "object.h"

#define CORO_STACK_SIZE (512 * 1024)

typedef enum {
    CORO_READY,
    CORO_RUNNING,
    CORO_SUSPENDED,
    CORO_DONE,
} CoroState;

struct Coro;
typedef void (*CoroCFunc)(struct Coro *self, void *arg);

typedef struct Coro {
    ucontext_t   ctx;
    char        *stack;
    CoroState    state;
    Object      *result;      /* value to resume with (set by promise_resolve) */
    Object      *promise;     /* OBJ_PROMISE this coro fulfills when done */
    struct Coro *sched_next;

    /* call info — Bowie-function path */
    struct Interpreter *interp;
    Object             *fn_obj;
    ObjList             call_args;

    /* C-function coroutine path (mutually exclusive with fn_obj) */
    CoroCFunc   c_func;
    void       *c_arg;
} Coro;

extern ucontext_t  g_scheduler_ctx;
extern Coro       *g_running_coro;

Coro *coro_new(struct Interpreter *it, Object *fn_obj, ObjList *args, Object *promise);
Coro *coro_new_c(CoroCFunc fn, void *arg);
void  coro_free(Coro *c);
void  coro_resume(Coro *c);
void  coro_yield(void);

#endif /* _WIN32 */
#endif /* BOWIE_CORO_H */
