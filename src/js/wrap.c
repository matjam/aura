/* Node wrappers and interface objects. The ownership model is described
 * in js_int.h. */
#include "js_int.h"

#include <string.h>

static const char *const iface_names[JSI_COUNT] = {
#define IFACE(id, name, parent) name,
#include "ifaces.def"
#undef IFACE
};

static const int iface_parent[JSI_COUNT] = {
#define IFACE(id, name, parent) JSI_##parent,
#include "ifaces.def"
#undef IFACE
};

/* ---- Element interfaces by tag ---- */

static const struct {
    const char *tag;
    int iface;
} tag_ifaces[] = {
    {"a", JSI_HTMLAnchorElement}, {"area", JSI_HTMLAreaElement}, {"audio", JSI_HTMLAudioElement},
    {"video", JSI_HTMLVideoElement}, {"br", JSI_HTMLBRElement}, {"base", JSI_HTMLBaseElement},
    {"body", JSI_HTMLBodyElement}, {"button", JSI_HTMLButtonElement}, {"canvas", JSI_HTMLCanvasElement},
    {"dl", JSI_HTMLDListElement}, {"data", JSI_HTMLDataElement}, {"datalist", JSI_HTMLDataListElement},
    {"details", JSI_HTMLDetailsElement}, {"dialog", JSI_HTMLDialogElement}, {"div", JSI_HTMLDivElement},
    {"embed", JSI_HTMLEmbedElement}, {"fieldset", JSI_HTMLFieldSetElement}, {"form", JSI_HTMLFormElement},
    {"frame", JSI_HTMLFrameElement}, {"frameset", JSI_HTMLFrameSetElement},
    {"h1", JSI_HTMLHeadingElement}, {"h2", JSI_HTMLHeadingElement}, {"h3", JSI_HTMLHeadingElement},
    {"h4", JSI_HTMLHeadingElement}, {"h5", JSI_HTMLHeadingElement}, {"h6", JSI_HTMLHeadingElement},
    {"head", JSI_HTMLHeadElement}, {"hr", JSI_HTMLHRElement}, {"html", JSI_HTMLHtmlElement},
    {"iframe", JSI_HTMLIFrameElement}, {"img", JSI_HTMLImageElement}, {"input", JSI_HTMLInputElement},
    {"li", JSI_HTMLLIElement}, {"label", JSI_HTMLLabelElement}, {"legend", JSI_HTMLLegendElement},
    {"link", JSI_HTMLLinkElement}, {"map", JSI_HTMLMapElement}, {"menu", JSI_HTMLMenuElement},
    {"meta", JSI_HTMLMetaElement}, {"meter", JSI_HTMLMeterElement}, {"ins", JSI_HTMLModElement},
    {"del", JSI_HTMLModElement}, {"ol", JSI_HTMLOListElement}, {"object", JSI_HTMLObjectElement},
    {"optgroup", JSI_HTMLOptGroupElement}, {"option", JSI_HTMLOptionElement}, {"output", JSI_HTMLOutputElement},
    {"p", JSI_HTMLParagraphElement}, {"picture", JSI_HTMLPictureElement}, {"pre", JSI_HTMLPreElement},
    {"listing", JSI_HTMLPreElement}, {"xmp", JSI_HTMLPreElement}, {"progress", JSI_HTMLProgressElement},
    {"q", JSI_HTMLQuoteElement}, {"blockquote", JSI_HTMLQuoteElement}, {"script", JSI_HTMLScriptElement},
    {"select", JSI_HTMLSelectElement}, {"slot", JSI_HTMLSlotElement}, {"source", JSI_HTMLSourceElement},
    {"span", JSI_HTMLSpanElement}, {"style", JSI_HTMLStyleElement}, {"table", JSI_HTMLTableElement},
    {"caption", JSI_HTMLTableCaptionElement}, {"td", JSI_HTMLTableCellElement}, {"th", JSI_HTMLTableCellElement},
    {"col", JSI_HTMLTableColElement}, {"colgroup", JSI_HTMLTableColElement},
    {"thead", JSI_HTMLTableSectionElement}, {"tbody", JSI_HTMLTableSectionElement},
    {"tfoot", JSI_HTMLTableSectionElement}, {"tr", JSI_HTMLTableRowElement},
    {"template", JSI_HTMLTemplateElement}, {"textarea", JSI_HTMLTextAreaElement}, {"time", JSI_HTMLTimeElement},
    {"title", JSI_HTMLTitleElement}, {"track", JSI_HTMLTrackElement}, {"ul", JSI_HTMLUListElement},
};

