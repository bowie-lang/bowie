#include "http.h"
#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
  #include <winsock2.h>
  typedef int socklen_t;
  #define close closesocket
#else
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <signal.h>
#endif

#define BUF_SIZE (1024 * 64)

#ifndef _WIN32
static volatile sig_atomic_t http_interrupt = 0;

static void http_sig_handler(int s) {
    (void)s;
    http_interrupt = 1;
}
#endif

/* ---- Listen sockets (IPv4 + IPv6) ----
 * Tools that resolve "localhost" to ::1 first need an IPv6 listener; otherwise
 * the client can stall for a long fallback timeout when only 0.0.0.0 (IPv4) is bound.
 */
static int listen_ipv4(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
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
#ifdef IPV6_V6ONLY
    /* Separate socket from IPv4; ::1 hits this listener immediately. */
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
    if (listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- Request parsing ---- */
static void parse_query(Object *req, const char *query_str) {
    Object *query = obj_hash();
    char *copy    = strdup(query_str);
    char *token   = strtok(copy, "&");
    while (token) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            hash_set(query, token, obj_string(eq + 1));
        } else {
            hash_set(query, token, obj_string(""));
        }
        token = strtok(NULL, "&");
    }
    free(copy);
    hash_set(req, "query", query);
    obj_release(query);
}

static Object *parse_request(const char *raw) {
    Object *req  = obj_hash();
    Object *hdrs = obj_hash();

    /* Copy so we can tokenize */
    char *buf  = strdup(raw);
    char *line = strtok(buf, "\r\n");
    if (!line) { free(buf); obj_release(req); return NULL; }

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

    hash_set(req, "method", obj_string(method));
    hash_set(req, "path",   obj_string(path));

    /* Headers */
    while ((line = strtok(NULL, "\r\n")) && line[0] != '\0') {
        char *colon = strchr(line, ':');
        if (!colon) break;
        *colon = '\0';
        const char *val = colon + 1;
        while (*val == ' ') val++;
        /* lowercase the key */
        for (char *p = line; *p; p++) *p = tolower((unsigned char)*p);
        hash_set(hdrs, line, obj_string(val));
        *colon = ':';
    }
    hash_set(req, "headers", hdrs);
    obj_release(hdrs);

    /* Body — whatever remains after blank line */
    /* strtok already consumed \r\n pairs; find double \r\n in original */
    const char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start) body_start += 4;
    else body_start = "";
    hash_set(req, "body", obj_string(body_start));

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
        if (r->query_key && !strcasecmp(r->method, method) && !strcmp(r->path, path)
            && query_param_present(query, r->query_key))
            return r;
        r = r->next;
    }
    r = server->server.routes;
    while (r) {
        if (!r->query_key && !strcasecmp(r->method, method) && !strcmp(r->path, path))
            return r;
        r = r->next;
    }
    r = server->server.routes;
    while (r) {
        if (!r->query_key && !strcasecmp(r->method, "*") && !strcmp(r->path, path)) return r;
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
                         "Connection: close\r\n",
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

static void serve_one_client(int client_fd, Object *server, Interpreter *it, Env *env,
                             char *buf) {
    int n = recv(client_fd, buf, BUF_SIZE - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        handle_request(client_fd, server, it, env, buf);
    }
    close(client_fd);
}

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

/* ---- Event loop ---- */
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

    int fd6 = listen_ipv6((uint16_t)port);
    if (fd6 < 0) {
        fprintf(stderr, "note: IPv6 listen not available (%s); use http://127.0.0.1:%d if localhost is slow\n",
                strerror(errno), port);
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

#ifndef _WIN32
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = http_sig_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
#endif

    char *buf = malloc(BUF_SIZE);
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd4, &rfds);
#ifdef _WIN32
        unsigned maxfd = (unsigned)fd4;
#else
        int maxfd = fd4;
#endif
        if (fd6 >= 0) {
            FD_SET(fd6, &rfds);
#ifdef _WIN32
            if ((unsigned)fd6 > maxfd) maxfd = (unsigned)fd6;
#else
            if (fd6 > maxfd) maxfd = fd6;
#endif
        }
        /* Winsock ignores nfds; POSIX uses it as highest fd + 1 */
#ifdef _WIN32
        if (select(0, &rfds, NULL, NULL, NULL) < 0) {
#else
        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
#endif
            if (errno == EINTR) {
#ifndef _WIN32
                if (http_interrupt) break;
#endif
                continue;
            }
            fprintf(stderr, "select(): %s\n", strerror(errno));
            continue;
        }
        if (FD_ISSET(fd4, &rfds)) {
            struct sockaddr_storage client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(fd4, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR) {
#ifndef _WIN32
                    if (http_interrupt) break;
#endif
                    continue;
                }
                fprintf(stderr, "accept(): %s\n", strerror(errno));
                continue;
            }
            serve_one_client(client_fd, server, it, env, buf);
        }
        if (fd6 >= 0 && FD_ISSET(fd6, &rfds)) {
            struct sockaddr_storage client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(fd6, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR) {
#ifndef _WIN32
                    if (http_interrupt) break;
#endif
                    continue;
                }
                fprintf(stderr, "accept(): %s\n", strerror(errno));
                continue;
            }
            serve_one_client(client_fd, server, it, env, buf);
        }
    }

    close(fd4);
    if (fd6 >= 0) close(fd6);
    free(buf);
}
