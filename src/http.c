#include "http.h"
#include "env.h"
#include "event_loop.h"
#include "coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define close closesocket
#else
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <signal.h>
#endif

#define BUF_SIZE (1024 * 64)

static char *dup_cstr(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    return copy;
}

static int str_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb)) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* ---- Listen sockets (IPv4 + IPv6) ----
 * Tools that resolve "localhost" to ::1 first need an IPv6 listener; otherwise
 * the client can stall for a long fallback timeout when only 0.0.0.0 (IPv4) is bound.
 */
static int listen_ipv4(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&opt, sizeof(opt));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_ipv6(uint16_t port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&opt, sizeof(opt));
#endif
#ifdef IPV6_V6ONLY
    int v6only = 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
#endif
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- Request parsing ---- */
static void parse_query(Object *req, const char *query_str) {
    Object *query = obj_hash();
    char *copy    = dup_cstr(query_str);
    if (!copy) {
        hash_set(req, "query", query);
        obj_release(query);
        return;
    }
    char *saveptr = NULL;
    char *token   = strtok_r(copy, "&", &saveptr);
    while (token) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            Object *_v = obj_string(eq + 1);
            hash_set(query, token, _v);
            obj_release(_v);
        } else {
            Object *_v = obj_string("");
            hash_set(query, token, _v);
            obj_release(_v);
        }
        token = strtok_r(NULL, "&", &saveptr);
    }
    free(copy);
    hash_set(req, "query", query);
    obj_release(query);
}

static Object *parse_request(const char *raw) {
    Object *req  = obj_hash();
    Object *hdrs = obj_hash();

    /* Copy so we can tokenize */
    char *buf  = dup_cstr(raw);
    if (!buf) { obj_release(hdrs); obj_release(req); return NULL; }
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    if (!line) { free(buf); obj_release(hdrs); obj_release(req); return NULL; }

    /* Request line: METHOD path HTTP/1.x */
    char method[16] = {0}, path_raw[1024] = {0}, proto[16] = {0};
    sscanf(line, "%15s %1023s %15s", method, path_raw, proto);

    /* Split path and query string */
    char path[1024] = {0};
    char *qmark = strchr(path_raw, '?');
    if (qmark) {
        int plen = qmark - path_raw;
        if (plen >= (int)sizeof(path)) plen = sizeof(path) - 1;
        strncpy(path, path_raw, plen);
        path[plen] = '\0';
        parse_query(req, qmark + 1);
    } else {
        strncpy(path, path_raw, sizeof(path) - 1);
        parse_query(req, "");
    }

    Object *_method = obj_string(method);
    hash_set(req, "method", _method);
    obj_release(_method);
    Object *_path = obj_string(path);
    hash_set(req, "path", _path);
    obj_release(_path);

    /* Headers */
    while ((line = strtok_r(NULL, "\r\n", &saveptr)) && line[0] != '\0') {
        char *colon = strchr(line, ':');
        if (!colon) break;
        *colon = '\0';
        const char *val = colon + 1;
        while (*val == ' ') val++;
        /* lowercase the key */
        for (char *p = line; *p; p++) *p = tolower((unsigned char)*p);
        Object *_hv = obj_string(val);
        hash_set(hdrs, line, _hv);
        obj_release(_hv);
        *colon = ':';
    }
    hash_set(req, "headers", hdrs);
    obj_release(hdrs);

    /* Body — whatever remains after blank line */
    /* strtok already consumed \r\n pairs; find double \r\n in original */
    const char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start) body_start += 4;
    else body_start = "";
    Object *_body = obj_string(body_start);
    hash_set(req, "body", _body);
    obj_release(_body);

    free(buf);
    return req;
}

/* ---- Route matching ---- */
static int query_param_present(Object *query, const char *key) {
    if (!key || !query || query->type != OBJ_HASH) return 0;
    Object *v = hash_get(query, key);
    if (v == BOWIE_NULL || v->type == OBJ_NULL) return 0;
    if (v->type == OBJ_STRING && v->string.str[0] == '\0') return 0;
    return 1;
}

