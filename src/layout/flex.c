/* Flexbox (CSS Flexible Box Layout Level 1), horizontal writing mode.
 *
 * l_flex_layout follows the algorithm of section 9: items in order-
 * modified document order, flex base and hypothetical main sizes (with
 * the automatic minimum size), line collection, the resolution of flexible
 * lengths with freezing, hypothetical cross sizes, line cross sizes
 * (baseline-aware), align-content, stretch, auto margins, justify-content
 * and align-self. Positions are computed on logical main and cross axes
 * from main-start and cross-start and then mapped to physical coordinates,
 * which handles row-reverse, column-reverse and wrap-reverse.
 *
 * Simplifications: absolutely positioned children get the content-box
 * origin as static position; baseline alignment applies to row containers
 * (column containers treat it as flex-start); the min-content size of a
 * column item is its laid-out height at its used width; visibility:
 * collapse items are laid out like visible ones. */
#include "layout_int.h"

#include <math.h>

#define F_BIG 1e30f

typedef struct FItem {
    LBlock *k;
    float base, hypo, target; /* content-box main sizes */
    float min_m, max_m;       /* content-box main min and max (min includes the automatic minimum) */
    float bp_m, ms, me;       /* main border+padding; main-start and main-end margins */
    float cross;              /* content-box cross size */
    float bp_c, cs, ce;       /* cross border+padding; cross-start and cross-end margins */
    float grow, shrink;
    float pos_m, off_c;       /* logical margin-box start on the main axis, offset in the line */
    float base_off;           /* row: baseline from the cross-start margin edge, NAN if not baseline-aligned */
    float h_spec;             /* row: definite content height of the item, or -1 */
    int align;                /* resolved align-self */
    bool frozen, auto_ms, auto_me, auto_cs, auto_ce, stretch;
} FItem;

typedef struct FLine {
    uint32_t first, n;
    float cross, pos; /* line cross size; logical cross position */
    float max_base;   /* baseline-aligned items: the largest base_off */
} FLine;

static int align_of(int v) { return v & 0xFF; }

static float gap_px(const AuLen *l, float basis)
{
    if (!au_len_is_definite_kind(l))
        return 0;
    if (l_len_pct(l) && basis < 0)
        return 0;
    return l_max(0, l_len(l, basis));
}

/* A main-size length as a content-box size, or -1. */
static float len_content(const LBlock *k, const AuLen *l, float basis, bool row)
{
    if (!au_len_is_definite_kind(l) || (l_len_pct(l) && basis < 0))
        return -1;
    float v = l_len(l, basis);
    if (!lb_anon(&k->b) && k->b.style->box->box_sizing == AU_BOX_SIZING_BORDER_BOX)
        v -= row ? l_bp_h(k) : l_bp_v(k);
    return l_max(0, v);
}

/* The content height of k laid out at content width wc. */
static float measure_h(AuLayout *L, LBlock *k, float wc, float ch)
{
    float h = l_spec_height(k, ch);
    if (h >= 0)
        h = l_clamp_h(k, h, ch);
    l_layout_content(L, k, wc, h, ch);
    return k->h - l_bp_v(k);
}

/* Row items: the max-content width used for flex-basis: content. */
static float content_w(AuLayout *L, LBlock *k, float cw, float ch)
{
    if (k->b.inner == LI_REPLACED)
        return l_used_width(L, k, cw, F_BIG, LS_FIT);
    float ratio, h = l_spec_height(k, ch);
    if (h >= 0 && l_aspect_ratio(k, &ratio) && ratio > 0)
        return h * ratio;
    float mn, mx;
    l_intrinsic(L, k, &mn, &mx);
    return mx;
}

static bool min_is_auto(const LBlock *k, bool row)
{
    if (lb_anon(&k->b))
        return true;
    const AuStyleBox *s = k->b.style->box;
    return (row ? s->min_width.kind : s->min_height.kind) == AU_LEN_AUTO;
}

