#include "parser.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Precedences ---- */
typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGN,
    PREC_OR,
    PREC_AND,
    PREC_EQ,
    PREC_CMP,
    PREC_ADD,
    PREC_MUL,
    PREC_PREFIX,
    PREC_CALL,
    PREC_INDEX,
} Prec;

static Prec tok_prec(TokenType t) {
    switch (t) {
        case TOK_ASSIGN:   return PREC_ASSIGN;
        case TOK_OR:       return PREC_OR;
        case TOK_AND:      return PREC_AND;
        case TOK_EQ:
        case TOK_NEQ:      return PREC_EQ;
        case TOK_LT:
        case TOK_LTE:
        case TOK_GT:
        case TOK_GTE:      return PREC_CMP;
        case TOK_PLUS:
        case TOK_MINUS:    return PREC_ADD;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:  return PREC_MUL;
        case TOK_LPAREN:   return PREC_CALL;
        case TOK_LBRACKET: return PREC_INDEX;
        case TOK_DOT:      return PREC_INDEX;
        default:           return PREC_NONE;
    }
}

/* ---- Helpers ---- */
static void advance(Parser *p) {
    token_free(p->cur);
    p->cur  = p->peek;
    p->peek = lexer_next(p->lexer);
}

static int cur_is(Parser *p, TokenType t)  { return p->cur.type  == t; }

static void set_error(Parser *p, const char *fmt, ...) {
    if (p->error) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    p->error = strdup(buf);
}

static int expect(Parser *p, TokenType t) {
    if (cur_is(p, t)) { advance(p); return 1; }
    set_error(p, "line %d: expected '%s', got '%s' ('%s')",
              p->cur.line, tok_name(t), tok_name(p->cur.type), p->cur.value);
    return 0;
}

/* ---- Forward declarations ---- */
static Node *parse_stmt(Parser *p);
static Node *parse_expr(Parser *p, Prec prec);
static Node *parse_block(Parser *p);

/* ---- Prefix parsers ---- */
static Node *parse_ident(Parser *p) {
    Node *n = node_new(NODE_IDENT, p->cur.line);
    n->ident.name = strdup(p->cur.value);
    advance(p);
    return n;
}

static Node *parse_int(Parser *p) {
    Node *n = node_new(NODE_INT, p->cur.line);
    n->integer.value = atoll(p->cur.value);
    advance(p);
    return n;
}

static Node *parse_float(Parser *p) {
    Node *n = node_new(NODE_FLOAT, p->cur.line);
    n->float_.value = atof(p->cur.value);
    advance(p);
    return n;
}

static Node *parse_string(Parser *p) {
    Node *n = node_new(NODE_STRING, p->cur.line);
    n->string.value = strdup(p->cur.value);
    advance(p);
    return n;
}

static Node *parse_bool(Parser *p) {
    Node *n = node_new(NODE_BOOL, p->cur.line);
    n->boolean.value = cur_is(p, TOK_TRUE) ? 1 : 0;
    advance(p);
    return n;
}

static Node *parse_null(Parser *p) {
    Node *n = node_new(NODE_NULL, p->cur.line);
    advance(p);
    return n;
}

static Node *parse_prefix(Parser *p) {
    Node *n = node_new(NODE_PREFIX, p->cur.line);
    n->prefix.op    = strdup(p->cur.value);
    advance(p);
    n->prefix.right = parse_expr(p, PREC_PREFIX);
    return n;
}

static Node *parse_grouped(Parser *p) {
    advance(p); /* skip ( */
    Node *n = parse_expr(p, PREC_NONE);
    if (!expect(p, TOK_RPAREN)) { node_free(n); return NULL; }
    return n;
}

static Node *parse_array_lit(Parser *p) {
    Node *n = node_new(NODE_ARRAY, p->cur.line);
    nodelist_init(&n->array.elems);
    advance(p); /* skip [ */
    while (!cur_is(p, TOK_RBRACKET) && !cur_is(p, TOK_EOF)) {
        if (p->error) break;
        Node *elem = parse_expr(p, PREC_NONE);
        if (!elem) break;
        nodelist_push(&n->array.elems, elem);
        if (cur_is(p, TOK_COMMA)) advance(p);
        else break;
    }
    if (!p->error) expect(p, TOK_RBRACKET);
    return n;
}