/* query_key routes (e.g. path pattern "/users:id" → base /users, key id) match before plain paths */
static Route *match_route(Object *server, const char *method, const char *path,
                          Object *query) {
    Route *r;
    r = server->server.routes;
    while (r) {
        if (r->query_key && str_eq_ci(r->method, method) && !strcmp(r->path, path)
            && query_param_present(query, r->query_key))
            return r;
        r = r->next;
    }
    r = server->server.routes;
    while (r) {
        if (!r->query_key && str_eq_ci(r->method, method) && !strcmp(r->path, path))
            return r;
        r = r->next;
    }
    r = server->server.routes;
    while (r) {
        if (!r->query_key && str_eq_ci(r->method, "*") && !strcmp(r->path, path)) return r;
        r = r->next;
    }
    r = server->server.routes;
    while (r) {
        if (!r->query_key && !strcmp(r->path, "*")) return r;
        r = r->next;
    }
    return NULL;
}

/* ---- Response building ---- */
static void send_response(int client_fd, int status, const char *body,
                           Object *headers_obj) {
    const char *status_text = "OK";
    if      (status == 201) status_text = "Created";
    else if (status == 204) status_text = "No Content";
    else if (status == 301) status_text = "Moved Permanently";
    else if (status == 302) status_text = "Found";
    else if (status == 400) status_text = "Bad Request";
    else if (status == 401) status_text = "Unauthorized";
    else if (status == 403) status_text = "Forbidden";
    else if (status == 404) status_text = "Not Found";
    else if (status == 405) status_text = "Method Not Allowed";
    else if (status == 409) status_text = "Conflict";
    else if (status == 422) status_text = "Unprocessable Entity";
    else if (status == 429) status_text = "Too Many Requests";
    else if (status == 500) status_text = "Internal Server Error";

    int body_len = body ? strlen(body) : 0;

    char header_buf[4096];
    int  hlen = snprintf(header_buf, sizeof(header_buf),
                         "HTTP/1.1 %d %s\r\n"
                         "Content-Length: %d\r\n"
                         "Connection: keep-alive\r\n"
                         "Keep-Alive: timeout=5\r\n",
                         status, status_text, body_len);

    /* Extra headers from response hash */
    if (headers_obj && headers_obj->type == OBJ_HASH) {
        for (int i = 0; i < 64 && hlen < (int)sizeof(header_buf) - 128; i++) {
            HashEntry *e = headers_obj->hash.buckets[i];
            while (e) {
                char *v = obj_inspect(e->value);
                hlen += snprintf(header_buf + hlen,
                                 sizeof(header_buf) - hlen,
                                 "%s: %s\r\n", e->key, v);
                free(v);
                e = e->next;
            }
        }
    }

    /* Default Content-Type if not set */
    if (!strstr(header_buf, "Content-Type")) {
        hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen,
                         "Content-Type: text/plain\r\n");
    }

    hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen, "\r\n");

    send(client_fd, header_buf, hlen, 0);
    if (body && body_len > 0) send(client_fd, body, body_len, 0);
}