static void stable_sort_by_order(FItem *it, uint32_t n)
{
    /* Insertion sort: stable, linear when already in order (the usual case). */
    for (uint32_t i = 1; i < n; i++) {
        FItem t = it[i];
        int32_t o = lb_anon(&t.k->b) ? 0 : t.k->b.style->flex->order;
        uint32_t j = i;
        while (j > 0) {
            int32_t p = lb_anon(&it[j - 1].k->b) ? 0 : it[j - 1].k->b.style->flex->order;
            if (p <= o)
                break;
            it[j] = it[j - 1];
            j--;
        }
        it[j] = t;
    }
}

/* Resolves the flexible lengths of one line (section 9.7). */
static void resolve_flexible(FItem *it, uint32_t n, float avail)
{
    float sum = 0;
    for (uint32_t i = 0; i < n; i++)
        sum += it[i].hypo + it[i].bp_m + it[i].ms + it[i].me;
    bool grow = sum < avail;
    for (uint32_t i = 0; i < n; i++) {
        FItem *f = &it[i];
        f->frozen = false;
        float factor = grow ? f->grow : f->shrink;
        if (factor == 0 || (grow && f->base > f->hypo) || (!grow && f->base < f->hypo)) {
            f->frozen = true;
            f->target = f->hypo;
        }
    }
    float initial = avail;
    for (uint32_t i = 0; i < n; i++) {
        FItem *f = &it[i];
        initial -= (f->frozen ? f->target : f->base) + f->bp_m + f->ms + f->me;
    }
    for (int guard = 0; guard < 64; guard++) {
        float free = avail, factors = 0, scaled = 0;
        uint32_t unfrozen = 0;
        for (uint32_t i = 0; i < n; i++) {
            FItem *f = &it[i];
            free -= (f->frozen ? f->target : f->base) + f->bp_m + f->ms + f->me;
            if (!f->frozen) {
                unfrozen++;
                factors += grow ? f->grow : f->shrink;
                scaled += f->shrink * f->base;
            }
        }
        if (!unfrozen)
            break;
        if (factors < 1) {
            float lim = initial * factors;
            if (fabsf(lim) < fabsf(free))
                free = lim;
        }
        float violation = 0;
        for (uint32_t i = 0; i < n; i++) {
            FItem *f = &it[i];
            if (f->frozen)
                continue;
            float t = f->base;
            if (free != 0) {
                if (grow && factors > 0)
                    t = f->base + free * f->grow / factors;
                else if (!grow && scaled > 0)
                    t = f->base + free * (f->shrink * f->base) / scaled;
            }
            float c = t;
            if (c > f->max_m)
                c = f->max_m;
            if (c < f->min_m)
                c = f->min_m;
            if (c < 0)
                c = 0;
            f->target = c;
            violation += c - t;
        }
        for (uint32_t i = 0; i < n; i++) {
            FItem *f = &it[i];
            if (f->frozen)
                continue;
            float t = f->base;
            if (free != 0) {
                if (grow && factors > 0)
                    t = f->base + free * f->grow / factors;
                else if (!grow && scaled > 0)
                    t = f->base + free * (f->shrink * f->base) / scaled;
            }
            if (fabsf(violation) < 0.001f || (violation > 0 && f->target > t + 0.0005f) ||
                (violation < 0 && f->target < t - 0.0005f))
                f->frozen = true;
        }
        if (fabsf(violation) < 0.001f)
            break;
    }
    for (uint32_t i = 0; i < n; i++)
        it[i].frozen = true;
}

/* Distribution of free space along an axis: the offset before the first
 * element and the extra space between elements. */
