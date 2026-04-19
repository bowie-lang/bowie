#include "coro.h"

#ifndef _WIN32

/* macOS marks ucontext_t as deprecated since 10.6 but it still works */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "interpreter.h"
#include <stdlib.h>
#include <string.h>

ucontext_t  g_scheduler_ctx;
Coro       *g_running_coro = NULL;

static void coro_trampoline(void) {
    Coro *self = g_running_coro;
    Object *result = interp_call_fn(self->interp, self->fn_obj, &self->call_args);
    /* Unwrap OBJ_RETURN if needed */
    if (result && result->type == OBJ_RETURN) {
        Object *v = result->ret.value;
        obj_retain(v);
        obj_release(result);
        result = v;
    }
    self->result = result; /* event loop will resolve the promise */
    self->state  = CORO_DONE;
    swapcontext(&self->ctx, &g_scheduler_ctx);
    /* Should never reach here */
}

Coro *coro_new(struct Interpreter *it, Object *fn_obj, ObjList *args, Object *promise) {
    Coro *c    = calloc(1, sizeof(Coro));
    c->stack   = malloc(CORO_STACK_SIZE);
    c->state   = CORO_READY;
    c->promise = promise;
    c->interp  = it;
    c->fn_obj  = fn_obj;
    obj_retain(fn_obj);
    obj_retain(promise);

    objlist_init(&c->call_args);
    for (int i = 0; i < args->count; i++) {
        obj_retain(args->items[i]);
        objlist_push(&c->call_args, args->items[i]);
    }

    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp   = c->stack;
    c->ctx.uc_stack.ss_size = CORO_STACK_SIZE;
    c->ctx.uc_link          = NULL;
    makecontext(&c->ctx, coro_trampoline, 0);

    return c;
}

void coro_free(Coro *c) {
    if (!c) return;
    free(c->stack);
    if (c->fn_obj)  obj_release(c->fn_obj);
    if (c->promise) obj_release(c->promise);
    if (c->result)  obj_release(c->result);
    objlist_free(&c->call_args);
    free(c);
}

void coro_resume(Coro *c) {
    Coro *prev  = g_running_coro;
    g_running_coro = c;
    c->state = CORO_RUNNING;
    swapcontext(&g_scheduler_ctx, &c->ctx);
    g_running_coro = prev;
}

void coro_yield(void) {
    Coro *self = g_running_coro;
    if (!self) return;
    self->state = CORO_SUSPENDED;
    swapcontext(&self->ctx, &g_scheduler_ctx);
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* _WIN32 */
