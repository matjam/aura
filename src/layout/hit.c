/* Hit testing: the reverse of the painting order, with the same clips,
 * scroll offsets and stacking contexts; and scrolling of inner scroll
 * containers. */
#include "layout_int.h"

void l_transform_offset(const LBlock *b, float *dx, float *dy);

typedef struct HCtx {
    AuLayout *L;
    float x, y;
    float scroll_x, scroll_y;
    LBox *box;     /* result */
    LFrag *frag;   /* text fragment hit, or NULL */
    LBlock *froot; /* its IFC root */
    float fx;      /* point x relative to the fragment */
    float bx, by;  /* result box origin (canvas space) */
    int depth;
} HCtx;

enum { PH_BG, PH_FLOAT, PH_INLINE };

static bool inside(const HCtx *H, float x, float y, float w, float h)
{
    return H->x >= x && H->x < x + w && H->y >= y && H->y < y + h;
}

static bool targetable(const LBox *b)
{
    const AuStyle *s = b->style;
    if (!s)
        return false;
    if (s->inhr->pointer_events == AU_POINTER_EVENTS_NONE)
        return false;
    if (s->inh->visibility != AU_VISIBILITY_VISIBLE)
        return false;
    /* An inert node is hit tested as if it had pointer-events: none. */
    return !l_box_inert(b);
}

/* Whether b and its subtree can be passed over: b is inert. An inert box
 * holds content that is not inert only through the top layer (a modal
 * dialog inside an inert ancestor), and top-layer boxes are layers of
 * the root's list, so only the root must still be entered. */
static bool skip_inert(const HCtx *H, const LBox *b)
{
    return b != H->L->root && l_box_inert(b);
}

static bool clip_content(HCtx *H, LBlock *b, float ox, float oy, float *cx, float *cy)
{
    *cx = ox;
    *cy = oy;
    if (!(b->b.flags & LF_CLIPS))
        return true;
    float x = ox + b->bw[3], y = oy + b->bw[0];
    float w = b->w - b->bw[1] - b->bw[3], h = b->h - b->bw[0] - b->bw[2];
    const AuStyleBox *s = b->b.style->box;
    bool okx = s->overflow_x == AU_OVERFLOW_VISIBLE || (H->x >= x && H->x < x + w);
    bool oky = s->overflow_y == AU_OVERFLOW_VISIBLE || (H->y >= y && H->y < y + h);
    *cx = ox - b->scroll_x;
    *cy = oy - b->scroll_y;
    return okx && oky;
}

/* Collects up to cap children of parent; returns the count (heap array in
 * *heap when more). */
static LBox **children(LBox *parent, LBox **local, int cap, int *n, LBox ***heap)
{
    int c = 0;
    for (LBox *x = parent->first; x; x = x->next)
        c++;
    LBox **arr = local;
    *heap = NULL;
    if (c > cap) {
        arr = au_alloc((size_t)c * sizeof *arr);
        *heap = arr;
    }
    int i = 0;
    for (LBox *x = parent->first; x; x = x->next)
        arr[i++] = x;
    *n = c;
    return arr;
}

static bool hit_atomic(HCtx *H, LBlock *k, float ox, float oy);

static LBox *frag_owner(const LFrag *f, const LBlock *root)
{
    for (LBox *p = f->box; p && p != &root->b; p = p->parent)
        if (p->flags & LF_LAYERED)
            return p;
    return NULL;
}

