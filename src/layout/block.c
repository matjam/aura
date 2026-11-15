/* Block formatting contexts: block flow with margin collapsing (CSS 2.1
 * 8.3.1, including collapsing through empty blocks and clearance), auto
 * margins, floats and clear, BFC roots avoiding floats, list markers.
 *
 * Margin collapsing. A BFC keeps one pending collapsible margin (the
 * largest positive and the most negative margin seen) that has not been
 * placed yet, and the outermost box whose position waits on it (the
 * owner: a box whose top margin collapses with its first child's). When
 * content is placed (a line box, a box with top border or padding, a BFC
 * root, clearance) the pending margin is resolved: it sets the owner's
 * position, or advances the current block's cursor when there is no
 * owner. Bottom margins join the pending margin as boxes end, which is how
 * they collapse with the next sibling's top margin, with the parent's
 * bottom margin, and through empty boxes. */
#include "layout_int.h"

/* ---- Floats ---- */

void l_floats_free(LFloats *fl)
{
    au_free(fl->f);
    fl->f = NULL;
    fl->n = fl->cap = 0;
}

void l_floats_avail(const LFloats *fl, float y, float h, float left, float right, float *l, float *r)
{
    float lo = left, hi = right;
    for (uint32_t i = 0; fl && i < fl->n; i++) {
        const struct LFloatRec *f = &fl->f[i];
        if (f->h <= 0 || f->w <= 0)
            continue;
        bool hit = h > 0 ? (f->y < y + h && f->y + f->h > y) : (f->y <= y && f->y + f->h > y);
        if (!hit)
            continue;
        if (f->side == 0)
            lo = l_max(lo, f->x + f->w);
        else
            hi = l_min(hi, f->x);
    }
    *l = lo;
    *r = hi;
}

float l_floats_next_bottom(const LFloats *fl, float y)
{
    float best = INFINITY;
    for (uint32_t i = 0; fl && i < fl->n; i++) {
        float b = fl->f[i].y + fl->f[i].h;
        if (b > y && b < best && fl->f[i].h > 0)
            best = b;
    }
    return best;
}

float l_floats_clear_y(const LFloats *fl, int clear)
{
    float y = -INFINITY;
    for (uint32_t i = 0; fl && i < fl->n; i++) {
        const struct LFloatRec *f = &fl->f[i];
        bool match = clear == AU_CLEAR_BOTH || ((clear == AU_CLEAR_LEFT || clear == AU_CLEAR_INLINE_START) && f->side == 0) ||
                     ((clear == AU_CLEAR_RIGHT || clear == AU_CLEAR_INLINE_END) && f->side == 1);
        if (match)
            y = l_max(y, f->y + f->h);
    }
    return y;
}

float l_floats_bottom(const LFloats *fl)
{
    float y = 0;
    for (uint32_t i = 0; fl && i < fl->n; i++)
        y = l_max(y, fl->f[i].y + fl->f[i].h);
    return y;
}

static int float_side(const LBlock *f)
{
    int s = f->b.style->box->float_;
    return s == AU_FLOAT_RIGHT || s == AU_FLOAT_INLINE_END ? 1 : 0;
}

/* Lays out float f and places it in fl. left/right: the containing
 * block's content edges in BFC coordinates; y: the earliest top (BFC);
 * (ox, oy): the BFC position of the border-box origin of f's geometry
 * parent, so the float's position can be stored relative to it. */
