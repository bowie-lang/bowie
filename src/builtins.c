#include "builtins.h"
#include "http.h"
#include "interpreter.h"
#ifndef _WIN32
#include "coro.h"
#include "event_loop.h"
#endif
#include "postgres.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>

/* ---- Macro helpers ---- */
#define ARG(n)    (args->count > (n) ? args->items[n] : BOWIE_NULL)
#define NARGS     (args->count)
#define REQUIRE(n, name) \
    if (NARGS < (n)) return obj_error_typef("ArityError", name "() requires %d argument(s)", n)

/* ---- I/O ---- */
static Object *bw_print(ObjList *args) {
    for (int i = 0; i < NARGS; i++) {
        char *s = obj_inspect(ARG(i));
        fputs(s, stdout);
        free(s);
        if (i < NARGS - 1) fputc(' ', stdout);
    }
    fflush(stdout);
    return obj_null();
}

static Object *bw_println(ObjList *args) {
    for (int i = 0; i < NARGS; i++) {
        char *s = obj_inspect(ARG(i));
        fputs(s, stdout);
        free(s);
        if (i < NARGS - 1) fputc(' ', stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return obj_null();
}

static Object *bw_eprint(ObjList *args) {
    for (int i = 0; i < NARGS; i++) {
        char *s = obj_inspect(ARG(i));
        fputs(s, stderr);
        free(s);
        if (i < NARGS - 1) fputc(' ', stderr);
    }
    fputc('\n', stderr);
    return obj_null();
}

/* ---- printf / sprintf (C-style format string) ---- */
enum {
    PF_LM_NONE,
    PF_LM_HH,
    PF_LM_H,
    PF_LM_L,
    PF_LM_LL,
    PF_LM_Z,
    PF_LM_CAP_L, /* L before fga etc. → long double */
};

static long long bow_as_ll(Object *o) {
    if (!o || o->type == OBJ_NULL) return 0;
    switch (o->type) {
        case OBJ_INT:   return o->int_val;
        case OBJ_FLOAT: return (long long)o->float_val;
        case OBJ_BOOL:  return o->bool_val ? 1 : 0;
        case OBJ_STRING: {
            char *end;
            long long v = strtoll(o->string.str, &end, 10);
            if (end != o->string.str && (*end == '\0' || isspace((unsigned char)*end)))
                return v;
            return 0;
        }
        default: return 0;
    }
}

static unsigned long long bow_as_ull(Object *o) {
    return (unsigned long long)bow_as_ll(o);
}

static double bow_as_double(Object *o) {
    if (!o || o->type == OBJ_NULL) return 0.0;
    switch (o->type) {
        case OBJ_FLOAT: return o->float_val;
        case OBJ_INT:   return (double)o->int_val;
        case OBJ_BOOL:  return o->bool_val ? 1.0 : 0.0;
        case OBJ_STRING: {
            char *end;
            double v = strtod(o->string.str, &end);
            if (end != o->string.str && (*end == '\0' || isspace((unsigned char)*end)))
                return v;
            return 0.0;
        }
        default: return 0.0;
    }
}

static int buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        while (*len + n + 1 > nc) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int buf_append_ch(char **buf, size_t *len, size_t *cap, char c) {
    return buf_append(buf, len, cap, &c, 1);
}

static const char *parse_len_mod(const char *q, int *lmod_out) {
    *lmod_out = PF_LM_NONE;
    if (q[0] == 'h' && q[1] == 'h') {
        *lmod_out = PF_LM_HH;
        return q + 2;
    }
    if (q[0] == 'h') {
        *lmod_out = PF_LM_H;
        return q + 1;
    }
    if (q[0] == 'l' && q[1] == 'l') {
        *lmod_out = PF_LM_LL;
        return q + 2;
    }
    if (q[0] == 'l') {
        *lmod_out = PF_LM_L;
        return q + 1;
    }
    if (q[0] == 'L') {
        *lmod_out = PF_LM_CAP_L;
        return q + 1;
    }
    if (q[0] == 'j') {
        *lmod_out = PF_LM_LL;
        return q + 1;
    }
    if (q[0] == 'z') {
        *lmod_out = PF_LM_Z;
        return q + 1;
    }
    if (q[0] == 't') {
        *lmod_out = PF_LM_L;
        return q + 1;
    }
    return q;
}

/* Build formatted string from ARG(0)=format and ARG(1..)=values. On error writes to err. */
static int bow_format_build(ObjList *args, char **out, char *err, size_t errlen) {
    *out = NULL;
    size_t len = 0, cap = 0;
    if (args->count < 1 || ARG(0)->type != OBJ_STRING) {
        snprintf(err, errlen, "first argument must be a format string");
        return -1;
    }
    const char *fmt = ARG(0)->string.str;
    int ai = 1;

    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            if (buf_append_ch(out, &len, &cap, *p) < 0) goto oom;
            p++;
            continue;
        }
        if (p[1] == '%') {
            if (buf_append_ch(out, &len, &cap, '%') < 0) goto oom;
            p += 2;
            continue;
        }

        const char *spec_start = p;
        p++;
        while (*p && strchr("-+ #0", *p)) p++;
        if (*p == '*') {
            snprintf(err, errlen, "'*' width/precision is not supported");
            goto fail;
        }
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            if (*p == '*') {
                snprintf(err, errlen, "'*' width/precision is not supported");
                goto fail;
            }
            while (*p >= '0' && *p <= '9') p++;
        }
        int lmod = PF_LM_NONE;
        p = parse_len_mod(p, &lmod);
        char conv = *p;
        if (!conv) {
            snprintf(err, errlen, "truncated format (missing conversion)");
            goto fail;
        }
        p++;

        if (conv == 'n') {
            snprintf(err, errlen, "%%n is not allowed");
            goto fail;
        }

        if (ai >= args->count) {
            snprintf(err, errlen, "not enough arguments for format");
            goto fail;
        }
        Object *val = args->items[ai++];

        size_t spec_len = (size_t)(p - spec_start);
        if (spec_len >= 256) {
            snprintf(err, errlen, "format specifier too long");
            goto fail;
        }
        char specbuf[256];
        memcpy(specbuf, spec_start, spec_len);
        specbuf[spec_len] = '\0';

        char piece[8192];
        int nw;

        switch (conv) {
            case 'd':
            case 'i':
                switch (lmod) {
                    case PF_LM_LL:
                        nw = snprintf(piece, sizeof(piece), specbuf, (long long)bow_as_ll(val));
                        break;
                    case PF_LM_L:
                        nw = snprintf(piece, sizeof(piece), specbuf, (long)bow_as_ll(val));
                        break;
                    case PF_LM_H:
                        nw = snprintf(piece, sizeof(piece), specbuf, (short)bow_as_ll(val));
                        break;
                    case PF_LM_HH:
                        nw = snprintf(piece, sizeof(piece), specbuf, (signed char)bow_as_ll(val));
                        break;
                    default:
                        nw = snprintf(piece, sizeof(piece), specbuf, (int)bow_as_ll(val));
                        break;
                }
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                switch (lmod) {
                    case PF_LM_LL:
                        nw = snprintf(piece, sizeof(piece), specbuf,
                                      (unsigned long long)bow_as_ull(val));
                        break;
                    case PF_LM_L:
                        nw = snprintf(piece, sizeof(piece), specbuf,
                                      (unsigned long)bow_as_ull(val));
                        break;
                    case PF_LM_Z:
                        nw = snprintf(piece, sizeof(piece), specbuf,
                                      (size_t)bow_as_ull(val));
                        break;
                    case PF_LM_H:
                        nw = snprintf(piece, sizeof(piece), specbuf,
                                      (unsigned short)bow_as_ull(val));
                        break;
                    case PF_LM_HH:
                        nw = snprintf(piece, sizeof(piece), specbuf,
                                      (unsigned char)bow_as_ull(val));
                        break;
                    default:
                        nw = snprintf(piece, sizeof(piece), specbuf,
                                      (unsigned int)bow_as_ull(val));
                        break;
                }
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                if (lmod == PF_LM_CAP_L)
                    nw = snprintf(piece, sizeof(piece), specbuf,
                                  (long double)bow_as_double(val));
                else
                    nw = snprintf(piece, sizeof(piece), specbuf, bow_as_double(val));
                break;
            case 's': {
                char *sfree = NULL;
                const char *cs;
                if (val->type == OBJ_STRING)
                    cs = val->string.str;
                else {
                    sfree = obj_inspect(val);
                    cs = sfree ? sfree : "";
                }
                nw = snprintf(piece, sizeof(piece), specbuf, cs);
                free(sfree);
                break;
            }
            case 'c': {
                int ch;
                if (val->type == OBJ_STRING && val->string.str[0])
                    ch = (unsigned char)val->string.str[0];
                else
                    ch = (int)(bow_as_ll(val) & 255);
                nw = snprintf(piece, sizeof(piece), specbuf, ch);
                break;
            }
            case 'p':
                nw = snprintf(piece, sizeof(piece), specbuf, (void *)val);
                break;
            default:
                snprintf(err, errlen, "unsupported conversion %%%c", conv);
                goto fail;
        }

        if (nw < 0 || (size_t)nw >= sizeof(piece)) {
            snprintf(err, errlen, "formatted output too large or invalid");
            goto fail;
        }
        if (buf_append(out, &len, &cap, piece, (size_t)nw) < 0) goto oom;
    }

    if (!*out) {
        *out = strdup("");
        if (!*out) goto oom;
    }
    return 0;

