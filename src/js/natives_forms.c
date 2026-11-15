/* Natives for form controls: the value, selection, custom validity and
 * files of a control live in its AuControl (src/dom/control.h), shared
 * with the document view, which edits them from user input. Selection
 * offsets cross here between UTF-16 code units (script) and UTF-8 bytes
 * (the control). */
#include "js_int.h"
#include "../dom/control.h"

#include <string.h>

static AuElement *el_arg(AuJs *js, int argc, JSValueConst *argv, int i)
{
    if (argc <= i)
        return NULL;
    AuNode *n = js_unwrap(js, argv[i]);
    return n && n->type == AU_NODE_ELEMENT ? (AuElement *)n : NULL;
}

static void value_changed(AuJs *js, AuElement *el)
{
    if (js->hooks.dom_changed)
        js->hooks.dom_changed(js->hooks.ctx, &el->node, 0, AU_JS_CHANGE_VALUE);
}

/* UTF-16 length of the UTF-8 text s[0..n). */
static uint32_t u16_len(const char *s, size_t n)
{
    uint32_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80)
            continue;
        k += c >= 0xF0 ? 2 : 1;
    }
    return k;
}

/* The byte offset of UTF-16 offset u (clamped). */
static uint32_t u16_to_byte(const char *s, size_t n, uint32_t u)
{
    uint32_t k = 0;
    size_t i = 0;
    while (i < n && k < u) {
        unsigned char c = (unsigned char)s[i];
        k += c >= 0xF0 ? 2 : 1;
        i++;
        while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80)
            i++;
    }
    return (uint32_t)i;
}

/* ctlValue(el): the current value. */
static JSValue n_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuElement *el = el_arg(js_of_ctx(ctx), argc, argv, 0);
    if (!el)
        return JS_NewString(ctx, "");
    size_t n;
    const char *v = au_ctl_value(el, &n);
    return js_str(ctx, v, n);
}

/* ctlSetValue(el, v): as the value setter (sanitized, dirty). */
static JSValue n_set_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *el = el_arg(js, argc, argv, 0);
    if (!el || argc < 2)
        return JS_UNDEFINED;
    size_t n;
    char *v = js_tostr(ctx, argv[1], &n);
    if (!v)
        return JS_EXCEPTION;
    au_ctl_set_value(el, v, n);
    au_free(v);
    value_changed(js, el);
    return JS_UNDEFINED;
}

/* ctlDirty(el): the dirty value flag. */
static JSValue n_dirty(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuElement *el = el_arg(js_of_ctx(ctx), argc, argv, 0);
    AuControl *c = el ? au_ctl_peek(el) : NULL;
    return JS_NewBool(ctx, c && c->dirty);
}

/* ctlSel(el): [start, end, direction] in UTF-16 units; direction 0 none
 * or forward, 1 backward. */
static JSValue n_sel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuElement *el = el_arg(js_of_ctx(ctx), argc, argv, 0);
    if (!el)
        return JS_NULL;
    size_t n;
    const char *v = au_ctl_value(el, &n);
    AuControl *c = au_ctl(el);
    uint32_t a = c->anchor < c->caret ? c->anchor : c->caret, b = c->anchor < c->caret ? c->caret : c->anchor;
    if (a > n)
        a = (uint32_t)n;
    if (b > n)
        b = (uint32_t)n;
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewUint32(ctx, u16_len(v, a)));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewUint32(ctx, u16_len(v, b)));
    JS_SetPropertyUint32(ctx, r, 2, JS_NewInt32(ctx, c->caret < c->anchor ? 1 : 0));
    return r;
}

/* ctlSetSel(el, start, end, backward). */
static JSValue n_set_sel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *el = el_arg(js, argc, argv, 0);
    if (!el || argc < 3)
        return JS_UNDEFINED;
    uint32_t s, e;
    if (JS_ToUint32(ctx, &s, argv[1]) || JS_ToUint32(ctx, &e, argv[2]))
        return JS_EXCEPTION;
    bool back = argc > 3 && JS_ToBool(ctx, argv[3]);
    size_t n;
    const char *v = au_ctl_value(el, &n);
    if (e < s)
        s = e;
    uint32_t bs = u16_to_byte(v, n, s), be = u16_to_byte(v, n, e);
    AuControl *c = au_ctl(el);
    c->anchor = back ? be : bs;
    c->caret = back ? bs : be;
    value_changed(js, el);
    return JS_UNDEFINED;
}