static void distribute(int mode, float free, uint32_t n, float *lead, float *between)
{
    *lead = 0;
    *between = 0;
    switch (mode) {
    case AU_ALIGN_FLEX_END:
    case AU_ALIGN_END:
        *lead = free;
        break;
    case AU_ALIGN_CENTER:
        *lead = free / 2;
        break;
    case AU_ALIGN_SPACE_BETWEEN:
        if (n > 1 && free > 0)
            *between = free / (float)(n - 1);
        break;
    case AU_ALIGN_SPACE_AROUND:
        if (free > 0) {
            *between = free / (float)n;
            *lead = *between / 2;
        } else {
            *lead = free / 2;
        }
        break;
    case AU_ALIGN_SPACE_EVENLY:
        if (free > 0) {
            *between = free / (float)(n + 1);
            *lead = *between;
        } else {
            *lead = free / 2;
        }
        break;
    default:
        break;
    }
}

/* Maps a justify-content or align-content value to flex-relative terms:
 * start/end/left/right follow the writing mode, which is reversed from
 * flex-start/flex-end when the axis is. */
static int flex_relative(int v, bool reversed)
{
    switch (v) {
    case AU_ALIGN_START:
    case AU_ALIGN_LEFT:
    case AU_ALIGN_SELF_START:
        return reversed ? AU_ALIGN_FLEX_END : AU_ALIGN_FLEX_START;
    case AU_ALIGN_END:
    case AU_ALIGN_RIGHT:
    case AU_ALIGN_SELF_END:
        return reversed ? AU_ALIGN_FLEX_START : AU_ALIGN_FLEX_END;
    case AU_ALIGN_NORMAL:
    case AU_ALIGN_AUTO:
        return AU_ALIGN_FLEX_START;
    default:
        return v;
    }
}

