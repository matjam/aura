/* Box sizing (widths, heights, min/max, box-sizing, aspect-ratio,
 * replaced elements), the formatting-context dispatch, absolutely
 * positioned layout, relative positioning, and the public entry points. */
#include "layout_int.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- Formatting contexts ---- */

static float replaced_layout(AuLayout *L, LBlock *b, const LConstraints *c)
{
    (void)L;
    (void)c;
    return 0;
}

static void replaced_intrinsic_size(AuLayout *L, LBlock *b, float *w, float *h, float *ratio);

static void replaced_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    float w, h, r;
    replaced_intrinsic_size(L, b, &w, &h, &r);
    float sh = l_spec_height(b, -1);
    if (sh >= 0 && r > 0)
        w = sh * r;
    *min_c = *max_c = w;
}

const LFcOps l_fc_flow = {l_flow_layout, l_flow_intrinsic};
const LFcOps l_fc_flex = {l_flex_layout, l_flex_intrinsic};
const LFcOps l_fc_table = {l_table_layout, l_table_intrinsic};
const LFcOps l_fc_replaced = {replaced_layout, replaced_intrinsic};

const LFcOps *l_fc_ops(const LBlock *b)
{
    switch (b->b.inner) {
    case LI_FLEX: return &l_fc_flex;
    case LI_TABLE: return &l_fc_table;
    case LI_REPLACED: return &l_fc_replaced;
    case LI_TABLE_ROW_GROUP:
    case LI_TABLE_ROW:
        /* Outside a table: the placeholder table code handles them too. */
        return &l_fc_table;
    default: return &l_fc_flow;
    }
}

/* ---- Edges ---- */

void l_resolve_box_edges(LBlock *b, float cb_w)
{
    if (lb_anon(&b->b)) {
        memset(b->m, 0, sizeof b->m);
        memset(b->p, 0, sizeof b->p);
        memset(b->bw, 0, sizeof b->bw);
        return;
    }
    const AuStyleSurround *s = b->b.style->surr;
    for (int i = 0; i < 4; i++) {
        b->m[i] = s->margin[i].kind == AU_LEN_AUTO ? 0 : l_len(&s->margin[i], cb_w);
        b->p[i] = l_max(0, l_len(&s->padding[i], cb_w));
        b->bw[i] = s->border_width[i];
    }
}

float l_bp_h(const LBlock *b) { return b->p[1] + b->p[3] + b->bw[1] + b->bw[3]; }
float l_bp_v(const LBlock *b) { return b->p[0] + b->p[2] + b->bw[0] + b->bw[2]; }
float l_edges_h(const LBlock *b) { return b->m[1] + b->m[3] + l_bp_h(b); }

static bool border_box_sizing(const LBlock *b)
{
    return !lb_anon(&b->b) && b->b.style->box->box_sizing == AU_BOX_SIZING_BORDER_BOX;
}

/* A width-like length to a content-box width, or -1 when not definite. */
static float content_w_of(const LBlock *b, const AuLen *l, float cb_w)
{
    if (!au_len_is_definite_kind(l))
        return -1;
    if (l_len_pct(l) && cb_w < 0)
        return -1;
    float v = l_len(l, cb_w);
    if (border_box_sizing(b))
        v -= l_bp_h(b);
    return l_max(0, v);
}

static float content_h_of(const LBlock *b, const AuLen *l, float cb_h)
{
    if (!au_len_is_definite_kind(l))
        return -1;
    if (l_len_pct(l) && cb_h < 0)
        return -1;
    float v = l_len(l, cb_h);
    if (border_box_sizing(b))
        v -= l_bp_v(b);
    return l_max(0, v);
}

float l_spec_width(const LBlock *b, float cb_w)
{
    if (lb_anon(&b->b))
        return -1;
    return content_w_of(b, &b->b.style->box->width, cb_w);
}

float l_spec_height(const LBlock *b, float cb_h)
{
    if (lb_anon(&b->b))
        return -1;
    return content_h_of(b, &b->b.style->box->height, cb_h);
}

float l_clamp_w(const LBlock *b, float w, float cb_w)
{
    if (lb_anon(&b->b))
        return w;
    const AuStyleBox *s = b->b.style->box;
    float mx = content_w_of(b, &s->max_width, cb_w);
    if (mx >= 0 && w > mx)
        w = mx;
    float mn = content_w_of(b, &s->min_width, cb_w);
    if (mn >= 0 && w < mn)
        w = mn;
    return l_max(0, w);
}

float l_clamp_h(const LBlock *b, float h, float cb_h)
{
    if (lb_anon(&b->b))
        return h;
    const AuStyleBox *s = b->b.style->box;
    float mx = content_h_of(b, &s->max_height, cb_h);
    if (mx >= 0 && h > mx)
        h = mx;
    float mn = content_h_of(b, &s->min_height, cb_h);
    if (mn >= 0 && h < mn)
        h = mn;
    return l_max(0, h);
}

bool l_aspect_ratio(const LBlock *b, float *ratio)
{
    if (lb_anon(&b->b))
        return false;
    const AuStyleBox *s = b->b.style->box;
    if (s->aspect_ratio > 0 && !(s->aspect_ratio_auto && b->b.inner == LI_REPLACED && b->rep && b->rep->ratio > 0)) {
        *ratio = s->aspect_ratio;
        return true;
    }
    if (b->b.inner == LI_REPLACED && b->rep && b->rep->ratio > 0) {
        *ratio = b->rep->ratio;
        return true;
    }
    return false;
}

/* The content height a non-replaced box's aspect-ratio gives at content
 * width content_w, or -1. */
float l_ratio_height(const LBlock *b, float content_w)
{
    float ratio;
    if (b->b.inner == LI_REPLACED || !l_aspect_ratio(b, &ratio) || ratio <= 0)
        return -1;
    float h = border_box_sizing(b) ? (content_w + l_bp_h(b)) / ratio - l_bp_v(b) : content_w / ratio;
    return l_max(0, h);
}

/* ---- Replaced elements ---- */

