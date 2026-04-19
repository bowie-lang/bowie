#include "ast.h"
#include <stdlib.h>
#include <string.h>

Node *node_new(NodeType type, int line) {
    Node *n = calloc(1, sizeof(Node));
    n->type = type;
    n->line = line;
    return n;
}

void nodelist_init(NodeList *nl) {
    nl->items = NULL;
    nl->count = 0;
    nl->cap   = 0;
}

void nodelist_push(NodeList *nl, Node *n) {
    if (nl->count >= nl->cap) {
        nl->cap   = nl->cap ? nl->cap * 2 : 8;
        nl->items = realloc(nl->items, nl->cap * sizeof(Node *));
    }
    nl->items[nl->count++] = n;
}

void nodelist_free(NodeList *nl) {
    for (int i = 0; i < nl->count; i++) node_free(nl->items[i]);
    free(nl->items);
}

void node_free(Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_PROGRAM:
        case NODE_BLOCK:
            nodelist_free(&n->block.stmts);
            break;
        case NODE_LET:
            free(n->let.name);
            node_free(n->let.value);
            break;
        case NODE_RETURN:
            node_free(n->ret.value);
            break;
        case NODE_EXPR_STMT:
            node_free(n->expr_stmt.expr);
            break;
        case NODE_WHILE:
            node_free(n->while_.cond);
            node_free(n->while_.body);
            break;
        case NODE_FOR:
            free(n->for_.var);
            node_free(n->for_.iter);
            node_free(n->for_.body);
            break;
        case NODE_IDENT:
            free(n->ident.name);
            break;
        case NODE_STRING:
            free(n->string.value);
            break;
        case NODE_ARRAY:
            nodelist_free(&n->array.elems);
            break;
        case NODE_HASH:
            for (int i = 0; i < n->hash.count; i++) {
                node_free(n->hash.pairs[i].key);
                node_free(n->hash.pairs[i].value);
            }
            free(n->hash.pairs);
            break;
        case NODE_PREFIX:
            free(n->prefix.op);
            node_free(n->prefix.right);
            break;
        case NODE_INFIX:
            free(n->infix.op);
            node_free(n->infix.left);
            node_free(n->infix.right);
            break;
        case NODE_ASSIGN:
            node_free(n->assign.target);
            node_free(n->assign.value);
            break;
        case NODE_IF:
            node_free(n->if_.cond);
            node_free(n->if_.then_);
            node_free(n->if_.else_);
            break;
        case NODE_FUNCTION:
            for (int i = 0; i < n->fn.param_count; i++) free(n->fn.params[i]);
            free(n->fn.params);
            free(n->fn.name);
            node_free(n->fn.body);
            break;
        case NODE_CALL:
            node_free(n->call.fn);
            nodelist_free(&n->call.args);
            break;
        case NODE_INDEX:
            node_free(n->index.left);
            node_free(n->index.index);
            break;
        case NODE_IMPORT:
            free(n->import_.path);
            free(n->import_.alias);
            for (int i = 0; i < n->import_.name_count; i++) free(n->import_.names[i]);
            free(n->import_.names);
            break;
        case NODE_EXPORT:
            node_free(n->export_.decl);
            break;
        case NODE_AWAIT:
            node_free(n->await_.expr);
            break;
        default:
            break;
    }
    free(n);
}