static Node *parse_hash_lit(Parser *p) {
    Node *n = node_new(NODE_HASH, p->cur.line);
    n->hash.pairs = NULL;
    n->hash.count = 0;
    n->hash.cap   = 0;
    advance(p); /* skip { */
    while (!cur_is(p, TOK_RBRACE) && !cur_is(p, TOK_EOF)) {
        if (p->error) break;
        Node *key = parse_expr(p, PREC_NONE);
        if (!key) break;
        if (!expect(p, TOK_COLON)) { node_free(key); break; }
        Node *val = parse_expr(p, PREC_NONE);
        if (!val) { node_free(key); break; }
        if (n->hash.count >= n->hash.cap) {
            n->hash.cap   = n->hash.cap ? n->hash.cap * 2 : 4;
            n->hash.pairs = realloc(n->hash.pairs, n->hash.cap * sizeof(HashPair));
        }
        n->hash.pairs[n->hash.count++] = (HashPair){ key, val };
        if (cur_is(p, TOK_COMMA)) advance(p);
        else break;
    }
    if (!p->error) expect(p, TOK_RBRACE);
    return n;
}

static Node *parse_fn(Parser *p) {
    int line = p->cur.line;
    advance(p); /* skip fn */

    char *fn_name = NULL;
    if (cur_is(p, TOK_IDENT)) {
        fn_name = strdup(p->cur.value);
        advance(p);
    }

    if (!expect(p, TOK_LPAREN)) { free(fn_name); return NULL; }

    char **params     = NULL;
    int    param_count= 0, param_cap = 0;

    while (!cur_is(p, TOK_RPAREN) && !cur_is(p, TOK_EOF)) {
        if (p->error) break;
        if (!cur_is(p, TOK_IDENT)) {
            set_error(p, "line %d: expected param name", p->cur.line);
            break;
        }
        if (param_count >= param_cap) {
            param_cap   = param_cap ? param_cap * 2 : 4;
            params      = realloc(params, param_cap * sizeof(char *));
        }
        params[param_count++] = strdup(p->cur.value);
        advance(p);
        if (cur_is(p, TOK_COMMA)) advance(p);
        else break;
    }
    if (!expect(p, TOK_RPAREN)) {
        for (int i = 0; i < param_count; i++) free(params[i]);
        free(params); free(fn_name); return NULL;
    }

    Node *body = parse_block(p);
    if (!body) {
        for (int i = 0; i < param_count; i++) free(params[i]);
        free(params); free(fn_name); return NULL;
    }

    Node *n            = node_new(NODE_FUNCTION, line);
    n->fn.params       = params;
    n->fn.param_count  = param_count;
    n->fn.body         = body;
    n->fn.name         = fn_name;
    return n;
}

static Node *parse_if(Parser *p) {
    int  line = p->cur.line;
    advance(p); /* skip if */
    Node *cond  = parse_expr(p, PREC_NONE);
    Node *then_ = parse_block(p);
    Node *else_ = NULL;

    if (cur_is(p, TOK_ELSE)) {
        advance(p);
        if (cur_is(p, TOK_IF)) {
            else_ = parse_if(p);
        } else {
            else_ = parse_block(p);
        }
    }

    Node *n    = node_new(NODE_IF, line);
    n->if_.cond  = cond;
    n->if_.then_ = then_;
    n->if_.else_ = else_;
    return n;
}

/* ---- Infix parsers ---- */
static Node *parse_infix(Parser *p, Node *left) {
    if (cur_is(p, TOK_ASSIGN)) {
        /* assignment expression */
        int line = p->cur.line;
        advance(p);
        Node *val = parse_expr(p, PREC_ASSIGN - 1);
        Node *n   = node_new(NODE_ASSIGN, line);
        n->assign.target = left;
        n->assign.value  = val;
        return n;
    }

    Node *n    = node_new(NODE_INFIX, p->cur.line);
    n->infix.op   = strdup(p->cur.value);
    Prec  prec    = tok_prec(p->cur.type);
    advance(p);
    n->infix.left  = left;
    n->infix.right = parse_expr(p, prec);
    return n;
}

