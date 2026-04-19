#ifndef BOWIE_EVENT_LOOP_H
#define BOWIE_EVENT_LOOP_H

#include "object.h"

#ifndef _WIN32
#include "coro.h"

typedef struct {
    Coro  **ready;
    int     ready_count;
    int     ready_cap;
#ifdef BOWIE_CURL
    void   *curl_multi; /* CURLM * */
    int     pending_io;
#endif
} EventLoop;

extern EventLoop *g_event_loop;

void event_loop_init(void);
void event_loop_free(void);
void event_loop_enqueue(Coro *c);
void event_loop_run(void);
int  event_loop_has_work(void);

void promise_resolve(Object *p, Object *val);
void promise_reject(Object *p, Object *err);
void promise_add_waiter(Object *p, Coro *c);

#ifdef BOWIE_CURL
Object *event_loop_fetch_async(const char *url, const char *method,
                               Object *req_headers, const char *body);
#endif

#endif /* _WIN32 */
#endif /* BOWIE_EVENT_LOOP_H */