void l_place_float(AuLayout *L, LFloats *fl, LBlock *f, float cb_w, float cb_h, float left, float right, float y,
                   float ox, float oy)
{
    l_layout_independent(L, f, cb_w, cb_h, cb_w, LS_FIT);
    float mw = f->w + f->m[1] + f->m[3], mh = f->h + f->m[0] + f->m[2];
    int side = float_side(f);
    /* Not above an earlier float's top. */
    for (uint32_t i = 0; i < fl->n; i++)
        y = l_max(y, fl->f[i].y);
    int clear = f->b.style->box->clear;
    if (clear != AU_CLEAR_NONE)
        y = l_max(y, l_floats_clear_y(fl, clear));
    float x = left;
    for (int guard = 0; guard < 1000; guard++) {
        float l, r;
        l_floats_avail(fl, y, l_max(mh, 0.01f), left, right, &l, &r);
        bool fits = r - l >= mw - 0.01f;
        if (fits || (l <= left && r >= right)) {
            x = side == 0 ? l : r - mw;
            if (!fits && side == 1)
                x = l_max(left, r - mw) == left && mw > right - left ? left : r - mw;
            break;
        }
        float nb = l_floats_next_bottom(fl, y);
        if (!isfinite(nb)) {
            x = side == 0 ? l : r - mw;
            break;
        }
        y = nb;
    }
    if (fl->n == fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 8;
        fl->f = au_realloc(fl->f, fl->cap * sizeof *fl->f);
    }
    fl->f[fl->n++] = (struct LFloatRec){x, y, mw, mh, (uint8_t)side};
    f->x = x + f->m[3] - ox;
    f->y = y + f->m[0] - oy;
}

/* ---- Block flow ---- */

typedef struct LMargin {
    float pos, neg;
} LMargin;

static void mc_add(LMargin *m, float v)
{
    if (v > m->pos)
        m->pos = v;
    if (v < m->neg)
        m->neg = v;
}

static float mc_val(LMargin m) { return m.pos + m.neg; }

typedef struct FlowFrame {
    LBlock *box;
    float bfc_x, bfc_y; /* content-box origin in BFC coordinates (provisional while pending) */
    float cursor;       /* next block's border-top position, content coordinates */
    float content_w;
} FlowFrame;

struct LBfc {
    LFloats floats;
    LMargin pending;
    LBlock *owner;
    float owner_base;
    int owner_level;
    FlowFrame *fr;
    int nfr, cap;
};

static int push_frame(LBfc *S, LBlock *b, float bx, float by, float cw)
{
    if (S->nfr == S->cap) {
        S->cap = S->cap ? S->cap * 2 : 16;
        S->fr = au_realloc(S->fr, S->cap * sizeof *S->fr);
    }
    S->fr[S->nfr] = (FlowFrame){b, bx, by, 0, cw};
    return S->nfr++;
}

/* Places the pending margin. */
static void resolve(LBfc *S)
{
    if (S->owner) {
        int lv = S->owner_level;
        S->owner->y = S->owner_base + mc_val(S->pending);
        float top = S->fr[lv - 1].bfc_y + S->owner->y + S->owner->bw[0] + S->owner->p[0];
        for (int i = lv; i < S->nfr; i++)
            S->fr[i].bfc_y = top;
        S->owner = NULL;
    } else {
        S->fr[S->nfr - 1].cursor += mc_val(S->pending);
    }
    S->pending = (LMargin){0, 0};
}

float l_flow_resolve(LFlowRef *fr)
{
    resolve(fr->S);
    return fr->S->fr[fr->level].bfc_y;
}

static bool is_flow_child(const LBox *c)
{
    return c->kind == LK_BLOCK && !(c->flags & (LF_FLOAT | LF_ABSPOS | LF_MARKER_OUT | LF_LEGEND));
}

/* Outside list markers: positioned against the item's first line. */
static void place_markers(AuLayout *L, LBlock *b)
{
    for (LBox *c = b->b.first; c; c = c->next) {
        if (c->kind != LK_BLOCK || !(c->flags & LF_MARKER_OUT))
            continue;
        LBlock *m = (LBlock *)c;
        float cw = b->w - l_bp_h(b);
        l_layout_independent(L, m, cw, -1, cw, LS_FIT);
        float mb = !isnan(m->baseline) ? m->baseline : m->h;
        float top = b->bw[0] + b->p[0];
        float base = !isnan(b->baseline) ? b->baseline : top + mb;
        m->x = b->bw[3] + b->p[3] - m->w - m->m[1];
        m->y = base - mb;
        l_finish_overflow(m);
    }
}