static bool hit_lines(HCtx *H, LBlock *b, float ox, float oy, LBox *owner)
{
    for (uint32_t li = b->nlines; li-- > 0;) {
        LLine *ln = &b->lines[li];
        for (uint32_t i = ln->first + ln->n; i-- > ln->first;) {
            LFrag *f = &b->frags[i];
            if ((f->flags & FRF_HIDDEN) || frag_owner(f, b) != owner)
                continue;
            if (f->kind == FR_ATOMIC) {
                LBlock *k = (LBlock *)f->box;
                if (!(k->b.flags & LF_LAYERED) && hit_atomic(H, k, ox + k->x, oy + k->y))
                    return true;
                continue;
            }
            if (!inside(H, ox + f->x, oy + f->y, f->w, f->h))
                continue;
            if (f->kind == FR_TEXT) {
                if (!targetable(f->box))
                    continue;
                H->box = f->box;
                H->frag = f;
                H->froot = b;
                H->fx = H->x - (ox + f->x);
                H->bx = ox + f->x;
                H->by = oy + f->y;
                return true;
            }
            if ((f->kind == FR_INLINE || f->kind == FR_BR) && targetable(f->box)) {
                H->box = f->box;
                H->bx = ox + f->x;
                H->by = oy + f->y;
                return true;
            }
        }
    }
    return false;
}

static bool hit_flow(HCtx *H, LBox *parent, float ox, float oy, int phase);

/* One in-flow block child c of a block at (ox, oy). */
static bool hit_block_child(HCtx *H, LBox *c, float kx, float ky, int phase)
{
    LBlock *k = (LBlock *)c;
    float cx, cy;
    if (skip_inert(H, c))
        return false;
    if (clip_content(H, k, kx, ky, &cx, &cy)) {
        if (hit_flow(H, c, cx, cy, phase) ||
            (phase == PH_INLINE && (c->flags & LF_INLINES) && hit_lines(H, k, cx, cy, NULL)))
            return true;
    }
    if (phase == PH_BG && inside(H, kx, ky, k->w, k->h) && targetable(c) && !lb_anon(c)) {
        H->box = c;
        H->bx = kx;
        H->by = ky;
        return true;
    }
    return false;
}

/* The cells of table t at (ox, oy), in reverse painting order, without
 * recursing through rows and row groups. The spacing between cells hits
 * the table, as in Chrome. */
static bool hit_table(HCtx *H, LTableBox *t, float ox, float oy, int phase)
{
    LTableGrid *g = t->g;
    if (!t->colx)
        return false;
    for (uint32_t i = g->ncells; i-- > 0;) {
        LCellBox *cb = g->cells[i];
        LBlock *k = &cb->k;
        if ((k->b.flags & (LF_LAYERED | LF_TOO_DEEP)) || !cb->colspan)
            continue;
        const LTableRow *r = &g->rows[cb->row];
        if ((r->box->b.flags & LF_LAYERED) || (r->group && (r->group->b.flags & LF_LAYERED)))
            continue;
        float x = ox + r->box->x + k->x, y = oy + r->box->y + k->y;
        if (r->group) {
            x += r->group->x;
            y += r->group->y;
        }
        if (!inside(H, x + k->ov.x, y + k->ov.y, k->ov.w, k->ov.h))
            continue;
        if (hit_block_child(H, &k->b, x, y, phase))
            return true;
    }
    return false;
}

static bool hit_flow(HCtx *H, LBox *parent, float ox, float oy, int phase)
{
    if (H->depth > L_MAX_DEPTH)
        return false;
    H->depth++;
    LBox *local[32], **heap;
    int n;
    LBox **arr = children(parent, local, 32, &n, &heap);
    bool hit = false;
    bool table = parent->kind == LK_BLOCK && parent->inner == LI_TABLE && ((LTableBox *)parent)->g;
    for (int i = n; i-- > 0 && !hit;) {
        LBox *c = arr[i];
        if (table && c->kind == LK_BLOCK && c->inner >= LI_TABLE_ROW_GROUP && c->inner <= LI_TABLE_COLUMN_GROUP &&
            c->inner != LI_TABLE_CELL)
            continue;
        if (c->kind == LK_INLINE) {
            if (phase == PH_FLOAT && !(c->flags & LF_LAYERED) && !skip_inert(H, c))
                hit = hit_flow(H, c, ox, oy, phase);
            continue;
        }
        if (c->kind != LK_BLOCK || (c->flags & (LF_LAYERED | LF_TOO_DEEP)))
            continue;
        LBlock *k = (LBlock *)c;
        float kx = ox + k->x, ky = oy + k->y;
        if (c->flags & LF_FLOAT) {
            if (phase == PH_FLOAT)
                hit = hit_atomic(H, k, kx, ky);
            continue;
        }
        if (c->flags & LF_MARKER_OUT) {
            if (phase == PH_INLINE)
                hit = hit_atomic(H, k, kx, ky);
            continue;
        }
        if (c->flags & LF_INLINE_LEVEL)
            continue;
        if (hit_block_child(H, c, kx, ky, phase))
            hit = true;
    }
    if (!hit && table)
        hit = hit_table(H, (LTableBox *)parent, ox, oy, phase);
    au_free(heap);
    H->depth--;
    return hit;
}

