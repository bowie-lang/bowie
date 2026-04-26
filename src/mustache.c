#include "mustache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- Dynamic string buffer ---- */

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
} MStrBuf;

static void mstrbuf_init(MStrBuf *b) {
    b->cap = 256;
    b->len = 0;
    b->buf = malloc(b->cap);
    b->buf[0] = '\0';
}

static void mstrbuf_append(MStrBuf *b, const char *s, size_t n) {
    while (b->len + n + 1 > b->cap) {
        b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void mstrbuf_append_escaped(MStrBuf *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '&':  mstrbuf_append(b, "&amp;",  5); break;
            case '<':  mstrbuf_append(b, "&lt;",   4); break;
            case '>':  mstrbuf_append(b, "&gt;",   4); break;
            case '"':  mstrbuf_append(b, "&quot;", 6); break;
            case '\'': mstrbuf_append(b, "&#x27;", 6); break;
            default:   mstrbuf_append(b, s, 1);        break;
        }
    }
}

static void mstrbuf_append_obj(MStrBuf *b, Object *v, int escape) {
    if (!v || v == BOWIE_NULL) return;
    char *s = obj_inspect(v);
    if (escape) mstrbuf_append_escaped(b, s);
    else        mstrbuf_append(b, s, strlen(s));
    free(s);
}

static char *mstrbuf_take(MStrBuf *b) {
    char *r = b->buf;
    b->buf = NULL;
    b->len = b->cap = 0;
    return r;
}