static Node *parse_call(Parser *p, Node *fn) {
    Node *n = node_new(NODE_CALL, p->cur.line);
    n->call.fn = fn;
    nodelist_init(&n->call.args);
    advance(p); /* skip ( */
    while (!cur_is(p, TOK_RPAREN) && !cur_is(p, TOK_EOF)) {
        if (p->error) break;
        Node *arg = parse_expr(p, PREC_NONE);
        if (!arg) break;
        nodelist_push(&n->call.args, arg);
        if (cur_is(p, TOK_COMMA)) advance(p);
        else break;
    }
    if (!p->error) expect(p, TOK_RPAREN);
    return n;
}

static Node *parse_index(Parser *p, Node *left) {
    Node *n = node_new(NODE_INDEX, p->cur.line);
    advance(p); /* skip [ */
    n->index.left  = left;
    n->index.index = parse_expr(p, PREC_NONE);
    if (!p->error) expect(p, TOK_RBRACKET);
    return n;
}

/* Desugar obj.field  →  obj["field"] */
static Node *parse_dot(Parser *p, Node *left) {
    int line = p->cur.line;
    advance(p); /* skip . */
    if (!cur_is(p, TOK_IDENT)) {
        set_error(p, "line %d: expected field name after '.'", p->cur.line);
        return NULL;
    }
    Node *key = node_new(NODE_STRING, line);
    key->string.value = strdup(p->cur.value);
    advance(p);
    Node *n = node_new(NODE_INDEX, line);
    n->index.left  = left;
    n->index.index = key;
    return n;
}

/* ---- Expression ---- */
static Node *parse_expr(Parser *p, Prec prec) {
    if (p->error) return NULL;

    Node *left = NULL;
    switch (p->cur.type) {
        case TOK_IDENT:     left = parse_ident(p);   break;
        case TOK_INT:       left = parse_int(p);     break;
        case TOK_FLOAT:     left = parse_float(p);   break;
        case TOK_STRING:    left = parse_string(p);  break;
        case TOK_TRUE:
        case TOK_FALSE:     left = parse_bool(p);    break;
        case TOK_NULL:      left = parse_null(p);    break;
        case TOK_BANG:
        case TOK_MINUS:     left = parse_prefix(p);  break;
        case TOK_LPAREN:    left = parse_grouped(p); break;
        case TOK_LBRACKET:  left = parse_array_lit(p); break;
        case TOK_LBRACE:    left = parse_hash_lit(p);  break;
        case TOK_FN:        left = parse_fn(p);      break;
        case TOK_IF:        left = parse_if(p);      break;
        case TOK_AWAIT: {
            int line = p->cur.line;
            advance(p);
            Node *expr = parse_expr(p, PREC_PREFIX);
            Node *n    = node_new(NODE_AWAIT, line);
            n->await_.expr = expr;
            left = n;
            break;
        }
        default:
            set_error(p, "line %d: unexpected token '%s' in expression",
                      p->cur.line, tok_name(p->cur.type));
            return NULL;
    }

    while (!p->error && left) {
        Prec next = tok_prec(p->cur.type);
        if (next <= prec && !(cur_is(p, TOK_ASSIGN) && prec < PREC_ASSIGN)) break;
        if (cur_is(p, TOK_LPAREN))   { left = parse_call(p, left);  continue; }
        if (cur_is(p, TOK_LBRACKET)) { left = parse_index(p, left); continue; }
        if (cur_is(p, TOK_DOT))      { left = parse_dot(p, left);   continue; }
        if (tok_prec(p->cur.type) <= prec) break;
        left = parse_infix(p, left);
    }
    return left;
}

/* ---- Statements ---- */
static Node *parse_block(Parser *p) {
    if (!expect(p, TOK_LBRACE)) return NULL;
    Node *b = node_new(NODE_BLOCK, p->cur.line);
    nodelist_init(&b->block.stmts);
    while (!cur_is(p, TOK_RBRACE) && !cur_is(p, TOK_EOF)) {
        if (p->error) break;
        Node *s = parse_stmt(p);
        if (s) nodelist_push(&b->block.stmts, s);
    }
    if (!p->error) expect(p, TOK_RBRACE);
    return b;
}

static Node *parse_let(Parser *p) {
    int line = p->cur.line;
    advance(p); /* skip let */
    if (!cur_is(p, TOK_IDENT)) {
        set_error(p, "line %d: expected identifier after 'let'", p->cur.line);
        return NULL;
    }
    char *name = strdup(p->cur.value);
    advance(p);
    if (!expect(p, TOK_ASSIGN)) { free(name); return NULL; }
    Node *val = parse_expr(p, PREC_NONE);
    if (!val) { free(name); return NULL; }
    Node *n    = node_new(NODE_LET, line);
    n->let.name  = name;
    n->let.value = val;
    /* optional semicolon */
    if (cur_is(p, TOK_SEMICOLON)) advance(p);
    return n;
}

