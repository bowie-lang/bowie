#ifndef BOWIE_PARSER_H
#define BOWIE_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer  *lexer;
    Token   cur;
    Token   peek;
    char   *error;   /* set on parse error */
} Parser;

Parser *parser_new(Lexer *l);
void    parser_free(Parser *p);
Node   *parser_parse(Parser *p);   /* returns NODE_PROGRAM */

#endif