/* ---- Call a Bowie handler ---- */
static void handle_request(int client_fd, Object *server,
                            Interpreter *it, Env *env,
                            const char *raw_req) {
    Object *req = parse_request(raw_req);
    if (!req) {
        send_response(client_fd, 400, "Bad Request", NULL);
        return;
    }

    Object *method_obj = hash_get(req, "method");
    Object *path_obj   = hash_get(req, "path");
    const char *method = method_obj->type == OBJ_STRING ? method_obj->string.str : "GET";
    const char *path   = path_obj->type   == OBJ_STRING ? path_obj->string.str   : "/";

    printf("[%s] %s\n", method, path);
    fflush(stdout);

    Object *query_obj = hash_get(req, "query");
    Route *route = match_route(server, method, path, query_obj);
    if (!route) {
        send_response(client_fd, 404, "Not Found", NULL);
        obj_release(req);
        return;
    }

    /* Call handler(req) */
    ObjList call_args;
    objlist_init(&call_args);
    obj_retain(req);
    objlist_push(&call_args, req);

    Object *result = NULL;
    if (route->handler->type == OBJ_FUNCTION) {
        Object *fn = route->handler;
        Env *fn_env = env_new(fn->fn.closure);
        if (fn->fn.param_count >= 1)
            env_set(fn_env, fn->fn.params[0], req);
        /* Eval body */
        result = interp_eval(it, fn->fn.body, fn_env);
        if (result && result->type == OBJ_RETURN) {
            Object *v = result->ret.value;
            obj_retain(v);
            obj_release(result);
            result = v;
        }
        env_release(fn_env);
    } else if (route->handler->type == OBJ_BUILTIN) {
        result = route->handler->builtin.fn(&call_args);
    }

    obj_release(req);
    for (int i = 0; i < call_args.count; i++) obj_release(call_args.items[i]);
    free(call_args.items);

    if (!result) { send_response(client_fd, 500, "Handler returned null", NULL); return; }

    if (result->type == OBJ_ERROR) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Internal error: %s", result->error.msg);
        send_response(client_fd, 500, buf, NULL);
        obj_release(result);
        return;
    }

    /* Result should be a hash: {status, body, headers?} or a plain string */
    if (result->type == OBJ_STRING) {
        send_response(client_fd, 200, result->string.str, NULL);
    } else if (result->type == OBJ_HASH) {
        Object *status_obj  = hash_get(result, "status");
        Object *body_obj    = hash_get(result, "body");
        Object *headers_obj = hash_get(result, "headers");

        int status = 200;
        if (status_obj && status_obj->type == OBJ_INT) status = (int)status_obj->int_val;

        const char *body = "";
        char *body_free = NULL;
        if (body_obj && body_obj->type == OBJ_STRING) {
            body = body_obj->string.str;
        } else if (body_obj != BOWIE_NULL) {
            body_free = obj_inspect(body_obj);
            body = body_free;
        }

        send_response(client_fd, status, body,
                      headers_obj != BOWIE_NULL ? headers_obj : NULL);
        free(body_free);
    } else {
        /* Fallback: stringify it */
        char *s = obj_inspect(result);
        send_response(client_fd, 200, s, NULL);
        free(s);
    }

    obj_release(result);
}

/* ---- Connection context for event-driven / coroutine handling ---- */
typedef struct {
    int          fd;
    char        *buf;
    int          buf_len;
    Object      *server;
    Interpreter *it;
    Env         *env;
} HttpConnCtx;

typedef struct {
    Object      *server;
    Interpreter *it;
    Env         *env;
    int          fd4;
    int          fd6;
} HttpServerCtx;

/* Forward declarations */
static void on_new_connection(int listen_fd, void *ctx);
static void http_conn_coro(struct Coro *self, void *arg);

/* ---- Listen callback (Express-style) ---- */
static void invoke_listen_cb(Object *cb, int port, Interpreter *it, Env *env) {
    (void)env;
    if (!cb) return;
    ObjList call_args;
    objlist_init(&call_args);
    Object *result = NULL;
    if (cb->type == OBJ_FUNCTION) {
        Object *fn = cb;
        Env *fn_env = env_new(fn->fn.closure);
        if (fn->fn.param_count >= 1) {
            Object *pv = obj_int(port);
            env_set(fn_env, fn->fn.params[0], pv);
            obj_release(pv);
        }
        result = interp_eval(it, fn->fn.body, fn_env);
        if (result && result->type == OBJ_RETURN) {
            Object *v = result->ret.value;
            obj_retain(v);
            obj_release(result);
            result = v;
        }
        env_release(fn_env);
    } else if (cb->type == OBJ_BUILTIN) {
        result = cb->builtin.fn(&call_args);
    }
    if (result) obj_release(result);
    objlist_free(&call_args);
}

/* ---- Coroutine per connection ---- */

/* FdCallback wrapper: re-enqueues the coroutine when fd becomes readable */
static void resume_coro_cb(int fd, void *ctx) {
    (void)fd;
    Coro *c = (Coro *)ctx;
    event_loop_unwatch_fd(fd);
    event_loop_enqueue(c);
}

