#include "dom.h"

#include <string.h>

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
    switch (n->type) {
    case AU_NODE_ELEMENT: {
        AuElement *e = (AuElement *)n;
        for (uint32_t i = 0; i < e->nattrs; i++)
            au_free(e->attrs[i].value);
        au_free(e->attrs);
        au_free(e->classes);
        if (e->template_content) {
            AuNode *c = e->template_content;
            e->template_content = NULL;
            au_node_unref(c);
        }
        break;
    }
    case AU_NODE_TEXT:
    case AU_NODE_COMMENT:
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
            } else {
                c->parent = n; /* the way back up */
                n = c;
            }
            continue;
        }
        AuNode *up = (n == root) ? NULL : n->parent;
        node_free(n);
        n = up;
    }
}

AuNode *au_node_ref(AuNode *n)
{
    if (n)
        n->refs++;
    return n;
}

void au_node_unref(AuNode *n)
{
    if (!n)
        return;
    if (n->refs > 0)
        n->refs--;
    if (n->refs == 0 && !n->parent)
        destroy_subtree(n);
}

/* ---- Tree ---- */

static void unlink_node(AuNode *c)
{
    AuNode *p = c->parent;
    if (!p)
        return;
    if (c->prev)
        c->prev->next = c->next;
    else
        p->first_child = c->next;
    if (c->next)
        c->next->prev = c->prev;
    else
        p->last_child = c->prev;
    c->parent = c->prev = c->next = NULL;
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

static void attr_changed(AuElement *el, AuAtom name, const char *v, size_t len)
{
    if (name == AU_ATOM_id)
        el->id = au_atom(v, len);
    else if (name == AU_ATOM_class)
        update_classes(el, v, len);
}

void au_element_set_attr_ns(AuElement *el, AuAtom name, AuAttrNamespace ns, const char *value, size_t len)
{
    AuAttr *a = au_element_attr(el, name);
    if (a) {
        au_free(a->value);
    } else {
        if (el->nattrs == el->attr_cap) {
            el->attr_cap = el->attr_cap ? el->attr_cap * 2 : 4;
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
    au_free(a->value);
    uint32_t i = (uint32_t)(a - el->attrs);
    memmove(a, a + 1, (el->nattrs - i - 1) * sizeof *a);
    el->nattrs--;
    attr_changed(el, name, "", 0);
}

bool au_element_has_class(const AuElement *el, AuAtom cls)
{
    for (uint32_t i = 0; i < el->nclasses; i++)
        if (el->classes[i] == cls)
            return true;
    return false;
}

/* ---- Character data ---- */

void au_text_append(AuText *t, const char *data, size_t len)
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

void au_text_set(AuText *t, const char *data, size_t len)
{
    t->len = 0;
    au_text_append(t, data, len);
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
    for (AuNode *p = el->node.parent; p && p->type == AU_NODE_ELEMENT; p = p->parent)
        set_bits((AuElement *)p, up, on);
}