oom:
    snprintf(err, errlen, "out of memory");
fail:
    free(*out);
    *out = NULL;
    return -1;
}

static Object *bw_printf(ObjList *args) {
    char *out = NULL;
    char err[256];
    if (bow_format_build(args, &out, err, sizeof err) != 0)
        return obj_error_typef("FormatError", "printf(): %s", err);
    fputs(out, stdout);
    fflush(stdout);
    free(out);
    return obj_null();
}

static Object *bw_sprintf(ObjList *args) {
    char *out = NULL;
    char err[256];
    if (bow_format_build(args, &out, err, sizeof err) != 0)
        return obj_error_typef("FormatError", "sprintf(): %s", err);
    Object *o = obj_string(out);
    free(out);
    return o;
}

/* ---- Type conversion ---- */
static Object *bw_str(ObjList *args) {
    REQUIRE(1, "str");
    char *s = obj_inspect(ARG(0));
    Object *o = obj_string(s);
    free(s);
    return o;
}

static Object *bw_int(ObjList *args) {
    REQUIRE(1, "int");
    Object *a = ARG(0);
    if (a->type == OBJ_INT)   return obj_int(a->int_val);
    if (a->type == OBJ_FLOAT) return obj_int((long long)a->float_val);
    if (a->type == OBJ_BOOL)  return obj_int(a->bool_val);
    if (a->type == OBJ_STRING) {
        char *end;
        long long v = strtoll(a->string.str, &end, 10);
        if (end == a->string.str || (*end != '\0' && !isspace(*end)))
            return obj_error_typef("ValueError", "int(): cannot convert '%s'", a->string.str);
        return obj_int(v);
    }
    return obj_error_typef("TypeMismatchError", "int(): cannot convert %s", obj_type_name(a->type));
}

static Object *bw_float(ObjList *args) {
    REQUIRE(1, "float");
    Object *a = ARG(0);
    if (a->type == OBJ_FLOAT) return obj_float(a->float_val);
    if (a->type == OBJ_INT)   return obj_float((double)a->int_val);
    if (a->type == OBJ_STRING) {
        char *end;
        double v = strtod(a->string.str, &end);
        if (*end != '\0' && !isspace(*end))
            return obj_error_typef("ValueError", "float(): cannot convert '%s'", a->string.str);
        return obj_float(v);
    }
    return obj_error_typef("TypeMismatchError", "float(): cannot convert %s", obj_type_name(a->type));
}

static Object *bw_bool(ObjList *args) {
    REQUIRE(1, "bool");
    return obj_bool(obj_is_truthy(ARG(0)));
}