/* Elements of the HTML standard with no interface of their own. */
static const char plain_html[] =
    " abbr acronym address article aside b basefont bdi bdo big center cite code dd dfn dt em figcaption figure"
    " footer header hgroup i kbd main mark nav nobr noembed noframes noscript plaintext rb rp rt rtc ruby s samp"
    " search section small strike strong sub summary sup tt u var wbr ";

static int html_iface(AuAtom tag)
{
    const char *s = au_atom_str(tag);
    size_t n = au_atom_len(tag);
    for (size_t i = 0; i < JS_COUNTOF(tag_ifaces); i++)
        if (strcmp(tag_ifaces[i].tag, s) == 0)
            return tag_ifaces[i].iface;
    if (memchr(s, '-', n))
        return JSI_HTMLElement;
    if (n < 16) {
        char key[20];
        key[0] = ' ';
        memcpy(key + 1, s, n);
        key[n + 1] = ' ';
        key[n + 2] = 0;
        if (strstr(plain_html, key))
            return JSI_HTMLElement;
    }
    return JSI_HTMLUnknownElement;
}

int js_iface_of(const AuNode *n)
{
    switch (n->type) {
    case AU_NODE_ELEMENT: {
        const AuElement *el = (const AuElement *)n;
        if (el->ns == AU_NS_SVG)
            return JSI_SVGElement;
        if (el->ns == AU_NS_MATHML)
            return JSI_MathMLElement;
        return html_iface(el->tag);
    }
    case AU_NODE_TEXT:
        return JSI_Text;
    case AU_NODE_COMMENT:
        return JSI_Comment;
    case AU_NODE_PROCESSING_INSTRUCTION:
        return JSI_ProcessingInstruction;
    case AU_NODE_DOCTYPE:
        return JSI_DocumentType;
    case AU_NODE_DOCUMENT:
        return JSI_HTMLDocument;
    case AU_NODE_FRAGMENT:
        return au_is_shadow_root(n) ? JSI_ShadowRoot : JSI_DocumentFragment;
    }
    return JSI_Node;
}

JSValue js_iface_proto(AuJs *js, int iface)
{
    return js->proto[iface];
}

/* ---- The node class ---- */

static void wrap_unlink_pending(AuJs *js, AuJsWrap *w)
{
    if (!w->pending)
        return;
    for (AuJsWrap **pp = &js->pending; *pp; pp = &(*pp)->pnext)
        if (*pp == w) {
            *pp = w->pnext;
            break;
        }
    w->pending = false;
}

static void node_finalizer(JSRuntime *rt, JSValueConst val)
{
    AuJs *js = JS_GetRuntimeOpaque(rt); /* the root: it keeps the wrapper lists */
    AuJsWrap *w = JS_GetOpaque(val, js->cls_node);
    if (!w)
        return;
    AuNode *n = w->node;
    wrap_unlink_pending(js, w);
    if (w->prev)
        w->prev->next = w->next;
    else
        js->wraps = w->next;
    if (w->next)
        w->next->prev = w->prev;
    js->nwraps--;
    n->js = NULL;
    n->flags &= (uint8_t)~AU_NODEF_JS_STRONG;
    au_free(w);
    au_node_unref(n);
}

static JSClassDef node_class = {"Node", node_finalizer, NULL, NULL, NULL};

/* The AuJs whose prototypes a node's wrapper gets: the one of the node's
 * document when that document has a context in this runtime (a frame's
 * node reached from its parent is an object of the frame's realm), else
 * the caller's. */
static AuJs *wrap_realm(AuJs *js, const AuNode *n)
{
    AuJs *o = au_js_of(n->doc);
    if (o && o != js && o->root == js->root && !o->closing)
        return o;
    return js;
}

