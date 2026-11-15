/* Natives for the prelude: inline style and computed style, geometry,
 * URLs, text encoding, and the embedder hooks (dialogs, navigation,
 * history, cookies, storage, media queries, focus). */
#include "js_int.h"
#include "../css/css.h"

#include <stdio.h>
#include <string.h>

static AuElement *arg_el(AuJs *js, JSValueConst v)
{
    AuNode *n = js_unwrap(js, v);
    return n && n->type == AU_NODE_ELEMENT ? (AuElement *)n : NULL;
}

/* ---- Inline style ----
 * The style attribute is the store: edits parse the attribute into a
 * list of declarations (through src/css's serialization, so values are
 * validated and canonical), change the list and write the attribute back.
 * src/css caches parsed inline blocks by text, so this is cheap to
 * restyle. */

typedef struct Decl {
    char *name, *value;
    bool important;
} Decl;

typedef struct DeclList {
    Decl *d;
    size_t n, cap;
} DeclList;

static void decls_free(DeclList *l)
{
    for (size_t i = 0; i < l->n; i++) {
        au_free(l->d[i].name);
        au_free(l->d[i].value);
    }
    au_free(l->d);
    l->d = NULL;
    l->n = l->cap = 0;
}

static void decls_add(DeclList *l, const char *name, size_t nl, const char *value, size_t vl, bool imp)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->d = au_realloc(l->d, l->cap * sizeof *l->d);
    }
    l->d[l->n].name = au_strndup(name, nl);
    l->d[l->n].value = au_strndup(value, vl);
    l->d[l->n].important = imp;
    l->n++;
}

/* Splits "a: b; c: d !important;" at top-level semicolons. */
static void decls_parse_serialized(DeclList *l, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && au_is_space((unsigned char)s[i]))
            i++;
        size_t st = i;
        int depth = 0;
        char quote = 0;
        for (; i < n; i++) {
            char c = s[i];
            if (quote) {
                if (c == '\\' && i + 1 < n)
                    i++;
                else if (c == quote)
                    quote = 0;
            } else if (c == '"' || c == '\'')
                quote = c;
            else if (c == '(')
                depth++;
            else if (c == ')' && depth)
                depth--;
            else if (c == ';' && !depth)
                break;
        }
        size_t en = i;
        if (i < n)
            i++;
        const char *colon = memchr(s + st, ':', en - st);
        if (!colon)
            continue;
        size_t nl = (size_t)(colon - (s + st));
        while (nl && au_is_space((unsigned char)s[st + nl - 1]))
            nl--;
        const char *v = colon + 1;
        size_t vl = (size_t)(s + en - v);
        while (vl && au_is_space((unsigned char)*v)) {
            v++;
            vl--;
        }
        while (vl && au_is_space((unsigned char)v[vl - 1]))
            vl--;
        bool imp = false;
        if (vl >= 10 && au_ieq(v + vl - 10, 10, "!important", 10)) {
            imp = true;
            vl -= 10;
            while (vl && au_is_space((unsigned char)v[vl - 1]))
                vl--;
        }
        if (nl)
            decls_add(l, s + st, nl, v, vl, imp);
    }
}

/* The declarations of a CSS text, as src/css serializes them. */
static void decls_of_text(DeclList *l, const char *css, size_t len)
{
    if (!css || !len)
        return;
    AuCssDeclBlock *b = au_css_parse_inline_style(css, len, NULL);
    AuBuf out;
    au_buf_init(&out);
    au_css_decl_block_serialize(b, &out);
    decls_parse_serialized(l, out.data ? out.data : "", out.len);
    au_buf_free(&out);
    au_css_decl_block_free(b);
}

static void decls_of_element(DeclList *l, const AuElement *e)
{
    const AuAttr *a = au_element_attr(e, AU_ATOM_style);
    if (a)
        decls_of_text(l, a->value, a->len);
}

static void decls_write(AuElement *e, const DeclList *l)
{
    AuBuf b;
    au_buf_init(&b);
    for (size_t i = 0; i < l->n; i++) {
        if (b.len)
            au_buf_putc(&b, ' ');
        au_buf_printf(&b, "%s: %s%s;", l->d[i].name, l->d[i].value, l->d[i].important ? " !important" : "");
    }
    au_element_set_attr(e, AU_ATOM_style, b.data ? b.data : "", b.len);
    au_buf_free(&b);
}

