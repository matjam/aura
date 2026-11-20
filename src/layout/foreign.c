/* The HTML content of foreignObjects in inline SVG: a box subtree per
 * object, kept beside the box tree (layout_int.h, "foreignObject
 * content"; docs/svg.md). The box tree build notes each svg box
 * (l_foreign_note); l_foreign_sync, at the start of each update, drops
 * the roots whose svg box was dropped or that a restyle made stale and
 * builds the roots of the noted svg boxes. layout.c lays the roots out
 * and compares their styles; draw.c paints a root when the SVG renderer
 * reaches its object. */
#include "layout_int.h"

void l_foreign_note(AuLayout *L, LBox *svg)
{
    if (L->nfpend >= L_FOREIGN_MAX)
        return; /* more svg boxes than roots could be made for */
    if (L->nfpend == L->fpend_cap) {
        L->fpend_cap = L->fpend_cap ? L->fpend_cap * 2 : 32;
        L->fpend = au_realloc(L->fpend, L->fpend_cap * sizeof *L->fpend);
    }
    L->fpend[L->nfpend].svg = svg;
    L->fpend[L->nfpend].depth = L->foreign_depth;
    L->nfpend++;
}

static void drop_entry(LForeign *e)
{
    if (e->root)
        l_drop_box(e->root);
    for (uint32_t i = 0; i < e->nhid; i++) {
        au_node_unref(&e->hid[i].el->node);
        au_style_release(e->hid[i].style);
    }
    au_free(e->hid);
    au_node_unref(&e->fo->node);
    memset(e, 0, sizeof *e);
}

void l_foreign_free(AuLayout *L)
{
    for (uint32_t i = 0; i < L->nforeign; i++)
        drop_entry(&L->foreign[i]);
    L->nforeign = 0;
    L->nfpend = 0;
}

LForeign *l_foreign_find(AuLayout *L, const AuElement *fo)
{
    for (uint32_t i = 0; i < L->nforeign; i++) {
        LForeign *e = &L->foreign[i];
        if (e->fo == fo && l_foreign_live(e) && !e->stale && e->w > 0 &&
            (e->root->flags & LF_HAS_LAYOUT))
            return e;
    }
    return NULL;
}

/* Builds the root of one object of svg box svg. */
static void build_one(AuLayout *L, LBox *svg, AuElement *fo, uint8_t depth)
{
    if (L->nforeign >= L_FOREIGN_MAX || au_svg_foreign_img(fo))
        return;
    uint32_t nhidden0 = L->nhidden;
    L->foreign_depth = (uint8_t)(depth + 1);
    LBox *root = l_build_foreign(L, fo);
    L->foreign_depth = 0;
    /* The boxless elements it met belong to the root, not to the tree's
     * list (which a rebuild of the tree compares with a whole build). */
    struct LHidden *hid = NULL;
    uint32_t nhid = L->nhidden - nhidden0;
    if (nhid) {
        hid = au_alloc(nhid * sizeof *hid);
        memcpy(hid, L->hidden + nhidden0, nhid * sizeof *hid);
        L->nhidden = nhidden0;
    }
    if (!root) {
        for (uint32_t i = 0; i < nhid; i++) {
            au_node_unref(&hid[i].el->node);
            au_style_release(hid[i].style);
        }
        au_free(hid);
        return;
    }
    if (L->nforeign == L->foreign_cap) {
        L->foreign_cap = L->foreign_cap ? L->foreign_cap * 2 : 16;
        L->foreign = au_realloc(L->foreign, L->foreign_cap * sizeof *L->foreign);
    }
    LForeign *e = &L->foreign[L->nforeign++];
    memset(e, 0, sizeof *e);
    e->fo = au_as_element(au_node_ref(&fo->node));
    e->svg = svg;
    e->root = root;
    e->hid = hid;
    e->nhid = nhid;
    e->depth = depth;
}

/* Builds the roots of the foreignObjects under svg box svg's element (not
 * those inside another foreignObject: their svg boxes are noted when the
 * object's root is built). */
static void build_for_svg(AuLayout *L, LBox *svg, uint8_t depth)
{
    if (depth >= L_FOREIGN_DEPTH || !svg->node || !au_is_element(svg->node))
        return;
    AuAtom fo_tag = au_atom_cstr("foreignObject");
    AuNode *top = svg->node, *c = top->first_child;
    while (c) {
        bool descend = false;
        if (c->type == AU_NODE_ELEMENT) {
            AuElement *e = au_as_element(c);
            if (e->ns == AU_NS_SVG && e->tag == fo_tag)
                build_one(L, svg, e, depth);
            else
                descend = e->ns == AU_NS_SVG && c->first_child;
        }
        if (descend) {
            c = c->first_child;
            continue;
        }
        while (c != top && !c->next)
            c = c->parent;
        c = c == top ? NULL : c->next;
    }
}

bool l_foreign_sync(AuLayout *L)
{
    bool changed = false;
    /* A stale root goes with every other root of its svg, which is built
     * again (LX_MARK on the svg box, cleared below). */
    uint32_t pend0 = L->nfpend;
    for (uint32_t i = 0; i < L->nforeign; i++) {
        LForeign *e = &L->foreign[i];
        LBlock *sk = (LBlock *)e->svg;
        if (!e->stale || (e->svg->flags & LF_DEAD) || (sk->xf & LX_MARK))
            continue;
        uint32_t np = L->nfpend;
        L->foreign_depth = e->depth;
        l_foreign_note(L, e->svg);
        L->foreign_depth = 0;
        if (L->nfpend > np)
            sk->xf |= LX_MARK;
        else
            e->stale = false; /* no room to build it again: it stays as it was */
    }
    /* In order: a root nested in another comes after it, so dropping the
     * outer one (whose svg boxes then are dead) drops it in this pass. */
    uint32_t k = 0;
    for (uint32_t i = 0; i < L->nforeign; i++) {
        LForeign *e = &L->foreign[i];
        if ((e->svg->flags & LF_DEAD) || (((LBlock *)e->svg)->xf & LX_MARK)) {
            drop_entry(e);
            changed = true;
        } else {
            L->foreign[k++] = *e;
        }
    }
    L->nforeign = k;
    for (uint32_t i = pend0; i < L->nfpend; i++)
        ((LBlock *)L->fpend[i].svg)->xf &= (uint8_t)~LX_MARK;
    /* The noted svg boxes, in order; building a root notes the svg boxes
     * in it, after these. */
    for (uint32_t i = 0; i < L->nfpend; i++) {
        LBox *svg = L->fpend[i].svg;
        if (svg->flags & LF_DEAD)
            continue;
        uint32_t n0 = L->nforeign;
        build_for_svg(L, svg, L->fpend[i].depth);
        if (L->nforeign != n0)
            changed = true;
    }
    L->nfpend = 0;
    return changed;
}
