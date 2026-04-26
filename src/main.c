#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "builtins.h"
#ifndef _WIN32
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include "event_loop.h"
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* Declared in builtins.c */
void builtins_set_interp(Interpreter *it, Env *env);

static char *dup_cstr(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    return copy;
}

/* Locate the std library directory.
 * Priority: $BOWIE_STD env var → sibling "std/" next to binary → "./std" fallback. */
static char *g_std_path = NULL;

/* A valid std dir must contain array.bow — prevents picking up unrelated "std/" dirs */
static int is_valid_std(const char *path) {
    char probe[4096];
    snprintf(probe, sizeof(probe), "%s/array.bow", path);
    FILE *f = fopen(probe, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

static char *self_exe_path(const char *argv0) {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char *real = realpath(buf, NULL);
        if (real) return real;
    }
#elif defined(__linux__)
    char *real = realpath("/proc/self/exe", NULL);
    if (real) return real;
#endif
    /* fallback: try to resolve argv0 via realpath (works when full path given) */
    return realpath(argv0, NULL);
}

static char *find_std_path(const char *argv0) {
    const char *env_std = getenv("BOWIE_STD");
    if (env_std && env_std[0]) return dup_cstr(env_std);

#ifndef _WIN32
    char *bin = self_exe_path(argv0);
    if (bin) {
        char *sl = strrchr(bin, '/');
        if (sl) {
            *sl = '\0'; /* bin is now the directory containing the binary */
            char candidate[4096];

            /* 1. {bin_dir}/std — dev layout: binary sits in project root */
            snprintf(candidate, sizeof(candidate), "%s/std", bin);
            if (is_valid_std(candidate)) { free(bin); return dup_cstr(candidate); }

            /* 2. {bin_dir}/../lib/bowie/std — installed: /usr/local/bin → /usr/local/lib/bowie/std */
            snprintf(candidate, sizeof(candidate), "%s/../lib/bowie/std", bin);
            char *real = realpath(candidate, NULL);
            if (real) {
                if (is_valid_std(real)) { free(bin); return real; }
                free(real);
            }
        }
        free(bin);
    }
#endif
    return NULL; /* not found — ImportError will surface at import time */
}

/* stdin is not seekable; avoid fseek/ftell (can hang or mis-size). */
static char *read_stdin_all(void) {
    size_t cap = 4096, len = 0;
    char  *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - 1 - len, stdin);
        len += n;
        buf[len] = '\0';
        if (n == 0) {
            if (ferror(stdin)) {
                fprintf(stderr, "bowie: error reading stdin: %s\n", strerror(errno));
                free(buf);
                return NULL;
            }
            break;
        }
    }
    return buf;
}

static char *read_file(const char *path) {
    if (!strcmp(path, "/dev/stdin") || !strcmp(path, "-"))
        return read_stdin_all();

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bowie: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int write_file_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "bowie: cannot write '%s': %s\n", path, strerror(errno));
        return 0;
    }
    size_t len = strlen(text);
    if (len > 0 && fwrite(text, 1, len, f) != len) {
        fprintf(stderr, "bowie: write failed for '%s'\n", path);
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

typedef struct {
    int use_tabs;
    int indent_size;
    int max_consecutive_blank_lines;
    int space_before_inline_comment;
    int final_newline;
} FormatConfig;

static void load_format_config(const char *source_path, FormatConfig *cfg);

typedef enum {
    JSONV_OBJECT,
    JSONV_ARRAY,
    JSONV_STRING,
    JSONV_NUMBER,
    JSONV_BOOL,
    JSONV_NULL
} JsonVType;

typedef struct JsonValue JsonValue;

typedef struct {
    char *key;
    JsonValue *value;
} JsonPair;

struct JsonValue {
    JsonVType type;
    union {
        struct {
            JsonPair *items;
            size_t count;
            size_t cap;
        } obj;
        struct {
            JsonValue **items;
            size_t count;
            size_t cap;
        } arr;
        char *str;
        char *num;
        int b;
    } as;
};

static const char *json_skip_ws(const char *p);
static char *json_parse_string(const char **p);

static FormatConfig default_format_config(void) {
    FormatConfig cfg;
    cfg.use_tabs = 0;
    cfg.indent_size = 4;
    cfg.max_consecutive_blank_lines = 1;
    cfg.space_before_inline_comment = 2;
    cfg.final_newline = 1;
    return cfg;
}

static int path_dirname(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return 0;
    size_t len = strlen(path);
    if (len + 1 > out_size) return 0;
    memcpy(out, path, len + 1);
    char *slash = strrchr(out, '/');
    if (!slash) {
        if (out_size < 2) return 0;
        out[0] = '.';
        out[1] = '\0';
        return 1;
    }
    if (slash == out) {
        out[1] = '\0';
        return 1;
    }
    *slash = '\0';
    return 1;
}

static int find_nearest_bowie_json(const char *file_path, char *out, size_t out_size) {
    char dir[4096];
    if (!path_dirname(file_path, dir, sizeof(dir))) return 0;

    while (1) {
        char probe[4096];
        snprintf(probe, sizeof(probe), "%s/bowie.json", dir);
        FILE *f = fopen(probe, "r");
        if (f) {
            fclose(f);
            size_t plen = strlen(probe);
            if (plen + 1 > out_size) return 0;
            memcpy(out, probe, plen + 1);
            return 1;
        }
        char *slash = strrchr(dir, '/');
        if (!slash) break;
        if (slash == dir) {
            dir[1] = '\0';
            snprintf(probe, sizeof(probe), "%s/bowie.json", dir);
            f = fopen(probe, "r");
            if (f) {
                fclose(f);
                size_t plen = strlen(probe);
                if (plen + 1 > out_size) return 0;
                memcpy(out, probe, plen + 1);
                return 1;
            }
            break;
        }
        *slash = '\0';
    }
    return 0;
}

static const char *json_skip_ws_local(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *json_skip_compound_local(const char *p) {
    if (*p != '{' && *p != '[') return p;
    char open = *p;
    char close = (open == '{') ? '}' : ']';
    int depth = 1;
    p++;
    while (*p && depth > 0) {
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == '"') { p++; break; }
                p++;
            }
            continue;
        }
        if (*p == open) depth++;
        if (*p == close) depth--;
        p++;
    }
    return p;
}

static int parse_json_int_local(const char **p, int *out) {
    const char *q = json_skip_ws_local(*p);
    int sign = 1;
    if (*q == '-') { sign = -1; q++; }
    if (!isdigit((unsigned char)*q)) return 0;
    long v = 0;
    while (isdigit((unsigned char)*q)) {
        v = v * 10 + (*q - '0');
        q++;
    }
    *out = (int)(sign * v);
    *p = q;
    return 1;
}

static int parse_json_bool_local(const char **p, int *out) {
    const char *q = json_skip_ws_local(*p);
    if (strncmp(q, "true", 4) == 0) {
        *out = 1;
        *p = q + 4;
        return 1;
    }
    if (strncmp(q, "false", 5) == 0) {
        *out = 0;
        *p = q + 5;
        return 1;
    }
    return 0;
}

static int split_comment(const char *line, char **code_out, char **comment_out) {
    int in_string = 0;
    char quote = '\0';
    int escaped = 0;
    size_t i = 0;
    size_t len = strlen(line);
    size_t comment_idx = len;

    for (i = 0; i < len; i++) {
        char ch = line[i];
        if (in_string) {
            if (escaped) escaped = 0;
            else if (ch == '\\') escaped = 1;
            else if (ch == quote) { in_string = 0; quote = '\0'; }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_string = 1;
            quote = ch;
            continue;
        }
        if (ch == '#') { comment_idx = i; break; }
    }

    size_t code_len = comment_idx;
    while (code_len > 0 && (line[code_len - 1] == ' ' || line[code_len - 1] == '\t'))
        code_len--;

    size_t comment_start = comment_idx;
    while (comment_start < len && (line[comment_start] == ' ' || line[comment_start] == '\t'))
        comment_start++;

    size_t comment_len = (comment_start < len) ? (len - comment_start) : 0;

    *code_out = malloc(code_len + 1);
    *comment_out = malloc(comment_len + 1);
    if (!*code_out || !*comment_out) {
        free(*code_out);
        free(*comment_out);
        *code_out = NULL;
        *comment_out = NULL;
        return 0;
    }

    memcpy(*code_out, line, code_len);
    (*code_out)[code_len] = '\0';
    if (comment_len > 0) memcpy(*comment_out, line + comment_start, comment_len);
    (*comment_out)[comment_len] = '\0';
    return 1;
}

