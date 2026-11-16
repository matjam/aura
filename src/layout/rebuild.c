/* Subtree rebuilds: a DOM change builds the boxes again only below the
 * nearest element whose box can be rebuilt on its own, and layout goes on
 * from there (docs/layout.md, "A DOM change").
 *
 * au_layout_node_changed records the node whose children, text or
 * box-relevant attributes changed. When the styles are current
 * (au_layout_styles_changed, au_layout_update), l_rebuild_dirty finds for
 * each such node the rebuild root: the nearest ancestor-or-self element
 * whose principal box
 *
 * - is a block-level, non-anonymous LK_BLOCK that is not the root box, a
 *   table part (row, row group, cell, column, caption) or a legend: its
 *   content starts after a collapsible space, the white space after it
 *   collapses the same whatever it holds, the anonymous blocks and flex
 *   items its parent wraps around siblings depend only on its being
 *   block-level, and no table grid outside it points into it;
 * - shares no counter and no quote with content outside it
 *   (LX_CTR_OPEN, found by the build: a counter's scope is the element
 *   that creates it, its following siblings and their descendants).
 *
 * The roots' subtrees are dropped, new boxes are built for each root with
 * the current styles (l_build_subtree), given the flags their parent's
 * normalization gives (flex and grid items) and put in place of the old
 * ones. A new box whose outer kind differs from the old one's (block-level,
 * floated, positioned, a list item, a counter property changed) cannot go
 * in place: the whole tree is built instead, as it is for every case this
 * file cannot prove equal to a whole build. */
#include "layout_int.h"

#include <stdio.h>
#include <stdlib.h>

#define L_DIRTY_MAX 64

void l_dirty_clear(AuLayout *L)
{
    for (uint32_t i = 0; i < L->ndirty; i++)
        au_node_unref(L->dirty[i]);
    L->ndirty = 0;
}

void au_layout_node_changed(AuLayout *L, AuNode *n)
{
    if (!n)
        return;
    L->need_paint = true;
    if (L->need_build)
        return; /* the whole tree is built at the next update */
    if (!L->doc || au_node_shadow_including_root(n) != &L->doc->node)
        return; /* outside the document: its insertion is recorded */
    au_svg_id_index_free(L->svg_ids); /* ids may have come or gone */
    L->svg_ids = NULL;
    if (!L->root) {
        L->need_build = true;
        return;
    }
    for (uint32_t i = L->ndirty, k = 0; i-- > 0 && k < 8; k++)
        if (L->dirty[i] == n)
            return;
    if (L->ndirty >= L_DIRTY_MAX) {
        l_dirty_clear(L);
        L->need_build = true;
        return;
    }
    if (L->ndirty == L->dirty_cap) {
        L->dirty_cap = L->dirty_cap ? L->dirty_cap * 2 : 16;
        L->dirty = au_realloc(L->dirty, L->dirty_cap * sizeof *L->dirty);
    }
    L->dirty[L->ndirty++] = au_node_ref(n);
}

/* ---- Choosing the rebuild roots ---- */

static bool style_floated_or_abspos(const AuStyle *s)
{
    return s->box->position == AU_POSITION_ABSOLUTE || s->box->position == AU_POSITION_FIXED ||
           s->box->float_ != AU_FLOAT_NONE;
}

/* The element's box is inline-level before its parent's normalization (an
 * inline-block, an inline replaced element, an inline flex container):
 * its content starts with the white space state of the content before. */
static bool style_inline_level(const AuStyle *s)
{
    return au_display_outer(s->box->display) == AU_DISPLAY_OUTER_INLINE && !style_floated_or_abspos(s);
}

static bool safe_root(const AuLayout *L, const AuElement *el)
{
    const LBox *b = el->box;
    if (!b || b == L->root || b->node != &el->node || b->pseudo || b->kind != LK_BLOCK ||
        (b->flags & (LF_ANON | LF_DEAD | LF_LEGEND)))
        return false;
    if (b->inner >= LI_TABLE_ROW_GROUP && b->inner <= LI_TABLE_COLUMN_GROUP)
        return false;
    if (l_is_caption(b) || au_is_html_element(&el->node, AU_ATOM_legend))
        return false;
    if (((const LBlock *)b)->xf & LX_CTR_OPEN)
        return false;
    if (style_inline_level(b->style))
        return false;
    return true;
}

