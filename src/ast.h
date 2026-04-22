#ifndef BOWIE_AST_H
#define BOWIE_AST_H

typedef struct Node     Node;
typedef struct NodeList NodeList;

typedef enum {
    /* Statements */
    NODE_PROGRAM,
    NODE_LET,
    NODE_RETURN,
    NODE_EXPR_STMT,
    NODE_BLOCK,
    NODE_WHILE,
    NODE_FOR,
    NODE_TRY,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_THROW,

    /* Module system */
    NODE_IMPORT,
    NODE_EXPORT,

    /* Expressions */
    NODE_IDENT,
    NODE_INT,
    NODE_FLOAT,
    NODE_STRING,
    NODE_BOOL,
    NODE_NULL,
    NODE_ARRAY,
    NODE_HASH,
    NODE_PREFIX,
    NODE_INFIX,
    NODE_ASSIGN,
    NODE_IF,
    NODE_FUNCTION,
    NODE_CALL,
    NODE_INDEX,
    NODE_AWAIT,
} NodeType;

struct NodeList {
    Node **items;
    int    count;
    int    cap;
};

typedef struct { Node *key; Node *value; } HashPair;

struct Node {
    NodeType type;
    int      line;

    union {
        /* NODE_PROGRAM / NODE_BLOCK */
        struct { NodeList stmts; } block;

        /* NODE_LET */
        struct { char *name; Node *value; int is_const; } let;

        /* NODE_RETURN */
        struct { Node *value; } ret;

        /* NODE_THROW */
        struct { Node *value; } throw_;

        /* NODE_EXPR_STMT */
        struct { Node *expr; } expr_stmt;

        /* NODE_WHILE */
        struct { Node *cond; Node *body; } while_;

        /* NODE_FOR */
        struct { char *var; Node *iter; Node *body; } for_;

        /* NODE_TRY */
        struct { Node *try_block; char *catch_ident; Node *catch_block; Node *finally_block; } try_;

        /* NODE_IDENT */
        struct { char *name; } ident;

        /* NODE_INT */
        struct { long long value; } integer;

        /* NODE_FLOAT */
        struct { double value; } float_;

        /* NODE_STRING */
        struct { char *value; } string;

        /* NODE_BOOL */
        struct { int value; } boolean;

        /* NODE_ARRAY */
        struct { NodeList elems; } array;

        /* NODE_HASH */
        struct { HashPair *pairs; int count; int cap; } hash;

        /* NODE_PREFIX */
        struct { char *op; Node *right; } prefix;

        /* NODE_INFIX */
        struct { char *op; Node *left; Node *right; } infix;

        /* NODE_ASSIGN */
        struct { Node *target; Node *value; } assign;

        /* NODE_IF */
        struct { Node *cond; Node *then_; Node *else_; } if_;

        /* NODE_FUNCTION */
        struct { char **params; int param_count; Node *body; char *name; int is_async; int has_rest; } fn;

        /* NODE_AWAIT */
        struct { Node *expr; } await_;

        /* NODE_CALL */
        struct { Node *fn; NodeList args; } call;

        /* NODE_INDEX */
        struct { Node *left; Node *index; } index;

        /* NODE_IMPORT: import "path" [as name | use n1,n2,...] */
        struct {
            char  *path;
            char  *alias;    /* "as name"  — NULL if not present */
            char **names;    /* "use a, b" — NULL if not present */
            int    name_count;
        } import_;

        /* NODE_EXPORT: export let/fn/expr */
        struct { Node *decl; } export_;
    };
};

Node *node_new(NodeType type, int line);
void  node_free(Node *n);

void nodelist_init(NodeList *nl);
void nodelist_push(NodeList *nl, Node *n);
void nodelist_free(NodeList *nl);

#endif