static int append_bytes(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap ? *cap : 64;
        while (*len + n + 1 > new_cap) new_cap *= 2;
        char *nb = realloc(*buf, new_cap);
        if (!nb) return 0;
        *buf = nb;
        *cap = new_cap;
    }
    if (n > 0) memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static int append_c(char **buf, size_t *len, size_t *cap, char c) {
    return append_bytes(buf, len, cap, &c, 1);
}

static int append_indent(char **buf, size_t *len, size_t *cap, int level, const FormatConfig *cfg) {
    int safe_level = level < 0 ? 0 : level;
    if (cfg->use_tabs) {
        for (int i = 0; i < safe_level; i++) {
            if (!append_c(buf, len, cap, '\t')) return 0;
        }
        return 1;
    }
    for (int i = 0; i < safe_level * cfg->indent_size; i++) {
        if (!append_c(buf, len, cap, ' ')) return 0;
    }
    return 1;
}

static JsonValue *jsonv_new(JsonVType type) {
    JsonValue *v = malloc(sizeof(JsonValue));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

static void jsonv_free(JsonValue *v) {
    if (!v) return;
    if (v->type == JSONV_OBJECT) {
        for (size_t i = 0; i < v->as.obj.count; i++) {
            free(v->as.obj.items[i].key);
            jsonv_free(v->as.obj.items[i].value);
        }
        free(v->as.obj.items);
    } else if (v->type == JSONV_ARRAY) {
        for (size_t i = 0; i < v->as.arr.count; i++) {
            jsonv_free(v->as.arr.items[i]);
        }
        free(v->as.arr.items);
    } else if (v->type == JSONV_STRING) {
        free(v->as.str);
    } else if (v->type == JSONV_NUMBER) {
        free(v->as.num);
    }
    free(v);
}

static int jsonv_obj_push(JsonValue *obj, char *key, JsonValue *value) {
    if (obj->as.obj.count + 1 > obj->as.obj.cap) {
        size_t nc = obj->as.obj.cap ? obj->as.obj.cap * 2 : 8;
        JsonPair *ni = realloc(obj->as.obj.items, nc * sizeof(JsonPair));
        if (!ni) return 0;
        obj->as.obj.items = ni;
        obj->as.obj.cap = nc;
    }
    obj->as.obj.items[obj->as.obj.count].key = key;
    obj->as.obj.items[obj->as.obj.count].value = value;
    obj->as.obj.count++;
    return 1;
}

static int jsonv_arr_push(JsonValue *arr, JsonValue *value) {
    if (arr->as.arr.count + 1 > arr->as.arr.cap) {
        size_t nc = arr->as.arr.cap ? arr->as.arr.cap * 2 : 8;
        JsonValue **ni = realloc(arr->as.arr.items, nc * sizeof(JsonValue *));
        if (!ni) return 0;
        arr->as.arr.items = ni;
        arr->as.arr.cap = nc;
    }
    arr->as.arr.items[arr->as.arr.count++] = value;
    return 1;
}

static JsonValue *jsonv_parse_value(const char **p);

static JsonValue *jsonv_parse_number(const char **p) {
    const char *start = *p;
    const char *q = *p;
    if (*q == '-') q++;
    if (!isdigit((unsigned char)*q)) return NULL;
    if (*q == '0') q++;
    else while (isdigit((unsigned char)*q)) q++;
    if (*q == '.') {
        q++;
        if (!isdigit((unsigned char)*q)) return NULL;
        while (isdigit((unsigned char)*q)) q++;
    }
    if (*q == 'e' || *q == 'E') {
        q++;
        if (*q == '+' || *q == '-') q++;
        if (!isdigit((unsigned char)*q)) return NULL;
        while (isdigit((unsigned char)*q)) q++;
    }
    size_t len = (size_t)(q - start);
    char *num = malloc(len + 1);
    if (!num) return NULL;
    memcpy(num, start, len);
    num[len] = '\0';
    JsonValue *v = jsonv_new(JSONV_NUMBER);
    if (!v) { free(num); return NULL; }
    v->as.num = num;
    *p = q;
    return v;
}

static JsonValue *jsonv_parse_array(const char **p) {
    if (**p != '[') return NULL;
    (*p)++;
    JsonValue *arr = jsonv_new(JSONV_ARRAY);
    if (!arr) return NULL;
    *p = json_skip_ws(*p);
    if (**p == ']') { (*p)++; return arr; }
    while (1) {
        *p = json_skip_ws(*p);
        JsonValue *item = jsonv_parse_value(p);
        if (!item) { jsonv_free(arr); return NULL; }
        if (!jsonv_arr_push(arr, item)) { jsonv_free(item); jsonv_free(arr); return NULL; }
        *p = json_skip_ws(*p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') { (*p)++; break; }
        jsonv_free(arr);
        return NULL;
    }
    return arr;
}

static JsonValue *jsonv_parse_object(const char **p) {
    if (**p != '{') return NULL;
    (*p)++;
    JsonValue *obj = jsonv_new(JSONV_OBJECT);
    if (!obj) return NULL;
    *p = json_skip_ws(*p);
    if (**p == '}') { (*p)++; return obj; }
    while (1) {
        *p = json_skip_ws(*p);
        if (**p != '"') { jsonv_free(obj); return NULL; }
        char *key = json_parse_string(p);
        if (!key) { jsonv_free(obj); return NULL; }
        *p = json_skip_ws(*p);
        if (**p != ':') { free(key); jsonv_free(obj); return NULL; }
        (*p)++;
        *p = json_skip_ws(*p);
        JsonValue *val = jsonv_parse_value(p);
        if (!val) { free(key); jsonv_free(obj); return NULL; }
        if (!jsonv_obj_push(obj, key, val)) {
            free(key);
            jsonv_free(val);
            jsonv_free(obj);
            return NULL;
        }
        *p = json_skip_ws(*p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; break; }
        jsonv_free(obj);
        return NULL;
    }
    return obj;
}

static JsonValue *jsonv_parse_value(const char **p) {
    *p = json_skip_ws(*p);
    if (**p == '{') return jsonv_parse_object(p);
    if (**p == '[') return jsonv_parse_array(p);
    if (**p == '"') {
        char *s = json_parse_string(p);
        if (!s) return NULL;
        JsonValue *v = jsonv_new(JSONV_STRING);
        if (!v) { free(s); return NULL; }
        v->as.str = s;
        return v;
    }
    if (strncmp(*p, "true", 4) == 0) {
        JsonValue *v = jsonv_new(JSONV_BOOL);
        if (!v) return NULL;
        v->as.b = 1;
        *p += 4;
        return v;
    }
    if (strncmp(*p, "false", 5) == 0) {
        JsonValue *v = jsonv_new(JSONV_BOOL);
        if (!v) return NULL;
        v->as.b = 0;
        *p += 5;
        return v;
    }
    if (strncmp(*p, "null", 4) == 0) {
        JsonValue *v = jsonv_new(JSONV_NULL);
        if (!v) return NULL;
        *p += 4;
        return v;
    }
    return jsonv_parse_number(p);
}

static int jsonv_append_escaped(char **buf, size_t *len, size_t *cap, const char *s) {
    if (!append_c(buf, len, cap, '"')) return 0;
    for (size_t i = 0; s && s[i]; i++) {
        char ch = s[i];
        if (ch == '"' || ch == '\\') {
            if (!append_c(buf, len, cap, '\\') || !append_c(buf, len, cap, ch)) return 0;
        } else if (ch == '\n') {
            if (!append_bytes(buf, len, cap, "\\n", 2)) return 0;
        } else if (ch == '\r') {
            if (!append_bytes(buf, len, cap, "\\r", 2)) return 0;
        } else if (ch == '\t') {
            if (!append_bytes(buf, len, cap, "\\t", 2)) return 0;
        } else {
            if (!append_c(buf, len, cap, ch)) return 0;
        }
    }
    return append_c(buf, len, cap, '"');
}

static int json_key_priority(const char *key) {
    if (strcmp(key, "name") == 0) return 0;
    if (strcmp(key, "version") == 0) return 1;
    if (strcmp(key, "description") == 0) return 2;
    if (strcmp(key, "dependencies") == 0) return 3;
    if (strcmp(key, "tasks") == 0) return 4;
    if (strcmp(key, "format") == 0) return 5;
    return 100;
}

static int json_key_cmp(const JsonPair *a, const JsonPair *b, int top_level) {
    if (top_level) {
        int pa = json_key_priority(a->key);
        int pb = json_key_priority(b->key);
        if (pa != pb) return (pa < pb) ? -1 : 1;
    }
    return strcmp(a->key, b->key);
}

static void jsonv_sort_object(JsonValue *obj, int top_level);

static void jsonv_sort(JsonValue *v, int top_level) {
    if (!v) return;
    if (v->type == JSONV_OBJECT) {
        jsonv_sort_object(v, top_level);
    } else if (v->type == JSONV_ARRAY) {
        for (size_t i = 0; i < v->as.arr.count; i++) jsonv_sort(v->as.arr.items[i], 0);
    }
}

static void jsonv_sort_object(JsonValue *obj, int top_level) {
    for (size_t i = 0; i < obj->as.obj.count; i++) {
        jsonv_sort(obj->as.obj.items[i].value, 0);
    }
    for (size_t i = 0; i < obj->as.obj.count; i++) {
        for (size_t j = i + 1; j < obj->as.obj.count; j++) {
            if (json_key_cmp(&obj->as.obj.items[j], &obj->as.obj.items[i], top_level) < 0) {
                JsonPair tmp = obj->as.obj.items[i];
                obj->as.obj.items[i] = obj->as.obj.items[j];
                obj->as.obj.items[j] = tmp;
            }
        }
    }
}

static int jsonv_serialize(JsonValue *v, int indent, char **buf, size_t *len, size_t *cap) {
    if (v->type == JSONV_NULL) return append_bytes(buf, len, cap, "null", 4);
    if (v->type == JSONV_BOOL) return append_bytes(buf, len, cap, v->as.b ? "true" : "false", v->as.b ? 4 : 5);
    if (v->type == JSONV_NUMBER) return append_bytes(buf, len, cap, v->as.num, strlen(v->as.num));
    if (v->type == JSONV_STRING) return jsonv_append_escaped(buf, len, cap, v->as.str);
    if (v->type == JSONV_ARRAY) {
        if (!append_c(buf, len, cap, '[')) return 0;
        if (v->as.arr.count == 0) return append_c(buf, len, cap, ']');
        if (!append_c(buf, len, cap, '\n')) return 0;
        for (size_t i = 0; i < v->as.arr.count; i++) {
            for (int s = 0; s < indent + 1; s++) if (!append_bytes(buf, len, cap, "  ", 2)) return 0;
            if (!jsonv_serialize(v->as.arr.items[i], indent + 1, buf, len, cap)) return 0;
            if (i + 1 < v->as.arr.count && !append_c(buf, len, cap, ',')) return 0;
            if (!append_c(buf, len, cap, '\n')) return 0;
        }
        for (int s = 0; s < indent; s++) if (!append_bytes(buf, len, cap, "  ", 2)) return 0;
        return append_c(buf, len, cap, ']');
    }
    if (!append_c(buf, len, cap, '{')) return 0;
    if (v->as.obj.count == 0) return append_c(buf, len, cap, '}');
    if (!append_c(buf, len, cap, '\n')) return 0;
    for (size_t i = 0; i < v->as.obj.count; i++) {
        for (int s = 0; s < indent + 1; s++) if (!append_bytes(buf, len, cap, "  ", 2)) return 0;
        if (!jsonv_append_escaped(buf, len, cap, v->as.obj.items[i].key)) return 0;
        if (!append_bytes(buf, len, cap, ": ", 2)) return 0;
        if (!jsonv_serialize(v->as.obj.items[i].value, indent + 1, buf, len, cap)) return 0;
        if (i + 1 < v->as.obj.count && !append_c(buf, len, cap, ',')) return 0;
        if (!append_c(buf, len, cap, '\n')) return 0;
    }
    for (int s = 0; s < indent; s++) if (!append_bytes(buf, len, cap, "  ", 2)) return 0;
    return append_c(buf, len, cap, '}');
}

static int sort_bowie_json_file(const char *path) {
    char *src = read_file(path);
    if (!src) return 1;
    const char *p = src;
    JsonValue *root = jsonv_parse_value(&p);
    p = json_skip_ws(p);
    if (!root || root->type != JSONV_OBJECT || *p != '\0') {
        fprintf(stderr, "bowie: '%s' is not valid JSON object content\n", path);
        jsonv_free(root);
        free(src);
        return 1;
    }

    jsonv_sort(root, 1);
    char *out = NULL;
    size_t len = 0, cap = 0;
    int ok = jsonv_serialize(root, 0, &out, &len, &cap);
    if (ok) ok = append_c(&out, &len, &cap, '\n');
    if (!ok) {
        fprintf(stderr, "bowie: failed to serialize sorted json for '%s'\n", path);
        free(out);
        jsonv_free(root);
        free(src);
        return 1;
    }

    int wr = write_file_text(path, out);
    free(out);
    jsonv_free(root);
    free(src);
    return wr ? 0 : 1;
}

static int cmd_sort_recursive(const char *path, int *sorted_count) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "bowie: cannot access '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        int error_count = 0;
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "bowie: cannot open directory '%s': %s\n", path, strerror(errno));
            return 1;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            if (name[0] == '.') continue;
            if (strcmp(name, "bowie_modules") == 0) continue;
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, name);
            int rc = cmd_sort_recursive(child, sorted_count);
            if (rc != 0) error_count++;
        }
        closedir(dir);
        return error_count == 0 ? 0 : 1;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "bowie.json") != 0) return 0;
    int rc = sort_bowie_json_file(path);
    if (rc == 0) (*sorted_count)++;
    return rc;
}

