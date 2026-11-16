#include "dom.h"
#include "control.h"

#include <string.h>

/* ---- Mutation hooks (additions for script) ---- */

#define HOOKS(n) ((n)->doc ? (n)->doc->hooks : NULL)

static inline void hold_changed(AuNode *n)
{
    const AuDomHooks *h;
    if (n->js && (h = HOOKS(n)) && h->hold_changed)
        h->hold_changed(n->doc->hooks_ctx, n);
}

static inline void bump(AuNode *n)
{
    if (n->doc)
        n->doc->version++;
}

void au_document_set_hooks(AuDocument *doc, const AuDomHooks *hooks, void *ctx)
{
    doc->hooks = hooks;
    doc->hooks_ctx = ctx;
}

/* ---- Construction ---- */

static AuNode *node_init(AuNode *n, AuNodeType type, AuDocument *doc)
{
    n->type = (uint8_t)type;
    n->refs = 1;
    n->doc = doc;
    return n;
}

AuDocument *au_document_new(void)
{
    AuDocument *d = au_calloc(sizeof *d);
    node_init(&d->node, AU_NODE_DOCUMENT, d);
    return d;
}

AuNode *au_element_new(AuDocument *doc, AuAtom tag, AuNamespace ns)
{
    AuElement *e = au_calloc(sizeof *e);
    e->tag = tag;
    e->ns = (uint8_t)ns;
    return node_init(&e->node, AU_NODE_ELEMENT, doc);
}

static AuNode *chardata_new(AuDocument *doc, AuNodeType type, const char *data, size_t len)
{
    AuText *t = au_calloc(sizeof *t);
    t->data = au_strndup(data, len);
    t->len = len;
    t->cap = len + 1;
    return node_init(&t->node, type, doc);
}

AuNode *au_text_new(AuDocument *doc, const char *data, size_t len)
{
    return chardata_new(doc, AU_NODE_TEXT, data, len);
}

AuNode *au_comment_new(AuDocument *doc, const char *data, size_t len)
{
    return chardata_new(doc, AU_NODE_COMMENT, data, len);
}

AuNode *au_doctype_new(AuDocument *doc, const char *name, const char *public_id, const char *system_id)
{
    AuDoctype *d = au_calloc(sizeof *d);
    d->name = au_strndup(name ? name : "", name ? strlen(name) : 0);
    d->public_id = au_strndup(public_id ? public_id : "", public_id ? strlen(public_id) : 0);
    d->system_id = au_strndup(system_id ? system_id : "", system_id ? strlen(system_id) : 0);
    return node_init(&d->node, AU_NODE_DOCTYPE, doc);
}

AuNode *au_fragment_new(AuDocument *doc)
{
    AuNode *n = au_calloc(sizeof *n);
    return node_init(n, AU_NODE_FRAGMENT, doc);
}

/* ---- Lifetime ---- */

static void destroy_subtree(AuNode *root);

/* Frees one node's own storage. Its children are already gone. */
static void node_free(AuNode *n)
{
    if (n->type != AU_NODE_DOCUMENT && n->doc && n->doc->nmanual)
        au_manual_slots_removed(n->doc, n); /* the pairs hold no references */
    switch (n->type) {
    case AU_NODE_ELEMENT: {
        AuElement *e = (AuElement *)n;
        for (uint32_t i = 0; i < e->nattrs; i++)
            au_free(e->attrs[i].value);
        au_free(e->attrs);
        au_free(e->classes);
        au_ctl_free(e);
        if (e->shadow_root) {
            AuNode *sr = e->shadow_root;
            e->shadow_root = NULL;
            ((AuShadowRoot *)sr)->host = NULL;
            au_node_unref(sr);
        }
        if (e->template_content) {
            AuNode *c = e->template_content;
            e->template_content = NULL;
            au_node_unref(c);
        }
        break;
    }
    case AU_NODE_TEXT:
    case AU_NODE_COMMENT:
    case AU_NODE_PROCESSING_INSTRUCTION:
        au_free(((AuText *)n)->data);
        break;
    case AU_NODE_DOCTYPE: {
        AuDoctype *d = (AuDoctype *)n;
        au_free(d->name);
        au_free(d->public_id);
        au_free(d->system_id);
        break;
    }
    case AU_NODE_DOCUMENT:
        au_free(((AuDocument *)n)->url);
        au_free(((AuDocument *)n)->top_layer);
        au_free(((AuDocument *)n)->manual);
        break;
    default:
        break;
    }
    au_free(n);
}