float l_flex_layout(AuLayout *L, LBlock *b, const LConstraints *c)
{
    const AuStyleFlex *fs = b->b.style->flex;
    int dir = fs->flex_direction;
    bool row = dir == AU_FLEX_DIRECTION_ROW || dir == AU_FLEX_DIRECTION_ROW_REVERSE;
    bool rev_m = dir == AU_FLEX_DIRECTION_ROW_REVERSE || dir == AU_FLEX_DIRECTION_COLUMN_REVERSE;
    bool multi = fs->flex_wrap != AU_FLEX_WRAP_NOWRAP;
    bool rev_c = fs->flex_wrap == AU_FLEX_WRAP_WRAP_REVERSE;
    float cw = c->avail_w, ch = c->avail_h;
    float gap_m = row ? gap_px(&fs->column_gap, cw) : gap_px(&fs->row_gap, ch);
    float gap_c = row ? gap_px(&fs->row_gap, ch) : gap_px(&fs->column_gap, cw);
    float ox = b->bw[3] + b->p[3], oy = b->bw[0] + b->p[0];
    int align_items = align_of(fs->align_items);

    uint32_t n = 0;
    for (LBox *x = b->b.first; x; x = x->next)
        if (x->kind == LK_BLOCK && !(x->flags & LF_ABSPOS))
            n++;
    for (LBox *x = b->b.first; x; x = x->next)
        if (x->kind == LK_BLOCK && (x->flags & LF_ABSPOS)) {
            l_resolve_box_edges((LBlock *)x, cw);
            l_queue_abspos(L, (LBlock *)x, ox, oy);
        }
    b->baseline = b->last_baseline = NAN;
    if (!n)
        return ch >= 0 ? ch : 0;
    FItem *it = au_alloc(n * sizeof *it);
    memset(it, 0, n * sizeof *it);
    uint32_t i = 0;
    for (LBox *x = b->b.first; x; x = x->next)
        if (x->kind == LK_BLOCK && !(x->flags & LF_ABSPOS))
            it[i++].k = (LBlock *)x;
    stable_sort_by_order(it, n);

    /* The container's inner main size; for an indefinite column height,
     * found after the hypothetical sizes. */
    float inner_m = row ? cw : ch;
    float limit_m = inner_m >= 0 ? inner_m : l_clamp_h(b, F_BIG, -1);

    /* ---- Items: margins, flex base size, hypothetical main size ---- */
    for (i = 0; i < n; i++) {
        FItem *f = &it[i];
        LBlock *k = f->k;
        bool anon = lb_anon(&k->b);
        l_resolve_box_edges(k, cw);
        const AuStyleSurround *sr = anon ? NULL : k->b.style->surr;
        bool am[4] = {false, false, false, false};
        if (sr)
            for (int e = 0; e < 4; e++)
                am[e] = sr->margin[e].kind == AU_LEN_AUTO;
        const AuStyleFlex *kf = anon ? NULL : k->b.style->flex;
        f->grow = kf ? kf->flex_grow : 0;
        f->shrink = kf ? kf->flex_shrink : 1;
        int as = kf ? align_of(kf->align_self) : AU_ALIGN_AUTO;
        if (as == AU_ALIGN_AUTO)
            as = align_items;
        if (as == AU_ALIGN_NORMAL)
            as = AU_ALIGN_STRETCH;
        if (!row && (as == AU_ALIGN_BASELINE || as == AU_ALIGN_LAST_BASELINE))
            as = AU_ALIGN_FLEX_START;
        f->align = as;
        /* Logical margins. */
        int e_ms = row ? (rev_m ? 1 : 3) : (rev_m ? 2 : 0);
        int e_me = row ? (rev_m ? 3 : 1) : (rev_m ? 0 : 2);
        int e_cs = row ? (rev_c ? 2 : 0) : (rev_c ? 1 : 3);
        int e_ce = row ? (rev_c ? 0 : 2) : (rev_c ? 3 : 1);
        f->ms = k->m[e_ms];
        f->me = k->m[e_me];
        f->cs = k->m[e_cs];
        f->ce = k->m[e_ce];
        f->auto_ms = am[e_ms];
        f->auto_me = am[e_me];
        f->auto_cs = am[e_cs];
        f->auto_ce = am[e_ce];
        f->bp_m = row ? l_bp_h(k) : l_bp_v(k);
        f->bp_c = row ? l_bp_v(k) : l_bp_h(k);
        bool cross_auto = anon || (row ? k->b.style->box->height.kind : k->b.style->box->width.kind) == AU_LEN_AUTO;
        f->stretch = as == AU_ALIGN_STRETCH && cross_auto && !f->auto_cs && !f->auto_ce;
        /* Column items: the cross size (width) is needed to measure. */
        if (!row) {
            if (f->stretch && !multi)
                f->cross = l_clamp_w(k, l_max(0, cw - f->cs - f->ce - f->bp_c), cw);
            else
                f->cross = l_used_width(L, k, cw, cw, LS_FIT);
        }
        /* Flex base size. */
        AuLen basis = kf ? kf->flex_basis : (AuLen){{0}, AU_LEN_AUTO};
        if (basis.kind == AU_LEN_AUTO && !anon)
            basis = row ? k->b.style->box->width : k->b.style->box->height;
        float base = -1;
        if (basis.kind != AU_LEN_CONTENT)
            base = len_content(k, &basis, inner_m, row);
        float measured = -1;
        if (base < 0) {
            if (row) {
                base = content_w(L, k, cw, ch);
            } else {
                measured = measure_h(L, k, f->cross, ch);
                base = measured;
            }
        }
        f->base = base;
        /* Min and max main sizes, with the automatic minimum. */
        f->max_m = row ? l_clamp_w(k, F_BIG, cw) : l_clamp_h(k, F_BIG, ch);
        if (min_is_auto(k, row) && !(k->b.flags & LF_CLIPS)) {
            float content;
            if (row) {
                if (k->b.inner == LI_REPLACED) {
                    content = l_used_width(L, k, cw, F_BIG, LS_FIT);
                } else {
                    float mn, mx;
                    l_intrinsic(L, k, &mn, &mx);
                    content = mn;
                }
            } else {
                content = measured >= 0 ? measured : measure_h(L, k, f->cross, ch);
            }
            float spec = anon ? -1 : len_content(k, row ? &k->b.style->box->width : &k->b.style->box->height,
                                                  row ? cw : ch, row);
            float am_min = spec >= 0 ? l_min(spec, content) : content;
            f->min_m = l_min(am_min, f->max_m);
        } else {
            f->min_m = row ? l_clamp_w(k, 0, cw) : l_clamp_h(k, 0, ch);
        }
        f->hypo = l_max(f->min_m, l_min(f->max_m, base));
    }

    /* ---- Lines ---- */
    FLine *lines = au_alloc(n * sizeof *lines);
    uint32_t nl = 0;
    {
        uint32_t s = 0;
        while (s < n) {
            float used = 0;
            uint32_t e = s;
            while (e < n) {
                float o = it[e].hypo + it[e].bp_m + it[e].ms + it[e].me;
                if (multi && e > s && used + gap_m + o > limit_m + 0.001f)
                    break;
                used += (e > s ? gap_m : 0) + o;
                e++;
            }
            lines[nl].first = s;
            lines[nl].n = e - s;
            lines[nl].cross = 0;
            lines[nl].pos = 0;
            lines[nl].max_base = NAN;
            nl++;
            s = e;
        }
    }
    if (inner_m < 0) {
        /* Indefinite column height: the largest line's hypothetical sum,
         * within the container's min and max height. */
        float mx = 0;
        for (uint32_t l = 0; l < nl; l++) {
            float sum = 0;
            for (uint32_t j = 0; j < lines[l].n; j++) {
                FItem *f = &it[lines[l].first + j];
                sum += f->hypo + f->bp_m + f->ms + f->me;
            }
            sum += gap_m * (float)(lines[l].n - 1);
            mx = l_max(mx, sum);
        }
        inner_m = l_clamp_h(b, mx, -1);
    }
    for (uint32_t l = 0; l < nl; l++)
        resolve_flexible(&it[lines[l].first], lines[l].n, inner_m - gap_m * (float)(lines[l].n - 1));

    /* ---- Hypothetical cross sizes ---- */
    for (i = 0; i < n; i++) {
        FItem *f = &it[i];
        LBlock *k = f->k;
        if (row) {
            float h = l_spec_height(k, ch);
            if (h < 0)
                h = l_ratio_height(k, f->target);
            if (h >= 0)
                h = l_clamp_h(k, h, ch);
            f->h_spec = h;
            l_layout_content(L, k, f->target, h, ch);
            f->cross = k->h - f->bp_c;
        } else {
            l_layout_content(L, k, f->cross, f->target, ch);
        }
    }

    /* ---- Line cross sizes ---- */
    float inner_c = row ? ch : cw;
    for (uint32_t l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        float mx = 0, above = 0, below = 0;
        bool any_base = false;
        for (uint32_t j = 0; j < ln->n; j++) {
            FItem *f = &it[ln->first + j];
            float outer = f->cross + f->bp_c + f->cs + f->ce;
            f->base_off = NAN;
            if (row && (f->align == AU_ALIGN_BASELINE || f->align == AU_ALIGN_LAST_BASELINE) && !f->auto_cs &&
                !f->auto_ce) {
                LBlock *k = f->k;
                float bl = f->align == AU_ALIGN_LAST_BASELINE ? k->last_baseline : k->baseline;
                if (isnan(bl))
                    bl = k->h;
                f->base_off = f->cs + bl;
                above = l_max(above, f->base_off);
                below = l_max(below, outer - f->base_off);
                any_base = true;
            } else {
                mx = l_max(mx, outer);
            }
        }
        if (any_base) {
            mx = l_max(mx, above + below);
            ln->max_base = above;
        }
        ln->cross = mx;
    }
    if (!multi) {
        if (inner_c >= 0)
            lines[0].cross = inner_c;
        else
            lines[0].cross = l_clamp_h(b, lines[0].cross, -1);
    }
    float sum_c = gap_c * (float)(nl - 1);
    for (uint32_t l = 0; l < nl; l++)
        sum_c += lines[l].cross;
    if (inner_c < 0)
        inner_c = l_clamp_h(b, sum_c, -1);

    /* ---- align-content ---- */
    {
        float free = inner_c - sum_c;
        int ac = align_of(fs->align_content);
        if (ac == AU_ALIGN_NORMAL)
            ac = AU_ALIGN_STRETCH;
        float lead = 0, between = 0;
        if (multi) {
            if (ac == AU_ALIGN_STRETCH) {
                if (free > 0)
                    for (uint32_t l = 0; l < nl; l++)
                        lines[l].cross += free / (float)nl;
            } else {
                int m = flex_relative(ac, rev_c);
                if (free < 0 && m == AU_ALIGN_SPACE_BETWEEN)
                    m = AU_ALIGN_FLEX_START;
                distribute(m, free, nl, &lead, &between);
            }
        }
        float pos = lead;
        for (uint32_t l = 0; l < nl; l++) {
            lines[l].pos = pos;
            pos += lines[l].cross + gap_c + between;
        }
    }

    /* ---- Stretch, cross alignment ---- */
    for (uint32_t l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        for (uint32_t j = 0; j < ln->n; j++) {
            FItem *f = &it[ln->first + j];
            LBlock *k = f->k;
            if (f->stretch) {
                float v = l_max(0, ln->cross - f->cs - f->ce - f->bp_c);
                v = row ? l_clamp_h(k, v, ch) : l_clamp_w(k, v, cw);
                if (row) {
                    if (f->h_spec < 0 || fabsf(v - f->cross) > 0.001f)
                        l_layout_content(L, k, f->target, v, ch);
                } else if (fabsf(v - f->cross) > 0.001f) {
                    l_layout_content(L, k, v, f->target, ch);
                }
                f->cross = v;
            }
            float outer = f->cross + f->bp_c + f->cs + f->ce;
            float free = ln->cross - outer;
            float off = 0;
            if ((f->auto_cs || f->auto_ce) && free > 0) {
                if (f->auto_cs && f->auto_ce)
                    off = free / 2;
                else if (f->auto_cs)
                    off = free;
            } else if (!isnan(f->base_off)) {
                off = ln->max_base - f->base_off;
            } else {
                int a = f->align;
                /* start/end follow the writing mode; flex-start/end the
                 * cross axis, reversed by wrap-reverse. */
                switch (a) {
                case AU_ALIGN_FLEX_END:
                    off = free;
                    break;
                case AU_ALIGN_END:
                case AU_ALIGN_SELF_END:
                    off = rev_c ? 0 : free;
                    break;
                case AU_ALIGN_START:
                case AU_ALIGN_SELF_START:
                    off = rev_c ? free : 0;
                    break;
                case AU_ALIGN_CENTER:
                    off = free / 2;
                    break;
                default:
                    off = 0;
                    break;
                }
            }
            f->off_c = off;
        }
    }

    /* ---- Main axis: auto margins and justify-content ---- */
    int jc = flex_relative(align_of(fs->justify_content), rev_m && row);
    if (jc == AU_ALIGN_STRETCH)
        jc = AU_ALIGN_FLEX_START;
    for (uint32_t l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        float used = gap_m * (float)(ln->n - 1);
        int autos = 0;
        for (uint32_t j = 0; j < ln->n; j++) {
            FItem *f = &it[ln->first + j];
            used += f->target + f->bp_m + f->ms + f->me;
            autos += f->auto_ms + f->auto_me;
        }
        float free = inner_m - used;
        float lead = 0, between = 0, per_auto = 0;
        if (autos && free > 0) {
            per_auto = free / (float)autos;
        } else {
            int m = jc;
            if (free < 0 && m == AU_ALIGN_SPACE_BETWEEN)
                m = AU_ALIGN_FLEX_START;
            distribute(m, free, ln->n, &lead, &between);
        }
        float pos = lead;
        for (uint32_t j = 0; j < ln->n; j++) {
            FItem *f = &it[ln->first + j];
            if (f->auto_ms)
                pos += per_auto;
            f->pos_m = pos;
            pos += f->target + f->bp_m + f->ms + f->me;
            if (f->auto_me)
                pos += per_auto;
            pos += gap_m + between;
        }
    }

    /* ---- Physical positions ---- */
    for (uint32_t l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        for (uint32_t j = 0; j < ln->n; j++) {
            FItem *f = &it[ln->first + j];
            LBlock *k = f->k;
            float msz = f->target + f->bp_m, csz = f->cross + f->bp_c;
            float lm = f->pos_m + f->ms;
            float lc = ln->pos + f->off_c + f->cs;
            float pm = rev_m ? inner_m - lm - msz : lm;
            float pcr = rev_c ? inner_c - lc - csz : lc;
            if (row) {
                k->x = ox + pm;
                k->y = oy + pcr;
            } else {
                k->x = ox + pcr;
                k->y = oy + pm;
            }
        }
    }

    /* ---- Baselines ---- */
    {
        LBlock *first = NULL, *last = NULL;
        FLine *fl = &lines[0], *ll = &lines[nl - 1];
        for (uint32_t j = 0; j < fl->n && !first; j++)
            if (!isnan(it[fl->first + j].base_off))
                first = it[fl->first + j].k;
        if (!first)
            first = it[fl->first].k;
        for (uint32_t j = 0; j < ll->n && !last; j++)
            if (!isnan(it[ll->first + j].base_off))
                last = it[ll->first + j].k;
        if (!last)
            last = it[ll->first + ll->n - 1].k;
        b->baseline = first->y + (isnan(first->baseline) ? first->h : first->baseline);
        b->last_baseline = last->y + (isnan(last->last_baseline) ? last->h : last->last_baseline);
    }

    au_free(lines);
    au_free(it);
    return row ? inner_c : inner_m;
}

