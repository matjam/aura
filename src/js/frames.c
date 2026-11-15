/* Frames: the windows of nested documents.
 *
 * The documents of one top-level page share one QuickJS runtime, each
 * with its own context (au_js_new_child), so a same-origin document's
 * objects are reached directly. The embedder knows the frame tree; these
 * natives ask it through the frame hooks and hand windows to the prelude
 * as global objects. The prelude never gives a cross-origin global to
 * page script: wrapWindow (prelude_window.js) returns the global itself
 * only when N.windowAccess says the two documents have the same origin,
 * and otherwise an object with the few members the standard allows,
 * which act through the natives below.
 *
 * A global whose document has gone (the frame was removed or navigated)
 * names no AuJs any more: js_of_global returns NULL and every native
 * treats it as a closed window. */
#include "js_int.h"

#include <string.h>

AuJs *js_of_global(AuJs *js, JSValueConst global)
{
    if (!JS_IsObject(global))
        return NULL;
    for (AuJs *g = js->root; g; g = g->group_next)
        if (!g->closing && JS_VALUE_GET_PTR(g->global) == JS_VALUE_GET_PTR(global))
            return g;
    return NULL;
}

bool js_is_frame_element(const AuNode *n)
{
    if (n->type != AU_NODE_ELEMENT || ((const AuElement *)n)->ns != AU_NS_HTML)
        return false;
    AuAtom t = ((const AuElement *)n)->tag;
    return t == AU_ATOM_iframe || t == AU_ATOM_frame || t == AU_ATOM_object || t == AU_ATOM_embed;
}

static bool same_origin(const AuJs *a, const AuJs *b)
{
    if (a == b)
        return true;
    return !a->opaque_origin && !b->opaque_origin && !strcmp(a->origin, b->origin);
}

static JSValue global_of(JSContext *ctx, AuJs *t)
{
    return t && !t->closing ? JS_DupValue(ctx, t->global) : JS_NULL;
}

static AuJs *parent_of(AuJs *js)
{
    AuJs *p = js->hooks.parent_js ? js->hooks.parent_js(js->hooks.ctx) : NULL;
    return p && p->root == js->root && !p->closing ? p : NULL;
}

static AuJs *top_of(AuJs *js)
{
    for (int i = 0; i < 64; i++) {
        AuJs *p = parent_of(js);
        if (!p)
            break;
        js = p;
    }
    return js;
}

static AuJs *frame_at(AuJs *js, int i)
{
    if (!js->hooks.frame_at || !js->hooks.frame_js || i < 0)
        return NULL;
    AuElement *el = js->hooks.frame_at(js->hooks.ctx, i);
    AuJs *c = el ? js->hooks.frame_js(js->hooks.ctx, el) : NULL;
    return c && c->root == js->root && !c->closing ? c : NULL;
}

/* frameCount(): the number of nested documents. */
static JSValue n_frame_count(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    return JS_NewInt32(ctx, js->hooks.frame_count ? js->hooks.frame_count(js->hooks.ctx) : 0);
}

/* frameNames(): the name attribute of each frame's element ("" for none). */
static JSValue n_frame_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    int n = js->hooks.frame_count && js->hooks.frame_at ? js->hooks.frame_count(js->hooks.ctx) : 0;
    for (int i = 0; i < n; i++) {
        AuElement *el = js->hooks.frame_at(js->hooks.ctx, i);
        const char *name = el ? au_element_get_attr(el, au_atom("name", 4)) : NULL;
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, name ? name : ""));
    }
    return arr;
}

/* frameWindow(element or index): the global of the nested document, or
 * null. The caller wraps it (wrapWindow). */
