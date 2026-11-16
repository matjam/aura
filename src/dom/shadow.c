/* Shadow roots, slot assignment and the flat tree. */
#include "dom.h"

#include <string.h>

bool au_element_can_host_shadow(const AuElement *e)
{
    if (e->ns != AU_NS_HTML)
        return false;
    static const char *const ok[] = {"article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3",
                                     "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span"};
    const char *s = au_atom_str(e->tag);
    if (strchr(s, '-'))
        return true;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (!strcmp(s, ok[i]))
            return true;
    return false;
}

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
    if (host->node.doc) {
        host->node.doc->version++;
        host->node.doc->has_shadow = true;
    }
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

AuNode *au_shadow_including_parent(const AuNode *n)
{
    if (n->parent)
        return n->parent;
    if (au_is_shadow_root(n) && ((const AuShadowRoot *)n)->host)
        return &((const AuShadowRoot *)n)->host->node;
    return NULL;
}

AuElement *au_node_tree_host(const AuNode *n)
{
    const AuNode *r = au_node_root(n);
    return au_is_shadow_root(r) ? ((const AuShadowRoot *)r)->host : NULL;
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
        v = au_element_get_attr(el, AU_ATOM_slot);
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

/* The shadow root a slot belongs to, or NULL. */
static const AuShadowRoot *slot_root(const AuElement *slot)
{
    const AuNode *r = au_node_root(&slot->node);
    return au_is_shadow_root(r) ? (const AuShadowRoot *)r : NULL;
}

/* ---- Manual assignment ----
 * The pairs live in the document (AuDocument.manual), in assign() order.
 * They hold no references: dom.c drops a pair when its node is unlinked
 * or its slot leaves its tree (au_manual_slots_removed). */

static bool before_in_tree(const AuNode *a, const AuNode *b, const AuNode *root)
{
    for (const AuNode *c = root->first_child; c; c = au_node_next(c, root)) {
        if (c == a)
            return true;
        if (c == b)
            return false;
    }
    return false;
}

/* The slot n is manually assigned to in sr: of the slots of sr whose list
 * holds n, the first in tree order. */
static AuElement *manual_slot_of(const AuShadowRoot *sr, const AuNode *n)
{
    const AuDocument *d = n->doc;
    if (!d || !d->nmanual)
        return NULL;
    AuElement *best = NULL;
    for (uint32_t i = 0; i < d->nmanual; i++) {
        const AuManualSlot *m = &d->manual[i];
        if (m->node != n || slot_root(m->slot) != sr)
            continue;
        if (!best || before_in_tree(&m->slot->node, &best->node, &sr->node))
            best = m->slot;
    }
    return best;
}

void au_slot_assign(AuElement *slot, AuNode *const *nodes, size_t n)
{
    AuDocument *d = slot->node.doc;
    if (!d)
        return;
    /* the slot's old list goes, and every node given leaves other lists */
    uint32_t k = 0;
    for (uint32_t i = 0; i < d->nmanual; i++) {
        AuManualSlot *m = &d->manual[i];
        bool drop = m->slot == slot;
        for (size_t j = 0; j < n && !drop; j++)
            drop = m->node == nodes[j];
        if (!drop)
            d->manual[k++] = *m;
    }
    d->nmanual = k;
    for (size_t j = 0; j < n; j++) {
        if (!nodes[j] || !is_slottable(nodes[j]) || nodes[j]->doc != d)
            continue;
        bool dup = false;
        for (size_t q = 0; q < j && !dup; q++)
            dup = nodes[q] == nodes[j];
        if (dup)
            continue;
        if (d->nmanual == d->manual_cap) {
            d->manual_cap = d->manual_cap ? d->manual_cap * 2 : 8;
            d->manual = au_realloc(d->manual, d->manual_cap * sizeof *d->manual);
        }
        d->manual[d->nmanual].node = nodes[j];
        d->manual[d->nmanual].slot = slot;
        d->nmanual++;
    }
    d->version++;
}

void au_manual_slots_removed(AuDocument *d, const AuNode *n)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < d->nmanual; i++) {
        AuManualSlot *m = &d->manual[i];
        if (m->node == n || &m->slot->node == n)
            continue;
        d->manual[k++] = *m;
    }
    d->nmanual = k;
}

/* ---- Assignment ---- */

AuElement *au_node_assigned_slot(const AuNode *n)
{
    if (!n->parent || n->parent->type != AU_NODE_ELEMENT || !is_slottable(n))
        return NULL;
    const AuShadowRoot *sr = au_element_shadow_root((const AuElement *)n->parent);
    if (!sr)
        return NULL;
    if (sr->manual_slots)
        return manual_slot_of(sr, n);
    return find_slot(sr, au_slot_name_of(n));
}

/* Whether slot is the slot its name selects in its root (named mode). */
static bool slot_is_active(const AuShadowRoot *sr, const AuElement *slot)
{
    return find_slot(sr, au_slot_name_of(&slot->node)) == slot;
}

/* The next node assigned to slot after c (a child of the host; NULL for
 * the first), or NULL. */