/* Frees root and every descendant nothing else references. Iterative: page
 * trees can be deeper than the Mac OS 9 stack allows for recursion.
 * A descendant that is still referenced becomes the root of its own tree. */
static void destroy_subtree(AuNode *root)
{
    AuNode *n = root;
    root->parent = NULL;
    while (n) {
        AuNode *c = n->first_child;
        if (c) {
            n->first_child = c->next;
            c->prev = c->next = NULL;
            if (c->refs > 0) {
                c->parent = NULL;
                hold_changed(c);
            } else {
                c->parent = n; /* the way back up */
                n = c;
            }
            continue;
        }
        if (n->type == AU_NODE_ELEMENT) {
            /* A shadow root or template contents that this element alone
             * holds is freed here, as if it were a child: node_free would
             * release it by a nested call, once per level of templates
             * nested in templates, which a page can make as deep as it
             * likes. */
            AuElement *e = (AuElement *)n;
            AuNode *x = NULL;
            if (e->shadow_root) {
                x = e->shadow_root;
                e->shadow_root = NULL;
                ((AuShadowRoot *)x)->host = NULL;
            } else if (e->template_content) {
                x = e->template_content;
                e->template_content = NULL;
            }
            if (x) {
                if (x->refs > 0)
                    x->refs--;
                hold_changed(x);
                if (x->refs == 0 && !x->parent) {
                    x->parent = n; /* the way back up */
                    n = x;
                }
                continue;
            }
        }
        AuNode *up = (n == root) ? NULL : n->parent;
        node_free(n);
        n = up;
    }
}

AuNode *au_node_ref(AuNode *n)
{
    if (n) {
        n->refs++;
        hold_changed(n);
    }
    return n;
}

void au_node_unref(AuNode *n)
{
    if (!n)
        return;
    if (n->refs > 0)
        n->refs--;
    hold_changed(n);
    if (n->refs == 0 && !n->parent)
        destroy_subtree(n);
}

/* ---- Tree ---- */

static void top_layer_left(AuDocument *d, AuNode *c);

static void unlink_node(AuNode *c)
{
    AuNode *p = c->parent;
    if (!p)
        return;
    AuNode *prev = c->prev, *next = c->next;
    if (c->prev)
        c->prev->next = c->next;
    else
        p->first_child = c->next;
    if (c->next)
        c->next->prev = c->prev;
    else
        p->last_child = c->prev;
    c->parent = c->prev = c->next = NULL;
    bump(p);
    if (p->doc && p->doc->ntop)
        top_layer_left(p->doc, c);
    const AuDomHooks *h = HOOKS(p);
    if (h && h->removed)
        h->removed(p->doc->hooks_ctx, p, c, prev, next);
    hold_changed(c);
}

void au_node_insert_before(AuNode *parent, AuNode *child, AuNode *before)
{
    if (child == before)
        return;
    unlink_node(child);
    child->parent = parent;
    child->next = before;
    if (before) {
        child->prev = before->prev;
        if (before->prev)
            before->prev->next = child;
        else
            parent->first_child = child;
        before->prev = child;
    } else {
        child->prev = parent->last_child;
        if (parent->last_child)
            parent->last_child->next = child;
        else
            parent->first_child = child;
        parent->last_child = child;
    }
    bump(parent);
    const AuDomHooks *h = HOOKS(parent);
    if (h && h->inserted)
        h->inserted(parent->doc->hooks_ctx, parent, child);
    hold_changed(child);
}

