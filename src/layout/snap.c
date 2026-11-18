/* Scroll snapping (CSS Scroll Snap 1): after a scroll container or the
 * viewport scrolls, moves it to a snap position of the boxes inside it
 * that have scroll-snap-align. docs/layout.md ("Scroll snapping") has
 * the rules this follows and what it leaves out. */
#include "layout_int.h"

enum { SNAP_X = 1, SNAP_Y = 2 };

/* The axes a scroll-snap-type value snaps in, and whether it is
 * mandatory. Block is y and inline is x (horizontal writing). */
static unsigned snap_axes(const AuCssBlob *t, bool *mandatory)
{
    *mandatory = false;
    if (!t)
        return 0;
    const AuCssValue *v = &t->value, *axis = v;
    if (v->type == AU_CSSV_LIST) {
        if (v->n < 1)
            return 0;
        axis = &v->u.items[0];
        *mandatory = v->n > 1 && v->u.items[1].type == AU_CSSV_KEYWORD && v->u.items[1].kw == AU_K_mandatory;
    }
    if (axis->type != AU_CSSV_KEYWORD)
        return 0;
    switch (axis->kw) {
    case AU_K_x:
    case AU_K_inline_:
        return SNAP_X;
    case AU_K_y:
    case AU_K_block:
        return SNAP_Y;
    case AU_K_both:
        return SNAP_X | SNAP_Y;
    default:
        return 0;
    }
}

/* A box's alignment keyword in an axis: start, end, center or none. */
static uint16_t snap_align(const AuCssBlob *a, unsigned axis)
{
    if (!a)
        return AU_K_none;
    const AuCssValue *v = &a->value;
    if (v->type == AU_CSSV_LIST) {
        if (v->n < 1)
            return AU_K_none;
        /* block (y) first, then inline (x) */
        v = &v->u.items[axis == SNAP_X && v->n > 1 ? 1 : 0];
    }
    return v->type == AU_CSSV_KEYWORD ? v->kw : AU_K_none;
}

/* One axis of a snap search. */
typedef struct {
    float origin, target, max; /* offsets: before the scroll, asked for, largest */
    float port0, port1;        /* the snapport inside the scrollport (after scroll-padding) */
    bool directional;          /* a scroll by a distance: only positions past origin count */
    int dir;                   /* its sign */
    bool found;
    float best, best_d;
    bool stop;   /* a scroll-snap-stop: always position ahead */
    float stop_at;
} SnapAxis;

static void snap_candidate(SnapAxis *a, uint16_t align, float lo, float hi, bool always)
{
    float p;
    switch (align) {
    case AU_K_start: p = lo - a->port0; break;
    case AU_K_end: p = hi - a->port1; break;
    case AU_K_center: p = (lo + hi) / 2 - (a->port0 + a->port1) / 2; break;
    default: return;
    }
    p = l_max(0, l_min(a->max, p));
    if (a->directional) {
        /* past the origin in the scroll's direction */
        if ((p - a->origin) * a->dir < 0.5f)
            return;
        if (always && (!a->stop || (p - a->stop_at) * a->dir < 0)) {
            a->stop = true;
            a->stop_at = p;
        }
    }
    float d = p > a->target ? p - a->target : a->target - p;
    if (!a->found || d < a->best_d) {
        a->found = true;
        a->best = p;
        a->best_d = d;
    }
}

/* The result of an axis: the target, moved to its snap position. */
static float snap_result(const SnapAxis *a, bool mandatory, float port)
{
    float p = a->best;
    if (!a->found)
        return a->target;
    if (a->stop && (p - a->stop_at) * a->dir > 0)
        p = a->stop_at;
    /* proximity: only a position near where the scroll would stop */
    if (!mandatory && !a->directional && a->best_d > port * 0.3f)
        return a->target;
    if (!mandatory && a->directional && (p > a->target ? p - a->target : a->target - p) > port * 0.3f)
        return a->target;
    return p;
}

static void axis_init(SnapAxis *a, float origin, float target, float max, float port0, float port1, bool directional)
{
    memset(a, 0, sizeof *a);
    a->origin = origin;
    a->target = l_max(0, l_min(max, target));
    a->max = max;
    a->port0 = port0;
    a->port1 = port1;
    a->dir = target > origin ? 1 : target < origin ? -1 : 0;
    a->directional = directional && a->dir != 0;
}

/* Walks the boxes whose snap container is the box under (or, for the
 * viewport, the root) and offers their positions to ax and ay. Box
 * positions are relative to (base_x, base_y). Does not descend into other
 * scroll containers. */