static void replaced_intrinsic_size(AuLayout *L, LBlock *b, float *w, float *h, float *ratio)
{
    LReplaced *r = b->rep;
    const AuStyle *s = b->b.style;
    LFont *f = l_font(L, s);
    float lh = f->m.ascent + f->m.descent;
    float em = s->inh->font_size;
    *ratio = 0;
    switch (b->b.replaced) {
    case LR_IMAGE:
        if (r && r->loaded) {
            *w = r->iw;
            *h = r->ih;
            *ratio = r->ratio;
        } else if (r && r->label && *r->label) {
            *w = l_measure(L, f, s, r->label, strlen(r->label)) + 2;
            *h = lh;
        } else {
            *w = *h = 0;
        }
        return;
    case LR_TEXT_INPUT: {
        const char *sz = l_attr(&b->b, AU_ATOM_size);
        int n = sz ? atoi(sz) : 20;
        if (n <= 0 || n > 1000)
            n = 20;
        *w = (float)n * em * 0.55f;
        *h = lh;
        return;
    }
    case LR_BUTTON:
        *w = r && r->label ? l_measure(L, f, s, r->label, strlen(r->label)) : 0;
        *h = lh;
        return;
    case LR_SELECT:
        *w = (r && r->label ? l_measure(L, f, s, r->label, strlen(r->label)) : 0) + em * 1.5f;
        *h = lh;
        return;
    case LR_TEXTAREA: {
        AuAtom ac = au_atom_lookup("cols", 4), ar = au_atom_lookup("rows", 4);
        const char *cs = ac ? l_attr(&b->b, ac) : NULL, *rs = ar ? l_attr(&b->b, ar) : NULL;
        int cols = cs ? atoi(cs) : 20, rows = rs ? atoi(rs) : 2;
        if (cols <= 0 || cols > 1000)
            cols = 20;
        if (rows <= 0 || rows > 1000)
            rows = 2;
        *w = cols * f->m.ch_advance;
        *h = rows * l_line_height(s, f);
        return;
    }
    case LR_CHECKBOX:
    case LR_RADIO:
        *w = *h = 13;
        return;
    case LR_RANGE:
        *w = 129;
        *h = 16;
        return;
    case LR_DISC:
    case LR_CIRCLE:
    case LR_SQUARE:
        *w = em * 0.8f;
        *h = f->m.x_height;
        return;
    default: {
        float aw = 300, ah = 150;
        AuAtom wa = au_atom_lookup("width", 5), ha = au_atom_lookup("height", 6);
        const char *ws = wa ? l_attr(&b->b, wa) : NULL, *hs = ha ? l_attr(&b->b, ha) : NULL;
        if (ws && atof(ws) > 0)
            aw = (float)atof(ws);
        if (hs && atof(hs) > 0)
            ah = (float)atof(hs);
        if (l_is_tag(&b->b, AU_ATOM_meter) || l_is_tag(&b->b, AU_ATOM_progress)) {
            aw = em * 10;
            ah = em;
        }
        *w = aw;
        *h = ah;
        *ratio = ah > 0 ? aw / ah : 0;
        return;
    }
    }
}

/* Used content size of a replaced box (CSS 2.1 10.3.2, 10.6.2 and the
 * min/max table of 10.4). */
static void replaced_size(AuLayout *L, LBlock *b, float cb_w, float cb_h, float avail_c, bool stretch, float *ow,
                          float *oh)
{
    float iw, ih, ir;
    replaced_intrinsic_size(L, b, &iw, &ih, &ir);
    float ratio = ir;
    l_aspect_ratio(b, &ratio);
    float w = l_spec_width(b, cb_w), h = l_spec_height(b, cb_h);
    if (w < 0 && stretch && b->b.style->box->width.kind == AU_LEN_STRETCH)
        w = avail_c;
    if (w < 0 && h < 0) {
        w = iw;
        h = ih;
        if (b->b.replaced == LR_IMAGE && b->rep && !b->rep->loaded && !b->rep->label) {
            w = h = 0;
        } else if (iw <= 0 && ratio > 0 && avail_c > 0) {
            w = avail_c;
            h = w / ratio;
        }
        /* min/max with an aspect ratio (10.4 table, simplified). */
        float cw = l_clamp_w(b, w, cb_w), chh = l_clamp_h(b, h, cb_h);
        if (ratio > 0 && (cw != w || chh != h)) {
            if (cw != w) {
                w = cw;
                h = l_clamp_h(b, w / ratio, cb_h);
            } else {
                h = chh;
                w = l_clamp_w(b, h * ratio, cb_w);
            }
        } else {
            w = cw;
            h = chh;
        }
    } else if (w < 0) {
        h = l_clamp_h(b, h, cb_h);
        w = ratio > 0 ? h * ratio : iw;
        w = l_clamp_w(b, w, cb_w);
    } else if (h < 0) {
        w = l_clamp_w(b, w, cb_w);
        h = ratio > 0 ? w / ratio : ih;
        h = l_clamp_h(b, h, cb_h);
    } else {
        w = l_clamp_w(b, w, cb_w);
        h = l_clamp_h(b, h, cb_h);
    }
    *ow = l_max(0, w);
    *oh = l_max(0, h);
}

/* ---- Intrinsic sizes ---- */

void l_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    if (b->b.flags & LF_INTRINSIC_OK) {
        *min_c = b->min_c;
        *max_c = b->max_c;
        return;
    }
    if (L->depth > L_MAX_DEPTH) {
        *min_c = *max_c = 0;
        return;
    }
    L->depth++;
    float mn = 0, mx = 0;
    if (b->b.inner == LI_REPLACED) {
        float w, h, r;
        replaced_intrinsic_size(L, b, &w, &h, &r);
        float sh = l_spec_height(b, -1);
        if (sh >= 0 && r > 0)
            w = sh * r;
        mn = mx = w;
    } else {
        l_fc_ops(b)->intrinsic(L, b, &mn, &mx);
    }
    L->depth--;
    if (mx < mn)
        mx = mn;
    b->min_c = mn;
    b->max_c = mx;
    b->b.flags |= LF_INTRINSIC_OK;
    *min_c = mn;
    *max_c = mx;
}

/* Margin-box min-content and max-content contributions. Percentages of an
 * unknown containing block (cb_w < 0) count as 0 (auto for widths). */
