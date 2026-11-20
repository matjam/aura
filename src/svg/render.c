/* The SVG renderer: walks a styled SVG element tree and paints it into a
 * canvas. See svg.h for what it supports.
 *
 * Geometry is built in each element's user space and mapped to the canvas
 * by the current transform (CTM) before it is filled, so everything is
 * drawn at the resolution it is shown. Strokes are outlined in user space
 * (non-uniform scales give the right shapes) unless vector-effect asks for
 * a non-scaling stroke. Gradients are painted through the canvas's
 * transformed gradients. Group opacity, masks and clip paths that are not
 * one axis-aligned rectangle draw through canvas groups; a shape with
 * opacity and only one of fill and stroke folds the opacity into its
 * paint instead. */
#include "svg_int.h"

#include <stdlib.h>

#define DEG 0.017453292519943295f

AuSvgNames *au_svg_names_new(void)
{
    AuSvgNames *n = au_calloc(sizeof *n);
#define N(f, s) n->f = au_atom_cstr(s);
    SVG_NAMES(N)
#undef N
    return n;
}

void au_svg_names_free(AuSvgNames *n) { au_free(n); }

/* ---- Attributes ---- */

const char *svg_attr(const AuElement *el, AuAtom a)
{
    if (!a)
        return NULL;
    for (uint32_t i = 0; i < el->nattrs; i++)
        if (el->attrs[i].name == a)
            return el->attrs[i].value;
    return NULL;
}

bool svg_is_tag(const AuElement *el, AuAtom tag) { return el->ns == AU_NS_SVG && el->tag == tag; }

void svg_path_append(AuPath *dst, const AuPath *src)
{
    const float *pt = src->pts;
    for (size_t i = 0; i < src->nops; i++) {
        switch (src->ops[i]) {
        case AU_PATH_MOVE: au_path_move_to(dst, pt[0], pt[1]); pt += 2; break;
        case AU_PATH_LINE: au_path_line_to(dst, pt[0], pt[1]); pt += 2; break;
        case AU_PATH_QUAD: au_path_quad_to(dst, pt[0], pt[1], pt[2], pt[3]); pt += 4; break;
        case AU_PATH_CUBIC: au_path_cubic_to(dst, pt[0], pt[1], pt[2], pt[3], pt[4], pt[5]); pt += 6; break;
        default: au_path_close(dst); break;
        }
    }
}

static const char *svg_href(const SvgCtx *x, const AuElement *el)
{
    const char *h = svg_attr(el, x->n->href);
    return h ? h : svg_attr(el, x->n->xlink_href);
}

/* axis: 0 x (viewport width), 1 y (height), 2 other (normalized diagonal). */
float svg_len_attr(const AuElement *el, AuAtom a, const SvgFrame *f, int axis, float dflt)
{
    SvgLen l;
    if (!svg_parse_length(svg_attr(el, a), &l))
        return dflt;
    float ref = axis == 0 ? f->vw : axis == 1 ? f->vh : sqrtf((f->vw * f->vw + f->vh * f->vh) / 2);
    float em = f->st.font_size > 0 ? f->st.font_size : 16;
    return svg_len_px(l, em, ref);
}

static float css_len_user(const AuLen *l, const SvgFrame *f)
{
    if (l->kind == AU_LEN_NUMBER)
        return l->u.num;
    float diag = sqrtf((f->vw * f->vw + f->vh * f->vh) / 2);
    return au_len_resolve(l, diag);
}

/* ---- State ---- */

static bool len_eq(const AuLen *a, const AuLen *b)
{
    if (a->kind != b->kind)
        return false;
    if (a->kind == AU_LEN_CALC)
        return a->u.calc == b->u.calc;
    return a->u.px == b->u.px;
}

void svg_state(const AuElement *el, const SvgFrame *parent, bool inst, SvgState *o)
{
    const AuStyle *s = au_element_style(el);
    if (!s) {
        /* No computed style (an element the cascade has not reached):
         * the parent's state, or nothing visible. */
        if (parent) {
            *o = parent->st;
        } else {
            memset(o, 0, sizeof *o);
            o->visibility = AU_VISIBILITY_HIDDEN;
        }
        return;
    }
    const AuStyle *d = NULL;
    if (inst && el->node.parent && el->node.parent->type == AU_NODE_ELEMENT)
        d = au_element_style(au_as_element(el->node.parent));
    const SvgState *p = parent ? &parent->st : NULL;
    bool sub = inst && d && p;
    /* A value equal to the DOM parent's was (most likely) inherited from
     * it: in an instance it inherits from the instance's parent. */
#define PICK(field, sv, dv) o->field = (sub && (sv) == (dv)) ? p->field : (sv)
    PICK(fill, s->inhr->fill, d->inhr->fill);
    PICK(stroke, s->inhr->stroke, d->inhr->stroke);
    PICK(dash, s->inhr->stroke_dasharray, d->inhr->stroke_dasharray);
    PICK(order, s->inhr->paint_order, d->inhr->paint_order);
    PICK(family, s->inh->font_family, d->inh->font_family);
    PICK(fvs, s->inhr->font_variation_settings, d->inhr->font_variation_settings);
    PICK(fill_op, s->inhr->fill_opacity, d->inhr->fill_opacity);
    PICK(stroke_op, s->inhr->stroke_opacity, d->inhr->stroke_opacity);
    PICK(miter, s->inhr->stroke_miterlimit, d->inhr->stroke_miterlimit);
    PICK(font_size, s->inh->font_size, d->inh->font_size);
    PICK(letter_spacing, s->inhr->letter_spacing, d->inhr->letter_spacing);
    PICK(stretch, s->inh->font_stretch, d->inh->font_stretch);
    PICK(color, s->inh->color, d->inh->color);
    PICK(weight, s->inh->font_weight, d->inh->font_weight);
    PICK(fill_rule, s->inhr->fill_rule, d->inhr->fill_rule);
    PICK(clip_rule, s->inhr->clip_rule, d->inhr->clip_rule);
    PICK(cap, s->inhr->stroke_linecap, d->inhr->stroke_linecap);
    PICK(join, s->inhr->stroke_linejoin, d->inhr->stroke_linejoin);
    PICK(anchor, s->inhr->text_anchor, d->inhr->text_anchor);
    PICK(visibility, s->inh->visibility, d->inh->visibility);
    PICK(font_style, s->inh->font_style, d->inh->font_style);
    PICK(opsz, s->inhr->font_optical_sizing, d->inhr->font_optical_sizing);
#undef PICK
    o->sw = (sub && len_eq(&s->inhr->stroke_width, &d->inhr->stroke_width)) ? p->sw : s->inhr->stroke_width;
    o->dashoff = (sub && len_eq(&s->inhr->stroke_dashoffset, &d->inhr->stroke_dashoffset)) ? p->dashoff
                                                                                          : s->inhr->stroke_dashoffset;
}

/* ---- Ids ---- */