static bool hit_contents(HCtx *H, LBlock *k, float cx, float cy)
{
    if (hit_flow(H, &k->b, cx, cy, PH_INLINE))
        return true;
    if ((k->b.flags & LF_INLINES) && hit_lines(H, k, cx, cy, NULL))
        return true;
    if (hit_flow(H, &k->b, cx, cy, PH_FLOAT))
        return true;
    if (hit_flow(H, &k->b, cx, cy, PH_BG))
        return true;
    return false;
}

static bool hit_atomic(HCtx *H, LBlock *k, float ox, float oy)
{
    if ((k->b.flags & LF_TOO_DEEP) || skip_inert(H, &k->b))
        return false;
    if (!inside(H, ox + k->ov.x, oy + k->ov.y, k->ov.w, k->ov.h))
        return false;
    if (k->b.inner != LI_REPLACED) {
        float cx, cy;
        if (clip_content(H, k, ox, oy, &cx, &cy) && hit_contents(H, k, cx, cy))
            return true;
    }
    if (inside(H, ox, oy, k->w, k->h) && targetable(&k->b)) {
        H->box = &k->b;
        H->bx = ox;
        H->by = oy;
        return true;
    }
    return false;
}

static bool hit_layer(HCtx *H, LBox *b, float ox, float oy);

/* Like draw.c's layer_origin, for hit testing: returns false when an
 * ancestor's clip excludes the point. */
static bool layer_origin(HCtx *H, LBox *sc, float sx, float sy, LBox *b, float *ox, float *oy)
{
    LBox *chain[L_MAX_DEPTH + 1];
    int n = 0;
    for (LBox *p = b->parent; p && p != sc && n < L_MAX_DEPTH; p = p->parent)
        chain[n++] = p;
    bool fixed = (b->flags & LF_FIXED) && b->kind == LK_BLOCK && ((LBlock *)b)->cb == NULL;
    bool applies[L_MAX_DEPTH + 1];
    LBox *need = b;
    for (int i = 0; i < n; i++) {
        LBox *a = chain[i];
        bool ap;
        if (fixed)
            ap = false;
        else if (need->kind == LK_BLOCK && (need->flags & LF_ABSPOS)) {
            LBox *cb = ((LBlock *)need)->cb;
            LBox *ref = cb && cb->kind != LK_BLOCK ? &l_geom_parent(cb)->b : cb;
            ap = a == ref || a == cb;
        } else
            ap = true;
        applies[i] = ap;
        if (ap && a->kind == LK_BLOCK)
            need = a;
    }
    float x = sx, y = sy;
    bool ok = true;
    for (int i = n; i-- > 0;) {
        LBox *a = chain[i];
        if (a->kind != LK_BLOCK)
            continue;
        LBlock *k = (LBlock *)a;
        x += k->x;
        y += k->y;
        if (applies[i] && (a->flags & LF_CLIPS)) {
            float cx, cy;
            if (!clip_content(H, k, x, y, &cx, &cy))
                ok = false;
            x = cx;
            y = cy;
        }
    }
    if (b->kind == LK_BLOCK) {
        LBlock *k = (LBlock *)b;
        x += k->x;
        y += k->y;
        if (b->flags & LF_STICKY) {
            float dx;
            float dy = l_sticky_offset(H->L, k, &dx);
            x += dx;
            y += dy;
        }
        if (fixed) {
            x += H->scroll_x;
            y += H->scroll_y;
        }
    }
    *ox = x;
    *oy = y;
    return ok;
}