/* The element whose subtree a change at n rebuilds. Returns NULL with
 * *full false when nothing shows the change (n is not in the document,
 * or below an element with display: none), and NULL with *full set when
 * only a whole build will do. */
static AuElement *find_root(AuLayout *L, AuNode *n, bool *full)
{
    *full = false;
    AuNode *top = n;
    while (top->parent)
        top = top->parent;
    if (top != &L->doc->node)
        return NULL; /* detached, or in a shadow tree (not rendered) */
    AuElement *found = NULL;
    bool hidden = false;
    for (AuNode *p = n; p; p = p->parent) {
        if (p->type == AU_NODE_DOCUMENT)
            break;
        if (p->type != AU_NODE_ELEMENT)
            continue;
        AuElement *e = au_as_element(p);
        const AuStyle *s = au_element_style(e);
        /* Nothing below display: none has a box, but a replaced box can
         * show what is there (a select its options' text), and an element
         * that has a box was shown until this restyle: its box must go. */
        const LBox *eb = e->box;
        if (eb && eb->node == p && eb->replaced)
            hidden = false;
        else if (s && s->box->display == AU_DISPLAY_NONE && !eb)
            hidden = true;
        if (hidden)
            continue;
        if (safe_root(L, e)) {
            found = e;
            break;
        }
    }
    if (hidden)
        return NULL;
    if (!found)
        *full = true;
    return found;
}

/* Counter properties of an element are applied in its parent's context:
 * a root whose own ones changed changes what follows it. */
static bool counters_same(const AuStyle *a, const AuStyle *b)
{
    return a->rare->counter_reset == b->rare->counter_reset &&
           a->rare->counter_increment == b->rare->counter_increment &&
           a->rare->counter_set == b->rare->counter_set &&
           au_display_list_item(a->box->display) == au_display_list_item(b->box->display);
}

static bool is_table_part(const LBox *b)
{
    return b->kind == LK_BLOCK &&
           ((b->inner >= LI_TABLE_ROW_GROUP && b->inner <= LI_TABLE_COLUMN_GROUP) || l_is_caption(b));
}

/* The flags a parent's normalization gives a child (normalize in
 * boxtree.c): flex and grid items. */
static void context_fix(const LBox *parent, LBox *b)
{
    if (parent && parent->kind == LK_BLOCK && (parent->inner == LI_FLEX || parent->inner == LI_GRID) &&
        !(b->flags & LF_ABSPOS)) {
        b->flags |= LF_FLEX_ITEM | LF_BFC;
        b->flags &= ~(LF_FLOAT | LF_INLINE_LEVEL);
    }
}

/* The flags by which a parent's normalization and the walk treat a child. */
#define OUTER_FLAGS (LF_INLINE_LEVEL | LF_FLOAT | LF_ABSPOS | LF_MARKER_OUT | LF_FLEX_ITEM | LF_LEGEND | LF_LIST_ITEM)

/* Whether box b's element is still a descendant of the element of b's
 * nearest non-anonymous ancestor box, with no element that has a box of
 * its own between them. */
static bool in_place(const AuLayout *L, const LBox *b)
{
    const LBox *top = b;
    while (top->parent)
        top = top->parent;
    if (top != L->root)
        return false; /* a box of another document's layout */
    const LBox *pb = b->parent;
    while (pb && ((pb->flags & LF_ANON) || pb->pseudo || !pb->node))
        pb = pb->parent;
    if (!pb || !b->node)
        return false;
    for (const AuNode *p = b->node->parent; p; p = p->parent) {
        if (p == pb->node)
            return true;
        if (p->type == AU_NODE_ELEMENT && ((const AuElement *)p)->box)
            return false;
    }
    return false;
}

typedef struct RScroll {
    AuElement *el; /* referenced */
    float x, y;
} RScroll;

typedef struct RRoot {
    AuElement *el; /* referenced */
    LBox *old;     /* the old principal box, kept in place until the new one replaces it */
} RRoot;