static void set_block_baselines(LBlock *b)
{
    b->baseline = b->last_baseline = NAN;
    if (b->b.flags & LF_INLINES) {
        if (b->nlines) {
            b->baseline = b->lines[0].y + b->lines[0].baseline;
            b->last_baseline = b->lines[b->nlines - 1].y + b->lines[b->nlines - 1].baseline;
        }
        return;
    }
    for (LBox *c = b->b.first; c; c = c->next) {
        if (!is_flow_child(c))
            continue;
        LBlock *k = (LBlock *)c;
        if (!isnan(k->baseline)) {
            b->baseline = k->y + k->baseline;
            break;
        }
    }
    for (LBox *c = b->b.first; c; c = c->next) {
        if (!is_flow_child(c))
            continue;
        LBlock *k = (LBlock *)c;
        if (!isnan(k->last_baseline))
            b->last_baseline = k->y + k->last_baseline;
    }
}

static void flow_contents(AuLayout *L, LBfc *S, int level, float cb_h);

/* Horizontal placement of an in-flow block: width and auto margins. */
static void block_width(AuLayout *L, LBlock *k, LBlock *parent, float cw)
{
    l_resolve_box_edges(k, cw);
    float w = l_used_width(L, k, cw, cw, LS_STRETCH);
    if (lb_anon(&k->b)) {
        k->x = parent->bw[3] + parent->p[3];
        k->w = w + l_bp_h(k);
        return;
    }
    const AuStyleSurround *s = k->b.style->surr;
    bool la = s->margin[3].kind == AU_LEN_AUTO, ra = s->margin[1].kind == AU_LEN_AUTO;
    float free = cw - w - l_bp_h(k) - k->m[1] - k->m[3];
    int ta = lb_anon(&parent->b) ? parent->b.style->inh->text_align : parent->b.style->inh->text_align;
    if (la && ra) {
        float h = free / 2;
        k->m[3] += l_max(0, h);
        k->m[1] += free - l_max(0, h);
    } else if (la) {
        k->m[3] += free;
    } else if (ra) {
        k->m[1] += free;
    } else if (ta == AU_TEXT_ALIGN_WEBKIT_CENTER && free > 0) {
        k->m[3] += free / 2;
    } else if (ta == AU_TEXT_ALIGN_WEBKIT_RIGHT && free > 0) {
        k->m[3] += free;
    }
    k->x = parent->bw[3] + parent->p[3] + k->m[3];
    k->w = w + l_bp_h(k);
}

/* Height of a box after its content is laid out; decides whether the
 * pending margin passes out through its bottom. Returns true if it is
 * empty and collapsed through. */
static bool finish_block(AuLayout *L, LBfc *S, int level, LBlock *k, bool joined, float cb_h)
{
    FlowFrame *f = &S->fr[level];
    float bpv = l_bp_v(k);
    float spec = l_spec_height(k, cb_h);
    bool auto_h = spec < 0;
    float ratio_h = auto_h ? l_ratio_height(k, f->content_w) : -1;
    bool can_pass = auto_h && ratio_h < 0 && k->bw[2] == 0 && k->p[2] == 0 && !(k->b.flags & LF_BFC);
    bool empty = joined && S->owner != NULL;
    float content = f->cursor;
    float h;
    if (empty) {
        /* Nothing placed inside: collapse through unless it has height. */
        h = auto_h ? l_clamp_h(k, l_max(0, ratio_h), cb_h) : l_clamp_h(k, spec, cb_h);
        if (h + bpv > 0) {
            resolve(S);
            empty = false;
            content = S->fr[level].cursor;
        }
    }
    if (empty) {
        if (S->owner == k)
            k->y = S->owner_base + mc_val(S->pending);
        h = 0;
        k->h = bpv;
        mc_add(&S->pending, k->m[2]);
        if (S->owner == k)
            S->owner = NULL;
        /* Otherwise an ancestor still waits: k sits at the top of its parent. */
        return true;
    }
    if (auto_h) {
        h = l_clamp_h(k, l_max(content, ratio_h), cb_h);
        if (h > content + 0.001f || !can_pass) {
            /* min-height grew it, or a bottom edge separates: the pending
             * margin stays inside. */
            if (mc_val(S->pending) != 0 || S->owner) {
                resolve(S);
                content = S->fr[level].cursor;
            }
            h = l_clamp_h(k, l_max(content, ratio_h), cb_h);
            S->pending = (LMargin){0, 0};
        }
    } else {
        h = l_clamp_h(k, spec, cb_h);
        S->pending = (LMargin){0, 0};
        S->owner = NULL;
    }
    k->h = h + bpv;
    mc_add(&S->pending, k->m[2]);
    return false;
}