static int cmd_sort(const char *path) {
    if (!path || !path[0]) path = "bowie.json";
    int sorted_count = 0;
    int rc = cmd_sort_recursive(path, &sorted_count);
    if (rc == 0 && sorted_count == 0) {
        fprintf(stderr, "bowie: no bowie.json files found\n");
    }
    return rc;
}

static int normalize_code_spacing(const char *code, char **outp) {
    char *out = NULL;
    size_t len = 0, cap = 0;
    int in_string = 0, escaped = 0;
    char quote = '\0';
    char prev_token = '\0'; /* 'o' operator, 'i' identifier, delimiters as-is */

    for (size_t i = 0; code[i]; i++) {
        char ch = code[i];
        char next = code[i + 1];

        if (in_string) {
            if (!append_c(&out, &len, &cap, ch)) { free(out); return 0; }
            if (escaped) escaped = 0;
            else if (ch == '\\') escaped = 1;
            else if (ch == quote) { in_string = 0; quote = '\0'; }
            prev_token = 'i';
            continue;
        }

        if (ch == '"' || ch == '\'') {
            if (!append_c(&out, &len, &cap, ch)) { free(out); return 0; }
            in_string = 1;
            quote = ch;
            prev_token = 'i';
            continue;
        }

        if (ch == ' ' || ch == '\t') {
            if (len > 0 && out[len - 1] != ' ' &&
                out[len - 1] != '(' && out[len - 1] != '[' && out[len - 1] != '{') {
                if (!append_c(&out, &len, &cap, ' ')) { free(out); return 0; }
            }
            continue;
        }

        if ((ch == '=' && next == '=') || (ch == '!' && next == '=') ||
            (ch == '<' && next == '=') || (ch == '>' && next == '=') ||
            (ch == '&' && next == '&') || (ch == '|' && next == '|') ||
            (ch == '+' && next == '=') || (ch == '-' && next == '=') ||
            (ch == '*' && next == '=') || (ch == '/' && next == '=') ||
            (ch == '%' && next == '=')) {
            while (len > 0 && out[len - 1] == ' ') len--;
            if (out) out[len] = '\0';
            if (len > 0 && !append_c(&out, &len, &cap, ' ')) { free(out); return 0; }
            if (!append_c(&out, &len, &cap, ch) || !append_c(&out, &len, &cap, next) ||
                !append_c(&out, &len, &cap, ' ')) { free(out); return 0; }
            i++;
            prev_token = 'o';
            continue;
        }

        if (ch == ',' || ch == ';' || ch == ')' || ch == ']' || ch == '}') {
            while (len > 0 && out[len - 1] == ' ') len--;
            if (out) out[len] = '\0';
            if (!append_c(&out, &len, &cap, ch)) { free(out); return 0; }
            if (next && next != ',' && next != ';' && next != ')' &&
                next != ']' && next != '}') {
                if (!append_c(&out, &len, &cap, ' ')) { free(out); return 0; }
            }
            prev_token = ch;
            continue;
        }

        if (ch == '(' || ch == '[' || ch == '{') {
            while (len > 0 && out[len - 1] == ' ') len--;
            if (out) out[len] = '\0';
            if (ch == '{' && len > 0 && out[len - 1] != ' ' &&
                out[len - 1] != '{' && out[len - 1] != '[' &&
                out[len - 1] != '(' && out[len - 1] != '\n') {
                if (!append_c(&out, &len, &cap, ' ')) { free(out); return 0; }
            }
            if (!append_c(&out, &len, &cap, ch)) { free(out); return 0; }
            prev_token = ch;
            continue;
        }

        if (ch == ':') {
            while (len > 0 && out[len - 1] == ' ') len--;
            if (out) out[len] = '\0';
            if (!append_c(&out, &len, &cap, ':') || !append_c(&out, &len, &cap, ' ')) {
                free(out);
                return 0;
            }
            prev_token = ':';
            continue;
        }

        if (ch == '=' || ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
            ch == '%' || ch == '<' || ch == '>' || ch == '?') {
            int unary_minus = (ch == '-') &&
                              (prev_token == '\0' || prev_token == '(' || prev_token == '[' ||
                               prev_token == '{' || prev_token == ',' || prev_token == ':' ||
                               prev_token == 'o');
            if (unary_minus) {
                while (len > 0 && out[len - 1] == ' ') len--;
                if (out) out[len] = '\0';
                if (!append_c(&out, &len, &cap, '-')) { free(out); return 0; }
            } else {
                while (len > 0 && out[len - 1] == ' ') len--;
                if (out) out[len] = '\0';
                if (len > 0 && !append_c(&out, &len, &cap, ' ')) { free(out); return 0; }
                if (!append_c(&out, &len, &cap, ch) || !append_c(&out, &len, &cap, ' ')) {
                    free(out);
                    return 0;
                }
            }
            prev_token = 'o';
            continue;
        }

        if (ch == '!' && next != '=') {
            while (len > 0 && out[len - 1] == ' ') len--;
            if (out) out[len] = '\0';
            if (!append_c(&out, &len, &cap, '!')) { free(out); return 0; }
            prev_token = 'o';
            continue;
        }

        if (!append_c(&out, &len, &cap, ch)) { free(out); return 0; }
        prev_token = 'i';
    }

    while (len > 0 && out[len - 1] == ' ') len--;
    if (!out) {
        out = malloc(1);
        if (!out) return 0;
        out[0] = '\0';
    } else {
        out[len] = '\0';
    }
    *outp = out;
    return 1;
}