static JSValue n_frame_window(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 1)
        return JS_NULL;
    if (JS_IsNumber(argv[0])) {
        int32_t i = -1;
        JS_ToInt32(ctx, &i, argv[0]);
        return global_of(ctx, frame_at(js, i));
    }
    AuNode *n = js_unwrap(js, argv[0]);
    if (!n || !js_is_frame_element(n) || !js->hooks.frame_js)
        return JS_NULL;
    /* The element's own document decides: an element moved to another
     * document belongs to that document's frame list. */
    AuJs *owner = au_js_of(n->doc);
    if (!owner || owner->root != js->root || owner->closing || !owner->hooks.frame_js)
        return JS_NULL;
    AuJs *c = owner->hooks.frame_js(owner->hooks.ctx, (AuElement *)n);
    return global_of(ctx, c && c->root == js->root ? c : NULL);
}

/* windowRel(which): 1 the parent's global, 2 the top's; this window's own
 * at the top. */
static JSValue n_window_rel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    int32_t which = 1;
    if (argc > 0)
        JS_ToInt32(ctx, &which, argv[0]);
    AuJs *t = which == 2 ? top_of(js) : parent_of(js);
    return global_of(ctx, t ? t : js);
}

/* frameElement(): the element holding this document, or null (the top, or
 * a parent of another origin). */
static JSValue n_frame_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *p = parent_of(js);
    if (!p || !same_origin(js, p) || !js->hooks.frame_element)
        return JS_NULL;
    AuElement *el = js->hooks.frame_element(js->hooks.ctx);
    return el ? js_wrap(p, &el->node) : JS_NULL;
}

/* windowAccess(global): true when this document may reach into that
 * window (same origin). */
static JSValue n_window_access(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *t = argc > 0 ? js_of_global(js, argv[0]) : NULL;
    return JS_NewBool(ctx, t && same_origin(js, t));
}

/* windowInfo(global, what): what another window shows to any origin.
 * 0 the number of frames, 1 its parent's global, 2 its top's, 3 whether
 * it is closed, 4 its origin. */
static JSValue n_window_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *t = argc > 0 ? js_of_global(js, argv[0]) : NULL;
    int32_t what = 0;
    if (argc > 1)
        JS_ToInt32(ctx, &what, argv[1]);
    switch (what) {
    case 0:
        return JS_NewInt32(ctx, t && t->hooks.frame_count ? t->hooks.frame_count(t->hooks.ctx) : 0);
    case 1: {
        AuJs *p = t ? parent_of(t) : NULL;
        return global_of(ctx, p ? p : t);
    }
    case 2:
        return global_of(ctx, t ? top_of(t) : NULL);
    case 3:
        return JS_NewBool(ctx, !t);
    default:
        return JS_NewString(ctx, t ? t->origin : "null");
    }
}

/* windowFrameAt(global, i): the global of that window's i-th frame. */
static JSValue n_window_frame_at(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *t = argc > 0 ? js_of_global(js, argv[0]) : NULL;
    int32_t i = -1;
    if (argc > 1)
        JS_ToInt32(ctx, &i, argv[1]);
    return global_of(ctx, t ? frame_at(t, i) : NULL);
}

/* enteredWindow(): the global of the document whose script the embedder
 * entered (the source of a message posted by a same-origin caller). */
static JSValue n_entered_window(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *r = js->root;
    int d = r->depth;
    if (d > (int)JS_COUNTOF(r->entered))
        d = (int)JS_COUNTOF(r->entered);
    AuJs *e = d > 0 ? r->entered[d - 1] : NULL;
    return global_of(ctx, e && !e->closing ? e : js);
}

/* postMessageTo(global, data, targetOrigin): queues a message event in
 * that window when its origin matches targetOrigin ("*" for any). The
 * receiving document clones data itself, in its own realm
 * (P.receiveMessage): it never gets an object of the sender's realm. */