static void decls_remove(DeclList *l, const char *name)
{
    for (size_t i = 0; i < l->n;) {
        if (!strcmp(l->d[i].name, name)) {
            au_free(l->d[i].name);
            au_free(l->d[i].value);
            memmove(&l->d[i], &l->d[i + 1], (l->n - i - 1) * sizeof *l->d);
            l->n--;
        } else {
            i++;
        }
    }
}

/* The longhand names a property name stands for (itself for longhands
 * and custom properties), found by parsing "name: initial". */
static void longhands_of(const char *name, DeclList *out)
{
    if (name[0] == '-' && name[1] == '-') {
        decls_add(out, name, strlen(name), "", 0, false);
        return;
    }
    AuBuf t;
    au_buf_init(&t);
    au_buf_printf(&t, "%s: initial", name);
    decls_of_text(out, t.data, t.len);
    au_buf_free(&t);
}

static char *lower_prop(JSContext *ctx, JSValueConst v)
{
    size_t n;
    char *s = js_tostr(ctx, v, &n);
    if (s && !(s[0] == '-' && s[1] == '-'))
        for (size_t i = 0; i < n; i++)
            s[i] = (char)au_lower((unsigned char)s[i]);
    return s;
}

/* styleSet(el, prop, value, priority) */
static JSValue n_style_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 2 ? arg_el(js, argv[0]) : NULL;
    if (!e)
        return JS_UNDEFINED;
    char *prop = lower_prop(ctx, argv[1]);
    size_t vl;
    char *val = js_tostr(ctx, argv[2], &vl);
    char *prio = argc > 3 && !JS_IsUndefined(argv[3]) ? js_tostr(ctx, argv[3], NULL) : NULL;
    if (!prop || !val) {
        au_free(prop);
        au_free(val);
        au_free(prio);
        return JS_EXCEPTION;
    }
    bool imp = prio && au_ieq(prio, strlen(prio), "important", 9);
    DeclList cur = {0}, add = {0};
    decls_of_element(&cur, e);
    if (vl == 0) {
        /* Setting an empty value removes the property. */
        longhands_of(prop, &add);
        for (size_t i = 0; i < add.n; i++)
            decls_remove(&cur, add.d[i].name);
        decls_remove(&cur, prop);
        decls_write(e, &cur);
    } else {
        AuBuf t;
        au_buf_init(&t);
        au_buf_printf(&t, "%s: %s%s", prop, val, imp ? " !important" : "");
        decls_of_text(&add, t.data, t.len);
        au_buf_free(&t);
        if (add.n) { /* invalid values are ignored */
            for (size_t i = 0; i < add.n; i++)
                decls_remove(&cur, add.d[i].name);
            for (size_t i = 0; i < add.n; i++)
                decls_add(&cur, add.d[i].name, strlen(add.d[i].name), add.d[i].value, strlen(add.d[i].value),
                          add.d[i].important);
            decls_write(e, &cur);
        }
    }
    decls_free(&cur);
    decls_free(&add);
    au_free(prop);
    au_free(val);
    au_free(prio);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

/* styleGet(el, prop, what): what 0 value, 1 priority. */
static JSValue n_style_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 1 ? arg_el(js, argv[0]) : NULL;
    if (!e)
        return JS_NewString(ctx, "");
    int32_t what = 0;
    if (argc > 2)
        JS_ToInt32(ctx, &what, argv[2]);
    char *prop = lower_prop(ctx, argv[1]);
    if (!prop)
        return JS_EXCEPTION;
    JSValue r = JS_NewString(ctx, "");
    const AuAttr *a = au_element_attr(e, AU_ATOM_style);
    if (a && a->len) {
        if (what == 0) {
            AuCssDeclBlock *b = au_css_parse_inline_style(a->value, a->len, NULL);
            AuBuf out;
            au_buf_init(&out);
            if (au_css_decl_block_get(b, au_atom_cstr(prop), &out)) {
                JS_FreeValue(ctx, r);
                r = js_str(ctx, out.data, out.len);
            }
            au_buf_free(&out);
            au_css_decl_block_free(b);
        } else {
            DeclList cur = {0}, lh = {0};
            decls_of_element(&cur, e);
            longhands_of(prop, &lh);
            bool all = lh.n > 0;
            for (size_t i = 0; i < lh.n && all; i++) {
                bool found = false;
                for (size_t j = 0; j < cur.n; j++)
                    if (!strcmp(cur.d[j].name, lh.d[i].name) && cur.d[j].important)
                        found = true;
                all = found;
            }
            if (all) {
                JS_FreeValue(ctx, r);
                r = JS_NewString(ctx, "important");
            }
            decls_free(&cur);
            decls_free(&lh);
        }
    }
    au_free(prop);
    return r;
}

