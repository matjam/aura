/* DOM bindings in C: Node, Element, Document, CharacterData, ShadowRoot.
 * Each interface's native members are a JSCFunctionListEntry table put on
 * its prototype (js_natives_init). Every member validates its receiver
 * and arguments; tree mutations check the DOM's hierarchy rules, since
 * src/dom does not. */
#include "js_int.h"
#include "../css/css.h"

#include <string.h>

#define JSTHIS(n)                                                                                                    \
    AuJs *js = js_of_ctx(ctx);                                                                                       \
    AuNode *n = js_unwrap(js, this_val);                                                                             \
    if (!n)                                                                                                          \
        return JS_ThrowTypeError(ctx, "Illegal invocation");

#define JSTHIS_EL(e)                                                                                                 \
    JSTHIS(n_);                                                                                                      \
    if (n_->type != AU_NODE_ELEMENT)                                                                                 \
        return JS_ThrowTypeError(ctx, "Illegal invocation");                                                         \
    AuElement *e = (AuElement *)n_;

static AuNode *arg_node(AuJs *js, JSValueConst v)
{
    return js_unwrap(js, v);
}

static JSValue throw_dom(JSContext *ctx, const char *name, const char *msg)
{
    return JS_ThrowDOMException(ctx, name, "%s", msg);
}

/* A string argument as an au_alloc'd UTF-8 copy; NULL after an exception. */
static char *arg_str(JSContext *ctx, JSValueConst v, size_t *len)
{
    return js_tostr(ctx, v, len);
}

/* A name argument as an atom; lowercased when lower. 0 after an exception. */
static AuAtom arg_atom(JSContext *ctx, JSValueConst v, bool lower)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return 0;
    AuAtom a;
    if (lower) {
        char buf[128];
        char *t = n < sizeof buf ? buf : au_alloc(n + 1);
        for (size_t i = 0; i < n; i++)
            t[i] = (char)au_lower((unsigned char)s[i]);
        a = au_atom(t, n);
        if (t != buf)
            au_free(t);
    } else {
        a = au_atom(s, n);
    }
    JS_FreeCString(ctx, s);
    return a;
}

static bool is_html_doc_element(const AuElement *e)
{
    return e->ns == AU_NS_HTML && e->node.doc;
}

/* ---- Names ---- */

static const char *const ns_uri[] = {"http://www.w3.org/1999/xhtml", "http://www.w3.org/2000/svg",
                                     "http://www.w3.org/1998/Math/MathML"};

static JSValue tag_name(JSContext *ctx, const AuElement *e)
{
    const char *s = au_atom_str(e->tag);
    size_t n = au_atom_len(e->tag);
    if (e->ns != AU_NS_HTML)
        return js_str(ctx, s, n);
    char buf[128];
    char *t = n < sizeof buf ? buf : au_alloc(n + 1);
    for (size_t i = 0; i < n; i++)
        t[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    JSValue r = js_str(ctx, t, n);
    if (t != buf)
        au_free(t);
    return r;
}

/* ---- Node: tree accessors ---- */

enum { G_PARENT, G_PARENT_EL, G_FIRST, G_LAST, G_PREV, G_NEXT, G_FIRST_EL, G_LAST_EL, G_PREV_EL, G_NEXT_EL };

static AuNode *elem_step(AuNode *n, bool forward)
{
    while (n && n->type != AU_NODE_ELEMENT)
        n = forward ? n->next : n->prev;
    return n;
}

static JSValue g_tree(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSTHIS(n);
    AuNode *r = NULL;
    switch (magic) {
    case G_PARENT:
        r = n->parent;
        break;
    case G_PARENT_EL:
        r = n->parent && n->parent->type == AU_NODE_ELEMENT ? n->parent : NULL;
        break;
    case G_FIRST:
        r = n->first_child;
        break;
    case G_LAST:
        r = n->last_child;
        break;
    case G_PREV:
        r = n->prev;
        break;
    case G_NEXT:
        r = n->next;
        break;
    case G_FIRST_EL:
        r = elem_step(n->first_child, true);
        break;
    case G_LAST_EL:
        r = elem_step(n->last_child, false);
        break;
    case G_PREV_EL:
        r = elem_step(n->prev, false);
        break;
    case G_NEXT_EL:
        r = elem_step(n->next, true);
        break;
    }
    return js_wrap(js, r);
}

static JSValue g_child_element_count(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    int c = 0;
    for (AuNode *k = n->first_child; k; k = k->next)
        c += k->type == AU_NODE_ELEMENT;
    return JS_NewInt32(ctx, c);
}

static JSValue g_node_type(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    return JS_NewInt32(ctx, n->type);
}

static JSValue g_node_name(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    switch (n->type) {
    case AU_NODE_ELEMENT:
        return tag_name(ctx, (AuElement *)n);
    case AU_NODE_TEXT:
        return JS_NewString(ctx, "#text");
    case AU_NODE_COMMENT:
        return JS_NewString(ctx, "#comment");
    case AU_NODE_DOCUMENT:
        return JS_NewString(ctx, "#document");
    case AU_NODE_FRAGMENT:
        return JS_NewString(ctx, "#document-fragment");
    case AU_NODE_DOCTYPE:
        return JS_NewString(ctx, ((AuDoctype *)n)->name);
    case AU_NODE_PROCESSING_INSTRUCTION: {
        AuAtom t = ((AuProcessingInstruction *)n)->target;
        return js_str(ctx, au_atom_str(t), au_atom_len(t));
    }
    }
    return JS_NewString(ctx, "");
}

static JSValue g_owner_document(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    if (n->type == AU_NODE_DOCUMENT)
        return JS_NULL;
    return js_wrap(js, n->doc ? &n->doc->node : NULL);
}

static JSValue g_is_connected(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    return JS_NewBool(ctx, au_node_is_connected(n));
}

static JSValue m_get_root_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(n);
    bool composed = false;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue c = JS_GetPropertyStr(ctx, argv[0], "composed");
        composed = JS_ToBool(ctx, c);
        JS_FreeValue(ctx, c);
    }
    return js_wrap(js, composed ? au_node_shadow_including_root(n) : au_node_root(n));
}

static JSValue m_has_child_nodes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(n);
    return JS_NewBool(ctx, n->first_child != NULL);
}

/* ---- Text content ---- */

static void collect_text(const AuNode *n, AuBuf *out)
{
    for (const AuNode *c = n->first_child; c; c = au_node_next(c, n))
        if (c->type == AU_NODE_TEXT)
            au_buf_append(out, ((const AuText *)c)->data, ((const AuText *)c)->len);
}

static bool is_chardata(const AuNode *n)
{
    return n->type == AU_NODE_TEXT || n->type == AU_NODE_COMMENT || n->type == AU_NODE_PROCESSING_INSTRUCTION;
}

static JSValue g_text_content(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    if (is_chardata(n))
        return js_str(ctx, ((AuText *)n)->data, ((AuText *)n)->len);
    if (n->type == AU_NODE_DOCUMENT || n->type == AU_NODE_DOCTYPE)
        return JS_NULL;
    AuBuf b;
    au_buf_init(&b);
    collect_text(n, &b);
    JSValue r = js_str(ctx, b.data, b.len);
    au_buf_free(&b);
    return r;
}

static void remove_all_children(AuNode *n)
{
    while (n->first_child)
        au_node_remove(n->first_child);
}