/* ctlReset(el): back to the default value and checkedness. */
static JSValue n_reset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *el = el_arg(js, argc, argv, 0);
    if (!el)
        return JS_UNDEFINED;
    au_ctl_reset(el);
    if (js->hooks.dom_changed)
        js->hooks.dom_changed(js->hooks.ctx, &el->node, 0, AU_JS_CHANGE_STATE);
    return JS_UNDEFINED;
}

/* ctlCustom(el[, message]): gets or sets the custom validity message. */
static JSValue n_custom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuElement *el = el_arg(js_of_ctx(ctx), argc, argv, 0);
    if (!el)
        return JS_NewString(ctx, "");
    AuControl *c = au_ctl(el);
    if (argc > 1) {
        size_t n;
        char *m = js_tostr(ctx, argv[1], &n);
        if (!m)
            return JS_EXCEPTION;
        au_free(c->custom);
        c->custom = n ? m : NULL;
        if (!n)
            au_free(m);
        return JS_UNDEFINED;
    }
    return JS_NewString(ctx, c->custom ? c->custom : "");
}

/* ctlFiles(el): [[name, type, Uint8Array], ...] of an input type=file. */
static JSValue n_files(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuElement *el = el_arg(js_of_ctx(ctx), argc, argv, 0);
    JSValue r = JS_NewArray(ctx);
    AuControl *c = el ? au_ctl_peek(el) : NULL;
    for (uint32_t i = 0; c && i < c->nfiles; i++) {
        JSValue f = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, f, 0, JS_NewString(ctx, c->files[i].name ? c->files[i].name : ""));
        JS_SetPropertyUint32(ctx, f, 1, JS_NewString(ctx, c->files[i].type ? c->files[i].type : ""));
        JS_SetPropertyUint32(ctx, f, 2, JS_NewUint8ArrayCopy(ctx, c->files[i].data, c->files[i].len));
        JS_SetPropertyUint32(ctx, r, i, f);
    }
    return r;
}

/* ctlClearFiles(el). */
static JSValue n_clear_files(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *el = el_arg(js, argc, argv, 0);
    if (el) {
        au_ctl_set_files(el, NULL, 0);
        if (js->hooks.dom_changed)
            js->hooks.dom_changed(js->hooks.ctx, &el->node, 0, AU_JS_CHANGE_STATE);
    }
    return JS_UNDEFINED;
}

/* ctlInteracted(el): the user has edited the control. */
static JSValue n_interacted(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuElement *el = el_arg(js_of_ctx(ctx), argc, argv, 0);
    AuControl *c = el ? au_ctl_peek(el) : NULL;
    return JS_NewBool(ctx, c && c->user_interacted);
}

/* submitForm(form, submitter): the embedder builds the entry list and
 * navigates. */
static JSValue n_submit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuElement *form = el_arg(js, argc, argv, 0);
    AuElement *sub = el_arg(js, argc, argv, 1);
    if (form && js->hooks.submit_form)
        js->hooks.submit_form(js->hooks.ctx, form, sub);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry natives_forms[] = {
    JS_CFUNC_DEF("ctlValue", 1, n_value),
    JS_CFUNC_DEF("ctlSetValue", 2, n_set_value),
    JS_CFUNC_DEF("ctlDirty", 1, n_dirty),
    JS_CFUNC_DEF("ctlSel", 1, n_sel),
    JS_CFUNC_DEF("ctlSetSel", 4, n_set_sel),
    JS_CFUNC_DEF("ctlReset", 1, n_reset),
    JS_CFUNC_DEF("ctlCustom", 2, n_custom),
    JS_CFUNC_DEF("ctlFiles", 1, n_files),
    JS_CFUNC_DEF("ctlClearFiles", 1, n_clear_files),
    JS_CFUNC_DEF("ctlInteracted", 1, n_interacted),
    JS_CFUNC_DEF("submitForm", 2, n_submit),
};

void js_natives_forms_init(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, natives_forms, JS_COUNTOF(natives_forms));
}