/* styleNames(el): the longhand names in the inline style, in order. */
static JSValue n_style_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 0 ? arg_el(js, argv[0]) : NULL;
    JSValue arr = JS_NewArray(ctx);
    if (!e)
        return arr;
    DeclList cur = {0};
    decls_of_element(&cur, e);
    for (size_t i = 0; i < cur.n; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, cur.d[i].name));
    decls_free(&cur);
    return arr;
}

/* styleText(el): cssText. */
static JSValue n_style_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 0 ? arg_el(js, argv[0]) : NULL;
    if (!e)
        return JS_NewString(ctx, "");
    const AuAttr *a = au_element_attr(e, AU_ATOM_style);
    if (!a || !a->len)
        return JS_NewString(ctx, "");
    AuCssDeclBlock *b = au_css_parse_inline_style(a->value, a->len, NULL);
    AuBuf out;
    au_buf_init(&out);
    au_css_decl_block_serialize(b, &out);
    au_css_decl_block_free(b);
    while (out.len && out.data[out.len - 1] == ' ')
        out.len--;
    JSValue r = js_str(ctx, out.data, out.len);
    au_buf_free(&out);
    return r;
}

/* computed(el, prop): getComputedStyle's value of one property. */
static JSValue n_computed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 1 ? arg_el(js, argv[0]) : NULL;
    if (!e || !js->hooks.computed_style)
        return JS_NewString(ctx, "");
    char *prop = lower_prop(ctx, argv[1]);
    if (!prop)
        return JS_EXCEPTION;
    const AuStyle *st = js->hooks.computed_style(js->hooks.ctx, e);
    /* computed(el, prop, pseudo): 1 ::before, 2 ::after, 3 ::marker. An
     * element without that pseudo-element gives "" (the prelude answers
     * content: none). */
    int32_t pseudo = 0;
    if (argc > 2)
        JS_ToInt32(ctx, &pseudo, argv[2]);
    if (st && pseudo)
        st = pseudo > 3 ? NULL
                        : au_style_pseudo(st, pseudo == 1   ? AU_CSS_PSEUDO_BEFORE
                                              : pseudo == 2 ? AU_CSS_PSEUDO_AFTER
                                                            : AU_CSS_PSEUDO_MARKER);
    JSValue r = JS_NewString(ctx, "");
    if (st) {
        AuBuf out;
        au_buf_init(&out);
        if (au_style_serialize_property(st, au_atom_cstr(prop), &out)) {
            JS_FreeValue(ctx, r);
            r = js_str(ctx, out.data, out.len);
        }
        au_buf_free(&out);
    }
    au_free(prop);
    return r;
}

/* cssPropertyNames(): every longhand name (getComputedStyle iteration). */
static JSValue n_css_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    /* cssPropertyNames(true): the shorthands and logical properties instead */
    bool rest = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
    JSValue arr = JS_NewArray(ctx);
    uint32_t k = 0;
    for (int p = rest ? AU_PROP_LONGHAND_END : 1; p < (rest ? AU_PROP_COUNT : AU_PROP_LONGHAND_END); p++) {
        const char *name = au_css_property_name((AuCssProp)p);
        if (name && *name)
            JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, name));
    }
    return arr;
}

