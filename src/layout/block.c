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

/* A formatting context's floats are a list in placement order. Every
 * line and every float asks it what room a band has and where the next
 * float ends, so a walk of the whole list per question made a page of n
 * floats cost n squared (8,000 floats: a quarter of a second on the
 * host). Up to FLOATS_PLAIN floats the list is walked; beyond that two
 * indexes answer:
 *
 * - tree: the tops never decrease along the list (a float is not placed
 *   above an earlier one), so the floats that start above a band's
 *   bottom are a prefix, found by bisection. Of those, the ones that
 *   reach below the band's top are found through a binary tree over the
 *   list's indices whose nodes hold the lowest bottom under them: a
 *   subtree that ends above the band is skipped.
 * - ends: the bottoms in sorted runs whose lengths are the bits of their
 *   count (a new bottom is a run of one, and equal runs merge), so the
 *   first bottom below a line is a bisection in each run.
 *
 * The answers are the walk's. */
#ifdef AURA_FLOATS_CHECK
#include <stdlib.h>
#define FLOATS_PLAIN 2
#else
#define FLOATS_PLAIN 32
#endif

void l_floats_free(LFloats *fl)
{
    au_free(fl->f);
    au_free(fl->tree);
    au_free(fl->ends);
    au_free(fl->ends_tmp);
    memset(fl, 0, sizeof *fl);
}

/* What float i holds in the tree: its bottom when it takes room. */
static float float_reach(const struct LFloatRec *f)
{
    return f->h > 0 && f->w > 0 ? f->y + f->h : -INFINITY;
}

static void ends_add(LFloats *fl, float b)
{
    if (fl->nends == fl->ends_cap) {
        fl->ends_cap = fl->ends_cap ? fl->ends_cap * 2 : 64;
        fl->ends = au_realloc(fl->ends, fl->ends_cap * sizeof *fl->ends);
        fl->ends_tmp = au_realloc(fl->ends_tmp, (fl->ends_cap / 2) * sizeof *fl->ends_tmp);
    }
    uint32_t m = fl->nends, start = m, len = 1;
    fl->ends[m] = b;
    fl->nends++;
    /* Merge the new run with the run before it while they are as long. */
    while (m & len) {
        float *a = fl->ends + start - len, *t = fl->ends_tmp;
        memcpy(t, a, len * sizeof *t);
        uint32_t i = 0, j = len, k = 0; /* t[i], a[j] -> a[k] */
        while (i < len && j < 2 * len)
            a[k++] = a[j] < t[i] ? a[j++] : t[i++];
        while (i < len)
            a[k++] = t[i++];
        start -= len;
        len <<= 1;
    }
}

static void tree_set(LFloats *fl, uint32_t i)
{
    uint32_t node = fl->tree_leaves + i;
    float v = float_reach(&fl->f[i]);
    fl->tree[node] = v;
    for (node >>= 1; node >= 1; node >>= 1) {
        if (!(v > fl->tree[node]))
            break;
        fl->tree[node] = v;
    }
}

/* Builds the tree for the list's capacity. */
static void tree_build(LFloats *fl)
{
    uint32_t leaves = 64;
    while (leaves < fl->cap)
        leaves <<= 1;
    au_free(fl->tree);
    fl->tree = au_alloc(2 * (size_t)leaves * sizeof *fl->tree);
    fl->tree_leaves = leaves;
    for (uint32_t i = 0; i < 2 * leaves; i++)
        fl->tree[i] = -INFINITY;
    for (uint32_t i = 0; i < fl->n; i++)
        fl->tree[leaves + i] = float_reach(&fl->f[i]);
    for (uint32_t i = leaves - 1; i >= 1; i--)
        fl->tree[i] = l_max(fl->tree[2 * i], fl->tree[2 * i + 1]);
}