static Node *parse_return(Parser *p) {
    int line = p->cur.line;
    advance(p); /* skip return */
    Node *val = NULL;
    if (!cur_is(p, TOK_RBRACE) && !cur_is(p, TOK_EOF) && !cur_is(p, TOK_SEMICOLON)) {
        val = parse_expr(p, PREC_NONE);
    }
    if (!val) {
        val = node_new(NODE_NULL, line);
    }
    if (cur_is(p, TOK_SEMICOLON)) advance(p);
    Node *n    = node_new(NODE_RETURN, line);
    n->ret.value = val;
    return n;
}

static Node *parse_while(Parser *p) {
    int line = p->cur.line;
    advance(p); /* skip while */
    Node *cond = parse_expr(p, PREC_NONE);
    Node *body = parse_block(p);
    Node *n    = node_new(NODE_WHILE, line);
    n->while_.cond = cond;
    n->while_.body = body;
    return n;
}

static Node *parse_for(Parser *p) {
    int line = p->cur.line;
    advance(p); /* skip for */
    if (!cur_is(p, TOK_IDENT)) {
        set_error(p, "line %d: expected identifier in for", p->cur.line);
        return NULL;
    }
    char *var = strdup(p->cur.value);
    advance(p);
    if (!expect(p, TOK_IN)) { free(var); return NULL; }
    Node *iter = parse_expr(p, PREC_NONE);
    Node *body = parse_block(p);
    Node *n    = node_new(NODE_FOR, line);
    n->for_.var  = var;
    n->for_.iter = iter;
    n->for_.body = body;
    return n;
}

static Node *parse_import(Parser *p) {
    int line = p->cur.line;
    advance(p); /* skip import */
    if (!cur_is(p, TOK_STRING)) {
        set_error(p, "line %d: import expects a string path", p->cur.line);
        return NULL;
    }
    Node *n = node_new(NODE_IMPORT, line);
    n->import_.path       = strdup(p->cur.value);
    n->import_.alias      = NULL;
    n->import_.names      = NULL;
    n->import_.name_count = 0;
    advance(p);

    if (cur_is(p, TOK_AS)) {
        advance(p);
        if (!cur_is(p, TOK_IDENT)) {
            set_error(p, "line %d: expected identifier after 'as'", p->cur.line);
            node_free(n); return NULL;
        }
        n->import_.alias = strdup(p->cur.value);
        advance(p);
    } else if (cur_is(p, TOK_USE)) {
        advance(p);
        int cap = 4;
        n->import_.names = malloc(cap * sizeof(char *));
        while (cur_is(p, TOK_IDENT)) {
            if (n->import_.name_count >= cap) {
                cap *= 2;
                n->import_.names = realloc(n->import_.names, cap * sizeof(char *));
            }
            n->import_.names[n->import_.name_count++] = strdup(p->cur.value);
            advance(p);
            if (cur_is(p, TOK_COMMA)) advance(p);
            else break;
        }
    }
    if (cur_is(p, TOK_SEMICOLON)) advance(p);
    return n;
}

static Node *parse_export(Parser *p) {
    int line = p->cur.line;
    advance(p); /* skip export */
    Node *decl = parse_stmt(p);
    if (!decl) return NULL;
    Node *n = node_new(NODE_EXPORT, line);
    n->export_.decl = decl;
    return n;
}

static Node *parse_break(Parser *p) {
    Node *n = node_new(NODE_BREAK, p->cur.line);
    advance(p);
    if (cur_is(p, TOK_SEMICOLON)) advance(p);
    return n;
}

static Node *parse_continue(Parser *p) {
    Node *n = node_new(NODE_CONTINUE, p->cur.line);
    advance(p);
    if (cur_is(p, TOK_SEMICOLON)) advance(p);
    return n;
}