static int id_cmp(const void *a, const void *b)
{
    const SvgIdEntry *x = a, *y = b;
    if (x->id != y->id)
        return x->id < y->id ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

struct AuSvgIdIndex {
    SvgIdEntry *ids;
    uint32_t n;
};

AuSvgIdIndex *au_svg_id_index_new(AuNode *top)
{
    AuSvgIdIndex *x = au_calloc(sizeof *x);
    uint32_t cap = 0, order = 0;
    for (AuNode *n = top; n; n = au_node_next(n, top)) {
        if (n->type != AU_NODE_ELEMENT || !au_as_element(n)->id)
            continue;
        if (x->n == cap) {
            cap = cap ? cap * 2 : 16;
            x->ids = au_realloc(x->ids, cap * sizeof *x->ids);
        }
        x->ids[x->n++] = (SvgIdEntry){au_as_element(n)->id, order++, au_as_element(n)};
    }
    if (x->n)
        qsort(x->ids, x->n, sizeof *x->ids, id_cmp);
    return x;
}

void au_svg_id_index_free(AuSvgIdIndex *x)
{
    if (!x)
        return;
    au_free(x->ids);
    au_free(x);
}

AuElement *au_svg_id_index_find(const AuSvgIdIndex *x, AuAtom id)
{
    uint32_t lo = 0, hi = x->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (x->ids[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < x->n && x->ids[lo].id == id ? x->ids[lo].el : NULL; /* the first in tree order */
}

static AuElement *find_id(SvgCtx *x, AuAtom id)
{
    if (!id)
        return NULL;
    if (x->env && x->env->find_id)
        return x->env->find_id(x->env->ctx, id);
    if (!x->ids)
        x->ids = au_svg_id_index_new(&x->root->node);
    return au_svg_id_index_find(x->ids, id);
}

/* The element a local reference ("#id", or url(#id) text) names. */
static AuElement *find_ref(SvgCtx *x, const char *ref, size_t n)
{
    if (!ref)
        return NULL;
    const char *h = memchr(ref, '#', n);
    if (!h || h != ref) /* only same-document references */
        return NULL;
    size_t len = n - 1;
    if (!len || len > 256)
        return NULL;
    AuAtom a = au_atom_lookup(h + 1, len);
    return a ? find_id(x, a) : NULL;
}

/* ---- Transforms ---- */

static float tf_arg(const AuCssValue *v, float basis)
{
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num * basis / 100;
    if (v->type == AU_CSSV_DIMENSION || v->type == AU_CSSV_NUMBER)
        return v->u.num;
    return 0;
}

static const AuCssValue *items(const AuCssValue *v, uint32_t *n)
{
    if (v->type == AU_CSSV_LIST || v->type == AU_CSSV_COMMA_LIST) {
        *n = v->n;
        return v->u.items;
    }
    *n = 1;
    return v;
}

/* The CSS transform of an SVG element (transform-box: view-box, so
 * percentages are of the viewport), about its transform-origin. */
static bool css_transform(const AuStyle *s, const SvgFrame *f, AuMatrix *out)
{
    if (!s)
        return false;
    const AuStyleRare *r = s->rare;
    if (!r->transform)
        return false;
    float w = f->vw, h = f->vh, ox = 0, oy = 0;
    uint32_t n;
    if (r->transform_origin) {
        const AuCssValue *it = items(&r->transform_origin->value, &n);
        if (n > 0)
            ox = it[0].type == AU_CSSV_KEYWORD ? (it[0].kw == AU_K_right ? w : it[0].kw == AU_K_center ? w / 2 : 0)
                                               : tf_arg(&it[0], w);
        if (n > 1)
            oy = it[1].type == AU_CSSV_KEYWORD ? (it[1].kw == AU_K_bottom ? h : it[1].kw == AU_K_center ? h / 2 : 0)
                                               : tf_arg(&it[1], h);
    }
    AuMatrix m = {1, 0, 0, 1, ox, oy};
    const AuCssValue *it = items(&r->transform->value, &n);
    for (uint32_t i = 0; i < n; i++) {
        const AuCssValue *fn = &it[i];
        if (fn->type != AU_CSSV_FUNCTION)
            continue;
        const AuCssValue *a = fn->u.items;
        uint32_t k = fn->n;
        float x0 = k > 0 ? tf_arg(&a[0], w) : 0, y0 = k > 1 ? tf_arg(&a[1], h) : 0;
        AuMatrix t = {1, 0, 0, 1, 0, 0};
        switch (fn->kw) {
        case AU_K_translate: case AU_K_translate3d: t.e = x0, t.f = y0; break;
        case AU_K_translatex: t.e = x0; break;
        case AU_K_translatey: t.f = k > 0 ? tf_arg(&a[0], h) : 0; break;
        case AU_K_scale: case AU_K_scale3d:
            t.a = k > 0 ? tf_arg(&a[0], 100) : 1;
            t.d = k > 1 ? tf_arg(&a[1], 100) : t.a;
            break;
        case AU_K_scalex: t.a = k > 0 ? tf_arg(&a[0], 100) : 1; break;
        case AU_K_scaley: t.d = k > 0 ? tf_arg(&a[0], 100) : 1; break;
        case AU_K_rotate: case AU_K_rotatez: {
            float rr = x0 * DEG;
            t = (AuMatrix){cosf(rr), sinf(rr), -sinf(rr), cosf(rr), 0, 0};
            break;
        }
        case AU_K_skew: t.c = tanf(x0 * DEG), t.b = tanf(y0 * DEG); break;
        case AU_K_skewx: t.c = tanf(x0 * DEG); break;
        case AU_K_skewy: t.b = tanf(x0 * DEG); break;
        case AU_K_matrix:
            if (k >= 6)
                t = (AuMatrix){a[0].u.num, a[1].u.num, a[2].u.num, a[3].u.num, tf_arg(&a[4], 0), tf_arg(&a[5], 0)};
            break;
        default: break;
        }
        au_matrix_multiply(&m, &t);
    }
    AuMatrix back = {1, 0, 0, 1, -ox, -oy};
    au_matrix_multiply(&m, &back);
    *out = m;
    return true;
}

/* The element's own transform: CSS when set, else the attribute. */
static void local_transform(const SvgCtx *x, const AuElement *el, const SvgFrame *f, AuMatrix *ctm)
{
    AuMatrix t;
    if (css_transform(au_element_style(el), f, &t)) {
        au_matrix_multiply(ctm, &t);
        return;
    }
    const char *ts = svg_attr(el, x->n->transform);
    if (ts && au_svg_transform_parse(ts, &t))
        au_matrix_multiply(ctm, &t);
}

static float mat_scale(const AuMatrix *m)
{
    float det = fabsf(m->a * m->d - m->b * m->c);
    return det > 0 ? sqrtf(det) : 0;
}

static bool mat_axis_aligned(const AuMatrix *m)
{
    return fabsf(m->b) < 1e-6f && fabsf(m->c) < 1e-6f && m->a > 0 && m->d > 0;
}

static AuRect map_rect(const AuMatrix *m, float x0, float y0, float x1, float y1)
{
    float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    for (int i = 0; i < 4; i++) {
        float X = m->a * xs[i] + m->c * ys[i] + m->e, Y = m->b * xs[i] + m->d * ys[i] + m->f;
        mnx = X < mnx ? X : mnx;
        mxx = X > mxx ? X : mxx;
        mny = Y < mny ? Y : mny;
        mxy = Y > mxy ? Y : mxy;
    }
    return (AuRect){mnx, mny, mxx - mnx, mxy - mny};
}

/* ---- Geometry ---- */

enum { K_NONE, K_CONTAINER, K_SHAPE, K_USE, K_SVG, K_TEXT, K_IMAGE, K_SWITCH, K_FOREIGN, K_SKIP };

static int kind_of(const SvgCtx *x, const AuElement *el)
{
    if (el->ns != AU_NS_SVG)
        return K_NONE;
    const AuSvgNames *n = x->n;
    AuAtom t = el->tag;
    if (t == n->g || t == n->a)
        return K_CONTAINER;
    if (t == n->path || t == n->rect || t == n->circle || t == n->ellipse || t == n->line || t == n->polyline ||
        t == n->polygon)
        return K_SHAPE;
    if (t == n->use)
        return K_USE;
    if (t == n->svg)
        return K_SVG;
    if (t == n->text)
        return K_TEXT;
    if (t == n->image)
        return K_IMAGE;
    if (t == n->switch_)
        return K_SWITCH;
    if (t == n->foreign_object)
        return K_FOREIGN;
    return K_SKIP; /* defs, symbol, gradients, clipPath, mask, style, title, ... and unknown elements */
}

/* Appends the shape's outline (user space). Returns false when the shape
 * draws nothing (zero size, bad data). *closed: a closed outline. */
static bool shape_path(const SvgCtx *x, const AuElement *el, const SvgFrame *f, AuPath *p)
{
    const AuSvgNames *n = x->n;
    AuAtom t = el->tag;
    if (t == n->path) {
        const char *d = svg_attr(el, n->d);
        if (!d)
            return false;
        au_svg_path_parse(d, strlen(d), p);
        return p->nops > 0;
    }
    if (t == n->rect) {
        float rx0 = svg_len_attr(el, n->x, f, 0, 0), ry0 = svg_len_attr(el, n->y, f, 1, 0);
        float w = svg_len_attr(el, n->width, f, 0, 0), h = svg_len_attr(el, n->height, f, 1, 0);
        if (!(w > 0) || !(h > 0))
            return false;
        float rx = svg_len_attr(el, n->rx, f, 0, -1), ry = svg_len_attr(el, n->ry, f, 1, -1);
        if (rx < 0 && ry < 0)
            rx = ry = 0;
        else if (rx < 0)
            rx = ry;
        else if (ry < 0)
            ry = rx;
        if (rx > w / 2)
            rx = w / 2;
        if (ry > h / 2)
            ry = h / 2;
        AuRadii r;
        for (int i = 0; i < 4; i++)
            r.rx[i] = rx, r.ry[i] = ry;
        au_path_rounded_rect(p, (AuRect){rx0, ry0, w, h}, rx > 0 && ry > 0 ? &r : NULL, false);
        return true;
    }
    if (t == n->circle || t == n->ellipse) {
        float cx = svg_len_attr(el, n->cx, f, 0, 0), cy = svg_len_attr(el, n->cy, f, 1, 0);
        float rx, ry;
        if (t == n->circle) {
            rx = ry = svg_len_attr(el, n->r, f, 2, 0);
        } else {
            rx = svg_len_attr(el, n->rx, f, 0, -1);
            ry = svg_len_attr(el, n->ry, f, 1, -1);
            if (rx < 0)
                rx = ry;
            if (ry < 0)
                ry = rx;
        }
        if (!(rx > 0) || !(ry > 0))
            return false;
        au_path_ellipse(p, cx, cy, rx, ry);
        return true;
    }
    if (t == n->line) {
        au_path_move_to(p, svg_len_attr(el, n->x1, f, 0, 0), svg_len_attr(el, n->y1, f, 1, 0));
        au_path_line_to(p, svg_len_attr(el, n->x2, f, 0, 0), svg_len_attr(el, n->y2, f, 1, 0));
        return true;
    }
    if (t == n->polyline || t == n->polygon) {
        const char *s = svg_attr(el, n->points);
        if (!s)
            return false;
        float a, b;
        bool first = true;
        while (svg_ws(*s))
            s++;
        while (*s) {
            if (!svg_parse_number(&s, &a))
                break;
            svg_skip_sep(&s);
            if (!svg_parse_number(&s, &b))
                break; /* an odd count: the last number is ignored */
            svg_skip_sep(&s);
            if (first)
                au_path_move_to(p, a, b);
            else
                au_path_line_to(p, a, b);
            first = false;
        }
        if (first)
            return false;
        if (t == n->polygon)
            au_path_close(p);
        return true;
    }
    return false;
}

/* Tight bounds of a path (curves sampled), user space. */
static bool path_bbox(const AuPath *p, float b[4])
{
    bool any = false;
    float cx = 0, cy = 0;
    const float *pt = p->pts;
#define ADD(X, Y)                                    \
    do {                                             \
        float X_ = (X), Y_ = (Y);                    \
        if (!any) {                                  \
            b[0] = b[2] = X_, b[1] = b[3] = Y_;      \
            any = true;                              \
        } else {                                     \
            b[0] = X_ < b[0] ? X_ : b[0];            \
            b[2] = X_ > b[2] ? X_ : b[2];            \
            b[1] = Y_ < b[1] ? Y_ : b[1];            \
            b[3] = Y_ > b[3] ? Y_ : b[3];            \
        }                                            \
    } while (0)
    for (size_t i = 0; i < p->nops; i++) {
        switch (p->ops[i]) {
        case AU_PATH_MOVE:
        case AU_PATH_LINE:
            cx = pt[0], cy = pt[1];
            ADD(cx, cy);
            pt += 2;
            break;
        case AU_PATH_QUAD:
            for (int k = 1; k <= 8; k++) {
                float t = k / 8.0f, m = 1 - t;
                ADD(m * m * cx + 2 * m * t * pt[0] + t * t * pt[2], m * m * cy + 2 * m * t * pt[1] + t * t * pt[3]);
            }
            cx = pt[2], cy = pt[3];
            pt += 4;
            break;
        case AU_PATH_CUBIC:
            for (int k = 1; k <= 16; k++) {
                float t = k / 16.0f, m = 1 - t;
                float A = m * m * m, B = 3 * m * m * t, C = 3 * m * t * t, D = t * t * t;
                ADD(A * cx + B * pt[0] + C * pt[2] + D * pt[4], A * cy + B * pt[1] + C * pt[3] + D * pt[5]);
            }
            cx = pt[4], cy = pt[5];
            pt += 6;
            break;
        default:
            break;
        }
    }
#undef ADD
    return any;
}

static void bbox_union(float b[4], bool *any, const float c[4])
{
    if (!*any) {
        memcpy(b, c, 4 * sizeof(float));
        *any = true;
        return;
    }
    b[0] = c[0] < b[0] ? c[0] : b[0];
    b[1] = c[1] < b[1] ? c[1] : b[1];
    b[2] = c[2] > b[2] ? c[2] : b[2];
    b[3] = c[3] > b[3] ? c[3] : b[3];
}

static void child_frame(SvgCtx *x, const AuElement *el, const SvgFrame *parent, SvgFrame *f);

/* The object bounding box of el in its user space (its transform not
 * applied): geometry only, strokes excluded. */
static bool element_bbox(SvgCtx *x, AuElement *el, const SvgFrame *f, float b[4], int depth)
{
    if (depth > 16 || ++x->painted > SVG_MAX_ELEMENTS)
        return false;
    int k = kind_of(x, el);
    if (k == K_SHAPE) {
        AuPath p;
        au_path_init(&p);
        bool ok = shape_path(x, el, f, &p) && path_bbox(&p, b);
        au_path_free(&p);
        return ok;
    }
    if (k == K_TEXT)
        return svg_text_bbox(x, el, f, b);
    if (k == K_IMAGE || k == K_FOREIGN) {
        float ix = svg_len_attr(el, x->n->x, f, 0, 0), iy = svg_len_attr(el, x->n->y, f, 1, 0);
        float w = svg_len_attr(el, x->n->width, f, 0, 0), h = svg_len_attr(el, x->n->height, f, 1, 0);
        if (!(w > 0 && h > 0))
            return false;
        b[0] = ix, b[1] = iy, b[2] = ix + w, b[3] = iy + h;
        return true;
    }
    if (k == K_USE) {
        AuElement *t = find_ref(x, svg_href(x, el), svg_href(x, el) ? strlen(svg_href(x, el)) : 0);
        if (!t || t == el)
            return false;
        float tb[4];
        SvgFrame cf;
        child_frame(x, t, f, &cf);
        cf.ctm = (AuMatrix){1, 0, 0, 1, 0, 0};
        if (!element_bbox(x, t, &cf, tb, depth + 1))
            return false;
        float ux = svg_len_attr(el, x->n->x, f, 0, 0), uy = svg_len_attr(el, x->n->y, f, 1, 0);
        b[0] = tb[0] + ux, b[1] = tb[1] + uy, b[2] = tb[2] + ux, b[3] = tb[3] + uy;
        return true;
    }
    if (k == K_CONTAINER || k == K_SVG || k == K_SWITCH) {
        bool any = false;
        for (AuNode *c = el->node.first_child; c; c = c->next) {
            if (c->type != AU_NODE_ELEMENT)
                continue;
            AuElement *ce = au_as_element(c);
            const AuStyle *cs = au_element_style(ce);
            if (!cs || cs->box->display == AU_DISPLAY_NONE)
                continue;
            SvgFrame cf;
            child_frame(x, ce, f, &cf);
            cf.ctm = (AuMatrix){1, 0, 0, 1, 0, 0};
            local_transform(x, ce, f, &cf.ctm);
            float cb[4];
            if (!element_bbox(x, ce, &cf, cb, depth + 1))
                continue;
            AuRect r = map_rect(&cf.ctm, cb[0], cb[1], cb[2], cb[3]);
            float rb[4] = {r.x, r.y, r.x + r.w, r.y + r.h};
            bbox_union(b, &any, rb);
        }
        return any;
    }
    return false;
}

/* ---- Paint servers ---- */

typedef struct SvgPaint {
    int kind; /* 0 none, 1 colour, 2 gradient */
    AuColor color;
    AuGradient g;
} SvgPaint;

static AuColor paint_color(uint32_t css, float opacity)
{
    uint32_t a = css & 255;
    float fa = a * (!(opacity > 0) ? 0 : opacity > 1 ? 1 : opacity); /* a NaN opacity is 0 */
    uint32_t na = (uint32_t)(fa + 0.5f);
    return (AuColor)((na << 24) | (css >> 8));
}

static void gattr(SvgCtx *x, AuElement *const *chain, int n, AuAtom a, const char **out)
{
    (void)x;
    *out = NULL;
    for (int i = 0; i < n && !*out; i++)
        *out = svg_attr(chain[i], a);
}

/* A gradient coordinate: user space lengths, or fractions of the bbox. */
static float gcoord(const char *s, bool obb, const SvgFrame *f, int axis, float dflt)
{
    SvgLen l;
    if (!svg_parse_length(s, &l))
        return dflt;
    if (obb)
        return l.unit == SVG_U_PCT ? l.v / 100 : l.unit == SVG_U_NUM || l.unit == SVG_U_PX ? l.v : svg_len_px(l, 16, 1);
    float ref = axis == 0 ? f->vw : axis == 1 ? f->vh : sqrtf((f->vw * f->vw + f->vh * f->vh) / 2);
    return svg_len_px(l, f->st.font_size > 0 ? f->st.font_size : 16, ref);
}

static bool gradient_paint(SvgCtx *x, AuElement *gel, const SvgFrame *f, const float bbox[4], float opacity,
                           SvgPaint *out)
{
    const AuSvgNames *n = x->n;
    /* The href chain, nearest first, stopping at cycles. */
    AuElement *chain[SVG_MAX_HREF_CHAIN];
    int nc = 0;
    for (AuElement *e = gel; e && nc < SVG_MAX_HREF_CHAIN;) {
        if (!svg_is_tag(e, n->linear_gradient) && !svg_is_tag(e, n->radial_gradient))
            break;
        bool seen = false;
        for (int i = 0; i < nc; i++)
            seen |= chain[i] == e;
        if (seen)
            break;
        chain[nc++] = e;
        const char *h = svg_href(x, e);
        e = h ? find_ref(x, h, strlen(h)) : NULL;
    }
    bool radial = svg_is_tag(gel, n->radial_gradient);
    const char *units, *gt, *spread;
    gattr(x, chain, nc, n->gradient_units, &units);
    gattr(x, chain, nc, n->gradient_transform, &gt);
    gattr(x, chain, nc, n->spread_method, &spread);
    bool obb = !units || strcmp(units, "userSpaceOnUse") != 0;
    /* Stops: from the first gradient in the chain that has any. */
    AuGradient *g = &out->g;
    memset(g, 0, sizeof *g);
    for (int i = 0; i < nc && g->nstops == 0; i++) {
        float last = 0;
        for (AuNode *c = chain[i]->node.first_child; c; c = c->next) {
            if (c->type != AU_NODE_ELEMENT || !svg_is_tag(au_as_element(c), n->stop))
                continue;
            AuElement *se = au_as_element(c);
            const AuStyle *ss = au_element_style(se);
            if (!ss)
                continue;
            float off = 0;
            SvgLen l;
            if (svg_parse_length(svg_attr(se, n->offset), &l))
                off = l.unit == SVG_U_PCT ? l.v / 100 : l.v;
            off = off < 0 ? 0 : off > 1 ? 1 : off;
            if (off < last)
                off = last;
            last = off;
            uint32_t col = au_style_color(ss, ss->rare->stop_color);
            if (g->nstops < AU_GRADIENT_MAX_STOPS)
                au_gradient_add_stop(g, off, paint_color(col, ss->rare->stop_opacity * opacity));
        }
    }
    if (g->nstops == 0)
        return false;
    if (g->nstops == 1) {
        out->kind = 1;
        out->color = g->stops[0].color;
        return true;
    }
    AuMatrix m = f->ctm;
    if (obb) {
        float bw = bbox[2] - bbox[0], bh = bbox[3] - bbox[1];
        if (!(bw > 0) || !(bh > 0))
            return false; /* SVG: an empty box does not paint */
        AuMatrix bm = {bw, 0, 0, bh, bbox[0], bbox[1]};
        au_matrix_multiply(&m, &bm);
    }
    AuMatrix tm;
    if (gt && au_svg_transform_parse(gt, &tm))
        au_matrix_multiply(&m, &tm);
    g->has_matrix = true;
    g->matrix = m;
    g->spread = spread && !strcmp(spread, "reflect") ? AU_SPREAD_REFLECT
                : spread && !strcmp(spread, "repeat") ? AU_SPREAD_REPEAT
                                                      : AU_SPREAD_PAD;
    const char *v;
    if (radial) {
        g->radial = true;
        gattr(x, chain, nc, n->cx, &v);
        g->x0 = gcoord(v, obb, f, 0, obb ? 0.5f : f->vw / 2);
        gattr(x, chain, nc, n->cy, &v);
        g->y0 = gcoord(v, obb, f, 1, obb ? 0.5f : f->vh / 2);
        gattr(x, chain, nc, n->r, &v);
        float r = gcoord(v, obb, f, 2, obb ? 0.5f : sqrtf((f->vw * f->vw + f->vh * f->vh) / 2) / 2);
        if (!(r > 0)) {
            /* A zero radius paints the last stop's colour. */
            out->kind = 1;
            out->color = g->stops[g->nstops - 1].color;
            return true;
        }
        g->rx = g->ry = r;
        const char *fxs, *fys;
        gattr(x, chain, nc, n->fx, &fxs);
        gattr(x, chain, nc, n->fy, &fys);
        float fx = fxs ? gcoord(fxs, obb, f, 0, g->x0) : g->x0, fy = fys ? gcoord(fys, obb, f, 1, g->y0) : g->y0;
        if (fx != g->x0 || fy != g->y0) {
            g->focal = true;
            g->fx = fx;
            g->fy = fy;
        }
    } else {
        gattr(x, chain, nc, n->x1, &v);
        g->x0 = gcoord(v, obb, f, 0, 0);
        gattr(x, chain, nc, n->y1, &v);
        g->y0 = gcoord(v, obb, f, 1, 0);
        gattr(x, chain, nc, n->x2, &v);
        g->x1 = gcoord(v, obb, f, 0, obb ? 1 : f->vw);
        gattr(x, chain, nc, n->y2, &v);
        g->y1 = gcoord(v, obb, f, 1, 0);
        if (g->x0 == g->x1 && g->y0 == g->y1) {
            out->kind = 1;
            out->color = g->stops[g->nstops - 1].color;
            return true;
        }
    }
    out->kind = 2;
    return true;
}

static bool resolve_paint(SvgCtx *x, const AuCssBlob *b, const SvgFrame *f, const float bbox[4], float opacity,
                          SvgPaint *out)
{
    out->kind = 0;
    if (!b || !(opacity > 0))
        return false;
    const AuCssValue *v = &b->value;
    const AuCssValue *fallback = NULL;
    if (v->type == AU_CSSV_LIST && v->n == 2) {
        fallback = &v->u.items[1];
        v = &v->u.items[0];
    }
    if (v->type == AU_CSSV_COLOR) {
        uint32_t c = v->u.rgba == AU_COLOR_CURRENT ? f->st.color : v->u.rgba;
        out->kind = 1;
        out->color = paint_color(c, opacity);
        return (out->color >> 24) != 0;
    }
    if (v->type == AU_CSSV_URL) {
        AuElement *t = find_ref(x, v->u.str, v->n);
        if (t && (svg_is_tag(t, x->n->linear_gradient) || svg_is_tag(t, x->n->radial_gradient)))
            return gradient_paint(x, t, f, bbox, opacity, out);
        /* Patterns are not drawn; a missing or unsupported server uses
         * the fallback colour, else nothing. */
        if (fallback && fallback->type == AU_CSSV_COLOR) {
            uint32_t c = fallback->u.rgba == AU_COLOR_CURRENT ? f->st.color : fallback->u.rgba;
            out->kind = 1;
            out->color = paint_color(c, opacity);
            return true;
        }
    }
    return false;
}

/* ---- Drawing ---- */

static void fill_with(SvgCtx *x, const AuPath *dev, AuFillRule rule, const SvgPaint *p)
{
    if (p->kind == 1)
        au_fill_path(x->c, dev, rule, p->color);
    else if (p->kind == 2)
        au_fill_path_gradient(x->c, dev, rule, &p->g);
}

static void stroke_style(const SvgState *st, const SvgFrame *f, float width, float scale, AuStrokeStyle *ss,
                         float *dashes)
{
    memset(ss, 0, sizeof *ss);
    ss->width = width;
    ss->cap = st->cap == AU_STROKE_LINECAP_ROUND ? AU_CAP_ROUND : st->cap == AU_STROKE_LINECAP_SQUARE ? AU_CAP_SQUARE : AU_CAP_BUTT;
    ss->join = st->join == AU_STROKE_LINEJOIN_ROUND ? AU_JOIN_ROUND : st->join == AU_STROKE_LINEJOIN_BEVEL ? AU_JOIN_BEVEL : AU_JOIN_MITER;
    ss->miter_limit = st->miter;
    ss->tolerance = scale > 0 ? 0.1f / scale : 0.1f;
    if (st->dash && !(st->dash->value.type == AU_CSSV_KEYWORD)) {
        uint32_t n;
        const AuCssValue *it = items(&st->dash->value, &n);
        size_t nd = 0;
        for (uint32_t i = 0; i < n && nd < AU_STROKE_MAX_DASHES; i++) {
            uint32_t m;
            const AuCssValue *in = items(&it[i], &m);
            for (uint32_t k = 0; k < m && nd < AU_STROKE_MAX_DASHES; k++) {
                float diag = sqrtf((f->vw * f->vw + f->vh * f->vh) / 2);
                float d = in[k].type == AU_CSSV_PERCENT ? in[k].u.num * diag / 100 : in[k].u.num;
                dashes[nd++] = d * (scale > 0 && width != 0 ? 1 : 1);
            }
        }
        ss->dashes = dashes;
        ss->ndashes = nd;
        ss->dash_offset = css_len_user(&st->dashoff, f);
    }
}

static bool order_stroke_first(const SvgState *st)
{
    if (!st->order || st->order->value.type != AU_CSSV_LIST)
        return st->order && st->order->value.type == AU_CSSV_KEYWORD && st->order->value.kw == AU_K_stroke;
    uint32_t n;
    const AuCssValue *it = items(&st->order->value, &n);
    for (uint32_t i = 0; i < n; i++) {
        if (it[i].kw == AU_K_stroke)
            return true;
        if (it[i].kw == AU_K_fill)
            return false;
    }
    return false;
}

void svg_paint_geometry(SvgCtx *x, const AuElement *el, const SvgFrame *f, const AuPath *path, const float bbox[4],
                        float opacity, bool closed_hint)
{
    (void)closed_hint;
    const SvgState *st = &f->st;
    if (st->visibility != AU_VISIBILITY_VISIBLE)
        return;
    const AuStyle *s = au_element_style(el);
    float scale = mat_scale(&f->ctm);
    SvgPaint fp, sp;
    bool fill = resolve_paint(x, st->fill, f, bbox, st->fill_op * opacity, &fp);
    float sw = css_len_user(&st->sw, f);
    bool stroke = sw > 0 && resolve_paint(x, st->stroke, f, bbox, st->stroke_op * opacity, &sp);
    if (!fill && !stroke)
        return;
    AuPath dev;
    au_path_init(&dev);
    for (int pass = 0; pass < 2; pass++) {
        bool do_stroke = (pass == 0) == order_stroke_first(st);
        if (!do_stroke && fill) {
            dev.nops = dev.npts = 0;
            svg_path_append(&dev, path);
            au_path_transform(&dev, &f->ctm);
            fill_with(x, &dev, st->fill_rule == AU_FILL_RULE_EVENODD ? AU_FILL_EVENODD : AU_FILL_NONZERO, &fp);
        } else if (do_stroke && stroke) {
            float dashes[AU_STROKE_MAX_DASHES];
            AuStrokeStyle ss;
            dev.nops = dev.npts = 0;
            bool nonscaling = s && s->rare->vector_effect == AU_VECTOR_EFFECT_NON_SCALING_STROKE;
            if (nonscaling) {
                /* Outline in canvas space at the specified width. */
                AuPath u;
                au_path_init(&u);
                svg_path_append(&u, path);
                au_path_transform(&u, &f->ctm);
                stroke_style(st, f, sw, 1, &ss, dashes);
                au_path_stroke_style(&u, &ss, &dev);
                au_path_free(&u);
            } else {
                stroke_style(st, f, sw, scale, &ss, dashes);
                au_path_stroke_style(path, &ss, &dev);
                au_path_transform(&dev, &f->ctm);
            }
            /* A gradient stroke uses the geometry's box, as fills do. */
            fill_with(x, &dev, AU_FILL_NONZERO, &sp);
        }
    }
    au_path_free(&dev);
}

/* ---- Conditional processing ---- */

static bool conditions_pass(const SvgCtx *x, const AuElement *el)
{
    const char *ext = svg_attr(el, x->n->required_extensions);
    if (ext) {
        const char *s = ext;
        while (svg_ws(*s))
            s++;
        if (*s)
            return false; /* no extensions are supported; an empty list is false too in SVG 2 */
        return false;
    }
    const char *lang = svg_attr(el, x->n->system_language);
    if (lang) {
        /* Match English, the language Aura reports. */
        const char *s = lang;
        bool ok = false;
        while (*s) {
            while (svg_ws(*s) || *s == ',')
                s++;
            if ((s[0] == 'e' || s[0] == 'E') && (s[1] == 'n' || s[1] == 'N') && (!s[2] || s[2] == '-' || s[2] == ',' || svg_ws(s[2])))
                ok = true;
            while (*s && *s != ',')
                s++;
        }
        if (!ok)
            return false;
    }
    return true;
}

/* ---- Frames ---- */

static void child_frame(SvgCtx *x, const AuElement *el, const SvgFrame *parent, SvgFrame *f)
{
    (void)x;
    *f = *parent;
    svg_state(el, parent, parent->inst, &f->st);
}

/* The canvas rectangle a group for el needs: its viewport. */
static AuRect group_bounds(const SvgFrame *f) { return f->vp_dev; }

static void render(SvgCtx *x, AuElement *el, const SvgFrame *parent);

static void render_children(SvgCtx *x, AuElement *el, const SvgFrame *f)
{
    for (AuNode *c = el->node.first_child; c; c = c->next)
        if (c->type == AU_NODE_ELEMENT)
            render(x, au_as_element(c), f);
}

/* Sets up a new viewport: x, y, width, height in the parent's user space,
 * viewBox and preserveAspectRatio of vbel. Returns false when nothing
 * renders (a zero size or viewBox). */
static bool enter_viewport(SvgCtx *x, const AuElement *vbel, SvgFrame *f, float vx, float vy, float vw, float vh,
                           bool clip, bool *pushed)
{
    *pushed = false;
    if (!(vw > 0) || !(vh > 0))
        return false;
    float vb[4];
    bool ok;
    size_t nvb = svg_parse_numbers(svg_attr(vbel, x->n->view_box), vb, 4, &ok);
    bool has_vb = nvb == 4 && ok;
    if (has_vb && (!(vb[2] > 0) || !(vb[3] > 0)))
        return false; /* a zero or negative viewBox disables rendering */
    if (clip) {
        AuRect dr = map_rect(&f->ctm, vx, vy, vx + vw, vy + vh);
        if (mat_axis_aligned(&f->ctm) || (fabsf(f->ctm.a) < 1e-6f && fabsf(f->ctm.d) < 1e-6f)) {
            au_canvas_clip_push(x->c, dr);
            *pushed = true;
        }
        f->vp_dev = dr;
    } else {
        f->vp_dev = map_rect(&f->ctm, vx, vy, vx + vw, vy + vh);
    }
    AuMatrix vm = {1, 0, 0, 1, vx, vy};
    if (has_vb) {
        SvgPar par;
        svg_parse_par(svg_attr(vbel, x->n->par), &par);
        vm = svg_viewbox_matrix(vb, (AuRect){vx, vy, vw, vh}, par);
        f->vw = vb[2];
        f->vh = vb[3];
    } else {
        f->vw = vw;
        f->vh = vh;
    }
    au_matrix_multiply(&f->ctm, &vm);
    return true;
}

static bool overflow_clips(const AuElement *el)
{
    const AuStyle *s = au_element_style(el);
    return s && s->box->overflow_x != AU_OVERFLOW_VISIBLE;
}

/* ---- Clip paths and masks ---- */

/* Collects el's clip geometry (canvas space) into x->clip_out pieces:
 * each child is filled separately into the mask group by the caller. */
static void clip_children(SvgCtx *x, AuElement *cp, const SvgFrame *f, AuFillRule *rules, AuPath *paths, int *np,
                          int cap)
{
    for (AuNode *c = cp->node.first_child; c && *np < cap; c = c->next) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        AuElement *ce = au_as_element(c);
        const AuStyle *cs = au_element_style(ce);
        if (!cs || cs->box->display == AU_DISPLAY_NONE || cs->inh->visibility != AU_VISIBILITY_VISIBLE)
            continue;
        int k = kind_of(x, ce);
        SvgFrame cf;
        child_frame(x, ce, f, &cf);
        local_transform(x, ce, f, &cf.ctm);
        AuElement *geo = ce;
        if (k == K_USE) {
            /* use: only a direct reference to a shape or text counts. */
            const char *h = svg_href(x, ce);
            AuElement *t = h ? find_ref(x, h, strlen(h)) : NULL;
            if (!t)
                continue;
            AuMatrix tr = {1, 0, 0, 1, svg_len_attr(ce, x->n->x, f, 0, 0), svg_len_attr(ce, x->n->y, f, 1, 0)};
            au_matrix_multiply(&cf.ctm, &tr);
            local_transform(x, t, &cf, &cf.ctm);
            geo = t;
            k = kind_of(x, t);
        }
        if (k != K_SHAPE && k != K_TEXT)
            continue;
        AuPath *out = &paths[*np];
        au_path_init(out);
        if (k == K_SHAPE) {
            AuPath p;
            au_path_init(&p);
            if (shape_path(x, geo, &cf, &p)) {
                au_path_transform(&p, &cf.ctm);
                *out = p;
            } else {
                au_path_free(&p);
            }
        } else {
            AuPath p;
            au_path_init(&p);
            svg_text_outline(x, geo, &cf, &p);
            au_path_transform(&p, &cf.ctm);
            *out = p;
        }
        rules[*np] = cf.st.clip_rule == AU_FILL_RULE_EVENODD ? AU_FILL_EVENODD : AU_FILL_NONZERO;
        (*np)++;
    }
}

#define MAX_CLIP_PIECES 64

/* Builds the clip of el (whose frame is f) from clipPath cp: a rectangle
 * clip when it is one axis-aligned rectangle (*rect_pushed), otherwise
 * the paths to fill into a mask group. Returns false when the clip is
 * empty (nothing renders). */
static bool clip_prepare(SvgCtx *x, AuElement *el, AuElement *cp, const SvgFrame *f, AuPath *paths, AuFillRule *rules,
                         int *np, bool *rect_pushed)
{
    *np = 0;
    *rect_pushed = false;
    SvgFrame cf = *f;
    svg_state(cp, f, false, &cf.st);
    local_transform(x, cp, f, &cf.ctm);
    const char *units = svg_attr(cp, x->n->clip_path_units);
    if (units && !strcmp(units, "objectBoundingBox")) {
        float b[4];
        if (!element_bbox(x, el, f, b, 0))
            return false;
        AuMatrix bm = {b[2] - b[0], 0, 0, b[3] - b[1], b[0], b[1]};
        au_matrix_multiply(&cf.ctm, &bm);
    }
    clip_children(x, cp, &cf, rules, paths, np, MAX_CLIP_PIECES);
    if (*np == 0)
        return false;
    /* One axis-aligned rectangle: a plain clip. */
    if (*np == 1 && paths[0].nops == 5 && paths[0].ops[0] == AU_PATH_MOVE) {
        const float *p = paths[0].pts;
        bool rect = paths[0].ops[1] == AU_PATH_LINE && paths[0].ops[2] == AU_PATH_LINE &&
                    paths[0].ops[3] == AU_PATH_LINE && paths[0].ops[4] == AU_PATH_CLOSE &&
                    ((p[1] == p[3] && p[2] == p[4] && p[5] == p[7] && p[6] == p[0]) ||
                     (p[0] == p[2] && p[3] == p[5] && p[4] == p[6] && p[7] == p[1]));
        if (rect) {
            float x0 = fminf(fminf(p[0], p[2]), p[4]), x1 = fmaxf(fmaxf(p[0], p[2]), p[4]);
            float y0 = fminf(fminf(p[1], p[3]), p[5]), y1 = fmaxf(fmaxf(p[1], p[3]), p[5]);
            au_canvas_clip_push(x->c, (AuRect){x0, y0, x1 - x0, y1 - y0});
            *rect_pushed = true;
            au_path_free(&paths[0]);
            *np = 0;
        }
    }
    return true;
}

static void mask_render(SvgCtx *x, AuElement *el, AuElement *mk, const SvgFrame *f)
{
    const AuSvgNames *n = x->n;
    const char *mu = svg_attr(mk, n->mask_units), *cu = svg_attr(mk, n->mask_content_units);
    bool region_obb = !mu || strcmp(mu, "userSpaceOnUse") != 0;
    bool content_obb = cu && !strcmp(cu, "objectBoundingBox");
    float b[4] = {0, 0, 0, 0};
    bool have_b = (region_obb || content_obb) && element_bbox(x, el, f, b, 0);
    if ((region_obb || content_obb) && !have_b)
        return; /* an empty box: the mask hides everything */
    float rx, ry, rw, rh;
    if (region_obb) {
        float bw = b[2] - b[0], bh = b[3] - b[1];
        SvgLen l;
        rx = svg_parse_length(svg_attr(mk, n->x), &l) ? (l.unit == SVG_U_PCT ? l.v / 100 : l.v) : -0.1f;
        ry = svg_parse_length(svg_attr(mk, n->y), &l) ? (l.unit == SVG_U_PCT ? l.v / 100 : l.v) : -0.1f;
        rw = svg_parse_length(svg_attr(mk, n->width), &l) ? (l.unit == SVG_U_PCT ? l.v / 100 : l.v) : 1.2f;
        rh = svg_parse_length(svg_attr(mk, n->height), &l) ? (l.unit == SVG_U_PCT ? l.v / 100 : l.v) : 1.2f;
        rx = b[0] + rx * bw, ry = b[1] + ry * bh, rw *= bw, rh *= bh;
    } else {
        SvgFrame tf = *f;
        rx = svg_len_attr(mk, n->x, &tf, 0, -0.1f * f->vw);
        ry = svg_len_attr(mk, n->y, &tf, 1, -0.1f * f->vh);
        rw = svg_len_attr(mk, n->width, &tf, 0, 1.2f * f->vw);
        rh = svg_len_attr(mk, n->height, &tf, 1, 1.2f * f->vh);
    }
    if (!(rw > 0 && rh > 0))
        return;
    au_canvas_group_begin(x->c, group_bounds(f));
    bool pushed = false;
    if (mat_axis_aligned(&f->ctm)) {
        au_canvas_clip_push(x->c, map_rect(&f->ctm, rx, ry, rx + rw, ry + rh));
        pushed = true;
    }
    SvgFrame mf = *f;
    mf.inst = false;
    svg_state(mk, f, false, &mf.st);
    if (content_obb) {
        AuMatrix bm = {b[2] - b[0], 0, 0, b[3] - b[1], b[0], b[1]};
        au_matrix_multiply(&mf.ctm, &bm);
    }
    render_children(x, mk, &mf);
    if (pushed)
        au_canvas_clip_pop(x->c);
    const AuStyle *ms = au_element_style(mk);
    au_canvas_mask_end(x->c, !(ms && ms->rare->mask_type == AU_MASK_TYPE_ALPHA));
}

/* ---- Images ---- */

static void image_draw(SvgCtx *x, AuElement *el, const char *href, float ix, float iy, float w, float h,
                       const char *par_attr, const SvgFrame *f, float opacity);

static void image_render(SvgCtx *x, AuElement *el, const SvgFrame *f, float opacity)
{
    const AuSvgNames *n = x->n;
    if (f->st.visibility != AU_VISIBILITY_VISIBLE)
        return;
    float ix = svg_len_attr(el, n->x, f, 0, 0), iy = svg_len_attr(el, n->y, f, 1, 0);
    float w = svg_len_attr(el, n->width, f, 0, -1), h = svg_len_attr(el, n->height, f, 1, -1);
    image_draw(x, el, svg_href(x, el), ix, iy, w, h, svg_attr(el, n->par), f, opacity);
}

/* A foreignObject draws only an HTML <img> in it, the first in its
 * subtree (in document order, at most 64 nodes looked at), stretched
 * over the object's box as object-fit: fill draws it. Sites put an
 * avatar or icon there to mask it: Discord's avatars and server icons
 * are <foreignObject mask="url(#...)"><div><img></div></foreignObject>.
 * Other HTML in a foreignObject is laid out by layout and painted through
 * AuSvgEnv.foreign. */
AuElement *au_svg_foreign_img(AuElement *fo)
{
    AuNode *root = &fo->node, *c = root->first_child;
    for (int seen = 0; c && seen < 64; seen++) {
        if (c->type == AU_NODE_ELEMENT) {
            AuElement *e = au_as_element(c);
            if (e->ns == AU_NS_HTML && e->tag == AU_ATOM_img)
                return e;
            if (c->first_child) {
                c = c->first_child;
                continue;
            }
        }
        while (c != root && !c->next)
            c = c->parent;
        if (c == root)
            break;
        c = c->next;
    }
    return NULL;
}

static void foreign_render(SvgCtx *x, AuElement *el, const SvgFrame *f, float opacity)
{
    const AuSvgNames *n = x->n;
    if (f->st.visibility != AU_VISIBILITY_VISIBLE)
        return;
    AuElement *img = au_svg_foreign_img(el);
    const AuStyle *is = img ? au_element_style(img) : NULL;
    if (img && is && (is->box->display == AU_DISPLAY_NONE || is->inh->visibility != AU_VISIBILITY_VISIBLE))
        return;
    float ix = svg_len_attr(el, n->x, f, 0, 0), iy = svg_len_attr(el, n->y, f, 1, 0);
    float w = svg_len_attr(el, n->width, f, 0, 0), h = svg_len_attr(el, n->height, f, 1, 0);
    if (!(w > 0 && h > 0))
        return;
    if (img) {
        image_draw(x, el, svg_attr(img, AU_ATOM_src), ix, iy, w, h, "none", f, opacity);
        return;
    }
    if (!x->env || !x->env->foreign)
        return;
    /* The HTML content, laid out by layout at w x h CSS pixels: drawn in
     * place when the user space only moves it, else into a layer drawn
     * through the user space's transform. */
    AuMatrix full = f->ctm, tr = {1, 0, 0, 1, ix, iy};
    au_matrix_multiply(&full, &tr);
    bool moved = fabsf(full.a - 1) < 1e-4f && fabsf(full.d - 1) < 1e-4f && fabsf(full.b) < 1e-4f &&
                 fabsf(full.c) < 1e-4f;
    if (moved) {
        AuRect box = {full.e, full.f, w, h};
        bool group = opacity < 1;
        if (group)
            au_canvas_group_begin(x->c, box);
        au_canvas_clip_push(x->c, box);
        x->env->foreign(x->env->ctx, el, x->c, box);
        au_canvas_clip_pop(x->c);
        if (group)
            au_canvas_group_end(x->c, opacity);
        return;
    }
    if (au_canvas_layer_begin(x->c, (AuRect){0, 0, w, h})) {
        au_canvas_clip_push(x->c, (AuRect){0, 0, w, h});
        x->env->foreign(x->env->ctx, el, x->c, (AuRect){0, 0, w, h});
        au_canvas_clip_pop(x->c);
        au_canvas_layer_end(x->c, &full, opacity);
    }
}

bool au_svg_foreign_size(const AuSvgNames *n, const AuElement *fo, float *w, float *h)
{
    const AuStyle *s = au_element_style(fo);
    float em = s && s->inh->font_size > 0 ? s->inh->font_size : 16;
    /* The percentage bases: the nearest ancestor svg's viewBox, else its
     * own absolute width and height. */
    float rw = 0, rh = 0;
    for (const AuNode *p = fo->node.parent; p && p->type == AU_NODE_ELEMENT; p = p->parent) {
        const AuElement *e = (const AuElement *)p;
        if (!svg_is_tag(e, n->svg))
            continue;
        float vb[4];
        bool ok;
        if (svg_parse_numbers(svg_attr(e, n->view_box), vb, 4, &ok) == 4 && ok && vb[2] > 0 && vb[3] > 0) {
            rw = vb[2], rh = vb[3];
        } else {
            AuSvgIntrinsic in;
            au_svg_intrinsic(n, e, em, &in);
            rw = in.width, rh = in.height;
        }
        break;
    }
    SvgLen l;
    *w = svg_parse_length(svg_attr(fo, n->width), &l) ? svg_len_px(l, em, rw) : 0;
    *h = svg_parse_length(svg_attr(fo, n->height), &l) ? svg_len_px(l, em, rh) : 0;
    return *w > 0 && *h > 0 && *w < 1e5f && *h < 1e5f;
}

bool au_svg_foreign_place(const AuSvgNames *n, AuElement *svg, AuRect view, const AuElement *fo, AuMatrix *m,
                          float *w, float *h)
{
    /* The elements from svg down to fo, then the frames the renderer
     * makes on its way to it (render, K_SVG, foreign_render), without
     * drawing: nothing here touches the canvas. */
    enum { CHAIN = 32 };
    const AuElement *chain[CHAIN];
    int nc = 0;
    const AuNode *p = &fo->node;
    for (; p && p != &svg->node; p = p->parent) {
        if (p->type != AU_NODE_ELEMENT || nc == CHAIN)
            return false;
        chain[nc++] = (const AuElement *)p;
    }
    const AuStyle *s = au_element_style(svg);
    if (!p || !s || !svg_is_tag(svg, n->svg) || !(view.w > 0) || !(view.h > 0) || s->box->display == AU_DISPLAY_NONE)
        return false;
    SvgCtx x;
    memset(&x, 0, sizeof x);
    x.n = n;
    x.root = svg;
    SvgFrame f;
    memset(&f, 0, sizeof f);
    f.ctm = (AuMatrix){1, 0, 0, 1, 0, 0};
    f.vw = view.w;
    f.vh = view.h;
    f.vp_dev = view;
    svg_state(svg, NULL, false, &f.st);
    bool pushed;
    if (!enter_viewport(&x, svg, &f, view.x, view.y, view.w, view.h, false, &pushed))
        return false;
    while (nc--) {
        const AuElement *el = chain[nc];
        const AuStyle *es = au_element_style(el);
        if (!es || es->box->display == AU_DISPLAY_NONE || el->ns != AU_NS_SVG)
            return false;
        SvgFrame cf;
        child_frame(&x, el, &f, &cf);
        local_transform(&x, el, &f, &cf.ctm);
        if (el != fo && svg_is_tag(el, n->svg)) {
            float vx = svg_len_attr(el, n->x, &cf, 0, 0), vy = svg_len_attr(el, n->y, &cf, 1, 0);
            float vw = svg_len_attr(el, n->width, &cf, 0, cf.vw), vh = svg_len_attr(el, n->height, &cf, 1, cf.vh);
            if (!enter_viewport(&x, el, &cf, vx, vy, vw, vh, false, &pushed))
                return false;
        }
        f = cf;
    }
    float ix = svg_len_attr(fo, n->x, &f, 0, 0), iy = svg_len_attr(fo, n->y, &f, 1, 0);
    *w = svg_len_attr(fo, n->width, &f, 0, 0);
    *h = svg_len_attr(fo, n->height, &f, 1, 0);
    if (!(*w > 0 && *h > 0))
        return false;
    AuMatrix tr = {1, 0, 0, 1, ix, iy};
    *m = f.ctm;
    au_matrix_multiply(m, &tr);
    return true;
}

static void image_draw(SvgCtx *x, AuElement *el, const char *href, float ix, float iy, float w, float h,
                       const char *par_attr, const SvgFrame *f, float opacity)
{
    if (!href || !x->env || !x->env->image || w == 0 || h == 0)
        return;
    float scale = mat_scale(&f->ctm);
    int dw = au_f2px(ceilf((w > 0 ? w : 0) * scale)), dh = au_f2px(ceilf((h > 0 ? h : 0) * scale));
    const AuSurface *img = x->env->image(x->env->ctx, el, href, dw, dh);
    if (!img || img->width <= 0 || img->height <= 0)
        return;
    /* auto sizes: the image's own size (its pixels at the asked scale). */
    float iw = (float)img->width, ih = (float)img->height;
    if (w < 0 && h < 0)
        w = iw / (scale > 0 ? scale : 1), h = ih / (scale > 0 ? scale : 1);
    else if (w < 0)
        w = h * iw / ih;
    else if (h < 0)
        h = w * ih / iw;
    SvgPar par;
    svg_parse_par(par_attr, &par);
    float vb[4] = {0, 0, iw, ih};
    AuMatrix m = svg_viewbox_matrix(vb, (AuRect){ix, iy, w, h}, par);
    AuMatrix full = f->ctm;
    au_matrix_multiply(&full, &m);
    au_canvas_clip_push(x->c, map_rect(&f->ctm, ix, iy, ix + w, iy + h));
    if (mat_axis_aligned(&full)) {
        AuRect dst = map_rect(&full, 0, 0, iw, ih);
        au_draw_image(x->c, img, (AuIRect){0, 0, img->width, img->height}, dst, AU_FILTER_BILINEAR, opacity);
    } else {
        float tx, ty;
        au_canvas_get_translation(x->c, &tx, &ty);
        (void)tx;
        (void)ty;
        if (au_canvas_layer_begin(x->c, (AuRect){0, 0, iw, ih})) {
            au_draw_image(x->c, img, (AuIRect){0, 0, img->width, img->height}, (AuRect){0, 0, iw, ih},
                          AU_FILTER_BILINEAR, 1);
            au_canvas_layer_end(x->c, &full, opacity);
        }
    }
    au_canvas_clip_pop(x->c);
}

/* ---- Elements ---- */

static AuElement *mask_ref(SvgCtx *x, const AuStyle *s)
{
    const AuCssBlob *mi = s->rare->mask_image;
    if (!mi)
        return NULL;
    uint32_t n;
    const AuCssValue *it = items(&mi->value, &n);
    if (n < 1 || it[0].type != AU_CSSV_URL)
        return NULL;
    AuElement *m = find_ref(x, it[0].u.str, it[0].n);
    /* A mask with no computed style (one the cascade has not reached)
     * cannot be drawn: the element is drawn unmasked. */
    return m && svg_is_tag(m, x->n->mask) && au_element_style(m) ? m : NULL;
}

static AuElement *clip_ref(SvgCtx *x, const AuStyle *s)
{
    const AuCssBlob *cp = s->rare->clip_path;
    if (!cp || cp->value.type != AU_CSSV_URL)
        return NULL;
    AuElement *c = find_ref(x, cp->value.u.str, cp->value.n);
    return c && svg_is_tag(c, x->n->clip_path) ? c : NULL;
}

static void render_use(SvgCtx *x, AuElement *el, SvgFrame *f, float opacity);

static void render_content(SvgCtx *x, AuElement *el, int kind, SvgFrame *f, float opacity)
{
    switch (kind) {
    case K_CONTAINER:
        render_children(x, el, f);
        break;
    case K_SWITCH:
        for (AuNode *c = el->node.first_child; c; c = c->next) {
            if (c->type != AU_NODE_ELEMENT)
                continue;
            AuElement *ce = au_as_element(c);
            if (svg_is_tag(ce, x->n->foreign_object) || !conditions_pass(x, ce))
                continue;
            render(x, ce, f);
            break;
        }
        break;
    case K_SHAPE: {
        AuPath p;
        au_path_init(&p);
        float b[4];
        if (shape_path(x, el, f, &p) && path_bbox(&p, b))
            svg_paint_geometry(x, el, f, &p, b, opacity, false);
        au_path_free(&p);
        break;
    }
    case K_TEXT:
        svg_text_paint(x, el, f, opacity);
        break;
    case K_IMAGE:
        image_render(x, el, f, opacity);
        break;
    case K_FOREIGN:
        foreign_render(x, el, f, opacity);
        break;
    case K_USE:
        render_use(x, el, f, opacity);
        break;
    case K_SVG: {
        /* A nested svg: a new viewport. */
        float vx = svg_len_attr(el, x->n->x, f, 0, 0), vy = svg_len_attr(el, x->n->y, f, 1, 0);
        float vw = svg_len_attr(el, x->n->width, f, 0, f->vw), vh = svg_len_attr(el, x->n->height, f, 1, f->vh);
        bool pushed;
        if (enter_viewport(x, el, f, vx, vy, vw, vh, overflow_clips(el), &pushed)) {
            render_children(x, el, f);
            if (pushed)
                au_canvas_clip_pop(x->c);
        }
        break;
    }
    default:
        break;
    }
}

static void render(SvgCtx *x, AuElement *el, const SvgFrame *parent)
{
    if (x->depth >= SVG_MAX_DEPTH || ++x->painted > SVG_MAX_ELEMENTS)
        return;
    const AuStyle *s = au_element_style(el);
    if (!s || s->box->display == AU_DISPLAY_NONE)
        return;
    int kind = kind_of(x, el);
    if (kind == K_NONE || kind == K_SKIP || !conditions_pass(x, el))
        return;
    SvgFrame f;
    child_frame(x, el, parent, &f);
    local_transform(x, el, parent, &f.ctm);
    float opacity = s->rare->opacity;
    if (!(opacity > 0))
        return;
    AuElement *mk = mask_ref(x, s), *cp = clip_ref(x, s);
    /* Opacity folds into a lone fill or stroke; otherwise a group. */
    bool fold = opacity < 1 && !mk && kind == K_SHAPE;
    if (fold) {
        bool hasf = f.st.fill && !(f.st.fill->value.type == AU_CSSV_KEYWORD);
        bool hass = f.st.stroke && css_len_user(&f.st.sw, &f) > 0;
        fold = !(hasf && hass);
    }
    if ((kind == K_IMAGE || kind == K_FOREIGN) && !mk)
        fold = true; /* an image draws with its opacity */
    /* Clip pieces live on the heap: this function recurses (stacks are
     * small on Mac OS 9). */
    AuPath *clip_paths = NULL;
    AuFillRule *clip_rules = NULL;
    int nclip = 0;
    bool clip_rect = false;
    if (cp) {
        clip_paths = au_alloc(MAX_CLIP_PIECES * sizeof *clip_paths);
        clip_rules = au_alloc(MAX_CLIP_PIECES * sizeof *clip_rules);
        if (!clip_prepare(x, el, cp, &f, clip_paths, clip_rules, &nclip, &clip_rect)) {
            au_free(clip_paths);
            au_free(clip_rules);
            return; /* an empty clip path hides the element */
        }
    }
    bool group = (opacity < 1 && !fold) || mk || nclip > 0;
    x->depth++;
    if (group)
        au_canvas_group_begin(x->c, group_bounds(&f));
    render_content(x, el, kind, &f, group ? 1 : opacity);
    if (nclip > 0) {
        au_canvas_group_begin(x->c, group_bounds(&f));
        for (int i = 0; i < nclip; i++)
            au_fill_path(x->c, &clip_paths[i], clip_rules[i], AU_RGB(0, 0, 0));
        au_canvas_mask_end(x->c, false);
    }
    for (int i = 0; i < nclip; i++)
        au_path_free(&clip_paths[i]);
    au_free(clip_paths);
    au_free(clip_rules);
    if (mk)
        mask_render(x, el, mk, &f);
    if (group)
        au_canvas_group_end(x->c, opacity < 1 && !fold ? opacity : 1);
    if (clip_rect)
        au_canvas_clip_pop(x->c);
    x->depth--;
}

static void render_use(SvgCtx *x, AuElement *el, SvgFrame *f, float opacity)
{
    (void)opacity;
    const char *h = svg_href(x, el);
    AuElement *t = h ? find_ref(x, h, strlen(h)) : NULL;
    if (!t || x->use_depth >= SVG_MAX_USE_DEPTH)
        return;
    /* No cycles: the target may not contain this use, nor be an element
     * already being instantiated. */
    for (AuNode *p = &el->node; p; p = p->parent)
        if (p == &t->node)
            return;
    for (int i = 0; i < x->use_depth; i++)
        if (x->use_stack[i] == t)
            return;
    float ux = svg_len_attr(el, x->n->x, f, 0, 0), uy = svg_len_attr(el, x->n->y, f, 1, 0);
    AuMatrix tr = {1, 0, 0, 1, ux, uy};
    SvgFrame uf = *f;
    au_matrix_multiply(&uf.ctm, &tr);
    uf.inst = true;
    x->use_stack[x->use_depth++] = t;
    const AuStyle *ts = au_element_style(t);
    if (ts && ts->box->display != AU_DISPLAY_NONE &&
        (svg_is_tag(t, x->n->symbol) || svg_is_tag(t, x->n->svg))) {
        /* A symbol or svg: a viewport sized by the use (100% by default),
         * or by the svg's own width and height. */
        SvgFrame sf;
        child_frame(x, t, &uf, &sf);
        sf.inst = true;
        bool is_svg = svg_is_tag(t, x->n->svg);
        float vw = svg_len_attr(el, x->n->width, f, 0, -1), vh = svg_len_attr(el, x->n->height, f, 1, -1);
        if (vw < 0)
            vw = is_svg ? svg_len_attr(t, x->n->width, f, 0, f->vw) : f->vw;
        if (vh < 0)
            vh = is_svg ? svg_len_attr(t, x->n->height, f, 1, f->vh) : f->vh;
        float ox = is_svg ? svg_len_attr(t, x->n->x, f, 0, 0) : 0, oy = is_svg ? svg_len_attr(t, x->n->y, f, 1, 0) : 0;
        local_transform(x, t, &uf, &sf.ctm);
        float op = ts->rare->opacity;
        bool group = op < 1;
        bool pushed;
        if (group)
            au_canvas_group_begin(x->c, group_bounds(&sf));
        if (op > 0 && enter_viewport(x, t, &sf, ox, oy, vw, vh, overflow_clips(t), &pushed)) {
            x->depth++;
            render_children(x, t, &sf);
            x->depth--;
            if (pushed)
                au_canvas_clip_pop(x->c);
        }
        if (group)
            au_canvas_group_end(x->c, op);
    } else {
        render(x, t, &uf);
    }
    x->use_depth--;
}

/* ---- Entry points ---- */

void au_svg_intrinsic(const AuSvgNames *n, const AuElement *svg, float em, AuSvgIntrinsic *out)
{
    memset(out, 0, sizeof *out);
    SvgLen w, h;
    bool hw = svg_parse_length(svg_attr(svg, n->width), &w) && w.unit != SVG_U_PCT;
    bool hh = svg_parse_length(svg_attr(svg, n->height), &h) && h.unit != SVG_U_PCT;
    if (hw)
        out->width = fmaxf(0, svg_len_px(w, em, 0));
    if (hh)
        out->height = fmaxf(0, svg_len_px(h, em, 0));
    if (hw && hh && out->width > 0 && out->height > 0) {
        out->ratio = out->width / out->height;
        return;
    }
    float vb[4];
    bool ok;
    if (svg_parse_numbers(svg_attr(svg, n->view_box), vb, 4, &ok) == 4 && ok && vb[2] > 0 && vb[3] > 0)
        out->ratio = vb[2] / vb[3];
}

void au_svg_paint(AuCanvas *c, const AuSvgNames *n, AuElement *svg, AuRect view, const AuSvgEnv *env)
{
    const AuStyle *s = au_element_style(svg);
    if (!s || !svg_is_tag(svg, n->svg) || !(view.w > 0) || !(view.h > 0) || s->box->display == AU_DISPLAY_NONE)
        return;
    SvgCtx x;
    memset(&x, 0, sizeof x);
    x.c = c;
    x.env = env;
    x.n = n;
    x.root = svg;
    if (!env || !env->find_id) {
        /* Index the whole tree the svg is in when it is a document of its
         * own (SVG images); inline svg indexes its own subtree. */
        AuNode *top = &svg->node;
        while (top->parent && top->parent->type == AU_NODE_ELEMENT)
            top = top->parent;
        x.root = au_as_element(top);
    }
    SvgFrame f;
    memset(&f, 0, sizeof f);
    f.ctm = (AuMatrix){1, 0, 0, 1, 0, 0};
    f.vw = view.w;
    f.vh = view.h;
    f.vp_dev = view;
    svg_state(svg, NULL, false, &f.st);
    /* The outer svg's CSS transform is the box's (layout's) business. */
    float op = s->rare->opacity;
    bool pushed = false;
    if (!(op > 0))
        return;
    AuElement *cp = clip_ref(&x, s), *mk = mask_ref(&x, s);
    bool group = op < 1 || mk || cp;
    if (group)
        au_canvas_group_begin(c, view);
    if (enter_viewport(&x, svg, &f, view.x, view.y, view.w, view.h, overflow_clips(svg), &pushed)) {
        x.depth = 1;
        render_children(&x, svg, &f);
        if (pushed)
            au_canvas_clip_pop(c);
    }
    if (group)
        au_canvas_group_end(c, op < 1 ? op : 1);
    au_svg_id_index_free(x.ids);
}