/* Saves the scroll offsets of the scrolled boxes in b's subtree. */
static void save_scrolls(LBox *b, RScroll **v, uint32_t *n, uint32_t *cap)
{
    LBox *c = b;
    while (c) {
        if (c->kind == LK_BLOCK && c->node && au_is_element(c->node) && au_as_element(c->node)->box == c) {
            LBlock *k = (LBlock *)c;
            if (k->scroll_x != 0 || k->scroll_y != 0) {
                if (*n == *cap) {
                    *cap = *cap ? *cap * 2 : 8;
                    *v = au_realloc(*v, *cap * sizeof **v);
                }
                (*v)[*n].el = au_as_element(au_node_ref(c->node));
                (*v)[*n].x = k->scroll_x;
                (*v)[*n].y = k->scroll_y;
                (*n)++;
            }
        }
        if (c->first && c->kind != LK_TEXT)
            c = c->first;
        else {
            while (c != b && !c->next)
                c = c->parent;
            c = c == b ? NULL : c->next;
        }
    }
}

static void replace_child(LBox *parent, LBox *old, LBox *nb)
{
    LBox *prev = NULL;
    for (LBox *c = parent->first; c && c != old; c = c->next)
        prev = c;
    nb->parent = parent;
    nb->next = old->next;
    if (prev)
        prev->next = nb;
    else
        parent->first = nb;
    if (parent->last == old)
        parent->last = nb;
    old->next = NULL;
    old->parent = NULL;
}

static void drop_children(LBox *b)
{
    LBox *c = b->first;
    b->first = b->last = NULL;
    while (c) {
        LBox *next = c->next;
        l_drop_box(c);
        c = next;
    }
}

