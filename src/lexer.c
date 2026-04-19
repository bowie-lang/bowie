#include "lexer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Lexer *lexer_new(const char *src) {
    Lexer *l = malloc(sizeof(Lexer));
    l->src  = src;
    l->pos  = 0;
    l->line = 1;
    l->len  = (int)strlen(src);
    return l;
}

void lexer_free(Lexer *l) { free(l); }

static char peek(Lexer *l) {
    return l->pos < l->len ? l->src[l->pos] : '\0';
}
static char peek2(Lexer *l) {
    return l->pos + 1 < l->len ? l->src[l->pos + 1] : '\0';
}
static char adv(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') l->line++;
    return c;
}

static void skip_ws(Lexer *l) {
    for (;;) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            adv(l);
        } else if (c == '#') {
            while (l->pos < l->len && peek(l) != '\n') adv(l);
        } else {
            break;
        }
    }
}

static Token mktok(TokenType type, const char *val, int line) {
    return (Token){ type, strdup(val), line };
}

static Token read_string(Lexer *l) {
    int line = l->line;
    adv(l); /* skip " */
    int  cap = 64, len = 0;
    char *buf = malloc(cap);
    while (l->pos < l->len && peek(l) != '"') {
        char c = adv(l);
        if (c == '\\') {
            char e = adv(l);
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"';  break;
                case '\\':c = '\\'; break;
                default:  c = e;   break;
            }
        }
        if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = c;
    }
    buf[len] = '\0';
    if (peek(l) == '"') adv(l);
    return (Token){ TOK_STRING, buf, line };
}

static Token read_number(Lexer *l) {
    int line  = l->line;
    int start = l->pos;
    int is_f  = 0;
    while (l->pos < l->len && isdigit(peek(l))) adv(l);
    if (peek(l) == '.' && isdigit(peek2(l))) {
        is_f = 1; adv(l);
        while (l->pos < l->len && isdigit(peek(l))) adv(l);
    }
    int   len = l->pos - start;
    char *val = malloc(len + 1);
    memcpy(val, l->src + start, len);
    val[len] = '\0';
    return (Token){ is_f ? TOK_FLOAT : TOK_INT, val, line };
}

static Token read_ident(Lexer *l) {
    int line  = l->line;
    int start = l->pos;
    while (l->pos < l->len && (isalnum(peek(l)) || peek(l) == '_')) adv(l);
    int   len = l->pos - start;
    char *val = malloc(len + 1);
    memcpy(val, l->src + start, len);
    val[len] = '\0';

    TokenType type = TOK_IDENT;
    if      (!strcmp(val, "let"))    type = TOK_LET;
    else if (!strcmp(val, "fn"))     type = TOK_FN;
    else if (!strcmp(val, "return")) type = TOK_RETURN;
    else if (!strcmp(val, "if"))     type = TOK_IF;
    else if (!strcmp(val, "else"))   type = TOK_ELSE;
    else if (!strcmp(val, "while"))  type = TOK_WHILE;
    else if (!strcmp(val, "for"))    type = TOK_FOR;
    else if (!strcmp(val, "in"))     type = TOK_IN;
    else if (!strcmp(val, "true"))   type = TOK_TRUE;
    else if (!strcmp(val, "false"))  type = TOK_FALSE;
    else if (!strcmp(val, "null"))   type = TOK_NULL;
    else if (!strcmp(val, "import")) type = TOK_IMPORT;
    else if (!strcmp(val, "export")) type = TOK_EXPORT;
    else if (!strcmp(val, "as"))     type = TOK_AS;
    else if (!strcmp(val, "use"))      type = TOK_USE;
    else if (!strcmp(val, "break"))    type = TOK_BREAK;
    else if (!strcmp(val, "continue")) type = TOK_CONTINUE;
    else if (!strcmp(val, "async"))    type = TOK_ASYNC;
    else if (!strcmp(val, "await"))    type = TOK_AWAIT;
    else if (!strcmp(val, "try"))      type = TOK_TRY;
    else if (!strcmp(val, "catch"))    type = TOK_CATCH;
    else if (!strcmp(val, "finally"))  type = TOK_FINALLY;
    else if (!strcmp(val, "throw"))    type = TOK_THROW;

    return (Token){ type, val, line };
}