static void count_delimiters(const char *code, int *opens, int *closes) {
    int in_string = 0, escaped = 0;
    char quote = '\0';
    *opens = 0;
    *closes = 0;

    for (size_t i = 0; code[i]; i++) {
        char ch = code[i];
        if (in_string) {
            if (escaped) escaped = 0;
            else if (ch == '\\') escaped = 1;
            else if (ch == quote) { in_string = 0; quote = '\0'; }
            continue;
        }
        if (ch == '"' || ch == '\'') { in_string = 1; quote = ch; continue; }
        if (ch == '{' || ch == '[') (*opens)++;
        if (ch == '}' || ch == ']') (*closes)++;
    }
}

static int leading_closers(const char *code) {
    int count = 0;
    for (size_t i = 0; code[i]; i++) {
        char ch = code[i];
        if (ch == '}' || ch == ']') {
            count++;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == ',' || ch == ';') continue;
        break;
    }
    return count;
}

static int split_trailing_closers(const char *code, char **body_out, char **closers_out) {
    size_t len = strlen(code);
    if (len == 0) return 0;

    size_t split = len;
    while (split > 0) {
        char ch = code[split - 1];
        if (ch == ')' || ch == ']' || ch == '}') {
            split--;
            continue;
        }
        break;
    }

    if (split == len || split == 0) return 0;
    if (!strchr(code + split, '}') && !strchr(code + split, ']')) return 0;

    size_t body_end = split;
    while (body_end > 0 && (code[body_end - 1] == ' ' || code[body_end - 1] == '\t')) body_end--;
    if (body_end == 0) return 0;

    char *body = malloc(body_end + 1);
    char *closers = malloc((len - split) + 1);
    if (!body || !closers) {
        free(body);
        free(closers);
        return 0;
    }
    memcpy(body, code, body_end);
    body[body_end] = '\0';
    memcpy(closers, code + split, len - split);
    closers[len - split] = '\0';

    *body_out = body;
    *closers_out = closers;
    return 1;
}

static int format_bowie(const char *text, const FormatConfig *cfg, char **outp) {
    size_t out_len = 0, out_cap = 0;
    char *out = NULL;
    int indent_level = 0;
    int consecutive_empty = 0;
    const char *p = text;

    while (1) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);
        if (*p == '\n') p++;
        if (line_len > 0 && line_start[line_len - 1] == '\r') line_len--;

        char *line = malloc(line_len + 1);
        if (!line) { free(out); return 0; }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        size_t left = 0, right = line_len;
        while (left < line_len && (line[left] == ' ' || line[left] == '\t')) left++;
        while (right > left && (line[right - 1] == ' ' || line[right - 1] == '\t')) right--;
        int is_empty = (left == right);

        if (is_empty) {
            if (consecutive_empty < cfg->max_consecutive_blank_lines) {
                if (!append_c(&out, &out_len, &out_cap, '\n')) {
                    free(line);
                    free(out);
                    return 0;
                }
            }
            consecutive_empty++;
            free(line);
            if (!*p) break;
            continue;
        }

        consecutive_empty = 0;
        char *trimmed = malloc((right - left) + 1);
        if (!trimmed) { free(line); free(out); return 0; }
        memcpy(trimmed, line + left, right - left);
        trimmed[right - left] = '\0';
        free(line);

        char *code = NULL;
        char *comment = NULL;
        if (!split_comment(trimmed, &code, &comment)) {
            free(trimmed);
            free(out);
            return 0;
        }
        free(trimmed);

        int dedent = leading_closers(code);
        if (dedent > indent_level) dedent = indent_level;
        int current_indent = indent_level - dedent;
        if (!append_indent(&out, &out_len, &out_cap, current_indent, cfg)) {
            free(code);
            free(comment);
            free(out);
            return 0;
        }

        char *normalized = NULL;
        if (!normalize_code_spacing(code, &normalized)) {
            free(code);
            free(comment);
            free(out);
            return 0;
        }

        char *trailing_body = NULL;
        char *trailing_closers = NULL;
        int has_trailing_closers = normalized[0] &&
                                   split_trailing_closers(normalized, &trailing_body, &trailing_closers);

        if (has_trailing_closers) {
            if (!append_bytes(&out, &out_len, &out_cap, trailing_body, strlen(trailing_body))) {
                free(code); free(comment); free(normalized); free(trailing_body); free(trailing_closers); free(out); return 0;
            }
            if (comment[0]) {
                for (int i = 0; i < cfg->space_before_inline_comment; i++) {
                    if (!append_c(&out, &out_len, &out_cap, ' ')) {
                        free(code); free(comment); free(normalized); free(trailing_body); free(trailing_closers); free(out); return 0;
                    }
                }
                if (!append_bytes(&out, &out_len, &out_cap, comment, strlen(comment))) {
                    free(code); free(comment); free(normalized); free(trailing_body); free(trailing_closers); free(out); return 0;
                }
            }
            if (!append_c(&out, &out_len, &out_cap, '\n')) {
                free(code); free(comment); free(normalized); free(trailing_body); free(trailing_closers); free(out); return 0;
            }

            int closer_dedent = (trailing_closers[0] == '}' || trailing_closers[0] == ']') ? 1 : 0;
            int closer_indent = indent_level - closer_dedent;
            if (closer_indent < 0) closer_indent = 0;
            if (!append_indent(&out, &out_len, &out_cap, closer_indent, cfg)) {
                free(code); free(comment); free(normalized); free(trailing_body); free(trailing_closers); free(out); return 0;
            }
            if (!append_bytes(&out, &out_len, &out_cap, trailing_closers, strlen(trailing_closers)) ||
                !append_c(&out, &out_len, &out_cap, '\n')) {
                free(code); free(comment); free(normalized); free(trailing_body); free(trailing_closers); free(out); return 0;
            }
        } else {
            if (normalized[0]) {
                if (!append_bytes(&out, &out_len, &out_cap, normalized, strlen(normalized))) {
                    free(code);
                    free(comment);
                    free(normalized);
                    free(out);
                    return 0;
                }
            }
            if (comment[0]) {
                if (normalized[0]) {
                    for (int i = 0; i < cfg->space_before_inline_comment; i++) {
                        if (!append_c(&out, &out_len, &out_cap, ' ')) {
                            free(code); free(comment); free(normalized); free(out); return 0;
                        }
                    }
                }
                if (!append_bytes(&out, &out_len, &out_cap, comment, strlen(comment))) {
                    free(code); free(comment); free(normalized); free(out); return 0;
                }
            }
            if (!append_c(&out, &out_len, &out_cap, '\n')) {
                free(code); free(comment); free(normalized); free(out); return 0;
            }
        }

        int opens = 0, closes = 0;
        count_delimiters(code, &opens, &closes);
        indent_level += opens - closes;
        if (indent_level < 0) indent_level = 0;

        free(code);
        free(comment);
        free(normalized);
        free(trailing_body);
        free(trailing_closers);

        if (!*p) break;
    }

    while (out_len > 0 && out[out_len - 1] == '\n') out_len--;
    if (out) out[out_len] = '\0';

    if (cfg->final_newline) {
        if (!append_c(&out, &out_len, &out_cap, '\n')) { free(out); return 0; }
    }

    *outp = out;
    return 1;
}