void au_node_append(AuNode *parent, AuNode *child)
{
    au_node_insert_before(parent, child, NULL);
}

void au_node_remove(AuNode *child)
{
    unlink_node(child);
    if (child->refs == 0)
        destroy_subtree(child);
}

void au_node_reparent_children(AuNode *from, AuNode *to)
{
    while (from->first_child)
        au_node_append(to, from->first_child);
}

AuNode *au_node_next_skip(const AuNode *n, const AuNode *root)
{
    while (n && n != root) {
        if (n->next)
            return n->next;
        n = n->parent;
    }
    return NULL;
}

AuNode *au_node_next(const AuNode *n, const AuNode *root)
{
    if (n->first_child)
        return n->first_child;
    return au_node_next_skip(n, root);
}

/* ---- Attributes ---- */

AuAttr *au_element_attr(const AuElement *el, AuAtom name)
{
    for (uint32_t i = 0; i < el->nattrs; i++)
        if (el->attrs[i].name == name)
            return &el->attrs[i];
    return NULL;
}

const char *au_element_get_attr(const AuElement *el, AuAtom name)
{
    AuAttr *a = au_element_attr(el, name);
    return a ? a->value : NULL;
}

static void update_classes(AuElement *el, const char *v, size_t len)
{
    au_free(el->classes);
    el->classes = NULL;
    el->nclasses = 0;
    uint32_t cap = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && au_is_space((unsigned char)v[i]))
            i++;
        size_t start = i;
        while (i < len && !au_is_space((unsigned char)v[i]))
            i++;
        if (i == start)
            break;
        AuAtom a = au_atom(v + start, i - start);
        if (au_element_has_class(el, a))
            continue;
        if (el->nclasses == cap) {
            cap = cap ? cap * 2 : 4;
            el->classes = au_realloc(el->classes, cap * sizeof *el->classes);
        }
        el->classes[el->nclasses++] = a;
    }
}

/* Notes a class name or id that an element gained or lost (see
 * AuDocument.style_names). */
static void style_name_changed(AuElement *el, AuAtom a)
{
    AuDocument *d = el->node.doc;
    if (!d || !a || d->style_names_n > AU_DOC_STYLE_NAMES)
        return;
    for (uint8_t i = 0; i < d->style_names_n; i++)
        if (d->style_names[i] == a)
            return;
    if (d->style_names_n == AU_DOC_STYLE_NAMES) {
        d->style_names_n = AU_DOC_STYLE_NAMES + 1;
        return;
    }
    d->style_names[d->style_names_n++] = a;
}

static void attr_changed(AuElement *el, AuAtom name, const char *v, size_t len)
{
    if (name == AU_ATOM_inert && el->node.doc)
        el->node.doc->has_inert = true;
    if (name == AU_ATOM_id) {
        AuAtom old = el->id;
        el->id = au_atom(v, len);
        if (old != el->id) {
            style_name_changed(el, old);
            style_name_changed(el, el->id);
        }
    } else if (name == AU_ATOM_class) {
        AuAtom old[32];
        uint32_t nold = el->nclasses;
        if (nold > 32) {
            if (el->node.doc)
                el->node.doc->style_names_n = AU_DOC_STYLE_NAMES + 1;
            nold = 0;
        }
        for (uint32_t i = 0; i < nold; i++)
            old[i] = el->classes[i];
        update_classes(el, v, len);
        for (uint32_t i = 0; i < nold; i++)
            if (!au_element_has_class(el, old[i]))
                style_name_changed(el, old[i]);
        for (uint32_t i = 0; i < el->nclasses; i++) {
            bool had = false;
            for (uint32_t k = 0; k < nold && !had; k++)
                had = old[k] == el->classes[i];
            if (!had)
                style_name_changed(el, el->classes[i]);
        }
    }
}

