#ifndef BOWIE_ENV_H
#define BOWIE_ENV_H

#include "object.h"

typedef struct EnvEntry {
    char        *key;
    Object      *value;
    int          is_const;
    struct EnvEntry *next;
} EnvEntry;

struct Env {
    EnvEntry *entries[64];
    Env      *outer;
    int       refs;
};

typedef enum {
    ENV_ASSIGN_OK = 0,
    ENV_ASSIGN_UNDEFINED,
    ENV_ASSIGN_CONST
} EnvAssignResult;

Env    *env_new(Env *outer);
void    env_retain(Env *e);
void    env_release(Env *e);
Object *env_get(Env *e, const char *key);
void    env_set(Env *e, const char *key, Object *val);
void    env_define(Env *e, const char *key, Object *val, int is_const);
EnvAssignResult env_assign(Env *e, const char *key, Object *val);

#endif