/* A block-level BFC root (or replaced block) in flow: its border box
 * avoids the floats. */
static void place_bfc_root(AuLayout *L, LBfc *S, int level, LBlock *k, float cb_h)
{
    FlowFrame *f = &S->fr[level];
    LBlock *parent = f->box;
    float cw = f->content_w;
    l_resolve_box_edges(k, cw);
    mc_add(&S->pending, k->m[0]);
    resolve(S);
    f = &S->fr[level];
    float top = f->cursor; /* border-top, content coordinates */
    const AuStyleSurround *s = lb_anon(&k->b) ? NULL : k->b.style->surr;
    bool la = s && s->margin[3].kind == AU_LEN_AUTO, ra = s && s->margin[1].kind == AU_LEN_AUTO;
    float cl = f->bfc_x, cr = f->bfc_x + cw;
    for (int guard = 0; guard < 1000; guard++) {
        float y = f->bfc_y + top;
        float l, r;
        float est_h = l_max(1, k->h);
        l_floats_avail(&S->floats, y, est_h, cl, cr, &l, &r);
        /* Negative margins pull the box out of its containing block as for
         * any block; only floats push its border box in. */
        float bl = l > cl ? l_max(l, cl + k->m[3]) : cl + k->m[3];
        float br = r < cr ? l_min(r, cr - k->m[1]) : cr - k->m[1];
        float avail = br - bl;
        float w = l_used_width(L, k, cw, avail + k->m[1] + k->m[3], LS_STRETCH);
        float bw = w + l_bp_h(k);
        bool narrowed = l > cl || r < cr;
        if (narrowed && bw > avail + 0.01f) {
            float nb = l_floats_next_bottom(&S->floats, y);
            if (isfinite(nb)) {
                top = nb - f->bfc_y;
                continue;
            }
        }
        float h = l_spec_height(k, cb_h);
        if (h >= 0)
            h = l_clamp_h(k, h, cb_h);
        l_layout_content(L, k, w, h, cb_h);
        /* The full height must fit too. */
        float l2, r2;
        l_floats_avail(&S->floats, y, l_max(k->h, 0.01f), cl, cr, &l2, &r2);
        if ((l2 > l + 0.01f || r2 < r - 0.01f) && guard < 999) {
            float nb = l_floats_next_bottom(&S->floats, y);
            if (isfinite(nb) && bw > r2 - l_max(l2, cl + k->m[3]) - k->m[1] + 0.01f) {
                top = nb - f->bfc_y;
                continue;
            }
        }
        float free = br - bl - bw;
        float x = bl;
        if (!narrowed) {
            if (la && ra && free > 0)
                x += free / 2;
            else if (la && free > 0)
                x += free;
            else if (!la && !ra && !lb_anon(&parent->b) &&
                     parent->b.style->inh->text_align == AU_TEXT_ALIGN_WEBKIT_CENTER && free > 0)
                x += free / 2;
        }
        k->x = x - f->bfc_x + parent->bw[3] + parent->p[3];
        k->y = top;
        break;
    }
    f->cursor = k->y + k->h;
    S->pending = (LMargin){0, 0};
    mc_add(&S->pending, k->m[2]);
}