static Object *bw_type(ObjList *args) {
    REQUIRE(1, "type");
    return obj_string(obj_type_name(ARG(0)->type));
}

static Object *bw_error_type(ObjList *args) {
    REQUIRE(1, "error_type");
    if (ARG(0)->type == OBJ_ERROR)
        return obj_string(ARG(0)->error.type ? ARG(0)->error.type : "RuntimeError");
    if (ARG(0)->type == OBJ_HASH) {
        Object *type = hash_get(ARG(0), "type");
        if (type != BOWIE_NULL && type->type == OBJ_STRING) return obj_string(type->string.str);
    }
    return obj_null();
}

static Object *bw_is_error_type(ObjList *args) {
    REQUIRE(2, "is_error_type");
    if (ARG(1)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "is_error_type(): second argument must be string");
    const char *actual = NULL;
    if (ARG(0)->type == OBJ_ERROR) {
        actual = ARG(0)->error.type ? ARG(0)->error.type : "RuntimeError";
    } else if (ARG(0)->type == OBJ_HASH) {
        Object *type = hash_get(ARG(0), "type");
        if (type != BOWIE_NULL && type->type == OBJ_STRING) actual = type->string.str;
    }
    if (!actual) return obj_bool(0);
    return obj_bool(strcmp(actual, ARG(1)->string.str) == 0);
}

/* ---- Collections ---- */
static Object *bw_len(ObjList *args) {
    REQUIRE(1, "len");
    Object *a = ARG(0);
    if (a->type == OBJ_STRING)  return obj_int(strlen(a->string.str));
    if (a->type == OBJ_ARRAY)   return obj_int(a->array.elems.count);
    if (a->type == OBJ_HASH)    return obj_int(a->hash.size);
    return obj_error_typef("TypeMismatchError", "len(): not supported for %s", obj_type_name(a->type));
}

static Object *bw_push(ObjList *args) {
    REQUIRE(2, "push");
    if (ARG(0)->type != OBJ_ARRAY)
        return obj_error_typef("TypeMismatchError", "push(): first argument must be array");
    array_push(ARG(0), ARG(1));
    return obj_null();
}

static Object *bw_pop(ObjList *args) {
    REQUIRE(1, "pop");
    if (ARG(0)->type != OBJ_ARRAY)
        return obj_error_typef("TypeMismatchError", "pop(): argument must be array");
    Object *arr = ARG(0);
    if (arr->array.elems.count == 0) return obj_null();
    int last = arr->array.elems.count - 1;
    Object *v = arr->array.elems.items[last];
    obj_retain(v);
    obj_release(arr->array.elems.items[last]);
    arr->array.elems.count--;
    return v;
}

static Object *bw_keys(ObjList *args) {
    REQUIRE(1, "keys");
    if (ARG(0)->type != OBJ_HASH)
        return obj_error_typef("TypeMismatchError", "keys(): argument must be hash");
    int n;
    char **ks = hash_keys(ARG(0), &n);
    Object *arr = obj_array();
    for (int i = 0; i < n; i++) {
        Object *s = obj_string(ks[i]);
        array_push(arr, s);
        obj_release(s);
    }
    free(ks);
    return arr;
}

static Object *bw_values(ObjList *args) {
    REQUIRE(1, "values");
    if (ARG(0)->type != OBJ_HASH)
        return obj_error_typef("TypeMismatchError", "values(): argument must be hash");
    Object *h   = ARG(0);
    Object *arr = obj_array();
    for (int i = 0; i < 64; i++) {
        HashEntry *e = h->hash.buckets[i];
        while (e) {
            obj_retain(e->value);
            array_push(arr, e->value);
            obj_release(e->value);
            e = e->next;
        }
    }
    return arr;
}

static Object *bw_range(ObjList *args) {
    REQUIRE(1, "range");
    long long start = 0, end_, step = 1;
    if (NARGS == 1) {
        if (ARG(0)->type != OBJ_INT) return obj_error_typef("TypeMismatchError", "range(): argument must be int");
        end_ = ARG(0)->int_val;
    } else {
        if (ARG(0)->type != OBJ_INT || ARG(1)->type != OBJ_INT)
            return obj_error_typef("TypeMismatchError", "range(): arguments must be int");
        start = ARG(0)->int_val;
        end_  = ARG(1)->int_val;
        if (NARGS >= 3) {
            if (ARG(2)->type != OBJ_INT) return obj_error_typef("TypeMismatchError", "range(): step must be int");
            step = ARG(2)->int_val;
            if (step == 0) return obj_error_typef("ValueError", "range(): step cannot be 0");
        }
    }
    Object *arr = obj_array();
    if (step > 0) {
        for (long long i = start; i < end_; i += step) {
            Object *v = obj_int(i);
            array_push(arr, v);
            obj_release(v);
        }
    } else {
        for (long long i = start; i > end_; i += step) {
            Object *v = obj_int(i);
            array_push(arr, v);
            obj_release(v);
        }
    }
    return arr;
}

static Object *bw_slice(ObjList *args) {
    REQUIRE(2, "slice");
    Object *a = ARG(0);
    if (a->type != OBJ_ARRAY && a->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "slice(): first arg must be array or string");
    if (ARG(1)->type != OBJ_INT) return obj_error_typef("TypeMismatchError", "slice(): start must be int");
    int start = (int)ARG(1)->int_val;
    int end_  = -1;
    if (NARGS >= 3) {
        if (ARG(2)->type != OBJ_INT) return obj_error_typef("TypeMismatchError", "slice(): end must be int");
        end_ = (int)ARG(2)->int_val;
    }
    if (a->type == OBJ_ARRAY) {
        int len = a->array.elems.count;
        if (start < 0) start += len;
        if (end_ < 0)  end_  = len;
        if (start < 0) start = 0;
        if (end_ > len) end_ = len;
        Object *arr = obj_array();
        for (int i = start; i < end_; i++) {
            array_push(arr, a->array.elems.items[i]);
        }
        return arr;
    } else {
        int len = strlen(a->string.str);
        if (start < 0) start += len;
        if (end_ < 0)  end_  = len;
        if (start < 0) start = 0;
        if (end_ > len) end_ = len;
        if (end_ <= start) return obj_string("");
        int sz = end_ - start;
        char *buf = malloc(sz + 1);
        memcpy(buf, a->string.str + start, sz);
        buf[sz] = '\0';
        Object *s = obj_string(buf);
        free(buf);
        return s;
    }
}

