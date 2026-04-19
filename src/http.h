#ifndef BOWIE_HTTP_H
#define BOWIE_HTTP_H

#include "object.h"
#include "interpreter.h"

/* Start the HTTP event loop.  Blocks until process exits. */
void http_serve(Object *server, Interpreter *it, Env *env);

#endif