static JSValue n_css_supports(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t a, b;
    if (argc >= 2) {
        char *p = js_tostr(ctx, argv[0], &a), *v = js_tostr(ctx, argv[1], &b);
        bool r = p && v && au_css_supports_decl(p, a, v, b);
        au_free(p);
        au_free(v);
        return JS_NewBool(ctx, r);
    }
    char *c = argc ? js_tostr(ctx, argv[0], &a) : NULL;
    bool r = c && au_css_supports(c, a);
    au_free(c);
    return JS_NewBool(ctx, r);
}

/* ---- Geometry ---- */

static JSValue n_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 0 ? arg_el(js, argv[0]) : NULL;
    AuJsRect r;
    if (!e || !js->hooks.element_rect || !js->hooks.element_rect(js->hooks.ctx, e, &r))
        return JS_NULL;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, r.x));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, r.y));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, r.w));
    JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, r.h));
    return arr;
}

/* random(n): an ArrayBuffer of n random bytes (n at most 65536) from the
 * embedder's source; false when the source is not ready; null when the
 * embedder has none. */
static JSValue n_random(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    uint32_t n = 0;
    if (!js->hooks.random)
        return JS_NULL;
    if (argc < 1 || JS_ToUint32(ctx, &n, argv[0]) || n > 65536)
        return JS_ThrowRangeError(ctx, "random: at most 65536 bytes");
    uint8_t *buf = au_alloc(n ? n : 1);
    JSValue r = JS_FALSE;
    if (js->hooks.random(js->hooks.ctx, buf, n))
        r = JS_NewArrayBufferCopy(ctx, buf, n);
    au_free(buf);
    return r;
}

/* ---- CSSOM: the sheet of a style or link element (prelude_cssom.js) ----
 * sheetText(el): [text, url] once the sheet has loaded ([null, url] when
 * its text is not kept), false while it loads, null when el has no sheet
 * or the embedder has no sheets. */
static JSValue n_sheet_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 0 ? arg_el(js, argv[0]) : NULL;
    if (!e || !js->hooks.sheet_text)
        return JS_NULL;
    AuBuf text, url;
    au_buf_init(&text);
    au_buf_init(&url);
    int st = js->hooks.sheet_text(js->hooks.ctx, e, &text, &url);
    JSValue r = JS_NULL;
    if (st == 1) {
        r = JS_FALSE;
    } else if (st == 2 || st == 3) {
        r = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, r, 0, st == 3 ? JS_NULL : JS_NewStringLen(ctx, text.data ? text.data : "", text.len));
        JS_SetPropertyUint32(ctx, r, 1, JS_NewStringLen(ctx, url.data ? url.data : "", url.len));
    }
    au_buf_free(&text);
    au_buf_free(&url);
    return r;
}

static JSValue n_sheet_set_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 1 ? arg_el(js, argv[0]) : NULL;
    if (!e || !js->hooks.sheet_set_text)
        return JS_UNDEFINED;
    size_t n = 0;
    char *t = js_tostr(ctx, argv[1], &n);
    if (!t)
        return JS_EXCEPTION;
    js->hooks.sheet_set_text(js->hooks.ctx, e, t, n);
    au_free(t);
    return JS_UNDEFINED;
}

static JSValue n_sheet_disable(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuElement *e = argc > 1 ? arg_el(js, argv[0]) : NULL;
    if (e && js->hooks.sheet_disable)
        js->hooks.sheet_disable(js->hooks.ctx, e, JS_ToBool(ctx, argv[1]) > 0);
    return JS_UNDEFINED;
}

static JSValue n_viewport(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    int v[6] = {800, 600, 0, 0, 800, 600};
    if (js->hooks.viewport)
        js->hooks.viewport(js->hooks.ctx, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 6; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, v[i]));
    return arr;
}

static JSValue n_scroll_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    double x = 0, y = 0;
    if (argc > 1 && !JS_ToFloat64(ctx, &x, argv[0]) && !JS_ToFloat64(ctx, &y, argv[1]) && js->hooks.scroll_to) {
        if (!(x == x))
            x = 0;
        if (!(y == y))
            y = 0;
        js->hooks.scroll_to(js->hooks.ctx, (int)(x < -1e7 ? -1e7 : x > 1e7 ? 1e7 : x),
                            (int)(y < -1e7 ? -1e7 : y > 1e7 ? 1e7 : y));
    }
    return JS_UNDEFINED;
}

/* ---- URLs ---- */