static void place_block(AuLayout *L, LBfc *S, int level, LBlock *k, float cb_h)
{
    FlowFrame *f = &S->fr[level];
    LBlock *parent = f->box;
    float cw = f->content_w;
    if (L->depth > L_MAX_DEPTH) {
        k->b.flags |= LF_TOO_DEEP;
        k->w = k->h = 0;
        k->x = parent->bw[3] + parent->p[3];
        k->y = f->cursor;
        return;
    }
    int clear = lb_anon(&k->b) ? AU_CLEAR_NONE : k->b.style->box->clear;
    if ((k->b.flags & LF_BFC) || k->b.inner == LI_REPLACED) {
        if (clear != AU_CLEAR_NONE && S->floats.n) {
            l_resolve_box_edges(k, cw);
            LMargin saved = S->pending;
            mc_add(&S->pending, k->m[0]);
            float hyp = f->bfc_y + f->cursor + mc_val(S->pending);
            float cy = l_floats_clear_y(&S->floats, clear);
            S->pending = saved;
            if (hyp < cy) {
                resolve(S);
                f = &S->fr[level];
                /* Clearance: the border top goes to the floats' bottom. */
                f->cursor = cy - f->bfc_y - k->m[0];
            }
        }
        place_bfc_root(L, S, level, k, cb_h);
        return;
    }
    block_width(L, k, parent, cw);
    float content_w = k->w - l_bp_h(k);
    bool clearance = false;
    if (clear != AU_CLEAR_NONE && S->floats.n) {
        LMargin saved = S->pending;
        mc_add(&S->pending, k->m[0]);
        float hyp = f->bfc_y + f->cursor + mc_val(S->pending);
        float cy = l_floats_clear_y(&S->floats, clear);
        if (hyp < cy) {
            S->pending = saved;
            resolve(S);
            f = &S->fr[level];
            k->y = cy - f->bfc_y;
            clearance = true;
        } else {
            S->pending = saved;
        }
    }
    bool joined = false;
    float provisional_y;
    if (clearance) {
        provisional_y = k->y;
    } else {
        mc_add(&S->pending, k->m[0]);
        if (k->bw[0] == 0 && k->p[0] == 0) {
            /* Its top margin collapses with its first child's. */
            joined = true;
            if (!S->owner) {
                S->owner = k;
                S->owner_base = f->cursor;
                S->owner_level = level + 1;
            }
            k->y = f->cursor;
            provisional_y = f->cursor + mc_val(S->pending);
        } else {
            resolve(S);
            f = &S->fr[level];
            k->y = f->cursor;
            provisional_y = k->y;
        }
    }
    float bx = f->bfc_x + (k->x - parent->bw[3] - parent->p[3]) + k->bw[3] + k->p[3];
    float by = f->bfc_y + provisional_y + k->bw[0] + k->p[0];
    float spec_h = l_spec_height(k, cb_h);
    if (spec_h < 0)
        spec_h = l_ratio_height(k, content_w);
    float child_cbh = spec_h >= 0 ? l_clamp_h(k, spec_h, cb_h) : -1;
    /* Quirks mode: percentage heights look through auto-height blocks to
     * the nearest ancestor with a height, up to the viewport (the
     * percentage height quirk of the HTML standard's quirks list). */
    if (child_cbh < 0 && L->doc && L->doc->quirks == AU_QUIRKS_FULL)
        child_cbh = cb_h;
    uint32_t mark = L->nabs;
    int lv = push_frame(S, k, bx, by, content_w);
    L->depth++;
    flow_contents(L, S, lv, child_cbh);
    L->depth--;
    bool empty = finish_block(L, S, lv, k, joined, cb_h);
    S->nfr--;
    f = &S->fr[level];
    set_block_baselines(k);
    k->key_w = content_w;
    k->b.flags |= LF_HAS_LAYOUT;
    k->b.flags &= ~(LF_DIRTY | LF_CHILD_DIRTY);
    l_layout_abspos_for(L, &k->b, mark);
    if (k->b.flags & LF_LIST_ITEM)
        place_markers(L, k);
    l_finish_overflow(k);
    /* An empty box that collapsed through leaves the cursor alone: its
     * margins are still pending. */
    if (!empty)
        f->cursor = k->y + k->h;
}