/* ---- String operations ---- */
static Object *bw_split(ObjList *args) {
    REQUIRE(2, "split");
    if (ARG(0)->type != OBJ_STRING || ARG(1)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "split(): requires two strings");
    const char *src  = ARG(0)->string.str;
    const char *delim= ARG(1)->string.str;
    Object *arr = obj_array();
    if (strlen(delim) == 0) {
        for (int i = 0; src[i]; i++) {
            char buf[2] = { src[i], '\0' };
            Object *s = obj_string(buf);
            array_push(arr, s); obj_release(s);
        }
        return arr;
    }
    char *copy = strdup(src);
    char *token = strtok(copy, delim);
    while (token) {
        Object *s = obj_string(token);
        array_push(arr, s); obj_release(s);
        token = strtok(NULL, delim);
    }
    free(copy);
    return arr;
}

static Object *bw_join(ObjList *args) {
    REQUIRE(2, "join");
    if (ARG(0)->type != OBJ_ARRAY || ARG(1)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "join(): requires (array, string)");
    Object *arr   = ARG(0);
    const char *d = ARG(1)->string.str;
    int dl = strlen(d);
    int total = 0;
    for (int i = 0; i < arr->array.elems.count; i++) {
        char *s = obj_inspect(arr->array.elems.items[i]);
        total += strlen(s) + dl;
        free(s);
    }
    char *buf = malloc(total + 1);
    buf[0] = '\0';
    for (int i = 0; i < arr->array.elems.count; i++) {
        char *s = obj_inspect(arr->array.elems.items[i]);
        strcat(buf, s);
        free(s);
        if (i < arr->array.elems.count - 1) strcat(buf, d);
    }
    Object *o = obj_string(buf);
    free(buf);
    return o;
}

static Object *bw_trim(ObjList *args) {
    REQUIRE(1, "trim");
    if (ARG(0)->type != OBJ_STRING) return obj_error_typef("TypeMismatchError", "trim(): requires string");
    const char *s = ARG(0)->string.str;
    while (*s && isspace((unsigned char)*s)) s++;
    const char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e - 1))) e--;
    int len = e - s;
    char *buf = malloc(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    Object *o = obj_string(buf);
    free(buf);
    return o;
}

static Object *bw_upper(ObjList *args) {
    REQUIRE(1, "upper");
    if (ARG(0)->type != OBJ_STRING) return obj_error_typef("TypeMismatchError", "upper(): requires string");
    char *s = strdup(ARG(0)->string.str);
    for (int i = 0; s[i]; i++) s[i] = toupper((unsigned char)s[i]);
    Object *o = obj_string(s); free(s); return o;
}

static Object *bw_lower(ObjList *args) {
    REQUIRE(1, "lower");
    if (ARG(0)->type != OBJ_STRING) return obj_error_typef("TypeMismatchError", "lower(): requires string");
    char *s = strdup(ARG(0)->string.str);
    for (int i = 0; s[i]; i++) s[i] = tolower((unsigned char)s[i]);
    Object *o = obj_string(s); free(s); return o;
}

static Object *bw_contains(ObjList *args) {
    REQUIRE(2, "contains");
    Object *a = ARG(0), *b = ARG(1);
    if (a->type == OBJ_STRING && b->type == OBJ_STRING)
        return obj_bool(strstr(a->string.str, b->string.str) != NULL);
    if (a->type == OBJ_ARRAY) {
        for (int i = 0; i < a->array.elems.count; i++)
            if (obj_equals(a->array.elems.items[i], b)) return obj_bool(1);
        return obj_bool(0);
    }
    return obj_error_typef("TypeMismatchError", "contains(): not supported for %s", obj_type_name(a->type));
}

