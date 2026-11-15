/* Shadow roots, slot assignment and the flat tree (additions for script). */
#include "dom.h"

#include <string.h>

AuShadowRoot *au_element_attach_shadow(AuElement *host, AuShadowMode mode)
{
    if (host->shadow_root)
        return NULL;
    AuShadowRoot *sr = au_calloc(sizeof *sr);
    sr->node.type = AU_NODE_FRAGMENT;
    sr->node.flags = AU_NODEF_SHADOW_ROOT;
    sr->node.refs = 1; /* the host's */
    sr->node.doc = host->node.doc;
    sr->host = host;
    sr->mode = (uint8_t)mode;
    host->shadow_root = &sr->node;
    if (host->node.doc)
        host->node.doc->version++;
    return sr;
}

AuNode *au_node_root(const AuNode *n)
{
    while (n->parent)
        n = n->parent;
    return (AuNode *)n;
}

AuNode *au_node_shadow_including_root(const AuNode *n)
{
    for (;;) {
        n = au_node_root(n);
        if (au_is_shadow_root(n) && ((const AuShadowRoot *)n)->host)
            n = &((const AuShadowRoot *)n)->host->node;
        else
            return (AuNode *)n;
    }
}

bool au_node_is_connected(const AuNode *n)
{
    return au_node_shadow_including_root(n)->type == AU_NODE_DOCUMENT;
}

const char *au_slot_name_of(const AuNode *n)
{
    if (n->type != AU_NODE_ELEMENT)
        return "";
    const AuElement *el = (const AuElement *)n;
    const char *v;
    if (el->ns == AU_NS_HTML && el->tag == AU_ATOM_slot)
        v = au_element_get_attr(el, au_atom("name", 4));
    else
        v = au_element_get_attr(el, au_atom("slot", 4));
    return v ? v : "";
}

static bool is_slot(const AuNode *n)
{
    return au_is_html_element(n, AU_ATOM_slot);
}

static bool is_slottable(const AuNode *n)
{
    return n->type == AU_NODE_ELEMENT || n->type == AU_NODE_TEXT;
}

/* The first <slot> in tree order in sr named name. */
static AuElement *find_slot(const AuShadowRoot *sr, const char *name)
{
    const AuNode *root = &sr->node;
    for (AuNode *c = root->first_child; c; c = au_node_next(c, root))
        if (is_slot(c) && strcmp(au_slot_name_of(c), name) == 0)
            return (AuElement *)c;
    return NULL;
}

AuElement *au_node_assigned_slot(const AuNode *n)
{
    if (!is_slottable(n) || !n->parent || n->parent->type != AU_NODE_ELEMENT)
        return NULL;
    const AuShadowRoot *sr = au_element_shadow_root((const AuElement *)n->parent);
    if (!sr || sr->manual_slots)
        return NULL;
    return find_slot(sr, au_slot_name_of(n));
}

/* The shadow root a slot belongs to, or NULL. */
static const AuShadowRoot *slot_root(const AuElement *slot)
{
    const AuNode *r = au_node_root(&slot->node);
    return au_is_shadow_root(r) ? (const AuShadowRoot *)r : NULL;
}

size_t au_slot_assigned_nodes(const AuElement *slot, AuNode **out, size_t cap)
{
    const AuShadowRoot *sr = slot_root(slot);
    if (!sr || !sr->host || sr->manual_slots)
        return 0;
    const char *name = au_slot_name_of(&slot->node);
    if (find_slot(sr, name) != slot)
        return 0;
    size_t n = 0;
    for (AuNode *c = sr->host->node.first_child; c; c = c->next) {
        if (!is_slottable(c) || strcmp(au_slot_name_of(c), name) != 0)
            continue;
        if (n < cap)
            out[n] = c;
        n++;
    }
    return n;
}

/* ---- The flat tree ---- */

/* The next assigned node of slot after c (a child of the host), or NULL. */
static AuNode *next_assigned(const AuElement *slot, const AuNode *c)
{
    const char *name = au_slot_name_of(&slot->node);
    for (c = c ? c->next : slot_root(slot)->host->node.first_child; c; c = c->next)
        if (is_slottable(c) && strcmp(au_slot_name_of(c), name) == 0)
            return (AuNode *)c;
    return NULL;
}

static bool slot_has_assigned(const AuElement *slot)
{
    const AuShadowRoot *sr = slot_root(slot);
    if (!sr || !sr->host || sr->manual_slots || find_slot(sr, au_slot_name_of(&slot->node)) != slot)
        return false;
    return next_assigned(slot, NULL) != NULL;
}

AuNode *au_flat_first_child(const AuNode *n)
{
    if (n->type == AU_NODE_ELEMENT) {
        const AuElement *el = (const AuElement *)n;
        if (el->shadow_root)
            return el->shadow_root->first_child;
        if (is_slot(n) && slot_has_assigned(el))
            return next_assigned(el, NULL);
    }
    return n->first_child;
}

AuNode *au_flat_next_sibling(const AuNode *n)
{
    AuElement *slot = au_node_assigned_slot(n);
    if (slot)
        return next_assigned(slot, n);
    return n->next;
}

AuNode *au_flat_parent(const AuNode *n)
{
    AuElement *slot = au_node_assigned_slot(n);
    if (slot)
        return &slot->node;
    AuNode *p = n->parent;
    if (p && au_is_shadow_root(p))
        return ((AuShadowRoot *)p)->host ? &((AuShadowRoot *)p)->host->node : NULL;
    return p;
}
