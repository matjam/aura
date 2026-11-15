/* DOM mutation hooks: style and layout invalidation through the embedder,
 * MutationObserver records, custom element reactions, slot changes and
 * wrapper holds. The hooks only record (src/dom forbids more inside a
 * mutation); records become script objects at the next checkpoint or
 * custom element reaction flush. */
#include "js_int.h"

#include <string.h>

static void changed(AuJs *js, AuNode *n, AuAtom attr, int kind)
{
    if (js->hooks.dom_changed && !js->closing)
        js->hooks.dom_changed(js->hooks.ctx, n, attr, kind);
}

static JSValue delivery_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    AuJs *js = js_of_ctx(ctx);
    js->delivery_queued = false;
    if (js->nmuts || js->slots_dirty)
        js_deliver_mutations(js, true);
    return JS_UNDEFINED;
}

/* Mutation observer delivery is a microtask queued by the first record
 * (in order with promise jobs). Queuing a job runs no script, so it is
 * allowed inside a DOM hook. */
static void queue_delivery(AuJs *js)
{
    if (js->delivery_queued || js->closing)
        return;
    if (JS_EnqueueJob(js->ctx, delivery_job, 0, NULL) == 0)
        js->delivery_queued = true;
}

static void push_mut(AuJs *js, uint8_t kind, AuNode *target, AuNode *added, AuNode *removed, AuNode *prev,
                     AuNode *next, AuAtom attr, const char *old, size_t old_len)
{
    queue_delivery(js);
    if (js->nmuts >= 100000)
        return; /* a runaway script: drop records rather than memory */
    if (js->nmuts == js->muts_cap) {
        js->muts_cap = js->muts_cap ? js->muts_cap * 2 : 32;
        js->muts = au_realloc(js->muts, js->muts_cap * sizeof *js->muts);
    }
    JsMutation *m = &js->muts[js->nmuts++];
    m->kind = kind;
    m->target = au_node_ref(target);
    m->added = au_node_ref(added);
    m->removed = au_node_ref(removed);
    m->prev = au_node_ref(prev);
    m->next = au_node_ref(next);
    m->attr = attr;
    m->old = old ? au_strndup(old, old_len) : NULL;
    m->old_len = old_len;
    m->batch = js->mut_batch;
    m->nobs = 0;
    for (AuNode *n = target; n && m->nobs < JS_MUT_MAX_OBSERVED; n = n->parent)
        if (n->flags & JS_NODEF_OBSERVED)
            m->obs[m->nobs++] = au_node_ref(n);
}

static bool is_custom_candidate(const AuNode *n)
{
    if (n->type != AU_NODE_ELEMENT)
        return false;
    const AuElement *el = (const AuElement *)n;
    if (el->ns != AU_NS_HTML)
        return false;
    if (memchr(au_atom_str(el->tag), '-', au_atom_len(el->tag)))
        return true;
    return au_element_get_attr(el, au_atom("is", 2)) != NULL;
}

static void push_react(AuJs *js, uint8_t kind, AuElement *el, AuAtom attr, const char *old, size_t old_len, bool had)
{
    if (js->nreacts >= 100000)
        return;
    if (js->nreacts == js->reacts_cap) {
        js->reacts_cap = js->reacts_cap ? js->reacts_cap * 2 : 16;
        js->reacts = au_realloc(js->reacts, js->reacts_cap * sizeof *js->reacts);
    }
    JsReaction *r = &js->reacts[js->nreacts++];
    r->kind = kind;
    r->el = (AuElement *)au_node_ref(&el->node);
    r->attr = attr;
    r->old = had ? au_strndup(old, old_len) : NULL;
    r->had_old = had;
}

/* Queues connected (kind 1) or disconnected (kind 2) reactions for the
 * custom element candidates in root's inclusive subtree, and through
 * shadow roots. */
static void subtree_reactions(AuJs *js, AuNode *root, uint8_t kind)
{
    AuNode *stack[64];
    int sp = 0;
    stack[sp++] = root;
    while (sp) {
        AuNode *top = stack[--sp];
        for (AuNode *n = top; n; n = au_node_next(n, top)) {
            if (n->type != AU_NODE_ELEMENT)
                continue;
            if (is_custom_candidate(n))
                push_react(js, kind, (AuElement *)n, 0, NULL, 0, false);
            AuElement *el = (AuElement *)n;
            if (el->shadow_root && sp < 64)
                stack[sp++] = el->shadow_root;
        }
    }
}