static Object *bw_replace(ObjList *args) {
    REQUIRE(3, "replace");
    if (ARG(0)->type != OBJ_STRING || ARG(1)->type != OBJ_STRING || ARG(2)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "replace(): requires three strings");
    const char *src = ARG(0)->string.str;
    const char *old = ARG(1)->string.str;
    const char *new = ARG(2)->string.str;
    int old_len = strlen(old), new_len = strlen(new);
    if (old_len == 0) return obj_string(src);
    int cap = strlen(src) * 2 + 64;
    char *buf = malloc(cap); buf[0] = '\0';
    int out = 0;
    const char *p = src;
    while (*p) {
        if (strncmp(p, old, old_len) == 0) {
            if (out + new_len + 1 >= cap) { cap = cap * 2 + new_len; buf = realloc(buf, cap); }
            memcpy(buf + out, new, new_len); out += new_len; p += old_len;
        } else {
            if (out + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[out++] = *p++;
        }
    }
    buf[out] = '\0';
    Object *o = obj_string(buf); free(buf); return o;
}

static Object *bw_starts_with(ObjList *args) {
    REQUIRE(2, "starts_with");
    if (ARG(0)->type != OBJ_STRING || ARG(1)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "starts_with(): requires two strings");
    return obj_bool(strncmp(ARG(0)->string.str, ARG(1)->string.str,
                            strlen(ARG(1)->string.str)) == 0);
}

static Object *bw_ends_with(ObjList *args) {
    REQUIRE(2, "ends_with");
    if (ARG(0)->type != OBJ_STRING || ARG(1)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "ends_with(): requires two strings");
    const char *s = ARG(0)->string.str, *suffix = ARG(1)->string.str;
    int sl = strlen(s), pl = strlen(suffix);
    if (pl > sl) return obj_bool(0);
    return obj_bool(strcmp(s + sl - pl, suffix) == 0);
}

static Object *bw_index_of(ObjList *args) {
    REQUIRE(2, "index_of");
    if (ARG(0)->type == OBJ_STRING && ARG(1)->type == OBJ_STRING) {
        const char *p = strstr(ARG(0)->string.str, ARG(1)->string.str);
        return obj_int(p ? (long long)(p - ARG(0)->string.str) : -1);
    }
    if (ARG(0)->type == OBJ_ARRAY) {
        for (int i = 0; i < ARG(0)->array.elems.count; i++)
            if (obj_equals(ARG(0)->array.elems.items[i], ARG(1))) return obj_int(i);
        return obj_int(-1);
    }
    return obj_error_typef("TypeMismatchError", "index_of(): not supported for %s", obj_type_name(ARG(0)->type));
}

/* Depth-first: first hash where o[key] equals want; retains and returns that hash, else NULL */
static Object *lookup_recurse(Object *node, const char *key, Object *want) {
    if (!node) return NULL;
    if (node->type == OBJ_HASH) {
        Object *field = hash_get(node, key);
        if (field != BOWIE_NULL && obj_equals(field, want)) {
            obj_retain(node);
            return node;
        }
        for (int i = 0; i < 64; i++) {
            HashEntry *e = node->hash.buckets[i];
            while (e) {
                Object *found = lookup_recurse(e->value, key, want);
                if (found) return found;
                e = e->next;
            }
        }
    } else if (node->type == OBJ_ARRAY) {
        for (int i = 0; i < node->array.elems.count; i++) {
            Object *found = lookup_recurse(node->array.elems.items[i], key, want);
            if (found) return found;
        }
    }
    return NULL;
}

static Object *bw_lookup(ObjList *args) {
    REQUIRE(3, "lookup");
    Object *keyobj = ARG(1);
    if (keyobj->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "lookup(): key must be string");
    Object *found = lookup_recurse(ARG(0), keyobj->string.str, ARG(2));
    if (found) return found;
    return obj_null();
}

/* ---- Math ---- */
static Object *bw_floor(ObjList *args) {
    REQUIRE(1, "floor");
    if (ARG(0)->type == OBJ_INT)   return obj_int(ARG(0)->int_val);
    if (ARG(0)->type == OBJ_FLOAT) return obj_int((long long)floor(ARG(0)->float_val));
    return obj_error_typef("TypeMismatchError", "floor(): requires number");
}
static Object *bw_ceil(ObjList *args) {
    REQUIRE(1, "ceil");
    if (ARG(0)->type == OBJ_INT)   return obj_int(ARG(0)->int_val);
    if (ARG(0)->type == OBJ_FLOAT) return obj_int((long long)ceil(ARG(0)->float_val));
    return obj_error_typef("TypeMismatchError", "ceil(): requires number");
}
static Object *bw_abs(ObjList *args) {
    REQUIRE(1, "abs");
    if (ARG(0)->type == OBJ_INT)   return obj_int(llabs(ARG(0)->int_val));
    if (ARG(0)->type == OBJ_FLOAT) return obj_float(fabs(ARG(0)->float_val));
    return obj_error_typef("TypeMismatchError", "abs(): requires number");
}
static Object *bw_sqrt(ObjList *args) {
    REQUIRE(1, "sqrt");
    double v = ARG(0)->type == OBJ_INT ? (double)ARG(0)->int_val : ARG(0)->float_val;
    return obj_float(sqrt(v));
}
static Object *bw_pow(ObjList *args) {
    REQUIRE(2, "pow");
    double a = ARG(0)->type == OBJ_INT ? (double)ARG(0)->int_val : ARG(0)->float_val;
    double b = ARG(1)->type == OBJ_INT ? (double)ARG(1)->int_val : ARG(1)->float_val;
    return obj_float(pow(a, b));
}
static Object *bw_max2(ObjList *args) {
    if (NARGS < 1) return obj_error_typef("ArityError", "max(): requires at least 1 argument");
    if (ARG(0)->type == OBJ_ARRAY) {
        Object *arr = ARG(0);
        if (arr->array.elems.count == 0) return obj_null();
        Object *m = arr->array.elems.items[0];
        for (int i = 1; i < arr->array.elems.count; i++) {
            Object *x = arr->array.elems.items[i];
            if ((x->type == OBJ_INT && m->type == OBJ_INT && x->int_val > m->int_val) ||
                (x->type == OBJ_FLOAT && m->type == OBJ_FLOAT && x->float_val > m->float_val))
                m = x;
        }
        obj_retain(m); return m;
    }
    REQUIRE(2, "max");
    if (ARG(0)->type == OBJ_INT && ARG(1)->type == OBJ_INT)
        return obj_int(ARG(0)->int_val > ARG(1)->int_val ? ARG(0)->int_val : ARG(1)->int_val);
    double a = ARG(0)->type == OBJ_INT ? (double)ARG(0)->int_val : ARG(0)->float_val;
    double b = ARG(1)->type == OBJ_INT ? (double)ARG(1)->int_val : ARG(1)->float_val;
    return obj_float(a > b ? a : b);
}
static Object *bw_min2(ObjList *args) {
    if (NARGS < 1) return obj_error_typef("ArityError", "min(): requires at least 1 argument");
    if (ARG(0)->type == OBJ_ARRAY) {
        Object *arr = ARG(0);
        if (arr->array.elems.count == 0) return obj_null();
        Object *m = arr->array.elems.items[0];
        for (int i = 1; i < arr->array.elems.count; i++) {
            Object *x = arr->array.elems.items[i];
            if ((x->type == OBJ_INT && m->type == OBJ_INT && x->int_val < m->int_val) ||
                (x->type == OBJ_FLOAT && m->type == OBJ_FLOAT && x->float_val < m->float_val))
                m = x;
        }
        obj_retain(m); return m;
    }
    REQUIRE(2, "min");
    if (ARG(0)->type == OBJ_INT && ARG(1)->type == OBJ_INT)
        return obj_int(ARG(0)->int_val < ARG(1)->int_val ? ARG(0)->int_val : ARG(1)->int_val);
    double a = ARG(0)->type == OBJ_INT ? (double)ARG(0)->int_val : ARG(0)->float_val;
    double b = ARG(1)->type == OBJ_INT ? (double)ARG(1)->int_val : ARG(1)->float_val;
    return obj_float(a < b ? a : b);
}

/* ---- Environment ---- */
static Object *bw_env(ObjList *args) {
    REQUIRE(1, "env");
    if (ARG(0)->type != OBJ_STRING) return obj_error_typef("TypeMismatchError", "env(): requires string");
    const char *v = getenv(ARG(0)->string.str);
    return v ? obj_string(v) : obj_null();
}

static Object *bw_env_or(ObjList *args) {
    REQUIRE(2, "env_or");
    if (ARG(0)->type != OBJ_STRING) return obj_error_typef("TypeMismatchError", "env_or(): requires string");
    const char *v = getenv(ARG(0)->string.str);
    if (v && v[0] != '\0') return obj_string(v);
    obj_retain(ARG(1));
    return ARG(1);
}

/* ---- File I/O ---- */
static Object *bw_read_file(ObjList *args) {
    REQUIRE(1, "read_file");
    if (ARG(0)->type != OBJ_STRING) return obj_error_typef("TypeMismatchError", "read_file(): requires string path");
    FILE *f = fopen(ARG(0)->string.str, "rb");
    if (!f) return obj_error_typef("IOError", "read_file(): cannot open '%s': %s",
                                   ARG(0)->string.str, strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    Object *o = obj_string(buf);
    free(buf);
    return o;
}

static Object *bw_write_file(ObjList *args) {
    REQUIRE(2, "write_file");
    if (ARG(0)->type != OBJ_STRING || ARG(1)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "write_file(): requires (string path, string content)");
    FILE *f = fopen(ARG(0)->string.str, "wb");
    if (!f) return obj_error_typef("IOError", "write_file(): cannot open '%s': %s",
                                   ARG(0)->string.str, strerror(errno));
    fputs(ARG(1)->string.str, f);
    fclose(f);
    return obj_null();
}

static Object *bw_append_file(ObjList *args) {
    REQUIRE(2, "append_file");
    if (ARG(0)->type != OBJ_STRING || ARG(1)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "append_file(): requires (string path, string content)");
    FILE *f = fopen(ARG(0)->string.str, "ab");
    if (!f) return obj_error_typef("IOError", "append_file(): cannot open '%s': %s",
                                   ARG(0)->string.str, strerror(errno));
    fputs(ARG(1)->string.str, f);
    fclose(f);
    return obj_null();
}

/* ---- JSON ---- */
static char *json_encode_obj(Object *o, int indent, int depth);

static char *indent_str(int n) {
    char *s = malloc(n * 2 + 1);
    memset(s, ' ', n * 2);
    s[n * 2] = '\0';
    return s;
}

static char *json_encode_string(const char *src) {
    int len = strlen(src);
    char *out = malloc(len * 2 + 3);
    int i = 0, o = 0;
    out[o++] = '"';
    while (src[i]) {
        unsigned char c = src[i++];
        if      (c == '"')  { out[o++] = '\\'; out[o++] = '"';  }
        else if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n';  }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r';  }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't';  }
        else                  out[o++] = c;
    }
    out[o++] = '"';
    out[o]   = '\0';
    return out;
}

