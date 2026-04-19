#ifndef BOWIE_HTTP_H
#define BOWIE_HTTP_H

#include "object.h"
#include "interpreter.h"

/* Start the HTTP event loop.  Blocks until process exits. */
void http_serve(Object *server, Interpreter *it, Env *env);

/* Outbound HTTP request.  Returns a response hash or an error object. */
Object *http_fetch(const char *url, const char *method,
                   Object *req_headers, const char *body);

#endif