void l_outer_intrinsic(AuLayout *L, LBlock *b, float cb_w, float *min_o, float *max_o)
{
    l_resolve_box_edges(b, cb_w < 0 ? 0 : cb_w);
    float edges = l_edges_h(b);
    float w = l_spec_width(b, cb_w);
    float mn, mx;
    if (w >= 0 && b->b.inner != LI_REPLACED) {
        mn = mx = w;
    } else if (b->b.inner == LI_REPLACED) {
        float rw, rh;
        replaced_size(L, b, cb_w, -1, -1, false, &rw, &rh);
        mn = mx = rw;
        /* A percentage width of a replaced element counts 0 for min-content. */
        if (l_len_pct(&b->b.style->box->width) && cb_w < 0)
            mn = 0;
    } else {
        l_intrinsic(L, b, &mn, &mx);
        const AuStyleBox *s = b->b.style->box;
        if (!lb_anon(&b->b) && s->width.kind == AU_LEN_MIN_CONTENT)
            mx = mn;
        else if (!lb_anon(&b->b) && s->width.kind == AU_LEN_MAX_CONTENT)
            mn = mx;
    }
    mn = l_clamp_w(b, mn, cb_w);
    mx = l_clamp_w(b, mx, cb_w);
    *min_o = mn + edges;
    *max_o = mx + edges;
}

/* ---- Widths ---- */

/* The used content-box width of b. avail is the space for its margin box.
 * mode LS_STRETCH fills it (block-level boxes in flow), LS_FIT shrinks to
 * fit (floats, inline-blocks, abspos with auto offsets). Margins must be
 * resolved already (auto margins as 0). */
float l_used_width(AuLayout *L, LBlock *b, float cb_w, float avail, int mode)
{
    if (mode == LS_STRETCH && b->b.inner == LI_TABLE)
        mode = LS_FIT; /* auto-width tables shrink to fit */
    float edges = l_edges_h(b);
    float avail_c = l_max(0, avail - edges);
    if (b->b.inner == LI_REPLACED) {
        float w, h;
        replaced_size(L, b, cb_w, -1, avail_c, true, &w, &h);
        return w;
    }
    float w = l_spec_width(b, cb_w);
    const AuStyleBox *s = lb_anon(&b->b) ? NULL : b->b.style->box;
    if (w < 0 && s) {
        float mn, mx;
        switch (s->width.kind) {
        case AU_LEN_MIN_CONTENT:
            l_intrinsic(L, b, &mn, &mx);
            w = mn;
            break;
        case AU_LEN_MAX_CONTENT:
            l_intrinsic(L, b, &mn, &mx);
            w = mx;
            break;
        case AU_LEN_FIT_CONTENT:
            l_intrinsic(L, b, &mn, &mx);
            w = l_min(mx, l_max(mn, avail_c));
            break;
        case AU_LEN_FIT_CONTENT_PX:
            l_intrinsic(L, b, &mn, &mx);
            w = l_min(mx, l_max(mn, l_min(avail_c, s->width.u.px)));
            break;
        case AU_LEN_STRETCH:
            w = avail_c;
            break;
        default:
            break;
        }
    }
    if (w < 0) {
        float ratio;
        float h = l_spec_height(b, -1);
        if (h >= 0 && l_aspect_ratio(b, &ratio)) {
            /* aspect-ratio with a definite height */
            if (border_box_sizing(b))
                w = (h + l_bp_v(b)) * ratio - l_bp_h(b);
            else
                w = h * ratio;
        } else if (mode == LS_FIT) {
            float mn, mx;
            l_intrinsic(L, b, &mn, &mx);
            w = l_min(l_max(mn, avail_c), mx);
        } else {
            w = avail_c;
        }
    }
    w = l_clamp_w(b, w, cb_w);
    if (b->b.inner == LI_TABLE) {
        /* A table is never narrower than its content. */
        float mn, mx;
        l_intrinsic(L, b, &mn, &mx);
        w = l_max(w, mn);
    }
    return w;
}

/* ---- Content layout ---- */

/* cb_h only reaches b's layout through b's own percentage heights (its
 * children resolve theirs against b's content height, passed separately). */
static bool same_cbh(const LBlock *b, float a, float c)
{
    if (a == c)
        return true;
    if (lb_anon(&b->b))
        return true;
    const AuStyleBox *s = b->b.style->box;
    return !l_len_pct(&s->height) && !l_len_pct(&s->min_height) && !l_len_pct(&s->max_height);
}

static bool can_skip(AuLayout *L, LBlock *b, float content_w, float content_h, float cb_h)
{
    (void)L;
    return (b->b.flags & (LF_HAS_LAYOUT | LF_DIRTY | LF_CHILD_DIRTY | LF_ESCAPES)) == LF_HAS_LAYOUT &&
           (b->b.flags & LF_BFC) && b->key_w == content_w && b->key_h == content_h && same_cbh(b, b->key_cbh, cb_h);
}

/* Lays out b's content at content width content_w (content_h: definite
 * content height or -1), sets b->w and b->h (border box), lays out the
 * absolutely positioned boxes it contains and computes overflow. Edges must
 * be resolved. */