/* Appends a placed float and keeps the summaries and indexes. */
static void floats_add(LFloats *fl, struct LFloatRec rec)
{
    bool grew = false;
    if (fl->n == fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 8;
        fl->f = au_realloc(fl->f, fl->cap * sizeof *fl->f);
        grew = true;
    }
    uint32_t i = fl->n++;
    fl->f[i] = rec;
    float b = rec.y + rec.h;
    fl->max_top = rec.y;
    fl->bottom = l_max(fl->bottom, b);
    fl->side_bottom[rec.side] = fl->side_n[rec.side] ? l_max(fl->side_bottom[rec.side], b) : l_max(-INFINITY, b);
    fl->side_n[rec.side]++;
    if (fl->n <= FLOATS_PLAIN)
        return;
    if (!fl->tree) {
        /* The list has just outgrown the walk: index all of it. */
        tree_build(fl);
        for (uint32_t k = 0; k < fl->n; k++) {
            float kb = fl->f[k].y + fl->f[k].h;
            if (fl->f[k].h > 0 && kb == kb) /* not a NaN: the runs are sorted */
                ends_add(fl, kb);
        }
        return;
    }
    if (grew && fl->cap > fl->tree_leaves)
        tree_build(fl);
    else
        tree_set(fl, i);
    if (rec.h > 0 && b == b)
        ends_add(fl, b);
}

/* The walks: the answers for a short list, and what the indexes must
 * give for a long one (a build with -DAURA_FLOATS_CHECK compares them on
 * every question and indexes from the third float on; run the layout
 * fuzzers in it after a change here). */
static void avail_walk(const LFloats *fl, float y, float h, float left, float right, float *l, float *r)
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

static float next_bottom_walk(const LFloats *fl, float y)
{
    float best = INFINITY;
    for (uint32_t i = 0; fl && i < fl->n; i++) {
        float b = fl->f[i].y + fl->f[i].h;
        if (b > y && b < best && fl->f[i].h > 0)
            best = b;
    }
    return best;
}

void l_floats_avail(const LFloats *fl, float y, float h, float left, float right, float *l, float *r)
{
    float lo = left, hi = right;
    if (fl && fl->tree) {
        /* The floats that start above the band's bottom: a prefix. */
        uint32_t a = 0, e = fl->n;
        while (a < e) {
            uint32_t mid = a + (e - a) / 2;
            bool above = h > 0 ? fl->f[mid].y < y + h : fl->f[mid].y <= y;
            if (above)
                a = mid + 1;
            else
                e = mid;
        }
        uint32_t prefix = a, leaves = fl->tree_leaves;
        uint32_t stack[72];
        int sp = 0;
        stack[sp++] = 1;
        while (sp) {
            uint32_t node = stack[--sp];
            if (!(fl->tree[node] > y))
                continue; /* nothing under it reaches below the band's top */
            if (node >= leaves) {
                const struct LFloatRec *f = &fl->f[node - leaves];
                if (node - leaves >= prefix)
                    continue;
                if (f->side == 0)
                    lo = l_max(lo, f->x + f->w);
                else
                    hi = l_min(hi, f->x);
                continue;
            }
            uint32_t first = node;
            while (first < leaves)
                first <<= 1;
            if (first - leaves >= prefix)
                continue; /* all of it starts below the band */
            stack[sp++] = 2 * node + 1;
            stack[sp++] = 2 * node;
        }
        *l = lo;
        *r = hi;
#ifdef AURA_FLOATS_CHECK
        float wl, wr;
        avail_walk(fl, y, h, left, right, &wl, &wr);
        /* NaN coordinates make either answer depend on the order. */
        if ((wl != *l && wl == wl && *l == *l) || (wr != *r && wr == wr && *r == *r))
            abort();
#endif
        return;
    }
    avail_walk(fl, y, h, left, right, l, r);
}

float l_floats_next_bottom(const LFloats *fl, float y)
{
    float best = INFINITY;
    if (fl && fl->tree) {
        /* The first bottom below y in each sorted run. */
        uint32_t pos = 0;
        for (uint32_t bit = 0x80000000u; bit; bit >>= 1) {
            if (!(fl->nends & bit))
                continue;
            const float *run = fl->ends + pos;
            uint32_t a = 0, e = bit;
            while (a < e) {
                uint32_t mid = a + (e - a) / 2;
                if (run[mid] > y)
                    e = mid;
                else
                    a = mid + 1;
            }
            if (a < bit && run[a] < best)
                best = run[a];
            pos += bit;
        }
#ifdef AURA_FLOATS_CHECK
        float w = next_bottom_walk(fl, y);
        if (w != best)
            abort();
#endif
        return best;
    }
    return next_bottom_walk(fl, y);
}