static JSValue n_url_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 1)
        return JS_NULL;
    char *in = js_tostr(ctx, argv[0], NULL);
    char *base = NULL;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        base = js_tostr(ctx, argv[1], NULL);
    else if (argc > 1 && JS_IsNull(argv[1]))
        base = au_strndup(js->base, strlen(js->base));
    if (!in) {
        au_free(base);
        return JS_EXCEPTION;
    }
    JsUrl u;
    bool ok = js_url_parse(in, base, &u);
    au_free(in);
    au_free(base);
    if (!ok)
        return JS_NULL;
    const char *parts[9] = {u.href, u.protocol, u.username, u.password, u.hostname, u.port, u.pathname, u.search, u.hash};
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 9; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, parts[i] ? parts[i] : ""));
    js_url_free(&u);
    return arr;
}

static JSValue n_doc_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    int32_t which = 0;
    if (argc > 0)
        JS_ToInt32(ctx, &which, argv[0]);
    return JS_NewString(ctx, which == 0 ? js->url : which == 1 ? js->base : js->origin);
}

static JSValue n_set_doc_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    char *u = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    if (!u)
        return JS_EXCEPTION;
    au_js_set_url(js, u);
    au_free(u);
    return JS_UNDEFINED;
}

static JSValue n_same_origin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    char *u = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    bool r = u && js_same_origin(js, u);
    au_free(u);
    return JS_NewBool(ctx, r);
}

static JSValue n_origin_of(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    char *u = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    if (!u)
        return JS_EXCEPTION;
    char o[512];
    js_origin_of(u, o, sizeof o);
    au_free(u);
    return JS_NewString(ctx, o);
}

/* ---- Text encoding ---- */

/* utf8Encode(str): the string as UTF-8 bytes. The prelude makes the
 * string well formed first (lone surrogates become U+FFFD). */
static JSValue n_utf8_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t n;
    const char *s = argc > 0 ? JS_ToCStringLen(ctx, &n, argv[0]) : NULL;
    if (!s)
        return argc > 0 ? JS_EXCEPTION : JS_NewUint8ArrayCopy(ctx, (const uint8_t *)"", 0);
    JSValue r = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)s, n);
    JS_FreeCString(ctx, s);
    return r;
}

static const uint8_t *bytes_of(JSContext *ctx, JSValueConst v, size_t *len)
{
    size_t n;
    uint8_t *p = JS_GetUint8Array(ctx, &n, v);
    if (p) {
        *len = n;
        return p;
    }
    JSValue e = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
    p = JS_GetArrayBuffer(ctx, &n, v);
    if (p) {
        *len = n;
        return p;
    }
    e = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
    return NULL;
}

/* imageSize(bytes): [width, height] from an image file's header (any
 * format src/image decodes), or null. */
static JSValue n_image_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t n = 0;
    const uint8_t *p = argc > 0 ? bytes_of(ctx, argv[0], &n) : NULL;
    int w, h;
    if (!p || !js_image_size(p, n, &w, &h))
        return JS_NULL;
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewInt32(ctx, w));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewInt32(ctx, h));
    return r;
}

/* utf8Decode(bytes, fatal, ignoreBOM, stream?): malformed sequences become
 * U+FFFD, or a TypeError when fatal. */
static JSValue n_utf8_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t n = 0;
    const uint8_t *p = argc > 0 ? bytes_of(ctx, argv[0], &n) : NULL;
    if (!p)
        return JS_NewString(ctx, "");
    bool fatal = argc > 1 && JS_ToBool(ctx, argv[1]);
    bool ignore_bom = argc > 2 && JS_ToBool(ctx, argv[2]);
    size_t i = 0;
    if (!ignore_bom && n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        i = 3;
    AuBuf b;
    au_buf_init(&b);
    au_buf_reserve(&b, n - i + 1);
    while (i < n) {
        if (p[i] < 0x80) {
            au_buf_putc(&b, (char)p[i++]);
            continue;
        }
        size_t adv;
        uint32_t cp = au_utf8_decode((const char *)p + i, n - i, &adv);
        bool bad = cp == 0xFFFD && !(adv == 3 && p[i] == 0xEF && p[i + 1] == 0xBF && p[i + 2] == 0xBD);
        if (bad && fatal) {
            au_buf_free(&b);
            return JS_ThrowTypeError(ctx, "The encoded data was not valid UTF-8");
        }
        au_buf_put_utf8(&b, cp);
        i += adv;
    }
    JSValue r = js_str(ctx, b.data, b.len);
    au_buf_free(&b);
    return r;
}