void l_layout_content(AuLayout *L, LBlock *b, float content_w, float content_h, float cb_h)
{
    uint32_t mark = L->nabs;
    float ch;
    if (L->depth > L_MAX_DEPTH) {
        b->b.flags |= LF_TOO_DEEP;
        b->w = content_w + l_bp_h(b);
        b->h = (content_h >= 0 ? content_h : 0) + l_bp_v(b);
        b->ov = (LArea){0, 0, b->w, b->h};
        return;
    }
    /* A full layout clears every box's cache first (forget_layouts), so a
     * hit here is from earlier in the same pass. */
    if (can_skip(L, b, content_w, content_h, cb_h)) {
        L->stats.layouts_skipped++;
        b->w = content_w + l_bp_h(b);
        float h = content_h >= 0 ? content_h : b->h - l_bp_v(b);
        b->h = h + l_bp_v(b);
        return;
    }
    L->depth++;
    b->b.flags &= ~(LF_TOO_DEEP | LF_ESCAPES);
    LConstraints c = {content_w, content_h, content_w, content_h};
    if (b->b.inner == LI_REPLACED) {
        float w, h;
        replaced_size(L, b, content_w, cb_h, content_w, false, &w, &h);
        float r;
        if (l_spec_width(b, content_w) < 0 && l_spec_height(b, cb_h) < 0 && l_aspect_ratio(b, &r) && r > 0)
            h = content_w / r;
        else if (l_spec_height(b, cb_h) < 0 && l_aspect_ratio(b, &r) && r > 0)
            h = l_clamp_h(b, content_w / r, cb_h);
        ch = h;
        b->baseline = b->last_baseline = NAN;
    } else {
        ch = l_fc_ops(b)->layout(L, b, &c);
    }
    L->depth--;
    float h = content_h >= 0 ? content_h : ch;
    if (content_h < 0) {
        float ah = l_ratio_height(b, content_w);
        /* The ratio gives the height unless the content needs more
         * (min-height: auto behaviour of aspect-ratio). */
        if (ah >= 0)
            h = l_max(ah, ch);
        h = l_clamp_h(b, h, cb_h);
    }
    b->w = content_w + l_bp_h(b);
    b->h = h + l_bp_v(b);
    b->key_w = content_w;
    b->key_h = content_h;
    b->key_cbh = cb_h;
    b->b.flags |= LF_HAS_LAYOUT;
    b->b.flags &= ~(LF_DIRTY | LF_CHILD_DIRTY);
    if (b->b.flags & LF_LIST_ITEM)
        l_flow_after(L, b);
    l_layout_abspos_for(L, &b->b, mark);
    l_finish_overflow(b);
}

float l_measure_content(AuLayout *L, LBlock *b, float content_w, float content_h, float cb_h)
{
    if (can_skip(L, b, content_w, content_h, cb_h))
        return content_h >= 0 ? content_h + l_bp_v(b) : b->h;
    if ((b->b.flags & (LF_MEASURE_OK | LF_DIRTY | LF_CHILD_DIRTY)) == LF_MEASURE_OK && b->mk_w == content_w &&
        b->mk_h == content_h && same_cbh(b, b->mk_cbh, cb_h)) {
        L->stats.layouts_skipped++;
        return b->m_res;
    }
    l_layout_content(L, b, content_w, content_h, cb_h);
    b->mk_w = content_w;
    b->mk_h = content_h;
    b->mk_cbh = cb_h;
    b->m_res = b->h;
    b->b.flags |= LF_MEASURE_OK;
    return b->h;
}

/* Sizes and lays out a box that establishes an independent formatting
 * context: floats, inline-blocks, flex items given a width (LS_GIVEN with
 * avail as the content width), the root. Auto margins become 0. */
void l_layout_independent(AuLayout *L, LBlock *b, float cb_w, float cb_h, float avail, int mode)
{
    l_resolve_box_edges(b, cb_w);
    float w = mode == LS_GIVEN ? avail : l_used_width(L, b, cb_w, avail, mode);
    float h = l_spec_height(b, cb_h);
    if (h >= 0)
        h = l_clamp_h(b, h, cb_h);
    l_layout_content(L, b, w, h, cb_h);
}

/* ---- Overflow ---- */

static void add_rect(LArea *r, float x, float y, float w, float h)
{
    if (w <= 0 && h <= 0)
        return;
    float x1 = l_max(r->x + r->w, x + w), y1 = l_max(r->y + r->h, y + h);
    r->x = l_min(r->x, x);
    r->y = l_min(r->y, y);
    r->w = x1 - r->x;
    r->h = y1 - r->y;
}

static void shadow_extent(const LBlock *b, LArea *r)
{
    if (lb_anon(&b->b))
        return;
    const AuStyleRare *rare = b->b.style->rare;
    if (rare->box_shadow) {
        uint32_t n;
        const AuCssValue *it = l_items(&rare->box_shadow->value, &n);
        if (rare->box_shadow->value.type != AU_CSSV_COMMA_LIST) {
            it = &rare->box_shadow->value;
            n = 1;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (it[i].type != AU_CSSV_LIST || it[i].n < 4)
                continue;
            const AuCssValue *v = it[i].u.items;
            if (it[i].n >= 6 && v[5].type == AU_CSSV_KEYWORD)
                continue; /* inset */
            float dx = v[0].u.num, dy = v[1].u.num, blur = v[2].u.num, spread = v[3].u.num;
            float e = blur + spread;
            add_rect(r, dx - e, dy - e, b->w + 2 * e, b->h + 2 * e);
        }
    }
    if (rare->outline_style > AU_OUTLINE_STYLE_HIDDEN && rare->outline_width > 0) {
        float e = rare->outline_width + rare->outline_offset;
        add_rect(r, -e, -e, b->w + 2 * e, b->h + 2 * e);
    }
}

/* Adds the overflow of LBlock descendants reached through inline boxes. */
static void union_children(LBlock *b, LArea *ink, LArea *scroll)
{
    LBox *c = b->b.first;
    while (c) {
        if (c->kind == LK_BLOCK) {
            LBlock *k = (LBlock *)c;
            if (!(c->flags & LF_FIXED)) {
                add_rect(ink, k->x + k->ov.x, k->y + k->ov.y, k->ov.w, k->ov.h);
                /* A clipping child contributes its margin box. */
                if (c->flags & LF_CLIPS)
                    add_rect(scroll, k->x, k->y, k->w + l_max(0, k->m[1]), k->h + l_max(0, k->m[2]));
                else
                    add_rect(scroll, k->x + k->ov.x, k->y + k->ov.y, k->ov.w, k->ov.h);
            }
        } else if (c->kind == LK_INLINE && c->first) {
            c = c->first;
            continue;
        }
        while (c && !c->next) {
            c = c->parent;
            if (c == &b->b)
                return;
        }
        if (c)
            c = c->next;
    }
}