static Node *make_incr(const char *name, const char *op, int line) {
    Node *target = node_new(NODE_IDENT, line);
    target->ident.name = strdup(name);
    Node *dup = node_new(NODE_IDENT, line);
    dup->ident.name = strdup(name);
    Node *one = node_new(NODE_INT, line);
    one->integer.value = 1;
    Node *infix = node_new(NODE_INFIX, line);
    infix->infix.op    = strdup(op);
    infix->infix.left  = dup;
    infix->infix.right = one;
    Node *assign = node_new(NODE_ASSIGN, line);
    assign->assign.target = target;
    assign->assign.value  = infix;
    Node *stmt = node_new(NODE_EXPR_STMT, line);
    stmt->expr_stmt.expr = assign;
    return stmt;
}

static Node *parse_stmt(Parser *p) {
    if (p->error) return NULL;
    switch (p->cur.type) {
        case TOK_LET:      return parse_let(p);
        case TOK_RETURN:   return parse_return(p);
        case TOK_BREAK:    return parse_break(p);
        case TOK_CONTINUE: return parse_continue(p);
        case TOK_PLUSPLUS:
        case TOK_MINUSMINUS: {
            int line = p->cur.line;
            const char *op = cur_is(p, TOK_PLUSPLUS) ? "+" : "-";
            advance(p);
            if (!cur_is(p, TOK_IDENT)) {
                set_error(p, "line %d: '++' / '--' requires a variable", line);
                return NULL;
            }
            char *name = p->cur.value;
            Node *n = make_incr(name, op, line);
            advance(p);
            if (cur_is(p, TOK_SEMICOLON)) advance(p);
            return n;
        }
        case TOK_ASYNC: {
            int line = p->cur.line;
            advance(p); /* consume async */
            if (!cur_is(p, TOK_FN)) {
                set_error(p, "line %d: expected 'fn' after 'async'", line);
                return NULL;
            }
            Node *fn = parse_fn(p);
            if (!fn) return NULL;
            fn->fn.is_async = 1;
            if (!fn->fn.name) return fn; /* anonymous async fn expr */
            char *name = strdup(fn->fn.name);
            Node *n    = node_new(NODE_LET, line);
            n->let.name  = name;
            n->let.value = fn;
            return n;
        }
        case TOK_WHILE:  return parse_while(p);
        case TOK_FOR:    return parse_for(p);
        case TOK_IMPORT: return parse_import(p);
        case TOK_EXPORT: return parse_export(p);
        case TOK_FN: {
            /* top-level fn foo() {} → let foo = fn foo() {} */
            int  line = p->cur.line;
            Node *fn  = parse_fn(p);
            if (!fn || !fn->fn.name) return fn ? (Node *)({ Node *r = node_new(NODE_EXPR_STMT, line); r->expr_stmt.expr = fn; r; }) : NULL;
            char *name = strdup(fn->fn.name);
            Node *n    = node_new(NODE_LET, line);
            n->let.name  = name;
            n->let.value = fn;
            return n;
        }
        default: {
            int  line = p->cur.line;
            Node *expr= parse_expr(p, PREC_NONE);
            if (!expr) return NULL;
            /* postfix i++ / i-- */
            if ((cur_is(p, TOK_PLUSPLUS) || cur_is(p, TOK_MINUSMINUS)) &&
                expr->type == NODE_IDENT) {
                const char *op = cur_is(p, TOK_PLUSPLUS) ? "+" : "-";
                char *name = expr->ident.name;
                Node *n = make_incr(name, op, line);
                node_free(expr);
                advance(p);
                if (cur_is(p, TOK_SEMICOLON)) advance(p);
                return n;
            }
            if (cur_is(p, TOK_SEMICOLON)) advance(p);
            Node *n    = node_new(NODE_EXPR_STMT, line);
            n->expr_stmt.expr = expr;
            return n;
        }
    }
}

/* ---- Public API ---- */
Parser *parser_new(Lexer *l) {
    Parser *p = calloc(1, sizeof(Parser));
    p->lexer  = l;
    p->cur    = lexer_next(l);
    p->peek   = lexer_next(l);
    return p;
}

void parser_free(Parser *p) {
    token_free(p->cur);
    token_free(p->peek);
    free(p->error);
    free(p);
}

Node *parser_parse(Parser *p) {
    Node *prog = node_new(NODE_PROGRAM, 1);
    nodelist_init(&prog->block.stmts);
    while (!cur_is(p, TOK_EOF) && !p->error) {
        Node *s = parse_stmt(p);
        if (s) nodelist_push(&prog->block.stmts, s);
    }
    return prog;
}