Token lexer_next(Lexer *l) {
    skip_ws(l);
    int line = l->line;
    if (l->pos >= l->len) return mktok(TOK_EOF, "", line);

    char c = peek(l);
    if (c == '"')                    return read_string(l);
    if (isdigit(c))                  return read_number(l);
    if (isalpha(c) || c == '_')      return read_ident(l);

    adv(l);
    switch (c) {
        case '+':
            if (peek(l) == '+') { adv(l); return mktok(TOK_PLUSPLUS,   "++", line); }
            return mktok(TOK_PLUS,  "+", line);
        case '-':
            if (peek(l) == '-') { adv(l); return mktok(TOK_MINUSMINUS, "--", line); }
            return mktok(TOK_MINUS, "-", line);
        case '*': return mktok(TOK_STAR,     "*", line);
        case '/': return mktok(TOK_SLASH,    "/", line);
        case '%': return mktok(TOK_PERCENT,  "%", line);
        case '(': return mktok(TOK_LPAREN,   "(", line);
        case ')': return mktok(TOK_RPAREN,   ")", line);
        case '{': return mktok(TOK_LBRACE,   "{", line);
        case '}': return mktok(TOK_RBRACE,   "}", line);
        case '[': return mktok(TOK_LBRACKET, "[", line);
        case ']': return mktok(TOK_RBRACKET, "]", line);
        case ',': return mktok(TOK_COMMA,    ",", line);
        case ':': return mktok(TOK_COLON,    ":", line);
        case '.': return mktok(TOK_DOT,      ".", line);
        case ';': return mktok(TOK_SEMICOLON,";", line);
        case '=':
            if (peek(l) == '=') { adv(l); return mktok(TOK_EQ,  "==", line); }
            return mktok(TOK_ASSIGN, "=", line);
        case '!':
            if (peek(l) == '=') { adv(l); return mktok(TOK_NEQ, "!=", line); }
            return mktok(TOK_BANG, "!", line);
        case '<':
            if (peek(l) == '=') { adv(l); return mktok(TOK_LTE, "<=", line); }
            return mktok(TOK_LT, "<", line);
        case '>':
            if (peek(l) == '=') { adv(l); return mktok(TOK_GTE, ">=", line); }
            return mktok(TOK_GT, ">", line);
        case '&':
            if (peek(l) == '&') { adv(l); return mktok(TOK_AND, "&&", line); }
            break;
        case '|':
            if (peek(l) == '|') { adv(l); return mktok(TOK_OR,  "||", line); }
            break;
    }
    char buf[2] = { c, '\0' };
    return mktok(TOK_ILLEGAL, buf, line);
}

void token_free(Token t) { free(t.value); }

const char *tok_name(TokenType t) {
    switch (t) {
        case TOK_EOF:       return "EOF";
        case TOK_ILLEGAL:   return "ILLEGAL";
        case TOK_IDENT:     return "IDENT";
        case TOK_INT:       return "INT";
        case TOK_FLOAT:     return "FLOAT";
        case TOK_STRING:    return "STRING";
        case TOK_LET:       return "let";
        case TOK_FN:        return "fn";
        case TOK_RETURN:    return "return";
        case TOK_IF:        return "if";
        case TOK_ELSE:      return "else";
        case TOK_WHILE:     return "while";
        case TOK_FOR:       return "for";
        case TOK_IN:        return "in";
        case TOK_TRUE:      return "true";
        case TOK_FALSE:     return "false";
        case TOK_NULL:      return "null";
        case TOK_IMPORT:    return "import";
        case TOK_EXPORT:    return "export";
        case TOK_AS:        return "as";
        case TOK_USE:       return "use";
        case TOK_BREAK:       return "break";
        case TOK_CONTINUE:    return "continue";
        case TOK_ASYNC:       return "async";
        case TOK_AWAIT:       return "await";
        case TOK_TRY:         return "try";
        case TOK_CATCH:       return "catch";
        case TOK_FINALLY:     return "finally";
        case TOK_THROW:       return "throw";
        case TOK_PLUSPLUS:    return "++";
        case TOK_MINUSMINUS:  return "--";
        case TOK_PLUS:      return "+";
        case TOK_MINUS:     return "-";
        case TOK_STAR:      return "*";
        case TOK_SLASH:     return "/";
        case TOK_PERCENT:   return "%";
        case TOK_EQ:        return "==";
        case TOK_NEQ:       return "!=";
        case TOK_LT:        return "<";
        case TOK_LTE:       return "<=";
        case TOK_GT:        return ">";
        case TOK_GTE:       return ">=";
        case TOK_AND:       return "&&";
        case TOK_OR:        return "||";
        case TOK_BANG:      return "!";
        case TOK_ASSIGN:    return "=";
        case TOK_LPAREN:    return "(";
        case TOK_RPAREN:    return ")";
        case TOK_LBRACE:    return "{";
        case TOK_RBRACE:    return "}";
        case TOK_LBRACKET:  return "[";
        case TOK_RBRACKET:  return "]";
        case TOK_COMMA:     return ",";
        case TOK_COLON:     return ":";
        case TOK_DOT:       return ".";
        case TOK_SEMICOLON: return ";";
    }
    return "UNKNOWN";
}