/* ---- Embedder hooks ---- */

static JSValue n_alert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    char *msg = argc > 0 && !JS_IsUndefined(argv[0]) ? js_tostr(ctx, argv[0], NULL) : au_strndup("", 0);
    if (!msg)
        return JS_EXCEPTION;
    JSValue r = JS_UNDEFINED;
    /* Time spent in a dialog does not count against the script budget. */
    double t0 = js_now(js);
    if (magic == 0) {
        if (js->hooks.alert)
            js->hooks.alert(js->hooks.ctx, js->origin, msg);
    } else if (magic == 1) {
        r = JS_NewBool(ctx, js->hooks.confirm && js->hooks.confirm(js->hooks.ctx, js->origin, msg));
    } else {
        char *def = argc > 1 && !JS_IsUndefined(argv[1]) ? js_tostr(ctx, argv[1], NULL) : au_strndup("", 0);
        char *ans = js->hooks.prompt && def ? js->hooks.prompt(js->hooks.ctx, js->origin, msg, def) : NULL;
        r = ans ? JS_NewString(ctx, ans) : JS_NULL;
        au_free(ans);
        au_free(def);
    }
    double spent = js_now(js) - t0;
    js->root->run_start += spent;
    js->root->next_check += spent;
    au_free(msg);
    return r;
}

static JSValue n_navigate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    if (magic == 2) {
        if (js->hooks.reload)
            js->hooks.reload(js->hooks.ctx);
        return JS_UNDEFINED;
    }
    char *u = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    if (!u)
        return JS_EXCEPTION;
    if (js->hooks.navigate)
        js->hooks.navigate(js->hooks.ctx, u, magic == 1);
    au_free(u);
    return JS_UNDEFINED;
}

static JSValue n_open_window(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    char *u = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    char *t = argc > 1 ? js_tostr(ctx, argv[1], NULL) : au_strndup("_blank", 6);
    bool ok = u && t && js->hooks.open_window && js->hooks.open_window(js->hooks.ctx, u, t);
    au_free(u);
    au_free(t);
    return JS_NewBool(ctx, ok);
}

static JSValue n_history(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    switch (magic) {
    case 0:
        return JS_NewInt32(ctx, js->hooks.history_length ? js->hooks.history_length(js->hooks.ctx) : 1);
    case 1: {
        int32_t d = 0;
        if (argc > 0)
            JS_ToInt32(ctx, &d, argv[0]);
        if (js->hooks.history_go)
            js->hooks.history_go(js->hooks.ctx, d);
        return JS_UNDEFINED;
    }
    default: {
        char *u = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
        if (!u)
            return JS_EXCEPTION;
        au_js_set_url(js, u);
        if (js->hooks.history_push)
            js->hooks.history_push(js->hooks.ctx, u, magic == 3);
        au_free(u);
        return JS_UNDEFINED;
    }
    }
}

static JSValue n_cookie(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    if (!js->settings.cookies_enabled)
        return magic ? JS_UNDEFINED : JS_NewString(ctx, "");
    if (magic == 0) {
        char *c = js->hooks.get_cookie ? js->hooks.get_cookie(js->hooks.ctx, js->url) : NULL;
        JSValue r = JS_NewString(ctx, c ? c : "");
        au_free(c);
        return r;
    }
    char *s = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    if (!s)
        return JS_EXCEPTION;
    if (js->hooks.set_cookie)
        js->hooks.set_cookie(js->hooks.ctx, js->url, s);
    au_free(s);
    return JS_UNDEFINED;
}

static JSValue n_storage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    if (magic == 0) {
        size_t len = 0;
        char *s = js->hooks.storage_load ? js->hooks.storage_load(js->hooks.ctx, js->origin, &len) : NULL;
        JSValue r = s ? js_str(ctx, s, len) : JS_NULL;
        au_free(s);
        return r;
    }
    size_t len;
    char *s = argc > 0 ? js_tostr(ctx, argv[0], &len) : NULL;
    if (!s)
        return JS_EXCEPTION;
    if (js->hooks.storage_save)
        js->hooks.storage_save(js->hooks.ctx, js->origin, s, len);
    au_free(s);
    return JS_UNDEFINED;
}