static AuNode *next_assigned(const AuShadowRoot *sr, const AuElement *slot, const AuNode *c)
{
    if (!sr->manual_slots) {
        const char *name = au_slot_name_of(&slot->node);
        for (c = c ? c->next : sr->host->node.first_child; c; c = c->next)
            if (is_slottable(c) && strcmp(au_slot_name_of(c), name) == 0)
                return (AuNode *)c;
        return NULL;
    }
    /* manual: in assign() order, the nodes that are children of the host
     * and whose first slot is this one */
    const AuDocument *d = slot->node.doc;
    uint32_t i = 0;
    if (c) {
        while (i < d->nmanual && !(d->manual[i].slot == slot && d->manual[i].node == c))
            i++;
        i++;
    }
    for (; i < d->nmanual; i++) {
        const AuManualSlot *m = &d->manual[i];
        if (m->slot == slot && m->node->parent == &sr->host->node && manual_slot_of(sr, m->node) == slot)
            return m->node;
    }
    return NULL;
}

size_t au_slot_assigned_nodes(const AuElement *slot, AuNode **out, size_t cap)
{
    const AuShadowRoot *sr = slot_root(slot);
    if (!sr || !sr->host)
        return 0;
    if (!sr->manual_slots && !slot_is_active(sr, slot))
        return 0;
    size_t n = 0;
    for (AuNode *c = next_assigned(sr, slot, NULL); c; c = next_assigned(sr, slot, c)) {
        if (n < cap)
            out[n] = c;
        n++;
    }
    return n;
}

AuNode *au_node_next_including_shadow(const AuNode *n, const AuNode *root)
{
    if (n->type == AU_NODE_ELEMENT && ((const AuElement *)n)->shadow_root &&
        ((const AuElement *)n)->shadow_root->first_child)
        return ((const AuElement *)n)->shadow_root->first_child;
    if (n->first_child)
        return n->first_child;
    for (;;) {
        if (n == root)
            return NULL;
        if (n->next)
            return n->next;
        if (n->parent) {
            n = n->parent;
            continue;
        }
        /* the end of a shadow tree: on to its host's children */
        if (!au_is_shadow_root(n) || !((const AuShadowRoot *)n)->host)
            return NULL;
        const AuElement *h = ((const AuShadowRoot *)n)->host;
        if (&h->node == root)
            return h->node.first_child;
        if (h->node.first_child)
            return h->node.first_child;
        n = &h->node;
    }
}

/* ---- The flat tree ---- */

/* The first node assigned to slot, or NULL (no root, an inactive slot). */
static AuNode *first_assigned(const AuElement *slot)
{
    const AuShadowRoot *sr = slot_root(slot);
    if (!sr || !sr->host)
        return NULL;
    if (!sr->manual_slots && !slot_is_active(sr, slot))
        return NULL;
    return next_assigned(sr, slot, NULL);
}

AuNode *au_flat_first_child(const AuNode *n)
{
    if (n->type == AU_NODE_ELEMENT) {
        const AuElement *el = (const AuElement *)n;
        if (el->shadow_root)
            return el->shadow_root->first_child;
        if (is_slot(n)) {
            AuNode *a = first_assigned(el);
            if (a)
                return a;
        }
    }
    return n->first_child;
}

AuNode *au_flat_next_sibling(const AuNode *n)
{
    if (n->parent && n->parent->type == AU_NODE_ELEMENT && ((const AuElement *)n->parent)->shadow_root) {
        AuElement *slot = au_node_assigned_slot(n);
        if (slot)
            return next_assigned(slot_root(slot), slot, n);
        /* not in the flat tree: its siblings are not reached from it */
        return NULL;
    }
    return n->next;
}

AuNode *au_flat_parent(const AuNode *n)
{
    AuNode *p = n->parent;
    if (!p)
        return NULL;
    if (p->type == AU_NODE_ELEMENT && ((const AuElement *)p)->shadow_root) {
        AuElement *slot = au_node_assigned_slot(n);
        if (slot)
            return &slot->node;
    }
    if (au_is_shadow_root(p))
        return ((AuShadowRoot *)p)->host ? &((AuShadowRoot *)p)->host->node : NULL;
    return p;
}

AuElement *au_flat_parent_element(const AuNode *n)
{
    AuNode *p = au_flat_parent(n);
    return p && p->type == AU_NODE_ELEMENT ? (AuElement *)p : NULL;
}

bool au_node_in_flat_tree(const AuNode *n)
{
    AuNode *p = n->parent;
    if (!p || p->type != AU_NODE_ELEMENT)
        return true;
    const AuElement *pe = (const AuElement *)p;
    if (pe->shadow_root)
        return au_node_assigned_slot(n) != NULL;
    if (is_slot(p) && first_assigned(pe))
        return false;
    return true;
}

AuNode *au_flat_next_skip(const AuNode *n, const AuNode *root)
{
    while (n && n != root) {
        AuNode *s = au_flat_next_sibling(n);
        if (s)
            return s;
        n = au_flat_parent(n);
    }
    return NULL;
}

AuNode *au_flat_next(const AuNode *n, const AuNode *root)
{
    AuNode *c = au_flat_first_child(n);
    if (c)
        return c;
    return au_flat_next_skip(n, root);
}