static JSValue n_post_message_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *t = argc > 0 ? js_of_global(js, argv[0]) : NULL;
    if (!t || argc < 3 || JS_IsUndefined(t->P))
        return JS_UNDEFINED;
    const char *want = JS_ToCString(ctx, argv[2]);
    if (!want)
        return JS_EXCEPTION;
    bool ok = !strcmp(want, "*") || (!t->opaque_origin && !strcmp(want, t->origin));
    JS_FreeCString(ctx, want);
    if (!ok)
        return JS_UNDEFINED;
    JSValue args[4] = {JS_DupValue(ctx, argv[1]), JS_DupValue(ctx, js->global), JS_NewString(ctx, js->origin),
                       argc > 3 ? JS_DupValue(ctx, argv[3]) : JS_UNDEFINED};
    JSValue fn = JS_GetPropertyStr(t->ctx, t->P, "receiveMessage");
    JSValue r = JS_UNDEFINED;
    if (JS_IsFunction(t->ctx, fn))
        r = JS_Call(t->ctx, fn, JS_UNDEFINED, 4, (JSValueConst *)args);
    JS_FreeValue(ctx, fn);
    for (int i = 0; i < 4; i++)
        JS_FreeValue(ctx, args[i]);
    return r; /* a DataCloneError reaches the sender */
}

/* navigateWindow(global, url): sets another window's location. Any
 * origin may do this, except a sandboxed document without
 * allow-top-navigation, which may navigate only itself and the documents
 * nested in it. Returns false when refused. */
static JSValue n_navigate_window(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *t = argc > 0 ? js_of_global(js, argv[0]) : NULL;
    if (!t || argc < 2)
        return JS_FALSE;
    if (js->settings.no_top_navigation) {
        bool inside = false;
        AuJs *a = t;
        for (int i = 0; i < 64 && a; i++) {
            if (a == js) {
                inside = true;
                break;
            }
            a = parent_of(a);
        }
        if (!inside)
            return JS_FALSE;
    }
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!url)
        return JS_EXCEPTION;
    if (t->hooks.navigate && strncmp(url, "javascript:", 11) != 0)
        t->hooks.navigate(t->hooks.ctx, url, false);
    JS_FreeCString(ctx, url);
    return JS_TRUE;
}

static const JSCFunctionListEntry frame_natives[] = {
    JS_CFUNC_DEF("frameCount", 0, n_frame_count),
    JS_CFUNC_DEF("frameNames", 0, n_frame_names),
    JS_CFUNC_DEF("frameWindow", 1, n_frame_window),
    JS_CFUNC_DEF("windowRel", 1, n_window_rel),
    JS_CFUNC_DEF("frameElement", 0, n_frame_element),
    JS_CFUNC_DEF("windowAccess", 1, n_window_access),
    JS_CFUNC_DEF("windowInfo", 2, n_window_info),
    JS_CFUNC_DEF("windowFrameAt", 2, n_window_frame_at),
    JS_CFUNC_DEF("enteredWindow", 0, n_entered_window),
    JS_CFUNC_DEF("postMessageTo", 4, n_post_message_to),
    JS_CFUNC_DEF("navigateWindow", 2, n_navigate_window),
};

void js_frames_natives(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, frame_natives, JS_COUNTOF(frame_natives));
}

static void window_frames(AuJs *js)
{
    if (js->closing || JS_IsUndefined(js->P))
        return;
    JSValue fn = JS_GetPropertyStr(js->ctx, js->P, "framesChanged");
    if (JS_IsFunction(js->ctx, fn)) {
        JSValue r = JS_Call(js->ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r))
            JS_FreeValue(js->ctx, JS_GetException(js->ctx));
        JS_FreeValue(js->ctx, r);
    }
    JS_FreeValue(js->ctx, fn);
}

void js_frames_flush(AuJs *js)
{
    AuJs *root = js->root;
    if (!root->any_frames_dirty)
        return;
    root->any_frames_dirty = false;
    for (AuJs *g = root; g; g = g->group_next) {
        if (!g->frames_dirty || g->closing)
            continue;
        g->frames_dirty = false;
        if (g->hooks.frames_changed)
            g->hooks.frames_changed(g->hooks.ctx);
        window_frames(g);
    }
}

void au_js_frames_changed(AuJs *js)
{
    js->frames_dirty = false;
    window_frames(js);
}