static bool hit_list(HCtx *H, LBlock *sc, float sx, float sy, uint32_t from, uint32_t to)
{
    for (uint32_t i = to; i-- > from;) {
        LBox *b = sc->layers->items[i];
        float ox, oy;
        if (!layer_origin(H, &sc->b, sx, sy, b, &ox, &oy))
            continue;
        if (hit_layer(H, b, ox, oy))
            return true;
    }
    return false;
}

static bool hit_layer(HCtx *H, LBox *b, float ox, float oy)
{
    if (H->depth > L_MAX_DEPTH || (b->flags & LF_TOO_DEEP) || skip_inert(H, b))
        return false;
    if (b->kind == LK_INLINE) {
        LBlock *g = l_geom_parent(b);
        return g && (hit_lines(H, g, ox, oy, b) || hit_flow(H, b, ox, oy, PH_FLOAT));
    }
    LBlock *k = (LBlock *)b;
    float tdx, tdy;
    l_transform_offset(k, &tdx, &tdy);
    ox += tdx;
    oy += tdy;
    /* A rotation or scale: test the point mapped back into the box. */
    float hx = H->x, hy = H->y;
    AuMatrix tm;
    if (!tdx && !tdy && l_transform_matrix(k, &tm)) {
        float det = tm.a * tm.d - tm.b * tm.c;
        if (det > -1e-6f && det < 1e-6f)
            return false;
        float px = H->x - ox - tm.e, py = H->y - oy - tm.f;
        H->x = ox + (tm.d * px - tm.c * py) / det;
        H->y = oy + (tm.a * py - tm.b * px) / det;
    }
    if (!lb_anon(b) && b->style->rare->opacity <= 0 && b != H->L->root) {
        /* invisible but still hit-testable in browsers; keep going */
    }
    H->depth++;
    bool sc = (b->flags & LF_STACKING) && k->layers;
    bool hit = false;
    float cx, cy;
    bool content = k->b.inner == LI_REPLACED ? false : clip_content(H, k, ox, oy, &cx, &cy);
    if (k->b.inner == LI_REPLACED) {
        cx = ox;
        cy = oy;
    }
    /* The box's own overflow clip applies to the layers of its stacking
     * context as paint_layer applies it (push_clip before
     * paint_layer_list): a point outside the clip reaches none of them.
     * layer_origin only sees the clips of the boxes between a layer and
     * its stacking context. */
    bool layers = sc && (content || k->b.inner == LI_REPLACED);
    if (layers && hit_list(H, k, cx, cy, k->layers->nneg, k->layers->n))
        hit = true;
    if (!hit && content && hit_contents(H, k, cx, cy))
        hit = true;
    if (!hit && layers && hit_list(H, k, cx, cy, 0, k->layers->nneg))
        hit = true;
    if (!hit && inside(H, ox, oy, k->w, k->h) && targetable(b) && !lb_anon(b)) {
        H->box = b;
        H->bx = ox;
        H->by = oy;
        hit = true;
    }
    H->depth--;
    H->x = hx;
    H->y = hy;
    return hit;
}

static AuElement *element_of(LBox *b)
{
    for (; b; b = b->parent) {
        if (lb_anon(b) && !b->pseudo)
            continue;
        AuNode *n = b->node;
        while (n && n->type != AU_NODE_ELEMENT)
            n = n->parent;
        if (n)
            return au_as_element(n);
    }
    return NULL;
}