static void attr_hook(AuElement *el, AuAtom name, uint8_t ns, const AuAttr *old)
{
    bump(&el->node);
    const AuDomHooks *h = HOOKS(&el->node);
    if (h && h->attr_changing)
        h->attr_changing(el->node.doc->hooks_ctx, el, name, ns, old ? old->value : NULL, old ? old->len : 0);
}

/* The popover state a popover attribute value names: 0 none (NULL), 1
 * auto, 2 hint, 3 manual (an invalid value). */
static int popover_kind(const char *v, size_t n)
{
    if (!v)
        return 0;
    if (!n || au_ieq(v, n, "auto", 4))
        return 1;
    return au_ieq(v, n, "hint", 4) ? 2 : 3;
}

/* A showing popover whose popover attribute changes state hides (HTML,
 * the popover attribute change steps), without toggle events. now is
 * the new value (NULL: removed). */
static void popover_attr(AuElement *el, const AuAttr *old, const char *now, size_t now_len)
{
    if (!(el->state & AU_STATE_POPOVER_OPEN) || !old ||
        popover_kind(old->value, old->len) == popover_kind(now, now_len))
        return;
    el->state &= ~(uint32_t)AU_STATE_POPOVER_OPEN;
    if (!(el->state & AU_STATE_MODAL))
        au_top_layer_remove(el);
}

void au_element_set_attr_ns(AuElement *el, AuAtom name, AuAttrNamespace ns, const char *value, size_t len)
{
    AuAttr *a = au_element_attr(el, name);
    if (name == AU_ATOM_popover && ns == AU_ATTR_NS_NONE)
        popover_attr(el, a, value, len);
    attr_hook(el, name, (uint8_t)ns, a);
    if (a) {
        au_free(a->value);
    } else {
        if (el->nattrs == el->attr_cap) {
            el->attr_cap = el->attr_cap ? el->attr_cap * 2 : 1;
            el->attrs = au_realloc(el->attrs, el->attr_cap * sizeof *el->attrs);
        }
        a = &el->attrs[el->nattrs++];
        a->name = name;
    }
    a->ns = (uint8_t)ns;
    a->value = au_strndup(value, len);
    a->len = (uint32_t)len;
    if (ns == AU_ATTR_NS_NONE)
        attr_changed(el, name, a->value, len);
}

void au_element_set_attr(AuElement *el, AuAtom name, const char *value, size_t len)
{
    au_element_set_attr_ns(el, name, AU_ATTR_NS_NONE, value, len);
}

void au_element_remove_attr(AuElement *el, AuAtom name)
{
    AuAttr *a = au_element_attr(el, name);
    if (!a)
        return;
    if (name == AU_ATOM_popover)
        popover_attr(el, a, NULL, 0);
    attr_hook(el, name, a->ns, a);
    au_free(a->value);
    uint32_t i = (uint32_t)(a - el->attrs);
    memmove(a, a + 1, (el->nattrs - i - 1) * sizeof *a);
    el->nattrs--;
    attr_changed(el, name, "", 0);
    /* A modal dialog whose open attribute is removed leaves the top
     * layer (HTML, the dialog's attribute change steps). */
    if ((el->state & AU_STATE_MODAL) && name == AU_ATOM_open && el->ns == AU_NS_HTML && el->tag == AU_ATOM_dialog) {
        el->state &= ~(uint32_t)AU_STATE_MODAL;
        au_top_layer_remove(el);
    }
}

bool au_element_has_class(const AuElement *el, AuAtom cls)
{
    for (uint32_t i = 0; i < el->nclasses; i++)
        if (el->classes[i] == cls)
            return true;
    return false;
}

/* ---- Character data ---- */

