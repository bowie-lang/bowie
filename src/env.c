#include "env.h"
#include <stdlib.h>
#include <string.h>

static unsigned int env_hash(const char *key) {
    unsigned int h = 5381;
    while (*key) h = ((h << 5) + h) + (unsigned char)*key++;
    return h % 64;
}

Env *env_new(Env *outer) {
    Env *e = calloc(1, sizeof(Env));
    e->outer = outer;
    e->refs  = 1;
    if (outer) env_retain(outer);
    return e;
}

void env_retain(Env *e) { if (e) e->refs++; }

void env_release(Env *e) {
    if (!e || --e->refs > 0) return;
    for (int i = 0; i < 64; i++) {
        EnvEntry *en = e->entries[i];
        while (en) {
            EnvEntry *next = en->next;
            free(en->key);
            obj_release(en->value);
            free(en);
            en = next;
        }
    }
    if (e->outer) env_release(e->outer);
    free(e);
}

Object *env_get(Env *e, const char *key) {
    unsigned int idx = env_hash(key);
    EnvEntry *en = e->entries[idx];
    while (en) {
        if (!strcmp(en->key, key)) return en->value;
        en = en->next;
    }
    return e->outer ? env_get(e->outer, key) : NULL;
}

void env_define(Env *e, const char *key, Object *val, int is_const) {
    unsigned int idx = env_hash(key);
    EnvEntry *en = e->entries[idx];
    while (en) {
        if (!strcmp(en->key, key)) {
            obj_release(en->value);
            en->value = val;
            en->is_const = is_const;
            obj_retain(val);
            return;
        }
        en = en->next;
    }
    en        = malloc(sizeof(EnvEntry));
    en->key   = strdup(key);
    en->value = val;
    en->is_const = is_const;
    obj_retain(val);
    en->next  = e->entries[idx];
    e->entries[idx] = en;
}

void env_set(Env *e, const char *key, Object *val) {
    env_define(e, key, val, 0);
}

/* Walk up the scope chain to find and update an existing binding */
EnvAssignResult env_assign(Env *e, const char *key, Object *val) {
    unsigned int idx = env_hash(key);
    EnvEntry *en = e->entries[idx];
    while (en) {
        if (!strcmp(en->key, key)) {
            if (en->is_const) return ENV_ASSIGN_CONST;
            obj_release(en->value);
            en->value = val;
            obj_retain(val);
            return ENV_ASSIGN_OK;
        }
        en = en->next;
    }
    return e->outer ? env_assign(e->outer, key, val) : ENV_ASSIGN_UNDEFINED;
}
