#include "event_loop.h"

#ifndef _WIN32

#include <stdlib.h>
#include <string.h>
#include <poll.h>

#ifdef BOWIE_CURL
#include <curl/curl.h>
#endif

EventLoop *g_event_loop = NULL;

void event_loop_init(void) {
    if (g_event_loop) return;
    g_event_loop = calloc(1, sizeof(EventLoop));
#ifdef BOWIE_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_event_loop->curl_multi = curl_multi_init();
#endif
}

void event_loop_free(void) {
    if (!g_event_loop) return;
#ifdef BOWIE_CURL
    if (g_event_loop->curl_multi)
        curl_multi_cleanup((CURLM *)g_event_loop->curl_multi);
    curl_global_cleanup();
#endif
    free(g_event_loop->ready);
    free(g_event_loop->watchers);
    free(g_event_loop);
    g_event_loop = NULL;
}

void event_loop_watch_fd(int fd, FdCallback cb, void *ctx) {
    if (!g_event_loop) return;
    if (g_event_loop->watcher_count >= g_event_loop->watcher_cap) {
        g_event_loop->watcher_cap = g_event_loop->watcher_cap ? g_event_loop->watcher_cap * 2 : 16;
        g_event_loop->watchers = realloc(g_event_loop->watchers,
                                         g_event_loop->watcher_cap * sizeof(FdWatcher));
    }
    FdWatcher *w = &g_event_loop->watchers[g_event_loop->watcher_count++];
    w->fd  = fd;
    w->cb  = cb;
    w->ctx = ctx;
}

void event_loop_unwatch_fd(int fd) {
    if (!g_event_loop) return;
    for (int i = 0; i < g_event_loop->watcher_count; i++) {
        if (g_event_loop->watchers[i].fd == fd) {
            /* swap-remove */
            g_event_loop->watchers[i] =
                g_event_loop->watchers[--g_event_loop->watcher_count];
            return;
        }
    }
}

void event_loop_add_server(void) {
    if (g_event_loop) g_event_loop->active_servers++;
}

void event_loop_remove_server(void) {
    if (g_event_loop && g_event_loop->active_servers > 0)
        g_event_loop->active_servers--;
}

void event_loop_enqueue(Coro *c) {
    if (!g_event_loop) return;
    c->state = CORO_READY;
    if (g_event_loop->ready_count >= g_event_loop->ready_cap) {
        g_event_loop->ready_cap = g_event_loop->ready_cap ? g_event_loop->ready_cap * 2 : 8;
        g_event_loop->ready = realloc(g_event_loop->ready,
                                      g_event_loop->ready_cap * sizeof(Coro *));
    }
    g_event_loop->ready[g_event_loop->ready_count++] = c;
}

int event_loop_has_work(void) {
    if (!g_event_loop) return 0;
    if (g_event_loop->active_servers > 0) return 1;
    if (g_event_loop->ready_count > 0) return 1;
    if (g_event_loop->watcher_count > 0) return 1;
#ifdef BOWIE_CURL
    if (g_event_loop->pending_io > 0) return 1;
#endif
    return 0;
}

/* ---- Promise helpers ---- */

void promise_add_waiter(Object *p, Coro *c) {
    if (!p || p->type != OBJ_PROMISE) return;
    if (p->promise.waiter_count >= p->promise.waiter_cap) {
        p->promise.waiter_cap = p->promise.waiter_cap ? p->promise.waiter_cap * 2 : 4;
        p->promise.waiters = realloc(p->promise.waiters,
                                     p->promise.waiter_cap * sizeof(void *));
    }
    p->promise.waiters[p->promise.waiter_count++] = (void *)c;
}

void promise_resolve(Object *p, Object *val) {
    if (!p || p->type != OBJ_PROMISE) return;
    if (p->promise.state != 0) return; /* already settled */
    p->promise.state = 1; /* FULFILLED */
    p->promise.value = val;
    if (val) obj_retain(val);
    /* Wake up all waiters */
    for (int i = 0; i < p->promise.waiter_count; i++) {
        Coro *c = (Coro *)p->promise.waiters[i];
        c->result = val;
        if (val) obj_retain(val);
        event_loop_enqueue(c);
    }
    p->promise.waiter_count = 0;
}

void promise_reject(Object *p, Object *err) {
    if (!p || p->type != OBJ_PROMISE) return;
    if (p->promise.state != 0) return;
    p->promise.state = 2; /* REJECTED */
    p->promise.value = err;
    if (err) obj_retain(err);
    for (int i = 0; i < p->promise.waiter_count; i++) {
        Coro *c = (Coro *)p->promise.waiters[i];
        c->result = err;
        if (err) obj_retain(err);
        event_loop_enqueue(c);
    }
    p->promise.waiter_count = 0;
}

/* ---- Curl polling ---- */
#ifdef BOWIE_CURL

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} FetchBuf;

typedef struct {
    Object   *promise;
    FetchBuf  body;
    Object   *headers;
    struct curl_slist *hlist;
} FetchState;

static size_t fetch_body_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
    FetchBuf *b = (FetchBuf *)ud;
    size_t n = sz * nmemb;
    if (b->len + n + 1 >= b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

#include <ctype.h>

static size_t fetch_header_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
    Object *hdrs = (Object *)ud;
    size_t n = sz * nmemb;
    if (n < 2 || ptr[0] == '\r' || strncmp(ptr, "HTTP/", 5) == 0) return n;
    char *colon = memchr(ptr, ':', n);
    if (!colon) return n;
    int klen = (int)(colon - ptr);
    char key[256];
    if (klen >= (int)sizeof(key)) return n;
    memcpy(key, ptr, klen); key[klen] = '\0';
    for (int i = 0; key[i]; i++) key[i] = (char)tolower((unsigned char)key[i]);
    const char *val = colon + 1;
    while (*val == ' ') val++;
    int vlen = (int)(ptr + n - val);
    while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == '\n')) vlen--;
    char val_buf[2048];
    if (vlen < (int)sizeof(val_buf)) {
        memcpy(val_buf, val, vlen); val_buf[vlen] = '\0';
        Object *vs = obj_string(val_buf);
        hash_set(hdrs, key, vs);
        obj_release(vs);
    }
    return n;
}