static void text_hook(AuText *t)
{
    bump(&t->node);
    const AuDomHooks *h = HOOKS(&t->node);
    if (h && h->text_changing)
        h->text_changing(t->node.doc->hooks_ctx, t, t->data, t->len);
}

static void text_append(AuText *t, const char *data, size_t len)
{
    if (t->len + len + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 16;
        while (cap < t->len + len + 1)
            cap *= 2;
        t->data = au_realloc(t->data, cap);
        t->cap = cap;
    }
    memcpy(t->data + t->len, data, len);
    t->len += len;
    t->data[t->len] = 0;
}

void au_text_append(AuText *t, const char *data, size_t len)
{
    text_hook(t);
    text_append(t, data, len);
}

void au_text_set(AuText *t, const char *data, size_t len)
{
    text_hook(t);
    t->len = 0;
    text_append(t, data, len);
    if (!len)
        t->data[0] = 0;
}

/* ---- Document ---- */

AuElement *au_document_element(const AuDocument *doc)
{
    for (AuNode *c = doc->node.first_child; c; c = c->next)
        if (c->type == AU_NODE_ELEMENT)
            return (AuElement *)c;
    return NULL;
}

static AuElement *html_child(const AuDocument *doc, AuAtom tag)
{
    AuElement *root = au_document_element(doc);
    if (!root || !au_is_html_element(&root->node, AU_ATOM_html))
        return NULL;
    for (AuNode *c = root->node.first_child; c; c = c->next)
        if (au_is_html_element(c, tag))
            return (AuElement *)c;
    return NULL;
}

AuElement *au_document_head(const AuDocument *doc)
{
    return html_child(doc, AU_ATOM_head);
}

AuElement *au_document_body(const AuDocument *doc)
{
    return html_child(doc, AU_ATOM_body);
}

/* ---- Additions for the HTML parser ---- */

AuNode *au_pi_new(AuDocument *doc, AuAtom target, const char *data, size_t len)
{
    AuProcessingInstruction *pi = au_calloc(sizeof *pi);
    pi->text.data = au_strndup(data, len);
    pi->text.len = len;
    pi->text.cap = len + 1;
    pi->target = target;
    return node_init(&pi->text.node, AU_NODE_PROCESSING_INSTRUCTION, doc);
}

void au_element_append_attr(AuElement *el, AuAtom name, AuAttrNamespace ns, const char *value, size_t len)
{
    attr_hook(el, name, (uint8_t)ns, NULL);
    if (el->nattrs == el->attr_cap) {
        el->attr_cap = el->attr_cap ? el->attr_cap * 2 : 1;
        el->attrs = au_realloc(el->attrs, el->attr_cap * sizeof *el->attrs);
    }
    AuAttr *a = &el->attrs[el->nattrs++];
    a->name = name;
    a->ns = (uint8_t)ns;
    a->value = au_strndup(value, len);
    a->len = (uint32_t)len;
    if (ns == AU_ATTR_NS_NONE)
        attr_changed(el, name, a->value, len);
}

/* ---- Element state ---- */

static void set_bits(AuElement *el, uint32_t bits, bool on)
{
    if (on)
        el->state |= bits;
    else
        el->state &= ~bits;
}

void au_element_set_state(AuElement *el, uint32_t bits, bool on)
{
    set_bits(el, bits, on);
    uint32_t up = bits & (AU_STATE_HOVER | AU_STATE_ACTIVE);
    if (bits & AU_STATE_FOCUS) {
        up |= AU_STATE_FOCUS_WITHIN;
        set_bits(el, AU_STATE_FOCUS_WITHIN, on);
    }
    if (!up)
        return;
    if (el->node.doc && el->node.doc->has_shadow) {
        /* :hover, :active and :focus-within follow the flat tree; a
         * focused element's shadow hosts match :focus too (HTML, "focus
         * delegation" and the :focus pseudo-class). */
        for (AuElement *p = au_flat_parent_element(&el->node); p; p = au_flat_parent_element(&p->node))
            set_bits(p, up, on);
        if (bits & AU_STATE_FOCUS)
            for (AuElement *h = au_node_tree_host(&el->node); h; h = au_node_tree_host(&h->node))
                set_bits(h, AU_STATE_FOCUS, on);
        return;
    }
    for (AuNode *p = el->node.parent; p && p->type == AU_NODE_ELEMENT; p = p->parent)
        set_bits((AuElement *)p, up, on);
}