static JSValue s_text_content(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    JSTHIS(n);
    if (n->type == AU_NODE_DOCUMENT || n->type == AU_NODE_DOCTYPE)
        return JS_UNDEFINED;
    size_t len = 0;
    char *s = JS_IsNull(val) ? NULL : arg_str(ctx, val, &len);
    if (!JS_IsNull(val) && !s)
        return JS_EXCEPTION;
    if (is_chardata(n)) {
        au_text_set((AuText *)n, s ? s : "", len);
    } else {
        remove_all_children(n);
        if (len) {
            AuNode *t = au_text_new(n->doc, s, len);
            au_node_append(n, t);
            au_node_unref(t);
        }
    }
    au_free(s);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

static JSValue g_node_value(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    if (is_chardata(n))
        return js_str(ctx, ((AuText *)n)->data, ((AuText *)n)->len);
    return JS_NULL;
}

static JSValue s_node_value(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    JSTHIS(n);
    if (!is_chardata(n))
        return JS_UNDEFINED;
    size_t len = 0;
    char *s = JS_IsNull(val) ? NULL : arg_str(ctx, val, &len);
    if (!JS_IsNull(val) && !s)
        return JS_EXCEPTION;
    au_text_set((AuText *)n, s ? s : "", len);
    au_free(s);
    return JS_UNDEFINED;
}

/* ---- Mutation with hierarchy checks ---- */

/* True when a is a host-including inclusive ancestor of b. */
static bool host_including_ancestor(const AuNode *a, const AuNode *b)
{
    for (const AuNode *n = b; n;) {
        if (n == a)
            return true;
        if (n->parent)
            n = n->parent;
        else if (au_is_shadow_root(n) && ((const AuShadowRoot *)n)->host)
            n = &((const AuShadowRoot *)n)->host->node;
        else
            break;
    }
    return false;
}

static int count_type(const AuNode *parent, int type, const AuNode *except)
{
    int c = 0;
    for (const AuNode *k = parent->first_child; k; k = k->next)
        c += k->type == type && k != except;
    return c;
}

/* The DOM's "ensure pre-insertion validity" (and the replace variant when
 * replacing is set: child is then the node replaced). Throws and returns
 * false when invalid. */
static bool check_insert(JSContext *ctx, AuNode *parent, AuNode *node, AuNode *child, bool replacing)
{
    if (parent->type != AU_NODE_DOCUMENT && parent->type != AU_NODE_FRAGMENT && parent->type != AU_NODE_ELEMENT) {
        throw_dom(ctx, "HierarchyRequestError", "the parent cannot have children");
        return false;
    }
    if (host_including_ancestor(node, parent)) {
        throw_dom(ctx, "HierarchyRequestError", "the new child is an ancestor of the parent");
        return false;
    }
    if (child && child->parent != parent) {
        throw_dom(ctx, "NotFoundError", "the reference node is not a child of this node");
        return false;
    }
    if (node->type == AU_NODE_DOCUMENT || au_is_shadow_root(node) ||
        (node->type == AU_NODE_TEXT && parent->type == AU_NODE_DOCUMENT) ||
        (node->type == AU_NODE_DOCTYPE && parent->type != AU_NODE_DOCUMENT)) {
        throw_dom(ctx, "HierarchyRequestError", "this node cannot be inserted here");
        return false;
    }
    if (parent->type != AU_NODE_DOCUMENT)
        return true;
    /* Document children: at most one element and one doctype, doctype first. */
    int els = 0, texts = 0;
    if (node->type == AU_NODE_FRAGMENT) {
        for (AuNode *k = node->first_child; k; k = k->next) {
            els += k->type == AU_NODE_ELEMENT;
            texts += k->type == AU_NODE_TEXT;
        }
        if (texts || els > 1) {
            throw_dom(ctx, "HierarchyRequestError", "a document has one element child and no text");
            return false;
        }
    } else if (node->type == AU_NODE_ELEMENT) {
        els = 1;
    }
    AuNode *except = replacing ? child : NULL;
    if (els) {
        if (count_type(parent, AU_NODE_ELEMENT, except)) {
            throw_dom(ctx, "HierarchyRequestError", "the document already has an element child");
            return false;
        }
        if (child && !replacing && (child->type == AU_NODE_DOCTYPE))
            goto bad_order;
        for (AuNode *k = child ? child->next : NULL; k; k = k->next)
            if (k->type == AU_NODE_DOCTYPE)
                goto bad_order;
    }
    if (node->type == AU_NODE_DOCTYPE) {
        if (count_type(parent, AU_NODE_DOCTYPE, except)) {
            throw_dom(ctx, "HierarchyRequestError", "the document already has a doctype");
            return false;
        }
        for (AuNode *k = parent->first_child; k && k != child; k = k->next)
            if (k->type == AU_NODE_ELEMENT && k != except)
                goto bad_order;
        if (!child && count_type(parent, AU_NODE_ELEMENT, except))
            goto bad_order;
    }
    return true;
bad_order:
    throw_dom(ctx, "HierarchyRequestError", "the doctype must come before the element");
    return false;
}

static void set_doc_subtree(AuNode *root, AuDocument *doc)
{
    /* Shadow roots met on the way are walked from a list rather than by a
     * nested call: script can nest hosts in shadow roots without limit. */
    AuNode **work = NULL;
    size_t nwork = 0, cap = 0;
    for (;;) {
        if (root->doc != doc) {
            for (AuNode *n = root; n; n = au_node_next(n, root)) {
                n->doc = doc;
                if (n->type == AU_NODE_ELEMENT) {
                    AuElement *e = (AuElement *)n;
                    if (e->template_content)
                        e->template_content->doc = doc; /* its contents keep their own document in the standard; close enough */
                    if (e->shadow_root) {
                        if (nwork == cap) {
                            cap = cap ? cap * 2 : 8;
                            work = au_realloc(work, cap * sizeof *work);
                        }
                        work[nwork++] = e->shadow_root;
                    }
                }
            }
        }
        if (!nwork)
            break;
        root = work[--nwork];
    }
    au_free(work);
}

/* Inserts node (a fragment's children when it is a fragment) before child.
 * The caller has checked validity. */
static void insert_node(AuNode *parent, AuNode *node, AuNode *child)
{
    if (node->doc != parent->doc)
        set_doc_subtree(node, parent->doc);
    if (node->type == AU_NODE_FRAGMENT) {
        while (node->first_child) {
            AuNode *k = node->first_child;
            if (k->doc != parent->doc)
                set_doc_subtree(k, parent->doc);
            au_node_insert_before(parent, k, child);
        }
    } else {
        au_node_insert_before(parent, node, child);
    }
}

static JSValue m_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(p);
    AuNode *node = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!node)
        return JS_ThrowTypeError(ctx, "appendChild: argument is not a Node");
    if (!check_insert(ctx, p, node, NULL, false))
        return JS_EXCEPTION;
    au_node_ref(node);
    insert_node(p, node, NULL);
    au_node_unref(node);
    js_ce_flush(js);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue m_insert_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(p);
    AuNode *node = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!node)
        return JS_ThrowTypeError(ctx, "insertBefore: argument is not a Node");
    AuNode *child = NULL;
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        child = arg_node(js, argv[1]);
        if (!child)
            return JS_ThrowTypeError(ctx, "insertBefore: reference is not a Node");
    }
    if (!check_insert(ctx, p, node, child, false))
        return JS_EXCEPTION;
    if (child == node)
        child = node->next;
    au_node_ref(node);
    if (child)
        au_node_ref(child);
    insert_node(p, node, child);
    au_node_unref(node);
    au_node_unref(child);
    js_ce_flush(js);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue m_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(p);
    AuNode *c = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!c)
        return JS_ThrowTypeError(ctx, "removeChild: argument is not a Node");
    if (c->parent != p)
        return throw_dom(ctx, "NotFoundError", "the node is not a child of this node");
    au_node_remove(c); /* the wrapper (argv[0]) holds it */
    js_ce_flush(js);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue m_replace_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(p);
    AuNode *node = argc > 0 ? arg_node(js, argv[0]) : NULL;
    AuNode *child = argc > 1 ? arg_node(js, argv[1]) : NULL;
    if (!node || !child)
        return JS_ThrowTypeError(ctx, "replaceChild: arguments are not Nodes");
    if (!check_insert(ctx, p, node, child, true))
        return JS_EXCEPTION;
    if (node == child)
        return JS_DupValue(ctx, argv[1]);
    AuNode *ref = child->next == node ? node->next : child->next;
    au_node_ref(node);
    if (ref)
        au_node_ref(ref);
    au_node_remove(child);
    insert_node(p, node, ref);
    au_node_unref(ref);
    au_node_unref(node);
    js_ce_flush(js);
    return JS_DupValue(ctx, argv[1]);
}

/* ---- Cloning ---- */

static AuNode *clone_one(AuDocument *doc, const AuNode *n)
{
    switch (n->type) {
    case AU_NODE_ELEMENT: {
        const AuElement *e = (const AuElement *)n;
        AuNode *c = au_element_new(doc, e->tag, (AuNamespace)e->ns);
        for (uint32_t i = 0; i < e->nattrs; i++)
            au_element_append_attr((AuElement *)c, e->attrs[i].name, (AuAttrNamespace)e->attrs[i].ns,
                                   e->attrs[i].value, e->attrs[i].len);
        if (e->template_content)
            ((AuElement *)c)->template_content = au_fragment_new(doc);
        return c;
    }
    case AU_NODE_TEXT:
        return au_text_new(doc, ((const AuText *)n)->data, ((const AuText *)n)->len);
    case AU_NODE_COMMENT:
        return au_comment_new(doc, ((const AuText *)n)->data, ((const AuText *)n)->len);
    case AU_NODE_PROCESSING_INSTRUCTION:
        return au_pi_new(doc, ((const AuProcessingInstruction *)n)->target, ((const AuText *)n)->data,
                         ((const AuText *)n)->len);
    case AU_NODE_DOCTYPE: {
        const AuDoctype *d = (const AuDoctype *)n;
        return au_doctype_new(doc, d->name, d->public_id, d->system_id);
    }
    case AU_NODE_FRAGMENT:
        return au_fragment_new(doc);
    case AU_NODE_DOCUMENT: {
        AuDocument *d = au_document_new();
        d->quirks = ((const AuDocument *)n)->quirks;
        return &d->node;
    }
    }
    return NULL;
}

/* Clones src's subtree under dst without recursion over depth. Template
 * contents met on the way are cloned from a list rather than by a nested
 * call: templates nest in templates as deeply as a page likes. */
typedef struct ClonePair {
    const AuNode *src;
    AuNode *dst;
} ClonePair;

static void clone_children(AuDocument *doc, const AuNode *src, AuNode *dst)
{
    ClonePair *work = NULL;
    size_t nwork = 0, cap = 0;
    for (;;) {
        const AuNode *s = src->first_child;
        AuNode *d = dst;
        while (s) {
            AuNode *c = clone_one(doc, s);
            if (c) {
                au_node_append(d, c);
                au_node_unref(c);
                if (s->type == AU_NODE_ELEMENT && ((const AuElement *)s)->template_content &&
                    ((AuElement *)c)->template_content) {
                    if (nwork == cap) {
                        cap = cap ? cap * 2 : 8;
                        work = au_realloc(work, cap * sizeof *work);
                    }
                    work[nwork].src = ((const AuElement *)s)->template_content;
                    work[nwork].dst = ((AuElement *)c)->template_content;
                    nwork++;
                }
            }
            if (s->first_child && c) {
                s = s->first_child;
                d = c;
                continue;
            }
            while (s && s != src && !s->next) {
                s = s->parent;
                d = d->parent;
            }
            if (!s || s == src)
                break;
            s = s->next;
        }
        if (!nwork)
            break;
        nwork--;
        src = work[nwork].src;
        dst = work[nwork].dst;
    }
    au_free(work);
}

static AuNode *clone_node(AuDocument *doc, const AuNode *n, bool deep)
{
    AuNode *c = clone_one(doc, n);
    if (!c)
        return NULL;
    AuDocument *cdoc = c->type == AU_NODE_DOCUMENT ? (AuDocument *)c : doc;
    if (n->type == AU_NODE_ELEMENT && ((const AuElement *)n)->template_content && deep)
        clone_children(cdoc, ((const AuElement *)n)->template_content, ((AuElement *)c)->template_content);
    if (deep)
        clone_children(cdoc, n, c);
    return c;
}

static JSValue m_clone_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(n);
    if (au_is_shadow_root(n))
        return throw_dom(ctx, "NotSupportedError", "shadow roots cannot be cloned");
    bool deep = argc > 0 && JS_ToBool(ctx, argv[0]);
    AuNode *c = clone_node(n->doc, n, deep);
    if (c && c->type == AU_NODE_DOCUMENT)
        js_keep_doc(js, (AuDocument *)c);
    JSValue r = js_wrap(js, c);
    au_node_unref(c);
    js_ce_flush(js);
    return r;
}

static JSValue m_import_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(d);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!n || d->type != AU_NODE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "importNode: argument is not a Node");
    if (n->type == AU_NODE_DOCUMENT || au_is_shadow_root(n))
        return throw_dom(ctx, "NotSupportedError", "this node cannot be imported");
    bool deep = argc > 1 && JS_ToBool(ctx, argv[1]);
    AuNode *c = clone_node((AuDocument *)d, n, deep);
    JSValue r = js_wrap(js, c);
    au_node_unref(c);
    return r;
}

static JSValue m_adopt_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(d);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!n || d->type != AU_NODE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "adoptNode: argument is not a Node");
    if (n->type == AU_NODE_DOCUMENT || au_is_shadow_root(n))
        return throw_dom(ctx, "NotSupportedError", "this node cannot be adopted");
    if (n->parent)
        au_node_remove(n);
    set_doc_subtree(n, (AuDocument *)d);
    return JS_DupValue(ctx, argv[0]);
}

/* ---- Comparison ---- */

static JSValue m_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(n);
    AuNode *o = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!o)
        return JS_FALSE;
    for (; o; o = o->parent)
        if (o == n)
            return JS_TRUE;
    return JS_FALSE;
}

static int depth_of(const AuNode *n)
{
    int d = 0;
    for (; n->parent; n = n->parent)
        d++;
    return d;
}

