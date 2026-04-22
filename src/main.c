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

    /* Compute project root (absolute directory of the entry-point file) for "@/" aliases */
    {
        char *abs = realpath(filename, NULL);
        if (abs) {
            char *sl = strrchr(abs, '/');
            if (sl) *sl = '\0';
            interp->project_root = dup_cstr(abs);
            free(abs);
        } else {
            char *root = dup_cstr(filename);
            if (!root) interp->project_root = NULL;
            else {
                char *sl = strrchr(root, '/');
                if (sl) { *sl = '\0'; interp->project_root = root; }
                else    { free(root); interp->project_root = dup_cstr("."); }
            }
        }
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

int main(int argc, char *argv[]) {
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

    fprintf(stderr, "Usage: bowie [script.bow]\n       bowie run <task>\n");
    return 1;
}