static JSValue n_media_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    size_t n;
    char *q = argc > 0 ? js_tostr(ctx, argv[0], &n) : NULL;
    if (!q)
        return JS_EXCEPTION;
    bool r = js->hooks.media_matches ? js->hooks.media_matches(js->hooks.ctx, q, n) : false;
    au_free(q);
    return JS_NewBool(ctx, r);
}

/* setFocus(el or null, previous or null): moves the focus state bits. */
static JSValue n_set_focus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *prev = argc > 1 ? arg_el(js, argv[1]) : NULL;
    AuElement *el = argc > 0 ? arg_el(js, argv[0]) : NULL;
    if (prev) {
        au_element_set_state(prev, AU_STATE_FOCUS | AU_STATE_FOCUS_VISIBLE, false);
        if (js->hooks.dom_changed)
            js->hooks.dom_changed(js->hooks.ctx, &prev->node, 0, AU_JS_CHANGE_STATE);
    }
    if (el) {
        au_element_set_state(el, AU_STATE_FOCUS, true);
        if (js->hooks.dom_changed)
            js->hooks.dom_changed(js->hooks.ctx, &el->node, 0, AU_JS_CHANGE_STATE);
    }
    if (js->hooks.focus_changed)
        js->hooks.focus_changed(js->hooks.ctx, el);
    return JS_UNDEFINED;
}

/* setChecked(el, on): form control checkedness (the :checked state). */
static JSValue n_set_checked(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *el = argc > 1 ? arg_el(js, argv[0]) : NULL;
    if (!el)
        return JS_UNDEFINED;
    bool on = JS_ToBool(ctx, argv[1]);
    au_element_set_state(el, AU_STATE_CHECKED_SET, true);
    au_element_set_state(el, AU_STATE_CHECKED, on);
    if (js->hooks.dom_changed)
        js->hooks.dom_changed(js->hooks.ctx, &el->node, 0, AU_JS_CHANGE_STATE);
    return JS_UNDEFINED;
}

static JSValue n_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *el = argc > 0 ? arg_el(js, argv[0]) : NULL;
    return JS_NewUint32(ctx, el ? el->state : 0);
}

static JSValue n_settings(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    const AuJsSettings *s = &js->settings;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "userAgent",
                      JS_NewString(ctx, s->user_agent ? s->user_agent : "Mozilla/5.0 (Macintosh; PPC Mac OS 9) Aura/1"));
    JS_SetPropertyStr(ctx, o, "language", JS_NewString(ctx, s->language ? s->language : "en-US"));
    JS_SetPropertyStr(ctx, o, "dnt", JS_NewBool(ctx, s->do_not_track));
    JS_SetPropertyStr(ctx, o, "gpc", JS_NewBool(ctx, s->global_privacy_control));
    JS_SetPropertyStr(ctx, o, "online", JS_NewBool(ctx, s->online));
    JS_SetPropertyStr(ctx, o, "cookieEnabled", JS_NewBool(ctx, s->cookies_enabled));
    JS_SetPropertyStr(ctx, o, "quota", JS_NewUint32(ctx, s->storage_quota));
    JS_SetPropertyStr(ctx, o, "screenWidth", JS_NewInt32(ctx, s->screen_width));
    JS_SetPropertyStr(ctx, o, "screenHeight", JS_NewInt32(ctx, s->screen_height));
    return o;
}

/* compileHandler(source, url): a function for an event handler attribute,
 * with the element, its form and the document on its scope chain (the
 * prelude passes them as with-scopes). */
