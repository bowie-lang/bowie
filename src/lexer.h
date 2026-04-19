#ifndef BOWIE_LEXER_H
#define BOWIE_LEXER_H

typedef enum {
    TOK_EOF = 0,
    TOK_ILLEGAL,

    /* Literals */
    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,

    /* Keywords */
    TOK_LET,
    TOK_FN,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_IMPORT,
    TOK_EXPORT,
    TOK_AS,
    TOK_USE,
    TOK_BREAK,
    TOK_CONTINUE,

    /* Arithmetic */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,

    /* Comparison */
    TOK_EQ,
    TOK_NEQ,
    TOK_LT,
    TOK_LTE,
    TOK_GT,
    TOK_GTE,

    /* Logical */
    TOK_AND,
    TOK_OR,
    TOK_BANG,

    /* Assignment */
    TOK_ASSIGN,

    /* Delimiters */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_COLON,
    TOK_DOT,
    TOK_SEMICOLON,
} TokenType;

typedef struct {
    TokenType type;
    char     *value; /* heap-allocated */
    int       line;
} Token;

typedef struct {
    const char *src;
    int         pos;
    int         line;
    int         len;
} Lexer;

Lexer      *lexer_new(const char *src);
void        lexer_free(Lexer *l);
Token       lexer_next(Lexer *l);
void        token_free(Token t);
const char *tok_name(TokenType t);

#endif