static char *json_encode_obj(Object *o, int indent, int depth) {
    char buf[128];
    if (!o || o->type == OBJ_NULL) return strdup("null");
    switch (o->type) {
        case OBJ_INT:
            snprintf(buf, sizeof(buf), "%lld", o->int_val);
            return strdup(buf);
        case OBJ_FLOAT:
            snprintf(buf, sizeof(buf), "%g", o->float_val);
            return strdup(buf);
        case OBJ_BOOL:
            return strdup(o->bool_val ? "true" : "false");
        case OBJ_STRING:
            return json_encode_string(o->string.str);
        case OBJ_ARRAY: {
            if (o->array.elems.count == 0) return strdup("[]");
            char *ind  = indent ? indent_str(depth + 1) : strdup("");
            char *ind0 = indent ? indent_str(depth)     : strdup("");
            char *out  = strdup("[\n");
            for (int i = 0; i < o->array.elems.count; i++) {
                char *v = json_encode_obj(o->array.elems.items[i], indent, depth + 1);
                int sz = strlen(out) + strlen(ind) + strlen(v) + 4;
                out = realloc(out, sz);
                if (indent) strcat(out, ind);
                strcat(out, v);
                free(v);
                if (i < o->array.elems.count - 1) strcat(out, ",");
                if (indent) strcat(out, "\n");
            }
            int sz = strlen(out) + strlen(ind0) + 4;
            out = realloc(out, sz);
            if (indent) strcat(out, ind0);
            strcat(out, "]");
            free(ind); free(ind0);
            return out;
        }
        case OBJ_HASH: {
            if (o->hash.size == 0) return strdup("{}");
            char *ind  = indent ? indent_str(depth + 1) : strdup("");
            char *ind0 = indent ? indent_str(depth)     : strdup("");
            char *out  = strdup("{\n");
            int first  = 1;
            for (int i = 0; i < 64; i++) {
                HashEntry *e = o->hash.buckets[i];
                while (e) {
                    char *k = json_encode_string(e->key);
                    char *v = json_encode_obj(e->value, indent, depth + 1);
                    int sz = strlen(out) + strlen(ind) + strlen(k) + strlen(v) + 8;
                    out = realloc(out, sz);
                    if (!first) strcat(out, ",\n");
                    if (indent) strcat(out, ind);
                    strcat(out, k);
                    strcat(out, ": ");
                    strcat(out, v);
                    free(k); free(v);
                    first = 0;
                    e = e->next;
                }
            }
            int sz = strlen(out) + strlen(ind0) + 4;
            out = realloc(out, sz);
            if (indent) strcat(out, "\n");
            if (indent) strcat(out, ind0);
            strcat(out, "}");
            free(ind); free(ind0);
            return out;
        }
        default:
            return strdup("null");
    }
}

static Object *bw_json_encode(ObjList *args) {
    REQUIRE(1, "json_encode");
    int pretty = NARGS >= 2 && obj_is_truthy(ARG(1));
    char *s = json_encode_obj(ARG(0), pretty, 0);
    Object *o = obj_string(s);
    free(s);
    return o;
}