/* ---- The top layer and inertness ---- */

/* n's parent in the flat sense: a shadow root's parent is its host. */
static const AuNode *up_node(const AuNode *n)
{
    if (n->parent)
        return n->parent;
    if (n->type == AU_NODE_FRAGMENT && (n->flags & AU_NODEF_SHADOW_ROOT))
        return (const AuNode *)((const AuShadowRoot *)n)->host;
    return NULL;
}

static uint32_t top_find(const AuDocument *d, const AuElement *el)
{
    for (uint32_t i = 0; i < d->ntop; i++)
        if (d->top_layer[i] == el)
            return i;
    return UINT32_MAX;
}

static void top_drop(AuDocument *d, uint32_t i)
{
    memmove(d->top_layer + i, d->top_layer + i + 1, (d->ntop - i - 1) * sizeof *d->top_layer);
    d->ntop--;
}

void au_top_layer_add(AuElement *el)
{
    AuDocument *d = el->node.doc;
    if (!d)
        return;
    uint32_t i = top_find(d, el);
    if (i != UINT32_MAX)
        top_drop(d, i);
    if (d->ntop == d->top_cap) {
        d->top_cap = d->top_cap ? d->top_cap * 2 : 4;
        d->top_layer = au_realloc(d->top_layer, d->top_cap * sizeof *d->top_layer);
    }
    d->top_layer[d->ntop++] = el;
}

void au_top_layer_remove(AuElement *el)
{
    AuDocument *d = el->node.doc;
    uint32_t i = d ? top_find(d, el) : UINT32_MAX;
    if (i != UINT32_MAX)
        top_drop(d, i);
}

int au_top_layer_index(const AuElement *el)
{
    const AuDocument *d = el->node.doc;
    uint32_t i = d ? top_find(d, el) : UINT32_MAX;
    return i == UINT32_MAX ? -1 : (int)i;
}

AuElement *au_document_modal(const AuDocument *doc)
{
    for (uint32_t i = doc ? doc->ntop : 0; i-- > 0;)
        if (doc->top_layer[i]->state & AU_STATE_MODAL)
            return doc->top_layer[i];
    return NULL;
}

/* c left its parent in d: the elements of the top layer at or under c
 * leave it too, and stop being modal or showing (HTML, the removing
 * steps of dialog and of popovers). */
static void top_layer_left(AuDocument *d, AuNode *c)
{
    for (uint32_t i = d->ntop; i-- > 0;) {
        AuElement *e = d->top_layer[i];
        const AuNode *p = &e->node;
        while (p && p != c)
            p = up_node(p);
        if (!p)
            continue;
        e->state &= ~(uint32_t)(AU_STATE_MODAL | AU_STATE_POPOVER_OPEN);
        top_drop(d, i);
    }
}

bool au_node_inert(const AuNode *n)
{
    const AuDocument *d = n ? n->doc : NULL;
    if (!d || (!d->ntop && !d->has_inert))
        return false;
    const AuElement *modal = au_document_modal(d);
    if (!modal && !d->has_inert)
        return false;
    bool inside = modal == NULL;
    for (const AuNode *p = n; p; p = up_node(p)) {
        if (p->type != AU_NODE_ELEMENT)
            continue;
        if (modal && p == &modal->node)
            inside = true;
        if (d->has_inert && au_element_attr((const AuElement *)p, AU_ATOM_inert))
            return true;
    }
    return !inside;
}
