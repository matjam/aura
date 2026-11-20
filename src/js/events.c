/* Events from the embedder: simple events, mouse and keyboard input, the
 * document lifecycle, viewport changes, layout notifications and import
 * maps. Each calls a prelude entry point, then runs a microtask
 * checkpoint. */
#include "js_int.h"

#include <string.h>

/* Calls P[name](args) and returns its result as a bool (true when the
 * result is not false). */
static bool call_bool(AuJs *js, const char *name, int argc, JSValue *argv)
{
    bool r = true;
    if (!js->closing) {
        JSValue v = js_call_prelude(js, name, argc, argv);
        if (JS_IsBool(v))
            r = JS_ToBool(js->ctx, v);
        JS_FreeValue(js->ctx, v);
    }
    for (int i = 0; i < argc; i++)
        JS_FreeValue(js->ctx, argv[i]);
    au_js_checkpoint(js);
    return r;
}

bool au_js_fire(AuJs *js, AuNode *node, const char *type, bool bubbles, bool cancelable)
{
    JSContext *c = js->ctx;
    JSValue a[4] = {js_wrap(js, node), JS_NewString(c, type), JS_NewBool(c, bubbles), JS_NewBool(c, cancelable)};
    return call_bool(js, "fire", 4, a);
}

bool au_js_fire_mouse(AuJs *js, AuElement *target, const char *type, int x, int y, int button, uint32_t mods,
                      int detail, AuElement *related)
{
    if (!target)
        return true;
    JSContext *c = js->ctx;
    JSValue a[8] = {js_wrap(js, &target->node), JS_NewString(c, type), JS_NewInt32(c, x), JS_NewInt32(c, y),
                    JS_NewInt32(c, button < 0 || button > 4 ? 0 : button), JS_NewUint32(c, mods & 15),
                    JS_NewInt32(c, detail), js_wrap(js, related ? &related->node : NULL)};
    return call_bool(js, "fireMouse", 8, a);
}

bool au_js_fire_key(AuJs *js, AuElement *target, const char *type, const char *key, const char *code,
                    uint32_t mods, bool repeat)
{
    JSContext *c = js->ctx;
    JSValue a[6] = {js_wrap(js, target ? &target->node : NULL), JS_NewString(c, type), JS_NewString(c, key ? key : ""),
                    JS_NewString(c, code ? code : ""), JS_NewUint32(c, mods & 15), JS_NewBool(c, repeat)};
    return call_bool(js, "fireKey", 6, a);
}

void au_js_focus(AuJs *js, AuElement *el)
{
    JSValue a[1] = {js_wrap(js, el ? &el->node : NULL)};
    call_bool(js, "focusFromEmbedder", 1, a);
}

bool au_js_fire_input(AuJs *js, AuElement *el, const char *type, const char *input_type, const char *data)
{
    JSContext *c = js->ctx;
    JSValue a[4] = {js_wrap(js, &el->node), JS_NewString(c, type), JS_NewString(c, input_type ? input_type : ""),
                    data ? JS_NewString(c, data) : JS_NULL};
    return call_bool(js, "fireInput", 4, a);
}

bool au_js_fire_submit(AuJs *js, AuElement *form, AuElement *submitter)
{
    JSValue a[2] = {js_wrap(js, &form->node), js_wrap(js, submitter ? &submitter->node : NULL)};
    return call_bool(js, "fireSubmit", 2, a);
}

bool au_js_close_request(AuJs *js)
{
    bool r = false;
    if (!js->closing) {
        JSValue v = js_call_prelude(js, "closeRequest", 0, NULL);
        r = JS_IsBool(v) && JS_ToBool(js->ctx, v);
        JS_FreeValue(js->ctx, v);
    }
    au_js_checkpoint(js);
    return r;
}

void au_js_light_dismiss(AuJs *js, AuElement *target, bool up)
{
    JSValue a[2] = {js_wrap(js, target ? &target->node : NULL), JS_NewBool(js->ctx, up)};
    call_bool(js, "lightDismiss", 2, a);
}

int au_js_pattern_test(AuJs *js, const char *pattern, const char *value)
{
    if (js->closing)
        return -1;
    JSContext *c = js->ctx;
    JSValue a[2] = {JS_NewString(c, pattern), JS_NewString(c, value)};
    JSValue v = js_call_prelude(js, "patternTest", 2, a);
    int r = -1;
    if (JS_IsNumber(v)) {
        int32_t x;
        if (!JS_ToInt32(c, &x, v))
            r = x;
    }
    JS_FreeValue(c, v);
    JS_FreeValue(c, a[0]);
    JS_FreeValue(c, a[1]);
    return r;
}

void au_js_set_ready_state(AuJs *js, const char *state)
{
    JSValue a[1] = {JS_NewString(js->ctx, state)};
    call_bool(js, "setReadyState", 1, a);
}

void au_js_unload(AuJs *js)
{
    call_bool(js, "unload", 0, NULL);
}

bool au_js_has_unload_handler(AuJs *js)
{
    bool r = true; /* when in doubt, the document is not kept */
    if (!js->closing) {
        JSValue v = js_call_prelude(js, "hasUnloadHandler", 0, NULL);
        if (JS_IsBool(v))
            r = JS_ToBool(js->ctx, v);
        JS_FreeValue(js->ctx, v);
    }
    return r;
}

void au_js_page_transition(AuJs *js, bool show)
{
    JSValue a[1] = {JS_NewBool(js->ctx, show)};
    call_bool(js, "pageTransition", 1, a);
}

void au_js_resized(AuJs *js)
{
    call_bool(js, "resized", 0, NULL);
}

void au_js_media_changed(AuJs *js)
{
    call_bool(js, "mediaChanged", 0, NULL);
}

void au_js_scrolled(AuJs *js)
{
    call_bool(js, "scrolled", 0, NULL);
}

void au_js_element_scrolled(AuJs *js, AuElement *el)
{
    JSValue a[1] = {js_wrap(js, &el->node)};
    call_bool(js, "elementScrolled", 1, a);
}

void au_js_layout_done(AuJs *js)
{
    call_bool(js, "layoutDone", 0, NULL);
}

void au_js_add_import_map(AuJs *js, const char *src, size_t len)
{
    JSValue a[1] = {js_str(js->ctx, src, len)};
    call_bool(js, "addImportMap", 1, a);
}

void au_js_pop_state(AuJs *js, const char *url, uint32_t key)
{
    JSValue a[2] = {url ? JS_NewString(js->ctx, url) : JS_NULL, JS_NewUint32(js->ctx, key)};
    call_bool(js, "popState", 2, a);
}

int au_js_script_kind(AuJs *js, AuElement *el)
{
    JSValue a[1] = {js_wrap(js, &el->node)};
    JSValue v = js_call_prelude(js, "scriptKind", 1, a);
    JS_FreeValue(js->ctx, a[0]);
    int k = AU_JS_SCRIPT_NONE;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(js->ctx, v);
        if (s) {
            k = !strcmp(s, "classic") ? AU_JS_SCRIPT_CLASSIC : !strcmp(s, "module") ? AU_JS_SCRIPT_MODULE
                : !strcmp(s, "importmap") ? AU_JS_SCRIPT_IMPORTMAP : AU_JS_SCRIPT_NONE;
            JS_FreeCString(js->ctx, s);
        }
    }
    JS_FreeValue(js->ctx, v);
    return k;
}