/* Lays out the children of S->fr[level].box. */
static void flow_contents(AuLayout *L, LBfc *S, int level, float cb_h)
{
    LBlock *b = S->fr[level].box;
    float cw = S->fr[level].content_w;
    if (b->b.flags & LF_INLINES) {
        LFlowRef ref = {S, level};
        float h = l_inline_layout(L, b, cw, &S->floats, S->fr[level].bfc_x, S->fr[level].bfc_y, cb_h, &ref);
        if (b->nlines)
            S->fr[level].cursor = h;
        set_block_baselines(b);
        return;
    }
    l_box_free_layout(&b->b);
    for (LBox *c = b->b.first; c; c = c->next) {
        if (c->kind != LK_BLOCK || (c->flags & (LF_MARKER_OUT | LF_LEGEND)))
            continue; /* a rendered legend was placed by its fieldset */
        LBlock *k = (LBlock *)c;
        FlowFrame *f = &S->fr[level];
        if (c->flags & LF_ABSPOS) {
            l_resolve_box_edges(k, cw);
            /* While b's top margin is pending with its owner's, b moves when
             * it resolves: the static position does not add it. */
            float pend = S->owner && level >= S->owner_level ? 0 : mc_val(S->pending);
            l_queue_abspos(L, k, b->bw[3] + b->p[3], b->bw[0] + b->p[0] + f->cursor + pend);
            continue;
        }
        if (c->flags & LF_FLOAT) {
            float y = f->bfc_y + f->cursor + mc_val(S->pending);
            if (S->owner && level >= S->owner_level) {
                /* This box's top margin is still pending with its owner's,
                 * and the provisional bfc_y already counts it: place the
                 * float where resolving the margin now would put the
                 * content top. */
                y = S->fr[S->owner_level - 1].bfc_y + S->owner_base + mc_val(S->pending) + f->cursor;
            }
            float ox = f->bfc_x - b->bw[3] - b->p[3], oy = f->bfc_y - b->bw[0] - b->p[0];
            L->depth++;
            l_place_float(L, &S->floats, k, cw, cb_h, f->bfc_x, f->bfc_x + cw, y, ox, oy);
            L->depth--;
            continue;
        }
        place_block(L, S, level, k, cb_h);
    }
    /* Flow children were placed in content coordinates (the cursor); move
     * them to the border-box coordinates the geometry uses. Floats are
     * already placed against the border box. */
    float top = b->bw[0] + b->p[0];
    if (top != 0)
        for (LBox *c = b->b.first; c; c = c->next)
            if (is_flow_child(c))
                ((LBlock *)c)->y += top;
    set_block_baselines(b);
}

/* The flow formatting context entry: b establishes a BFC. */
float l_flow_layout(AuLayout *L, LBlock *b, const LConstraints *c)
{
    LBfc S;
    memset(&S, 0, sizeof S);
    push_frame(&S, b, 0, 0, c->avail_w);
    flow_contents(L, &S, 0, c->avail_h >= 0 ? c->avail_h : c->cb_h);
    float h;
    if (S.owner) {
        /* Only empty boxes: their margins collapse into one, contained. */
        h = S.owner_base + mc_val(S.pending);
        resolve(&S);
    } else {
        h = S.fr[0].cursor + mc_val(S.pending);
    }
    if (b->b.flags & LF_INLINES)
        h = S.fr[0].cursor;
    /* A BFC root's auto height includes its floats. */
    h = l_max(h, l_floats_bottom(&S.floats));
    l_floats_free(&S.floats);
    au_free(S.fr);
    if (b->b.flags & LF_LIST_ITEM) {
        /* Markers need the final height: placed by the caller via finish. */
    }
    return h;
}

/* Markers of independently laid out list items. */

void l_flow_after(AuLayout *L, LBlock *b)
{
    if (b->b.flags & LF_LIST_ITEM)
        place_markers(L, b);
}

/* ---- Intrinsic sizes ---- */

void l_flow_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    if (b->b.flags & LF_INLINES) {
        l_inline_intrinsic(L, b, min_c, max_c);
        return;
    }
    float mn = 0, mx = 0, fsum = 0;
    for (LBox *c = b->b.first; c; c = c->next) {
        if (c->kind != LK_BLOCK || (c->flags & (LF_ABSPOS | LF_MARKER_OUT)))
            continue;
        LBlock *k = (LBlock *)c;
        float cmn, cmx;
        l_outer_intrinsic(L, k, -1, &cmn, &cmx);
        mn = l_max(mn, cmn);
        if (c->flags & LF_FLOAT) {
            int clear = k->b.style->box->clear;
            if (clear != AU_CLEAR_NONE)
                fsum = 0;
            fsum += cmx;
            mx = l_max(mx, fsum);
        } else {
            int clear = lb_anon(c) ? AU_CLEAR_NONE : c->style->box->clear;
            if (clear != AU_CLEAR_NONE)
                fsum = 0;
            mx = l_max(mx, fsum + cmx);
        }
    }
    *min_c = mn;
    *max_c = mx;
}