/* Simple recursive descent JSON decoder */
typedef struct { const char *src; int pos; } JD;

static void jd_skip_ws(JD *j) {
    while (j->src[j->pos] && isspace((unsigned char)j->src[j->pos])) j->pos++;
}

static Object *jd_parse_value(JD *j);

static Object *jd_parse_string(JD *j) {
    j->pos++; /* skip " */
    int   cap = 64, len = 0;
    char *buf = malloc(cap);
    while (j->src[j->pos] && j->src[j->pos] != '"') {
        char c = j->src[j->pos++];
        if (c == '\\') {
            char e = j->src[j->pos++];
            switch (e) {
                case '"': c='"'; break; case '\\': c='\\'; break;
                case 'n': c='\n'; break; case 't': c='\t'; break;
                case 'r': c='\r'; break; default: c=e; break;
            }
        }
        if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = c;
    }
    buf[len] = '\0';
    if (j->src[j->pos] == '"') j->pos++;
    Object *o = obj_string(buf); free(buf); return o;
}

static Object *jd_parse_number(JD *j) {
    int start = j->pos;
    int is_f  = 0;
    if (j->src[j->pos] == '-') j->pos++;
    while (isdigit((unsigned char)j->src[j->pos])) j->pos++;
    if (j->src[j->pos] == '.') { is_f = 1; j->pos++; while (isdigit((unsigned char)j->src[j->pos])) j->pos++; }
    if (j->src[j->pos] == 'e' || j->src[j->pos] == 'E') {
        is_f = 1; j->pos++;
        if (j->src[j->pos] == '+' || j->src[j->pos] == '-') j->pos++;
        while (isdigit((unsigned char)j->src[j->pos])) j->pos++;
    }
    int   len = j->pos - start;
    char *buf = malloc(len + 1);
    memcpy(buf, j->src + start, len); buf[len] = '\0';
    Object *o = is_f ? obj_float(atof(buf)) : obj_int(atoll(buf));
    free(buf); return o;
}

static Object *jd_parse_array(JD *j) {
    j->pos++; /* skip [ */
    Object *arr = obj_array();
    jd_skip_ws(j);
    while (j->src[j->pos] && j->src[j->pos] != ']') {
        Object *v = jd_parse_value(j);
        if (!v) break;
        array_push(arr, v); obj_release(v);
        jd_skip_ws(j);
        if (j->src[j->pos] == ',') j->pos++;
        jd_skip_ws(j);
    }
    if (j->src[j->pos] == ']') j->pos++;
    return arr;
}

static Object *jd_parse_object(JD *j) {
    j->pos++; /* skip { */
    Object *h = obj_hash();
    jd_skip_ws(j);
    while (j->src[j->pos] && j->src[j->pos] != '}') {
        jd_skip_ws(j);
        if (j->src[j->pos] != '"') break;
        Object *k = jd_parse_string(j);
        jd_skip_ws(j);
        if (j->src[j->pos] == ':') j->pos++;
        jd_skip_ws(j);
        Object *v = jd_parse_value(j);
        if (k && v) hash_set(h, k->string.str, v);
        obj_release(k); obj_release(v);
        jd_skip_ws(j);
        if (j->src[j->pos] == ',') j->pos++;
        jd_skip_ws(j);
    }
    if (j->src[j->pos] == '}') j->pos++;
    return h;
}

static Object *jd_parse_value(JD *j) {
    jd_skip_ws(j);
    char c = j->src[j->pos];
    if (c == '"') return jd_parse_string(j);
    if (c == '[') return jd_parse_array(j);
    if (c == '{') return jd_parse_object(j);
    if (c == '-' || isdigit((unsigned char)c)) return jd_parse_number(j);
    if (strncmp(j->src + j->pos, "true",  4) == 0) { j->pos += 4; return obj_bool(1); }
    if (strncmp(j->src + j->pos, "false", 5) == 0) { j->pos += 5; return obj_bool(0); }
    if (strncmp(j->src + j->pos, "null",  4) == 0) { j->pos += 4; return obj_null(); }
    return NULL;
}

static Object *bw_json_decode(ObjList *args) {
    REQUIRE(1, "json_decode");
    if (ARG(0)->type != OBJ_STRING) return obj_error_typef("TypeMismatchError", "json_decode(): requires string");
    JD j = { ARG(0)->string.str, 0 };
    Object *o = jd_parse_value(&j);
    return o ? o : obj_error_typef("JsonError", "json_decode(): invalid JSON");
}

/* ---- HTTP Server builtins ---- */
static Object *bw_create_server(ObjList *args) {
    if (NARGS < 1 || NARGS > 2)
        return obj_error_typef("ArityError", "create_server() requires 1 or 2 argument(s)");
    if (ARG(0)->type != OBJ_INT) return obj_error_typef("TypeMismatchError", "create_server(): port must be int");
    Object *cb = NULL;
    if (NARGS == 2) {
        if (ARG(1)->type != OBJ_FUNCTION && ARG(1)->type != OBJ_BUILTIN)
            return obj_error_typef("TypeMismatchError", "create_server(): optional callback must be a function");
        cb = ARG(1);
    }
    return obj_http_server((int)ARG(0)->int_val, cb);
}

static Object *bw_route(ObjList *args) {
    REQUIRE(4, "route");
    if (ARG(0)->type != OBJ_HTTP_SERVER) return obj_error_typef("TypeMismatchError", "route(): first arg must be server");
    if (ARG(1)->type != OBJ_STRING)      return obj_error_typef("TypeMismatchError", "route(): method must be string");
    if (ARG(2)->type != OBJ_STRING)      return obj_error_typef("TypeMismatchError", "route(): path must be string");
    if (ARG(3)->type != OBJ_FUNCTION && ARG(3)->type != OBJ_BUILTIN)
        return obj_error_typef("TypeMismatchError", "route(): handler must be function");

    Route *r   = malloc(sizeof(Route));
    r->method  = strdup(ARG(1)->string.str);
    r->query_key = NULL;
    const char *pat = ARG(2)->string.str;
    char *colon = strrchr(pat, ':');
    if (colon && colon > pat && colon[1] != '\0') {
        size_t n = (size_t)(colon - pat);
        r->path = malloc(n + 1);
        memcpy(r->path, pat, n);
        r->path[n] = '\0';
        r->query_key = strdup(colon + 1);
    } else {
        r->path = strdup(pat);
    }
    r->handler = ARG(3);
    obj_retain(r->handler);
    r->next    = ARG(0)->server.routes;
    ARG(0)->server.routes = r;
    return obj_null();
}

