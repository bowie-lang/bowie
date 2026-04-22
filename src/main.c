#include <errno.h>
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

/* Declared in builtins.c */
void builtins_set_interp(Interpreter *it, Env *env);

static char *dup_cstr(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    return copy;
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

static int cmd_install(const char *pkg_path) {
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

    printf("bowie: installed %s @ %s\n", pkg_path, hash);
    free(hash);
    return 0;
}

#endif /* !_WIN32 */

int main(int argc, char *argv[]) {
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
                "    install <package>       Install a package (e.g. github.com/owner/repo)\n"
                "\n"
                "EXAMPLES:\n"
                "    bowie                   Start the interactive REPL\n"
                "    bowie script.bow        Run a bowie script\n"
                "    bowie run dev           Run the 'dev' task from bowie.json\n"
                "    bowie install github.com/user/repo\n"
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

#ifndef _WIN32
    if (argc == 3 && strcmp(argv[1], "install") == 0) {
        return cmd_install(argv[2]);
    }
#endif

    fprintf(stderr,
            "Usage: bowie [script.bow]\n"
            "       bowie run <task>\n"
            "       bowie install <package>\n"
            "       bowie [--env-file <path>] ...\n");
    return 1;
}