/*
 * One coroutine per HTTP connection. Loops over keep-alive requests on the
 * same fd. Yields when recv would block so other connections can make progress.
 */
static void http_conn_coro(struct Coro *self, void *arg) {
    HttpConnCtx *conn = (HttpConnCtx *)arg;
    int fd = conn->fd;

    for (;;) {
        /* Receive until we have a complete HTTP request header block */
        conn->buf_len = 0;
        for (;;) {
            /* socket is O_NONBLOCK — EAGAIN/EWOULDBLOCK means no data yet */
            int n = recv(fd, conn->buf + conn->buf_len,
                         BUF_SIZE - conn->buf_len - 1, 0);
            if (n > 0) {
                conn->buf_len += n;
                conn->buf[conn->buf_len] = '\0';
                if (strstr(conn->buf, "\r\n\r\n")) break;
                if (conn->buf_len >= BUF_SIZE - 1) break; /* oversized request */
            } else if (n == 0) {
                goto done; /* connection closed by client */
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Register fd with event loop and yield until readable */
                    event_loop_watch_fd(fd, resume_coro_cb, self);
                    coro_yield();
                    /* resume_coro_cb already called event_loop_unwatch_fd */
                    continue;
                }
                goto done; /* real error */
            }
        }

        if (conn->buf_len == 0) goto done;
        handle_request(fd, conn->server, conn->it, conn->env, conn->buf);

        /* Check if client wants to close (Connection: close in request) */
        if (strstr(conn->buf, "Connection: close") ||
            strstr(conn->buf, "connection: close")) {
            goto done;
        }
    }

done:
    close(fd);
    free(conn->buf);
    free(conn);
}

/* Called by event loop when listener socket is readable */
static void on_new_connection(int listen_fd, void *ctx) {
    HttpServerCtx *sctx = (HttpServerCtx *)ctx;

    /* Accept all pending connections in one callback invocation */
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* no more */
            if (errno == EINTR) continue;
            fprintf(stderr, "accept(): %s\n", strerror(errno));
            break;
        }

        /* Non-blocking + no Nagle delay */
        fcntl(client_fd, F_SETFL, O_NONBLOCK);
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&one, sizeof(one));

        HttpConnCtx *conn = malloc(sizeof(HttpConnCtx));
        conn->fd      = client_fd;
        conn->buf     = malloc(BUF_SIZE);
        conn->buf_len = 0;
        conn->server  = sctx->server;
        conn->it      = sctx->it;
        conn->env     = sctx->env;

        Coro *c = coro_new_c(http_conn_coro, conn);
        event_loop_enqueue(c);
    }
}

/* ---- Signal handling for graceful shutdown ---- */
#ifndef _WIN32
static volatile sig_atomic_t http_interrupt = 0;
static HttpServerCtx *g_server_ctx = NULL;

static void http_sig_handler(int s) {
    (void)s;
    http_interrupt = 1;
    if (g_server_ctx) {
        /* Close listener FDs so event loop stops accepting */
        if (g_server_ctx->fd4 >= 0) { close(g_server_ctx->fd4); g_server_ctx->fd4 = -1; }
        if (g_server_ctx->fd6 >= 0) { close(g_server_ctx->fd6); g_server_ctx->fd6 = -1; }
        event_loop_remove_server();
    }
}
#endif

