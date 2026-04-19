#ifndef BOWIE_INTERPRETER_H
#define BOWIE_INTERPRETER_H

#include "ast.h"
#include "object.h"
#include "env.h"

typedef struct ModCache {
    char          *path;    /* resolved absolute path */
    Object        *module;  /* hash of exported names */
    Node          *ast;     /* kept alive — functions reference body nodes */
    struct ModCache *next;
} ModCache;

typedef struct {
    Env      *globals;
    ModCache *cache;          /* loaded module cache */
    Object   *exports;        /* current module's export hash (NULL in main) */
    char     *current_file;   /* path of the file being evaluated */
    char     *project_root;   /* directory of the entry-point file; resolves "@/" aliases */
} Interpreter;

Interpreter *interp_new(void);
void         interp_free(Interpreter *it);
Object      *interp_eval(Interpreter *it, Node *node, Env *env);

/* Load and evaluate a .bow file, returning its export hash */
Object      *interp_load_module(Interpreter *it, const char *path);

#endif