void l_finish_overflow(LBlock *b)
{
    LArea ink = {0, 0, b->w, b->h};
    shadow_extent(b, &ink);
    /* Content extent, for scroll containers: starts as the content box; the
     * end padding is added below and the padding box is the minimum. */
    LArea content = {b->bw[3] + b->p[3], b->bw[0] + b->p[0], l_max(0, b->w - l_bp_h(b)), l_max(0, b->h - l_bp_v(b))};
    for (uint32_t i = 0; i < b->nlines; i++) {
        LLine *ln = &b->lines[i];
        add_rect(&content, ln->x, ln->y, ln->w, ln->h);
    }
    for (uint32_t i = 0; i < b->nfrags; i++) {
        LFrag *f = &b->frags[i];
        if (f->kind == FR_ATOMIC || (f->flags & FRF_HIDDEN))
            continue;
        add_rect(&content, f->x, f->y, f->w, f->h);
    }
    LArea kids = content;
    union_children(b, &kids, &content);
    if (b->b.flags & LF_CLIPS) {
        /* Scrollable extent includes the end padding. */
        float right = content.x + content.w + b->p[1], bottom = content.y + content.h + b->p[2];
        b->sw = right - b->bw[3];
        b->sh = bottom - b->bw[0];
        b->sw = l_max(b->sw, b->w - b->bw[1] - b->bw[3]);
        b->sh = l_max(b->sh, b->h - b->bw[0] - b->bw[2]);
        float mx = l_max(0, b->sw - (b->w - b->bw[1] - b->bw[3]));
        float my = l_max(0, b->sh - (b->h - b->bw[0] - b->bw[2]));
        if (b->scroll_x > mx)
            b->scroll_x = mx;
        if (b->scroll_y > my)
            b->scroll_y = my;
    } else {
        add_rect(&ink, kids.x, kids.y, kids.w, kids.h);
        add_rect(&ink, content.x, content.y, content.w, content.h);
    }
    b->ov = ink;
}

/* ---- Geometry helpers ---- */

LBlock *l_geom_parent(LBox *b)
{
    for (LBox *p = b->parent; p; p = p->parent)
        if (p->kind == LK_BLOCK)
            return (LBlock *)p;
    return NULL;
}

void l_box_origin(LBox *b, float *x, float *y)
{
    float ox = 0, oy = 0;
    int guard = 0;
    for (LBox *p = b; p && guard < 100000; p = p->parent, guard++)
        if (p->kind == LK_BLOCK) {
            ox += ((LBlock *)p)->x;
            oy += ((LBlock *)p)->y;
        }
    *x = ox;
    *y = oy;
}

/* ---- Relative positioning ---- */

void l_apply_relative(LBlock *b, float cb_w, float cb_h)
{
    const AuStyleBox *s = b->b.style->box;
    float dx = 0, dy = 0;
    if (s->left.kind != AU_LEN_AUTO)
        dx = l_len(&s->left, cb_w);
    else if (s->right.kind != AU_LEN_AUTO)
        dx = -l_len(&s->right, cb_w);
    if (s->top.kind != AU_LEN_AUTO && !(l_len_pct(&s->top) && cb_h < 0))
        dy = l_len(&s->top, cb_h);
    else if (s->bottom.kind != AU_LEN_AUTO && !(l_len_pct(&s->bottom) && cb_h < 0))
        dy = -l_len(&s->bottom, cb_h);
    b->x += dx;
    b->y += dy;
}