static JSValue m_compare_position(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(a);
    AuNode *b = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!b)
        return JS_ThrowTypeError(ctx, "compareDocumentPosition: argument is not a Node");
    if (a == b)
        return JS_NewInt32(ctx, 0);
    enum { DISCONNECTED = 1, PRECEDING = 2, FOLLOWING = 4, CONTAINS = 8, CONTAINED_BY = 16, IMPL = 32 };
    if (au_node_root(a) != au_node_root(b))
        return JS_NewInt32(ctx, DISCONNECTED | IMPL | ((uintptr_t)a < (uintptr_t)b ? PRECEDING : FOLLOWING));
    int da = depth_of(a), db = depth_of(b);
    const AuNode *x = a, *y = b;
    while (da > db) {
        x = x->parent;
        da--;
    }
    while (db > da) {
        y = y->parent;
        db--;
    }
    if (x == b)
        return JS_NewInt32(ctx, CONTAINS | PRECEDING); /* b is an ancestor of a */
    if (y == a)
        return JS_NewInt32(ctx, CONTAINED_BY | FOLLOWING);
    while (x->parent != y->parent) {
        x = x->parent;
        y = y->parent;
    }
    for (const AuNode *k = x->next; k; k = k->next)
        if (k == y)
            return JS_NewInt32(ctx, FOLLOWING); /* b follows a */
    return JS_NewInt32(ctx, PRECEDING);
}

static bool attrs_equal(const AuElement *a, const AuElement *b)
{
    if (a->nattrs != b->nattrs)
        return false;
    for (uint32_t i = 0; i < a->nattrs; i++) {
        const AuAttr *o = au_element_attr(b, a->attrs[i].name);
        if (!o || o->ns != a->attrs[i].ns || o->len != a->attrs[i].len ||
            memcmp(o->value, a->attrs[i].value, o->len) != 0)
            return false;
    }
    return true;
}

static bool node_equal_shallow(const AuNode *a, const AuNode *b)
{
    if (a->type != b->type)
        return false;
    switch (a->type) {
    case AU_NODE_ELEMENT: {
        const AuElement *x = (const AuElement *)a, *y = (const AuElement *)b;
        return x->tag == y->tag && x->ns == y->ns && attrs_equal(x, y);
    }
    case AU_NODE_TEXT:
    case AU_NODE_COMMENT:
    case AU_NODE_PROCESSING_INSTRUCTION: {
        const AuText *x = (const AuText *)a, *y = (const AuText *)b;
        if (a->type == AU_NODE_PROCESSING_INSTRUCTION &&
            ((const AuProcessingInstruction *)a)->target != ((const AuProcessingInstruction *)b)->target)
            return false;
        return x->len == y->len && memcmp(x->data, y->data, x->len) == 0;
    }
    case AU_NODE_DOCTYPE: {
        const AuDoctype *x = (const AuDoctype *)a, *y = (const AuDoctype *)b;
        return !strcmp(x->name, y->name) && !strcmp(x->public_id, y->public_id) && !strcmp(x->system_id, y->system_id);
    }
    }
    return true;
}

static JSValue m_is_equal_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(a);
    AuNode *b = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!b)
        return JS_FALSE;
    const AuNode *x = a, *y = b;
    while (x && y) {
        if (!node_equal_shallow(x, y))
            return JS_FALSE;
        if ((x->first_child == NULL) != (y->first_child == NULL))
            return JS_FALSE;
        const AuNode *nx = au_node_next(x, a), *ny = au_node_next(y, b);
        /* Same shape: the next nodes must sit at the same relative depth. */
        if ((nx == NULL) != (ny == NULL))
            return JS_FALSE;
        if (nx && ny && ((nx->parent == x) != (ny->parent == y)))
            return JS_FALSE;
        x = nx;
        y = ny;
    }
    return JS_TRUE;
}

static JSValue m_is_same_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(a);
    return JS_NewBool(ctx, argc > 0 && arg_node(js, argv[0]) == a);
}

static JSValue m_normalize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(root);
    AuNode *n = root->first_child ? root : NULL;
    for (AuNode *c = n ? root->first_child : NULL; c;) {
        if (c->type == AU_NODE_TEXT) {
            AuText *t = (AuText *)c;
            if (t->len == 0) {
                AuNode *nx = au_node_next_skip(c, root);
                au_node_remove(c);
                c = nx;
                continue;
            }
            while (c->next && c->next->type == AU_NODE_TEXT) {
                AuText *o = (AuText *)c->next;
                au_text_append(t, o->data, o->len);
                au_node_remove(c->next);
            }
        }
        c = au_node_next(c, root);
    }
    return JS_UNDEFINED;
}

/* ---- Element: names and attributes ---- */

static JSValue g_tag_name(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS_EL(e);
    return tag_name(ctx, e);
}

static JSValue g_local_name(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS_EL(e);
    return js_str(ctx, au_atom_str(e->tag), au_atom_len(e->tag));
}

static JSValue g_namespace(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS_EL(e);
    return JS_NewString(ctx, ns_uri[e->ns < 3 ? e->ns : 0]);
}

static AuAtom attr_name_atom(JSContext *ctx, const AuElement *e, JSValueConst v)
{
    return arg_atom(ctx, v, is_html_doc_element(e));
}

static bool valid_name(const char *s, size_t n)
{
    if (!n)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= ' ' || c == '/' || c == '>' || c == '<' || c == '=' || c == '"' || c == '\'' || c == 0x7f)
            return false;
    }
    return true;
}

static JSValue m_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getAttribute: 1 argument required");
    AuAtom a = attr_name_atom(ctx, e, argv[0]);
    if (!a)
        return JS_EXCEPTION;
    const AuAttr *at = au_element_attr(e, a);
    return at ? js_str(ctx, at->value, at->len) : JS_NULL;
}

static JSValue m_set_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setAttribute: 2 arguments required");
    size_t nl;
    const char *ns = JS_ToCStringLen(ctx, &nl, argv[0]);
    if (!ns)
        return JS_EXCEPTION;
    bool ok = valid_name(ns, nl);
    JS_FreeCString(ctx, ns);
    if (!ok)
        return throw_dom(ctx, "InvalidCharacterError", "invalid attribute name");
    AuAtom a = attr_name_atom(ctx, e, argv[0]);
    size_t len;
    char *v = arg_str(ctx, argv[1], &len);
    if (!a || !v)
        return JS_EXCEPTION;
    au_element_set_attr(e, a, v, len);
    au_free(v);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

static JSValue m_remove_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "removeAttribute: 1 argument required");
    AuAtom a = attr_name_atom(ctx, e, argv[0]);
    if (!a)
        return JS_EXCEPTION;
    au_element_remove_attr(e, a);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

static JSValue m_has_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "hasAttribute: 1 argument required");
    AuAtom a = attr_name_atom(ctx, e, argv[0]);
    if (!a)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, au_element_attr(e, a) != NULL);
}

static JSValue m_toggle_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "toggleAttribute: 1 argument required");
    AuAtom a = attr_name_atom(ctx, e, argv[0]);
    if (!a)
        return JS_EXCEPTION;
    if (!valid_name(au_atom_str(a), au_atom_len(a)))
        return throw_dom(ctx, "InvalidCharacterError", "invalid attribute name");
    bool has = au_element_attr(e, a) != NULL;
    bool want = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : !has;
    if (want && !has)
        au_element_set_attr(e, a, "", 0);
    else if (!want && has)
        au_element_remove_attr(e, a);
    js_ce_flush(js);
    return JS_NewBool(ctx, want);
}

static JSValue m_get_attribute_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < e->nattrs; i++)
        JS_SetPropertyUint32(ctx, arr, i, js_str(ctx, au_atom_str(e->attrs[i].name), au_atom_len(e->attrs[i].name)));
    return arr;
}

static JSValue m_has_attributes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    return JS_NewBool(ctx, e->nattrs > 0);
}

/* Attribute i as [name, value, namespace] for the prelude's NamedNodeMap. */
static JSValue n_attr_at(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 1 ? arg_node(js, argv[0]) : NULL;
    uint32_t i;
    if (!n || n->type != AU_NODE_ELEMENT || JS_ToUint32(ctx, &i, argv[1]))
        return JS_NULL;
    AuElement *e = (AuElement *)n;
    if (i >= e->nattrs)
        return JS_NULL;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, js_str(ctx, au_atom_str(e->attrs[i].name), au_atom_len(e->attrs[i].name)));
    JS_SetPropertyUint32(ctx, arr, 1, js_str(ctx, e->attrs[i].value, e->attrs[i].len));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewInt32(ctx, e->attrs[i].ns));
    return arr;
}

static JSValue n_attr_count(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!n || n->type != AU_NODE_ELEMENT)
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, (int32_t)((AuElement *)n)->nattrs);
}

/* setAttributeNS(ns, qualifiedName, value): the namespace is kept for the
 * xlink/xml/xmlns ones Aura models; others are stored without one. */
static JSValue m_set_attribute_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "setAttributeNS: 3 arguments required");
    AuAttrNamespace ans = AU_ATTR_NS_NONE;
    if (!JS_IsNull(argv[0])) {
        const char *u = JS_ToCString(ctx, argv[0]);
        if (!u)
            return JS_EXCEPTION;
        if (!strcmp(u, "http://www.w3.org/1999/xlink"))
            ans = AU_ATTR_NS_XLINK;
        else if (!strcmp(u, "http://www.w3.org/XML/1998/namespace"))
            ans = AU_ATTR_NS_XML;
        else if (!strcmp(u, "http://www.w3.org/2000/xmlns/"))
            ans = AU_ATTR_NS_XMLNS;
        JS_FreeCString(ctx, u);
    }
    AuAtom a = arg_atom(ctx, argv[1], false);
    size_t len;
    char *v = arg_str(ctx, argv[2], &len);
    if (!a || !v)
        return JS_EXCEPTION;
    if (!valid_name(au_atom_str(a), au_atom_len(a))) {
        au_free(v);
        return throw_dom(ctx, "InvalidCharacterError", "invalid attribute name");
    }
    au_element_set_attr_ns(e, a, ans, v, len);
    au_free(v);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

/* getAttributeNS / removeAttributeNS / hasAttributeNS (magic 0, 1, 2) by
 * local name: the qualified name with any prefix. */