static void snap_walk(LBox *top, unsigned axes, float base_x, float base_y, SnapAxis *ax, SnapAxis *ay)
{
    uint32_t guard = 0;
    LBox *b = top->first;
    while (b && guard++ < 1000000u) {
        bool descend = true;
        if (b->kind == LK_BLOCK && !lb_anon(b)) {
            const AuStyleRare *r = b->style->rare;
            if (r->scroll_snap_align) {
                LBlock *k = (LBlock *)b;
                float x, y;
                l_box_origin(b, &x, &y);
                x -= base_x;
                y -= base_y;
                bool always = r->scroll_snap_stop == AU_SCROLL_SNAP_STOP_ALWAYS;
                if (axes & SNAP_X)
                    snap_candidate(ax, snap_align(r->scroll_snap_align, SNAP_X), x - r->scroll_margin[3],
                                   x + k->w + r->scroll_margin[1], always);
                if (axes & SNAP_Y)
                    snap_candidate(ay, snap_align(r->scroll_snap_align, SNAP_Y), y - r->scroll_margin[0],
                                   y + k->h + r->scroll_margin[2], always);
            }
            if (b->flags & LF_SCROLLER)
                descend = false;
        }
        if (descend && b->first) {
            b = b->first;
            continue;
        }
        while (b && b != top && !b->next)
            b = b->parent;
        if (!b || b == top)
            break;
        b = b->next;
    }
}

bool l_snap(AuLayout *L, LBlock *k, float ox, float oy, bool directional)
{
    (void)L;
    const AuStyleRare *r = k->b.style->rare;
    bool mandatory;
    unsigned axes = snap_axes(r->scroll_snap_type, &mandatory);
    if (!axes)
        return false;
    float vw = k->w - k->bw[1] - k->bw[3], vh = k->h - k->bw[0] - k->bw[2];
    float mx = l_max(0, k->sw - vw), my = l_max(0, k->sh - vh);
    float x0, y0;
    l_box_origin(&k->b, &x0, &y0);
    x0 += k->bw[3];
    y0 += k->bw[0];
    SnapAxis ax, ay;
    axis_init(&ax, ox, k->scroll_x, mx, l_len(&r->scroll_padding[3], vw), vw - l_len(&r->scroll_padding[1], vw),
              directional);
    axis_init(&ay, oy, k->scroll_y, my, l_len(&r->scroll_padding[0], vh), vh - l_len(&r->scroll_padding[2], vh),
              directional);
    snap_walk(&k->b, axes, x0, y0, &ax, &ay);
    float nx = (axes & SNAP_X) ? snap_result(&ax, mandatory, vw) : k->scroll_x;
    float ny = (axes & SNAP_Y) ? snap_result(&ay, mandatory, vh) : k->scroll_y;
    if (nx == k->scroll_x && ny == k->scroll_y)
        return false;
    k->scroll_x = nx;
    k->scroll_y = ny;
    L->need_paint = true;
    return true;
}

bool au_layout_snap_viewport(AuLayout *L, float vw, float vh, float ox, float oy, float *x, float *y,
                             bool directional)
{
    if (!L || !L->root || L->root->kind != LK_BLOCK || lb_anon(L->root))
        return false;
    LBlock *root = (LBlock *)L->root;
    const AuStyleRare *r = L->root->style->rare;
    bool mandatory;
    unsigned axes = snap_axes(r->scroll_snap_type, &mandatory);
    if (!axes)
        return false;
    float dw, dh;
    au_layout_document_size(L, &dw, &dh);
    float mx = l_max(0, dw - vw), my = l_max(0, dh - vh);
    (void)root;
    SnapAxis ax, ay;
    axis_init(&ax, ox, *x, mx, l_len(&r->scroll_padding[3], vw), vw - l_len(&r->scroll_padding[1], vw), directional);
    axis_init(&ay, oy, *y, my, l_len(&r->scroll_padding[0], vh), vh - l_len(&r->scroll_padding[2], vh), directional);
    /* the root's own box can be a snap area too */
    snap_walk(L->root, axes, 0, 0, &ax, &ay);
    float nx = (axes & SNAP_X) ? snap_result(&ax, mandatory, vw) : *x;
    float ny = (axes & SNAP_Y) ? snap_result(&ay, mandatory, vh) : *y;
    if (nx == *x && ny == *y)
        return false;
    *x = nx;
    *y = ny;
    return true;
}