static int cmd_format_recursive(const char *path, int write_back, int *formatted_count) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "bowie: cannot access '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        int error_count = 0;
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "bowie: cannot open directory '%s': %s\n", path, strerror(errno));
            return 1;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            if (name[0] == '.') continue;
            if (strcmp(name, "bowie_modules") == 0) continue;

            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, name);
            if (stat(child, &st) != 0) {
                error_count++;
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                int sub = cmd_format_recursive(child, write_back, formatted_count);
                if (sub != 0) error_count++;
                continue;
            }

            size_t nlen = strlen(name);
            if (nlen < 4 || strcmp(name + (nlen - 4), ".bow") != 0) continue;

            int file_rc = cmd_format_recursive(child, write_back, formatted_count);
            if (file_rc != 0) error_count++;
        }
        closedir(dir);
        return (error_count == 0) ? 0 : 1;
    }

    size_t path_len = strlen(path);
    if (path_len < 4 || strcmp(path + (path_len - 4), ".bow") != 0) return 0;

    char *src = read_file(path);
    if (!src) return 1;

    FormatConfig cfg = default_format_config();
    load_format_config(path, &cfg);

    char *formatted = NULL;
    if (!format_bowie(src, &cfg, &formatted)) {
        free(src);
        fprintf(stderr, "bowie: formatter failed due to memory error\n");
        return 1;
    }

    int ok = 1;
    if (write_back) {
        ok = write_file_text(path, formatted);
    } else {
        fputs(formatted, stdout);
    }

    free(src);
    free(formatted);
    if (ok) (*formatted_count)++;
    return ok ? 0 : 1;
}

static int cmd_format(const char *path, int write_back) {
    if (!path || !path[0]) {
        fprintf(stderr, "bowie: format requires a file path\n");
        return 1;
    }

    int formatted_count = 0;
    int rc = cmd_format_recursive(path, write_back, &formatted_count);
    if (strcmp(path, ".") == 0 && rc == 0 && formatted_count == 0) {
        fprintf(stderr, "bowie: no .bow files found under current directory\n");
    }
    return rc;
}

static void run_source(const char *source, const char *filename) {
    Lexer  *lexer  = lexer_new(source);
    Parser *parser = parser_new(lexer);
    Node   *ast    = parser_parse(parser);

    if (parser->error) {
        fprintf(stderr, "Parse error in %s:\n  %s\n", filename, parser->error);
        node_free(ast);
        parser_free(parser);
        lexer_free(lexer);
        exit(1);
    }

    Interpreter *interp  = interp_new();
    interp->current_file = (char *)filename;

    /* Compute project root: walk up from the entry-point file's directory until
       a directory containing bowie.json is found. Falls back to the entry dir. */
    {
        char *abs = realpath(filename, NULL);
        char *entry_dir = NULL;
        if (abs) {
            char *sl = strrchr(abs, '/');
            if (sl) *sl = '\0';
            entry_dir = abs;
        }
        if (!entry_dir) {
            char *copy = dup_cstr(filename);
            if (copy) {
                char *sl = strrchr(copy, '/');
                if (sl) { *sl = '\0'; entry_dir = copy; }
                else    { free(copy); entry_dir = dup_cstr("."); }
            }
        }
        char *found = NULL;
        if (entry_dir) {
            char *dir = dup_cstr(entry_dir);
            while (dir) {
                char probe[4096];
                snprintf(probe, sizeof(probe), "%s/bowie.json", dir);
                FILE *f = fopen(probe, "r");
                if (f) { fclose(f); found = dir; dir = NULL; break; }
                char *sl = strrchr(dir, '/');
                if (!sl || sl == dir) { free(dir); dir = NULL; break; }
                *sl = '\0';
            }
        }
        interp->project_root = found ? found : entry_dir;
        if (found && entry_dir) free(entry_dir);
        if (!interp->project_root) interp->project_root = dup_cstr(".");
    }

    interp->std_path = g_std_path ? dup_cstr(g_std_path) : dup_cstr("std");
    builtins_set_interp(interp, interp->globals);

#ifndef _WIN32
    event_loop_init();
#endif

    Object *result = interp_eval(interp, ast, interp->globals);

    if (result && result->type == OBJ_ERROR) {
        fprintf(stderr, "Runtime error in %s:\n  %s: %s\n",
                filename,
                result->error.type ? result->error.type : "RuntimeError",
                result->error.msg);
        obj_release(result);
        interp_free(interp);
        node_free(ast);
        parser_free(parser);
        lexer_free(lexer);
        exit(1);
    }

    obj_release(result);

#ifndef _WIN32
    event_loop_run();
    event_loop_free();
#endif

    interp_free(interp);
    node_free(ast);
    parser_free(parser);
    lexer_free(lexer);
}

static void repl(void) {
    printf("Bowie REPL (type 'exit()' to quit)\n");

    Interpreter *interp = interp_new();
    interp->std_path = g_std_path ? dup_cstr(g_std_path) : dup_cstr("std");
    builtins_set_interp(interp, interp->globals);

    char line[4096];
    for (;;) {
        printf(">> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (!line[0]) continue;

        Lexer  *lexer  = lexer_new(line);
        Parser *parser = parser_new(lexer);
        Node   *ast    = parser_parse(parser);

        if (parser->error) {
            fprintf(stderr, "Parse error: %s\n", parser->error);
        } else {
            Object *result = interp_eval(interp, ast, interp->globals);
            if (result) {
                if (result->type == OBJ_ERROR) {
                    fprintf(stderr, "Error (%s): %s\n",
                            result->error.type ? result->error.type : "RuntimeError",
                            result->error.msg);
                } else if (result->type != OBJ_NULL) {
                    char *s = obj_inspect(result);
                    printf("%s\n", s);
                    free(s);
                }
                obj_release(result);
            }
        }

        node_free(ast);
        parser_free(parser);
        lexer_free(lexer);
    }

    interp_free(interp);
    printf("\nBye!\n");
}

static int load_env_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "bowie: cannot open env file '%s': %s\n", path, strerror(errno));
        return 0;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;
        if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 7;
            while (*p == ' ' || *p == '\t') p++;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;

        /* key */
        char *key_end = eq;
        while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
        size_t key_len = (size_t)(key_end - p);
        char *key = malloc(key_len + 1);
        if (!key) { fclose(f); return 0; }
        memcpy(key, p, key_len);
        key[key_len] = '\0';

        /* value: trim trailing newline first */
        char *val = eq + 1;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r')) vlen--;
        val[vlen] = '\0';

        if (val[0] == '"' || val[0] == '\'') {
            /* quoted value: strip enclosing quotes, handle escaped chars for double-quoted */
            char q = val[0];
            val++;
            vlen--;
            char *close = NULL;
            for (size_t i = 0; i < vlen; i++) {
                if (q == '"' && val[i] == '\\' && i + 1 < vlen) { i++; continue; }
                if (val[i] == q) { close = val + i; break; }
            }
            if (close) *close = '\0';
        } else {
            /* unquoted: strip inline comment and trailing whitespace */
            for (size_t i = 0; i < vlen; i++) {
                if (val[i] == '#' && (i == 0 || val[i - 1] == ' ' || val[i - 1] == '\t')) {
                    val[i] = '\0';
                    vlen = i;
                    break;
                }
            }
            while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) vlen--;
            val[vlen] = '\0';
        }

#ifdef _WIN32
        _putenv_s(key, val);
#else
        setenv(key, val, 1);
#endif
        free(key);
    }
    fclose(f);
    return 1;
}

/* Skip whitespace in JSON text */
static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Parse a JSON string, returning a malloc'd copy of the contents.
   On entry *p points at the opening '"'; on exit *p points past the closing '"'. */
static char *json_parse_string(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    const char *start = *p;
    size_t len = 0;
    /* first pass: measure */
    const char *q = start;
    while (*q && *q != '"') {
        if (*q == '\\') { q++; if (*q) q++; }
        else q++;
        len++;
    }
    if (*q != '"') return NULL;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t i = 0;
    while (*start != '"') {
        if (*start == '\\') {
            start++;
            switch (*start) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/';  break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *start; break;
            }
            start++;
        } else {
            out[i++] = *start++;
        }
    }
    out[i] = '\0';
    *p = start + 1; /* skip closing '"' */
    return out;
}