float l_floats_clear_y(const LFloats *fl, int clear)
{
    float y = -INFINITY;
    if (!fl)
        return y;
    bool left = clear == AU_CLEAR_BOTH || clear == AU_CLEAR_LEFT || clear == AU_CLEAR_INLINE_START;
    bool right = clear == AU_CLEAR_BOTH || clear == AU_CLEAR_RIGHT || clear == AU_CLEAR_INLINE_END;
    if (left && fl->side_n[0])
        y = l_max(y, fl->side_bottom[0]);
    if (right && fl->side_n[1])
        y = l_max(y, fl->side_bottom[1]);
    return y;
}

float l_floats_bottom(const LFloats *fl)
{
    return fl ? fl->bottom : 0;
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
    /* Not above an earlier float's top (the last one's is the lowest). */
    if (fl->n)
        y = l_max(y, fl->max_top);
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
    floats_add(fl, (struct LFloatRec){x, y, mw, mh, (uint8_t)side});
    f->x = x + f->m[3] - ox;
    f->y = y + f->m[0] - oy;
}

/* ---- Block flow ---- */

/* qpos and qstart carry the margin quirk of quirks mode (as Blink does
 * it): a positive margin that is the user agent's default for its element
 * (a paragraph's, a heading's, a list's) is ignored at the start of a
 * table cell or the body, through every margin that collapses with it,
 * and dropped at their end. */
typedef struct LMargin {
    float pos, neg;
    float qpos;  /* the largest quirky positive margin */
    bool qstart; /* at the start of a quirky container: quirky margins do not count */
    /* The margins joining now collapse with the top margin of a box that
     * clearance has placed: they add nothing (see place_block). */
    bool absorb;
} LMargin;

static void mc_add(LMargin *m, float v)
{
    if (m->absorb)
        return;
    if (v > m->pos)
        m->pos = v;
    if (v < m->neg)
        m->neg = v;
}

static float mc_val(LMargin m) { return l_max(m.pos, m.qpos) + m.neg; }

/* A table cell or the body in quirks mode. */
static bool quirky_container(AuLayout *L, const LBlock *k)
{
    if (!L->doc || L->doc->quirks != AU_QUIRKS_FULL || lb_anon(&k->b))
        return false;
    if (k->b.inner == LI_TABLE_CELL)
        return true;
    return au_is_html_element(k->b.node, AU_ATOM_body);
}

/* Whether margin side (0 top, 2 bottom) of k is the user agent style
 * sheet's default, which Blink marks as quirky: the elements and factors
 * of its em-based block margins. An author margin of the same length
 * counts as the default. */
static bool quirky_margin(const LBlock *k, int side)
{
    if (lb_anon(&k->b) || !k->b.node || k->m[side] <= 0)
        return false;
    const AuNode *n = k->b.node;
    static const AuAtom one[] = {AU_ATOM_p,    AU_ATOM_blockquote, AU_ATOM_dl,  AU_ATOM_ol,
                                 AU_ATOM_ul,   AU_ATOM_menu,       AU_ATOM_dir, AU_ATOM_pre,
                                 AU_ATOM_xmp,  AU_ATOM_plaintext,  AU_ATOM_listing};
    static const AuAtom heads[] = {AU_ATOM_h1, AU_ATOM_h2, AU_ATOM_h3, AU_ATOM_h4, AU_ATOM_h5, AU_ATOM_h6};
    static const float factors[] = {0.67f, 0.83f, 1.0f, 1.33f, 1.67f, 2.33f};
    const AuStyleSurround *s = k->b.style->surr;
    if (s->margin[side].kind != AU_LEN_PX)
        return false;
    float fs = k->b.style->inh->font_size, v = s->margin[side].u.px;
    for (size_t i = 0; i < sizeof one / sizeof one[0]; i++)
        if (au_is_html_element(n, one[i]))
            return fabsf(v - fs) < 0.01f;
    for (size_t i = 0; i < sizeof heads / sizeof heads[0]; i++)
        if (au_is_html_element(n, heads[i])) {
            for (size_t j = 0; j < sizeof factors / sizeof factors[0]; j++)
                if (fabsf(v - fs * factors[j]) < 0.01f)
                    return true;
            return false;
        }
    return false;
}