void l_rebuild_dirty(AuLayout *L)
{
    if (!L->ndirty)
        return;
    /* The differential check also covers changes found to need no
     * rebuild (below display: none). */
    L->rebuilt = true;
    if (L->need_build || !L->root || !L->doc) {
        l_dirty_clear(L);
        return;
    }
    /* Dropped boxes stay in the arena until a whole build. Once they
     * would make it more than twice the size of the last whole build,
     * build the whole tree, which frees them. */
    if (L->tree.total > 2 * L->tree_full_bytes + 256 * 1024) {
        l_dirty_clear(L);
        L->need_build = true;
        return;
    }
    RRoot roots[L_DIRTY_MAX];
    uint32_t nroots = 0;
    bool full = false;
    for (uint32_t i = 0; i < L->ndirty && !full; i++) {
        bool f;
        AuElement *el = find_root(L, L->dirty[i], &f);
        if (f)
            full = true;
        if (!el)
            continue;
        if (!in_place(L, el->box)) {
            full = true;
            break;
        }
        bool dup = false;
        for (uint32_t k = 0; k < nroots && !dup; k++)
            dup = roots[k].el == el;
        if (!dup) {
            roots[nroots].el = el;
            roots[nroots].old = el->box;
            nroots++;
        }
    }
    l_dirty_clear(L);
    if (full) {
        L->need_build = true;
        return;
    }
    if (!nroots)
        return;
    /* A root inside another root's subtree goes with it. */
    for (uint32_t i = 0; i < nroots; i++)
        ((LBlock *)roots[i].old)->xf |= LX_MARK;
    uint32_t n = 0;
    for (uint32_t i = 0; i < nroots; i++) {
        bool inside = false;
        for (LBox *p = roots[i].old->parent; p && !inside; p = p->parent)
            inside = p->kind == LK_BLOCK && (((LBlock *)p)->xf & LX_MARK);
        if (!inside)
            roots[n++] = roots[i];
    }
    for (uint32_t i = 0; i < nroots; i++)
        ((LBlock *)roots[i].old)->xf &= (uint8_t)~LX_MARK;
    nroots = n;
    /* What a root's own style decides outside its box must not change.
     * (in_place above: its old box must still be where its element is,
     * since an element that moved gets its new box from the root it
     * moved into.) */
    for (uint32_t i = 0; i < nroots; i++) {
        const AuStyle *ns = au_element_style(roots[i].el);
        const AuStyle *os = roots[i].old->style;
        if (!ns || !counters_same(os, ns) || style_inline_level(ns) || ns->box->display == AU_DISPLAY_CONTENTS) {
            L->need_build = true;
            return;
        }
    }
    for (uint32_t i = 0; i < nroots; i++)
        au_node_ref(&roots[i].el->node);
    /* Drop every old subtree before building any new one: a node that
     * moved from one root to another must not keep its old box. */
    RScroll *scrolls = NULL;
    uint32_t nscrolls = 0, scrolls_cap = 0;
    for (uint32_t i = 0; i < nroots; i++) {
        LBox *ob = roots[i].old;
        save_scrolls(ob, &scrolls, &nscrolls, &scrolls_cap);
        drop_children(ob);
        l_box_free_layout(ob);
        if (roots[i].el->box == ob)
            roots[i].el->box = NULL;
    }
    bool failed = false;
    uint32_t boxes0 = L->stats.boxes;
    for (uint32_t i = 0; i < nroots && !failed; i++) {
        LBox *ob = roots[i].old;
        LBox *nb = l_build_subtree(L, roots[i].el);
        if (!nb) {
            failed = true;
            break;
        }
        context_fix(ob->parent, nb);
        if (nb->kind != ob->kind || (nb->flags & OUTER_FLAGS) != (ob->flags & OUTER_FLAGS) || is_table_part(nb) ||
            nb == L->root || !ob->parent) {
            l_drop_box(nb);
            failed = true;
            break;
        }
        replace_child(ob->parent, ob, nb);
        l_drop_box(ob);
        roots[i].old = NULL;
        /* Every ancestor: l_mark_dirty stops at one already marked, and
         * a box layout did not reach (inside a cell or float laid out
         * from a cache) can keep its mark while its ancestors lost it. */
        l_mark_dirty(nb);
        for (LBox *p = nb->parent; p; p = p->parent) {
            p->flags |= LF_CHILD_DIRTY;
            p->flags &= ~(LF_INTRINSIC_OK | LF_MEASURE_OK);
            l_table_forget(p);
        }
    }
    L->subtree_boxes += L->stats.boxes - boxes0;
    /* The display: none elements of the dropped boxes are gone from the
     * tree; the new subtrees listed theirs. */
    uint32_t k = 0;
    for (uint32_t i = 0; i < L->nhidden; i++) {
        if (L->hidden[i].box->flags & LF_DEAD)
            au_node_unref(&L->hidden[i].el->node);
        else
            L->hidden[k++] = L->hidden[i];
    }
    L->nhidden = k;
    for (uint32_t i = 0; i < nscrolls; i++) {
        LBox *b = scrolls[i].el->box;
        if (!failed && b && b->kind == LK_BLOCK && (b->flags & LF_SCROLLER)) {
            ((LBlock *)b)->scroll_x = scrolls[i].x;
            ((LBlock *)b)->scroll_y = scrolls[i].y;
        }
        au_node_unref(&scrolls[i].el->node);
    }
    au_free(scrolls);
    for (uint32_t i = 0; i < nroots; i++)
        au_node_unref(&roots[i].el->node);
    if (failed) {
        L->need_build = true;
        return;
    }
    L->subtree_builds++;
    L->rebuilt = true;
    L->need_layout = true;
    L->need_restack = true;
    L->need_paint = true;
}

/* ---- The differential check ----
 * After an update that rebuilt a subtree, a second layout builds the
 * whole tree from the same document and styles and lays it out at the
 * same viewport; the two trees must be equal box for box: kinds, flags,
 * nodes, styles, text, replaced content, geometry, lines and fragments,
 * the element box pointers and the list of display: none elements. */

typedef struct CPair {
    const LBox *full, *incr;
} CPair;

static int cpair_cmp(const void *a, const void *b)
{
    const CPair *x = a, *y = b;
    return x->full < y->full ? -1 : x->full > y->full;
}

static int ptr_cmp(const void *a, const void *b)
{
    const void *x = *(const void *const *)a, *y = *(const void *const *)b;
    return x < y ? -1 : x > y;
}

static bool feq(float a, float b)
{
    if (a == b || (a != a && b != b))
        return true;
    float d = a - b;
    return d < 0.01f && d > -0.01f;
}