/* ---- Public API: non-blocking setup, returns immediately ---- */
void http_serve(Object *server, Interpreter *it, Env *env) {
    int port = server->server.port;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int fd4 = listen_ipv4((uint16_t)port);
    if (fd4 < 0) {
        fprintf(stderr, "listen (IPv4): %s\n", strerror(errno));
        return;
    }
    fcntl(fd4, F_SETFL, O_NONBLOCK);

    int fd6 = listen_ipv6((uint16_t)port);
    if (fd6 < 0) {
        fprintf(stderr, "note: IPv6 listen not available (%s); "
                "use http://127.0.0.1:%d if localhost is slow\n",
                strerror(errno), port);
    } else {
        fcntl(fd6, F_SETFL, O_NONBLOCK);
    }

    if (server->server.listen_cb) {
        invoke_listen_cb(server->server.listen_cb, port, it, env);
    } else {
        printf("Bowie server listening on http://0.0.0.0:%d", port);
        if (fd6 >= 0) printf(" and http://[::]:%d", port);
        printf("\n");
        fflush(stdout);
    }

    server->server.fd = fd4;

    HttpServerCtx *sctx = malloc(sizeof(HttpServerCtx));
    sctx->server = server;
    sctx->it     = it;
    sctx->env    = env;
    sctx->fd4    = fd4;
    sctx->fd6    = fd6;

#ifndef _WIN32
    g_server_ctx = sctx;
    struct sigaction sa;
    sa.sa_handler = http_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

    event_loop_watch_fd(fd4, on_new_connection, sctx);
    if (fd6 >= 0)
        event_loop_watch_fd(fd6, on_new_connection, sctx);
    event_loop_add_server();
    /* Returns immediately — event_loop_run() drives the server */
}

/* ---- Outbound fetch ---- */

#ifdef BOWIE_CURL
#include <curl/curl.h>

typedef struct { char *data; size_t len; size_t cap; } DynBuf;