JSValue js_wrap(AuJs *js, AuNode *n)
{
    if (!n)
        return JS_NULL;
    AuJs *root = js->root;
    AuJsWrap *w = n->js;
    if (w && w->js == root)
        return JS_DupValue(js->ctx, w->obj);
    if (w || js->closing)
        return JS_NULL; /* a node of another runtime: not reachable from here */
    js = wrap_realm(js, n);
    JSValue obj = JS_NewObjectProtoClass(js->ctx, js->proto[js_iface_of(n)], js->cls_node);
    if (JS_IsException(obj))
        return obj;
    w = au_calloc(sizeof *w);
    w->obj = obj;
    w->node = n;
    w->js = root; /* wrappers belong to the runtime */
    js = root;
    w->next = js->wraps;
    if (js->wraps)
        js->wraps->prev = w;
    js->wraps = w;
    js->nwraps++;
    JS_SetOpaque(obj, w);
    n->js = w;
    au_node_ref(n); /* the wrapper's reference; hold_changed then sees the node held or not */
    if (au_node_held_elsewhere(n) && !(n->flags & AU_NODEF_JS_STRONG)) {
        n->flags |= AU_NODEF_JS_STRONG;
        JS_DupValue(js->ctx, obj);
    }
    return obj;
}

AuNode *js_unwrap(AuJs *js, JSValueConst v)
{
    AuJsWrap *w = JS_GetOpaque(v, js->cls_node);
    return w ? w->node : NULL;
}

/* Called from the DOM's hold_changed hook. Taking the reference happens at
 * once; dropping it waits for a safe point. */
void js_hold_changed(AuJs *js, AuNode *n)
{
    AuJsWrap *w = n->js;
    js = js->root;
    if (!w || w->js != js || js->closing)
        return;
    bool want = au_node_held_elsewhere(n);
    bool strong = (n->flags & AU_NODEF_JS_STRONG) != 0;
    if (want && !strong) {
        n->flags |= AU_NODEF_JS_STRONG;
        JS_DupValue(js->ctx, w->obj);
    } else if (!want && strong && !w->pending) {
        w->pending = true;
        w->pnext = js->pending;
        js->pending = w;
    }
}

void js_drain_pending(AuJs *js)
{
    js = js->root;
    while (js->pending) {
        AuJsWrap *w = js->pending;
        js->pending = w->pnext;
        w->pending = false;
        AuNode *n = w->node;
        if (!au_node_held_elsewhere(n) && (n->flags & AU_NODEF_JS_STRONG)) {
            n->flags &= (uint8_t)~AU_NODEF_JS_STRONG;
            JS_FreeValue(js->ctx, w->obj); /* may finalize w and free n */
        }
    }
}

/* A frame's context is going. The nodes of its documents let go of their
 * wrappers, so wrappers no script holds are collected (and release their
 * nodes); the context's interface objects go too. */
void js_wrap_free_child(AuJs *js)
{
    AuJs *root = js->root;
    uint32_t n = 0, cap = root->nwraps;
    JSValue *vals = cap ? au_alloc(cap * sizeof *vals) : NULL;
    for (AuJsWrap *w = root->wraps; w; w = w->next) {
        AuDocument *d = w->node->doc;
        bool mine = d == js->doc;
        for (uint32_t i = 0; i < js->ndocs && !mine; i++)
            mine = js->docs[i] == d;
        if (mine && (w->node->flags & AU_NODEF_JS_STRONG) && n < cap) {
            w->node->flags &= (uint8_t)~AU_NODEF_JS_STRONG;
            vals[n++] = w->obj;
        }
    }
    for (uint32_t i = 0; i < n; i++)
        JS_FreeValue(js->ctx, vals[i]);
    au_free(vals);
    for (int i = 0; i < JSI_COUNT; i++) {
        JS_FreeValue(js->ctx, js->proto[i]);
        JS_FreeValue(js->ctx, js->ctor[i]);
        js->proto[i] = js->ctor[i] = JS_UNDEFINED;
    }
}