/* Called by http.c to add an async fetch to the event loop */
Object *event_loop_fetch_async(const char *url, const char *method,
                               Object *req_headers, const char *body) {
    if (!g_event_loop || !g_event_loop->curl_multi)
        return obj_errorf("fetch: event loop not initialised");

    Object *promise = obj_promise();

    FetchState *fs = calloc(1, sizeof(FetchState));
    fs->promise       = promise;
    obj_retain(promise);
    fs->body.data = malloc(4096);
    fs->body.data[0] = '\0';
    fs->body.len  = 0;
    fs->body.cap  = 4096;
    fs->headers   = obj_hash();

    CURL *easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, fetch_body_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &fs->body);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, fetch_header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, fs->headers);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, fs);

    if (strcmp(method, "POST") == 0) curl_easy_setopt(easy, CURLOPT_POST, 1L);
    else if (strcmp(method, "PUT") == 0) curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
    else if (strcmp(method, "GET") != 0)
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);

    if (body) {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }

    struct curl_slist *hlist = NULL;
    if (req_headers && req_headers->type == OBJ_HASH) {
        for (int i = 0; i < 64; i++) {
            for (HashEntry *e = req_headers->hash.buckets[i]; e; e = e->next) {
                char *v = obj_inspect(e->value);
                char line[1024];
                snprintf(line, sizeof(line), "%s: %s", e->key, v);
                free(v);
                hlist = curl_slist_append(hlist, line);
            }
        }
    }
    if (hlist) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hlist);
    fs->hlist = hlist;

    curl_multi_add_handle((CURLM *)g_event_loop->curl_multi, easy);
    g_event_loop->pending_io++;

    return promise;
}

static void poll_curl(void) {
    if (!g_event_loop->curl_multi) return;
    CURLM *multi = (CURLM *)g_event_loop->curl_multi;
    int running = 0;
    curl_multi_perform(multi, &running);

    CURLMsg *msg;
    int msgs_left;
    while ((msg = curl_multi_info_read(multi, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE) continue;
        CURL *easy = msg->easy_handle;
        FetchState *fs = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &fs);

        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

        Object *resp     = obj_hash();
        Object *status_o = obj_int(status);
        Object *ok_o     = obj_bool(status >= 200 && status < 300);
        Object *body_o   = obj_string(fs->body.data);
        hash_set(resp, "status",  status_o);
        hash_set(resp, "ok",      ok_o);
        hash_set(resp, "headers", fs->headers);
        hash_set(resp, "body",    body_o);
        obj_release(status_o); obj_release(ok_o);
        obj_release(body_o); obj_release(fs->headers);

        promise_resolve(fs->promise, resp);
        obj_release(resp);
        obj_release(fs->promise);
        free(fs->body.data);
        if (fs->hlist) curl_slist_free_all(fs->hlist);
        free(fs);

        curl_multi_remove_handle(multi, easy);
        curl_easy_cleanup(easy);
        g_event_loop->pending_io--;
    }
}
#endif /* BOWIE_CURL */

void event_loop_run(void) {
    if (!g_event_loop) return;
    while (event_loop_has_work()) {
        /* drain the ready queue */
        while (g_event_loop->ready_count > 0) {
            Coro *c = g_event_loop->ready[0];
            g_event_loop->ready_count--;
            if (g_event_loop->ready_count > 0)
                memmove(g_event_loop->ready, g_event_loop->ready + 1,
                        g_event_loop->ready_count * sizeof(Coro *));

            coro_resume(c);

            if (c->state == CORO_DONE) {
                if (c->promise)
                    promise_resolve(c->promise, c->result);
                coro_free(c);
            }
        }

        /* poll watched FDs (HTTP listeners, client sockets, PG sockets) */
        int nw = g_event_loop->watcher_count;
        if (nw > 0) {
            struct pollfd *pfds = malloc(nw * sizeof(struct pollfd));
            for (int i = 0; i < nw; i++) {
                pfds[i].fd      = g_event_loop->watchers[i].fd;
                pfds[i].events  = POLLIN;
                pfds[i].revents = 0;
            }
            int timeout_ms = (g_event_loop->ready_count > 0) ? 0 : 1;
            poll(pfds, nw, timeout_ms);
            /* iterate a snapshot — callbacks may mutate watchers[] */
            for (int i = 0; i < nw; i++) {
                if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                    /* find the watcher again in case array shifted */
                    for (int j = 0; j < g_event_loop->watcher_count; j++) {
                        if (g_event_loop->watchers[j].fd == pfds[i].fd) {
                            g_event_loop->watchers[j].cb(pfds[i].fd,
                                                         g_event_loop->watchers[j].ctx);
                            break;
                        }
                    }
                }
            }
            free(pfds);
        } else if (g_event_loop->active_servers > 0) {
            /* nothing to poll but a server is running — brief yield */
            struct pollfd dummy; dummy.fd = -1; dummy.events = 0;
            poll(&dummy, 0, 1);
        }

#ifdef BOWIE_CURL
        poll_curl();
#endif
    }
}

#endif /* _WIN32 */