static size_t curl_body_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    DynBuf *b = (DynBuf *)ud;
    size_t n = size * nmemb;
    if (b->len + n + 1 >= b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static size_t curl_header_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    Object *hdrs = (Object *)ud;
    size_t n = size * nmemb;
    /* Skip status line and blank line */
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

Object *http_fetch(const char *url, const char *method,
                   Object *req_headers, const char *body) {
    CURL *curl = curl_easy_init();
    if (!curl) return obj_error_typef("NetworkError", "fetch: failed to initialise curl");

    DynBuf resp_body = { malloc(4096), 0, 4096 };
    resp_body.data[0] = '\0';
    Object *resp_hdrs = obj_hash();

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp_hdrs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    /* Method */
    if (strcmp(method, "POST") == 0) curl_easy_setopt(curl, CURLOPT_POST, 1L);
    else if (strcmp(method, "PUT") == 0) curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    else if (strcmp(method, "GET") != 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

    /* Body */
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }

    /* Request headers */
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
    if (hlist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        curl_easy_cleanup(curl);
        if (hlist) curl_slist_free_all(hlist);
        obj_release(resp_hdrs);
        free(resp_body.data);
        return obj_error_typef("NetworkError", "fetch: %s", curl_easy_strerror(rc));
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (hlist) curl_slist_free_all(hlist);

    Object *resp      = obj_hash();
    Object *status_o  = obj_int(status);
    Object *ok_o      = obj_bool(status >= 200 && status < 300);
    Object *body_o    = obj_string(resp_body.data);
    hash_set(resp, "status",  status_o);
    hash_set(resp, "ok",      ok_o);
    hash_set(resp, "headers", resp_hdrs);
    hash_set(resp, "body",    body_o);
    obj_release(status_o); obj_release(ok_o);
    obj_release(resp_hdrs); obj_release(body_o);
    free(resp_body.data);
    return resp;
}

#else  /* plain-socket HTTP-only fallback */

typedef struct { char host[256]; int port; char path[2048]; } ParsedURL;

static int parse_url(const char *url, ParsedURL *out) {
    if (strncmp(url, "https://", 8) == 0)
        return -1; /* HTTPS not supported */
    if (strncmp(url, "http://", 7) != 0)
        return 0;
    const char *p = url + 7;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        int hlen = (int)(colon - p);
        if (hlen >= (int)sizeof(out->host)) hlen = sizeof(out->host) - 1;
        memcpy(out->host, p, hlen); out->host[hlen] = '\0';
        out->port = atoi(colon + 1);
    } else {
        int hlen = slash ? (int)(slash - p) : (int)strlen(p);
        if (hlen >= (int)sizeof(out->host)) hlen = sizeof(out->host) - 1;
        memcpy(out->host, p, hlen); out->host[hlen] = '\0';
        out->port = 80;
    }
    if (slash)
        snprintf(out->path, sizeof(out->path), "%s", slash);
    else
        strcpy(out->path, "/");
    return 1;
}

Object *http_fetch(const char *url, const char *method,
                   Object *req_headers, const char *body) {
    ParsedURL parsed;
    int r = parse_url(url, &parsed);
    if (r == -1)
        return obj_error_typef("URLParseError", "fetch: HTTPS is not supported (use an http:// URL)");
    if (r == 0)
        return obj_error_typef("URLParseError",
                               "fetch: invalid URL '%s' (only http:// is supported)",
                               url);

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", parsed.port);
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(parsed.host, port_str, &hints, &res) != 0)
        return obj_error_typef("NetworkError", "fetch: cannot resolve host '%s'", parsed.host);

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return obj_error_typef("NetworkError",
                               "fetch: cannot connect to '%s:%d'",
                               parsed.host, parsed.port);

    /* Build request — use HTTP/1.0 to avoid chunked encoding */
    int body_len = body ? (int)strlen(body) : 0;
    char req_buf[16384];
    int  rlen = snprintf(req_buf, sizeof(req_buf),
                         "%s %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "Connection: close\r\n",
                         method, parsed.path, parsed.host);
    if (req_headers && req_headers->type == OBJ_HASH) {
        for (int i = 0; i < 64 && rlen < (int)sizeof(req_buf) - 512; i++) {
            for (HashEntry *e = req_headers->hash.buckets[i]; e; e = e->next) {
                char *v = obj_inspect(e->value);
                rlen += snprintf(req_buf + rlen, sizeof(req_buf) - rlen,
                                 "%s: %s\r\n", e->key, v);
                free(v);
            }
        }
    }
    if (body_len > 0)
        rlen += snprintf(req_buf + rlen, sizeof(req_buf) - rlen,
                         "Content-Length: %d\r\n", body_len);
    rlen += snprintf(req_buf + rlen, sizeof(req_buf) - rlen, "\r\n");
    send(fd, req_buf, rlen, 0);
    if (body_len > 0) send(fd, body, body_len, 0);

    /* Read full response */
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        int n = (int)recv(fd, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += n;
    }
    buf[len] = '\0';
    close(fd);

    /* Parse status line */
    int status = 0;
    const char *p = buf;
    const char *eol = strstr(p, "\r\n");
    if (eol) { sscanf(p, "HTTP/%*s %d", &status); p = eol + 2; }

    /* Parse response headers */
    Object *resp_hdrs = obj_hash();
    while (p < buf + len && !(p[0] == '\r' && p[1] == '\n')) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) break;
        const char *col = (const char *)memchr(p, ':', line_end - p);
        if (col) {
            char key[256]; int klen = (int)(col - p);
            if (klen < (int)sizeof(key)) {
                memcpy(key, p, klen); key[klen] = '\0';
                for (int i = 0; key[i]; i++) key[i] = (char)tolower((unsigned char)key[i]);
                const char *val = col + 1; while (*val == ' ') val++;
                int vlen = (int)(line_end - val);
                char val_buf[2048];
                if (vlen < (int)sizeof(val_buf)) {
                    memcpy(val_buf, val, vlen); val_buf[vlen] = '\0';
                    Object *vs = obj_string(val_buf);
                    hash_set(resp_hdrs, key, vs);
                    obj_release(vs);
                }
            }
        }
        p = line_end + 2;
    }
    if (p[0] == '\r' && p[1] == '\n') p += 2;

    /* Body — copy to null-terminated buffer */
    int blen = (int)((buf + len) - p);
    char *body_copy = malloc(blen + 1);
    memcpy(body_copy, p, blen); body_copy[blen] = '\0';
    Object *body_str = obj_string(body_copy);
    free(body_copy);

    Object *resp = obj_hash();
    Object *status_obj = obj_int(status);
    Object *ok_obj     = obj_bool(status >= 200 && status < 300);
    hash_set(resp, "status",  status_obj);
    hash_set(resp, "ok",      ok_obj);
    hash_set(resp, "headers", resp_hdrs);
    hash_set(resp, "body",    body_str);
    obj_release(status_obj);
    obj_release(resp_hdrs);
    obj_release(body_str);

    free(buf);
    return resp;
}

#endif /* BOWIE_CURL */