/* Queues a kind 5 reaction for each HTML script element in root's
 * inclusive subtree: the prelude runs the ones script created (a script
 * inserted by the parser or through innerHTML does not run here). */
static void subtree_scripts(AuJs *js, AuNode *root)
{
    for (AuNode *n = root; n; n = au_node_next(n, root))
        if (au_is_html_element(n, AU_ATOM_script))
            push_react(js, 5, (AuElement *)n, 0, NULL, 0, false);
}

static void touch_slots(AuJs *js, AuNode *parent)
{
    if ((parent->type == AU_NODE_ELEMENT && ((AuElement *)parent)->shadow_root) ||
        au_is_shadow_root(au_node_root(parent))) {
        js->slots_dirty = true;
        queue_delivery(js);
    }
}

/* A frame element (iframe, frame, object, embed) in child's subtree went
 * into or left the document: the frame list is brought up to date at the
 * end of the native that did it (js_frames_flush). */
static void frames_touched(AuJs *js, AuNode *parent, AuNode *child)
{
    if (js->frames_dirty || !js->hooks.frames_changed || !au_node_is_connected(parent))
        return;
    for (AuNode *n = child; n; n = au_node_next(n, child))
        if (js_is_frame_element(n)) {
            js->frames_dirty = true;
            js->root->any_frames_dirty = true;
            return;
        }
}

static void h_inserted(void *ctx, AuNode *parent, AuNode *child)
{
    AuJs *js = ctx;
    if (js->closing)
        return;
    js_ranges_inserted(js, parent, child);
    changed(js, parent, 0, AU_JS_CHANGE_CHILDREN);
    if (js->observers)
        push_mut(js, 1, parent, child, NULL, child->prev, child->next, 0, NULL, 0);
    if (js->ce_defs && au_node_is_connected(parent))
        subtree_reactions(js, child, 1);
    if (js->script_watch && au_node_is_connected(parent))
        subtree_scripts(js, child);
    frames_touched(js, parent, child);
    touch_slots(js, parent);
}

static void h_removed(void *ctx, AuNode *parent, AuNode *child, AuNode *prev, AuNode *next)
{
    AuJs *js = ctx;
    if (js->closing)
        return;
    js_ranges_removed(js, parent, child, prev);
    changed(js, parent, 0, AU_JS_CHANGE_CHILDREN);
    if (js->observers)
        push_mut(js, 1, parent, NULL, child, prev, next, 0, NULL, 0);
    if (js->ce_defs && au_node_is_connected(parent))
        subtree_reactions(js, child, 2);
    frames_touched(js, parent, child);
    touch_slots(js, parent);
}

static void h_attr(void *ctx, AuElement *el, AuAtom name, uint8_t ns, const char *old, size_t old_len)
{
    AuJs *js = ctx;
    if (js->closing)
        return;
    changed(js, &el->node, name, AU_JS_CHANGE_ATTR);
    if (js->observers)
        push_mut(js, 2, &el->node, NULL, NULL, NULL, NULL, name, old, old_len);
    if (js->ce_defs && is_custom_candidate(&el->node))
        push_react(js, 3, el, name, old, old_len, old != NULL);
    if (el->node.parent && (name == AU_ATOM_slot || name == au_atom("name", 4)))
        touch_slots(js, el->node.parent);
    if (js->hooks.frames_changed && js_is_frame_element(&el->node) && au_node_is_connected(&el->node)) {
        js->frames_dirty = true; /* src, srcdoc, data, name: after the change (the hook runs before it) */
        js->root->any_frames_dirty = true;
    }
}

static void h_text(void *ctx, AuText *t, const char *old, size_t old_len)
{
    AuJs *js = ctx;
    if (js->closing)
        return;
    js_ranges_text(js, t, old, old_len);
    changed(js, &t->node, 0, AU_JS_CHANGE_TEXT);
    if (js->observers)
        push_mut(js, 3, &t->node, NULL, NULL, NULL, NULL, 0, old, old_len);
}

static void h_hold(void *ctx, AuNode *n)
{
    js_hold_changed(ctx, n);
}

static const AuDomHooks dom_hooks = {h_inserted, h_removed, h_attr, h_text, h_hold};

void js_hooks_install(AuJs *js)
{
    au_document_set_hooks(js->doc, &dom_hooks, js);
}

void js_hooks_remove(AuJs *js)
{
    if (js->doc->hooks_ctx == js)
        au_document_set_hooks(js->doc, NULL, NULL);
}

/* ---- Delivery ---- */