static JSValue n_compile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    size_t n;
    char *src = argc > 0 ? js_tostr(ctx, argv[0], &n) : NULL;
    char *name = argc > 1 ? js_tostr(ctx, argv[1], NULL) : NULL;
    if (!src || !name) {
        au_free(src);
        au_free(name);
        return JS_EXCEPTION;
    }
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "(function(__d,__f,__e){with(__d)with(__f)with(__e)return function(");
    au_buf_puts(&b, strcmp(name, "onerror") == 0 ? "event, source, lineno, colno, error" : "event");
    au_buf_puts(&b, "){\n");
    au_buf_append(&b, src, n);
    au_buf_puts(&b, "\n}})");
    au_buf_cstr(&b); /* QuickJS reads up to a NUL */
    JSValue r = JS_Eval(ctx, b.data, b.len, js->url, JS_EVAL_TYPE_GLOBAL);
    au_buf_free(&b);
    au_free(src);
    au_free(name);
    return r;
}

static const JSCFunctionListEntry natives2[] = {
    JS_CFUNC_DEF("styleSet", 4, n_style_set),
    JS_CFUNC_DEF("styleGet", 3, n_style_get),
    JS_CFUNC_DEF("styleNames", 1, n_style_names),
    JS_CFUNC_DEF("styleText", 1, n_style_text),
    JS_CFUNC_DEF("computed", 2, n_computed),
    JS_CFUNC_DEF("cssPropertyNames", 0, n_css_names),
    JS_CFUNC_DEF("cssSupports", 2, n_css_supports),
    JS_CFUNC_DEF("rect", 1, n_rect),
    JS_CFUNC_DEF("random", 1, n_random),
    JS_CFUNC_DEF("sheetText", 1, n_sheet_text),
    JS_CFUNC_DEF("sheetSetText", 2, n_sheet_set_text),
    JS_CFUNC_DEF("sheetDisable", 2, n_sheet_disable),
    JS_CFUNC_DEF("viewport", 0, n_viewport),
    JS_CFUNC_DEF("scrollTo", 2, n_scroll_to),
    JS_CFUNC_DEF("urlParse", 2, n_url_parse),
    JS_CFUNC_DEF("docUrl", 1, n_doc_url),
    JS_CFUNC_DEF("setDocUrl", 1, n_set_doc_url),
    JS_CFUNC_DEF("sameOrigin", 1, n_same_origin),
    JS_CFUNC_DEF("originOf", 1, n_origin_of),
    JS_CFUNC_DEF("utf8Encode", 1, n_utf8_encode),
    JS_CFUNC_DEF("utf8Decode", 3, n_utf8_decode),
    JS_CFUNC_DEF("imageSize", 1, n_image_size),
    JS_CFUNC_MAGIC_DEF("alert", 1, n_alert, 0),
    JS_CFUNC_MAGIC_DEF("confirm", 1, n_alert, 1),
    JS_CFUNC_MAGIC_DEF("prompt", 2, n_alert, 2),
    JS_CFUNC_MAGIC_DEF("navigate", 1, n_navigate, 0),
    JS_CFUNC_MAGIC_DEF("navigateReplace", 1, n_navigate, 1),
    JS_CFUNC_MAGIC_DEF("reload", 0, n_navigate, 2),
    JS_CFUNC_DEF("openWindow", 2, n_open_window),
    JS_CFUNC_MAGIC_DEF("historyLength", 0, n_history, 0),
    JS_CFUNC_MAGIC_DEF("historyGo", 1, n_history, 1),
    JS_CFUNC_MAGIC_DEF("historyPush", 1, n_history, 2),
    JS_CFUNC_MAGIC_DEF("historyReplace", 1, n_history, 3),
    JS_CFUNC_MAGIC_DEF("getCookie", 0, n_cookie, 0),
    JS_CFUNC_MAGIC_DEF("setCookie", 1, n_cookie, 1),
    JS_CFUNC_MAGIC_DEF("storageLoad", 0, n_storage, 0),
    JS_CFUNC_MAGIC_DEF("storageSave", 1, n_storage, 1),
    JS_CFUNC_DEF("mediaMatches", 1, n_media_matches),
    JS_CFUNC_DEF("setFocus", 2, n_set_focus),
    JS_CFUNC_DEF("setChecked", 2, n_set_checked),
    JS_CFUNC_DEF("state", 1, n_state),
    JS_CFUNC_DEF("settings", 0, n_settings),
    JS_CFUNC_DEF("compileHandler", 2, n_compile),
};

void js_natives2_init(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, natives2, JS_COUNTOF(natives2));
}