static void load_format_config(const char *source_path, FormatConfig *cfg) {
    *cfg = default_format_config();
    if (!source_path || !source_path[0]) return;

    char bowie_json_path[4096];
    if (!find_nearest_bowie_json(source_path, bowie_json_path, sizeof(bowie_json_path))) {
        return;
    }

    char *json = read_file(bowie_json_path);
    if (!json) return;

    const char *p = json_skip_ws(json);
    if (*p != '{') { free(json); return; }
    p++;

    while (1) {
        p = json_skip_ws(p);
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') break;
        char *key = json_parse_string(&p);
        if (!key) break;
        p = json_skip_ws(p);
        if (*p != ':') { free(key); break; }
        p++;
        p = json_skip_ws(p);

        int is_format = strcmp(key, "format") == 0;
        free(key);

        if (!is_format) {
            if (*p == '"') {
                char *v = json_parse_string(&p);
                free(v);
            } else if (*p == '{' || *p == '[') {
                p = json_skip_compound_local(p);
            } else {
                while (*p && *p != ',' && *p != '}') p++;
            }
            p = json_skip_ws(p);
            if (*p == ',') p++;
            continue;
        }

        if (*p != '{') break;
        p++;
        while (1) {
            p = json_skip_ws(p);
            if (*p == '}' || *p == '\0') break;
            if (*p != '"') break;
            char *fkey = json_parse_string(&p);
            if (!fkey) break;
            p = json_skip_ws(p);
            if (*p != ':') { free(fkey); break; }
            p++;
            p = json_skip_ws(p);

            if (strcmp(fkey, "indentSize") == 0) {
                int v = 0;
                if (parse_json_int_local(&p, &v) && v >= 0) cfg->indent_size = v;
            } else if (strcmp(fkey, "indentStyle") == 0) {
                if (*p == '"') {
                    char *v = json_parse_string(&p);
                    if (v) {
                        if (strcmp(v, "tabs") == 0) cfg->use_tabs = 1;
                        else if (strcmp(v, "spaces") == 0) cfg->use_tabs = 0;
                        free(v);
                    }
                }
            } else if (strcmp(fkey, "maxConsecutiveBlankLines") == 0) {
                int v = 0;
                if (parse_json_int_local(&p, &v) && v >= 0) cfg->max_consecutive_blank_lines = v;
            } else if (strcmp(fkey, "spaceBeforeInlineComment") == 0) {
                int v = 0;
                if (parse_json_int_local(&p, &v) && v >= 0) cfg->space_before_inline_comment = v;
            } else if (strcmp(fkey, "finalNewline") == 0) {
                int v = 1;
                if (parse_json_bool_local(&p, &v)) cfg->final_newline = v ? 1 : 0;
            } else {
                if (*p == '"') {
                    char *v = json_parse_string(&p);
                    free(v);
                } else if (*p == '{' || *p == '[') {
                    p = json_skip_compound_local(p);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
            }
            free(fkey);

            p = json_skip_ws(p);
            if (*p == ',') p++;
        }
        break;
    }

    free(json);
}

/* Find a task command in bowie.json for the given task name.
   Returns a malloc'd string on success, NULL on failure. */
static char *find_task(const char *json, const char *task_name) {
    const char *p = json_skip_ws(json);
    if (*p != '{') return NULL;
    p++;

    /* Walk top-level keys to find "tasks" */
    while (1) {
        p = json_skip_ws(p);
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') return NULL;
        char *key = json_parse_string(&p);
        if (!key) return NULL;
        p = json_skip_ws(p);
        if (*p != ':') { free(key); return NULL; }
        p++;
        p = json_skip_ws(p);

        int is_tasks = strcmp(key, "tasks") == 0;
        free(key);

        if (is_tasks) {
            /* Parse the tasks object */
            if (*p != '{') return NULL;
            p++;
            while (1) {
                p = json_skip_ws(p);
                if (*p == '}' || *p == '\0') break;
                if (*p != '"') return NULL;
                char *tkey = json_parse_string(&p);
                if (!tkey) return NULL;
                p = json_skip_ws(p);
                if (*p != ':') { free(tkey); return NULL; }
                p++;
                p = json_skip_ws(p);
                if (*p != '"') { free(tkey); return NULL; }
                char *tval = json_parse_string(&p);
                if (!tval) { free(tkey); return NULL; }

                if (strcmp(tkey, task_name) == 0) {
                    free(tkey);
                    return tval;
                }
                free(tkey);
                free(tval);

                p = json_skip_ws(p);
                if (*p == ',') p++;
            }
            return NULL; /* task not found */
        }

        /* Skip the value for non-"tasks" keys */
        if (*p == '"') {
            char *v = json_parse_string(&p);
            free(v);
        } else if (*p == '{' || *p == '[') {
            /* Skip nested object/array by counting brackets */
            char open = *p, close = (*p == '{') ? '}' : ']';
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '"') { char *s = json_parse_string(&p); free(s); continue; }
                if (*p == open)  depth++;
                if (*p == close) depth--;
                p++;
            }
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }

        p = json_skip_ws(p);
        if (*p == ',') p++;
    }
    return NULL;
}

static int cmd_run(const char *task_name) {
    char *json = read_file("bowie.json");
    if (!json) {
        fprintf(stderr, "bowie: bowie.json not found in current directory\n");
        return 1;
    }

    char *cmd = find_task(json, task_name);
    free(json);

    if (!cmd) {
        fprintf(stderr, "bowie: task '%s' not found in bowie.json\n", task_name);
        return 1;
    }

    int ret = system(cmd);
    free(cmd);
#ifdef _WIN32
    return (ret == -1) ? 1 : ret;
#else
    return (ret == -1) ? 1 : WEXITSTATUS(ret);
#endif
}

#ifndef _WIN32

static int run_postinstall_task_if_present(void) {
    char *json = read_file("bowie.json");
    if (!json) return 0; /* nothing to do */

    char *cmd = find_task(json, "postinstall");
    free(json);
    if (!cmd) return 0; /* hook not defined */

    printf("bowie: running postinstall task...\n");
    fflush(stdout);
    int ret = system(cmd);
    free(cmd);
    if (ret == -1 || WEXITSTATUS(ret) != 0) {
        fprintf(stderr, "bowie: postinstall task failed\n");
        return 1;
    }
    return 0;
}

static int is_valid_pkg_path(const char *pkg) {
    if (!pkg || pkg[0] == '.' || pkg[0] == '/' || pkg[0] == '@') return 0;
    for (const char *p = pkg; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == '/'))
            return 0;
    }
    int slashes = 0;
    const char *first_slash = NULL;
    for (const char *p = pkg; *p; p++) {
        if (*p == '/') { if (!first_slash) first_slash = p; slashes++; }
    }
    if (slashes < 2 || !first_slash) return 0;
    for (const char *c = pkg; c < first_slash; c++)
        if (*c == '.') return 1;
    return 0;
}

static int mkdir_p(const char *path) {
    char *buf = malloc(strlen(path) + 1);
    if (!buf) return -1;
    strcpy(buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) { free(buf); return -1; }
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) { free(buf); return -1; }
    free(buf);
    return 0;
}

static char *get_commit_hash(const char *dir) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse HEAD 2>/dev/null", dir);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char buf[128];
    if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return NULL; }
    pclose(fp);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    if (len == 0) return NULL;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, buf, len + 1);
    return out;
}

static int remove_cloned_git_dir(const char *repo_dir) {
    char git_dir[4096];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_dir);
    struct stat st;
    if (stat(git_dir, &st) != 0) return 1;
    char rm_cmd[8192];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", git_dir);
    int ret = system(rm_cmd);
    if (ret == -1 || WEXITSTATUS(ret) != 0) {
        fprintf(stderr, "bowie: failed to remove git metadata at '%s'\n", git_dir);
        return 0;
    }
    return 1;
}

/* Return the malloc'd string value for `key` within the object bounded by
   obj_open ('{') and obj_close ('}'). Returns NULL if key not found. */
static char *json_obj_get(const char *obj_open, const char *obj_close, const char *key) {
    const char *q = obj_open + 1;
    while (1) {
        q = json_skip_ws(q);
        if (!*q || q >= obj_close || *q == '}') break;
        if (*q != '"') break;
        char *k = json_parse_string(&q);
        if (!k) break;
        q = json_skip_ws(q);
        if (*q != ':') { free(k); break; }
        q++;
        q = json_skip_ws(q);
        char *v = json_parse_string(&q);
        if (!v) { free(k); break; }
        if (strcmp(k, key) == 0) { free(k); return v; }
        free(k); free(v);
        q = json_skip_ws(q);
        if (*q == ',') q++;
    }
    return NULL;
}