static const AuAttr *find_attr_ns(const AuElement *e, JSContext *ctx, JSValueConst local)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, local);
    if (!s)
        return NULL;
    const AuAttr *r = NULL;
    for (uint32_t i = 0; i < e->nattrs && !r; i++) {
        const char *q = au_atom_str(e->attrs[i].name);
        const char *colon = strchr(q, ':');
        const char *ln = colon && e->attrs[i].ns != AU_ATTR_NS_NONE ? colon + 1 : q;
        if (strlen(ln) == n && memcmp(ln, s, n) == 0)
            r = &e->attrs[i];
    }
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue m_attr_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSTHIS_EL(e);
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "2 arguments required");
    const AuAttr *a = find_attr_ns(e, ctx, argv[1]);
    switch (magic) {
    case 0:
        return a ? js_str(ctx, a->value, a->len) : JS_NULL;
    case 1:
        if (a)
            au_element_remove_attr(e, a->name);
        js_ce_flush(js);
        return JS_UNDEFINED;
    default:
        return JS_NewBool(ctx, a != NULL);
    }
}

/* Reflected string attributes: id, className (class), slot. */
static JSValue g_reflect(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSTHIS_EL(e);
    AuAtom a = magic == 0 ? AU_ATOM_id : magic == 1 ? AU_ATOM_class : AU_ATOM_slot;
    const AuAttr *at = au_element_attr(e, a);
    return at ? js_str(ctx, at->value, at->len) : JS_NewString(ctx, "");
}

static JSValue s_reflect(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSTHIS_EL(e);
    AuAtom a = magic == 0 ? AU_ATOM_id : magic == 1 ? AU_ATOM_class : AU_ATOM_slot;
    size_t len;
    char *v = arg_str(ctx, val, &len);
    if (!v)
        return JS_EXCEPTION;
    au_element_set_attr(e, a, v, len);
    au_free(v);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

/* ---- Selectors ---- */

static AuCssSelectorList *arg_selector(JSContext *ctx, JSValueConst v)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return NULL;
    AuCssSelectorList *l = au_css_selector_parse(s, n);
    if (!l)
        JS_ThrowDOMException(ctx, "SyntaxError", "'%s' is not a valid selector", s);
    JS_FreeCString(ctx, s);
    return l;
}

static JSValue m_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "matches: 1 argument required");
    AuCssSelectorList *l = arg_selector(ctx, argv[0]);
    if (!l)
        return JS_EXCEPTION;
    bool m = au_css_selector_matches(l, e);
    au_css_selector_free(l);
    return JS_NewBool(ctx, m);
}

static JSValue m_closest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "closest: 1 argument required");
    AuCssSelectorList *l = arg_selector(ctx, argv[0]);
    if (!l)
        return JS_EXCEPTION;
    AuNode *r = NULL;
    for (AuNode *p = &e->node; p && p->type == AU_NODE_ELEMENT; p = p->parent)
        if (au_css_selector_matches_scoped(l, (AuElement *)p, &e->node)) {
            r = p;
            break;
        }
    au_css_selector_free(l);
    return js_wrap(js, r);
}

static JSValue m_query_selector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(n);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "querySelector: 1 argument required");
    AuCssSelectorList *l = arg_selector(ctx, argv[0]);
    if (!l)
        return JS_EXCEPTION;
    AuElement *r = au_css_query_selector(l, n);
    au_css_selector_free(l);
    return js_wrap(js, r ? &r->node : NULL);
}

/* Matching elements as a plain array (the prelude makes a static NodeList).
 * Nodes are referenced while wrappers are made, since making a wrapper can
 * run the garbage collector. */
static JSValue m_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(root);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "querySelectorAll: 1 argument required");
    AuCssSelectorList *l = arg_selector(ctx, argv[0]);
    if (!l)
        return JS_EXCEPTION;
    AuNode **found = NULL;
    uint32_t nf = 0, cap = 0;
    for (AuNode *c = root->first_child; c; c = au_node_next(c, root))
        if (c->type == AU_NODE_ELEMENT && au_css_selector_matches_scoped(l, (AuElement *)c, root)) {
            if (nf == cap) {
                cap = cap ? cap * 2 : 16;
                found = au_realloc(found, cap * sizeof *found);
            }
            found[nf++] = au_node_ref(c);
        }
    au_css_selector_free(l);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < nf; i++) {
        JS_SetPropertyUint32(ctx, arr, i, js_wrap(js, found[i]));
        au_node_unref(found[i]);
    }
    au_free(found);
    return arr;
}

/* ---- Collections ----
 * collect(root, kind, arg): the members of a live collection now, as an
 * array. kind: 0 element children, 1 child nodes, 2 by qualified tag name
 * ("*" for all), 3 by class names, 4 by name attribute (document-wide),
 * 5 descendants that are form controls, 6 option descendants, 7 table
 * rows, 8 cells of a row, 9 tBodies, 10 images, 11 links (a/area with
 * href), 12 forms, 13 scripts, 14 anchors (a with name). */
static bool is_form_control(const AuElement *e)
{
    if (e->ns != AU_NS_HTML)
        return false;
    AuAtom t = e->tag;
    return t == AU_ATOM_button || t == AU_ATOM_fieldset || t == AU_ATOM_input || t == AU_ATOM_object ||
           t == AU_ATOM_output || t == AU_ATOM_select || t == AU_ATOM_textarea;
}

static JSValue n_collect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *root = argc > 1 ? arg_node(js, argv[0]) : NULL;
    int32_t kind;
    if (!root || JS_ToInt32(ctx, &kind, argv[1]))
        return JS_NewArray(ctx);
    AuAtom tag = 0, tag_lower = 0;
    AuAtom *classes = NULL;
    uint32_t nclasses = 0;
    char *name = NULL;
    bool all = false;
    if (kind == 2 && argc > 2) {
        tag = arg_atom(ctx, argv[2], false);
        tag_lower = arg_atom(ctx, argv[2], true);
        all = tag == au_atom("*", 1);
    } else if (kind == 3 && argc > 2) {
        size_t n;
        char *s = arg_str(ctx, argv[2], &n);
        for (size_t i = 0; s && i < n;) {
            while (i < n && au_is_space((unsigned char)s[i]))
                i++;
            size_t st = i;
            while (i < n && !au_is_space((unsigned char)s[i]))
                i++;
            if (i > st && nclasses < 32) {
                classes = au_realloc(classes, (nclasses + 1) * sizeof *classes);
                classes[nclasses++] = au_atom(s + st, i - st);
            }
        }
        au_free(s);
    } else if (kind == 4 && argc > 2) {
        name = arg_str(ctx, argv[2], NULL);
    }
    AuNode **found = NULL;
    uint32_t nf = 0, cap = 0;
#define ADD(x)                                                                                                       \
    do {                                                                                                             \
        if (nf == cap) {                                                                                             \
            cap = cap ? cap * 2 : 16;                                                                                \
            found = au_realloc(found, cap * sizeof *found);                                                          \
        }                                                                                                            \
        found[nf++] = au_node_ref(x);                                                                                \
    } while (0)
    if (kind == 0 || kind == 1 || kind == 8) {
        for (AuNode *c = root->first_child; c; c = c->next)
            if (kind == 1 || (c->type == AU_NODE_ELEMENT &&
                              (kind == 0 || au_is_html_element(c, AU_ATOM_td) || au_is_html_element(c, AU_ATOM_th))))
                ADD(c);
    } else if (kind == 7) {
        /* rows of a table: its own tr children and those of its row groups */
        for (AuNode *c = root->first_child; c; c = c->next) {
            if (au_is_html_element(c, AU_ATOM_tr))
                ADD(c);
            else if (au_is_html_element(c, AU_ATOM_thead) || au_is_html_element(c, AU_ATOM_tbody) ||
                     au_is_html_element(c, AU_ATOM_tfoot))
                for (AuNode *r = c->first_child; r; r = r->next)
                    if (au_is_html_element(r, AU_ATOM_tr))
                        ADD(r);
        }
    } else if (kind == 9) {
        for (AuNode *c = root->first_child; c; c = c->next)
            if (au_is_html_element(c, AU_ATOM_tbody))
                ADD(c);
    } else {
        AuNode *scope = kind == 4 ? &root->doc->node : root;
        AuAtom aname = au_atom("name", 4), ahref = au_atom("href", 4);
        for (AuNode *c = scope->first_child; c; c = au_node_next(c, scope)) {
            if (c->type != AU_NODE_ELEMENT)
                continue;
            AuElement *e = (AuElement *)c;
            bool ok = false;
            switch (kind) {
            case 2:
                ok = all || (e->ns == AU_NS_HTML ? e->tag == tag_lower : e->tag == tag);
                break;
            case 3:
                ok = nclasses > 0;
                for (uint32_t i = 0; i < nclasses && ok; i++)
                    ok = au_element_has_class(e, classes[i]);
                break;
            case 4: {
                const char *v = au_element_get_attr(e, aname);
                ok = v && name && e->ns == AU_NS_HTML && strcmp(v, name) == 0;
                break;
            }
            case 5:
                ok = is_form_control(e) && !(e->tag == AU_ATOM_input && au_element_get_attr(e, au_atom("type", 4)) &&
                                              au_ieq(au_element_get_attr(e, au_atom("type", 4)),
                                                     strlen(au_element_get_attr(e, au_atom("type", 4))), "image", 5));
                break;
            case 6:
                ok = au_is_html_element(c, AU_ATOM_option);
                break;
            case 10:
                ok = au_is_html_element(c, AU_ATOM_img);
                break;
            case 11:
                ok = (au_is_html_element(c, AU_ATOM_a) || au_is_html_element(c, AU_ATOM_area)) &&
                     au_element_attr(e, ahref);
                break;
            case 12:
                ok = au_is_html_element(c, AU_ATOM_form);
                break;
            case 13:
                ok = au_is_html_element(c, AU_ATOM_script);
                break;
            case 14:
                ok = au_is_html_element(c, AU_ATOM_a) && au_element_attr(e, aname);
                break;
            }
            if (ok)
                ADD(c);
        }
    }
#undef ADD
    au_free(classes);
    au_free(name);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < nf; i++) {
        JS_SetPropertyUint32(ctx, arr, i, js_wrap(js, found[i]));
        au_node_unref(found[i]);
    }
    au_free(found);
    return arr;
}

static JSValue n_version(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    AuDocument *d = n && n->doc ? n->doc : js->doc;
    return JS_NewUint32(ctx, d->version);
}

/* ---- Document ---- */

static bool valid_element_name(const char *s, size_t n)
{
    if (!n || !au_is_alpha((unsigned char)s[0]))
        return n && ((unsigned char)s[0] >= 0x80 || s[0] == '_' || s[0] == ':') && valid_name(s, n);
    return valid_name(s, n);
}

static JSValue m_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(d);
    if (d->type != AU_NODE_DOCUMENT || argc < 1)
        return JS_ThrowTypeError(ctx, "createElement: 1 argument required");
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    bool ok = valid_element_name(s, n);
    JS_FreeCString(ctx, s);
    if (!ok)
        return throw_dom(ctx, "InvalidCharacterError", "invalid element name");
    AuAtom tag = arg_atom(ctx, argv[0], true);
    AuNode *e = au_element_new((AuDocument *)d, tag, AU_NS_HTML);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue is = JS_GetPropertyStr(ctx, argv[1], "is");
        if (JS_IsString(is)) {
            size_t il;
            char *iv = arg_str(ctx, is, &il);
            if (iv)
                au_element_set_attr((AuElement *)e, au_atom("is", 2), iv, il);
            au_free(iv);
        }
        JS_FreeValue(ctx, is);
    }
    JSValue r = js_wrap(js, e);
    au_node_unref(e);
    return r;
}