static bool seq(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

static void box_name(const LBox *b, AuBuf *o)
{
    static const char *const kinds[] = {"block", "inline", "text", "br", "wbr"};
    au_buf_puts(o, b->kind <= LK_WBR ? kinds[b->kind] : "?");
    if (b->flags & LF_ANON)
        au_buf_puts(o, "(anon)");
    if (b->node && au_is_element(b->node)) {
        const AuElement *e = au_as_element(b->node);
        au_buf_printf(o, "<%s", au_atom_str(e->tag));
        if (e->id)
            au_buf_printf(o, "#%s", au_atom_str(e->id));
        au_buf_putc(o, '>');
    }
    if (b->pseudo)
        au_buf_printf(o, "::%u", (unsigned)b->pseudo);
}

static void box_path(const LBox *b, AuBuf *o)
{
    const LBox *chain[12];
    int n = 0;
    for (const LBox *p = b; p && n < 12; p = p->parent)
        chain[n++] = p;
    while (n--) {
        box_name(chain[n], o);
        if (n)
            au_buf_puts(o, " > ");
    }
}

#define CHECK_FLAGS                                                                                                   \
    (~(uint32_t)(LF_DIRTY | LF_CHILD_DIRTY | LF_INTRINSIC_OK | LF_HAS_LAYOUT | LF_MEASURE_OK | LF_ABS_QUEUED |        \
                 LF_ESCAPES | LF_DEAD))

/* Compares one box of the incremental tree (a) with its counterpart in
 * the whole build (b); writes what differs to msg. */
static bool cmp_box(const LBox *a, const LBox *b, bool geom, AuBuf *msg)
{
    const char *what = NULL;
    if (a->kind != b->kind || a->inner != b->inner || a->replaced != b->replaced || a->pseudo != b->pseudo)
        what = "kind";
    else if (a->node != b->node)
        what = "node";
    else if (a->style != b->style)
        what = "style";
    else if ((a->flags & CHECK_FLAGS) != (b->flags & CHECK_FLAGS))
        what = "flags";
    else if ((a->first == NULL) != (b->first == NULL) || (a->next == NULL) != (b->next == NULL))
        what = "children";
    else if (a->kind == LK_TEXT) {
        const LText *x = (const LText *)a, *y = (const LText *)b;
        if (x->len != y->len || memcmp(x->text, y->text, x->len) != 0)
            what = "text";
    } else if (a->kind == LK_BLOCK) {
        const LBlock *x = (const LBlock *)a, *y = (const LBlock *)b;
        bool g = feq(x->x, y->x) && feq(x->y, y->y) && feq(x->w, y->w) && feq(x->h, y->h) &&
                 feq(x->baseline, y->baseline) && feq(x->last_baseline, y->last_baseline);
        for (int i = 0; i < 4 && g; i++)
            g = feq(x->m[i], y->m[i]) && feq(x->p[i], y->p[i]) && feq(x->bw[i], y->bw[i]);
        if ((x->xf & LX_CTR_OPEN) != (y->xf & LX_CTR_OPEN))
            what = "counter scope";
        else if (!geom)
            ; /* the tree only: lines and fragments follow the geometry */
        else if (!g)
            what = "geometry";
        else if (!(feq(x->ov.x, y->ov.x) && feq(x->ov.y, y->ov.y) && feq(x->ov.w, y->ov.w) && feq(x->ov.h, y->ov.h)))
            what = "overflow";
        else if ((a->flags & LF_CLIPS) && !(feq(x->sw, y->sw) && feq(x->sh, y->sh)))
            what = "scroll extent";
        else if (x->nlines != y->nlines || x->nfrags != y->nfrags)
            what = "line count";
        else if ((x->rep == NULL) != (y->rep == NULL))
            what = "replaced data";
        else if (x->rep) {
            const LReplaced *r = x->rep, *s = y->rep;
            if (!seq(r->src, s->src) || !seq(r->label, s->label) || !seq(r->aux, s->aux) || !seq(r->alt, s->alt) ||
                r->rows != s->rows || r->cols != s->cols || r->loaded != s->loaded || !feq(r->iw, s->iw) ||
                !feq(r->ih, s->ih) || r->marker != s->marker || r->placeholder != s->placeholder ||
                !feq(r->density, s->density))
                what = "replaced data";
        }
        for (uint32_t i = 0; geom && !what && i < x->nlines; i++) {
            const LLine *p = &x->lines[i], *q = &y->lines[i];
            if (!feq(p->y, q->y) || !feq(p->h, q->h) || !feq(p->baseline, q->baseline) || !feq(p->x, q->x) ||
                !feq(p->w, q->w) || p->first != q->first || p->n != q->n)
                what = "line";
        }
        for (uint32_t i = 0; geom && !what && i < x->nfrags; i++) {
            const LFrag *p = &x->frags[i], *q = &y->frags[i];
            if (p->kind != q->kind || p->flags != q->flags || p->depth != q->depth || p->start != q->start ||
                p->len != q->len || !feq(p->x, q->x) || !feq(p->y, q->y) || !feq(p->w, q->w) || !feq(p->h, q->h) ||
                !feq(p->baseline, q->baseline) || !feq(p->spacing, q->spacing) || p->box->node != q->box->node ||
                p->box->kind != q->box->kind || p->box->pseudo != q->box->pseudo)
                what = "fragment";
        }
        if (!what && a->inner == LI_TABLE) {
            const LTableGrid *p = ((const LTableBox *)a)->g, *q = ((const LTableBox *)b)->g;
            if ((p == NULL) != (q == NULL) ||
                (p && (p->ncells != q->ncells || p->nrows != q->nrows || p->ncols != q->ncols)))
                what = "table grid";
        }
    }
    if (!what)
        return true;
    au_buf_printf(msg, "%s differs at ", what);
    box_path(a, msg);
    if (a->kind != b->kind || a->inner != b->inner || a->replaced != b->replaced || a->pseudo != b->pseudo)
        au_buf_printf(msg, " (incremental kind %u inner %u replaced %u pseudo %u; whole %u %u %u %u)",
                      (unsigned)a->kind, (unsigned)a->inner, (unsigned)a->replaced, (unsigned)a->pseudo,
                      (unsigned)b->kind, (unsigned)b->inner, (unsigned)b->replaced, (unsigned)b->pseudo);
    if (a->kind == LK_BLOCK && b->kind == LK_BLOCK) {
        const LBlock *x = (const LBlock *)a, *y = (const LBlock *)b;
        au_buf_printf(msg, " (incremental %g,%g %gx%g m %g %g %g %g; whole %g,%g %gx%g m %g %g %g %g)", (double)x->x,
                      (double)x->y, (double)x->w, (double)x->h, (double)x->m[0], (double)x->m[1], (double)x->m[2],
                      (double)x->m[3], (double)y->x, (double)y->y, (double)y->w, (double)y->h, (double)y->m[0],
                      (double)y->m[1], (double)y->m[2], (double)y->m[3]);
    }
    au_buf_puts(msg, "\n");
    return false;
}

void l_check_incremental(AuLayout *L)
{
    if (!L->rebuilt || !L->doc || L->need_build || L->need_layout)
        return;
    L->rebuilt = false;
    L->check_runs++;
    /* The whole build sets the element box pointers: keep the real ones. */
    const AuNode *top = &L->doc->node;
    uint32_t nel = 0;
    for (AuNode *n = top->first_child; n; n = au_node_next(n, top))
        nel += n->type == AU_NODE_ELEMENT;
    void **saved = au_alloc((nel ? nel : 1) * sizeof *saved);
    uint32_t i = 0;
    for (AuNode *n = top->first_child; n; n = au_node_next(n, top))
        if (n->type == AU_NODE_ELEMENT) {
            saved[i++] = au_as_element(n)->box;
            au_as_element(n)->box = NULL;
        }
    AuLayout *F = au_layout_new(&L->env);
    F->check = 0; /* the whole build is not checked again */
    au_layout_set_document(F, L->doc);
    F->transparent = L->transparent;
    F->src_vw = L->src_vw;
    F->src_vh = L->src_vh;
    au_layout_update(F, L->vw, L->vh);
    AuBuf msg;
    au_buf_init(&msg);
    bool ok = true;
    bool geom = !L->other_relayout;
    if (geom && (!feq(F->doc_w, L->doc_w) || !feq(F->doc_h, L->doc_h))) {
        au_buf_printf(&msg, "document size differs: %g x %g, whole build %g x %g\n", (double)L->doc_w,
                      (double)L->doc_h, (double)F->doc_w, (double)F->doc_h);
        ok = false;
    }
    /* Box for box, in preorder; the pairs map the whole build's boxes to
     * the incremental ones for the element pointers. */
    CPair *pairs = NULL;
    uint32_t npairs = 0, pcap = 0;
    const LBox *a = L->root, *b = F->root;
    if ((a == NULL) != (b == NULL)) {
        au_buf_puts(&msg, "one tree is empty\n");
        ok = false;
    }
    while (ok && a && b) {
        if (!cmp_box(a, b, geom, &msg)) {
            ok = false;
            break;
        }
        if (npairs == pcap) {
            pcap = pcap ? pcap * 2 : 1024;
            pairs = au_realloc(pairs, pcap * sizeof *pairs);
        }
        pairs[npairs].full = b;
        pairs[npairs].incr = a;
        npairs++;
        if (a->first && a->kind != LK_TEXT) {
            a = a->first;
            b = b->first;
        } else {
            while (a && !a->next) {
                a = a->parent;
                b = b->parent;
            }
            if (a) {
                a = a->next;
                b = b->next;
            }
        }
    }
    if (ok) {
        qsort(pairs, npairs, sizeof *pairs, cpair_cmp);
        i = 0;
        for (AuNode *n = top->first_child; n && ok; n = au_node_next(n, top)) {
            if (n->type != AU_NODE_ELEMENT)
                continue;
            const LBox *fb = au_as_element(n)->box, *mine = saved[i++];
            const LBox *want = NULL;
            if (fb) {
                CPair key = {fb, NULL};
                const CPair *p = bsearch(&key, pairs, npairs, sizeof *pairs, cpair_cmp);
                want = p ? p->incr : NULL;
            }
            if (want != mine) {
                au_buf_printf(&msg, "the box pointer of <%s> differs: ", au_atom_str(au_as_element(n)->tag));
                if (mine)
                    box_path(mine, &msg);
                else
                    au_buf_puts(&msg, "none");
                au_buf_puts(&msg, ", whole build ");
                if (want)
                    box_path(want, &msg);
                else
                    au_buf_puts(&msg, fb ? "a box with no counterpart" : "none");
                au_buf_puts(&msg, "\n");
                ok = false;
            }
        }
    }
    if (ok) {
        /* The display: none elements, as sets. */
        uint32_t nh = L->nhidden, fh = F->nhidden;
        void **x = au_alloc((nh + fh + 1) * sizeof *x);
        uint32_t ux = 0, uy = 0;
        for (uint32_t k = 0; k < nh; k++)
            x[ux++] = L->hidden[k].el;
        void **y = x + nh;
        for (uint32_t k = 0; k < fh; k++)
            y[uy++] = F->hidden[k].el;
        qsort(x, ux, sizeof *x, ptr_cmp);
        qsort(y, uy, sizeof *y, ptr_cmp);
        uint32_t m = 0;
        for (uint32_t k = 0; k < ux; k++)
            if (!k || x[k] != x[k - 1])
                x[m++] = x[k];
        ux = m;
        if (ux != uy || memcmp(x, y, ux * sizeof *x) != 0) {
            au_buf_printf(&msg, "the display: none elements differ (%u, whole build %u)\n", ux, uy);
            ok = false;
        }
        au_free(x);
    }
    au_free(pairs);
    au_layout_free(F);
    i = 0;
    for (AuNode *n = top->first_child; n; n = au_node_next(n, top))
        if (n->type == AU_NODE_ELEMENT)
            au_as_element(n)->box = saved[i++];
    au_free(saved);
    if (!ok) {
        L->check_failures++;
        if (!L->check_msg.len)
            au_buf_append(&L->check_msg, msg.data, msg.len);
#ifdef AURA_INCR_CHECK
        fprintf(stderr, "aura: incremental box tree differs from a whole build: %.*s", (int)msg.len, msg.data);
        abort();
#endif
    }
    au_buf_free(&msg);
}

void au_layout_set_check(AuLayout *L, bool on)
{
    L->check = on;
}

uint32_t au_layout_check_failures(const AuLayout *L, AuBuf *msg)
{
    if (msg && L->check_msg.len)
        au_buf_append(msg, L->check_msg.data, L->check_msg.len);
    return L->check_failures;
}

void au_layout_subtree_counts(const AuLayout *L, uint32_t *rebuilds, uint32_t *boxes, uint32_t *checks)
{
    *rebuilds = L->subtree_builds;
    *boxes = L->subtree_boxes;
    *checks = L->check_runs;
}