/* Inject or update "pkg_path": "hash" inside a JSON object whose text is in `orig`.
   obj_open points to '{' and obj_close points to the matching '}'.
   Writes the full modified content to `out`. */
static void write_updated_obj(FILE *out, const char *orig,
                               const char *obj_open, const char *obj_close,
                               const char *pkg_path, const char *hash) {
    const char *q = obj_open + 1;
    const char *existing_val_open  = NULL;
    const char *existing_val_close = NULL;
    const char *last_val_close     = NULL;
    const char *last_key_start     = NULL;

    while (1) {
        q = json_skip_ws(q);
        if (!*q || q >= obj_close || *q == '}') break;
        if (*q != '"') break;
        last_key_start = q;
        char *k = json_parse_string(&q);
        if (!k) break;
        q = json_skip_ws(q);
        if (*q != ':') { free(k); break; }
        q++;
        q = json_skip_ws(q);
        const char *val_start = q;
        char *v = json_parse_string(&q);
        if (!v) { free(k); break; }
        last_val_close = q;
        int match = (strcmp(k, pkg_path) == 0);
        free(k);
        if (match) {
            existing_val_open  = val_start;
            existing_val_close = q;
            free(v);
            break;
        }
        free(v);
        q = json_skip_ws(q);
        if (*q == ',') q++;
    }

    if (existing_val_open) {
        fwrite(orig, 1, (size_t)(existing_val_open - orig), out);
        fprintf(out, "\"%s\"", hash);
        fputs(existing_val_close, out);
        return;
    }

    if (!last_val_close) {
        /* Empty block */
        fwrite(orig, 1, (size_t)(obj_close - orig), out);
        fprintf(out, "\n    \"%s\": \"%s\"\n  ", pkg_path, hash);
        fputs(obj_close, out);
        return;
    }

    /* Determine indent from the whitespace preceding the last key */
    char indent[64] = "    ";
    if (last_key_start && last_key_start > orig) {
        const char *nl = last_key_start - 1;
        while (nl >= orig && *nl != '\n') nl--;
        size_t ilen = (size_t)(last_key_start - nl - 1);
        if (ilen > 0 && ilen < sizeof(indent) - 1) {
            memcpy(indent, nl + 1, ilen);
            indent[ilen] = '\0';
        }
    }

    /* Write up to end of last value, append comma + new entry, then write the rest
       (which includes the original trailing whitespace and closing '}') */
    fwrite(orig, 1, (size_t)(last_val_close - orig), out);
    fprintf(out, ",\n%s\"%s\": \"%s\"", indent, pkg_path, hash);
    fputs(last_val_close, out);
}

static int update_bowie_json(const char *pkg_path, const char *hash) {
    char *orig = read_file("bowie.json");
    if (!orig) {
        FILE *f = fopen("bowie.json", "w");
        if (!f) { fprintf(stderr, "bowie: cannot create bowie.json\n"); return -1; }
        fprintf(f, "{\n  \"dependencies\": {\n    \"%s\": \"%s\"\n  }\n}\n", pkg_path, hash);
        fclose(f);
        return 0;
    }

    const char *p = json_skip_ws(orig);
    if (*p != '{') { free(orig); fprintf(stderr, "bowie: bowie.json is not a JSON object\n"); return -1; }
    p++;

    const char *dep_obj_open  = NULL;
    const char *dep_obj_close = NULL;

    while (1) {
        p = json_skip_ws(p);
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') { free(orig); return -1; }
        char *key = json_parse_string(&p);
        if (!key) { free(orig); return -1; }
        p = json_skip_ws(p);
        if (*p != ':') { free(key); free(orig); return -1; }
        p++;
        p = json_skip_ws(p);
        int is_dep = (strcmp(key, "dependencies") == 0);
        free(key);

        if (is_dep && *p == '{') {
            dep_obj_open = p;
            int depth = 1; p++;
            while (*p && depth > 0) {
                if (*p == '"') { char *s = json_parse_string(&p); free(s); continue; }
                if (*p == '{') depth++;
                if (*p == '}') { depth--; if (depth == 0) break; }
                p++;
            }
            dep_obj_close = p;
            break;
        }

        /* Skip non-"dependencies" value */
        if (*p == '"') { char *v = json_parse_string(&p); free(v); }
        else if (*p == '{' || *p == '[') {
            char open = *p, close = (*p == '{') ? '}' : ']';
            int depth = 1; p++;
            while (*p && depth > 0) {
                if (*p == '"') { char *s = json_parse_string(&p); free(s); continue; }
                if (*p == open)  depth++;
                if (*p == close) depth--;
                p++;
            }
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
        p = json_skip_ws(p);
        if (*p == ',') p++;
    }

    /* Skip write if the package already has this exact hash */
    if (dep_obj_open) {
        char *existing = json_obj_get(dep_obj_open, dep_obj_close, pkg_path);
        if (existing) {
            int unchanged = (strcmp(existing, hash) == 0);
            free(existing);
            if (unchanged) { free(orig); return 0; }
        }
    }

    FILE *out = fopen("bowie.json", "w");
    if (!out) { free(orig); fprintf(stderr, "bowie: cannot write bowie.json\n"); return -1; }

    if (!dep_obj_open) {
        /* No dependencies block: find final '}' of top-level and inject before it */
        const char *end = orig + strlen(orig) - 1;
        while (end > orig && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) end--;
        /* end now points to closing '}'; find last non-whitespace before it */
        const char *last_content = end - 1;
        while (last_content > orig &&
               (*last_content == ' ' || *last_content == '\t' ||
                *last_content == '\n' || *last_content == '\r')) last_content--;
        int need_comma = (*last_content != '{');
        fwrite(orig, 1, (size_t)(last_content - orig + 1), out);
        if (need_comma) fputc(',', out);
        fprintf(out, "\n  \"dependencies\": {\n    \"%s\": \"%s\"\n  }\n}\n", pkg_path, hash);
    } else {
        write_updated_obj(out, orig, dep_obj_open, dep_obj_close, pkg_path, hash);
    }

    fclose(out);
    free(orig);
    return 0;
}

static int update_bowie_lock(const char *pkg_path, const char *hash) {
    /* Read existing lock file silently (it may not exist yet) */
    char *orig = NULL;
    {
        FILE *rf = fopen("bowie.lock", "rb");
        if (rf) {
            fseek(rf, 0, SEEK_END); long sz = ftell(rf); fseek(rf, 0, SEEK_SET);
            orig = malloc(sz + 1);
            if (orig) { size_t n = fread(orig, 1, sz, rf); orig[n] = '\0'; }
            fclose(rf);
        }
    }

    if (orig) {
        const char *p = json_skip_ws(orig);
        if (*p == '{') {
            /* Find closing '}' of the flat object */
            const char *obj_open = p;
            int depth = 1; p++;
            while (*p && depth > 0) {
                if (*p == '"') { char *s = json_parse_string(&p); free(s); continue; }
                if (*p == '{') depth++;
                if (*p == '}') { depth--; if (depth == 0) break; }
                p++;
            }
            const char *obj_close = p;

            /* Skip write if already up to date */
            char *existing = json_obj_get(obj_open, obj_close, pkg_path);
            if (existing) {
                int unchanged = (strcmp(existing, hash) == 0);
                free(existing);
                if (unchanged) { free(orig); return 0; }
            }

            /* Need to update: open for writing only now */
            FILE *out = fopen("bowie.lock", "w");
            if (!out) { free(orig); fprintf(stderr, "bowie: cannot write bowie.lock\n"); return -1; }
            write_updated_obj(out, orig, obj_open, obj_close, pkg_path, hash);
            fclose(out);
            free(orig);
            return 0;
        }
        free(orig);
        /* Malformed — fall through to overwrite */
    }

    /* No file or malformed: create fresh */
    FILE *out = fopen("bowie.lock", "w");
    if (!out) { fprintf(stderr, "bowie: cannot write bowie.lock\n"); return -1; }
    fprintf(out, "{\n  \"%s\": \"%s\"\n}\n", pkg_path, hash);
    fclose(out);
    return 0;
}

static int cmd_install_single(const char *pkg_path, int run_postinstall) {
    if (!is_valid_pkg_path(pkg_path)) {
        fprintf(stderr, "bowie: invalid package path '%s'\n"
                        "       expected format: github.com/owner/repo\n", pkg_path);
        return 1;
    }

    char clone_url[4096];
    snprintf(clone_url, sizeof(clone_url), "https://%s", pkg_path);

    char local_path[4096];
    snprintf(local_path, sizeof(local_path), "bowie_modules/%s", pkg_path);

    /* mkdir -p the parent directory */
    char parent_dir[4096];
    strncpy(parent_dir, local_path, sizeof(parent_dir) - 1);
    parent_dir[sizeof(parent_dir) - 1] = '\0';
    char *last_slash = strrchr(parent_dir, '/');
    if (last_slash) *last_slash = '\0';
    if (mkdir_p(parent_dir) != 0) {
        fprintf(stderr, "bowie: cannot create directory '%s': %s\n", parent_dir, strerror(errno));
        return 1;
    }

    struct stat st;
    int already_exists = (stat(local_path, &st) == 0 && S_ISDIR(st.st_mode));

    if (!already_exists) {
        char git_cmd[8192];
        snprintf(git_cmd, sizeof(git_cmd), "git clone \"%s\" \"%s\"", clone_url, local_path);
        printf("bowie: cloning %s ...\n", pkg_path);
        fflush(stdout);
        int ret = system(git_cmd);
        if (ret == -1 || WEXITSTATUS(ret) != 0) {
            fprintf(stderr, "bowie: git clone failed for '%s'\n", pkg_path);
            return 1;
        }
        if (!remove_cloned_git_dir(local_path)) return 1;
    } else {
        printf("bowie: %s already cloned, updating lock info\n", pkg_path);
    }

    char *hash = get_commit_hash(local_path);
    if (!hash) {
        fprintf(stderr, "bowie: could not determine commit hash for '%s'\n", local_path);
        return 1;
    }

    if (update_bowie_json(pkg_path, hash) != 0) { free(hash); return 1; }
    if (update_bowie_lock(pkg_path, hash) != 0)  { free(hash); return 1; }

    if (run_postinstall && run_postinstall_task_if_present() != 0) { free(hash); return 1; }

    printf("bowie: installed %s @ %s\n", pkg_path, hash);
    free(hash);
    return 0;
}

static int cmd_install_dependencies(void) {
    char *json = read_file("bowie.json");
    if (!json) {
        fprintf(stderr, "bowie: bowie.json not found in current directory\n");
        return 1;
    }

    const char *p = json_skip_ws(json);
    if (*p != '{') {
        free(json);
        fprintf(stderr, "bowie: bowie.json is not a JSON object\n");
        return 1;
    }
    p++;

    char **deps = NULL;
    size_t dep_count = 0, dep_cap = 0;

    while (1) {
        p = json_skip_ws(p);
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') { free(json); return 1; }
        char *key = json_parse_string(&p);
        if (!key) { free(json); return 1; }
        p = json_skip_ws(p);
        if (*p != ':') { free(key); free(json); return 1; }
        p++;
        p = json_skip_ws(p);

        int is_dep = (strcmp(key, "dependencies") == 0);
        free(key);

        if (is_dep) {
            if (*p != '{') { free(json); return 1; }
            p++;
            while (1) {
                p = json_skip_ws(p);
                if (*p == '}' || *p == '\0') break;
                if (*p != '"') { free(json); return 1; }
                char *dep_key = json_parse_string(&p);
                if (!dep_key) { free(json); return 1; }
                p = json_skip_ws(p);
                if (*p != ':') { free(dep_key); free(json); return 1; }
                p++;
                p = json_skip_ws(p);
                if (*p == '"') {
                    char *dep_val = json_parse_string(&p);
                    free(dep_val);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }

                if (dep_count + 1 > dep_cap) {
                    size_t nc = dep_cap ? dep_cap * 2 : 8;
                    char **nd = realloc(deps, nc * sizeof(char *));
                    if (!nd) { free(dep_key); free(json); return 1; }
                    deps = nd;
                    dep_cap = nc;
                }
                deps[dep_count++] = dep_key;

                p = json_skip_ws(p);
                if (*p == ',') p++;
            }
            break;
        }

        if (*p == '"') {
            char *v = json_parse_string(&p);
            free(v);
        } else if (*p == '{' || *p == '[') {
            char open = *p, close = (*p == '{') ? '}' : ']';
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '"') { char *s = json_parse_string(&p); free(s); continue; }
                if (*p == open)  depth++;
                if (*p == close) depth--;
                p++;
            }
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
        p = json_skip_ws(p);
        if (*p == ',') p++;
    }

    free(json);

    if (dep_count == 0) {
        fprintf(stderr, "bowie: no dependencies found in bowie.json\n");
        free(deps);
        return 1;
    }

    for (size_t i = 0; i < dep_count; i++) {
        if (cmd_install_single(deps[i], 0) != 0) {
            for (size_t j = 0; j < dep_count; j++) free(deps[j]);
            free(deps);
            return 1;
        }
    }

    for (size_t i = 0; i < dep_count; i++) free(deps[i]);
    free(deps);

    if (run_postinstall_task_if_present() != 0) return 1;
    return 0;
}