/* Adds margin side of k to the pending margin. */
static void mc_add_side(AuLayout *L, LMargin *m, const LBlock *k, int side)
{
    if (m->absorb)
        return;
    if (L->doc && L->doc->quirks == AU_QUIRKS_FULL && quirky_margin(k, side)) {
        if (!m->qstart && k->m[side] > m->qpos)
            m->qpos = k->m[side];
        return;
    }
    mc_add(m, k->m[side]);
}

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
    S->pending = (LMargin){0};
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
    if (quirky_container(L, k)) {
        S->pending.qpos = 0; /* the margin quirk: dropped at its end */
        S->pending.qstart = false;
    }
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
        mc_add_side(L, &S->pending, k, 2);
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
            S->pending = (LMargin){0};
        }
    } else {
        h = l_clamp_h(k, spec, cb_h);
        S->pending = (LMargin){0};
        S->owner = NULL;
    }
    k->h = h + bpv;
    mc_add_side(L, &S->pending, k, 2);
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
    mc_add_side(L, &S->pending, k, 0);
    resolve(S);
    f = &S->fr[level];
    float top = f->cursor; /* border-top, content coordinates */
    const AuStyleSurround *s = lb_anon(&k->b) ? NULL : k->b.style->surr;
    bool la = s && s->margin[3].kind == AU_LEN_AUTO, ra = s && s->margin[1].kind == AU_LEN_AUTO;
    float cl = f->bfc_x, cr = f->bfc_x + cw;
    /* The height is not known before the box is laid out at its width:
     * start from 1, not from the height a previous layout left in k, so a
     * relayout places it as a layout from scratch does. */
    float est_h = 1;
    for (int guard = 0; guard < 1000; guard++) {
        float y = f->bfc_y + top;
        float l, r;
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
                est_h = l_max(1, k->h);
                continue;
            }
            /* It fits beside the floats its full height meets: place it
             * again with that height. */
            if (est_h < k->h) {
                est_h = k->h;
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
    S->pending = (LMargin){0};
    mc_add_side(L, &S->pending, k, 2);
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
            mc_add_side(L, &S->pending, k, 0);
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
        mc_add_side(L, &S->pending, k, 0);
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
        mc_add_side(L, &S->pending, k, 0);
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
    if (quirky_container(L, k))
        S->pending.qstart = true;
    int lv = push_frame(S, k, bx, by, content_w);
    /* Clearance put k's top border edge at the floats' bottom. The top
     * margins of its first children still collapse with k's own (CSS 2.1
     * 8.3.1: clearance separates k's margin from the margins before it,
     * not from its children's), and the collapsed margin lies above that
     * edge: they move nothing. Acid2's smile depends on it. */
    bool absorbing = clearance && k->bw[0] == 0 && k->p[0] == 0;
    if (absorbing)
        S->pending.absorb = true;
    L->depth++;
    flow_contents(L, S, lv, child_cbh);
    L->depth--;
    if (absorbing && S->pending.absorb)
        S->pending = (LMargin){0}; /* nothing but empty boxes inside */
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
    bool quirky = quirky_container(L, b);
    S.pending.qstart = quirky;
    flow_contents(L, &S, 0, c->avail_h >= 0 ? c->avail_h : c->cb_h);
    if (quirky)
        S.pending.qpos = 0;
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
            /* As Blink does: a box that avoids floats (a new formatting
             * context) sits beside the floats before it; any other block
             * flows under them (its line boxes do not count beside floats
             * that come later or earlier). Either way the floats end
             * there. */
            int clear = lb_anon(c) ? AU_CLEAR_NONE : c->style->box->clear;
            if (clear != AU_CLEAR_NONE)
                fsum = 0;
            mx = l_max(mx, (c->flags & LF_BFC) ? fsum + cmx : l_max(fsum, cmx));
            fsum = 0;
        }
    }
    *min_c = mn;
    *max_c = mx;
}
