#ifndef BOWIE_ENV_H
#define BOWIE_ENV_H

#include "object.h"

typedef struct EnvEntry {
    char        *key;
    Object      *value;
    struct EnvEntry *next;
} EnvEntry;

struct Env {
    EnvEntry *entries[64];
    Env      *outer;
    int       refs;
};

Env    *env_new(Env *outer);
void    env_retain(Env *e);
void    env_release(Env *e);
Object *env_get(Env *e, const char *key);
void    env_set(Env *e, const char *key, Object *val);
int     env_assign(Env *e, const char *key, Object *val);

#endif