/* serve() is called in main.c via http.h */
extern void http_serve(Object *server, Interpreter *it, Env *env);
static Interpreter *_global_interp = NULL;
static Env         *_global_env    = NULL;

static Object *bw_serve(ObjList *args) {
    REQUIRE(1, "serve");
    if (ARG(0)->type != OBJ_HTTP_SERVER) return obj_error_typef("TypeMismatchError", "serve(): argument must be server");
    if (!_global_interp) return obj_error_typef("InternalError", "serve(): interpreter not set");
    http_serve(ARG(0), _global_interp, _global_env);
    return obj_null();
}

void builtins_set_interp(Interpreter *it, Env *env) {
    _global_interp = it;
    _global_env    = env;
}

/* ---- fetch ---- */
static Object *bw_fetch(ObjList *args) {
    if (NARGS < 1 || ARG(0)->type != OBJ_STRING)
        return obj_error_typef("TypeMismatchError", "fetch(): first argument must be a url string");

    const char *url    = ARG(0)->string.str;
    const char *method = "GET";
    Object     *hdrs   = NULL;
    const char *body   = NULL;

    if (NARGS >= 2 && ARG(1)->type == OBJ_HASH) {
        Object *opts = ARG(1);
        Object *m = hash_get(opts, "method");
        if (m && m->type == OBJ_STRING) method = m->string.str;
        Object *h = hash_get(opts, "headers");
        if (h && h->type == OBJ_HASH) hdrs = h;
        Object *b = hash_get(opts, "body");
        if (b && b->type == OBJ_STRING) body = b->string.str;
    }

#ifndef _WIN32
    if (g_running_coro) {
#ifdef BOWIE_CURL
        return event_loop_fetch_async(url, method, hdrs, body);
#else
        /* No curl: do sync request, wrap result in a pre-resolved promise */
        Object *resp    = http_fetch(url, method, hdrs, body);
        Object *promise = obj_promise();
        promise_resolve(promise, resp);
        obj_release(resp);
        return promise;
#endif
    }
#endif
    return http_fetch(url, method, hdrs, body);
}

/* ---- Misc ---- */
static Object *bw_exit(ObjList *args) {
    int code = NARGS > 0 && ARG(0)->type == OBJ_INT ? (int)ARG(0)->int_val : 0;
    exit(code);
}

static Object *bw_assert(ObjList *args) {
    REQUIRE(1, "assert");
    if (!obj_is_truthy(ARG(0))) {
        const char *msg = NARGS > 1 && ARG(1)->type == OBJ_STRING
                          ? ARG(1)->string.str : "assertion failed";
        fprintf(stderr, "AssertionError: %s\n", msg);
        exit(1);
    }
    return obj_null();
}

static Object *bw_input(ObjList *args) {
    if (NARGS > 0) {
        char *s = obj_inspect(ARG(0));
        fputs(s, stdout);
        fflush(stdout);
        free(s);
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return obj_string("");
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return obj_string(buf);
}

static Object *bw_is_null(ObjList *args) {
    REQUIRE(1, "is_null");
    return obj_bool(ARG(0)->type == OBJ_NULL);
}

/* ---- Register ---- */
#define REG(name, fn) env_set(env, name, (tmp = obj_builtin(fn, name))); obj_release(tmp)

void builtins_register(Env *env) {
    Object *tmp;

    /* I/O */
    REG("print",       bw_print);
    REG("println",     bw_println);
    REG("printf",      bw_printf);
    REG("sprintf",     bw_sprintf);
    REG("eprint",      bw_eprint);
    REG("input",       bw_input);

    /* Type conversion */
    REG("str",         bw_str);
    REG("int",         bw_int);
    REG("float",       bw_float);
    REG("bool",        bw_bool);
    REG("type",        bw_type);
    REG("error_type",  bw_error_type);
    REG("is_error_type", bw_is_error_type);

    /* Collections */
    REG("len",         bw_len);
    REG("push",        bw_push);
    REG("pop",         bw_pop);
    REG("keys",        bw_keys);
    REG("values",      bw_values);
    REG("range",       bw_range);
    REG("slice",       bw_slice);
    REG("index_of",    bw_index_of);
    REG("lookup",      bw_lookup);

    /* String */
    REG("split",       bw_split);
    REG("join",        bw_join);
    REG("trim",        bw_trim);
    REG("upper",       bw_upper);
    REG("lower",       bw_lower);
    REG("contains",    bw_contains);
    REG("replace",     bw_replace);
    REG("starts_with", bw_starts_with);
    REG("ends_with",   bw_ends_with);

    /* Math */
    REG("floor",       bw_floor);
    REG("ceil",        bw_ceil);
    REG("abs",         bw_abs);
    REG("sqrt",        bw_sqrt);
    REG("pow",         bw_pow);
    REG("max",         bw_max2);
    REG("min",         bw_min2);

    /* Environment */
    REG("env",         bw_env);
    REG("env_or",      bw_env_or);

    /* File I/O */
    REG("read_file",   bw_read_file);
    REG("write_file",  bw_write_file);
    REG("append_file", bw_append_file);

    /* JSON */
    REG("json_encode", bw_json_encode);
    REG("json_decode", bw_json_decode);

    /* HTTP */
    REG("fetch",          bw_fetch);
    REG("create_server",  bw_create_server);
    REG("route",       bw_route);
    REG("serve",       bw_serve);

    /* PostgreSQL */
    postgres_register(env);

    /* Misc */
    REG("exit",        bw_exit);
    REG("assert",      bw_assert);
    REG("is_null",     bw_is_null);
}