static LBox *hit_box(AuLayout *L, float x, float y, float scroll_x, float scroll_y, AuHitResult *r, float *bx,
                     float *by)
{
    memset(r, 0, sizeof *r);
    if (!L->root)
        return NULL;
    L->view_scroll_x = scroll_x;
    L->view_scroll_y = scroll_y;
    HCtx H;
    memset(&H, 0, sizeof H);
    H.L = L;
    /* Canvas space = viewport space: document point minus scroll. */
    H.x = x;
    H.y = y;
    H.scroll_x = scroll_x;
    H.scroll_y = scroll_y;
    LBlock *root = (LBlock *)L->root;
    bool ok = hit_layer(&H, L->root, root->x - scroll_x, root->y - scroll_y);
    LBox *b = ok ? H.box : L->root;
    r->element = element_of(b);
    /* Nothing that is not inert under the point (outside a modal dialog
     * without a backdrop, or in an inert page): no target. */
    if (!ok && r->element && au_node_inert(&r->element->node))
        r->element = NULL;
    r->node = r->element ? &r->element->node : NULL;
    if (ok && H.frag && b->kind == LK_TEXT) {
        LText *t = (LText *)b;
        r->node = b->node;
        LFrag *f = H.frag;
        LFont *font = l_font(L, b->style);
        float skipped;
        uint32_t k = l_prefix_seek(L, font, b->style, t->text + f->start, f->len, H.fx, &skipped);
        while (k < f->len) {
            size_t adv;
            au_utf8_decode(t->text + f->start + k, f->len - k, &adv);
            float w = l_measure(L, font, b->style, t->text + f->start, k + (uint32_t)adv);
            if (w > H.fx) {
                float pw = k ? l_measure(L, font, b->style, t->text + f->start, k) : 0;
                if (H.fx - pw > (w - pw) / 2)
                    k += (uint32_t)adv;
                break;
            }
            k += (uint32_t)adv;
        }
        r->text_offset = f->start + k;
    }
    AuAtom href = au_atom_lookup("href", 4);
    /* up the flat tree: a link around a slot holds what the slot shows */
    bool flat = L->doc && L->doc->has_shadow;
    for (AuElement *e = r->element; e && href;
         e = flat ? au_flat_parent_element(&e->node)
                  : au_is_element(e->node.parent) ? au_as_element(e->node.parent) : NULL)
        if ((au_is_html_element(&e->node, AU_ATOM_a) || au_is_html_element(&e->node, AU_ATOM_area)) &&
            au_element_get_attr(e, href)) {
            r->link = e;
            break;
        }
    r->local_x = x - H.bx;
    r->local_y = y - H.by;
    if (bx)
        *bx = H.bx;
    if (by)
        *by = H.by;
    return ok ? b : NULL;
}

bool au_layout_hit_test(AuLayout *L, float x, float y, float scroll_x, float scroll_y, AuHitResult *r)
{
    return hit_box(L, x, y, scroll_x, scroll_y, r, NULL, NULL) != NULL;
}

bool au_layout_scroll_at(AuLayout *L, float x, float y, float scroll_x, float scroll_y, float dx, float dy)
{
    AuHitResult r;
    LBox *b = hit_box(L, x, y, scroll_x, scroll_y, &r, NULL, NULL);
    for (; b; b = b->parent) {
        if (b->kind != LK_BLOCK || !(b->flags & LF_SCROLLER))
            continue;
        LBlock *k = (LBlock *)b;
        float vw = k->w - k->bw[1] - k->bw[3], vh = k->h - k->bw[0] - k->bw[2];
        float mx = l_max(0, k->sw - vw), my = l_max(0, k->sh - vh);
        const AuStyleBox *s = b->style->box;
        float nx = k->scroll_x, ny = k->scroll_y;
        if (s->overflow_x == AU_OVERFLOW_SCROLL || s->overflow_x == AU_OVERFLOW_AUTO)
            nx = l_max(0, l_min(mx, k->scroll_x + dx));
        if (s->overflow_y == AU_OVERFLOW_SCROLL || s->overflow_y == AU_OVERFLOW_AUTO)
            ny = l_max(0, l_min(my, k->scroll_y + dy));
        if (nx != k->scroll_x || ny != k->scroll_y) {
            k->scroll_x = nx;
            k->scroll_y = ny;
            L->need_paint = true;
            return true;
        }
    }
    return false;
}
