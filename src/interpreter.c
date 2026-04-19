#include "interpreter.h"
#include "builtins.h"
#include "http.h"
#include "lexer.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

static char *dup_cstr(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    return copy;
}

#ifdef _WIN32
  #include <windows.h>
  static char *resolve_path(const char *base_file, const char *rel) {
      char buf[4096];
      if (rel[0] == '/' || rel[1] == ':') { strncpy(buf, rel, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0'; return dup_cstr(buf); }
      char dir[4096]; strncpy(dir, base_file, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
      char *slash = strrchr(dir, '\\');
      if (!slash) slash = strrchr(dir, '/');
      if (slash) { *(slash+1) = '\0'; snprintf(buf, sizeof(buf), "%s%s", dir, rel); }
      else { strncpy(buf, rel, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0'; }
      return dup_cstr(buf);
  }
#else
  static char *resolve_path(const char *base_file, const char *rel) {
      if (!base_file || rel[0] == '/') {
          char *real = realpath(rel, NULL);
          return real ? real : dup_cstr(rel);
      }

      char *base_copy = dup_cstr(base_file);
      if (!base_copy) return NULL;

      char *slash = strrchr(base_copy, '/');
      const char *dir = ".";
      if (slash) {
          *(slash + 1) = '\0';
          dir = base_copy;
      }

      size_t dir_len = strlen(dir);
      size_t rel_len = strlen(rel);
      int needs_sep = (dir_len > 0 && dir[dir_len - 1] != '/');
      size_t joined_len = dir_len + (size_t)needs_sep + rel_len + 1;
      char *joined = malloc(joined_len);
      if (!joined) {
          free(base_copy);
          return NULL;
      }

      if (needs_sep) snprintf(joined, joined_len, "%s/%s", dir, rel);
      else snprintf(joined, joined_len, "%s%s", dir, rel);

      free(base_copy);

      char *real = realpath(joined, NULL);
      if (real) {
          free(joined);
          return real;
      }
      return joined;
  }
#endif

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read = fread(buf, 1, sz, f);
    buf[read] = '\0';
    fclose(f); return buf;
}

#define IS_ERR(o)  ((o) && (o)->type == OBJ_ERROR)
#define IS_RET(o)  ((o) && (o)->type == OBJ_RETURN)
#define IS_BRK(o)  ((o) && (o)->type == OBJ_BREAK)
#define IS_CNT(o)  ((o) && (o)->type == OBJ_CONTINUE)

static Object *eval_node(Interpreter *it, Node *n, Env *env);

/* ---- Arithmetic helpers ---- */
static Object *eval_int_infix(const char *op, long long a, long long b, int line) {
    if (!strcmp(op, "+"))  return obj_int(a + b);
    if (!strcmp(op, "-"))  return obj_int(a - b);
    if (!strcmp(op, "*"))  return obj_int(a * b);
    if (!strcmp(op, "/")) {
        if (b == 0) return obj_errorf("line %d: division by zero", line);
        return obj_int(a / b);
    }
    if (!strcmp(op, "%")) {
        if (b == 0) return obj_errorf("line %d: modulo by zero", line);
        return obj_int(a % b);
    }
    if (!strcmp(op, "==")) return obj_bool(a == b);
    if (!strcmp(op, "!=")) return obj_bool(a != b);
    if (!strcmp(op, "<"))  return obj_bool(a < b);
    if (!strcmp(op, "<=")) return obj_bool(a <= b);
    if (!strcmp(op, ">"))  return obj_bool(a > b);
    if (!strcmp(op, ">=")) return obj_bool(a >= b);
    return obj_errorf("line %d: unknown operator '%s' for int", line, op);
}

static Object *eval_float_infix(const char *op, double a, double b, int line) {
    if (!strcmp(op, "+"))  return obj_float(a + b);
    if (!strcmp(op, "-"))  return obj_float(a - b);
    if (!strcmp(op, "*"))  return obj_float(a * b);
    if (!strcmp(op, "/")) {
        if (b == 0.0) return obj_errorf("line %d: division by zero", line);
        return obj_float(a / b);
    }
    if (!strcmp(op, "%"))  return obj_float(fmod(a, b));
    if (!strcmp(op, "==")) return obj_bool(a == b);
    if (!strcmp(op, "!=")) return obj_bool(a != b);
    if (!strcmp(op, "<"))  return obj_bool(a < b);
    if (!strcmp(op, "<=")) return obj_bool(a <= b);
    if (!strcmp(op, ">"))  return obj_bool(a > b);
    if (!strcmp(op, ">=")) return obj_bool(a >= b);
    return obj_errorf("line %d: unknown operator '%s' for float", line, op);
}

static Object *eval_string_infix(const char *op, const char *a, const char *b, int line) {
    if (!strcmp(op, "+")) {
        int   la  = strlen(a), lb = strlen(b);
        char *buf = malloc(la + lb + 1);
        memcpy(buf, a, la);
        memcpy(buf + la, b, lb + 1);
        Object *o = obj_string(buf);
        free(buf);
        return o;
    }
    if (!strcmp(op, "==")) return obj_bool(!strcmp(a, b));
    if (!strcmp(op, "!=")) return obj_bool(strcmp(a, b) != 0);
    if (!strcmp(op, "<"))  return obj_bool(strcmp(a, b) < 0);
    if (!strcmp(op, ">"))  return obj_bool(strcmp(a, b) > 0);
    return obj_errorf("line %d: operator '%s' not supported for strings", line, op);
}

/* ---- Eval helpers ---- */
static Object *eval_infix(Interpreter *it, Node *n, Env *env) {
    const char *op = n->infix.op;

    /* Short-circuit && and || */
    if (!strcmp(op, "&&")) {
        Object *l = eval_node(it, n->infix.left, env);
        if (IS_ERR(l)) return l;
        if (!obj_is_truthy(l)) { obj_release(l); return obj_bool(0); }
        obj_release(l);
        Object *r = eval_node(it, n->infix.right, env);
        if (IS_ERR(r)) return r;
        int t = obj_is_truthy(r); obj_release(r);
        return obj_bool(t);
    }
    if (!strcmp(op, "||")) {
        Object *l = eval_node(it, n->infix.left, env);
        if (IS_ERR(l)) return l;
        if (obj_is_truthy(l)) { obj_release(l); return obj_bool(1); }
        obj_release(l);
        Object *r = eval_node(it, n->infix.right, env);
        if (IS_ERR(r)) return r;
        int t = obj_is_truthy(r); obj_release(r);
        return obj_bool(t);
    }

    Object *left  = eval_node(it, n->infix.left,  env);
    if (IS_ERR(left))  return left;
    Object *right = eval_node(it, n->infix.right, env);
    if (IS_ERR(right)) { obj_release(left); return right; }

    Object *res = NULL;

    if (left->type == OBJ_INT && right->type == OBJ_INT) {
        res = eval_int_infix(op, left->int_val, right->int_val, n->line);
    } else if (left->type == OBJ_FLOAT || right->type == OBJ_FLOAT) {
        double a = left->type  == OBJ_FLOAT ? left->float_val  : (double)left->int_val;
        double b = right->type == OBJ_FLOAT ? right->float_val : (double)right->int_val;
        res = eval_float_infix(op, a, b, n->line);
    } else if (left->type == OBJ_STRING && !strcmp(op, "+")) {
        char *rs = obj_inspect(right);
        res = eval_string_infix(op, left->string.str, rs, n->line);
        free(rs);
    } else if (right->type == OBJ_STRING && !strcmp(op, "+")) {
        char *ls = obj_inspect(left);
        res = eval_string_infix(op, ls, right->string.str, n->line);
        free(ls);
    } else if (left->type == OBJ_STRING && right->type == OBJ_STRING) {
        res = eval_string_infix(op, left->string.str, right->string.str, n->line);
    } else if (!strcmp(op, "==")) {
        res = obj_bool(obj_equals(left, right));
    } else if (!strcmp(op, "!=")) {
        res = obj_bool(!obj_equals(left, right));
    } else {
        res = obj_errorf("line %d: type mismatch: %s %s %s",
                         n->line, obj_type_name(left->type), op, obj_type_name(right->type));
    }

    obj_release(left);
    obj_release(right);
    return res;
}

static Object *eval_prefix(Interpreter *it, Node *n, Env *env) {
    Object *right = eval_node(it, n->prefix.right, env);
    if (IS_ERR(right)) return right;
    Object *res = NULL;
    if (!strcmp(n->prefix.op, "!")) {
        res = obj_bool(!obj_is_truthy(right));
    } else if (!strcmp(n->prefix.op, "-")) {
        if (right->type == OBJ_INT)   res = obj_int(-(right->int_val));
        else if (right->type == OBJ_FLOAT) res = obj_float(-(right->float_val));
        else res = obj_errorf("line %d: '-' prefix not supported for %s",
                              n->line, obj_type_name(right->type));
    } else {
        res = obj_errorf("line %d: unknown prefix '%s'", n->line, n->prefix.op);
    }
    obj_release(right);
    return res;
}

static Object *eval_block(Interpreter *it, Node *block, Env *env) {
    Object *result = obj_null();
    for (int i = 0; i < block->block.stmts.count; i++) {
        obj_release(result);
        result = eval_node(it, block->block.stmts.items[i], env);
        if (IS_ERR(result) || IS_RET(result) || IS_BRK(result) || IS_CNT(result)) return result;
    }
    return result;
}

static Object *eval_call(Interpreter *it, Node *n, Env *env) {
    Object *fn = eval_node(it, n->call.fn, env);
    if (IS_ERR(fn)) return fn;

    /* Evaluate arguments */
    ObjList args;
    objlist_init(&args);
    for (int i = 0; i < n->call.args.count; i++) {
        Object *arg = eval_node(it, n->call.args.items[i], env);
        if (IS_ERR(arg)) {
            obj_release(fn);
            objlist_free(&args);
            return arg;
        }
        objlist_push(&args, arg);
    }

    Object *result = NULL;

    if (fn->type == OBJ_BUILTIN) {
        result = fn->builtin.fn(&args);
    } else if (fn->type == OBJ_FUNCTION) {
        if (args.count != fn->fn.param_count) {
            result = obj_errorf("line %d: %s expects %d args, got %d",
                                n->line,
                                fn->fn.name ? fn->fn.name : "fn",
                                fn->fn.param_count, args.count);
        } else {
            Env *fn_env = env_new(fn->fn.closure);
            for (int i = 0; i < fn->fn.param_count; i++) {
                env_set(fn_env, fn->fn.params[i], args.items[i]);
            }
            result = eval_block(it, fn->fn.body, fn_env);
            if (IS_RET(result)) {
                Object *v = result->ret.value;
                obj_retain(v);
                obj_release(result);
                result = v;
            }
            env_release(fn_env);
        }
    } else {
        result = obj_errorf("line %d: not a function: %s", n->line, obj_type_name(fn->type));
    }

    for (int i = 0; i < args.count; i++) obj_release(args.items[i]);
    free(args.items);
    obj_release(fn);
    return result ? result : obj_null();
}

static Object *eval_index(Interpreter *it, Node *n, Env *env) {
    Object *left  = eval_node(it, n->index.left,  env);
    if (IS_ERR(left))  return left;
    Object *index = eval_node(it, n->index.index, env);
    if (IS_ERR(index)) { obj_release(left); return index; }

    Object *res = NULL;
    if (left->type == OBJ_ARRAY) {
        if (index->type != OBJ_INT) {
            res = obj_errorf("line %d: array index must be int", n->line);
        } else {
            res = array_get(left, (int)index->int_val);
            obj_retain(res);
        }
    } else if (left->type == OBJ_HASH) {
        char *key = obj_inspect(index);
        res = hash_get(left, key);
        obj_retain(res);
        free(key);
    } else if (left->type == OBJ_STRING) {
        if (index->type != OBJ_INT) {
            res = obj_errorf("line %d: string index must be int", n->line);
        } else {
            int idx = (int)index->int_val;
            int len = strlen(left->string.str);
            if (idx < 0) idx += len;
            if (idx < 0 || idx >= len) {
                res = obj_null();
            } else {
                char buf[2] = { left->string.str[idx], '\0' };
                res = obj_string(buf);
            }
        }
    } else {
        res = obj_errorf("line %d: index operator not supported for %s",
                         n->line, obj_type_name(left->type));
    }

    obj_release(left);
    obj_release(index);
    return res;
}

static Object *eval_assign(Interpreter *it, Node *n, Env *env) {
    Object *val = eval_node(it, n->assign.value, env);
    if (IS_ERR(val)) return val;

    Node *target = n->assign.target;
    if (target->type == NODE_IDENT) {
        if (!env_assign(env, target->ident.name, val)) {
            obj_release(val);
            return obj_errorf("line %d: undefined variable '%s'", n->line, target->ident.name);
        }
        return val;
    }
    if (target->type == NODE_INDEX) {
        Object *container = eval_node(it, target->index.left, env);
        if (IS_ERR(container)) { obj_release(val); return container; }
        Object *key_obj   = eval_node(it, target->index.index, env);
        if (IS_ERR(key_obj)) { obj_release(val); obj_release(container); return key_obj; }

        if (container->type == OBJ_ARRAY) {
            if (key_obj->type != OBJ_INT) {
                obj_release(container); obj_release(key_obj); obj_release(val);
                return obj_errorf("line %d: array index must be int", n->line);
            }
            int idx = (int)key_obj->int_val;
            if (idx < 0) idx += container->array.elems.count;
            if (idx < 0 || idx >= container->array.elems.count) {
                obj_release(container); obj_release(key_obj); obj_release(val);
                return obj_errorf("line %d: index out of bounds", n->line);
            }
            obj_release(container->array.elems.items[idx]);
            container->array.elems.items[idx] = val;
            obj_retain(val);
        } else if (container->type == OBJ_HASH) {
            char *key = obj_inspect(key_obj);
            hash_set(container, key, val);
            free(key);
        } else {
            obj_release(container); obj_release(key_obj); obj_release(val);
            return obj_errorf("line %d: cannot index-assign into %s",
                              n->line, obj_type_name(container->type));
        }
        obj_release(container);
        obj_release(key_obj);
        return val;
    }
    obj_release(val);
    return obj_errorf("line %d: invalid assignment target", n->line);
}

/* ---- Main eval ---- */
static Object *eval_node(Interpreter *it, Node *n, Env *env) {
    if (!n) return obj_null();

    switch (n->type) {
        /* --- Literals --- */
        case NODE_INT:    return obj_int(n->integer.value);
        case NODE_FLOAT:  return obj_float(n->float_.value);
        case NODE_STRING: return obj_string(n->string.value);
        case NODE_BOOL:   return obj_bool(n->boolean.value);
        case NODE_NULL:   return obj_null();

        /* --- Identifier --- */
        case NODE_IDENT: {
            Object *v = env_get(env, n->ident.name);
            if (!v) return obj_errorf("line %d: undefined variable '%s'", n->line, n->ident.name);
            obj_retain(v);
            return v;
        }

        /* --- Array literal --- */
        case NODE_ARRAY: {
            Object *arr = obj_array();
            for (int i = 0; i < n->array.elems.count; i++) {
                Object *elem = eval_node(it, n->array.elems.items[i], env);
                if (IS_ERR(elem)) { obj_release(arr); return elem; }
                array_push(arr, elem);
                obj_release(elem);
            }
            return arr;
        }

        /* --- Hash literal --- */
        case NODE_HASH: {
            Object *h = obj_hash();
            for (int i = 0; i < n->hash.count; i++) {
                Object *k = eval_node(it, n->hash.pairs[i].key, env);
                if (IS_ERR(k)) { obj_release(h); return k; }
                Object *v = eval_node(it, n->hash.pairs[i].value, env);
                if (IS_ERR(v)) { obj_release(k); obj_release(h); return v; }
                char *ks = obj_inspect(k);
                hash_set(h, ks, v);
                free(ks);
                obj_release(k);
                obj_release(v);
            }
            return h;
        }

        /* --- Operators --- */
        case NODE_PREFIX: return eval_prefix(it, n, env);
        case NODE_INFIX:  return eval_infix(it, n, env);
        case NODE_ASSIGN: return eval_assign(it, n, env);

        /* --- Index --- */
        case NODE_INDEX:  return eval_index(it, n, env);

        /* --- If --- */
        case NODE_IF: {
            Object *cond = eval_node(it, n->if_.cond, env);
            if (IS_ERR(cond)) return cond;
            int truthy = obj_is_truthy(cond);
            obj_release(cond);
            if (truthy) {
                Env *block_env = env_new(env);
                Object *r = eval_block(it, n->if_.then_, block_env);
                env_release(block_env);
                return r;
            } else if (n->if_.else_) {
                Env *block_env = env_new(env);
                Object *r;
                if (n->if_.else_->type == NODE_IF) {
                    r = eval_node(it, n->if_.else_, env);
                } else {
                    r = eval_block(it, n->if_.else_, block_env);
                }
                env_release(block_env);
                return r;
            }
            return obj_null();
        }

        /* --- While --- */
        case NODE_WHILE: {
            Object *result = obj_null();
            for (;;) {
                Object *cond = eval_node(it, n->while_.cond, env);
                if (IS_ERR(cond)) { obj_release(result); return cond; }
                int truthy = obj_is_truthy(cond);
                obj_release(cond);
                if (!truthy) break;
                obj_release(result);
                Env *loop_env = env_new(env);
                result = eval_block(it, n->while_.body, loop_env);
                env_release(loop_env);
                if (IS_ERR(result) || IS_RET(result)) return result;
                if (IS_BRK(result)) { obj_release(result); result = obj_null(); break; }
                if (IS_CNT(result)) { obj_release(result); result = obj_null(); continue; }
            }
            return result;
        }

        /* --- For --- */
        case NODE_FOR: {
            Object *iter = eval_node(it, n->for_.iter, env);
            if (IS_ERR(iter)) return iter;
            if (iter->type != OBJ_ARRAY) {
                obj_release(iter);
                return obj_errorf("line %d: for..in requires an array", n->line);
            }
            Object *result = obj_null();
            for (int i = 0; i < iter->array.elems.count; i++) {
                obj_release(result);
                Env *loop_env = env_new(env);
                env_set(loop_env, n->for_.var, iter->array.elems.items[i]);
                result = eval_block(it, n->for_.body, loop_env);
                env_release(loop_env);
                if (IS_ERR(result) || IS_RET(result)) break;
                if (IS_BRK(result)) { obj_release(result); result = obj_null(); break; }
                if (IS_CNT(result)) { obj_release(result); result = obj_null(); continue; }
            }
            obj_release(iter);
            return result;
        }

        /* --- Function literal --- */
        case NODE_FUNCTION: {
            char **params = malloc(n->fn.param_count * sizeof(char *));
            for (int i = 0; i < n->fn.param_count; i++)
                params[i] = dup_cstr(n->fn.params[i]);
            Object *fn = obj_function(params, n->fn.param_count, n->fn.body, env, n->fn.name);
            return fn;
        }

        /* --- Call --- */
        case NODE_CALL:   return eval_call(it, n, env);

        /* --- Let --- */
        case NODE_LET: {
            Object *val = eval_node(it, n->let.value, env);
            if (IS_ERR(val)) return val;
            env_set(env, n->let.name, val);
            obj_release(val);
            return obj_null();
        }

        /* --- Break / Continue --- */
        case NODE_BREAK:    return obj_break();
        case NODE_CONTINUE: return obj_continue();

        /* --- Return --- */
        case NODE_RETURN: {
            Object *val = eval_node(it, n->ret.value, env);
            if (IS_ERR(val)) return val;
            Object *r = obj_return(val);
            obj_release(val);
            return r;
        }

        /* --- Expression statement --- */
        case NODE_EXPR_STMT:
            return eval_node(it, n->expr_stmt.expr, env);

        /* --- Block --- */
        case NODE_BLOCK:
        case NODE_PROGRAM: {
            Env *block_env = env_new(env);
            Object *r = eval_block(it, n, block_env);
            env_release(block_env);
            return r;
        }

        /* --- Export --- */
        case NODE_EXPORT: {
            Object *val = eval_node(it, n->export_.decl, env);
            if (IS_ERR(val)) return val;
            /* Determine the exported name from the inner declaration */
            Node *decl = n->export_.decl;
            const char *name = NULL;
            if (decl->type == NODE_LET)        name = decl->let.name;
            else if (decl->type == NODE_EXPR_STMT &&
                     decl->expr_stmt.expr->type == NODE_ASSIGN &&
                     decl->expr_stmt.expr->assign.target->type == NODE_IDENT)
                name = decl->expr_stmt.expr->assign.target->ident.name;
            if (name && it->exports) {
                Object *exported = env_get(env, name);
                if (exported) hash_set(it->exports, name, exported);
            }
            obj_release(val);
            return obj_null();
        }

        /* --- Import --- */
        case NODE_IMPORT: {
            const char *raw = n->import_.path;
            char *aliased = NULL;
            if (it->project_root && raw[0] == '@' && raw[1] == '/') {
                size_t rlen = strlen(it->project_root);
                size_t plen = strlen(raw + 1);
                aliased = malloc(rlen + plen + 1);
                memcpy(aliased, it->project_root, rlen);
                memcpy(aliased + rlen, raw + 1, plen + 1);
                raw = aliased;
            }
            char *resolved = resolve_path(it->current_file, raw);
            free(aliased);
            Object *mod = interp_load_module(it, resolved);
            free(resolved);
            if (IS_ERR(mod)) return mod;

            if (n->import_.alias) {
                /* import "x" as name */
                env_set(env, n->import_.alias, mod);
            } else if (n->import_.name_count > 0) {
                /* import "x" use a, b */
                for (int i = 0; i < n->import_.name_count; i++) {
                    Object *v = hash_get(mod, n->import_.names[i]);
                    if (v == BOWIE_NULL) {
                        obj_release(mod);
                        return obj_errorf("line %d: '%s' not exported by '%s'",
                                          n->line, n->import_.names[i], n->import_.path);
                    }
                    env_set(env, n->import_.names[i], v);
                }
            } else {
                /* import "x"  — bring everything into scope */
                int count;
                char **keys = hash_keys(mod, &count);
                for (int i = 0; i < count; i++) {
                    Object *v = hash_get(mod, keys[i]);
                    env_set(env, keys[i], v);
                }
                free(keys);
            }
            obj_release(mod);
            return obj_null();
        }

        default:
            return obj_errorf("eval: unknown node type %d", n->type);
    }
}

/* ---- Module loader ---- */
Object *interp_load_module(Interpreter *it, const char *path) {
    /* Check cache */
    for (ModCache *c = it->cache; c; c = c->next) {
        if (!strcmp(c->path, path)) {
            obj_retain(c->module);
            return c->module;
        }
    }

    char *src = read_file_str(path);
    if (!src) return obj_errorf("import: cannot open '%s': %s", path, strerror(errno));

    Lexer  *lexer  = lexer_new(src);
    Parser *parser = parser_new(lexer);
    Node   *ast    = parser_parse(parser);
    free(src);

    if (parser->error) {
        char *msg = dup_cstr(parser->error);
        node_free(ast); parser_free(parser); lexer_free(lexer);
        Object *err = obj_errorf("import '%s': %s", path, msg);
        free(msg);
        return err;
    }

    /* Save and replace interpreter state for the module's execution */
    Object *saved_exports     = it->exports;
    char   *saved_file        = it->current_file;

    it->exports      = obj_hash();
    it->current_file = (char *)path;

    Env    *mod_env  = env_new(it->globals);
    Object *result   = eval_block(it, ast, mod_env);
    env_release(mod_env);
    /* parser and lexer freed here; ast kept alive for function body references */
    parser_free(parser); lexer_free(lexer);

    Object *mod      = it->exports;
    it->exports      = saved_exports;
    it->current_file = saved_file;

    if (IS_ERR(result)) {
        obj_release(mod);
        node_free(ast);
        return result;
    }
    obj_release(result);
    /* ast ownership transferred to cache entry below */

    /* Cache it */
    ModCache *entry = malloc(sizeof(ModCache));
    entry->path     = dup_cstr(path);
    entry->module   = mod;
    entry->ast      = ast;  /* owns the ast */
    obj_retain(mod);
    entry->next     = it->cache;
    it->cache       = entry;

    return mod;
}

/* ---- Public API ---- */
Interpreter *interp_new(void) {
    objects_init();
    Interpreter *it  = calloc(1, sizeof(Interpreter));
    it->globals      = env_new(NULL);
    it->cache        = NULL;
    it->exports      = NULL;
    it->current_file = NULL;
    builtins_register(it->globals);
    return it;
}

void interp_free(Interpreter *it) {
    free(it->project_root);
    ModCache *c = it->cache;
    while (c) {
        ModCache *next = c->next;
        free(c->path);
        obj_release(c->module);
        node_free(c->ast);
        free(c);
        c = next;
    }
    env_release(it->globals);
    free(it);
}

Object *interp_eval(Interpreter *it, Node *node, Env *env) {
    return eval_node(it, node, env ? env : it->globals);
}