static void relative_pass(AuLayout *L)
{
    LBox *b = L->root;
    while (b) {
        if (b->kind == LK_BLOCK && (b->flags & LF_RELPOS) && b != L->root) {
            LBlock *g = l_geom_parent(b);
            float cw = g ? g->w - l_bp_h(g) : L->vw;
            float ch = g && l_spec_height(g, -1) >= 0 ? g->h - l_bp_v(g) : -1;
            l_apply_relative((LBlock *)b, cw, ch);
        }
        if (b->first && b->kind != LK_TEXT && !(b->flags & LF_TOO_DEEP))
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
}

/* ---- Absolute positioning ---- */

void l_queue_abspos(AuLayout *L, LBlock *b, float sx, float sy)
{
    b->sx = sx;
    b->sy = sy;
    LBox *cb = NULL;
    if (!(b->b.flags & LF_FIXED)) {
        for (LBox *p = b->b.parent; p; p = p->parent)
            if (p->flags & (LF_POSITIONED | LF_TRANSFORM | LF_CB)) {
                cb = p;
                break;
            }
    } else {
        for (LBox *p = b->b.parent; p; p = p->parent)
            if (p->flags & LF_TRANSFORM) {
                cb = p;
                break;
            }
    }
    b->cb = cb;
    /* Boxes between it and its containing block cannot be reused by
     * incremental layout without it. */
    LBox *ref = cb && cb->kind != LK_BLOCK ? &l_geom_parent(cb)->b : cb;
    for (LBox *p = b->b.parent; p && p != ref; p = p->parent)
        p->flags |= LF_ESCAPES;
    if (L->nabs == L->abs_cap) {
        L->abs_cap = L->abs_cap ? L->abs_cap * 2 : 32;
        L->abs = au_realloc(L->abs, L->abs_cap * sizeof *L->abs);
    }
    L->abs[L->nabs].box = &b->b;
    L->abs[L->nabs].cb = cb;
    L->nabs++;
}

/* The reference block of a containing block box: itself, or for an inline
 * box the block container holding its fragments. */
static LBox *ref_block(LBox *cb)
{
    if (!cb || cb->kind == LK_BLOCK)
        return cb;
    return &l_geom_parent(cb)->b;
}

/* Containing block rectangle in the reference block's border-box
 * coordinates. */
static void cb_rect(AuLayout *L, LBox *cb, LArea *r)
{
    if (!cb) {
        *r = (LArea){0, 0, L->vw, L->vh};
        return;
    }
    if (cb->kind == LK_BLOCK) {
        LBlock *k = (LBlock *)cb;
        *r = (LArea){k->bw[3], k->bw[0], k->w - k->bw[1] - k->bw[3], k->h - k->bw[0] - k->bw[2]};
        return;
    }
    /* Inline: the box from its first fragment's start to its last
     * fragment's end (padding edges). */
    LBlock *g = l_geom_parent(cb);
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (uint32_t i = 0; g && i < g->nfrags; i++) {
        LFrag *f = &g->frags[i];
        if (f->box != cb)
            continue;
        if (!any) {
            x0 = f->x;
            y0 = f->y;
            x1 = f->x + f->w;
            y1 = f->y + f->h;
            any = true;
        } else {
            x1 = l_max(x1, f->x + f->w);
            y1 = l_max(y1, f->y + f->h);
            x0 = l_min(x0, f->x);
        }
    }
    const AuStyleSurround *s = cb->style->surr;
    *r = (LArea){x0 + s->border_width[3], y0 + s->border_width[0],
                 l_max(0, x1 - x0 - s->border_width[1] - s->border_width[3]),
                 l_max(0, y1 - y0 - s->border_width[0] - s->border_width[2])};
}

static void layout_abspos(AuLayout *L, LBlock *b, LBox *cb)
{
    LBox *ref = ref_block(cb);
    LArea r;
    cb_rect(L, cb, &r);
    /* Static position into the reference block's coordinates. */
    LBlock *g = l_geom_parent(&b->b);
    float gx = 0, gy = 0, rx = 0, ry = 0;
    if (g)
        l_box_origin(&g->b, &gx, &gy);
    if (ref)
        l_box_origin(ref, &rx, &ry);
    float offx = gx - rx, offy = gy - ry; /* geometry parent in ref coordinates */
    float spx = b->sx + offx, spy = b->sy + offy;
    const AuStyleBox *s = b->b.style->box;
    const AuStyleSurround *sr = b->b.style->surr;
    l_resolve_box_edges(b, r.w);
    bool lauto = s->left.kind == AU_LEN_AUTO, rauto = s->right.kind == AU_LEN_AUTO;
    bool tauto = s->top.kind == AU_LEN_AUTO, bauto = s->bottom.kind == AU_LEN_AUTO;
    float left = lauto ? 0 : l_len(&s->left, r.w), right = rauto ? 0 : l_len(&s->right, r.w);
    float top = tauto ? 0 : l_len(&s->top, r.h), bottom = bauto ? 0 : l_len(&s->bottom, r.h);
    bool mlauto = sr->margin[3].kind == AU_LEN_AUTO, mrauto = sr->margin[1].kind == AU_LEN_AUTO;
    bool mtauto = sr->margin[0].kind == AU_LEN_AUTO, mbauto = sr->margin[2].kind == AU_LEN_AUTO;
    float bph = l_bp_h(b), bpv = l_bp_v(b);
    /* Horizontal. */
    float w = l_spec_width(b, r.w);
    bool replaced = b->b.inner == LI_REPLACED;
    if (replaced)
        w = l_used_width(L, b, r.w, r.w, LS_FIT);
    else if (w < 0 && s->width.kind != AU_LEN_AUTO)
        w = l_used_width(L, b, r.w, lauto || rauto ? r.w - left - right : r.w - left - right, LS_FIT);
    if (lauto && rauto)
        left = spx - r.x;
    if (w < 0) {
        if (!lauto && !rauto) {
            /* stretch */
            w = r.w - left - right - b->m[1] - b->m[3] - bph;
            w = l_clamp_w(b, l_max(0, w), r.w);
        } else {
            float avail = r.w - (lauto ? 0 : left) - (rauto ? 0 : right);
            if (lauto && rauto)
                avail = r.w - left;
            w = l_used_width(L, b, r.w, avail, LS_FIT);
        }
    } else {
        w = l_clamp_w(b, w, r.w);
    }
    if (!lauto && !rauto) {
        float free = r.w - left - right - w - bph - b->m[1] - b->m[3];
        if (mlauto && mrauto) {
            if (free >= 0)
                b->m[3] = b->m[1] = free / 2;
            else
                b->m[1] = free;
        } else if (mlauto)
            b->m[3] = free;
        else if (mrauto)
            b->m[1] = free;
    }
    float x;
    if (lauto && !rauto)
        x = r.x + r.w - right - b->m[1] - w - bph;
    else
        x = r.x + left + b->m[3];
    /* Vertical: is the height definite before layout? */
    float h = l_spec_height(b, r.h);
    bool stretched_h = false;
    if (replaced) {
        float rw, rh;
        (void)rw;
        h = -1;
        rh = 0;
        (void)rh;
    }
    if (h < 0 && !replaced && !tauto && !bauto && s->height.kind == AU_LEN_AUTO) {
        h = l_max(0, r.h - top - bottom - (mtauto ? 0 : b->m[0]) - (mbauto ? 0 : b->m[2]) - bpv);
        stretched_h = true;
    }
    if (h >= 0)
        h = l_clamp_h(b, h, r.h);
    l_layout_content(L, b, w, h, r.h);
    float hh = b->h - bpv;
    (void)stretched_h;
    if (tauto && bauto)
        top = spy - r.y;
    float y;
    if (!tauto && !bauto) {
        float free = r.h - top - bottom - hh - bpv - b->m[0] - b->m[2];
        if (mtauto && mbauto)
            b->m[0] = b->m[2] = free / 2;
        else if (mtauto)
            b->m[0] = free;
        else if (mbauto)
            b->m[2] = free;
        y = r.y + top + b->m[0];
    } else if (tauto && !bauto) {
        y = r.y + r.h - bottom - b->m[2] - b->h;
    } else {
        y = r.y + top + b->m[0];
    }
    b->x = x - offx;
    b->y = y - offy;
}

/* Lays out the absolutely positioned boxes queued since mark whose
 * reference block is cb (cb NULL: the initial containing block, every
 * remaining entry). */
void l_layout_abspos_for(AuLayout *L, LBox *cb, uint32_t mark)
{
    uint32_t i = mark;
    while (i < L->nabs) {
        LAbsEntry e = L->abs[i];
        if (cb && ref_block(e.cb) != cb) {
            i++;
            continue;
        }
        /* Remove it, then lay it out (which may queue more after it). */
        memmove(&L->abs[i], &L->abs[i + 1], (L->nabs - i - 1) * sizeof *L->abs);
        L->nabs--;
        if (!cb && e.cb) {
            /* A containing block that never finished (too deep): use it anyway. */
        }
        layout_abspos(L, (LBlock *)e.box, e.cb);
    }
}

/* ---- Entry points ---- */

AuLayout *au_layout_new(const AuLayoutEnv *env)
{
    AuLayout *L = au_calloc(sizeof *L);
    if (env)
        L->env = *env;
    ar_init(&L->tree);
    ar_init(&L->font_arena);
    ar_init(&L->word_arena);
    L->need_build = true;
    return L;
}

void au_layout_free(AuLayout *L)
{
    if (!L)
        return;
    l_free_tree(L);
    l_text_free_caches(L);
    au_free(L->items);
    au_free(L->segs);
    au_free(L->abs);
    au_free(L);
}

void au_layout_set_document(AuLayout *L, AuDocument *doc)
{
    l_free_tree(L);
    L->doc = doc;
    L->need_build = true;
    L->need_paint = true;
}

void au_layout_tree_changed(AuLayout *L)
{
    L->need_build = true;
    L->need_paint = true;
}

void au_layout_images_changed(AuLayout *L)
{
    L->need_build = true;
    L->need_paint = true;
}

/* Before a full layout: no box's cached layout, measure or intrinsic
 * widths may be reused (viewport units and initial-containing-block
 * percentages may have changed). Within the pass the caches fill again. */
static void forget_layouts(AuLayout *L)
{
    LBox *b = L->root;
    while (b) {
        b->flags &= ~(LF_HAS_LAYOUT | LF_MEASURE_OK | LF_INTRINSIC_OK);
        if (b->first)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
}

static void mark_dirty(LBox *b)
{
    b->flags |= LF_DIRTY;
    b->flags &= ~(LF_INTRINSIC_OK | LF_MEASURE_OK);
    for (LBox *p = b->parent; p; p = p->parent) {
        if ((p->flags & LF_CHILD_DIRTY) && !(p->flags & LF_INTRINSIC_OK))
            break;
        p->flags |= LF_CHILD_DIRTY;
        p->flags &= ~(LF_INTRINSIC_OK | LF_MEASURE_OK);
    }
}

/* How a style change affects layout: 0 none, 1 repaint, 2 relayout of
 * the box, 3 rebuild of the box tree. */
static int style_diff(const AuStyle *a, const AuStyle *b)
{
    if (a == b)
        return 0;
    if (a->box != b->box) {
        const AuStyleBox *x = a->box, *y = b->box;
        if (x->display != y->display || x->position != y->position || x->float_ != y->float_ ||
            x->overflow_x != y->overflow_x || x->overflow_y != y->overflow_y)
            return 3;
    }
    int r = 1;
    const AuStyleInh *ia = a->inh, *ib = b->inh;
    if (ia != ib) {
        if (ia->white_space_collapse != ib->white_space_collapse || ia->visibility != ib->visibility ||
            ia->list_style_type != ib->list_style_type || ia->list_style_position != ib->list_style_position)
            return 3;
        if (ia->font_size != ib->font_size || ia->font_weight != ib->font_weight ||
            ia->font_style != ib->font_style || ia->font_family != ib->font_family ||
            ia->text_align != ib->text_align || ia->text_wrap_mode != ib->text_wrap_mode ||
            memcmp(&ia->line_height, &ib->line_height, sizeof ia->line_height) != 0)
            r = 2;
    }
    if (a->inhr != b->inhr) {
        if (a->inhr->text_transform != b->inhr->text_transform || a->inhr->quotes != b->inhr->quotes)
            return 3;
        r = 2;
    }
    if (a->box != b->box || a->surr != b->surr || a->flex != b->flex)
        r = 2;
    if (a->rare != b->rare) {
        const AuStyleRare *x = a->rare, *y = b->rare;
        if (x->content != y->content || x->counter_reset != y->counter_reset ||
            x->counter_increment != y->counter_increment || x->counter_set != y->counter_set ||
            (x->transform == NULL) != (y->transform == NULL) || (x->opacity < 1) != (y->opacity < 1) ||
            x->line_clamp != y->line_clamp || x->contain != y->contain)
            return 3;
        if (x->transform != y->transform || x->box_shadow != y->box_shadow ||
            x->outline_width != y->outline_width || x->text_overflow != y->text_overflow)
            r = r < 2 ? 2 : r;
    }
    if (a->pseudos != b->pseudos) {
        const AuStyle *pa = a->pseudos, *pb = b->pseudos;
        while (pa && pb) {
            if (pa->pseudo != pb->pseudo || style_diff(pa, pb) >= 2)
                return 3;
            pa = pa->next;
            pb = pb->next;
        }
        if (pa || pb)
            return 3;
    }
    return r;
}

static const AuStyle *current_style(const LBox *b)
{
    AuNode *n = b->node;
    if (!n)
        return NULL;
    const AuStyle *es;
    if (n->type == AU_NODE_TEXT) {
        AuNode *p = n->parent;
        while (p && p->type != AU_NODE_ELEMENT)
            p = p->parent;
        es = p ? au_element_style(au_as_element(p)) : NULL;
    } else if (au_is_element(n)) {
        es = au_element_style(au_as_element(n));
    } else
        return NULL;
    if (!es)
        return NULL;
    if (b->pseudo)
        return au_style_pseudo(es, (AuCssPseudo)b->pseudo);
    return es;
}

void au_layout_styles_changed(AuLayout *L)
{
    if (L->need_build || !L->root) {
        L->need_build = true;
        return;
    }
    LBox *b = L->root;
    int worst = 0;
    while (b && worst < 3) {
        const AuStyle *ns;
        if (lb_anon(b)) {
            ns = b->parent ? b->parent->style : NULL;
            if (b->pseudo == AU_CSS_PSEUDO_MARKER || (b->parent && b->parent->pseudo))
                ns = NULL; /* compared through its element */
        } else {
            ns = current_style(b);
        }
        if (!lb_anon(b) && b->node && !ns && b->node->type != AU_NODE_TEXT) {
            worst = 3;
            break;
        }
        if (ns && ns != b->style) {
            int d = style_diff(b->style, ns);
            if (d > worst)
                worst = d;
            if (d == 2)
                mark_dirty(b);
            if (d < 3) {
                au_style_retain((AuStyle *)ns);
                au_style_release(b->style);
                b->style = (AuStyle *)ns;
            }
        }
        if (b->first && b->kind != LK_TEXT)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
    if (worst == 3)
        L->need_build = true;
    else if (worst == 2)
        L->need_layout = true;
    if (worst >= 1)
        L->need_paint = true;
    /* Anonymous boxes share their parent's style: refresh them. */
    if (worst < 3) {
        b = L->root;
        while (b) {
            if (lb_anon(b) && b->parent && b->parent->style != b->style && b->pseudo == 0 && b->kind != LK_TEXT &&
                !(b->parent->pseudo)) {
                au_style_retain(b->parent->style);
                au_style_release(b->style);
                b->style = b->parent->style;
            }
            if (b->first && b->kind != LK_TEXT)
                b = b->first;
            else {
                while (b && !b->next)
                    b = b->parent;
                if (b)
                    b = b->next;
            }
        }
    }
}

static void layout_root(AuLayout *L)
{
    LBlock *root = (LBlock *)L->root;
    L->nabs = 0;
    L->depth = 0;
    L->stats.layouts_skipped = 0;
    l_resolve_box_edges(root, L->vw);
    float w = l_used_width(L, root, L->vw, L->vw, LS_STRETCH);
    float h = l_spec_height(root, L->vh);
    if (h >= 0)
        h = l_clamp_h(root, h, L->vh);
    root->x = root->m[3];
    root->y = root->m[0];
    l_layout_content(L, root, w, h, L->vh);
    relative_pass(L);
    l_layout_abspos_for(L, NULL, 0);
    l_finish_overflow(root);
    /* Document size: the root's overflow plus positioned boxes outside it. */
    float right = root->x + root->ov.x + root->ov.w, bottom = root->y + root->ov.y + root->ov.h;
    right = l_max(right, root->x + root->w + root->m[1]);
    bottom = l_max(bottom, root->y + root->h + root->m[2]);
    LBox *b = L->root;
    while (b) {
        bool descend = b->first && b->kind != LK_TEXT && !(b->flags & (LF_CLIPS | LF_TOO_DEEP));
        if (b->kind == LK_BLOCK && (b->flags & LF_ABSPOS) && !(b->flags & LF_FIXED)) {
            float x, y;
            l_box_origin(b, &x, &y);
            LBlock *k = (LBlock *)b;
            right = l_max(right, x + k->ov.x + k->ov.w);
            bottom = l_max(bottom, y + k->ov.y + k->ov.h);
        }
        if (descend)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
    L->doc_w = l_max(L->vw, right);
    L->doc_h = l_max(L->vh, bottom);
}

bool au_layout_update(AuLayout *L, float vw, float vh)
{
    if (vw < 1)
        vw = 1;
    if (vh < 1)
        vh = 1;
    bool changed = false;
    if (L->need_build) {
        /* Keep inner scroll offsets across the rebuild. */
        l_build_tree(L);
        L->need_build = false;
        L->need_layout = true;
        L->need_restack = true;
    }
    if (!L->root) {
        L->doc_w = vw;
        L->doc_h = vh;
        L->vw = vw;
        L->vh = vh;
        L->need_layout = false;
        return true;
    }
    if (L->need_layout || vw != L->vw || vh != L->vh) {
        /* A viewport change can move anything (viewport units, fixed
         * boxes, percentages of the initial containing block). */
        L->full_layout = vw != L->vw || vh != L->vh || !L->built_once;
        L->built_once = true;
        L->vw = vw;
        L->vh = vh;
        if (L->full_layout)
            forget_layouts(L);
        layout_root(L);
        L->full_layout = false;
        L->need_layout = false;
        L->need_restack = true;
        changed = true;
    }
    if (L->need_restack) {
        l_build_stacking(L);
        L->need_restack = false;
    }
    if (changed)
        L->need_paint = true;
    return changed;
}

bool au_layout_needs_paint(const AuLayout *L)
{
    return L->need_paint;
}

void au_layout_document_size(const AuLayout *L, float *w, float *h)
{
    *w = L->doc_w;
    *h = L->doc_h;
}

void au_layout_stats(const AuLayout *L, AuLayoutStats *s)
{
    *s = L->stats;
    s->lines = s->frags = 0;
    s->frag_bytes = 0;
    LBox *b = L->root;
    while (b) {
        if (b->kind == LK_BLOCK) {
            LBlock *k = (LBlock *)b;
            s->lines += k->nlines;
            s->frags += k->nfrags;
            s->frag_bytes += k->nlines * sizeof(LLine) + k->nfrags * sizeof(LFrag);
        }
        if (b->first && b->kind != LK_TEXT)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
    s->tree_bytes = L->tree.total;
}

bool au_layout_element_rect(AuLayout *L, const AuElement *el, AuRect *out)
{
    LBox *b = el ? (LBox *)el->box : NULL;
    if (!b)
        return false;
    if (b->kind == LK_BLOCK) {
        LBlock *k = (LBlock *)b;
        float x, y;
        l_box_origin(b, &x, &y);
        *out = (AuRect){x, y, k->w, k->h};
        return true;
    }
    LBlock *g = l_geom_parent(b);
    if (!g)
        return false;
    float gx, gy;
    l_box_origin(&g->b, &gx, &gy);
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (uint32_t i = 0; i < g->nfrags; i++) {
        LFrag *f = &g->frags[i];
        bool mine = false;
        for (LBox *p = f->box; p && p != &g->b; p = p->parent)
            if (p == b) {
                mine = true;
                break;
            }
        if (!mine)
            continue;
        if (!any) {
            x0 = f->x;
            y0 = f->y;
            x1 = f->x + f->w;
            y1 = f->y + f->h;
            any = true;
        } else {
            x0 = l_min(x0, f->x);
            y0 = l_min(y0, f->y);
            x1 = l_max(x1, f->x + f->w);
            y1 = l_max(y1, f->y + f->h);
        }
    }
    if (!any) {
        *out = (AuRect){gx, gy, 0, 0};
        return true;
    }
    *out = (AuRect){gx + x0, gy + y0, x1 - x0, y1 - y0};
    return true;
}