void js_wrap_free_all(AuJs *js)
{
    js->pending = NULL;
    /* Collect the strong wrappers first: freeing one may finalize others
     * and unlink them from the list. */
    uint32_t n = 0, cap = js->nwraps;
    JSValue *vals = cap ? au_alloc(cap * sizeof *vals) : NULL;
    for (AuJsWrap *w = js->wraps; w; w = w->next) {
        w->pending = false;
        if ((w->node->flags & AU_NODEF_JS_STRONG) && n < cap) {
            w->node->flags &= (uint8_t)~AU_NODEF_JS_STRONG;
            vals[n++] = w->obj;
        }
    }
    for (uint32_t i = 0; i < n; i++)
        JS_FreeValue(js->ctx, vals[i]);
    au_free(vals);
    for (int i = 0; i < JSI_COUNT; i++) {
        JS_FreeValue(js->ctx, js->proto[i]);
        JS_FreeValue(js->ctx, js->ctor[i]);
        js->proto[i] = js->ctor[i] = JS_UNDEFINED;
    }
}

/* ---- Interface objects ---- */

static JSValue iface_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Failed to construct '%s': use the new operator", iface_names[magic]);
    AuNode *n = NULL;
    switch (magic) {
    case JSI_EventTarget: {
        JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        JSValue o = JS_NewObjectProto(ctx, proto);
        JS_FreeValue(ctx, proto);
        return o;
    }
    case JSI_Text:
    case JSI_Comment: {
        size_t len = 0;
        const char *s = argc > 0 && !JS_IsUndefined(argv[0]) ? JS_ToCStringLen(ctx, &len, argv[0]) : NULL;
        if (argc > 0 && !JS_IsUndefined(argv[0]) && !s)
            return JS_EXCEPTION;
        n = magic == JSI_Text ? au_text_new(js->doc, s ? s : "", len) : au_comment_new(js->doc, s ? s : "", len);
        if (s)
            JS_FreeCString(ctx, s);
        break;
    }
    case JSI_DocumentFragment:
        n = au_fragment_new(js->doc);
        break;
    default:
        if (magic >= JSI_HTMLElement) {
            /* Custom elements: the registry finds the definition by
             * new.target and creates (or upgrades) the element. */
            JSValue args[2] = {JS_DupValue(ctx, new_target), JS_NewInt32(ctx, magic)};
            JSValue fn = JS_GetPropertyStr(ctx, js->P, "constructElement");
            JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, args[0]);
            return r;
        }
        return JS_ThrowTypeError(ctx, "Illegal constructor");
    }
    JSValue obj = js_wrap(js, n);
    au_node_unref(n); /* the wrapper holds it */
    /* A subclass (class X extends Text) gets its own prototype. */
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsObject(proto) && JS_VALUE_GET_PTR(proto) != JS_VALUE_GET_PTR(js->proto[magic]))
        JS_SetPrototype(ctx, obj, proto);
    JS_FreeValue(ctx, proto);
    return obj;
}

void js_wrap_init(AuJs *js)
{
    JSContext *ctx = js->ctx;
    if (js->root != js) {
        js->cls_node = js->root->cls_node; /* classes belong to the runtime */
    } else {
        JS_NewClassID(js->rt, &js->cls_node);
        JS_NewClass(js->rt, js->cls_node, &node_class);
    }
    JSValue objproto = JS_GetPrototype(ctx, js->global); /* Object.prototype (via the global's chain) */
    for (int i = 0; i < JSI_COUNT; i++) {
        int p = iface_parent[i];
        js->proto[i] = JS_NewObjectProto(ctx, p == JSI_COUNT ? objproto : js->proto[p]);
        JSValue c = JS_NewCFunction2(ctx, (JSCFunction *)(void *)iface_ctor, iface_names[i], 0,
                                     JS_CFUNC_constructor_or_func_magic, i);
        if (p != JSI_COUNT)
            JS_SetPrototype(ctx, c, js->ctor[p]); /* static inheritance: HTMLElement.__proto__ === Element */
        JS_SetConstructor(ctx, c, js->proto[i]);
        js->ctor[i] = c;
        JS_DefinePropertyValueStr(ctx, js->global, iface_names[i], JS_DupValue(ctx, c),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    JS_FreeValue(ctx, objproto);
    /* The global object is the window. */
    JS_SetPrototype(ctx, js->global, js->proto[JSI_Window]);
}