static JSValue m_create_element_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(d);
    if (d->type != AU_NODE_DOCUMENT || argc < 2)
        return JS_ThrowTypeError(ctx, "createElementNS: 2 arguments required");
    AuNamespace ns = AU_NS_HTML;
    bool known = true;
    if (!JS_IsNull(argv[0])) {
        const char *u = JS_ToCString(ctx, argv[0]);
        if (!u)
            return JS_EXCEPTION;
        if (!strcmp(u, ns_uri[1]))
            ns = AU_NS_SVG;
        else if (!strcmp(u, ns_uri[2]))
            ns = AU_NS_MATHML;
        else if (strcmp(u, ns_uri[0]) != 0)
            known = false;
        JS_FreeCString(ctx, u);
    } else {
        known = false;
    }
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!s)
        return JS_EXCEPTION;
    bool ok = valid_element_name(s, n);
    const char *colon = memchr(s, ':', n);
    AuAtom tag = colon ? au_atom(colon + 1, n - (size_t)(colon + 1 - s)) : au_atom(s, n);
    JS_FreeCString(ctx, s);
    if (!ok)
        return throw_dom(ctx, "InvalidCharacterError", "invalid element name");
    /* Elements in other namespaces are modelled as SVG-like foreign
     * elements (no HTML behaviour). */
    AuNode *e = au_element_new((AuDocument *)d, tag, known ? ns : AU_NS_SVG);
    JSValue r = js_wrap(js, e);
    au_node_unref(e);
    return r;
}

static JSValue m_create_chardata(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSTHIS(d);
    if (d->type != AU_NODE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    size_t len = 0;
    char *s = argc > 0 ? arg_str(ctx, argv[0], &len) : au_strndup("", 0);
    if (!s)
        return JS_EXCEPTION;
    AuNode *n;
    if (magic == 0)
        n = au_text_new((AuDocument *)d, s, len);
    else if (magic == 1)
        n = au_comment_new((AuDocument *)d, s, len);
    else if (magic == 2)
        n = au_fragment_new((AuDocument *)d);
    else {
        size_t dl = 0;
        char *data = argc > 1 ? arg_str(ctx, argv[1], &dl) : au_strndup("", 0);
        if (!data) {
            au_free(s);
            return JS_EXCEPTION;
        }
        if (strstr(data, "?>") || !valid_name(s, len)) {
            au_free(s);
            au_free(data);
            return throw_dom(ctx, "InvalidCharacterError", "invalid processing instruction");
        }
        n = au_pi_new((AuDocument *)d, au_atom(s, len), data, dl);
        au_free(data);
    }
    au_free(s);
    JSValue r = js_wrap(js, n);
    au_node_unref(n);
    return r;
}

static AuElement *find_by_id(AuNode *root, AuAtom id)
{
    for (AuNode *c = root->first_child; c; c = au_node_next(c, root))
        if (c->type == AU_NODE_ELEMENT && ((AuElement *)c)->id == id)
            return (AuElement *)c;
    return NULL;
}

static JSValue m_get_element_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS(root);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getElementById: 1 argument required");
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    AuAtom id = n ? au_atom_lookup(s, n) : 0;
    JS_FreeCString(ctx, s);
    if (!id)
        return JS_NULL;
    AuElement *e = find_by_id(root, id);
    return js_wrap(js, e ? &e->node : NULL);
}

enum { D_ELEMENT, D_HEAD, D_BODY, D_DOCTYPE };

static JSValue g_doc_part(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSTHIS(d);
    if (d->type != AU_NODE_DOCUMENT)
        return JS_NULL;
    AuDocument *doc = (AuDocument *)d;
    AuElement *e = NULL;
    switch (magic) {
    case D_ELEMENT:
        e = au_document_element(doc);
        break;
    case D_HEAD:
        e = au_document_head(doc);
        break;
    case D_BODY: {
        AuElement *root = au_document_element(doc);
        if (root && au_is_html_element(&root->node, AU_ATOM_html))
            for (AuNode *c = root->node.first_child; c; c = c->next)
                if (au_is_html_element(c, AU_ATOM_body) || au_is_html_element(c, AU_ATOM_frameset)) {
                    e = (AuElement *)c;
                    break;
                }
        break;
    }
    case D_DOCTYPE:
        for (AuNode *c = d->first_child; c; c = c->next)
            if (c->type == AU_NODE_DOCTYPE)
                return js_wrap(js, c);
        return JS_NULL;
    }
    return js_wrap(js, e ? &e->node : NULL);
}

static JSValue s_body(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSTHIS(d);
    AuNode *b = arg_node(js, val);
    if (d->type != AU_NODE_DOCUMENT || !b || !(au_is_html_element(b, AU_ATOM_body) || au_is_html_element(b, AU_ATOM_frameset)))
        return throw_dom(ctx, "HierarchyRequestError", "body must be a body or frameset element");
    AuElement *root = au_document_element((AuDocument *)d);
    if (!root)
        return throw_dom(ctx, "HierarchyRequestError", "no document element");
    AuNode *old = NULL;
    for (AuNode *c = root->node.first_child; c; c = c->next)
        if (au_is_html_element(c, AU_ATOM_body) || au_is_html_element(c, AU_ATOM_frameset)) {
            old = c;
            break;
        }
    if (old == b)
        return JS_UNDEFINED;
    au_node_ref(b);
    if (old) {
        AuNode *ref = old->next;
        if (ref)
            au_node_ref(ref);
        au_node_remove(old);
        insert_node(&root->node, b, ref);
        au_node_unref(ref);
    } else {
        insert_node(&root->node, b, NULL);
    }
    au_node_unref(b);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

static JSValue g_doc_url(JSContext *ctx, JSValueConst this_val)
{
    AuJs *js = js_of_ctx(ctx);
    return JS_NewString(ctx, js->url);
}

static JSValue g_compat_mode(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(d);
    return JS_NewString(ctx, d->type == AU_NODE_DOCUMENT && ((AuDocument *)d)->quirks == AU_QUIRKS_FULL
                                 ? "BackCompat"
                                 : "CSS1Compat");
}

/* document.title: the first <title> (or SVG title) text, whitespace
 * collapsed; setting replaces its text or creates it in <head>. */
static AuElement *title_element(AuDocument *d)
{
    for (AuNode *c = d->node.first_child; c; c = au_node_next(c, &d->node))
        if (au_is_html_element(c, AU_ATOM_title))
            return (AuElement *)c;
    return NULL;
}

static JSValue g_title(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(d);
    if (d->type != AU_NODE_DOCUMENT)
        return JS_NewString(ctx, "");
    AuElement *t = title_element((AuDocument *)d);
    if (!t)
        return JS_NewString(ctx, "");
    AuBuf b, o;
    au_buf_init(&b);
    au_buf_init(&o);
    for (AuNode *c = t->node.first_child; c; c = c->next)
        if (c->type == AU_NODE_TEXT)
            au_buf_append(&b, ((AuText *)c)->data, ((AuText *)c)->len);
    bool sp = true;
    for (size_t i = 0; i < b.len; i++) {
        if (au_is_space((unsigned char)b.data[i])) {
            if (!sp)
                au_buf_putc(&o, ' ');
            sp = true;
        } else {
            au_buf_putc(&o, b.data[i]);
            sp = false;
        }
    }
    while (o.len && o.data[o.len - 1] == ' ')
        o.len--;
    JSValue r = js_str(ctx, o.data, o.len);
    au_buf_free(&b);
    au_buf_free(&o);
    return r;
}

static JSValue s_title(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    JSTHIS(d);
    if (d->type != AU_NODE_DOCUMENT)
        return JS_UNDEFINED;
    size_t len;
    char *s = arg_str(ctx, val, &len);
    if (!s)
        return JS_EXCEPTION;
    AuDocument *doc = (AuDocument *)d;
    AuElement *t = title_element(doc);
    if (!t) {
        AuElement *head = au_document_head(doc);
        if (!head) {
            au_free(s);
            return JS_UNDEFINED;
        }
        AuNode *te = au_element_new(doc, AU_ATOM_title, AU_NS_HTML);
        au_node_append(&head->node, te);
        au_node_unref(te);
        t = (AuElement *)te;
    }
    remove_all_children(&t->node);
    if (len) {
        AuNode *tx = au_text_new(doc, s, len);
        au_node_append(&t->node, tx);
        au_node_unref(tx);
    }
    au_free(s);
    return JS_UNDEFINED;
}

/* ---- CharacterData ---- */

static JSValue g_data(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    if (!is_chardata(n))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    return js_str(ctx, ((AuText *)n)->data, ((AuText *)n)->len);
}

static JSValue s_data(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    JSTHIS(n);
    if (!is_chardata(n))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    size_t len = 0;
    char *s = JS_IsNull(val) ? au_strndup("", 0) : arg_str(ctx, val, &len);
    if (!s)
        return JS_EXCEPTION;
    au_text_set((AuText *)n, s, len);
    au_free(s);
    return JS_UNDEFINED;
}

static JSValue g_doctype_part(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSTHIS(n);
    if (n->type != AU_NODE_DOCTYPE)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    AuDoctype *d = (AuDoctype *)n;
    return JS_NewString(ctx, magic == 0 ? d->name : magic == 1 ? d->public_id : d->system_id);
}

static JSValue g_pi_target(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    if (n->type != AU_NODE_PROCESSING_INSTRUCTION)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    AuAtom t = ((AuProcessingInstruction *)n)->target;
    return js_str(ctx, au_atom_str(t), au_atom_len(t));
}

/* ---- HTML parsing and serialization ---- */

static const char *const void_tags[] = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr",
                                        "img", "input", "keygen", "link", "meta", "param", "source", "track", "wbr"};

static bool is_void_el(const AuElement *e)
{
    if (e->ns != AU_NS_HTML)
        return false;
    const char *s = au_atom_str(e->tag);
    for (size_t i = 0; i < JS_COUNTOF(void_tags); i++)
        if (!strcmp(s, void_tags[i]))
            return true;
    return false;
}

static void attr_escape(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '&')
            au_buf_puts(out, "&amp;");
        else if (c == '"')
            au_buf_puts(out, "&quot;");
        else if (c == '<')
            au_buf_puts(out, "&lt;");
        else if (c == '>')
            au_buf_puts(out, "&gt;");
        else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) {
            au_buf_puts(out, "&nbsp;");
            i++;
        } else
            au_buf_putc(out, (char)c);
    }
}

