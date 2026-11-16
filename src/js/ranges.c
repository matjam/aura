/* Live ranges: the boundary points of every script Range, kept here so
 * that the DOM mutation hooks can move them as the DOM standard says
 * (insert, remove) without running script inside a mutation. The Range
 * class itself is in range.js (read on first use); it reads and writes its boundary
 * points through N.rangeGet and N.rangeSet, and reports character data
 * changes with N.rangeText and N.rangeSplit, because offsets in text are
 * UTF-16 code units, which only script knows.
 *
 * A range references its two boundary nodes, so they outlive removal from
 * the tree, as the standard requires. A slot is freed by N.rangeFree (the
 * prelude's FinalizationRegistry) or at au_js_free. */
#include "js_int.h"

#include <string.h>

struct JsRange {
    AuNode *sc, *ec; /* referenced; NULL: a free slot */
    uint32_t so, eo;
};

static void settle(AuJs *js);

static uint32_t child_index(const AuNode *n)
{
    uint32_t i = 0;
    for (const AuNode *p = n->prev; p; p = p->prev)
        i++;
    return i;
}

static void set_point(AuNode **slot, uint32_t *off, AuNode *n, uint32_t o)
{
    if (*slot != n) {
        au_node_ref(n);
        if (*slot)
            au_node_unref(*slot);
        *slot = n;
    }
    *off = o;
}

/* True when n is top or a descendant of top (top is detached: the walk
 * ends at it or at another root). */
static bool inside(const AuNode *n, const AuNode *top)
{
    for (; n; n = n->parent)
        if (n == top)
            return true;
    return false;
}

void js_ranges_inserted(AuJs *js, AuNode *parent, AuNode *child)
{
    if (!js->live_ranges)
        return;
    settle(js);
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < js->nranges; i++) {
        JsRange *r = &js->ranges[i];
        if (!r->sc || (r->sc != parent && r->ec != parent))
            continue;
        if (index == UINT32_MAX)
            index = child_index(child);
        if (r->sc == parent && r->so > index)
            r->so++;
        if (r->ec == parent && r->eo > index)
            r->eo++;
    }
}

void js_ranges_removed(AuJs *js, AuNode *parent, AuNode *child, AuNode *prev)
{
    if (!js->live_ranges)
        return;
    settle(js);
    uint32_t index = prev ? child_index(prev) + 1 : 0;
    for (uint32_t i = 0; i < js->nranges; i++) {
        JsRange *r = &js->ranges[i];
        if (!r->sc)
            continue;
        if (inside(r->sc, child))
            set_point(&r->sc, &r->so, parent, index);
        else if (r->sc == parent && r->so > index)
            r->so--;
        if (inside(r->ec, child))
            set_point(&r->ec, &r->eo, parent, index);
        else if (r->ec == parent && r->eo > index)
            r->eo--;
    }
}

/* Finishes the text change js_ranges_text recorded: the hook runs before
 * the data changes, so the old and new data are compared here, before
 * anything else reads or moves a range. */
static void settle(AuJs *js)
{
    AuText *t = (AuText *)js->range_pend_node;
    if (!t)
        return;
    size_t old_len = js->range_pend_len;
    bool appended = t->len >= old_len && (!old_len || !memcmp(t->data, js->range_pend_old, old_len));
    if (!appended)
        for (uint32_t i = 0; i < js->nranges; i++) {
            JsRange *r = &js->ranges[i];
            if (!r->sc)
                continue;
            if (r->sc == &t->node)
                r->so = 0;
            if (r->ec == &t->node)
                r->eo = 0;
        }
    js->range_pend_node = NULL;
    au_free(js->range_pend_old);
    js->range_pend_old = NULL;
    au_node_unref(&t->node);
}

/* A text node's data is about to change outside the prelude's replace
 * steps (the data, nodeValue and textContent setters, the parser). Data
 * only appended moves nothing; anything else is the standard's "replace
 * data" of the whole string, which puts every boundary point in the node
 * at offset 0. */
void js_ranges_text(AuJs *js, AuText *t, const char *old, size_t old_len)
{
    if (!js->live_ranges)
        return;
    settle(js);
    if (js->range_text_node == &t->node)
        return; /* the prelude adjusted the ranges (N.rangeText) */
    bool used = false;
    for (uint32_t i = 0; i < js->nranges && !used; i++)
        used = js->ranges[i].sc && (js->ranges[i].sc == &t->node || js->ranges[i].ec == &t->node);
    if (!used)
        return;
    js->range_pend_node = &t->node;
    au_node_ref(&t->node);
    js->range_pend_old = au_strndup(old ? old : "", old_len);
    js->range_pend_len = old_len;
}

void js_ranges_free(AuJs *js)
{
    if (js->range_pend_node) {
        au_node_unref(js->range_pend_node);
        js->range_pend_node = NULL;
        au_free(js->range_pend_old);
        js->range_pend_old = NULL;
    }
    for (uint32_t i = 0; i < js->nranges; i++)
        if (js->ranges[i].sc) {
            au_node_unref(js->ranges[i].sc);
            au_node_unref(js->ranges[i].ec);
        }
    au_free(js->ranges);
    js->ranges = NULL;
    js->nranges = js->live_ranges = 0;
}

static JsRange *range_arg(AuJs *js, JSContext *ctx, JSValueConst v)
{
    settle(js);
    uint32_t id = 0;
    if (JS_ToUint32(ctx, &id, v) || id >= js->nranges || !js->ranges[id].sc)
        return NULL;
    return &js->ranges[id];
}