static void mstrbuf_free(MStrBuf *b) {
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* ---- Context stack ---- */

typedef struct MCtxFrame {
    Object          *data;
    struct MCtxFrame *parent;
} MCtxFrame;

/* ---- Renderer state ---- */

typedef struct {
    MStrBuf   out;
    MCtxFrame *top;
    char      *error;
} MCtx;

static void ctx_set_error(MCtx *ctx, const char *fmt, ...) {
    if (ctx->error) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->error = strdup(buf);
}

/* ---- Variable lookup ---- */

static Object *ctx_lookup_on(Object *obj, const char *path);

static Object *ctx_lookup_on(Object *obj, const char *path) {
    if (!obj || obj == BOWIE_NULL) return BOWIE_NULL;
    const char *dot = strchr(path, '.');
    if (!dot) {
        if (obj->type != OBJ_HASH) return BOWIE_NULL;
        return hash_get(obj, path);
    }
    if (obj->type != OBJ_HASH) return BOWIE_NULL;
    char seg[256];
    size_t n = (size_t)(dot - path);
    if (n >= sizeof(seg)) n = sizeof(seg) - 1;
    memcpy(seg, path, n);
    seg[n] = '\0';
    Object *child = hash_get(obj, seg);
    if (child == BOWIE_NULL) return BOWIE_NULL;
    return ctx_lookup_on(child, dot + 1);
}

static Object *ctx_lookup(MCtxFrame *frame, const char *name) {
    if (!frame) return BOWIE_NULL;
    if (strcmp(name, ".") == 0) return frame->data;

    if (strchr(name, '.')) {
        /* Try in each frame going up */
        char seg[256];
        const char *dot = strchr(name, '.');
        size_t n = (size_t)(dot - name);
        if (n >= sizeof(seg)) n = sizeof(seg) - 1;
        memcpy(seg, name, n);
        seg[n] = '\0';
        for (MCtxFrame *f = frame; f; f = f->parent) {
            if (!f->data || f->data->type != OBJ_HASH) continue;
            Object *base = hash_get(f->data, seg);
            if (base != BOWIE_NULL)
                return ctx_lookup_on(base, dot + 1);
        }
        return BOWIE_NULL;
    }

    for (MCtxFrame *f = frame; f; f = f->parent) {
        if (!f->data || f->data->type != OBJ_HASH) continue;
        Object *v = hash_get(f->data, name);
        if (v != BOWIE_NULL) return v;
    }
    return BOWIE_NULL;
}

/* ---- Tag name parser ---- */

/*
 * Extract trimmed tag name from [p, close).
 * Returns close pointer (for caller to advance past the closing braces themselves).
 */
static void extract_tag(const char *p, const char *close,
                         char *out, size_t out_max) {
    /* trim leading whitespace */
    while (p < close && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    /* trim trailing whitespace */
    while (close > p && (*(close-1) == ' ' || *(close-1) == '\t' ||
                         *(close-1) == '\n' || *(close-1) == '\r')) close--;
    size_t n = (size_t)(close - p);
    if (n >= out_max) n = out_max - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Find first occurrence of needle[0..nlen-1] in [p, end). */
static const char *find_str(const char *p, const char *end,
                              const char *needle, size_t nlen) {
    for (; p + nlen <= end; p++) {
        if (memcmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

/* ---- Forward declaration ---- */
static const char *mustache_chunk(const char *p, const char *end,
                                   const char *stop_tag, MCtx *ctx);

/* ---- Section renderer ---- */

static void render_section(const char *body_start, const char *tmpl_end,
                            const char *name, int inverted,
                            MCtx *ctx, const char **out_after) {
    Object *v = ctx_lookup(ctx->top, name);

    int is_array       = (v->type == OBJ_ARRAY);
    int empty_array    = is_array && (v->array.elems.count == 0);
    int truthy         = obj_is_truthy(v) && !empty_array;
    int should_render  = inverted ? !truthy : truthy;

    if (should_render && is_array && !inverted) {
        /* First pass: scan once to find end of section body */
        MStrBuf discard;
        mstrbuf_init(&discard);
        MStrBuf saved_out = ctx->out;
        ctx->out = discard;
        const char *body_end = mustache_chunk(body_start, tmpl_end, name, ctx);
        discard = ctx->out;
        ctx->out = saved_out;
        mstrbuf_free(&discard);
        if (ctx->error) { *out_after = body_end; return; }
        *out_after = body_end;

        /* Second pass: render once per element */
        for (int i = 0; i < v->array.elems.count; i++) {
            Object *elem = v->array.elems.items[i];
            MCtxFrame frame = { elem, ctx->top };
            MCtxFrame *saved_top = ctx->top;
            ctx->top = &frame;
            mustache_chunk(body_start, tmpl_end, name, ctx);
            ctx->top = saved_top;
            if (ctx->error) return;
        }
    } else if (should_render) {
        MCtxFrame *saved_top = ctx->top;
        MCtxFrame frame;
        if (!inverted && v->type == OBJ_HASH) {
            frame.data   = v;
            frame.parent = ctx->top;
            ctx->top = &frame;
        }
        const char *after = mustache_chunk(body_start, tmpl_end, name, ctx);
        ctx->top = saved_top;
        *out_after = after;
    } else {
        /* Skip section body without emitting */
        MStrBuf discard;
        mstrbuf_init(&discard);
        MStrBuf saved_out = ctx->out;
        ctx->out = discard;
        const char *after = mustache_chunk(body_start, tmpl_end, name, ctx);
        discard = ctx->out;
        ctx->out = saved_out;
        mstrbuf_free(&discard);
        *out_after = after;
    }
}

/* ---- Main render loop ---- */

static const char *mustache_chunk(const char *p, const char *end,
                                   const char *stop_tag, MCtx *ctx) {
    char tag[256];

    while (p < end) {
        if (ctx->error) return p;

        /* Find next {{ */
        const char *open = find_str(p, end, "{{", 2);
        if (!open) {
            mstrbuf_append(&ctx->out, p, (size_t)(end - p));
            p = end;
            break;
        }

        /* Emit literal before {{ */
        if (open > p)
            mstrbuf_append(&ctx->out, p, (size_t)(open - p));

        const char *inner = open + 2; /* points to char right after '{{' */

        if (inner >= end) {
            ctx_set_error(ctx, "unclosed tag at end of template");
            return end;
        }

        char tc = *inner; /* tag type character */

        if (tc == '{') {
            /* Triple mustache {{{var}}} — unescaped */
            const char *content_start = inner + 1; /* past the third '{' */
            const char *close = find_str(content_start, end, "}}}", 3);
            if (!close) { ctx_set_error(ctx, "unclosed {{{"); return end; }
            extract_tag(content_start, close, tag, sizeof(tag));
            Object *v = ctx_lookup(ctx->top, tag);
            mstrbuf_append_obj(&ctx->out, v, 0);
            p = close + 3;

        } else if (tc == '#' || tc == '^') {
            /* Section {{#name}} or inverted {{^name}} */
            const char *content_start = inner + 1;
            const char *close = find_str(content_start, end, "}}", 2);
            if (!close) { ctx_set_error(ctx, "unclosed section tag"); return end; }
            extract_tag(content_start, close, tag, sizeof(tag));
            int inverted = (tc == '^');
            const char *body_start = close + 2;
            const char *after_section = body_start;
            render_section(body_start, end, tag, inverted, ctx, &after_section);
            p = after_section;

        } else if (tc == '/') {
            /* Close tag {{/name}} */
            const char *content_start = inner + 1;
            const char *close = find_str(content_start, end, "}}", 2);
            if (!close) { ctx_set_error(ctx, "unclosed close tag"); return end; }
            extract_tag(content_start, close, tag, sizeof(tag));
            p = close + 2;
            if (stop_tag && strcmp(tag, stop_tag) == 0)
                return p;
            ctx_set_error(ctx, "unexpected {{/%s}}", tag);
            return p;

        } else if (tc == '!') {
            /* Comment {{!...}} — discard */
            const char *close = find_str(inner + 1, end, "}}", 2);
            p = close ? close + 2 : end;

        } else {
            /* Regular variable {{var}} */
            const char *close = find_str(inner, end, "}}", 2);
            if (!close) { ctx_set_error(ctx, "unclosed tag"); return end; }
            extract_tag(inner, close, tag, sizeof(tag));
            Object *v = ctx_lookup(ctx->top, tag);
            mstrbuf_append_obj(&ctx->out, v, 1);
            p = close + 2;
        }
    }

    if (stop_tag && !ctx->error)
        ctx_set_error(ctx, "unclosed section '%s'", stop_tag);

    return end;
}

/* ---- Public API ---- */

char *mustache_render(const char *tmpl, Object *data, char **err_out) {
    MCtxFrame root = { data, NULL };
    MCtx ctx;
    ctx.top   = &root;
    ctx.error = NULL;
    mstrbuf_init(&ctx.out);

    mustache_chunk(tmpl, tmpl + strlen(tmpl), NULL, &ctx);

    if (ctx.error) {
        if (err_out) *err_out = ctx.error;
        else free(ctx.error);
        mstrbuf_free(&ctx.out);
        return NULL;
    }

    if (err_out) *err_out = NULL;
    return mstrbuf_take(&ctx.out);
}