static void serialize_outer(const AuNode *n, AuBuf *out)
{
    if (n->type != AU_NODE_ELEMENT) {
        /* A text or comment child: serialize it as its parent would. */
        if (n->type == AU_NODE_TEXT)
            attr_escape(out, ((const AuText *)n)->data, ((const AuText *)n)->len);
        else if (n->type == AU_NODE_COMMENT) {
            au_buf_puts(out, "<!--");
            au_buf_append(out, ((const AuText *)n)->data, ((const AuText *)n)->len);
            au_buf_puts(out, "-->");
        }
        return;
    }
    const AuElement *e = (const AuElement *)n;
    au_buf_putc(out, '<');
    au_buf_append(out, au_atom_str(e->tag), au_atom_len(e->tag));
    for (uint32_t i = 0; i < e->nattrs; i++) {
        au_buf_putc(out, ' ');
        au_buf_append(out, au_atom_str(e->attrs[i].name), au_atom_len(e->attrs[i].name));
        au_buf_puts(out, "=\"");
        attr_escape(out, e->attrs[i].value, e->attrs[i].len);
        au_buf_putc(out, '"');
    }
    au_buf_putc(out, '>');
    if (is_void_el(e))
        return;
    au_html_serialize(n, out, true);
    au_buf_puts(out, "</");
    au_buf_append(out, au_atom_str(e->tag), au_atom_len(e->tag));
    au_buf_putc(out, '>');
}

static JSValue g_inner_html(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    AuBuf b;
    au_buf_init(&b);
    au_html_serialize(n, &b, true);
    JSValue r = js_str(ctx, b.data, b.len);
    au_buf_free(&b);
    return r;
}

static JSValue g_outer_html(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS_EL(e);
    AuBuf b;
    au_buf_init(&b);
    serialize_outer(&e->node, &b);
    JSValue r = js_str(ctx, b.data, b.len);
    au_buf_free(&b);
    return r;
}

/* Parses html with context as the context element; returns a fragment
 * (owned by the caller). */
static AuNode *parse_fragment(AuElement *context, const char *html, size_t len)
{
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.scripting = true;
    o.encoding = AU_HTML_ENC_UTF8;
    return au_html_parse_fragment(context, html, len, &o);
}

/* Moves the fragment's children before ref in parent (ref NULL: append). */
static void splice_fragment(AuNode *parent, AuNode *frag, AuNode *ref)
{
    while (frag->first_child) {
        AuNode *k = frag->first_child;
        au_node_insert_before(parent, k, ref);
    }
}

static JSValue s_inner_html(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    JSTHIS(n);
    AuElement *context;
    AuNode *target = n;
    AuNode *tmp = NULL;
    if (n->type == AU_NODE_ELEMENT) {
        context = (AuElement *)n;
        if (au_is_html_element(n, AU_ATOM_template)) {
            if (!context->template_content)
                context->template_content = au_fragment_new(n->doc);
            target = context->template_content;
        }
    } else if (n->type == AU_NODE_FRAGMENT) {
        /* ShadowRoot.innerHTML: parse with a <body> context. */
        tmp = au_element_new(n->doc, AU_ATOM_body, AU_NS_HTML);
        context = (AuElement *)tmp;
    } else {
        return JS_UNDEFINED;
    }
    size_t len = 0;
    char *s = JS_IsNull(val) ? au_strndup("", 0) : arg_str(ctx, val, &len);
    if (!s) {
        au_node_unref(tmp);
        return JS_EXCEPTION;
    }
    AuNode *frag = parse_fragment(context, s, len);
    au_free(s);
    au_node_unref(tmp);
    au_node_ref(target);
    remove_all_children(target);
    if (frag) {
        set_doc_subtree(frag, target->doc);
        splice_fragment(target, frag, NULL);
        au_node_unref(frag);
    }
    au_node_unref(target);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

static JSValue s_outer_html(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    JSTHIS_EL(e);
    AuNode *parent = e->node.parent;
    if (!parent)
        return JS_UNDEFINED;
    if (parent->type == AU_NODE_DOCUMENT)
        return throw_dom(ctx, "NoModificationAllowedError", "cannot replace the document element");
    AuNode *tmp = NULL;
    AuElement *context = parent->type == AU_NODE_ELEMENT ? (AuElement *)parent : NULL;
    if (!context) {
        tmp = au_element_new(e->node.doc, AU_ATOM_body, AU_NS_HTML);
        context = (AuElement *)tmp;
    }
    size_t len;
    char *s = arg_str(ctx, val, &len);
    if (!s) {
        au_node_unref(tmp);
        return JS_EXCEPTION;
    }
    AuNode *frag = parse_fragment(context, s, len);
    au_free(s);
    au_node_unref(tmp);
    au_node_ref(parent);
    AuNode *ref = e->node.next;
    if (ref)
        au_node_ref(ref);
    au_node_remove(&e->node); /* this_val's wrapper keeps it */
    if (frag) {
        set_doc_subtree(frag, parent->doc);
        splice_fragment(parent, frag, ref);
        au_node_unref(frag);
    }
    au_node_unref(ref);
    au_node_unref(parent);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

/* insertAdjacentHTML(position, html) */
static int position_of(JSContext *ctx, JSValueConst v)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -2;
    int r = -1;
    if (au_ieq(s, n, "beforebegin", 11))
        r = 0;
    else if (au_ieq(s, n, "afterbegin", 10))
        r = 1;
    else if (au_ieq(s, n, "beforeend", 9))
        r = 2;
    else if (au_ieq(s, n, "afterend", 8))
        r = 3;
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue m_insert_adjacent_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "insertAdjacentHTML: 2 arguments required");
    int pos = position_of(ctx, argv[0]);
    if (pos == -2)
        return JS_EXCEPTION;
    if (pos < 0)
        return throw_dom(ctx, "SyntaxError", "invalid position");
    AuNode *parent = (pos == 0 || pos == 3) ? e->node.parent : &e->node;
    if (!parent || parent->type == AU_NODE_DOCUMENT)
        return throw_dom(ctx, "NoModificationAllowedError", "no parent element");
    AuNode *tmp = NULL;
    AuElement *context = parent->type == AU_NODE_ELEMENT ? (AuElement *)parent : NULL;
    if (!context) {
        tmp = au_element_new(e->node.doc, AU_ATOM_body, AU_NS_HTML);
        context = (AuElement *)tmp;
    }
    size_t len;
    char *s = arg_str(ctx, argv[1], &len);
    if (!s) {
        au_node_unref(tmp);
        return JS_EXCEPTION;
    }
    AuNode *frag = parse_fragment(context, s, len);
    au_free(s);
    au_node_unref(tmp);
    if (!frag)
        return JS_UNDEFINED;
    set_doc_subtree(frag, parent->doc);
    AuNode *ref = pos == 0 ? &e->node : pos == 1 ? e->node.first_child : pos == 2 ? NULL : e->node.next;
    au_node_ref(parent);
    if (ref)
        au_node_ref(ref);
    splice_fragment(parent, frag, ref);
    au_node_unref(ref);
    au_node_unref(parent);
    au_node_unref(frag);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

/* document.write(text): magic 1 adds a newline (writeln). */
static JSValue m_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSTHIS(d);
    if (d->type != AU_NODE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    AuBuf b;
    au_buf_init(&b);
    for (int i = 0; i < argc; i++) {
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, argv[i]);
        if (!s) {
            au_buf_free(&b);
            return JS_EXCEPTION;
        }
        au_buf_append(&b, s, n);
        JS_FreeCString(ctx, s);
    }
    if (magic)
        au_buf_putc(&b, '\n');
    if ((AuDocument *)d != js->doc) {
        au_buf_free(&b);
        return JS_UNDEFINED;
    }
    bool ok = js->parser && au_html_parser_write(js->parser, b.data ? b.data : "", b.len);
    if (!ok && js->hooks.parser && (!js->parser || au_html_parser_done(js->parser))) {
        /* document.open: the embedder replaces the document's contents
         * with a fresh parse (minimal: no new window, no history entry). */
        remove_all_children(d);
        AuHtmlParser *p = js->hooks.parser(js->hooks.ctx);
        if (p) {
            js->parser = p;
            au_html_parser_write(p, b.data ? b.data : "", b.len);
        }
    }
    au_buf_free(&b);
    js_ce_flush(js);
    return JS_UNDEFINED;
}

/* ---- Shadow DOM ---- */

static bool valid_shadow_host(const AuElement *e)
{
    if (e->ns != AU_NS_HTML)
        return false;
    static const char *const ok[] = {"article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3",
                                     "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span"};
    const char *s = au_atom_str(e->tag);
    if (strchr(s, '-'))
        return true;
    for (size_t i = 0; i < JS_COUNTOF(ok); i++)
        if (!strcmp(s, ok[i]))
            return true;
    return false;
}

static JSValue m_attach_shadow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSTHIS_EL(e);
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "attachShadow: an options object is required");
    if (!valid_shadow_host(e))
        return throw_dom(ctx, "NotSupportedError", "this element cannot have a shadow root");
    JSValue m = JS_GetPropertyStr(ctx, argv[0], "mode");
    const char *ms = JS_ToCString(ctx, m);
    JS_FreeValue(ctx, m);
    if (!ms)
        return JS_EXCEPTION;
    int mode = !strcmp(ms, "open") ? AU_SHADOW_OPEN : !strcmp(ms, "closed") ? AU_SHADOW_CLOSED : -1;
    JS_FreeCString(ctx, ms);
    if (mode < 0)
        return JS_ThrowTypeError(ctx, "attachShadow: mode must be 'open' or 'closed'");
    if (e->shadow_root) {
        AuShadowRoot *old = (AuShadowRoot *)e->shadow_root;
        if (old->declarative && old->mode == mode) {
            /* A declarative root is emptied and returned. */
            remove_all_children(&old->node);
            old->declarative = 0;
            return js_wrap(js, &old->node);
        }
        return throw_dom(ctx, "NotSupportedError", "the element already has a shadow root");
    }
    AuShadowRoot *sr = au_element_attach_shadow(e, (AuShadowMode)mode);
    JSValue v;
    v = JS_GetPropertyStr(ctx, argv[0], "delegatesFocus");
    sr->delegates_focus = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "clonable");
    sr->clonable = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "serializable");
    sr->serializable = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "slotAssignment");
    if (JS_IsString(v)) {
        const char *sa = JS_ToCString(ctx, v);
        sr->manual_slots = sa && !strcmp(sa, "manual");
        JS_FreeCString(ctx, sa);
    }
    JS_FreeValue(ctx, v);
    if (js->hooks.dom_changed)
        js->hooks.dom_changed(js->hooks.ctx, &e->node, 0, AU_JS_CHANGE_CHILDREN);
    return js_wrap(js, &sr->node);
}