/* rangeNew(): a range collapsed at (document, 0); returns its id. */
static JSValue n_range_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val, (void)argc, (void)argv;
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    while (id < js->nranges && js->ranges[id].sc)
        id++;
    if (id == js->nranges) {
        uint32_t cap = js->nranges ? js->nranges * 2 : 8;
        js->ranges = au_realloc(js->ranges, cap * sizeof *js->ranges);
        memset(js->ranges + js->nranges, 0, (cap - js->nranges) * sizeof *js->ranges);
        js->nranges = cap;
    }
    JsRange *r = &js->ranges[id];
    r->sc = r->ec = &js->doc->node;
    au_node_ref(r->sc);
    au_node_ref(r->ec);
    r->so = r->eo = 0;
    js->live_ranges++;
    return JS_NewUint32(ctx, id);
}

static JSValue n_range_free(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    JsRange *r = argc > 0 ? range_arg(js, ctx, argv[0]) : NULL;
    if (r) {
        au_node_unref(r->sc);
        au_node_unref(r->ec);
        r->sc = r->ec = NULL;
        js->live_ranges--;
    }
    return JS_UNDEFINED;
}

/* rangeGet(id): [startContainer, startOffset, endContainer, endOffset]. */
static JSValue n_range_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    JsRange *r = argc > 0 ? range_arg(js, ctx, argv[0]) : NULL;
    if (!r)
        return JS_NULL;
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, js_wrap(js, r->sc));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewUint32(ctx, r->so));
    JS_SetPropertyUint32(ctx, a, 2, js_wrap(js, r->ec));
    JS_SetPropertyUint32(ctx, a, 3, JS_NewUint32(ctx, r->eo));
    return a;
}

/* rangeSet(id, startContainer, startOffset, endContainer, endOffset). */
static JSValue n_range_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    JsRange *r = argc > 4 ? range_arg(js, ctx, argv[0]) : NULL;
    AuNode *sc = r ? js_unwrap(js, argv[1]) : NULL, *ec = r ? js_unwrap(js, argv[3]) : NULL;
    uint32_t so = 0, eo = 0;
    if (!sc || !ec || JS_ToUint32(ctx, &so, argv[2]) || JS_ToUint32(ctx, &eo, argv[4]))
        return JS_ThrowTypeError(ctx, "rangeSet: bad arguments");
    set_point(&r->sc, &r->so, sc, so);
    set_point(&r->ec, &r->eo, ec, eo);
    return JS_UNDEFINED;
}

/* rangeText(node, offset, count, length): node's data is about to have
 * count units at offset replaced by length units (the standard's "replace
 * data"); the prelude sets the data next and then calls rangeText() with
 * no arguments. Until then the text hook leaves the ranges alone. */
static JSValue n_range_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 3 ? js_unwrap(js, argv[0]) : NULL;
    uint32_t off = 0, count = 0, len = 0;
    settle(js);
    js->range_text_node = NULL;
    if (!n || !js->live_ranges || JS_ToUint32(ctx, &off, argv[1]) || JS_ToUint32(ctx, &count, argv[2]) ||
        JS_ToUint32(ctx, &len, argv[3]))
        return JS_UNDEFINED;
    js->range_text_node = n; /* compared only, never dereferenced */
    for (uint32_t i = 0; i < js->nranges; i++) {
        JsRange *r = &js->ranges[i];
        if (!r->sc)
            continue;
        if (r->sc == n && r->so > off)
            r->so = r->so <= off + count ? off : r->so + len - count;
        if (r->ec == n && r->eo > off)
            r->eo = r->eo <= off + count ? off : r->eo + len - count;
    }
    return JS_UNDEFINED;
}

/* rangeSplit(node, offset, newNode): the range steps of splitText, called
 * after newNode was inserted after node (the insert hook has run) and
 * before node's data is cut: boundary points in node past offset move
 * into newNode, and points at (parent, index of node + 1) move past
 * newNode. */
static JSValue n_range_split(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 2 ? js_unwrap(js, argv[0]) : NULL, *nn = argc > 2 ? js_unwrap(js, argv[2]) : NULL;
    uint32_t off = 0;
    settle(js);
    if (!n || !nn || !js->live_ranges || JS_ToUint32(ctx, &off, argv[1]))
        return JS_UNDEFINED;
    AuNode *parent = n->parent;
    uint32_t index = parent ? child_index(n) : 0;
    for (uint32_t i = 0; i < js->nranges; i++) {
        JsRange *r = &js->ranges[i];
        if (!r->sc)
            continue;
        if (r->sc == n && r->so > off)
            set_point(&r->sc, &r->so, nn, r->so - off);
        else if (parent && r->sc == parent && r->so == index + 1)
            r->so++;
        if (r->ec == n && r->eo > off)
            set_point(&r->ec, &r->eo, nn, r->eo - off);
        else if (parent && r->ec == parent && r->eo == index + 1)
            r->eo++;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry range_natives[] = {
    JS_CFUNC_DEF("rangeNew", 0, n_range_new),   JS_CFUNC_DEF("rangeFree", 1, n_range_free),
    JS_CFUNC_DEF("rangeGet", 1, n_range_get),   JS_CFUNC_DEF("rangeSet", 5, n_range_set),
    JS_CFUNC_DEF("rangeText", 4, n_range_text), JS_CFUNC_DEF("rangeSplit", 3, n_range_split),
};

void js_natives_ranges_init(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, range_natives, JS_COUNTOF(range_natives));
}