#endif /* !_WIN32 */

int main(int argc, char *argv[]) {
    g_std_path = find_std_path(argc > 0 ? argv[0] : "bowie");

    /* Pre-pass: extract --env-file <path> and handle --help/-h from anywhere in argv */
    char *filtered[argc];
    int fargc = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf(
                "bowie - the bowie language interpreter\n"
                "\n"
                "USAGE:\n"
                "    bowie [OPTIONS] [script.bow]\n"
                "    bowie <command> [args]\n"
                "\n"
                "OPTIONS:\n"
                "    -h, --help              Print this help message\n"
                "    --env-file <path>       Load environment variables from a .env file\n"
                "\n"
                "COMMANDS:\n"
                "    run <task>              Run a task defined in bowie.json\n"
                "    install [package]       Install one package or all dependencies from bowie.json\n"
                "    format [path]           Print strictly formatted source to stdout\n"
                "    format --write [path]   Format and write file(s) in place\n"
                "    sort [path]             Sort bowie.json key order recursively\n"
                "\n"
                "EXAMPLES:\n"
                "    bowie                   Start the interactive REPL\n"
                "    bowie script.bow        Run a bowie script\n"
                "    bowie run dev           Run the 'dev' task from bowie.json\n"
                "    bowie install github.com/user/repo\n"
                "    bowie install           Install all dependencies from bowie.json\n"
                "    bowie format app.bow\n"
                "    bowie format --write app.bow\n"
                "    bowie format            Format all .bow files under current directory\n"
                "    bowie format --write    Write formatted .bow files under current directory\n"
                "    bowie sort              Sort ./bowie.json in place\n"
            );
            return 0;
        } else if (strcmp(argv[i], "--env-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bowie: --env-file requires a path argument\n");
                return 1;
            }
            if (!load_env_file(argv[i + 1])) return 1;
            i++;
        } else if (strncmp(argv[i], "--env-file=", 11) == 0) {
            if (!load_env_file(argv[i] + 11)) return 1;
        } else {
            filtered[fargc++] = argv[i];
        }
    }
    argc = fargc;
    argv = filtered;

    if (argc == 1) {
        repl();
        return 0;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "format") == 0) {
            return cmd_format(".", 0);
        }
        if (strcmp(argv[1], "sort") == 0) {
            return cmd_sort("bowie.json");
        }
#ifndef _WIN32
        if (strcmp(argv[1], "install") == 0) {
            return cmd_install_dependencies();
        }
#endif
        const char *path = argv[1];
        char *src = read_file(path);
        if (!src) return 1;
        run_source(src, path);
        free(src);
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "run") == 0) {
        return cmd_run(argv[2]);
    }

    if (argc == 3 && strcmp(argv[1], "format") == 0) {
        if (strcmp(argv[2], "--write") == 0) return cmd_format(".", 1);
        return cmd_format(argv[2], 0);
    }

    if (argc == 3 && strcmp(argv[1], "sort") == 0) {
        return cmd_sort(argv[2]);
    }

    if (argc == 4 && strcmp(argv[1], "format") == 0) {
        if (strcmp(argv[2], "--write") == 0) return cmd_format(argv[3], 1);
        if (strcmp(argv[3], "--write") == 0) return cmd_format(argv[2], 1);
    }

#ifndef _WIN32
    if (argc == 3 && strcmp(argv[1], "install") == 0) {
        return cmd_install_single(argv[2], 1);
    }
#endif

    fprintf(stderr,
            "Usage: bowie [script.bow]\n"
            "       bowie run <task>\n"
            "       bowie install [package]\n"
            "       bowie format [path] [--write]\n"
            "       bowie sort [path]\n"
            "       bowie [--env-file <path>] ...\n");
    return 1;
}