static JSValue g_shadow_root(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS_EL(e);
    AuShadowRoot *sr = au_element_shadow_root(e);
    return sr && sr->mode == AU_SHADOW_OPEN ? js_wrap(js, &sr->node) : JS_NULL;
}

/* shadowRootOf(el): including closed roots, for the prelude. */
static JSValue n_shadow_root_of(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!n || n->type != AU_NODE_ELEMENT || !((AuElement *)n)->shadow_root)
        return JS_NULL;
    return js_wrap(js, ((AuElement *)n)->shadow_root);
}

static JSValue g_sr_host(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    if (!au_is_shadow_root(n))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    AuElement *h = ((AuShadowRoot *)n)->host;
    return js_wrap(js, h ? &h->node : NULL);
}

static JSValue g_sr_prop(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSTHIS(n);
    if (!au_is_shadow_root(n))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    AuShadowRoot *sr = (AuShadowRoot *)n;
    switch (magic) {
    case 0:
        return JS_NewString(ctx, sr->mode == AU_SHADOW_OPEN ? "open" : "closed");
    case 1:
        return JS_NewBool(ctx, sr->delegates_focus);
    case 2:
        return JS_NewString(ctx, sr->manual_slots ? "manual" : "named");
    case 3:
        return JS_NewBool(ctx, sr->clonable);
    default:
        return JS_NewBool(ctx, sr->serializable);
    }
}

static JSValue g_assigned_slot(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS(n);
    AuElement *s = au_node_assigned_slot(n);
    if (s) {
        AuShadowRoot *sr = (AuShadowRoot *)au_node_root(&s->node);
        if (sr->mode != AU_SHADOW_OPEN)
            s = NULL;
    }
    return js_wrap(js, s ? &s->node : NULL);
}

/* assigned(slot, flatten): the assigned nodes as an array. */
static JSValue n_assigned(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    JSValue arr = JS_NewArray(ctx);
    if (!n || !au_is_html_element(n, AU_ATOM_slot))
        return arr;
    size_t cnt = au_slot_assigned_nodes((AuElement *)n, NULL, 0);
    if (!cnt)
        return arr;
    AuNode **v = au_alloc(cnt * sizeof *v);
    au_slot_assigned_nodes((AuElement *)n, v, cnt);
    for (size_t i = 0; i < cnt; i++)
        au_node_ref(v[i]);
    for (size_t i = 0; i < cnt; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, js_wrap(js, v[i]));
        au_node_unref(v[i]);
    }
    au_free(v);
    return arr;
}

/* host(node): the shadow host when node is a shadow root, else null. */
static JSValue n_host_of(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!n || !au_is_shadow_root(n) || !((AuShadowRoot *)n)->host)
        return JS_NULL;
    return js_wrap(js, &((AuShadowRoot *)n)->host->node);
}

/* template content */
static JSValue g_template_content(JSContext *ctx, JSValueConst this_val)
{
    JSTHIS_EL(e);
    if (!e->template_content)
        e->template_content = au_fragment_new(e->node.doc);
    return js_wrap(js, e->template_content);
}

/* Declarative shadow roots: converts <template shadowrootmode> elements
 * in root's subtree that are complete (the parser has moved past them or
 * final is set) into shadow roots of their parents. */
static JSValue n_declarative_shadow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *root = argc > 0 ? arg_node(js, argv[0]) : &js->doc->node;
    if (!root)
        root = &js->doc->node;
    bool final = argc > 1 && JS_ToBool(ctx, argv[1]);
    AuAtom mode_attr = au_atom("shadowrootmode", 14);
    int done = 0;
    for (AuNode *c = root->first_child; c;) {
        AuNode *nx = au_node_next(c, root);
        if (au_is_html_element(c, AU_ATOM_template) && c->parent && c->parent->type == AU_NODE_ELEMENT &&
            (final || c->next)) {
            AuElement *t = (AuElement *)c;
            const char *m = au_element_get_attr(t, mode_attr);
            AuElement *host = (AuElement *)c->parent;
            if (m && (!strcmp(m, "open") || !strcmp(m, "closed")) && !host->shadow_root && valid_shadow_host(host)) {
                AuShadowRoot *sr = au_element_attach_shadow(host, !strcmp(m, "open") ? AU_SHADOW_OPEN : AU_SHADOW_CLOSED);
                sr->declarative = 1;
                sr->delegates_focus = au_element_get_attr(t, au_atom("shadowrootdelegatesfocus", 24)) != NULL;
                sr->clonable = au_element_get_attr(t, au_atom("shadowrootclonable", 18)) != NULL;
                sr->serializable = au_element_get_attr(t, au_atom("shadowrootserializable", 22)) != NULL;
                if (t->template_content)
                    splice_fragment(&sr->node, t->template_content, NULL);
                nx = au_node_next_skip(c, root);
                au_node_remove(c);
                done++;
            }
        }
        c = nx;
    }
    return JS_NewInt32(ctx, done);
}

/* ---- Custom element support ---- */

static JSValue n_set_defined(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 1 ? arg_node(js, argv[0]) : NULL;
    if (n && n->type == AU_NODE_ELEMENT) {
        bool on = JS_ToBool(ctx, argv[1]);
        AuElement *e = (AuElement *)n;
        if (((e->state & AU_STATE_DEFINED) != 0) != on) {
            au_element_set_state(e, AU_STATE_DEFINED, on);
            if (js->hooks.dom_changed)
                js->hooks.dom_changed(js->hooks.ctx, n, 0, AU_JS_CHANGE_STATE);
        }
    }
    return JS_UNDEFINED;
}

static JSValue n_set_counts(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t obs = 0, defs = 0, scripts = 0;
    if (argc > 0)
        JS_ToUint32(ctx, &obs, argv[0]);
    if (argc > 1)
        JS_ToUint32(ctx, &defs, argv[1]);
    if (argc > 2)
        JS_ToUint32(ctx, &scripts, argv[2]);
    js->observers = obs;
    js->ce_defs = defs;
    js->script_watch = scripts;
    return JS_UNDEFINED;
}

/* setObserved(node, on): marks a node that has MutationObserver
 * registrations. */
static JSValue n_set_observed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 1 ? arg_node(js, argv[0]) : NULL;
    if (n) {
        if (JS_ToBool(ctx, argv[1]))
            n->flags |= JS_NODEF_OBSERVED;
        else
            n->flags &= (uint8_t)~JS_NODEF_OBSERVED;
    }
    return JS_UNDEFINED;
}

/* flushMutations(): moves queued records to the prelude without
 * notifying observers (for takeRecords). */
static JSValue n_flush_mutations(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (js->nmuts)
        js_deliver_mutations(js, false);
    return JS_UNDEFINED;
}

/* createElementRaw(name): an HTML element with no custom element
 * processing, for the registry's constructors. */
static JSValue n_create_element_raw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 1)
        return JS_NULL;
    AuAtom tag = arg_atom(ctx, argv[0], true);
    if (!tag)
        return JS_EXCEPTION;
    AuNode *e = au_element_new(js->doc, tag, AU_NS_HTML);
    JSValue r = js_wrap(js, e);
    au_node_unref(e);
    return r;
}

static JSValue n_current_script(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    return js_wrap(js, js->current_script ? &js->current_script->node : NULL);
}

static JSValue n_document(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    return js_wrap(js, &js->doc->node);
}

void js_keep_doc(AuJs *js, AuDocument *d)
{
    if (js->ndocs == js->docs_cap) {
        js->docs_cap = js->docs_cap ? js->docs_cap * 2 : 4;
        js->docs = au_realloc(js->docs, js->docs_cap * sizeof *js->docs);
    }
    au_node_ref(&d->node);
    js->docs[js->ndocs++] = d;
}

/* parseHTMLDocument(text): a new, inert document parsed from text with
 * scripting off (DOMParser, createHTMLDocument). Its mutations reach no
 * observers and nothing renders it. */
static JSValue n_parse_html_document(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    size_t n = 0;
    char *s = argc > 0 ? arg_str(ctx, argv[0], &n) : NULL;
    if (argc > 0 && !s)
        return JS_EXCEPTION;
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.scripting = false;
    AuDocument *d = au_html_parse(s ? s : "", n, &o);
    au_free(s);
    js_keep_doc(js, d);
    JSValue r = js_wrap(js, &d->node);
    au_node_unref(&d->node);
    return r;
}

/* is(node, iface): instance checks the prelude needs without trusting
 * prototypes script can change. iface: 0 node, 1 element, 2 document, 3
 * fragment, 4 shadow root, 5 text, 6 chardata, 7 connected. */
static JSValue n_is(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 1 ? arg_node(js, argv[0]) : NULL;
    int32_t k = 0;
    if (!n)
        return JS_FALSE;
    JS_ToInt32(ctx, &k, argv[1]);
    switch (k) {
    case 0:
        return JS_TRUE;
    case 1:
        return JS_NewBool(ctx, n->type == AU_NODE_ELEMENT);
    case 2:
        return JS_NewBool(ctx, n->type == AU_NODE_DOCUMENT);
    case 3:
        return JS_NewBool(ctx, n->type == AU_NODE_FRAGMENT);
    case 4:
        return JS_NewBool(ctx, au_is_shadow_root(n));
    case 5:
        return JS_NewBool(ctx, n->type == AU_NODE_TEXT);
    case 6:
        return JS_NewBool(ctx, is_chardata(n));
    case 7:
        return JS_NewBool(ctx, au_node_is_connected(n));
    }
    return JS_FALSE;
}

/* parent(node): parentNode, or the host for a shadow root (event paths). */
static JSValue n_event_parent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = argc > 0 ? arg_node(js, argv[0]) : NULL;
    if (!n)
        return JS_NULL;
    AuElement *slot = au_node_assigned_slot(n);
    if (slot)
        return js_wrap(js, &slot->node);
    if (n->parent)
        return js_wrap(js, n->parent);
    if (au_is_shadow_root(n) && ((AuShadowRoot *)n)->host)
        return js_wrap(js, &((AuShadowRoot *)n)->host->node);
    return JS_NULL;
}

/* ---- Tables ---- */