/* Intrinsic widths of a flex container: row containers sum their items'
 * contributions (min-content: the largest one when wrapping); column
 * containers take the largest. */
void l_flex_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    const AuStyleFlex *fs = b->b.style->flex;
    bool row = fs->flex_direction == AU_FLEX_DIRECTION_ROW || fs->flex_direction == AU_FLEX_DIRECTION_ROW_REVERSE;
    bool multi = fs->flex_wrap != AU_FLEX_WRAP_NOWRAP;
    float gap = row ? gap_px(&fs->column_gap, -1) : 0;
    float mn = 0, mx = 0;
    uint32_t count = 0;
    for (LBox *x = b->b.first; x; x = x->next) {
        if (x->kind != LK_BLOCK || (x->flags & LF_ABSPOS))
            continue;
        LBlock *k = (LBlock *)x;
        float cmn, cmx;
        l_outer_intrinsic(L, k, -1, &cmn, &cmx);
        if (row) {
            /* An item that cannot shrink keeps at least its basis. */
            const AuStyleFlex *kf = lb_anon(x) ? NULL : k->b.style->flex;
            if (kf && au_len_is_definite_kind(&kf->flex_basis) && !l_len_pct(&kf->flex_basis)) {
                float bs = len_content(k, &kf->flex_basis, -1, true) + l_edges_h(k);
                if (kf->flex_grow == 0)
                    cmx = l_min(cmx, l_max(bs, cmn));
                if (kf->flex_shrink == 0)
                    cmn = l_max(cmn, bs);
                cmx = l_max(cmx, cmn);
            }
            mx += cmx + (count ? gap : 0);
            if (multi)
                mn = l_max(mn, cmn);
            else
                mn += cmn + (count ? gap : 0);
        } else {
            mn = l_max(mn, cmn);
            mx = l_max(mx, cmx);
        }
        count++;
    }
    *min_c = mn;
    *max_c = l_max(mn, mx);
}