static void free_mut(JsMutation *m)
{
    au_node_unref(m->target);
    au_node_unref(m->added);
    au_node_unref(m->removed);
    au_node_unref(m->prev);
    au_node_unref(m->next);
    au_free(m->old);
    for (uint8_t i = 0; i < m->nobs; i++)
        au_node_unref(m->obs[i]);
}

static void free_react(JsReaction *r)
{
    au_node_unref(&r->el->node);
    au_free(r->old);
}

void js_deliver_mutations(AuJs *js, bool notify)
{
    JSContext *ctx = js->ctx;
    /* Take the queue: delivery may queue more. */
    JsMutation *muts = js->muts;
    uint32_t n = js->nmuts;
    js->muts = NULL;
    js->nmuts = js->muts_cap = 0;
    bool slots = js->slots_dirty;
    js->slots_dirty = false;
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++) {
        JsMutation *m = &muts[i];
        JSValue rec = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, rec, 0, JS_NewInt32(ctx, m->kind));
        JS_SetPropertyUint32(ctx, rec, 1, js_wrap(js, m->target));
        JS_SetPropertyUint32(ctx, rec, 2, js_wrap(js, m->added));
        JS_SetPropertyUint32(ctx, rec, 3, js_wrap(js, m->removed));
        JS_SetPropertyUint32(ctx, rec, 4, js_wrap(js, m->prev));
        JS_SetPropertyUint32(ctx, rec, 5, js_wrap(js, m->next));
        JS_SetPropertyUint32(ctx, rec, 6, m->attr ? js_str(ctx, au_atom_str(m->attr), au_atom_len(m->attr)) : JS_NULL);
        JS_SetPropertyUint32(ctx, rec, 7, m->old ? js_str(ctx, m->old, m->old_len) : JS_NULL);
        JSValue obs = JS_NewArray(ctx);
        for (uint8_t k = 0; k < m->nobs; k++)
            JS_SetPropertyUint32(ctx, obs, k, js_wrap(js, m->obs[k]));
        JS_SetPropertyUint32(ctx, rec, 8, obs);
        JS_SetPropertyUint32(ctx, rec, 9, JS_NewUint32(ctx, m->batch));
        JS_SetPropertyUint32(ctx, arr, i, rec);
        free_mut(m);
    }
    au_free(muts);
    JSValue args[3] = {arr, JS_NewBool(ctx, slots), JS_NewBool(ctx, notify)};
    JSValue r = js_call_prelude(js, "deliverMutations", 3, args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arr);
}

void js_run_reactions(AuJs *js)
{
    if (js->in_reactions)
        return;
    js->in_reactions = true;
    JSContext *ctx = js->ctx;
    for (int round = 0; round < 1000 && js->nreacts; round++) {
        JsReaction *rs = js->reacts;
        uint32_t n = js->nreacts;
        js->reacts = NULL;
        js->nreacts = js->reacts_cap = 0;
        JSValue arr = JS_NewArray(ctx);
        for (uint32_t i = 0; i < n; i++) {
            JsReaction *r = &rs[i];
            JSValue rec = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, rec, 0, JS_NewInt32(ctx, r->kind));
            JS_SetPropertyUint32(ctx, rec, 1, js_wrap(js, &r->el->node));
            JS_SetPropertyUint32(ctx, rec, 2, r->attr ? js_str(ctx, au_atom_str(r->attr), au_atom_len(r->attr)) : JS_NULL);
            JS_SetPropertyUint32(ctx, rec, 3, r->had_old ? js_str(ctx, r->old, strlen(r->old)) : JS_NULL);
            JS_SetPropertyUint32(ctx, arr, i, rec);
            free_react(r);
        }
        au_free(rs);
        JSValue res = js_call_prelude(js, "runReactions", 1, &arr);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, arr);
    }
    js->in_reactions = false;
}

void js_ce_flush(AuJs *js)
{
    js->mut_batch++;
    if (js->nreacts && !js->in_reactions && !js->closing)
        js_run_reactions(js);
    if (js->root->any_frames_dirty)
        js_frames_flush(js);
}

void js_mutations_free(AuJs *js)
{
    for (uint32_t i = 0; i < js->nmuts; i++)
        free_mut(&js->muts[i]);
    au_free(js->muts);
    js->muts = NULL;
    js->nmuts = js->muts_cap = 0;
    for (uint32_t i = 0; i < js->nreacts; i++)
        free_react(&js->reacts[i]);
    au_free(js->reacts);
    js->reacts = NULL;
    js->nreacts = js->reacts_cap = 0;
}