static const JSCFunctionListEntry node_proto[] = {
    JS_CGETSET_MAGIC_DEF("parentNode", g_tree, NULL, G_PARENT),
    JS_CGETSET_MAGIC_DEF("parentElement", g_tree, NULL, G_PARENT_EL),
    JS_CGETSET_MAGIC_DEF("firstChild", g_tree, NULL, G_FIRST),
    JS_CGETSET_MAGIC_DEF("lastChild", g_tree, NULL, G_LAST),
    JS_CGETSET_MAGIC_DEF("previousSibling", g_tree, NULL, G_PREV),
    JS_CGETSET_MAGIC_DEF("nextSibling", g_tree, NULL, G_NEXT),
    JS_CGETSET_DEF("nodeType", g_node_type, NULL),
    JS_CGETSET_DEF("nodeName", g_node_name, NULL),
    JS_CGETSET_DEF("ownerDocument", g_owner_document, NULL),
    JS_CGETSET_DEF("isConnected", g_is_connected, NULL),
    JS_CGETSET_DEF("textContent", g_text_content, s_text_content),
    JS_CGETSET_DEF("nodeValue", g_node_value, s_node_value),
    JS_CFUNC_DEF("getRootNode", 0, m_get_root_node),
    JS_CFUNC_DEF("hasChildNodes", 0, m_has_child_nodes),
    JS_CFUNC_DEF("appendChild", 1, m_append_child),
    JS_CFUNC_DEF("insertBefore", 2, m_insert_before),
    JS_CFUNC_DEF("removeChild", 1, m_remove_child),
    JS_CFUNC_DEF("replaceChild", 2, m_replace_child),
    JS_CFUNC_DEF("cloneNode", 0, m_clone_node),
    JS_CFUNC_DEF("contains", 1, m_contains),
    JS_CFUNC_DEF("compareDocumentPosition", 1, m_compare_position),
    JS_CFUNC_DEF("isEqualNode", 1, m_is_equal_node),
    JS_CFUNC_DEF("isSameNode", 1, m_is_same_node),
    JS_CFUNC_DEF("normalize", 0, m_normalize),
};

/* ParentNode members shared by Element, Document and DocumentFragment. */
static const JSCFunctionListEntry parent_proto[] = {
    JS_CGETSET_MAGIC_DEF("firstElementChild", g_tree, NULL, G_FIRST_EL),
    JS_CGETSET_MAGIC_DEF("lastElementChild", g_tree, NULL, G_LAST_EL),
    JS_CGETSET_DEF("childElementCount", g_child_element_count, NULL),
    JS_CFUNC_DEF("querySelector", 1, m_query_selector),
    JS_CFUNC_DEF("querySelectorAllArray", 1, m_query_selector_all),
};

static const JSCFunctionListEntry element_proto[] = {
    JS_CGETSET_MAGIC_DEF("previousElementSibling", g_tree, NULL, G_PREV_EL),
    JS_CGETSET_MAGIC_DEF("nextElementSibling", g_tree, NULL, G_NEXT_EL),
    JS_CGETSET_DEF("tagName", g_tag_name, NULL),
    JS_CGETSET_DEF("localName", g_local_name, NULL),
    JS_CGETSET_DEF("namespaceURI", g_namespace, NULL),
    JS_CGETSET_MAGIC_DEF("id", g_reflect, s_reflect, 0),
    JS_CGETSET_MAGIC_DEF("className", g_reflect, s_reflect, 1),
    JS_CGETSET_MAGIC_DEF("slot", g_reflect, s_reflect, 2),
    JS_CFUNC_DEF("getAttribute", 1, m_get_attribute),
    JS_CFUNC_DEF("setAttribute", 2, m_set_attribute),
    JS_CFUNC_DEF("removeAttribute", 1, m_remove_attribute),
    JS_CFUNC_DEF("hasAttribute", 1, m_has_attribute),
    JS_CFUNC_DEF("toggleAttribute", 1, m_toggle_attribute),
    JS_CFUNC_DEF("getAttributeNames", 0, m_get_attribute_names),
    JS_CFUNC_DEF("hasAttributes", 0, m_has_attributes),
    JS_CFUNC_DEF("setAttributeNS", 3, m_set_attribute_ns),
    JS_CFUNC_MAGIC_DEF("getAttributeNS", 2, m_attr_ns, 0),
    JS_CFUNC_MAGIC_DEF("removeAttributeNS", 2, m_attr_ns, 1),
    JS_CFUNC_MAGIC_DEF("hasAttributeNS", 2, m_attr_ns, 2),
    JS_CFUNC_DEF("matches", 1, m_matches),
    JS_CFUNC_DEF("webkitMatchesSelector", 1, m_matches),
    JS_CFUNC_DEF("closest", 1, m_closest),
    JS_CGETSET_DEF("innerHTML", g_inner_html, s_inner_html),
    JS_CGETSET_DEF("outerHTML", g_outer_html, s_outer_html),
    JS_CFUNC_DEF("insertAdjacentHTML", 2, m_insert_adjacent_html),
    JS_CFUNC_DEF("attachShadow", 1, m_attach_shadow),
    JS_CGETSET_DEF("shadowRoot", g_shadow_root, NULL),
};

static const JSCFunctionListEntry chardata_proto[] = {
    JS_CGETSET_DEF("data", g_data, s_data),
    JS_CGETSET_MAGIC_DEF("previousElementSibling", g_tree, NULL, G_PREV_EL),
    JS_CGETSET_MAGIC_DEF("nextElementSibling", g_tree, NULL, G_NEXT_EL),
};

static const JSCFunctionListEntry slottable_proto[] = {
    JS_CGETSET_DEF("assignedSlot", g_assigned_slot, NULL),
};

static const JSCFunctionListEntry document_proto[] = {
    JS_CFUNC_DEF("createElement", 1, m_create_element),
    JS_CFUNC_DEF("createElementNS", 2, m_create_element_ns),
    JS_CFUNC_MAGIC_DEF("createTextNode", 1, m_create_chardata, 0),
    JS_CFUNC_MAGIC_DEF("createComment", 1, m_create_chardata, 1),
    JS_CFUNC_MAGIC_DEF("createDocumentFragment", 0, m_create_chardata, 2),
    JS_CFUNC_MAGIC_DEF("createProcessingInstruction", 2, m_create_chardata, 3),
    JS_CFUNC_DEF("getElementById", 1, m_get_element_by_id),
    JS_CFUNC_DEF("importNode", 1, m_import_node),
    JS_CFUNC_DEF("adoptNode", 1, m_adopt_node),
    JS_CGETSET_MAGIC_DEF("documentElement", g_doc_part, NULL, D_ELEMENT),
    JS_CGETSET_MAGIC_DEF("head", g_doc_part, NULL, D_HEAD),
    JS_CGETSET_MAGIC_DEF("doctype", g_doc_part, NULL, D_DOCTYPE),
    JS_CGETSET_DEF("URL", g_doc_url, NULL),
    JS_CGETSET_DEF("documentURI", g_doc_url, NULL),
    JS_CGETSET_DEF("compatMode", g_compat_mode, NULL),
    JS_CGETSET_DEF("title", g_title, s_title),
    JS_CFUNC_MAGIC_DEF("write", 1, m_write, 0),
    JS_CFUNC_MAGIC_DEF("writeln", 1, m_write, 1),
};

static const JSCFunctionListEntry body_getset[] = {
    JS_CGETSET_MAGIC_DEF("body", g_doc_part, s_body, D_BODY),
};

static const JSCFunctionListEntry fragment_proto[] = {
    JS_CFUNC_DEF("getElementById", 1, m_get_element_by_id),
};

static const JSCFunctionListEntry shadow_proto[] = {
    JS_CGETSET_DEF("host", g_sr_host, NULL),
    JS_CGETSET_MAGIC_DEF("mode", g_sr_prop, NULL, 0),
    JS_CGETSET_MAGIC_DEF("delegatesFocus", g_sr_prop, NULL, 1),
    JS_CGETSET_MAGIC_DEF("slotAssignment", g_sr_prop, NULL, 2),
    JS_CGETSET_MAGIC_DEF("clonable", g_sr_prop, NULL, 3),
    JS_CGETSET_MAGIC_DEF("serializable", g_sr_prop, NULL, 4),
    JS_CGETSET_DEF("innerHTML", g_inner_html, s_inner_html),
};

static const JSCFunctionListEntry doctype_proto[] = {
    JS_CGETSET_MAGIC_DEF("name", g_doctype_part, NULL, 0),
    JS_CGETSET_MAGIC_DEF("publicId", g_doctype_part, NULL, 1),
    JS_CGETSET_MAGIC_DEF("systemId", g_doctype_part, NULL, 2),
};

static const JSCFunctionListEntry pi_proto[] = {
    JS_CGETSET_DEF("target", g_pi_target, NULL),
};

static const JSCFunctionListEntry template_proto[] = {
    JS_CGETSET_DEF("content", g_template_content, NULL),
};

static const JSCFunctionListEntry dom_natives[] = {
    JS_CFUNC_DEF("attrAt", 2, n_attr_at),
    JS_CFUNC_DEF("attrCount", 1, n_attr_count),
    JS_CFUNC_DEF("collect", 3, n_collect),
    JS_CFUNC_DEF("version", 1, n_version),
    JS_CFUNC_DEF("shadowRootOf", 1, n_shadow_root_of),
    JS_CFUNC_DEF("assigned", 1, n_assigned),
    JS_CFUNC_DEF("hostOf", 1, n_host_of),
    JS_CFUNC_DEF("declarativeShadow", 2, n_declarative_shadow),
    JS_CFUNC_DEF("setDefined", 2, n_set_defined),
    JS_CFUNC_DEF("setCounts", 3, n_set_counts),
    JS_CFUNC_DEF("flushMutations", 0, n_flush_mutations),
    JS_CFUNC_DEF("setObserved", 2, n_set_observed),
    JS_CFUNC_DEF("createElementRaw", 1, n_create_element_raw),
    JS_CFUNC_DEF("currentScript", 0, n_current_script),
    JS_CFUNC_DEF("document", 0, n_document),
    JS_CFUNC_DEF("parseHTMLDocument", 1, n_parse_html_document),
    JS_CFUNC_DEF("is", 2, n_is),
    JS_CFUNC_DEF("eventParent", 1, n_event_parent),
};

#define PUT(iface, tab) JS_SetPropertyFunctionList(ctx, js->proto[iface], tab, JS_COUNTOF(tab))

void js_natives_init(AuJs *js, JSValue N)
{
    JSContext *ctx = js->ctx;
    PUT(JSI_Node, node_proto);
    PUT(JSI_Element, parent_proto);
    PUT(JSI_Document, parent_proto);
    PUT(JSI_DocumentFragment, parent_proto);
    PUT(JSI_Element, element_proto);
    PUT(JSI_Element, slottable_proto);
    PUT(JSI_Text, slottable_proto);
    PUT(JSI_CharacterData, chardata_proto);
    PUT(JSI_Document, document_proto);
    PUT(JSI_Document, body_getset);
    PUT(JSI_DocumentFragment, fragment_proto);
    PUT(JSI_ShadowRoot, shadow_proto);
    PUT(JSI_DocumentType, doctype_proto);
    PUT(JSI_ProcessingInstruction, pi_proto);
    PUT(JSI_HTMLTemplateElement, template_proto);
    JS_SetPropertyFunctionList(ctx, N, dom_natives, JS_COUNTOF(dom_natives));
}
