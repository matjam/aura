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
const LFcOps l_fc_grid = {l_grid_layout, l_grid_intrinsic};
const LFcOps l_fc_table = {l_table_layout, l_table_intrinsic};
const LFcOps l_fc_replaced = {replaced_layout, replaced_intrinsic};

const LFcOps *l_fc_ops(const LBlock *b)
{
    switch (b->b.inner) {
    case LI_FLEX: return &l_fc_flex;
    case LI_GRID: return &l_fc_grid;
    case LI_TABLE: return &l_fc_table;
    case LI_REPLACED: return &l_fc_replaced;
    default: return &l_fc_flow; /* rows, row groups, columns: laid out by their table */
    }
}

/* ---- Edges ---- */

void l_resolve_box_edges(LBlock *b, float cb_w)
{
    if (b->b.inner >= LI_TABLE && b->b.inner <= LI_TABLE_COLUMN_GROUP) {
        l_table_box_edges(b, cb_w);
        return;
    }
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

/* The aspect ratio of replaced box b: aspect-ratio, else its natural
 * ratio (an image's, or an inline SVG's from its size or viewBox). */
bool l_replaced_ratio(AuLayout *L, LBlock *b, float *ratio)
{
    if (b->b.inner != LI_REPLACED || lb_anon(&b->b))
        return false;
    float w, h, r;
    replaced_intrinsic_size(L, b, &w, &h, &r);
    if (!l_aspect_ratio(b, &r) && !(r > 0))
        return false;
    *ratio = r;
    return r > 0;
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

/* ---- Form controls ----
 *
 * Sizes and baselines follow current browsers (checked against Chrome):
 * a single-line control's content box holds one line of its font at its
 * line-height, centred vertically when the box is taller, and the control's
 * baseline is that line's baseline. Checkboxes, radio buttons and range
 * sliders put the bottom of their border box on the baseline (their bottom
 * margin hangs below it). Textareas, progress and meter bars align by
 * their bottom margin edge, as inline-blocks without a usable line. A list
 * box's baseline is that of the text in its last visible row. */

float l_control_line(AuLayout *L, const LBlock *b, float *ascent)
{
    const AuStyle *s = b->b.style;
    LFont *f = l_font(L, s);
    float lh = l_line_height(s, f);
    if (ascent)
        *ascent = floorf((lh - (f->m.ascent + f->m.descent)) / 2) + f->m.ascent;
    return lh;
}

/* A list box row: the line and 1px above and below. */
float l_listbox_row(AuLayout *L, const LBlock *b) { return l_control_line(L, b, NULL) + 2; }

/* Width of the drop-down arrow area of a single-line select. */
float l_select_arrow_w(const LBlock *b)
{
    if (b->b.style->rare->appearance == AU_APPEARANCE_NONE)
        return 0;
    return roundf(b->b.style->inh->font_size * 1.2f);
}

/* The file input's button width, for its label. */
float l_file_button_w(AuLayout *L, const LBlock *b)
{
    LFont *f = l_font(L, b->b.style);
    const char *t = b->rep && b->rep->label ? b->rep->label : "";
    return l_measure(L, f, b->b.style, t, strlen(t)) + 16;
}

/* Baseline of a laid-out replaced control from its border-box top (ch is
 * its content height), or NAN to align by the bottom margin edge. */
static float control_baseline(AuLayout *L, const LBlock *b, float ch)
{
    float top = b->bw[0] + b->p[0], asc, lh;
    switch (b->b.replaced) {
    case LR_TEXT_INPUT:
    case LR_BUTTON:
    case LR_SELECT:
    case LR_COLOR:
        lh = l_control_line(L, b, &asc);
        return top + (ch - lh) / 2 + asc;
    case LR_FILE:
        lh = l_control_line(L, b, &asc);
        return top + (ch - lh) / 2 + asc;
    case LR_LISTBOX: {
        int rows = b->rep && b->rep->rows ? b->rep->rows : 4;
        float rh = l_listbox_row(L, b);
        l_control_line(L, b, &asc);
        return top + (rows - 1) * rh + 1 + asc;
    }
    case LR_CHECKBOX:
    case LR_RADIO:
    case LR_RANGE:
        /* With appearance: none they are plain empty inline-blocks. */
        if (b->b.style->rare->appearance == AU_APPEARANCE_NONE)
            return NAN;
        return ch + l_bp_v(b);
    default:
        return NAN;
    }
}

float l_atomic_baseline(const LBlock *k)
{
    float mh = k->h + k->m[0] + k->m[2];
    if (k->b.replaced == LR_BUTTON && k->b.inner != LI_REPLACED) {
        /* A <button>: its first line; with no line, its content-box bottom. */
        float bl = !isnan(k->baseline) ? k->baseline : k->h - k->bw[2] - k->p[2];
        return k->m[0] + bl;
    }
    if ((k->b.inner == LI_FLEX || k->b.inner == LI_GRID) && !isnan(k->baseline))
        return k->m[0] + k->baseline;
    if (isnan(k->last_baseline) || (k->b.flags & LF_CLIPS))
        return mh;
    return k->m[0] + k->last_baseline;
}

/* Auto content width of a textarea: cols average characters. */
static float textarea_w(AuLayout *L, const LBlock *b)
{
    LFont *f = l_font(L, b->b.style);
    int cols = b->rep && b->rep->cols ? b->rep->cols : 20;
    return cols * f->m.avg_advance;
}

/* Moves the content of b (lines, fragments, child boxes) down by dy. */
void l_shift_content(LBlock *b, float dy)
{
    if (dy == 0)
        return;
    for (uint32_t i = 0; i < b->nlines; i++)
        b->lines[i].y += dy;
    for (uint32_t i = 0; i < b->nfrags; i++) {
        b->frags[i].y += dy;
        if (b->frags[i].kind == FR_ATOMIC)
            ((LBlock *)b->frags[i].box)->y += dy;
    }
    /* Block children, and floats reached through inline boxes. */
    LBox *c = b->b.first;
    while (c) {
        if (c->kind == LK_BLOCK && (c->flags & LF_ABSPOS)) {
            /* Its static position moves; so does the box if it was placed
             * there (laid out already, with top and bottom auto). */
            LBlock *k = (LBlock *)c;
            k->sy += dy;
            const AuStyleBox *s = c->style->box;
            if (s->top.kind == AU_LEN_AUTO && s->bottom.kind == AU_LEN_AUTO)
                k->y += dy;
        } else if (c->kind == LK_BLOCK) {
            if (!(c->flags & LF_INLINE_LEVEL))
                ((LBlock *)c)->y += dy;
        } else if (c->kind == LK_INLINE && c->first) {
            c = c->first;
            continue;
        }
        while (c && !c->next) {
            c = c->parent;
            if (c == &b->b)
                c = NULL;
        }
        if (c)
            c = c->next;
    }
    if (!isnan(b->baseline))
        b->baseline += dy;
    if (!isnan(b->last_baseline))
        b->last_baseline += dy;
}

/* The rendered legend of fieldset b, laid out and placed on its top
 * border; the space it takes is added to b's top padding (HTML, "The
 * fieldset and legend elements"). */
static void place_legend(AuLayout *L, LBlock *b, float content_w)
{
    LBlock *lg = NULL;
    for (LBox *c = b->b.first; c; c = c->next)
        if (c->kind == LK_BLOCK && (c->flags & LF_LEGEND)) {
            lg = (LBlock *)c;
            break;
        }
    if (!lg)
        return;
    l_layout_independent(L, lg, content_w, -1, content_w, LS_FIT);
    const AuStyleSurround *ls = lg->b.style->surr;
    float free = content_w - lg->w - lg->m[1] - lg->m[3];
    float x = b->bw[3] + b->p[3] + lg->m[3];
    bool la = ls->margin[3].kind == AU_LEN_AUTO, ra = ls->margin[1].kind == AU_LEN_AUTO;
    const char *al = l_attr(&lg->b, au_atom_lookup("align", 5));
    if (al && au_ieq(al, strlen(al), "center", 6))
        la = ra = true;
    else if (al && au_ieq(al, strlen(al), "right", 5))
        la = true, ra = false;
    if (free > 0 && la)
        x += ra ? free / 2 : free;
    float mh = lg->h + lg->m[0] + lg->m[2];
    float bt = b->bw[0];
    lg->x = x;
    lg->y = (mh >= bt ? 0 : (bt - mh) / 2) + lg->m[0];
    b->p[0] += l_max(mh, bt) - bt;
}

/* ---- Replaced elements ---- */

/* A canvas size attribute: a non-negative integer, else def. */
static int l_canvas_attr(const AuNode *n, const char *name, int def)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    const char *v = a && n && au_is_element(n) ? au_element_get_attr((const AuElement *)n, a) : NULL;
    if (!v)
        return def;
    while (au_is_space((unsigned char)*v))
        v++;
    if (*v == '+')
        v++;
    if (!au_is_digit((unsigned char)*v))
        return def;
    long r = 0;
    for (; au_is_digit((unsigned char)*v); v++)
        if (r < 1000000)
            r = r * 10 + (*v - '0');
    return (int)r;
}

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
        if (r && r->loaded && r->marker) {
            /* A marker image: its own size, then the gap. */
            *w = r->iw > 0 ? r->iw + L_MARKER_IMAGE_GAP : 0;
            *h = r->ih;
        } else if (r && r->loaded && r->ratio_only) {
            /* No natural size: replaced_size fills the available width,
             * and the intrinsic contributions are 0 (as in browsers). */
            *w = *h = 0;
            *ratio = r->ratio;
        } else if (r && r->loaded) {
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
        /* size (default 20) average characters (HTML, "The input element
         * as a text entry widget"), measured as Chrome measures them on
         * the Mac: size + 1 times the width of "x", rounded. That is 147
         * pixels at 13.33 pixels in Arial, Helvetica, Times and SF alike;
         * the OS/2 average width gave 115 to 172 by face, and a wide
         * field pushed Python's search button past a 400-pixel page. */
        const char *sz = l_attr(&b->b, AU_ATOM_size);
        int n = sz ? atoi(sz) : 20;
        if (n <= 0 || n > 1000)
            n = 20;
        float xw = roundf(l_measure(L, f, s, "x", 1));
        if (xw <= 0)
            xw = f->m.avg_advance;
        *w = (float)(n + 1) * xw;
        *h = l_control_line(L, b, NULL);
        return;
    }
    case LR_TEXTAREA: {
        int rows = r && r->rows ? r->rows : 2;
        *w = textarea_w(L, b);
        *h = rows * l_control_line(L, b, NULL);
        return;
    }
    case LR_BUTTON:
    case LR_COLOR:
        *w = r && r->label ? l_measure(L, f, s, r->label, strlen(r->label)) : 0;
        if (b->b.replaced == LR_COLOR)
            *w = 44;
        *h = l_control_line(L, b, NULL);
        return;
    case LR_FILE: {
        float bw = l_file_button_w(L, b);
        const char *t = r && r->aux ? r->aux : "";
        *w = bw + 4 + l_measure(L, f, s, t, strlen(t));
        *h = l_control_line(L, b, NULL) + 6;
        return;
    }
    case LR_SELECT: {
        const char *t = r && r->aux ? r->aux : "";
        *w = l_measure(L, f, s, t, strlen(t)) + l_select_arrow_w(b);
        *h = l_control_line(L, b, NULL);
        return;
    }
    case LR_LISTBOX: {
        float mw = 0;
        const char *t = r && r->label ? r->label : "";
        while (*t) {
            const char *e = strchr(t, '\n');
            size_t n = e ? (size_t)(e - t) : strlen(t);
            uint8_t fl = (uint8_t)t[0] & 15;
            float tw = n > 1 ? l_measure(L, f, s, t + 1, n - 1) : 0;
            if (fl & LB_GROUP)
                tw *= 1.1f; /* bold */
            if (fl & LB_INDENT)
                tw += roundf(em * 0.75f);
            mw = l_max(mw, tw);
            t += n + (e ? 1 : 0);
        }
        int rows = r && r->rows ? r->rows : 4;
        *w = mw + 4;
        *h = rows * l_listbox_row(L, b);
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
    case LR_PROGRESS:
        *w = em * 10;
        *h = em;
        return;
    case LR_METER:
        *w = em * 5;
        *h = em;
        return;
    case LR_DISC:
    case LR_CIRCLE:
    case LR_SQUARE:
    case LR_DISCLOSURE_OPEN:
    case LR_DISCLOSURE_CLOSED:
        *w = em * 0.8f;
        *h = f->m.x_height;
        return;
    case LR_CANVAS: {
        /* The bitmap's size: the width and height attributes, 300 x 150
         * by default (HTML, "The canvas element"); the ratio follows. */
        *w = (float)l_canvas_attr(b->b.node, "width", 300);
        *h = (float)l_canvas_attr(b->b.node, "height", 150);
        if (*w > 0 && *h > 0)
            *ratio = *w / *h;
        return;
    }
    case LR_SVG: {
        /* Absolute width and height attributes, and the ratio from them
         * or the viewBox. Neither size nor ratio: 300 x 150 (CSS 2.1
         * 10.3.2). A ratio alone fills the available width (replaced_size). */
        AuSvgIntrinsic in;
        au_svg_intrinsic(l_svg_names(L), au_as_element(b->b.node), em, &in);
        *ratio = in.ratio;
        *w = in.width;
        *h = in.height;
        if (in.ratio > 0) {
            if (*w > 0 && *h <= 0)
                *h = *w / in.ratio;
            else if (*h > 0 && *w <= 0)
                *w = *h * in.ratio;
        } else {
            if (*w <= 0)
                *w = 300;
            if (*h <= 0)
                *h = 150;
        }
        return;
    }
    default: {
        float aw = 300, ah = 150;
        AuAtom wa = au_atom_lookup("width", 5), ha = au_atom_lookup("height", 6);
        const char *ws = wa ? l_attr(&b->b, wa) : NULL, *hs = ha ? l_attr(&b->b, ha) : NULL;
        if (ws && atof(ws) > 0)
            aw = (float)atof(ws);
        if (hs && atof(hs) > 0)
            ah = (float)atof(hs);
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
        } else if (iw <= 0 && ratio > 0) {
            /* No natural width: fill the available width, or the
             * containing block when that is unbounded (flex sizing
             * passes an unbounded width; browsers fill the container). */
            float fill = avail_c > 0 && avail_c < 1e29f ? avail_c : cb_w;
            if (fill > 0 && fill < 1e29f) {
                w = fill;
                h = w / ratio;
            }
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

int l_size_contained(const LBlock *b)
{
    if (lb_anon(&b->b) || !b->b.style || b->b.replaced)
        return 0;
    if (b->b.inner != LI_FLOW && b->b.inner != LI_FLEX && b->b.inner != LI_GRID)
        return 0;
    const AuStyleRare *r = b->b.style->rare;
    if ((r->contain & AU_CONTAIN_SIZE) || r->container_type == AU_CONTAINER_TYPE_SIZE)
        return 3;
    if ((r->contain & AU_CONTAIN_INLINE_SIZE) || r->container_type == AU_CONTAINER_TYPE_INLINE_SIZE)
        return 1;
    return 0;
}

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
    L->intrinsic_runs++;
    float mn = 0, mx = 0;
    if (b->b.inner == LI_REPLACED) {
        float w, h, r;
        replaced_intrinsic_size(L, b, &w, &h, &r);
        float sh = l_spec_height(b, -1);
        if (sh >= 0 && r > 0)
            w = sh * r;
        mn = mx = w;
    } else if (b->b.replaced == LR_TEXTAREA) {
        mn = mx = textarea_w(L, b);
    } else if (l_size_contained(b) & 1) {
        mn = mx = b->b.style->rare->contain_intrinsic_w; /* sized as if empty, or contain-intrinsic-width */
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
    if (w >= 0 && b->b.inner == LI_TABLE) {
        /* A table is never narrower than its min-content width. */
        float imn, imx;
        l_intrinsic(L, b, &imn, &imx);
        mn = mx = l_max(w, imn);
    } else if (w >= 0 && b->b.inner != LI_REPLACED) {
        mn = mx = w;
    } else if (b->b.inner == LI_REPLACED) {
        float rw, rh;
        replaced_size(L, b, cb_w, -1, -1, false, &rw, &rh);
        mn = mx = rw;
        /* A compressible replaced element (CSS Sizing 3, 5.2.2): a
         * percentage width or max-width counts 0 for min-content. MDN's
         * 728-pixel banner (max-width: 100%) in a grid's auto column
         * widened the page to 760 pixels at a 400-pixel window. */
        const AuStyleBox *s = b->b.style->box;
        if (cb_w < 0 && !lb_anon(&b->b) && (l_len_pct(&s->width) || l_len_pct(&s->max_width)))
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
    /* An auto-width button is fit-content even as a block (HTML, "The
     * button element"); a textarea's auto width comes from cols, like a
     * replaced element's. */
    if (mode == LS_STRETCH && b->b.replaced == LR_BUTTON)
        mode = LS_FIT;
    if (b->b.replaced == LR_TEXTAREA && mode != LS_GIVEN && !lb_anon(&b->b) &&
        b->b.style->box->width.kind == AU_LEN_AUTO)
        return l_clamp_w(b, textarea_w(L, b), cb_w);
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
static bool same_cbh(const AuLayout *L, const LBlock *b, float a, float c)
{
    if (a == c)
        return true;
    /* Quirks mode: an auto-height block passes its percentage base down
     * to its children (l_layout_content), so they depend on it too. */
    if (L->doc && L->doc->quirks == AU_QUIRKS_FULL && b->b.inner == LI_FLOW)
        return false;
    if (lb_anon(&b->b))
        return true;
    const AuStyleBox *s = b->b.style->box;
    return !l_len_pct(&s->height) && !l_len_pct(&s->min_height) && !l_len_pct(&s->max_height);
}

static bool can_skip(AuLayout *L, LBlock *b, float content_w, float content_h, float cb_h)
{
    /* The content is placed from the border box's corner: a padding or
     * border above or left of it that changed (a percentage of the
     * containing block) moves all of it. */
    return (b->b.flags & (LF_HAS_LAYOUT | LF_DIRTY | LF_CHILD_DIRTY | LF_ESCAPES)) == LF_HAS_LAYOUT &&
           (b->b.flags & LF_BFC) && b->key_w == content_w && b->key_h == content_h &&
           b->key_top == b->p[0] + b->bw[0] && b->key_left == b->p[3] + b->bw[3] &&
           same_cbh(L, b, b->key_cbh, cb_h);
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
        b->ov = b->sov = (LArea){0, 0, b->w, b->h};
        return;
    }
    if (b->b.inner == LI_FLOW && !lb_anon(&b->b) && l_is_tag(&b->b, AU_ATOM_fieldset))
        place_legend(L, b, content_w);
    /* Tables and cells size themselves: a table's content_h is the height
     * of its grid (a minimum), and a cell's is only the percentage base of
     * its children; the cell's height is its content height, which its
     * table then stretches to the row. */
    bool tabular = b->b.inner == LI_TABLE || b->b.inner == LI_TABLE_CELL;
    if (can_skip(L, b, content_w, content_h, cb_h)) {
        L->stats.layouts_skipped++;
        b->skip_gen = L->gen;
        /* The edges were resolved again (a percentage padding follows the
         * containing block): the content keeps its size, the box takes
         * the new edges. */
        b->w = content_w + l_bp_h(b);
        float h = content_h >= 0 && !tabular ? content_h : b->key_ch;
        b->h = h + l_bp_v(b);
        return;
    }
    L->depth++;
    b->lay_gen = L->gen;
    b->b.flags &= ~(LF_TOO_DEEP | LF_ESCAPES);
    LConstraints c = {content_w, b->b.inner == LI_TABLE_CELL ? -1 : content_h, content_w, content_h};
    /* Quirks mode: an auto-height block passes its own percentage base
     * down (the percentage height quirk; see place_block). */
    if (content_h < 0 && b->b.inner == LI_FLOW && L->doc && L->doc->quirks == AU_QUIRKS_FULL &&
        !(b->b.flags & LF_FLOAT) && !(b->b.flags & LF_ABSPOS))
        c.cb_h = cb_h;
    if (b->b.inner == LI_REPLACED) {
        float w, h;
        replaced_size(L, b, content_w, cb_h, content_w, false, &w, &h);
        float r = 0;
        bool has_r = l_aspect_ratio(b, &r) && r > 0;
        if (!has_r) {
            /* A canvas or an inline SVG has no LReplaced ratio: it comes
             * from the width and height attributes or the viewBox. */
            float iw, ih;
            replaced_intrinsic_size(L, b, &iw, &ih, &r);
            has_r = r > 0;
        }
        if (l_spec_width(b, content_w) < 0 && l_spec_height(b, cb_h) < 0 && has_r)
            h = content_w / r;
        else if (l_spec_height(b, cb_h) < 0 && has_r)
            h = l_clamp_h(b, content_w / r, cb_h);
        ch = h;
        b->baseline = b->last_baseline = NAN;
    } else {
        ch = l_fc_ops(b)->layout(L, b, &c);
        if (l_size_contained(b) & 2)
            ch = b->b.style->rare->contain_intrinsic_h; /* the content is laid out, and overflows */
    }
    L->depth--;
    if (b->b.replaced == LR_TEXTAREA && content_h < 0) {
        /* rows lines; more content scrolls */
        int rows = b->rep && b->rep->rows ? b->rep->rows : 2;
        ch = rows * l_control_line(L, b, NULL);
    }
    float h = content_h >= 0 && !tabular ? content_h : ch;
    if (content_h < 0 && !tabular) {
        float ah = l_ratio_height(b, content_w);
        /* The ratio gives the height unless the content needs more
         * (min-height: auto behaviour of aspect-ratio). */
        if (ah >= 0)
            h = l_max(ah, ch);
        h = l_clamp_h(b, h, cb_h);
    }
    /* A button centres its content vertically in a taller box. */
    if (b->b.replaced == LR_BUTTON && b->b.inner == LI_FLOW && h > ch)
        l_shift_content(b, (h - ch) / 2);
    if (b->b.inner == LI_REPLACED && b->b.replaced)
        b->baseline = b->last_baseline = control_baseline(L, b, h);
    b->w = content_w + l_bp_h(b);
    b->h = h + l_bp_v(b);
    b->key_ch = h;
    b->key_top = b->p[0] + b->bw[0];
    b->key_left = b->p[3] + b->bw[3];
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
    /* A definite content height is the answer (l_layout_content uses it
     * as given); no layout needed. */
    if (content_h >= 0 && b->b.inner != LI_TABLE && b->b.inner != LI_TABLE_CELL)
        return content_h + l_bp_v(b);
    /* Heights are kept as content heights: the padding and border can
     * change while the content does not (percentages of the containing
     * block). */
    if (can_skip(L, b, content_w, content_h, cb_h)) {
        b->skip_gen = L->gen;
        return b->key_ch + l_bp_v(b);
    }
    if ((b->b.flags & (LF_MEASURE_OK | LF_DIRTY | LF_CHILD_DIRTY)) == LF_MEASURE_OK && b->mk_w == content_w &&
        b->mk_h == content_h && same_cbh(L, b, b->mk_cbh, cb_h)) {
        L->stats.layouts_skipped++;
        b->skip_gen = L->gen;
        return b->m_res + l_bp_v(b);
    }
    l_layout_content(L, b, content_w, content_h, cb_h);
    b->mk_w = content_w;
    b->mk_h = content_h;
    b->mk_cbh = cb_h;
    b->m_res = b->h - l_bp_v(b);
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
            if (it[i].n >= 6 && v[5].type == AU_CSSV_KEYWORD && v[5].kw == AU_K_inset)
                continue; /* inset */
            float dx = v[0].u.num, dy = v[1].u.num, blur = v[2].u.num, spread = v[3].u.num;
            /* As far as au_draw_box_shadow paints: three box blurs reach
             * about 1.5 times the blur radius. */
            float e = (blur > 0 ? blur * 1.5f + 2 : 0) + spread;
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
            /* An absolutely positioned box counts where its containing
             * block is b (or an inline box in b), or for the root the
             * initial containing block. One positioned against a box
             * outside b is laid out after b, so here it would have the
             * geometry of the layout before (none in a first layout). */
            bool outside = false;
            if (c->flags & LF_ABSPOS) {
                LBox *ref = k->cb && k->cb->kind != LK_BLOCK ? &l_geom_parent(k->cb)->b : k->cb;
                outside = ref ? ref != &b->b : b->b.parent != NULL;
            }
            if (!(c->flags & LF_FIXED) && !outside) {
                add_rect(ink, k->x + k->ov.x, k->y + k->ov.y, k->ov.w, k->ov.h);
                /* A clipping child contributes its margin box. */
                if (c->flags & LF_CLIPS)
                    add_rect(scroll, k->x, k->y, k->w + l_max(0, k->m[1]), k->h + l_max(0, k->m[2]));
                else
                    add_rect(scroll, k->x + k->sov.x, k->y + k->sov.y, k->sov.w, k->sov.h);
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
    /* Inline SVG with overflow visible draws outside its box by an amount
     * only the drawing knows: allow its own size again on every side. */
    if (b->b.replaced == LR_SVG && !lb_anon(&b->b) &&
        (b->b.style->box->overflow_x == AU_OVERFLOW_VISIBLE || b->b.style->box->overflow_y == AU_OVERFLOW_VISIBLE)) {
        float e = l_max(b->w, b->h);
        add_rect(&ink, -e, -e, b->w + 2 * e, b->h + 2 * e);
    }
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
    LArea sov = {0, 0, b->w, b->h};
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
        add_rect(&sov, content.x, content.y, content.w, content.h);
    }
    b->ov = ink;
    b->sov = sov;
}

/* ---- URLs ---- */

char *l_resolve_url(AuLayout *L, const char *base, const char *url, char *buf, size_t cap)
{
    size_t n = strlen(url);
    if (L->env.resolve_url) {
        if (L->env.resolve_url(L->env.ctx, base, url, buf, cap))
            return buf;
        /* Longer than buf: the result is url resolved against the base,
         * so room for url and a base of up to 4 KB, then 64 KB, covers it.
         * A URL the document refuses for another reason costs two tries. */
        size_t room = n + 4096;
        for (int i = 0; i < 2 && room > n; i++, room = n + 65536) {
            char *h = au_alloc(room);
            if (L->env.resolve_url(L->env.ctx, base, url, h, room))
                return h;
            au_free(h);
        }
    }
    char *out = n < cap ? buf : au_alloc(n + 1);
    memcpy(out, url, n + 1);
    return out;
}

void l_url_free(char *url, char *buf)
{
    if (url != buf)
        au_free(url);
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

/* An absolutely positioned box whose containing block is the initial one:
 * the root lays it out after the relative pass (l_layout_abspos_for). */
static bool late_abspos(const LBox *b)
{
    return b->kind == LK_BLOCK && (b->flags & LF_ABSPOS) && !((const LBlock *)b)->cb;
}

/* Offsets the relatively positioned boxes below top (and top itself when
 * it is one). Content reused in this pass kept its positions, offsets
 * included, and is not offset again. With skip_late, the boxes laid out
 * after this pass (late_abspos) are left for relative_pass_late. */
static void relative_pass(AuLayout *L, LBox *top, bool skip_late)
{
    LBox *b = top;
    while (b) {
        if (b->kind == LK_BLOCK && (b->flags & LF_RELPOS) && b != L->root) {
            LBlock *g = l_geom_parent(b);
            float cw = g ? g->w - l_bp_h(g) : L->vw;
            float ch = g && l_spec_height(g, -1) >= 0 ? g->h - l_bp_v(g) : -1;
            l_apply_relative((LBlock *)b, cw, ch);
        }
        bool reused = b->kind == LK_BLOCK && ((LBlock *)b)->skip_gen == L->gen && ((LBlock *)b)->lay_gen != L->gen;
        bool later = skip_late && b != top && late_abspos(b);
        if (b->first && b->kind != LK_TEXT && !(b->flags & LF_TOO_DEEP) && !reused && !later)
            b = b->first;
        else {
            while (b != top && !b->next)
                b = b->parent;
            b = b == top ? NULL : b->next;
        }
    }
}

/* The relative pass inside the boxes the root laid out after the first
 * one: before this, relatively positioned boxes inside a fixed box or an
 * absolutely positioned box without a positioned ancestor kept no offset
 * in a fresh layout, and got it on the next relayout. */
static void relative_pass_late(AuLayout *L)
{
    LBox *b = L->root;
    while (b) {
        bool late = b != L->root && late_abspos(b);
        if (late)
            relative_pass(L, b, false);
        if (b->first && b->kind != LK_TEXT && !(b->flags & LF_TOO_DEEP) && !late)
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
    if (l_top_layer(&b->b)) {
        /* the initial containing block, whatever the ancestors */
    } else if (!(b->b.flags & LF_FIXED)) {
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
    /* Queued again before its containing block finished (a table lays its
     * cells out twice): one entry, at the latest static position. Two
     * entries per pass would double at every level of nesting. */
    if (b->b.flags & LF_ABS_QUEUED) {
        for (uint32_t i = L->nabs; i-- > 0;)
            if (L->abs[i].box == &b->b) {
                L->abs[i].cb = cb;
                return;
            }
    }
    b->b.flags |= LF_ABS_QUEUED;
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
    if (L->ngrid_abs)
        l_grid_abspos_area(L, b, &r);
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
    /* An entry is taken out by clearing its box, and the gaps are closed
     * once at the end. Moving every later entry down for each one taken
     * cost the square of their number (a block with thousands of
     * positioned children). Laying a box out may queue more entries
     * after it, and may run this function for a block inside it, whose
     * mark is then behind this loop's place: it closes gaps only from
     * its mark on, so the entries before stay where they are. */
    for (uint32_t i = mark; i < L->nabs; i++) {
        LAbsEntry e = L->abs[i];
        if (!e.box || (cb && ref_block(e.cb) != cb))
            continue;
        L->abs[i].box = NULL;
        e.box->flags &= ~LF_ABS_QUEUED;
        /* (a containing block that never finished, too deep, is used anyway) */
        layout_abspos(L, (LBlock *)e.box, e.cb);
    }
    uint32_t n = mark;
    for (uint32_t i = mark; i < L->nabs; i++)
        if (L->abs[i].box)
            L->abs[n++] = L->abs[i];
    L->nabs = n;
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
    au_buf_init(&L->check_msg);
    L->need_build = true;
#ifdef AURA_INCR_CHECK
    L->check = 1;
#endif
    return L;
}

void au_layout_free(AuLayout *L)
{
    if (!L)
        return;
    l_dirty_clear(L);
    au_free(L->dirty);
    au_buf_free(&L->check_msg);
    l_free_tree(L);
    l_text_free_caches(L);
    au_svg_names_free(L->svg_names);
    au_free(L->items);
    au_free(L->segs);
    au_free(L->abs);
    au_free(L->grid_abs);
    au_free(L->hidden);
    au_free(L);
}

void au_layout_release(AuLayout *L)
{
    l_dirty_clear(L);
    l_free_tree(L);
    l_text_free_caches(L);
    au_free(L->items);
    au_free(L->segs);
    au_free(L->abs);
    au_free(L->grid_abs);
    au_free(L->hidden);
    L->items = NULL;
    L->segs = NULL;
    L->abs = NULL;
    L->grid_abs = NULL;
    L->hidden = NULL;
    L->nitems = L->items_cap = 0;
    L->nsegs = L->segs_cap = 0;
    L->nabs = L->abs_cap = 0;
    L->ngrid_abs = L->grid_abs_cap = 0;
    L->nhidden = L->hidden_cap = 0;
    L->nview_boxes = 0;
    L->need_build = true;
    L->need_paint = true;
}

AuLayoutScroll *au_layout_scroll_save(AuLayout *L, uint32_t *n)
{
    AuLayoutScroll *out = NULL;
    uint32_t count = 0, cap = 0;
    LBox *b = L->root;
    while (b) {
        if (b->kind == LK_BLOCK && b->node && au_is_element(b->node) && au_as_element(b->node)->box == b) {
            LBlock *k = (LBlock *)b;
            if (k->scroll_x != 0 || k->scroll_y != 0) {
                if (count == cap) {
                    cap = cap ? cap * 2 : 8;
                    out = au_realloc(out, cap * sizeof *out);
                }
                au_node_ref(b->node);
                out[count++] = (AuLayoutScroll){au_as_element(b->node), k->scroll_x, k->scroll_y};
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
    *n = count;
    return out;
}

void au_layout_scroll_restore(AuLayout *L, const AuLayoutScroll *s, uint32_t n)
{
    if (!L->root || L->need_build)
        return;
    for (uint32_t i = 0; i < n; i++) {
        LBox *b = s[i].el->box;
        if (!b || b->kind != LK_BLOCK || !(b->flags & LF_CLIPS))
            continue;
        LBlock *k = (LBlock *)b;
        float vw = k->w - k->bw[1] - k->bw[3], vh = k->h - k->bw[0] - k->bw[2];
        k->scroll_x = l_max(0, l_min(l_max(0, k->sw - vw), s[i].x));
        k->scroll_y = l_max(0, l_min(l_max(0, k->sh - vh), s[i].y));
        L->need_paint = true;
    }
}

void au_layout_scroll_free(AuLayoutScroll *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        au_node_unref(&s[i].el->node);
    au_free(s);
}

void au_layout_set_document(AuLayout *L, AuDocument *doc)
{
    l_dirty_clear(L);
    l_free_tree(L);
    L->doc = doc;
    L->need_build = true;
    L->need_paint = true;
}

AuSvgNames *l_svg_names(AuLayout *L)
{
    if (!L->svg_names)
        L->svg_names = au_svg_names_new();
    return L->svg_names;
}

void au_layout_tree_changed(AuLayout *L)
{
    au_svg_id_index_free(L->svg_ids);
    L->svg_ids = NULL;
    l_dirty_clear(L);
    L->need_build = true;
    L->need_paint = true;
}

/* Marks b for layout and every ancestor for a layout of its children.
 * It does not stop at an ancestor already marked: a box that layout did
 * not reach (in a cell or a float laid out from a cache) can keep its
 * mark while its ancestors lost theirs, and stopping there left a
 * restyled box with its old geometry. */
void l_mark_dirty(LBox *b)
{
    b->flags |= LF_DIRTY;
    b->flags &= ~(LF_INTRINSIC_OK | LF_MEASURE_OK);
    l_table_forget(b);
    for (LBox *p = b->parent; p; p = p->parent) {
        p->flags |= LF_CHILD_DIRTY;
        p->flags &= ~(LF_INTRINSIC_OK | LF_MEASURE_OK);
        l_table_forget(p);
    }
}

void au_layout_images_changed(AuLayout *L)
{
    if (L->need_build || !L->root || L->images_shape) {
        L->need_build = true;
        L->need_paint = true;
        return;
    }
    /* The tree keeps its shape: only image boxes read the loader. A box
     * whose image changed size (or became known, or failed) is laid out
     * again; backgrounds and masks are looked up when painted. */
    LBox *b = L->root;
    while (b) {
        if (b->kind == LK_BLOCK && b->replaced == LR_IMAGE && ((LBlock *)b)->rep) {
            int r = l_image_refresh(L, ((LBlock *)b)->rep);
            /* A width and height in pixels leave nothing to the image. */
            const AuStyleBox *sb = b->style->box;
            if (r == 2 && !(sb->width.kind == AU_LEN_PX && sb->height.kind == AU_LEN_PX)) {
                l_mark_dirty(b);
                L->need_layout = true;
                L->other_relayout = true;
            }
            if (r)
                L->need_paint = true;
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

/* Adds the paint extent of b, restyled for paint only, to the damage.
 * When the list is full, the new extent joins the rectangle it grows
 * least. */
static void damage_box(AuLayout *L, LBox *b)
{
    if (L->dmg_all)
        return;
    AuRect r;
    if (!l_box_paint_extent(L, b, &r)) {
        L->dmg_all = true;
        return;
    }
    if (!(r.w > 0 && r.h > 0))
        return; /* it paints nothing */
    if (L->ndmg < L_DAMAGE) {
        L->dmg[L->ndmg++] = r;
        return;
    }
    int best = 0;
    float grow = 0;
    for (int i = 0; i < L->ndmg; i++) {
        AuRect a = L->dmg[i];
        float x0 = l_min(a.x, r.x), y0 = l_min(a.y, r.y);
        float x1 = l_max(a.x + a.w, r.x + r.w), y1 = l_max(a.y + a.h, r.y + r.h);
        float g = (x1 - x0) * (y1 - y0) - a.w * a.h;
        if (i == 0 || g < grow) {
            best = i;
            grow = g;
        }
    }
    AuRect a = L->dmg[best];
    float x0 = l_min(a.x, r.x), y0 = l_min(a.y, r.y);
    float x1 = l_max(a.x + a.w, r.x + r.w), y1 = l_max(a.y + a.h, r.y + r.h);
    L->dmg[best] = (AuRect){x0, y0, x1 - x0, y1 - y0};
}

int au_layout_take_damage(AuLayout *L, AuRect *out, int max)
{
    int n = L->dmg_all ? -1 : L->ndmg;
    if (n > max)
        n = -1;
    for (int i = 0; i < n; i++)
        out[i] = L->dmg[i];
    L->dmg_all = false;
    L->ndmg = 0;
    return n;
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
        if (a->inhr->text_transform != b->inhr->text_transform || a->inhr->quotes != b->inhr->quotes ||
            a->inhr->list_style_image != b->inhr->list_style_image)
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
            l_style_contains_fixed(a) != l_style_contains_fixed(b) ||
            x->line_clamp != y->line_clamp || x->contain != y->contain)
            return 3;
        if (x->container_type != y->container_type) /* size containment */
            r = r < 2 ? 2 : r;
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

/* Whether a style change can make or unmake a stacking context or a
 * positioned layer (creates_stacking in draw.c): the layer lists are
 * built again even when the change needs only a repaint. */
static bool stacking_style_differs(const AuStyle *a, const AuStyle *b)
{
    if (a->box->z_index != b->box->z_index)
        return true;
    const AuStyleRare *x = a->rare, *y = b->rare;
    if (x == y)
        return false;
    return (x->opacity < 1) != (y->opacity < 1) || (x->transform == NULL) != (y->transform == NULL) ||
           (x->translate == NULL) != (y->translate == NULL) || (x->rotate == NULL) != (y->rotate == NULL) ||
           (x->scale == NULL) != (y->scale == NULL) || x->isolation != y->isolation ||
           (x->filter == NULL) != (y->filter == NULL) || x->mix_blend_mode != y->mix_blend_mode ||
           (x->clip_path == NULL) != (y->clip_path == NULL) ||
           (x->contain & AU_CONTAIN_PAINT) != (y->contain & AU_CONTAIN_PAINT) ||
           (au_style_mask_layer_count(a) > 0) != (au_style_mask_layer_count(b) > 0);
}

static const AuStyle *current_style(const LBox *b)
{
    AuNode *n = b->node;
    if (!n)
        return NULL;
    const AuStyle *es;
    if (n->type == AU_NODE_TEXT) {
        /* the text takes its flat parent's style: a slot's, or a shadow
         * root's host's */
        bool flat = n->doc && n->doc->has_shadow;
        AuNode *p = flat ? au_flat_parent(n) : n->parent;
        while (p && p->type != AU_NODE_ELEMENT)
            p = flat ? au_flat_parent(p) : p->parent;
        es = p ? au_element_style(au_as_element(p)) : NULL;
    } else if (au_is_element(n)) {
        es = au_element_style(au_as_element(n));
    } else
        return NULL;
    if (!es)
        return NULL;
    if (b->pseudo)
        return au_style_pseudo(es, (AuCssPseudo)b->pseudo);
    if (b->kind == LK_TEXT && n->type == AU_NODE_ELEMENT) {
        /* Generated text (a pseudo-element's content, a marker's number):
         * it has the style of its pseudo-element. */
        int which = 0;
        for (const LBox *p = b->parent; p && p->node == n && !which; p = p->parent) {
            which = p->pseudo;
            if (!lb_anon(p))
                break;
        }
        if (which == AU_CSS_PSEUDO_MARKER) {
            const AuStyle *ms = au_style_pseudo(es, AU_CSS_PSEUDO_MARKER);
            return ms ? ms : es;
        }
        if (which)
            return au_style_pseudo(es, (AuCssPseudo)which);
        /* Content of a display: contents pseudo-element, placed in the
         * element's parent box: unchanged while the element still has
         * that pseudo style. */
        for (const AuStyle *ps = es->pseudos; ps; ps = ps->next)
            if (ps == b->style)
                return ps;
        return NULL;
    }
    return es;
}

void au_layout_styles_changed(AuLayout *L)
{
    /* An id attribute may have changed. */
    au_svg_id_index_free(L->svg_ids);
    L->svg_ids = NULL;
    /* The subtrees whose DOM changed first: their new boxes take the
     * current styles, so the comparison below finds nothing there. */
    l_rebuild_dirty(L);
    if (L->need_build || !L->root) {
        L->need_build = true;
        return;
    }
    LBox *b = L->root;
    int worst = 0;
    /* A box whose new style changes what is built (display, float,
     * position, content, list markers, counters) is rebuilt with its
     * parent's subtree: the changed box is recorded as a DOM change of
     * its parent (or, for a pseudo-element, of its element) and its
     * descendants are not compared. */
    bool deferred = false;
    while (b && worst < 3) {
        const AuStyle *ns;
        if (lb_anon(b)) {
            ns = b->parent ? b->parent->style : NULL;
            if (b->parent && b->parent->pseudo)
                ns = NULL; /* compared through its element */
            else if (b->pseudo == AU_CSS_PSEUDO_MARKER) {
                /* A marker has its ::marker style, or its item's. */
                const AuStyle *es = b->node && au_is_element(b->node) ? au_element_style(au_as_element(b->node)) : NULL;
                const AuStyle *ms = es ? au_style_pseudo(es, AU_CSS_PSEUDO_MARKER) : NULL;
                ns = ms ? ms : es;
            }
        } else {
            ns = current_style(b);
        }
        if (!lb_anon(b) && b->node && !ns && b->node->type != AU_NODE_TEXT) {
            worst = 3;
            break;
        }
        if (ns && ns != b->style) {
            int d = style_diff(b->style, ns);
            if (d == 3 && b->node && (!lb_anon(b) || b->pseudo == AU_CSS_PSEUDO_MARKER)) {
                AuNode *at = b->pseudo || !b->node->parent ? b->node : b->node->parent;
                au_layout_node_changed(L, at);
                if (L->need_build) {
                    worst = 3;
                    break;
                }
                deferred = true;
                if (worst < 2)
                    worst = 2;
                while (b && !b->next) /* the subtree is built again */
                    b = b->parent;
                if (b)
                    b = b->next;
                continue;
            }
            if (d > worst)
                worst = d;
            if (d < 3 && stacking_style_differs(b->style, ns))
                L->need_restack = true;
            if (d == 1)
                damage_box(L, b); /* with its old style: the same geometry */
            if (d == 2) {
                l_mark_dirty(b);
                L->other_relayout = true;
            }
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
    /* An element left out of the tree for display: none that is shown
     * now, or a display: contents element whose new style builds
     * something else (no box of its own was compared above). */
    for (uint32_t i = 0; i < L->nhidden && worst < 3; i++) {
        AuElement *hel = L->hidden[i].el;
        const AuStyle *hs = au_element_style(hel), *was = L->hidden[i].style;
        bool changed = false;
        if (hs && was && was->box->display == AU_DISPLAY_NONE)
            changed = hs->box->display != AU_DISPLAY_NONE;
        else if (hs && was && hs != was)
            changed = style_diff(was, hs) == 3;
        if (changed) {
            au_layout_node_changed(L, hel->node.parent ? hel->node.parent : &hel->node);
            if (L->need_build)
                worst = 3;
            deferred = true;
        }
    }
    if (deferred && worst < 3) {
        l_rebuild_dirty(L);
        if (L->need_build)
            worst = 3;
        else if (worst < 2)
            worst = 2;
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
    L->nabs = 0; /* empty after a finished pass; a new tree has no queued boxes */
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
    relative_pass(L, L->root, true);
    l_layout_abspos_for(L, NULL, 0);
    relative_pass_late(L);
    l_finish_overflow(root);
    /* Document size: the root's scrollable overflow plus positioned boxes
     * outside it (shadows and outlines do not widen the page). */
    float right = root->x + root->sov.x + root->sov.w, bottom = root->y + root->sov.y + root->sov.h;
    right = l_max(right, root->x + root->w + root->m[1]);
    bottom = l_max(bottom, root->y + root->h + root->m[2]);
    LBox *b = L->root;
    while (b) {
        bool descend = b->first && b->kind != LK_TEXT && !(b->flags & (LF_CLIPS | LF_TOO_DEEP));
        if (b->kind == LK_BLOCK && (b->flags & LF_ABSPOS) && !(b->flags & LF_FIXED)) {
            float x, y;
            l_box_origin(b, &x, &y);
            LBlock *k = (LBlock *)b;
            right = l_max(right, x + k->sov.x + k->sov.w);
            bottom = l_max(bottom, y + k->sov.y + k->sov.h);
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

void au_layout_set_dark(AuLayout *L, bool dark)
{
    if (L->dark == dark)
        return;
    L->dark = dark;
    if (L->src_viewport)
        L->need_build = true; /* a <source media> or sizes condition may choose another image */
}

bool au_layout_update(AuLayout *L, float vw, float vh)
{
    if (vw < 1)
        vw = 1;
    if (vh < 1)
        vh = 1;
    bool changed = false;
    l_rebuild_dirty(L);
    if (vw != L->vw || vh != L->vh)
        L->other_relayout = true;
    if (L->src_viewport && (vw != L->src_vw || vh != L->src_vh))
        L->need_build = true; /* another srcset candidate may apply */
    if (L->need_build) {
        L->src_vw = vw;
        L->src_vh = vh;
        /* Keep inner scroll offsets across the rebuild: each goes back on
         * its element's new box, and l_finish_overflow clamps it to the
         * new extent. Without this a sheet or font that arrived after
         * MDN's script scrolled its sidebar put the sidebar back at the
         * top. */
        uint32_t nscrolls = 0;
        AuLayoutScroll *scrolls = L->root ? au_layout_scroll_save(L, &nscrolls) : NULL;
        l_build_tree(L);
        for (uint32_t i = 0; i < nscrolls; i++) {
            LBox *nb = scrolls[i].el->box;
            if (nb && nb->kind == LK_BLOCK && (nb->flags & LF_CLIPS)) {
                ((LBlock *)nb)->scroll_x = scrolls[i].x;
                ((LBlock *)nb)->scroll_y = scrolls[i].y;
            }
        }
        au_layout_scroll_free(scrolls, nscrolls);
        L->build_count++;
        L->need_build = false;
        L->other_relayout = false; /* laid out from scratch below */
        L->need_layout = true;
        L->need_restack = true;
    }
    if (!L->root) {
        L->doc_w = vw;
        L->doc_h = vh;
        L->vw = vw;
        L->vh = vh;
        L->need_layout = false;
        L->dmg_all = true;
        return true;
    }
    if (L->need_layout || vw != L->vw || vh != L->vh) {
        /* A viewport change is an incremental layout like any other: the
         * root's constraints change, and each independent subtree below is
         * laid out again only when its own width, height or percentage
         * base changed, it is dirty, or it holds a box positioned against
         * something outside it (LF_ESCAPES; the viewport for fixed boxes).
         * Intrinsic widths do not depend on the viewport. Lengths in
         * viewport units are computed by the cascade, so they arrive as
         * style changes (au_layout_styles_changed). */
        L->vw = vw;
        L->vh = vh;
        L->gen++;
        layout_root(L);
        L->layout_count++;
        L->need_layout = false;
        L->need_restack = true;
        changed = true;
    }
    if (L->need_restack) {
        l_build_stacking(L);
        L->need_restack = false;
        L->dmg_all = true; /* the paint order changed */
    }
    if (changed) {
        L->need_paint = true;
        L->dmg_all = true;
    }
    if (L->check)
        l_check_incremental(L);
    L->rebuilt = false;
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

void au_layout_pass_counts(const AuLayout *L, uint32_t *builds, uint32_t *layouts)
{
    *builds = L->build_count;
    *layouts = L->layout_count;
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
    s->tree_builds = L->build_count;
    s->layout_passes = L->layout_count;
    s->intrinsic_runs = L->intrinsic_runs;
}

static bool box_rect(LBox *b, AuRect *out)
{
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

bool au_layout_element_rect(AuLayout *L, const AuElement *el, AuRect *out)
{
    (void)L;
    LBox *b = el ? (LBox *)el->box : NULL;
    return b ? box_rect(b, out) : false;
}

/* Maps r through the transform of k, whose border box starts at (ox, oy),
 * to the bounding box of the result. */
static void map_through_transform(const LBlock *k, float ox, float oy, AuRect *r)
{
    AuMatrix m;
    if (!l_transform_matrix(k, &m))
        return;
    float xs[4] = {r->x, r->x + r->w, r->x + r->w, r->x};
    float ys[4] = {r->y, r->y, r->y + r->h, r->y + r->h};
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        float lx = xs[i] - ox, ly = ys[i] - oy;
        float X = m.a * lx + m.c * ly + m.e + ox, Y = m.b * lx + m.d * ly + m.f + oy;
        x0 = l_min(x0, X), y0 = l_min(y0, Y), x1 = l_max(x1, X), y1 = l_max(y1, Y);
    }
    *r = (AuRect){x0, y0, x1 - x0, y1 - y0};
}

bool au_layout_element_visual_rect(AuLayout *L, const AuElement *el, float view_x, float view_y, AuRect *out)
{
    LBox *b = el ? (LBox *)el->box : NULL;
    if (!b || !box_rect(b, out))
        return false;
    float svx = L->view_scroll_x, svy = L->view_scroll_y;
    L->view_scroll_x = view_x; /* l_sticky_offset reads them */
    L->view_scroll_y = view_y;
    /* Each level, from the box up: the scroll offset of an ancestor that
     * scrolls it (the containing block chain, as layer_origin decides),
     * then the level's transform about its own origin, then its sticky
     * or fixed offset. Offsets from below a level are moves inside that
     * level's box, so its transform applies to them too. */
    LBox *need = b;
    bool escaped = false; /* above a box fixed to the viewport: nothing scrolls it */
    int guard = 0;
    for (LBox *p = b; p && guard < L_MAX_DEPTH * 4; p = p->parent, guard++) {
        if (p->kind != LK_BLOCK)
            continue;
        LBlock *k = (LBlock *)p;
        if (p != b && !escaped) {
            bool ap;
            if (need->kind == LK_BLOCK && (need->flags & LF_ABSPOS)) {
                LBox *cb = ((LBlock *)need)->cb;
                LBox *ref = cb && cb->kind != LK_BLOCK ? &l_geom_parent(cb)->b : cb;
                ap = p == ref || p == cb;
            } else {
                ap = true;
            }
            if (ap) {
                need = p;
                if (p->flags & LF_CLIPS) {
                    out->x -= k->scroll_x;
                    out->y -= k->scroll_y;
                }
            }
        }
        if (!lb_anon(p)) {
            float ox, oy;
            l_box_origin(p, &ox, &oy);
            map_through_transform(k, ox, oy, out);
        }
        if (p->flags & LF_STICKY) {
            float dx, dy = l_sticky_offset(L, k, &dx);
            out->x += dx;
            out->y += dy;
        }
        if ((p->flags & LF_FIXED) && !k->cb && !escaped) {
            out->x += view_x;
            out->y += view_y;
            escaped = true;
        }
    }
    L->view_scroll_x = svx;
    L->view_scroll_y = svy;
    return true;
}

/* A box script can scroll: overflow auto, scroll or hidden (not clip). */
static bool script_scrollable(const LBox *b)
{
    if (!b || b->kind != LK_BLOCK || !(b->flags & LF_CLIPS) || lb_anon(b))
        return false;
    const AuStyleBox *s = b->style->box;
    return s->overflow_x != AU_OVERFLOW_CLIP || s->overflow_y != AU_OVERFLOW_CLIP;
}

bool au_layout_element_scroll(AuLayout *L, const AuElement *el, float out[6])
{
    (void)L;
    LBox *b = el ? (LBox *)el->box : NULL;
    if (!script_scrollable(b))
        return false;
    LBlock *k = (LBlock *)b;
    float vw = k->w - k->bw[1] - k->bw[3], vh = k->h - k->bw[0] - k->bw[2];
    out[0] = k->scroll_x;
    out[1] = k->scroll_y;
    out[2] = l_max(vw, k->sw);
    out[3] = l_max(vh, k->sh);
    out[4] = vw;
    out[5] = vh;
    return true;
}

bool au_layout_element_scroll_to(AuLayout *L, const AuElement *el, float x, float y)
{
    LBox *b = el ? (LBox *)el->box : NULL;
    if (!script_scrollable(b))
        return false;
    LBlock *k = (LBlock *)b;
    float vw = k->w - k->bw[1] - k->bw[3], vh = k->h - k->bw[0] - k->bw[2];
    float mx = l_max(0, k->sw - vw), my = l_max(0, k->sh - vh);
    /* Script scrolls overflow: hidden boxes too. */
    float nx = l_max(0, l_min(mx, x == x ? x : 0)), ny = l_max(0, l_min(my, y == y ? y : 0));
    if (nx == k->scroll_x && ny == k->scroll_y)
        return false;
    k->scroll_x = nx;
    k->scroll_y = ny;
    L->need_paint = true;
    return true;
}

/* The value names url (after resolving) as one of its images. */
static bool value_names_image(AuLayout *L, const AuCssValue *v, const char *base, const char *url)
{
    if (!v || v->type != AU_CSSV_URL || !v->u.str)
        return false;
    char buf[2048];
    char *abs = l_resolve_url(L, base, v->u.str, buf, sizeof buf);
    bool same = !strcmp(abs, url);
    l_url_free(abs, buf);
    return same;
}

static bool style_names_image(AuLayout *L, const AuStyle *s, const char *url)
{
    AuBgLayer ly;
    if (s->bg->background_image) {
        size_t n = au_style_bg_layer_count(s);
        for (size_t i = 0; i < n; i++) {
            au_style_bg_layer(s, i, &ly);
            if (value_names_image(L, ly.image, ly.base_url, url))
                return true;
        }
    }
    if (s->rare->mask_image) {
        size_t n = au_style_mask_layer_count(s);
        for (size_t i = 0; i < n; i++) {
            au_style_mask_layer(s, i, &ly);
            if (value_names_image(L, ly.image, ly.base_url, url))
                return true;
        }
    }
    return false;
}

int au_layout_image_users(AuLayout *L, const char *url, void (*fn)(void *ctx, AuRect r), void *ctx)
{
    if (!L->root || L->need_build || L->ndirty)
        return -1;
    int count = 0;
    const AuStyle *last = NULL;
    bool last_hit = false;
    LBox *b = L->root;
    while (b) {
        bool hit = false;
        if (b->kind == LK_BLOCK && b->replaced == LR_IMAGE && ((LBlock *)b)->rep) {
            const char *src = ((LBlock *)b)->rep->src;
            hit = src && !strcmp(src, url);
        } else if (b->kind == LK_BLOCK && b->replaced == LR_SVG) {
            hit = true; /* an <image> inside may show it */
        }
        if (!hit && b->kind != LK_TEXT && b->style && !lb_anon(b) &&
            (b->style->bg->background_image || b->style->rare->mask_image)) {
            if (b->style != last) {
                last = b->style;
                last_hit = style_names_image(L, b->style, url);
            }
            hit = last_hit;
        }
        if (hit) {
            /* The canvas takes the root's or the body's background, and a
             * box that does not move with the document has no fixed
             * document rectangle: the caller repaints everything. */
            if (b == L->root || b->parent == L->root)
                return -1;
            for (LBox *p = b; p; p = p->parent) {
                const AuStyle *ps = p->style;
                if ((p->flags & LF_FIXED) || (ps && (ps->box->position == AU_POSITION_STICKY || ps->rare->transform ||
                                                     ps->rare->translate || ps->rare->rotate || ps->rare->scale)))
                    return -1;
                if (p->kind == LK_BLOCK && p != L->root &&
                    (((LBlock *)p)->scroll_x != 0 || ((LBlock *)p)->scroll_y != 0))
                    return -1;
            }
            AuRect r;
            if (box_rect(b, &r)) {
                if (b->kind == LK_BLOCK) {
                    /* Ink outside the border box: an inline SVG with
                     * visible overflow. */
                    const LArea *ov = &((LBlock *)b)->ov;
                    float x0 = l_min(r.x, r.x + ov->x), y0 = l_min(r.y, r.y + ov->y);
                    float x1 = l_max(r.x + r.w, r.x + ov->x + ov->w), y1 = l_max(r.y + r.h, r.y + ov->y + ov->h);
                    r = (AuRect){x0, y0, x1 - x0, y1 - y0};
                }
                fn(ctx, r);
                count++;
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
    return count;
}
