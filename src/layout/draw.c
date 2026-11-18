/* Stacking contexts and painting.
 *
 * Painting follows CSS 2.1 Appendix E. Each stacking context keeps the
 * list of its layered descendants (positioned boxes and nested stacking
 * contexts) sorted by z-index (l_build_stacking). Painting a stacking
 * context draws its own background and borders, the negative layers, then
 * three tree walks over its non-layered descendants (block backgrounds,
 * floats, inline content with atomic inlines), then the z-index 0/auto and
 * positive layers. Floats and atomic inlines are painted atomically as if
 * they were stacking contexts. Subtrees whose overflow rectangle misses
 * the dirty rectangle are skipped, and only the line boxes that intersect
 * it are visited, so scrolling a long page paints little.
 *
 * Coordinates here are canvas coordinates: every box origin is computed
 * explicitly (the canvas translation is set once), with inner scroll
 * offsets and positioned-fixed adjustments applied on the way. A
 * translation moves the origin; a 2D transform with rotation, scale or
 * skew paints the stacking context into a canvas layer that is drawn
 * through the matrix (3D functions other than their 2D part are
 * ignored). Clips are rectangles; rounded overflow clipping masks a
 * group with the rounded padding box. */
#include "layout_int.h"

#include <stdlib.h>

/* ---- Stacking contexts ---- */

static bool creates_stacking(const LBox *b, bool flex_item)
{
    const AuStyle *s = b->style;
    if (lb_anon(b))
        return false;
    bool positioned = b->flags & LF_POSITIONED;
    int32_t z = s->box->z_index;
    if (positioned && z != AU_INT_AUTO)
        return true;
    if (b->flags & (LF_FIXED | LF_STICKY))
        return true;
    if (flex_item && z != AU_INT_AUTO)
        return true;
    const AuStyleRare *r = s->rare;
    if (r->opacity < 1 || r->transform || r->translate || r->rotate || r->scale || r->isolation == AU_ISOLATION_ISOLATE ||
        r->filter || r->mix_blend_mode != AU_MIX_BLEND_MODE_NORMAL || r->clip_path || (r->contain & AU_CONTAIN_PAINT) ||
        au_style_mask_layer_count(s) > 0)
        return true;
    return false;
}

typedef struct SCBuild {
    LBox *sc;
} SCBuild;

static void layers_add(LBox *sc, LBox *b)
{
    LBlock *k = (LBlock *)sc;
    if (!k->layers)
        k->layers = au_calloc(sizeof *k->layers);
    LLayers *ls = k->layers;
    if ((ls->n & (ls->n - 1)) == 0 || ls->n == 0) {
        uint32_t cap = ls->n ? ls->n * 2 : 4;
        ls->items = au_realloc(ls->items, cap * sizeof *ls->items);
    }
    ls->items[ls->n++] = b;
}

static int32_t zof(const LBox *b)
{
    if (lb_anon(b))
        return 0;
    int32_t z = b->style->box->z_index;
    return z == AU_INT_AUTO ? 0 : z;
}

/* Whether a is painted after b in a stacking context's list: the top
 * layer after every z-index, in the order of the document's top layer
 * (the element added last on top), each element just above its
 * ::backdrop (which is a child of the element's box, so comes later in
 * tree order); otherwise by z-index. */
static bool layer_after(const LBox *a, const LBox *b)
{
    bool ta = l_top_layer(a), tb = l_top_layer(b);
    if (ta != tb)
        return ta;
    if (ta) {
        if (a->node == b->node)
            return a->pseudo == AU_CSS_PSEUDO_NONE && b->pseudo == AU_CSS_PSEUDO_BACKDROP;
        return au_top_layer_index((const AuElement *)a->node) > au_top_layer_index((const AuElement *)b->node);
    }
    return zof(a) > zof(b);
}

static void sort_layers(LLayers *ls)
{
    /* Stable: insertion sort by z (lists are short; tree order kept). */
    for (uint32_t i = 1; i < ls->n; i++) {
        LBox *x = ls->items[i];
        uint32_t j = i;
        while (j > 0 && layer_after(ls->items[j - 1], x)) {
            ls->items[j] = ls->items[j - 1];
            j--;
        }
        ls->items[j] = x;
    }
    ls->nneg = ls->nzero = 0;
    for (uint32_t i = 0; i < ls->n; i++) {
        if (l_top_layer(ls->items[i]))
            continue; /* painted with the positive layers, after them */
        int32_t z = zof(ls->items[i]);
        if (z < 0)
            ls->nneg++;
        else if (z == 0)
            ls->nzero++;
    }
}

void l_build_stacking(AuLayout *L)
{
    LBox *root = L->root;
    L->nview_boxes = 0;
    if (!root)
        return;
    /* Clear old lists and flags. */
    for (LBox *b = root; b;) {
        if (b->kind == LK_BLOCK) {
            LBlock *k = (LBlock *)b;
            if (k->layers) {
                au_free(k->layers->items);
                au_free(k->layers);
                k->layers = NULL;
            }
        }
        b->flags &= ~(LF_LAYERED | LF_STACKING);
        if (b->first && b->kind != LK_TEXT)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
    root->flags |= LF_STACKING;
    LBox **stack = au_alloc(64 * sizeof *stack);
    uint32_t sp = 0, cap = 64;
    stack[sp++] = root; /* SC for the children of the current box */
    LBox *b = root->first;
    if (!b) {
        au_free(stack);
        return;
    }
    for (;;) {
        LBox *sc = stack[sp - 1];
        bool flex_item = (b->flags & LF_FLEX_ITEM) != 0;
        bool layered = false;
        if (l_top_layer(b)) {
            /* A stacking context of the root's, whatever its ancestors. */
            sc = root;
            b->flags |= LF_STACKING;
        }
        if (b->kind == LK_BLOCK || b->kind == LK_INLINE) {
            if (!lb_anon(b) && ((b->flags & LF_POSITIONED) || creates_stacking(b, flex_item))) {
                layered = true;
                if (b->kind == LK_BLOCK && creates_stacking(b, flex_item))
                    b->flags |= LF_STACKING;
            }
        }
        if (layered) {
            b->flags |= LF_LAYERED;
            layers_add(sc, b);
            if ((b->flags & LF_STICKY) || (b->kind == LK_BLOCK && (b->flags & LF_FIXED) && !((LBlock *)b)->cb)) {
                if (L->nview_boxes < L_VIEW_BOXES)
                    L->view_boxes[L->nview_boxes] = b;
                L->nview_boxes++;
            }
        }
        bool descend = b->first && b->kind != LK_TEXT && !(b->flags & LF_TOO_DEEP);
        if (descend) {
            if (sp == cap) {
                cap *= 2;
                stack = au_realloc(stack, cap * sizeof *stack);
            }
            stack[sp++] = (b->flags & LF_STACKING) ? b : sc;
            b = b->first;
            continue;
        }
        while (b && !b->next) {
            b = b->parent;
            sp--;
            if (!b || b == root || sp == 0) {
                b = NULL;
                break;
            }
        }
        if (!b)
            break;
        b = b->next;
    }
    au_free(stack);
    for (b = root; b;) {
        if (b->kind == LK_BLOCK && ((LBlock *)b)->layers)
            sort_layers(((LBlock *)b)->layers);
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

/* ---- Paint context ---- */

typedef struct PCtx {
    AuLayout *L;
    AuCanvas *c;
    LArea dirty;   /* canvas coordinates */
    float view_x, view_y; /* canvas position of the viewport's top-left */
    float scroll_x, scroll_y;
    int depth;
    const LBlock *canvas_box; /* its background was painted on the canvas */
} PCtx;

enum { PH_BG, PH_FLOAT, PH_INLINE };

/* Whether a box's ink may reach the dirty area. One pixel of slack above
 * and below: text fragments are as tall as the face's ascent and descent
 * rounded to whole pixels (l_font), and glyphs reach up to half a pixel
 * past them. */
static bool rect_hits(const LArea *d, float x, float y, float w, float h)
{
    return x < d->x + d->w && x + w > d->x && y - 1 < d->y + d->h && y + h + 1 > d->y;
}

static uint32_t pc(const AuStyle *s, uint32_t css)
{
    return l_paint_color(au_style_color(s, css));
}

/* ---- Transforms ---- */

/* m = m * n (n applies first to points). */
static void mat_mul(AuMatrix *m, AuMatrix n)
{
    AuMatrix r = {m->a * n.a + m->c * n.b,       m->b * n.a + m->d * n.b,       m->a * n.c + m->c * n.d,
                  m->b * n.c + m->d * n.d,       m->a * n.e + m->c * n.f + m->e, m->b * n.e + m->d * n.f + m->f};
    *m = r;
}

static float deg_rad(float deg)
{
    return deg * 3.14159265f / 180;
}

/* A length or percentage of basis; an angle in degrees; a number. */
static float tf_arg(const AuCssValue *v, float basis)
{
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num * basis / 100;
    if (v->type == AU_CSSV_DIMENSION || v->type == AU_CSSV_NUMBER)
        return v->u.num;
    return 0;
}

/* The matrix of one transform function over a w x h box. */
static AuMatrix tf_function(const AuCssValue *f, float w, float h)
{
    AuMatrix m = {1, 0, 0, 1, 0, 0};
    const AuCssValue *a = f->u.items;
    uint32_t n = f->n;
    float x = n > 0 ? tf_arg(&a[0], w) : 0;
    float y = n > 1 ? tf_arg(&a[1], h) : 0;
    switch (f->kw) {
    case AU_K_translate:
    case AU_K_translate3d: m.e = x, m.f = y; break;
    case AU_K_translatex: m.e = x; break;
    case AU_K_translatey: m.f = n > 0 ? tf_arg(&a[0], h) : 0; break;
    case AU_K_scale:
    case AU_K_scale3d:
        m.a = n > 0 ? tf_arg(&a[0], 100) : 1;
        m.d = n > 1 ? tf_arg(&a[1], 100) : m.a;
        break;
    case AU_K_scalex: m.a = n > 0 ? tf_arg(&a[0], 100) : 1; break;
    case AU_K_scaley: m.d = n > 0 ? tf_arg(&a[0], 100) : 1; break;
    case AU_K_rotate:
    case AU_K_rotatez: {
        float r = deg_rad(x);
        m.a = m.d = cosf(r);
        m.b = sinf(r);
        m.c = -m.b;
        break;
    }
    case AU_K_skew: m.c = tanf(deg_rad(x)), m.b = tanf(deg_rad(y)); break;
    case AU_K_skewx: m.c = tanf(deg_rad(x)); break;
    case AU_K_skewy: m.b = tanf(deg_rad(x)); break;
    case AU_K_matrix:
        if (n >= 6)
            m = (AuMatrix){a[0].u.num, a[1].u.num, a[2].u.num, a[3].u.num, tf_arg(&a[4], 0), tf_arg(&a[5], 0)};
        break;
    case AU_K_matrix3d:
        if (n >= 16)
            m = (AuMatrix){a[0].u.num, a[1].u.num, a[4].u.num, a[5].u.num, tf_arg(&a[12], 0), tf_arg(&a[13], 0)};
        break;
    default: break; /* 3D rotations and perspective: the identity */
    }
    return m;
}

/* One transform-origin coordinate: a length, percentage or keyword. */
static float origin_arg(const AuCssValue *v, float len)
{
    if (v->type == AU_CSSV_KEYWORD) {
        switch (v->kw) {
        case AU_K_left: case AU_K_top: return 0;
        case AU_K_right: case AU_K_bottom: return len;
        default: return len / 2;
        }
    }
    return tf_arg(v, len);
}

/* b's transform as a matrix in its border-box coordinates (CSS Transforms
 * 2: translate, rotate, scale, then transform, about transform-origin).
 * Returns false when there is none. */
bool l_transform_matrix(const LBlock *b, AuMatrix *out)
{
    *out = (AuMatrix){1, 0, 0, 1, 0, 0};
    if (lb_anon(&b->b))
        return false;
    const AuStyleRare *r = b->b.style->rare;
    if (!r->transform && !r->translate && !r->rotate && !r->scale)
        return false;
    float w = b->w, h = b->h;
    float ox = w / 2, oy = h / 2;
    if (r->transform_origin) {
        uint32_t n;
        const AuCssValue *it = l_items(&r->transform_origin->value, &n);
        /* A lone vertical keyword sets y. */
        if (n == 1 && it[0].type == AU_CSSV_KEYWORD && (it[0].kw == AU_K_top || it[0].kw == AU_K_bottom))
            oy = origin_arg(&it[0], h);
        else {
            if (n > 0)
                ox = origin_arg(&it[0], w);
            if (n > 1)
                oy = origin_arg(&it[1], h);
            /* "top left" order */
            if (n > 1 && it[0].type == AU_CSSV_KEYWORD && (it[0].kw == AU_K_top || it[0].kw == AU_K_bottom)) {
                oy = origin_arg(&it[0], h);
                ox = origin_arg(&it[1], w);
            }
        }
    }
    AuMatrix m = {1, 0, 0, 1, ox, oy};
    uint32_t n;
    if (r->translate) {
        const AuCssValue *it = l_items(&r->translate->value, &n);
        mat_mul(&m, (AuMatrix){1, 0, 0, 1, n > 0 ? tf_arg(&it[0], w) : 0, n > 1 ? tf_arg(&it[1], h) : 0});
    }
    if (r->rotate) {
        const AuCssValue *it = l_items(&r->rotate->value, &n);
        /* "angle", or an axis then the angle: only z rotations are 2D. */
        if (n > 0 && (n == 1 || (n == 2 && it[0].type == AU_CSSV_KEYWORD && it[0].kw == AU_K_z))) {
            float rad = deg_rad(tf_arg(&it[n - 1], 0));
            mat_mul(&m, (AuMatrix){cosf(rad), sinf(rad), -sinf(rad), cosf(rad), 0, 0});
        }
    }
    if (r->scale) {
        const AuCssValue *it = l_items(&r->scale->value, &n);
        if (n > 0) {
            float sx = tf_arg(&it[0], 1), sy = n > 1 ? tf_arg(&it[1], 1) : sx;
            if (it[0].type == AU_CSSV_PERCENT)
                sx = it[0].u.num / 100;
            if (n > 1 && it[1].type == AU_CSSV_PERCENT)
                sy = it[1].u.num / 100;
            mat_mul(&m, (AuMatrix){sx, 0, 0, sy, 0, 0});
        }
    }
    if (r->transform) {
        const AuCssValue *it = l_items(&r->transform->value, &n);
        for (uint32_t i = 0; i < n; i++)
            if (it[i].type == AU_CSSV_FUNCTION)
                mat_mul(&m, tf_function(&it[i], w, h));
    }
    mat_mul(&m, (AuMatrix){1, 0, 0, 1, -ox, -oy});
    *out = m;
    return true;
}

/* True when m only translates (within rounding). */
static bool mat_is_translation(const AuMatrix *m)
{
    return fabsf(m->a - 1) < 1e-5f && fabsf(m->d - 1) < 1e-5f && fabsf(m->b) < 1e-5f && fabsf(m->c) < 1e-5f;
}

/* The translation part of b's transform when it is only a translation,
 * else (0, 0): rotations and scales go through l_transform_matrix. */
void l_transform_offset(const LBlock *b, float *dx, float *dy);
void l_transform_offset(const LBlock *b, float *dx, float *dy)
{
    AuMatrix m;
    *dx = *dy = 0;
    if (l_transform_matrix(b, &m) && mat_is_translation(&m)) {
        *dx = m.e;
        *dy = m.f;
    }
}

/* ---- Borders and backgrounds ---- */

static void box_radii(const LBlock *b, AuRadii *r, float w, float h)
{
    const AuStyleSurround *s = b->b.style->surr;
    for (int i = 0; i < 4; i++) {
        r->rx[i] = l_max(0, l_len(&s->radius[i][0], w));
        r->ry[i] = l_max(0, l_len(&s->radius[i][1], h));
    }
    au_radii_fit(r, (AuRect){0, 0, w, h});
}

static void inset_radii(AuRadii *r, const float *in)
{
    /* top-left uses left and top insets, and so on */
    static const int hs[4] = {3, 1, 1, 3}, vs[4] = {0, 0, 2, 2};
    for (int i = 0; i < 4; i++) {
        r->rx[i] = l_max(0, r->rx[i] - in[hs[i]]);
        r->ry[i] = l_max(0, r->ry[i] - in[vs[i]]);
    }
}

/* The dark or light shade of a groove, ridge, inset or outset border, as
 * Blink computes it: the largest channel v (0..1) moves by 0.33 and the
 * others scale with it. Black darkens to #545454 and lightens to #a8a8a8
 * so a black bevel still shows. */
static uint32_t bevel(uint32_t argb, bool dark)
{
    uint32_t a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    if (!r && !g && !b)
        return (a << 24) | (dark ? 0x545454u : 0xa8a8a8u);
    float v = (float)(r > g ? (r > b ? r : b) : (g > b ? g : b)) / 255.0f;
    float m = dark ? l_max(0, (v - 0.33f) / v) : l_min(1, v + 0.33f) / v;
    float k = m * 255.99f / 255.0f;
    r = (uint32_t)l_min(255, r * k);
    g = (uint32_t)l_min(255, g * k);
    b = (uint32_t)l_min(255, b * k);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void dashed_side(AuCanvas *c, AuRect box, int side, float w, uint32_t col, bool dotted)
{
    float len = side == AU_SIDE_TOP || side == AU_SIDE_BOTTOM ? box.w : box.h;
    float dash = dotted ? w : 2 * w, gap = w;
    if (dash <= 0)
        return;
    int n = au_f2px((len + gap) / (dash + gap));
    if (n < 1)
        n = 1;
    if (n > 4000)
        n = 4000;
    float step = n > 1 ? (len - dash) / (n - 1) : 0;
    for (int i = 0; i < n; i++) {
        float p = i * step;
        AuRect r;
        switch (side) {
        case AU_SIDE_TOP: r = (AuRect){box.x + p, box.y, dash, w}; break;
        case AU_SIDE_BOTTOM: r = (AuRect){box.x + p, box.y + box.h - w, dash, w}; break;
        case AU_SIDE_LEFT: r = (AuRect){box.x, box.y + p, w, dash}; break;
        default: r = (AuRect){box.x + box.w - w, box.y + p, w, dash}; break;
        }
        if (dotted) {
            AuRadii rr = {{w / 2, w / 2, w / 2, w / 2}, {w / 2, w / 2, w / 2, w / 2}};
            au_fill_rounded_rect(c, r, &rr, col);
        } else
            au_fill_rect(c, r, col);
    }
}

/* One side of a rounded border for draw_rounded_dashes: its length along
 * the side, its width, and the extent of each end's corner along (e) and
 * across (d) the side. */
typedef struct {
    AuRect box;
    int side;
    float len, w, e0, d0, e1, d1;
} LDashSide;

/* Adds to p the part of the band of side s from a to b along it: as thick
 * as the corner where it lies inside one, else the border width. */
static void dash_piece(AuPath *p, const LDashSide *s, float a, float b, bool dot)
{
    float cut[4] = {a, l_max(a, l_min(b, s->e0)), l_min(b, l_max(a, s->len - s->e1)), b};
    float th[3] = {s->d0, s->w, s->d1};
    for (int k = 0; k < 3; k++) {
        float u = cut[k], v = cut[k + 1];
        if (v - u <= 0)
            continue;
        float d = th[k];
        AuRect r;
        switch (s->side) {
        case AU_SIDE_TOP: r = (AuRect){s->box.x + u, s->box.y, v - u, d}; break;
        case AU_SIDE_BOTTOM: r = (AuRect){s->box.x + u, s->box.y + s->box.h - d, v - u, d}; break;
        case AU_SIDE_LEFT: r = (AuRect){s->box.x, s->box.y + u, d, v - u}; break;
        default: r = (AuRect){s->box.x + s->box.w - d, s->box.y + u, d, v - u}; break;
        }
        if (dot && k == 1 && u == a && v == b) {
            AuRadii rr = {{s->w / 2, s->w / 2, s->w / 2, s->w / 2}, {s->w / 2, s->w / 2, s->w / 2, s->w / 2}};
            au_path_rounded_rect(p, r, &rr, false);
        } else {
            au_path_rect(p, r);
        }
    }
}

static void draw_borders(AuCanvas *c, AuRect box, const float *bw, const uint32_t *col, const uint8_t *st,
                         const AuRadii *radii);

/* Rounded borders with dashed or dotted sides: the sides are drawn solid
 * in a group, then masked to dashes spaced over each side's full length
 * as for square corners. A dash in a corner exposes the curve there. */
static void draw_rounded_dashes(AuCanvas *c, AuRect box, const float *bw, const uint32_t *col,
                                const uint8_t *st, const AuRadii *radii)
{
    uint8_t solid_st[4];
    for (int i = 0; i < 4; i++)
        solid_st[i] = (st[i] == AU_BORDER_STYLE_DASHED || st[i] == AU_BORDER_STYLE_DOTTED)
                          ? AU_BORDER_STYLE_SOLID
                          : st[i];
    au_canvas_group_begin(c, box);
    draw_borders(c, box, bw, col, solid_st, radii);
    /* Corner extents: the larger of the radius and the adjacent border. */
    float cw[4], ch[4];
    static const int hs[4] = {3, 1, 1, 3}, vs[4] = {0, 0, 2, 2};
    for (int i = 0; i < 4; i++) {
        cw[i] = l_max(radii->rx[i], bw[hs[i]]);
        ch[i] = l_max(radii->ry[i], bw[vs[i]]);
    }
    /* Corners at each side's start and end (top-left is 0, clockwise). */
    static const int c0[4] = {0, 1, 3, 0}, c1[4] = {1, 2, 2, 3};
    AuPath p;
    au_path_init(&p);
    for (int i = 0; i < 4; i++) {
        if (bw[i] <= 0)
            continue;
        bool horiz = i == AU_SIDE_TOP || i == AU_SIDE_BOTTOM;
        LDashSide s = {box, i, horiz ? box.w : box.h, bw[i], 0, 0, 0, 0};
        s.e0 = horiz ? cw[c0[i]] : ch[c0[i]];
        s.d0 = horiz ? ch[c0[i]] : cw[c0[i]];
        s.e1 = horiz ? cw[c1[i]] : ch[c1[i]];
        s.d1 = horiz ? ch[c1[i]] : cw[c1[i]];
        bool dot = st[i] == AU_BORDER_STYLE_DOTTED;
        if (!dot && st[i] != AU_BORDER_STYLE_DASHED) {
            dash_piece(&p, &s, 0, s.len, false);
            continue;
        }
        float dash = dot ? s.w : 2 * s.w, gap = s.w;
        int n = au_f2i((s.len + gap) / (dash + gap));
        n = n < 1 ? 1 : n > 4000 ? 4000 : n;
        if (n == 1)
            dash = l_min(dash, s.len);
        float step = n > 1 ? (s.len - dash) / (n - 1) : 0;
        for (int k = 0; k < n; k++)
            dash_piece(&p, &s, k * step, k * step + dash, dot);
    }
    au_canvas_group_mask(c, &p);
    au_path_free(&p);
    au_canvas_group_end(c, 1.0f);
}

/* Draws borders with widths bw, colours col (paint ARGB) and styles st. */
static void draw_borders(AuCanvas *c, AuRect box, const float *bw, const uint32_t *col, const uint8_t *st,
                         const AuRadii *radii)
{
    AuBorder solid = {{0}, {0}};
    bool any_solid = false;
    bool has_radii = radii && !au_radii_zero(radii);
    if (has_radii)
        for (int i = 0; i < 4; i++)
            if (bw[i] > 0 && (col[i] >> 24) &&
                (st[i] == AU_BORDER_STYLE_DASHED || st[i] == AU_BORDER_STYLE_DOTTED)) {
                draw_rounded_dashes(c, box, bw, col, st, radii);
                return;
            }
    for (int i = 0; i < 4; i++) {
        /* Every side keeps its width, painted or not: the corner joins
         * depend on the neighbours' widths (CSS triangles are boxes with
         * transparent sides). */
        solid.width[i] = bw[i] > 0 ? bw[i] : 0;
        if (bw[i] <= 0 || !(col[i] >> 24))
            continue;
        int s = st[i];
        if ((s == AU_BORDER_STYLE_DASHED || s == AU_BORDER_STYLE_DOTTED) && !has_radii) {
            dashed_side(c, box, i, bw[i], col[i], s == AU_BORDER_STYLE_DOTTED);
            continue;
        }
        solid.width[i] = bw[i];
        uint32_t cc = col[i];
        bool tl = i == AU_SIDE_TOP || i == AU_SIDE_LEFT;
        if (s == AU_BORDER_STYLE_INSET)
            cc = bevel(cc, tl);
        else if (s == AU_BORDER_STYLE_OUTSET)
            cc = bevel(cc, !tl);
        solid.color[i] = cc;
        any_solid = true;
    }
    if (!any_solid)
        return;
    int s0 = st[0];
    if (s0 == AU_BORDER_STYLE_DOUBLE && solid.width[0] >= 3) {
        AuBorder third = solid;
        float in[4];
        for (int i = 0; i < 4; i++) {
            third.width[i] = solid.width[i] / 3;
            in[i] = solid.width[i] * 2 / 3;
        }
        au_draw_border(c, box, &third, radii);
        AuRect inner = {box.x + in[3], box.y + in[0], box.w - in[1] - in[3], box.h - in[0] - in[2]};
        AuRadii ir;
        if (radii) {
            ir = *radii;
            inset_radii(&ir, in);
        }
        au_draw_border(c, inner, &third, radii ? &ir : NULL);
        return;
    }
    if (s0 == AU_BORDER_STYLE_GROOVE || s0 == AU_BORDER_STYLE_RIDGE) {
        AuBorder outer = solid, inner_b = solid;
        float in[4];
        bool groove = s0 == AU_BORDER_STYLE_GROOVE;
        for (int i = 0; i < 4; i++) {
            outer.width[i] = inner_b.width[i] = solid.width[i] / 2;
            in[i] = solid.width[i] / 2;
            bool tl = i == AU_SIDE_TOP || i == AU_SIDE_LEFT;
            uint32_t dark = bevel(solid.color[i], true), light = bevel(solid.color[i], false);
            outer.color[i] = (tl == groove) ? dark : light;
            inner_b.color[i] = (tl == groove) ? light : dark;
        }
        au_draw_border(c, box, &outer, radii);
        AuRect inner = {box.x + in[3], box.y + in[0], box.w - in[1] - in[3], box.h - in[0] - in[2]};
        AuRadii ir;
        if (radii) {
            ir = *radii;
            inset_radii(&ir, in);
        }
        au_draw_border(c, inner, &inner_b, radii ? &ir : NULL);
        return;
    }
    au_draw_border(c, box, &solid, radii);
}

static float stop_pos(const AuCssValue *v, float len)
{
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num / 100;
    if (v->type == AU_CSSV_DIMENSION)
        return len > 0 ? v->u.num / len : 0;
    return -1;
}

/* A length or percentage of a gradient argument against basis. */
static float grad_len(const AuCssValue *v, float basis)
{
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num * basis / 100;
    if (v->type == AU_CSSV_DIMENSION || v->type == AU_CSSV_NUMBER)
        return v->u.num;
    return 0;
}

/* The centre and radii of a radial gradient (CSS Images 3, 3.2) from its
 * [shape || size] [at position] argument over area. */
static void radial_shape(const AuCssValue *a, uint32_t n, AuRect area, AuGradient *g)
{
    bool circle = false;
    int size_kw = AU_K_farthest_corner;
    float lens[2];
    int nlen = 0, npos = 0;
    const AuCssValue *pos[2] = {NULL, NULL};
    bool at = false;
    for (uint32_t i = 0; i < n; i++) {
        const AuCssValue *v = &a[i];
        if (v->type == AU_CSSV_KEYWORD && v->kw == AU_K_at) {
            at = true;
            continue;
        }
        if (at) {
            if (npos < 2)
                pos[npos++] = v;
            continue;
        }
        if (v->type == AU_CSSV_KEYWORD) {
            if (v->kw == AU_K_circle)
                circle = true;
            else if (v->kw == AU_K_closest_side || v->kw == AU_K_closest_corner || v->kw == AU_K_farthest_side ||
                     v->kw == AU_K_farthest_corner)
                size_kw = v->kw;
        } else if (nlen < 2) {
            lens[nlen] = grad_len(v, nlen == 0 ? area.w : area.h);
            nlen++;
        }
    }
    /* Position: keywords or lengths, x then y (a lone y keyword works). */
    float cx = area.w / 2, cy = area.h / 2;
    bool xset = false;
    for (int i = 0; i < npos; i++) {
        const AuCssValue *v = pos[i];
        if (v->type == AU_CSSV_KEYWORD) {
            switch (v->kw) {
            case AU_K_left: cx = 0; xset = true; break;
            case AU_K_right: cx = area.w; xset = true; break;
            case AU_K_top: cy = 0; break;
            case AU_K_bottom: cy = area.h; break;
            default: break; /* center */
            }
        } else if (!xset) {
            cx = grad_len(v, area.w);
            xset = true;
        } else {
            cy = grad_len(v, area.h);
        }
    }
    float dl = cx, dr = area.w - cx, dt = cy, db = area.h - cy;
    if (dl < 0)
        dl = -dl;
    if (dr < 0)
        dr = -dr;
    if (dt < 0)
        dt = -dt;
    if (db < 0)
        db = -db;
    float rx, ry;
    if (nlen) {
        rx = lens[0];
        ry = circle || nlen < 2 ? lens[0] : lens[1];
    } else {
        bool closest = size_kw == AU_K_closest_side || size_kw == AU_K_closest_corner;
        float sx = closest ? l_min(dl, dr) : l_max(dl, dr);
        float sy = closest ? l_min(dt, db) : l_max(dt, db);
        bool corner = size_kw == AU_K_closest_corner || size_kw == AU_K_farthest_corner;
        if (circle) {
            rx = ry = corner ? sqrtf(sx * sx + sy * sy) : (closest ? l_min(sx, sy) : l_max(sx, sy));
        } else if (corner) {
            /* the ellipse through the corner with the sides' aspect ratio */
            rx = sx * 1.41421356f;
            ry = sy * 1.41421356f;
        } else {
            rx = sx;
            ry = sy;
        }
    }
    g->radial = true;
    g->x0 = area.x + cx;
    g->y0 = area.y + cy;
    g->rx = l_max(rx, 0.01f);
    g->ry = l_max(ry, 0.01f);
}

/* Fills r (canvas coordinates) with a linear or radial gradient value,
 * repeating or not. Conic gradients are approximated with the average of
 * their colours. */
static void fill_gradient(AuCanvas *c, const AuCssValue *fn, AuRect area, AuRect r, const AuRadii *radii,
                          const AuStyle *s)
{
    AuGradient g;
    memset(&g, 0, sizeof g);
    uint32_t first = 0;
    float angle = 180;
    bool linear = fn->kw == AU_K_linear_gradient || fn->kw == AU_K_repeating_linear_gradient ||
                  fn->kw == AU_K__webkit_linear_gradient || fn->kw == AU_K__webkit_repeating_linear_gradient;
    bool legacy = fn->kw == AU_K__webkit_linear_gradient || fn->kw == AU_K__webkit_repeating_linear_gradient;
    if (fn->n == 0)
        return;
    uint32_t n0;
    const AuCssValue *p0 = l_items(&fn->u.items[0], &n0);
    bool first_is_color = false;
    for (uint32_t i = 0; i < n0; i++)
        if (p0[i].type == AU_CSSV_COLOR)
            first_is_color = true;
    if (!first_is_color) {
        first = 1;
        if (linear) {
            if (n0 == 1 && p0[0].type == AU_CSSV_DIMENSION) {
                angle = p0[0].u.num;
                if (legacy)
                    angle = 90 - angle;
            } else {
                bool top = false, bottom = false, left = false, right = false;
                for (uint32_t i = 0; i < n0; i++) {
                    if (p0[i].type != AU_CSSV_KEYWORD)
                        continue;
                    top |= p0[i].kw == AU_K_top;
                    bottom |= p0[i].kw == AU_K_bottom;
                    left |= p0[i].kw == AU_K_left;
                    right |= p0[i].kw == AU_K_right;
                }
                if (legacy) {
                    /* -webkit-linear-gradient(top, ...) starts at the top */
                    bool t = top, b2 = bottom, l2 = left, r2 = right;
                    top = b2;
                    bottom = t;
                    left = r2;
                    right = l2;
                }
                if (top && left) angle = 315;
                else if (top && right) angle = 45;
                else if (bottom && left) angle = 225;
                else if (bottom && right) angle = 135;
                else if (top) angle = 0;
                else if (right) angle = 90;
                else if (left) angle = 270;
                else angle = 180;
            }
        }
    }
    bool radial = fn->kw == AU_K_radial_gradient || fn->kw == AU_K_repeating_radial_gradient ||
                  fn->kw == AU_K__webkit_radial_gradient || fn->kw == AU_K__webkit_repeating_radial_gradient;
    bool repeating = fn->kw == AU_K_repeating_linear_gradient || fn->kw == AU_K_repeating_radial_gradient ||
                     fn->kw == AU_K__webkit_repeating_linear_gradient ||
                     fn->kw == AU_K__webkit_repeating_radial_gradient;
    float glen;
    if (radial) {
        radial_shape(first ? p0 : NULL, first ? n0 : 0, area, &g);
        glen = g.rx;
    } else {
        au_gradient_css_line(&g, area, angle);
        glen = sqrtf((g.x1 - g.x0) * (g.x1 - g.x0) + (g.y1 - g.y0) * (g.y1 - g.y0));
    }
    /* Stops: positions where given, the rest spread evenly. */
    float pos[AU_GRADIENT_MAX_STOPS];
    uint32_t col[AU_GRADIENT_MAX_STOPS];
    int ns = 0;
    for (uint32_t i = first; i < fn->n && ns < AU_GRADIENT_MAX_STOPS; i++) {
        uint32_t n;
        const AuCssValue *p = l_items(&fn->u.items[i], &n);
        int ci = -1;
        float ps[2] = {-1, -1};
        int np = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (p[k].type == AU_CSSV_COLOR)
                ci = (int)k;
            else if (np < 2)
                ps[np++] = stop_pos(&p[k], glen);
        }
        if (ci < 0)
            continue; /* colour hint: ignored */
        uint32_t cc = pc(s, p[ci].u.rgba);
        pos[ns] = ps[0];
        col[ns++] = cc;
        if (np == 2 && ns < AU_GRADIENT_MAX_STOPS) {
            pos[ns] = ps[1];
            col[ns++] = cc;
        }
    }
    if (ns == 0)
        return;
    if (pos[0] < 0)
        pos[0] = 0;
    if (pos[ns - 1] < 0)
        pos[ns - 1] = 1;
    for (int i = 1; i < ns; i++)
        if (pos[i] >= 0 && pos[i] < pos[i - 1])
            pos[i] = pos[i - 1];
    for (int i = 1; i < ns - 1; i++) {
        if (pos[i] >= 0)
            continue;
        int j = i;
        while (pos[j] < 0)
            j++;
        for (int k = i; k < j; k++)
            pos[k] = pos[i - 1] + (pos[j] - pos[i - 1]) * (k - i + 1) / (j - i + 1);
        i = j;
    }
    if (repeating && (linear || radial)) {
        /* One period, from the first stop to the last, repeats. */
        float p0s = radial ? 0 : pos[0], period = pos[ns - 1] - p0s;
        if (period > 1e-4f) {
            if (radial) {
                g.rx *= period;
                g.ry *= period;
            } else {
                float vx = g.x1 - g.x0, vy = g.y1 - g.y0;
                float x0 = g.x0 + vx * p0s, y0 = g.y0 + vy * p0s;
                g.x0 = x0;
                g.y0 = y0;
                g.x1 = x0 + vx * period;
                g.y1 = y0 + vy * period;
            }
            for (int i = 0; i < ns; i++)
                pos[i] = (pos[i] - p0s) / period;
            g.repeat = true;
        }
    }
    if (!linear && !radial) {
        /* conic-gradient: the average of its colours */
        uint32_t a = 0, rr = 0, gg = 0, bb = 0;
        for (int i = 0; i < ns; i++) {
            a += col[i] >> 24;
            rr += (col[i] >> 16) & 255;
            gg += (col[i] >> 8) & 255;
            bb += col[i] & 255;
        }
        uint32_t avg = ((a / ns) << 24) | ((rr / ns) << 16) | ((gg / ns) << 8) | (bb / ns);
        if (radii)
            au_fill_rounded_rect(c, r, radii, avg);
        else
            au_fill_rect(c, r, avg);
        return;
    }
    for (int i = 0; i < ns; i++)
        au_gradient_add_stop(&g, pos[i], col[i]);
    if (radii && !au_radii_zero(radii)) {
        AuPath p;
        au_path_init(&p);
        au_path_rounded_rect(&p, r, radii, false);
        au_fill_path_gradient(c, &p, AU_FILL_NONZERO, &g);
        au_path_free(&p);
    } else {
        au_fill_rect_gradient(c, r, &g);
    }
}

static float bg_len(const AuCssValue *v, float basis)
{
    if (!v)
        return 0;
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num * basis / 100;
    if (v->type == AU_CSSV_DIMENSION)
        return v->u.num;
    if (v->type == AU_CSSV_CALC) {
        /* calc(p% + px) as computed: sum of a percentage and a length */
        float t = 0;
        for (uint32_t i = 0; i < v->n; i++)
            t += bg_len(&v->u.items[i], basis);
        return t;
    }
    return 0;
}

/* One background or mask layer: a gradient (abs NULL) or the image at the
 * resolved URL abs. */
static void draw_bg_layer(PCtx *P, const AuStyle *s, const AuBgLayer *ly, AuRect origin_box, AuRect clip_box,
                          const AuRadii *radii, const char *abs)
{
    const AuCssValue *im = ly->image;
    bool gradient = abs == NULL;
    int iw = 0, ih = 0;
    const AuSurface *px = NULL;
    if (gradient) {
        /* No intrinsic size: auto is the origin box (CSS Backgrounds 3). */
        iw = au_f2px(ceilf(origin_box.w));
        ih = au_f2px(ceilf(origin_box.h));
        if (iw <= 0 || ih <= 0)
            return;
    } else if (!P->L->env.image(P->L->env.ctx, abs, &iw, &ih, &px) || !px || iw <= 0 || ih <= 0) {
        return;
    }
    /* Tile size. */
    float tw = gradient ? origin_box.w : (float)iw, th = gradient ? origin_box.h : (float)ih;
    const AuCssValue *sz = ly->size;
    /* An image that does not state its size (an SVG with only a viewBox)
     * is fitted to the origin box (CSS Images 3, default sizing): with a
     * ratio as contain, without one as the box, and a side it states is
     * kept. iw x ih then carry only its ratio. */
    unsigned miss = !gradient && P->L->env.image_missing ? P->L->env.image_missing(P->L->env.ctx, abs) : 0;
    bool size_auto = !sz || (sz->type == AU_CSSV_KEYWORD && sz->kw == AU_K_auto_) ||
                     (sz->type == AU_CSSV_LIST && sz->n == 2 && sz->u.items[0].type == AU_CSSV_KEYWORD &&
                      sz->u.items[1].type == AU_CSSV_KEYWORD);
    if (miss && size_auto) {
        if ((miss & AU_LAYOUT_IMG_MISSING_W) && (miss & AU_LAYOUT_IMG_MISSING_H) && !(miss & AU_LAYOUT_IMG_MISSING_RATIO)) {
            float sc = l_min(origin_box.w / iw, origin_box.h / ih);
            tw = iw * sc;
            th = ih * sc;
        } else if (miss & AU_LAYOUT_IMG_MISSING_RATIO) {
            if (miss & AU_LAYOUT_IMG_MISSING_W)
                tw = origin_box.w;
            if (miss & AU_LAYOUT_IMG_MISSING_H)
                th = origin_box.h;
        }
    } else if (sz && sz->type == AU_CSSV_KEYWORD && (sz->kw == AU_K_cover || sz->kw == AU_K_contain)) {
        if (!gradient) {
            float sx = origin_box.w / iw, sy = origin_box.h / ih;
            float sc = sz->kw == AU_K_cover ? l_max(sx, sy) : l_min(sx, sy);
            tw = iw * sc;
            th = ih * sc;
        }
    } else if (sz && sz->type == AU_CSSV_LIST && sz->n == 2) {
        const AuCssValue *a = &sz->u.items[0], *b = &sz->u.items[1];
        bool aa = a->type == AU_CSSV_KEYWORD, ba = b->type == AU_CSSV_KEYWORD;
        if (!aa)
            tw = bg_len(a, origin_box.w);
        if (!ba)
            th = bg_len(b, origin_box.h);
        if (!gradient && aa && !ba)
            tw = !(miss & AU_LAYOUT_IMG_MISSING_RATIO) ? th * iw / ih : (miss & AU_LAYOUT_IMG_MISSING_W) ? origin_box.w : (float)iw;
        if (!gradient && ba && !aa)
            th = !(miss & AU_LAYOUT_IMG_MISSING_RATIO) ? tw * ih / iw : (miss & AU_LAYOUT_IMG_MISSING_H) ? origin_box.h : (float)ih;
    }
    if (tw < 0.5f || th < 0.5f)
        return;
    if (!gradient && P->L->env.image_use && tw <= 8192 && th <= 8192) {
        /* A vector image is drawn at the tile size. */
        P->L->env.image_use(P->L->env.ctx, abs, au_f2px(ceilf(tw)), au_f2px(ceilf(th)));
    }
    float px0 = origin_box.x + bg_len(ly->position_x, origin_box.w - tw) * 1;
    float py0 = origin_box.y + bg_len(ly->position_y, origin_box.h - th);
    if (ly->position_x && ly->position_x->type == AU_CSSV_PERCENT)
        px0 = origin_box.x + (origin_box.w - tw) * ly->position_x->u.num / 100;
    if (ly->position_y && ly->position_y->type == AU_CSSV_PERCENT)
        py0 = origin_box.y + (origin_box.h - th) * ly->position_y->u.num / 100;
    bool rx = ly->repeat_x != AU_K_no_repeat, ry = ly->repeat_y != AU_K_no_repeat;
    if (gradient && fabsf(tw - origin_box.w) < 0.01f && fabsf(th - origin_box.h) < 0.01f &&
        fabsf(px0 - origin_box.x) < 0.01f && fabsf(py0 - origin_box.y) < 0.01f &&
        (rx && ry ? true
                  : clip_box.x >= origin_box.x - 0.01f && clip_box.y >= origin_box.y - 0.01f &&
                        clip_box.x + clip_box.w <= origin_box.x + origin_box.w + 0.01f &&
                        clip_box.y + clip_box.h <= origin_box.y + origin_box.h + 0.01f)) {
        /* One tile over the origin box (the usual case). A repeated tile
         * of the full box fills the clip with the same gradient extended,
         * which is close enough when the clip box is only a little larger
         * (border-box clip with padding-box origin). */
        fill_gradient(P->c, im, origin_box, clip_box, radii, s);
        return;
    }
    au_canvas_clip_push(P->c, (AuRect){clip_box.x, clip_box.y, clip_box.w, clip_box.h});
    float x0 = px0, y0 = py0;
    if (rx)
        x0 = px0 - ceilf((px0 - clip_box.x) / tw) * tw;
    if (ry)
        y0 = py0 - ceilf((py0 - clip_box.y) / th) * th;
    float x1 = rx ? clip_box.x + clip_box.w : px0 + tw;
    float y1 = ry ? clip_box.y + clip_box.h : py0 + th;
    int count = 0;
    for (float y = y0; y < y1 && count < 20000; y += th)
        for (float x = x0; x < x1 && count < 20000; x += tw, count++) {
            if (!rect_hits(&P->dirty, x, y, tw, th))
                continue;
            if (gradient) {
                AuRect t = {x, y, tw, th};
                fill_gradient(P->c, im, t, t, NULL, s);
                continue;
            }
            au_draw_image(P->c, px, (AuIRect){0, 0, px->width, px->height}, (AuRect){x, y, tw, th},
                          AU_FILTER_BILINEAR, 1);
        }
    au_canvas_clip_pop(P->c);
}

static void draw_bg_image(PCtx *P, const AuStyle *s, const AuBgLayer *ly, AuRect origin_box, AuRect clip_box,
                          const AuRadii *radii)
{
    const AuCssValue *im = ly->image;
    if (!im)
        return;
    if (im->type == AU_CSSV_FUNCTION) {
        draw_bg_layer(P, s, ly, origin_box, clip_box, radii, NULL);
        return;
    }
    if (im->type != AU_CSSV_URL || !P->L->env.image)
        return;
    char buf[2048];
    char *abs = l_resolve_url(P->L, ly->base_url, im->u.str, buf, sizeof buf);
    draw_bg_layer(P, s, ly, origin_box, clip_box, radii, abs);
    l_url_free(abs, buf);
}

static AuRect box_area_of(const LBlock *b, AuRect r, int which)
{
    if (which == AU_K_padding_box || which == AU_K_content_box) {
        r.x += b->bw[3];
        r.y += b->bw[0];
        r.w -= b->bw[1] + b->bw[3];
        r.h -= b->bw[0] + b->bw[2];
    }
    if (which == AU_K_content_box) {
        r.x += b->p[3];
        r.y += b->p[0];
        r.w -= b->p[1] + b->p[3];
        r.h -= b->p[0] + b->p[2];
    }
    return r;
}

static AuRect box_area(const LBlock *b, float ox, float oy, int which)
{
    return box_area_of(b, (AuRect){ox, oy, b->w, b->h}, which);
}

/* r with its edges rounded to whole surface pixels, as browsers snap a
 * box's background: a one-pixel rule at a fractional position stays one
 * sharp row in its own colour instead of two pale ones. Edges shared by
 * two boxes round alike, so neighbours still meet. */
static AuRect snap_device(const AuCanvas *c, AuRect r)
{
    float dx, dy;
    au_canvas_get_translation(c, &dx, &dy);
    float x0 = roundf(r.x + dx), y0 = roundf(r.y + dy);
    float x1 = roundf(r.x + r.w + dx), y1 = roundf(r.y + r.h + dy);
    return (AuRect){x0 - dx, y0 - dy, x1 - x0, y1 - y0};
}

static void paint_background(PCtx *P, const AuStyle *s, const LBlock *b, AuRect border_box, bool is_root_canvas)
{
    (void)is_root_canvas;
    size_t nl = au_style_bg_layer_count(s);
    AuRadii radii;
    box_radii(b, &radii, border_box.w, border_box.h);
    bool rounded = !au_radii_zero(&radii);
    uint32_t bgc = pc(s, s->bg->background_color);
    AuBgLayer last;
    au_style_bg_layer(s, nl - 1, &last);
    if (bgc >> 24) {
        AuRect r = box_area_of(b, border_box, last.clip);
        if (rounded) {
            AuRadii rr = radii;
            float in[4] = {0};
            if (last.clip != AU_K_border_box)
                for (int i = 0; i < 4; i++)
                    in[i] = b->bw[i] + (last.clip == AU_K_content_box ? b->p[i] : 0);
            inset_radii(&rr, in);
            au_fill_rounded_rect(P->c, r, &rr, bgc);
        } else {
            au_fill_rect(P->c, snap_device(P->c, r), bgc);
        }
    }
    for (size_t i = nl; i-- > 0;) {
        AuBgLayer ly;
        au_style_bg_layer(s, i, &ly);
        if (!ly.image || (ly.image->type == AU_CSSV_KEYWORD))
            continue;
        AuRect clip = box_area_of(b, border_box, ly.clip);
        AuRect origin = box_area_of(b, border_box, ly.origin);
        if (ly.attachment == AU_K_fixed) {
            /* Positioned against the viewport and seen through the box's
             * clip area (CSS Backgrounds 3, 3.5). A scroll then changes
             * what the box shows: au_layout_scroll_damage repaints. */
            origin = (AuRect){P->view_x, P->view_y, P->L->vw, P->L->vh};
            P->L->fixed_backgrounds = true;
        }
        draw_bg_image(P, s, &ly, origin, clip, rounded ? &radii : NULL);
    }
}

static void paint_shadows(PCtx *P, const AuStyle *s, AuRect box, const AuRadii *radii, bool inset)
{
    const AuCssBlob *bs = s->rare->box_shadow;
    if (!bs)
        return;
    uint32_t n;
    const AuCssValue *it = l_items(&bs->value, &n);
    if (bs->value.type != AU_CSSV_COMMA_LIST) {
        it = &bs->value;
        n = 1;
    }
    for (uint32_t i = n; i-- > 0;) {
        if (it[i].type != AU_CSSV_LIST || it[i].n < 6)
            continue;
        const AuCssValue *v = it[i].u.items;
        bool is_inset = v[5].type == AU_CSSV_KEYWORD && v[5].kw == AU_K_inset;
        if (is_inset != inset)
            continue;
        uint32_t col = pc(s, v[4].u.rgba);
        if (!(col >> 24))
            continue;
        au_draw_box_shadow(P->c, box, radii, v[0].u.num, v[1].u.num, v[2].u.num, v[3].u.num, col, inset);
    }
}

/* ---- Form controls: native (Platinum) look ----
 *
 * A control keeps its native look while the author leaves its appearance
 * alone: appearance is not none, and no author style sets its background,
 * border or box-shadow (AU_STYLE_F_AUTHOR_DECOR). Checkboxes, radio
 * buttons, sliders and meters stay native unless appearance is none, as in
 * browsers. A restyled control is painted as its CSS box plus its content
 * (text, arrow). The colours are the Platinum greys of the browser chrome. */

#define PL_FACE AU_RGB(221, 221, 221)
#define PL_LIGHT AU_RGB(255, 255, 255)
#define PL_SHADOW AU_RGB(153, 153, 153)
#define PL_FRAME AU_RGB(102, 102, 102)
#define PL_DARK AU_RGB(68, 68, 68)
#define PL_INK AU_RGB(0, 0, 0)
#define PL_OFF AU_RGB(170, 170, 170)
#define PL_HILITE AU_RGB(204, 204, 255)
#define PL_BAR AU_RGB(102, 102, 204)
#define PL_BAR_LIGHT AU_RGB(153, 153, 238)

static bool control_native(const LBlock *b)
{
    if (lb_anon(&b->b) || !l_is_control_kind(b->b.replaced))
        return false;
    const AuStyle *s = b->b.style;
    if (s->rare->appearance == AU_APPEARANCE_NONE)
        return false;
    switch (b->b.replaced) {
    case LR_CHECKBOX:
    case LR_RADIO:
    case LR_RANGE:
    case LR_METER:
    case LR_FILE:
        return true;
    default:
        return !(s->flags & AU_STYLE_F_AUTHOR_DECOR);
    }
}

static bool control_disabled(const LBlock *b)
{
    /* Its own disabled attribute, or a disabled fieldset around it. */
    for (AuNode *n = b->b.node; n && au_is_element(n); n = n->parent)
        if ((n == b->b.node || au_is_html_element(n, AU_ATOM_fieldset)) &&
            au_element_get_attr(au_as_element(n), AU_ATOM_disabled))
            return true;
    return false;
}

static void pl_fill(AuCanvas *c, float x, float y, float w, float h, uint32_t col)
{
    if (w > 0 && h > 0)
        au_fill_rect(c, (AuRect){x, y, w, h}, col);
}

/* r rounded to whole pixels, so one-pixel lines stay sharp. */
static AuRect snap(AuRect r)
{
    float x0 = roundf(r.x), y0 = roundf(r.y), x1 = roundf(r.x + r.w), y1 = roundf(r.y + r.h);
    return (AuRect){x0, y0, x1 - x0, y1 - y0};
}

/* A sunken edit field frame of width bw (1 or 2) around r, filled. */
static void pl_field(AuCanvas *c, AuRect r, float bw, uint32_t face, bool off)
{
    r = snap(r);
    pl_fill(c, r.x, r.y, r.w, r.h, face);
    if (bw >= 2) {
        uint32_t o1 = off ? PL_OFF : PL_SHADOW, i1 = off ? PL_OFF : PL_DARK;
        pl_fill(c, r.x, r.y, r.w, 1, o1);
        pl_fill(c, r.x, r.y, 1, r.h, o1);
        pl_fill(c, r.x, r.y + r.h - 1, r.w, 1, PL_LIGHT);
        pl_fill(c, r.x + r.w - 1, r.y, 1, r.h, PL_LIGHT);
        pl_fill(c, r.x + 1, r.y + 1, r.w - 2, 1, i1);
        pl_fill(c, r.x + 1, r.y + 1, 1, r.h - 2, i1);
        pl_fill(c, r.x + 1, r.y + r.h - 2, r.w - 2, 1, PL_FACE);
        pl_fill(c, r.x + r.w - 2, r.y + 1, 1, r.h - 2, PL_FACE);
    } else if (bw >= 1) {
        uint32_t f = off ? PL_OFF : PL_FRAME;
        pl_fill(c, r.x, r.y, r.w, 1, f);
        pl_fill(c, r.x, r.y + r.h - 1, r.w, 1, f);
        pl_fill(c, r.x, r.y, 1, r.h, f);
        pl_fill(c, r.x + r.w - 1, r.y, 1, r.h, f);
    }
}

/* A raised Platinum push button: a dark rounded frame, a grey face with a
 * light top-left and a shadowed bottom-right edge. */
static void pl_button(AuCanvas *c, AuRect r, bool off, float radius)
{
    r = snap(r);
    if (r.w < 2 || r.h < 2)
        return;
    float rd = l_min(radius, l_min(r.w, r.h) / 2);
    AuRadii ro = {{rd, rd, rd, rd}, {rd, rd, rd, rd}};
    au_fill_rounded_rect(c, r, &ro, off ? PL_OFF : PL_DARK);
    AuRect in = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
    float ri = l_max(0, rd - 1);
    AuRadii rr = {{ri, ri, ri, ri}, {ri, ri, ri, ri}};
    au_fill_rounded_rect(c, in, &rr, off ? AU_RGB(232, 232, 232) : PL_FACE);
    if (in.w > 4 && in.h > 4) {
        float k = l_max(1, ri / 2);
        pl_fill(c, in.x + k, in.y, in.w - 2 * k, 1, PL_LIGHT);
        pl_fill(c, in.x, in.y + k, 1, in.h - 2 * k, PL_LIGHT);
        if (!off) {
            pl_fill(c, in.x + k, in.y + in.h - 1, in.w - 2 * k, 1, PL_SHADOW);
            pl_fill(c, in.x + in.w - 1, in.y + k, 1, in.h - 2 * k, PL_SHADOW);
        }
    }
}

static void tri_down(AuCanvas *c, float cx, float cy, float a, uint32_t col)
{
    AuPath p;
    au_path_init(&p);
    au_path_move_to(&p, cx - a, cy - a / 2);
    au_path_line_to(&p, cx + a, cy - a / 2);
    au_path_line_to(&p, cx, cy + a / 2);
    au_path_close(&p);
    au_fill_path(c, &p, AU_FILL_NONZERO, col);
    au_path_free(&p);
}

/* The native frame of control b (border box r), in place of its CSS
 * background and border. */
static void paint_native_frame(PCtx *P, LBlock *b, AuRect r)
{
    bool off = control_disabled(b);
    const AuStyle *s = b->b.style;
    uint32_t field = pc(s, s->bg->background_color);
    if (!(field >> 24))
        field = PL_LIGHT;
    switch (b->b.replaced) {
    case LR_TEXT_INPUT:
    case LR_TEXTAREA:
    case LR_LISTBOX:
        pl_field(P->c, r, l_max(1, l_min(b->bw[0], 2)), off ? AU_RGB(238, 238, 238) : field, off);
        break;
    case LR_BUTTON:
    case LR_COLOR:
        pl_button(P->c, r, off, 4);
        break;
    case LR_SELECT: {
        pl_button(P->c, r, off, 3);
        AuRect sr = snap(r);
        float aw = l_select_arrow_w(b);
        float x = roundf(sr.x + sr.w - b->bw[1] - aw);
        if (aw > 0 && sr.h > 6) {
            pl_fill(P->c, x, sr.y + 2, 1, sr.h - 4, PL_SHADOW);
            pl_fill(P->c, x + 1, sr.y + 2, 1, sr.h - 4, PL_LIGHT);
        }
        break;
    }
    default:
        break;
    }
}

/* Background, shadows and borders of a block box at (ox, oy). */
static LBlock *fieldset_legend(const LBlock *b)
{
    if (b->b.inner != LI_FLOW || lb_anon(&b->b) || !l_is_tag(&b->b, AU_ATOM_fieldset))
        return NULL;
    for (LBox *c = b->b.first; c; c = c->next)
        if (c->kind == LK_BLOCK && (c->flags & LF_LEGEND))
            return (LBlock *)c;
    return NULL;
}

static void paint_decor_box(PCtx *P, LBlock *b, float ox, float oy);

static void paint_decor(PCtx *P, LBlock *b, float ox, float oy)
{
    paint_decor_box(P, b, ox, oy);
}

/* The backgrounds of the column group, column, row group and row under
 * cell k (CSS 2.1 17.5.1 layers 2 to 5), each placed on its own box and
 * painted only inside the cell's border box, as browsers paint them. */
static void paint_cell_layers(PCtx *P, LCellBox *cb, float ox, float oy)
{
    LBlock *k = &cb->k;
    LBlock *row = (LBlock *)k->b.parent;
    LTableBox *t = l_table_of(&k->b);
    if (!t || !t->g || !row || row->b.inner != LI_TABLE_ROW || cb->row >= t->g->nrows)
        return;
    LBlock *grp = t->g->rows[cb->row].group;
    float rx = ox - k->x, ry = oy - k->y;           /* row origin */
    float gx = grp ? rx - row->x : 0, gy = grp ? ry - row->y : 0;
    float tx = grp ? gx - grp->x : rx - row->x, ty = grp ? gy - grp->y : ry - row->y; /* table origin */
    AuRect cell = {ox, oy, k->w, k->h};
    au_canvas_clip_push(P->c, cell);
    LBlock *done_g = NULL, *done_c = NULL;
    for (int layer = 0; layer < 2; layer++) {
        for (uint32_t c = cb->col; c < (uint32_t)cb->col + cb->colspan && c < t->g->ncols; c++) {
            LBlock *cg = t->g->cols[c].group, *col = t->g->cols[c].col;
            LBlock *x = layer == 0 ? cg : col;
            if (!x || lb_anon(&x->b) || (x->b.flags & LF_HIDDEN) || x == (layer == 0 ? done_g : done_c))
                continue;
            if (layer == 0)
                done_g = x;
            else
                done_c = x;
            float bx = tx + x->x, by = ty + x->y;
            if (layer == 1 && cg && col->b.parent == &cg->b)
                bx += cg->x, by += cg->y;
            paint_background(P, x->b.style, x, (AuRect){bx, by, x->w, x->h}, false);
        }
    }
    if (grp && !lb_anon(&grp->b) && !(grp->b.flags & LF_HIDDEN))
        paint_background(P, grp->b.style, grp, (AuRect){gx, gy, grp->w, grp->h}, false);
    if (!lb_anon(&row->b) && !(row->b.flags & LF_HIDDEN))
        paint_background(P, row->b.style, row, (AuRect){rx, ry, row->w, row->h}, false);
    au_canvas_clip_pop(P->c);
}

static void paint_decor_box(PCtx *P, LBlock *b, float ox, float oy)
{
    int in = b->b.inner;
    if (in == LI_TABLE_ROW || in == LI_TABLE_ROW_GROUP || in == LI_TABLE_COLUMN || in == LI_TABLE_COLUMN_GROUP)
        return; /* painted under each cell */
    if (b->b.flags & LF_HIDDEN)
        return;
    const AuStyle *s = b->b.style;
    bool collapsed = false;
    if (in == LI_TABLE_CELL) {
        LTableBox *t = l_table_of(&b->b);
        collapsed = t && l_table_collapsed(t);
        if (!rect_hits(&P->dirty, ox, oy, b->w, b->h))
            return;
        paint_cell_layers(P, (LCellBox *)b, ox, oy);
        /* empty-cells: hide drops the cell's own background and borders;
         * the row and column backgrounds still show (as in Chrome). */
        if (!collapsed && s->inhr->empty_cells == AU_EMPTY_CELLS_HIDE && l_cell_empty(b))
            return;
    } else if (in == LI_TABLE && !lb_anon(&b->b)) {
        collapsed = l_table_collapsed((LTableBox *)b);
    }
    if (lb_anon(&b->b))
        return;
    AuRect box = {ox, oy, b->w, b->h};
    if (!rect_hits(&P->dirty, ox + b->ov.x, oy + b->ov.y, b->ov.w, b->ov.h))
        return;
    if (in == LI_TABLE) {
        /* The table's border and background go around its grid, not its
         * captions. */
        box.y += ((LTableBox *)b)->grid_y;
        box.h = ((LTableBox *)b)->grid_h;
    }
    if (collapsed) {
        /* Borders are painted on the grid lines by the table. */
        paint_shadows(P, s, box, NULL, false);
        paint_background(P, s, b, box, false);
        paint_shadows(P, s, box, NULL, true);
        return;
    }
    /* A fieldset's border runs through the middle of its legend. */
    LBlock *lg = fieldset_legend(b);
    if (lg) {
        float lt = l_max(0, (lg->h + lg->m[0] + lg->m[2] - b->bw[0]) / 2);
        box.y += lt;
        box.h = l_max(0, box.h - lt);
    }
    AuRadii radii;
    box_radii(b, &radii, box.w, box.h);
    bool rounded = !au_radii_zero(&radii);
    paint_shadows(P, s, box, rounded ? &radii : NULL, false);
    if (control_native(b)) {
        paint_native_frame(P, b, box);
        paint_shadows(P, s, box, rounded ? &radii : NULL, true);
        return;
    }
    if (b != (LBlock *)P->L->root && b != P->canvas_box)
        paint_background(P, s, b, box, false);
    paint_shadows(P, s, box, rounded ? &radii : NULL, true);
    const AuStyleSurround *sr = s->surr;
    uint32_t col[4];
    for (int i = 0; i < 4; i++)
        col[i] = pc(s, sr->border_color[i]);
    if (!lg) {
        draw_borders(P->c, box, b->bw, col, sr->border_style, rounded ? &radii : NULL);
        return;
    }
    /* Leave out the top border behind the legend's border box. */
    float gx0 = ox + lg->x, gx1 = gx0 + lg->w;
    AuRect parts[3] = {{box.x - 50, box.y - 50, gx0 - box.x + 50, box.h + 100},
                       {gx1, box.y - 50, box.x + box.w + 50 - gx1, box.h + 100},
                       {gx0, box.y + b->bw[0], gx1 - gx0, box.h + 50}};
    for (int i = 0; i < 3; i++) {
        if (parts[i].w <= 0)
            continue;
        au_canvas_clip_push(P->c, parts[i]);
        draw_borders(P->c, box, b->bw, col, sr->border_style, rounded ? &radii : NULL);
        au_canvas_clip_pop(P->c);
    }
}

static void paint_outline(PCtx *P, LBox *bx, AuRect box)
{
    if (lb_anon(bx) || (bx->flags & LF_HIDDEN))
        return;
    const AuStyle *s = bx->style;
    const AuStyleRare *r = s->rare;
    if (r->outline_style <= AU_OUTLINE_STYLE_HIDDEN || r->outline_width <= 0)
        return;
    float e = r->outline_width + r->outline_offset;
    AuRect o = {box.x - e, box.y - e, box.w + 2 * e, box.h + 2 * e};
    float w[4] = {r->outline_width, r->outline_width, r->outline_width, r->outline_width};
    uint32_t c = r->outline_style == AU_OUTLINE_STYLE_AUTO ? AU_RGB(0x10, 0x60, 0xE0) : pc(s, r->outline_color);
    uint32_t cs[4] = {c, c, c, c};
    uint8_t st = r->outline_style == AU_OUTLINE_STYLE_AUTO ? AU_BORDER_STYLE_SOLID : r->outline_style;
    uint8_t sts[4] = {st, st, st, st};
    draw_borders(P->c, o, w, cs, sts, NULL);
}

/* ---- Replaced content ---- */

/* Draws one line of text t in area: vertically centred as a control's
 * inner line, aligned 0 left, 1 centre, 2 right, clipped to clip. */
static void paint_label(PCtx *P, LBlock *b, const char *t, AuRect area, uint32_t color, int align, AuRect clip)
{
    if (!t || !*t || clip.w <= 0 || clip.h <= 0)
        return;
    LFont *f = l_font(P->L, b->b.style);
    size_t n = strlen(t);
    float w = l_measure(P->L, f, b->b.style, t, n);
    float x = area.x;
    if (align == 1)
        x = area.x + (area.w - w) / 2;
    else if (align == 2)
        x = area.x + area.w - w;
    float asc, lh = l_control_line(P->L, b, &asc);
    float y = area.y + (area.h - lh) / 2 + asc;
    au_canvas_clip_push(P->c, clip);
    l_draw_text(P->L, P->c, f, b->b.style, t, n, x, y, color, 0);
    au_canvas_clip_pop(P->c);
}

/* The text alignment of a control's label: 0 left, 1 centre, 2 right. */
static int label_align(const AuStyle *s, int dflt)
{
    switch (s->inh->text_align) {
    case AU_TEXT_ALIGN_CENTER:
    case AU_TEXT_ALIGN_WEBKIT_CENTER: return 1;
    case AU_TEXT_ALIGN_RIGHT:
    case AU_TEXT_ALIGN_WEBKIT_RIGHT:
    case AU_TEXT_ALIGN_END: return 2;
    case AU_TEXT_ALIGN_LEFT:
    case AU_TEXT_ALIGN_WEBKIT_LEFT: return 0;
    default: return dflt;
    }
}

static uint32_t placeholder_color(const AuStyle *s)
{
    const AuStyle *ps = au_style_pseudo(s, AU_CSS_PSEUDO_PLACEHOLDER);
    return ps ? pc(ps, ps->inh->color) : AU_RGB(0x75, 0x75, 0x75);
}

static bool control_checked(const LBlock *b)
{
    if (!b->b.node || !au_is_element(b->b.node))
        return false;
    const AuElement *e = au_as_element(b->b.node);
    if (e->state & AU_STATE_CHECKED_SET)
        return (e->state & AU_STATE_CHECKED) != 0;
    return l_attr(&b->b, au_atom_lookup("checked", 7)) != NULL;
}

static float num_attr(const LBlock *b, const char *name, float dflt, bool *present)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    const char *v = a ? l_attr(&b->b, a) : NULL;
    char *end = NULL;
    float f = v ? strtof(v, &end) : 0;
    bool ok = v && end != v && isfinite(f);
    if (present)
        *present = ok;
    return ok ? f : dflt;
}

static void pl_checkbox(AuCanvas *c, AuRect r, bool checked, bool off)
{
    r = snap(r);
    if (r.w < 4 || r.h < 4)
        return;
    pl_fill(c, r.x, r.y, r.w, r.h, off ? PL_OFF : PL_INK);
    pl_fill(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, off ? AU_RGB(238, 238, 238) : PL_FACE);
    if (!off) {
        pl_fill(c, r.x + 1, r.y + 1, r.w - 3, 1, PL_LIGHT);
        pl_fill(c, r.x + 1, r.y + 1, 1, r.h - 3, PL_LIGHT);
        pl_fill(c, r.x + 2, r.y + r.h - 2, r.w - 3, 1, PL_SHADOW);
        pl_fill(c, r.x + r.w - 2, r.y + 2, 1, r.h - 3, PL_SHADOW);
    }
    if (checked) {
        AuPath p, sp;
        au_path_init(&p);
        au_path_init(&sp);
        au_path_move_to(&p, r.x + r.w * 0.22f, r.y + r.h * 0.50f);
        au_path_line_to(&p, r.x + r.w * 0.44f, r.y + r.h * 0.76f);
        au_path_line_to(&p, r.x + r.w * 0.86f, r.y + r.h * 0.10f);
        au_path_stroke(&p, l_max(1.6f, r.w / 6.5f), &sp);
        au_fill_path(c, &sp, AU_FILL_NONZERO, off ? PL_SHADOW : PL_INK);
        au_path_free(&p);
        au_path_free(&sp);
    }
}

static void pl_radio(AuCanvas *c, AuRect r, bool checked, bool off)
{
    float d = l_min(r.w, r.h);
    if (d < 4)
        return;
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rr = d / 2;
    AuPath p;
    au_path_init(&p);
    au_path_ellipse(&p, cx, cy, rr, rr);
    au_fill_path(c, &p, AU_FILL_NONZERO, off ? PL_OFF : PL_INK);
    au_path_clear(&p);
    au_path_ellipse(&p, cx, cy, rr - 1, rr - 1);
    au_fill_path(c, &p, AU_FILL_NONZERO, off ? AU_RGB(238, 238, 238) : PL_FACE);
    if (!off) {
        au_path_clear(&p);
        au_path_ellipse(&p, cx - rr * 0.22f, cy - rr * 0.22f, rr * 0.5f, rr * 0.5f);
        au_fill_path(c, &p, AU_FILL_NONZERO, PL_LIGHT);
    }
    if (checked) {
        au_path_clear(&p);
        au_path_ellipse(&p, cx, cy, rr * 0.42f, rr * 0.42f);
        au_fill_path(c, &p, AU_FILL_NONZERO, off ? PL_SHADOW : PL_INK);
    }
    au_path_free(&p);
}

/* Parses #rgb or #rrggbb (an input's colour value); black otherwise. */
static uint32_t color_value(const char *v)
{
    unsigned rgb[3] = {0, 0, 0};
    size_t n = v ? strlen(v) : 0;
    if (n == 7 && v[0] == '#') {
        for (int i = 0; i < 3; i++) {
            char h[3] = {v[1 + 2 * i], v[2 + 2 * i], 0};
            char *e;
            unsigned long x = strtoul(h, &e, 16);
            if (*e)
                return AU_RGB(0, 0, 0);
            rgb[i] = (unsigned)x;
        }
    }
    return AU_RGB(rgb[0], rgb[1], rgb[2]);
}

/* Progress and meter bars: a framed track and its filled part. */
static void paint_bar(PCtx *P, LBlock *b, AuRect cb, bool native)
{
    bool has_value;
    float frac;
    uint32_t fillc = PL_BAR;
    if (b->b.replaced == LR_PROGRESS) {
        float mx = num_attr(b, "max", 1, NULL);
        if (mx <= 0)
            mx = 1;
        float v = num_attr(b, "value", 0, &has_value);
        frac = l_max(0, l_min(1, v / mx));
    } else {
        /* HTML, "The meter element": the gauge regions. */
        float mn = num_attr(b, "min", 0, NULL), mx = num_attr(b, "max", 1, NULL);
        if (mx < mn)
            mx = mn;
        float v = l_max(mn, l_min(mx, num_attr(b, "value", 0, NULL)));
        float lo = l_max(mn, l_min(mx, num_attr(b, "low", mn, NULL)));
        float hi = l_max(lo, l_min(mx, num_attr(b, "high", mx, NULL)));
        float opt = l_max(mn, l_min(mx, num_attr(b, "optimum", (mn + mx) / 2, NULL)));
        frac = mx > mn ? (v - mn) / (mx - mn) : 0;
        has_value = true;
        int region; /* 0 optimum, 1 suboptimum, 2 even less good */
        if (opt < lo)
            region = v <= lo ? 0 : v <= hi ? 1 : 2;
        else if (opt > hi)
            region = v >= hi ? 0 : v >= lo ? 1 : 2;
        else
            region = (v >= lo && v <= hi) ? 0 : 1;
        fillc = region == 0 ? AU_RGB(51, 170, 51) : region == 1 ? AU_RGB(230, 180, 0) : AU_RGB(204, 51, 51);
    }
    AuRect r = snap(cb);
    if (r.w < 2 || r.h < 2)
        return;
    if (native) {
        pl_fill(P->c, r.x, r.y, r.w, r.h, PL_FRAME);
        r = (AuRect){r.x + 1, r.y + 1, r.w - 2, r.h - 2};
        pl_fill(P->c, r.x, r.y, r.w, r.h, AU_RGB(238, 238, 238));
    }
    if (!has_value) {
        /* Indeterminate: Platinum's diagonal stripes. */
        au_canvas_clip_push(P->c, r);
        float step = l_max(4, r.h);
        for (float x = r.x - r.h; x < r.x + r.w; x += step * 2) {
            AuPath p;
            au_path_init(&p);
            au_path_move_to(&p, x, r.y + r.h);
            au_path_line_to(&p, x + step, r.y + r.h);
            au_path_line_to(&p, x + step + r.h, r.y);
            au_path_line_to(&p, x + r.h, r.y);
            au_path_close(&p);
            au_fill_path(P->c, &p, AU_FILL_NONZERO, AU_RGB(170, 170, 221));
            au_path_free(&p);
        }
        au_canvas_clip_pop(P->c);
        return;
    }
    float w = roundf(r.w * frac);
    pl_fill(P->c, r.x, r.y, w, r.h, fillc);
    if (fillc == PL_BAR)
        pl_fill(P->c, r.x, r.y, w, 1, PL_BAR_LIGHT);
}

static void paint_listbox(PCtx *P, LBlock *b, AuRect pb, uint32_t color)
{
    LReplaced *r = b->rep;
    if (!r || !r->label)
        return;
    const AuStyle *s = b->b.style;
    LFont *f = l_font(P->L, s);
    float rh = l_listbox_row(P->L, b), asc;
    l_control_line(P->L, b, &asc);
    float em = s->inh->font_size;
    au_canvas_clip_push(P->c, pb);
    const char *t = r->label;
    float y = pb.y + b->p[0];
    float x0 = pb.x + b->p[3] + 2;
    while (*t && y < pb.y + pb.h) {
        const char *e = strchr(t, '\n');
        size_t n = e ? (size_t)(e - t) : strlen(t);
        uint8_t fl = (uint8_t)t[0] & 15;
        if (fl & LB_SELECTED)
            pl_fill(P->c, pb.x, y, pb.w, rh, control_disabled(b) ? AU_RGB(221, 221, 221) : PL_HILITE);
        float x = x0 + ((fl & LB_INDENT) ? roundf(em * 0.75f) : 0);
        uint32_t col = (fl & LB_DISABLED) ? AU_RGB(128, 128, 128) : color;
        if (n > 1) {
            l_draw_text(P->L, P->c, f, s, t + 1, n - 1, x, y + 1 + asc, col, 0);
            if (fl & LB_GROUP) /* bold, by overstriking */
                l_draw_text(P->L, P->c, f, s, t + 1, n - 1, x + 0.6f, y + 1 + asc, col, 0);
        }
        y += rh;
        t += n + (e ? 1 : 0);
    }
    au_canvas_clip_pop(P->c);
}

static void paint_range(PCtx *P, LBlock *b, AuRect cb, bool native)
{
    float mn = num_attr(b, "min", 0, NULL), mx = num_attr(b, "max", 100, NULL);
    if (mx < mn)
        mx = mn;
    float v = num_attr(b, "value", mn + (mx - mn) / 2, NULL);
    float frac = mx > mn ? l_max(0, l_min(1, (v - mn) / (mx - mn))) : 0;
    bool off = control_disabled(b);
    float tw = 11, th = l_min(cb.h, 15);
    float cy = roundf(cb.y + cb.h / 2);
    pl_field(P->c, (AuRect){cb.x + 2, cy - 2, cb.w - 4, 5}, 2, AU_RGB(204, 204, 204), off);
    float tx = cb.x + (cb.w - tw) * frac;
    AuRect th_r = snap((AuRect){tx, cy - th / 2, tw, th});
    if (native)
        pl_button(P->c, th_r, off, 2);
    else
        pl_fill(P->c, th_r.x, th_r.y, th_r.w, th_r.h, PL_FRAME);
}

/* ---- Inline SVG ---- */

/* Resolves url against the document and gets its image, asking for it at
 * dw x dh first when dw > 0. NULL while it loads or when it failed. */
static const AuSurface *env_image_at(AuLayout *L, const char *url, int dw, int dh, int *iw, int *ih)
{
    if (!L->env.image)
        return NULL;
    char buf[2048];
    char *abs = l_resolve_url(L, NULL, url, buf, sizeof buf);
    const AuSurface *px = NULL;
    int w = 0, h = 0;
    bool ok = L->env.image(L->env.ctx, abs, &w, &h, &px) && px && w > 0 && h > 0;
    if (ok && dw > 0 && dh > 0 && L->env.image_use)
        L->env.image_use(L->env.ctx, abs, dw, dh);
    l_url_free(abs, buf);
    if (!ok)
        return NULL;
    if (iw)
        *iw = w;
    if (ih)
        *ih = h;
    return px->width > 1 || px->height > 1 || w == 1 ? px : NULL;
}

static const AuSurface *svg_env_image(void *ctx, const AuElement *el, const char *href, int dw, int dh)
{
    (void)el;
    while (*href == ' ' || *href == '\t' || *href == '\n')
        href++;
    return *href ? env_image_at(ctx, href, dw, dh, NULL, NULL) : NULL;
}

static AuElement *svg_env_find_id(void *ctx, AuAtom id)
{
    AuLayout *L = ctx;
    if (!L->doc)
        return NULL;
    if (!L->svg_ids)
        L->svg_ids = au_svg_id_index_new(&L->doc->node);
    return au_svg_id_index_find(L->svg_ids, id);
}

static void paint_svg(PCtx *P, LBlock *b, AuRect cb)
{
    if (!b->b.node || !au_is_element(b->b.node) || cb.w <= 0 || cb.h <= 0)
        return;
    AuSvgEnv env = {P->L->env.fonts, P->L, svg_env_image, svg_env_find_id};
    /* The whole pixels of the box's place go into the canvas translation
     * and the drawing gets the fraction: its coordinates, and so every
     * antialiased edge, are then the same wherever the page is scrolled
     * to (the rasterizer keeps whole pixels of the translation apart). */
    float ix = floorf(cb.x), iy = floorf(cb.y);
    au_canvas_translate(P->c, ix, iy);
    cb.x -= ix;
    cb.y -= iy;
    au_svg_paint(P->c, l_svg_names(P->L), au_as_element(b->b.node), cb, &env);
    au_canvas_translate(P->c, -ix, -iy);
}

/* The content-box radii that clip a replaced element's content (CSS
 * Backgrounds 3, 5.3: the border radii less the border and padding),
 * if any corner stays rounded. */
static bool content_radii(const LBlock *b, AuRadii *out)
{
    box_radii(b, out, b->w, b->h);
    if (au_radii_zero(out))
        return false;
    float in[4];
    for (int i = 0; i < 4; i++)
        in[i] = b->bw[i] + b->p[i];
    inset_radii(out, in);
    return !au_radii_zero(out);
}

static void paint_replaced(PCtx *P, LBlock *b, float ox, float oy)
{
    if (b->b.flags & LF_HIDDEN)
        return;
    AuRect cb = box_area(b, ox, oy, AU_K_content_box);
    AuRect pbx = box_area(b, ox, oy, AU_K_padding_box);
    const AuStyle *s = b->b.style;
    uint32_t color = pc(s, s->inh->color);
    LReplaced *r = b->rep;
    bool native = control_native(b);
    switch (b->b.replaced) {
    case LR_SVG:
        paint_svg(P, b, cb);
        break;
    case LR_CANVAS:
        /* The canvas's bitmap, scaled to the content box. */
        if (P->L->env.canvas && b->b.node && au_is_element(b->b.node) && cb.w > 0 && cb.h > 0) {
            const AuSurface *px = NULL;
            if (P->L->env.canvas(P->L->env.ctx, au_as_element(b->b.node), &px) == 0 && px && px->width > 0 &&
                px->height > 0)
                au_draw_image(P->c, px, (AuIRect){0, 0, px->width, px->height}, cb,
                              s->inhr->image_rendering == AU_IMAGE_RENDERING_PIXELATED ? AU_FILTER_NEAREST
                                                                                     : AU_FILTER_BILINEAR,
                              1);
        }
        break;
    case LR_OTHER:
        /* A nested document (iframe, frame, object, embed). */
        if (P->L->env.paint_frame && b->b.node && au_is_element(b->b.node) && cb.w >= 1 && cb.h >= 1)
            P->L->env.paint_frame(P->L->env.ctx, au_as_element(b->b.node), P->c, cb);
        break;
    case LR_IMAGE:
        if (r && r->pixels) {
            /* object-fit and object-position (CSS Images 3, section 5.5):
             * the image is drawn at its concrete object size, placed in
             * the content box and clipped to it. */
            if (r->marker)
                cb.w = l_max(0, cb.w - L_MARKER_IMAGE_GAP);
            AuRect ob = cb;
            int fit = r->marker ? AU_OBJECT_FIT_FILL : s->rare->object_fit;
            if (fit != AU_OBJECT_FIT_FILL && r->iw > 0 && r->ih > 0 && cb.w > 0 && cb.h > 0) {
                float sx = cb.w / r->iw, sy = cb.h / r->ih;
                float sc = fit == AU_OBJECT_FIT_COVER ? l_max(sx, sy) : fit == AU_OBJECT_FIT_NONE ? 1 : l_min(sx, sy);
                if (fit == AU_OBJECT_FIT_SCALE_DOWN && sc > 1)
                    sc = 1;
                ob.w = r->iw * sc;
                ob.h = r->ih * sc;
                const AuCssValue *px_ = NULL, *py_ = NULL;
                const AuCssBlob *op = s->rare->object_position;
                if (op && op->value.type == AU_CSSV_LIST && op->value.n == 2) {
                    px_ = &op->value.u.items[0];
                    py_ = &op->value.u.items[1];
                }
                ob.x = cb.x + (px_ ? bg_len(px_, cb.w - ob.w) : (cb.w - ob.w) / 2);
                ob.y = cb.y + (py_ ? bg_len(py_, cb.h - ob.h) : (cb.h - ob.h) / 2);
            }
            bool crop = ob.x < cb.x - 0.01f || ob.y < cb.y - 0.01f || ob.x + ob.w > cb.x + cb.w + 0.01f ||
                        ob.y + ob.h > cb.y + cb.h + 0.01f;
            if (r->url && P->L->env.image_use && ob.w >= 1 && ob.h >= 1 && ob.w <= 8192 && ob.h <= 8192)
                P->L->env.image_use(P->L->env.ctx, r->url, au_f2px(ceilf(ob.w)), au_f2px(ceilf(ob.h)));
            const AuSurface *px = r->pixels;
            AuRadii rr;
            bool round = content_radii(b, &rr);
            if (crop)
                au_canvas_clip_push(P->c, cb);
            if (round)
                au_canvas_group_begin(P->c, cb);
            if (ob.w > 0 && ob.h > 0)
                au_draw_image(P->c, px, (AuIRect){0, 0, px->width, px->height}, ob,
                              s->inhr->image_rendering == AU_IMAGE_RENDERING_PIXELATED ? AU_FILTER_NEAREST
                                                                                     : AU_FILTER_BILINEAR,
                              1);
            if (round) {
                AuPath path;
                au_path_init(&path);
                au_path_rounded_rect(&path, cb, &rr, false);
                au_canvas_group_mask(P->c, &path);
                au_path_free(&path);
                au_canvas_group_end(P->c, 1.0f);
            }
            if (crop)
                au_canvas_clip_pop(P->c);
        } else if (cb.w >= 1 && cb.h >= 1) {
            /* Placeholder: a light frame, and the alt text. */
            float w1[4] = {1, 1, 1, 1};
            uint32_t g = AU_RGB(0xC0, 0xC0, 0xC0);
            uint32_t gs[4] = {g, g, g, g};
            uint8_t st[4] = {AU_BORDER_STYLE_SOLID, AU_BORDER_STYLE_SOLID, AU_BORDER_STYLE_SOLID, AU_BORDER_STYLE_SOLID};
            if (r && r->loaded)
                au_fill_rect(P->c, cb, AU_RGB(0xEE, 0xEE, 0xEE));
            draw_borders(P->c, cb, w1, gs, st, NULL);
            if (r && r->label)
                paint_label(P, b, r->label, (AuRect){cb.x + 2, cb.y, cb.w - 4, cb.h}, color, 0, cb);
        }
        break;
    case LR_TEXT_INPUT:
    case LR_TEXTAREA:
        /* The live value (or placeholder), selection and caret. */
        l_paint_field(P->L, P->c, b, cb, pbx, color, placeholder_color(s));
        break;
    case LR_BUTTON:
        paint_label(P, b, r ? r->label : NULL, cb, color, label_align(s, 1), pbx);
        break;
    case LR_SELECT: {
        float aw = l_select_arrow_w(b);
        AuRect area = {cb.x, cb.y, l_max(0, cb.w - aw), cb.h};
        paint_label(P, b, r ? r->label : NULL, area, color, 0, (AuRect){cb.x, pbx.y, area.w, pbx.h});
        if (aw > 0) {
            float a = l_max(2.5f, roundf(s->inh->font_size * 0.25f));
            float cx = roundf(cb.x + cb.w - aw / 2 + b->p[1] / 2), cy = roundf(cb.y + cb.h / 2) + 0.5f;
            tri_down(P->c, cx, cy, a, native && control_disabled(b) ? PL_SHADOW : color);
        }
        break;
    }
    case LR_LISTBOX:
        paint_listbox(P, b, pbx, color);
        break;
    case LR_FILE: {
        float asc, lh = l_control_line(P->L, b, &asc);
        float bh = lh + 6, bw = l_file_button_w(P->L, b);
        AuRect br = {cb.x, cb.y + (cb.h - bh) / 2, bw, bh};
        bool off = control_disabled(b);
        if (s->rare->appearance != AU_APPEARANCE_NONE)
            pl_button(P->c, br, off, 4);
        paint_label(P, b, r ? r->label : NULL, br, off ? AU_RGB(128, 128, 128) : color, 1, br);
        AuRect tr = {cb.x + bw + 4, cb.y, l_max(0, cb.w - bw - 4), cb.h};
        paint_label(P, b, r ? r->aux : NULL, tr, color, 0, (AuRect){tr.x, pbx.y, tr.w, pbx.h});
        break;
    }
    case LR_COLOR: {
        AuRect sw = snap((AuRect){cb.x + 2, cb.y + 2, cb.w - 4, cb.h - 4});
        if (sw.w > 2 && sw.h > 2) {
            pl_fill(P->c, sw.x, sw.y, sw.w, sw.h, PL_FRAME);
            pl_fill(P->c, sw.x + 1, sw.y + 1, sw.w - 2, sw.h - 2,
                 color_value(l_attr(&b->b, au_atom_lookup("value", 5))));
        }
        break;
    }
    case LR_CHECKBOX:
    case LR_RADIO: {
        if (!native)
            break; /* appearance: none: only the CSS box */
        AuRect bx = {ox, oy, b->w, b->h};
        if (b->b.replaced == LR_RADIO)
            pl_radio(P->c, bx, control_checked(b), control_disabled(b));
        else
            pl_checkbox(P->c, bx, control_checked(b), control_disabled(b));
        break;
    }
    case LR_RANGE:
        paint_range(P, b, cb, native);
        break;
    case LR_PROGRESS:
    case LR_METER:
        paint_bar(P, b, cb, native);
        break;
    case LR_DISCLOSURE_OPEN:
    case LR_DISCLOSURE_CLOSED: {
        /* A triangle: closed points right, open points down; sat on the
         * baseline like the text it precedes. */
        float em = s->inh->font_size, a = roundf(em * 0.3f);
        float cx = cb.x + a + em * 0.05f, cy = oy + b->h - a;
        AuPath p;
        au_path_init(&p);
        if (b->b.replaced == LR_DISCLOSURE_OPEN) {
            au_path_move_to(&p, cx - a, cy - a * 0.6f);
            au_path_line_to(&p, cx + a, cy - a * 0.6f);
            au_path_line_to(&p, cx, cy + a * 0.9f);
        } else {
            au_path_move_to(&p, cx - a * 0.7f, cy - a);
            au_path_line_to(&p, cx + a * 0.9f, cy);
            au_path_line_to(&p, cx - a * 0.7f, cy + a);
        }
        au_path_close(&p);
        au_fill_path(P->c, &p, AU_FILL_NONZERO, color);
        au_path_free(&p);
        break;
    }
    case LR_DISC:
    case LR_CIRCLE:
    case LR_SQUARE: {
        float em = s->inh->font_size;
        float d = l_max(2, em * 0.35f);
        float cx = cb.x + d / 2 + em * 0.05f, cy = oy + b->h - d / 2 - l_max(0, (b->h - d) / 2);
        if (b->b.replaced == LR_SQUARE) {
            au_fill_rect(P->c, (AuRect){cx - d / 2, cy - d / 2, d, d}, color);
        } else {
            AuPath p;
            au_path_init(&p);
            au_path_ellipse(&p, cx, cy, d / 2, d / 2);
            if (b->b.replaced == LR_DISC) {
                au_fill_path(P->c, &p, AU_FILL_NONZERO, color);
            } else {
                AuPath st;
                au_path_init(&st);
                au_path_stroke(&p, l_max(1, em / 14), &st);
                au_fill_path(P->c, &st, AU_FILL_NONZERO, color);
                au_path_free(&st);
            }
            au_path_free(&p);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- Text ---- */

typedef struct Deco {
    uint8_t lines;
    uint32_t color;
    float thickness;
    uint8_t style;
} Deco;

/* Decorations propagated to text in box t: from its inline ancestors and
 * the block ancestors up to an atomic, floating or positioned box. */
static int collect_decos(const LBox *t, Deco *out, int max)
{
    int n = 0;
    for (const LBox *p = t->kind == LK_TEXT ? t->parent : t; p && n < max; p = p->parent) {
        if (!lb_anon(p)) {
            const AuStyleRare *r = p->style->rare;
            if (r->text_decoration_line & (AU_TDL_UNDERLINE | AU_TDL_OVERLINE | AU_TDL_LINE_THROUGH)) {
                Deco *d = &out[n++];
                d->lines = r->text_decoration_line;
                d->color = pc(p->style, r->text_decoration_color);
                d->thickness = r->text_decoration_thickness.kind == AU_LEN_PX ? r->text_decoration_thickness.u.px : 0;
                d->style = r->text_decoration_style;
            }
        }
        if (p->kind == LK_BLOCK && (p->flags & (LF_INLINE_LEVEL | LF_FLOAT | LF_ABSPOS | LF_FLEX_ITEM)))
            break;
    }
    return n;
}

/* One text decoration line from x to x + w with its top at y: solid,
 * double, dotted, dashed or wavy (CSS Text Decoration 3). Dots and dashes
 * are phased from x = 0 so neighbouring fragments continue the pattern. */
static void deco_line(AuCanvas *c, float x, float y, float w, float th, uint32_t col, int style)
{
    if (w <= 0)
        return;
    switch (style) {
    case AU_TEXT_DECORATION_STYLE_DOUBLE:
        au_fill_rect(c, (AuRect){x, y, w, th}, col);
        au_fill_rect(c, (AuRect){x, y + th * 2, w, th}, col);
        return;
    case AU_TEXT_DECORATION_STYLE_DOTTED:
    case AU_TEXT_DECORATION_STYLE_DASHED: {
        float on = style == AU_TEXT_DECORATION_STYLE_DOTTED ? th : th * 3, period = on * 2;
        float start = floorf(x / period) * period;
        au_canvas_clip_push(c, (AuRect){x, y - th, w, th * 3});
        for (float p = start; p < x + w && p - start < 40000; p += period)
            au_fill_rect(c, (AuRect){p, y, on, th}, col);
        au_canvas_clip_pop(c);
        return;
    }
    case AU_TEXT_DECORATION_STYLE_WAVY: {
        /* A zigzag of period 4 * th around the line, stroked th wide. */
        float amp = l_max(1, th), step = l_max(2, th * 2);
        float start = floorf(x / (2 * step)) * (2 * step);
        AuPath p, s;
        au_path_init(&p);
        au_path_init(&s);
        au_path_move_to(&p, start, y + amp);
        int k = 0;
        for (float px = start + step; px <= x + w + step && k < 20000; px += step, k++)
            au_path_line_to(&p, px, (k & 1) ? y + amp : y - amp);
        au_path_stroke(&p, th, &s);
        au_canvas_clip_push(c, (AuRect){x, y - amp - th, w, 2 * (amp + th)});
        au_fill_path(c, &s, AU_FILL_NONZERO, col);
        au_canvas_clip_pop(c);
        au_path_free(&p);
        au_path_free(&s);
        return;
    }
    default:
        au_fill_rect(c, (AuRect){x, y, w, th}, col);
        return;
    }
}

static void paint_text_frag(PCtx *P, LBlock *root, LFrag *f, float ox, float oy)
{
    LText *t = (LText *)f->box;
    const AuStyle *s = f->box->style;
    if (s->inh->visibility != AU_VISIBILITY_VISIBLE)
        return;
    LFont *font = l_font(P->L, s);
    float x = ox + f->x, base = oy + f->y + f->baseline;
    uint32_t color = pc(s, s->inh->color);
    if (s->inhr->text_fill_color != AU_COLOR_CURRENT && s->inhr->text_fill_color != s->inh->color &&
        (s->inhr->text_fill_color & 0xFF))
        color = pc(s, s->inhr->text_fill_color);
    const char *txt = t->text + f->start;
    uint32_t len = f->len;
    if (P->L->sel.on)
        l_select_paint_frag(P->L, P->c, f, font, x, oy + f->y);
    /* Shadows first. */
    if (s->inhr->text_shadow) {
        uint32_t n;
        const AuCssValue *it = l_items(&s->inhr->text_shadow->value, &n);
        if (s->inhr->text_shadow->value.type != AU_CSSV_COMMA_LIST) {
            it = &s->inhr->text_shadow->value;
            n = 1;
        }
        for (uint32_t i = n; i-- > 0;) {
            if (it[i].type != AU_CSSV_LIST || it[i].n < 4)
                continue;
            const AuCssValue *v = it[i].u.items;
            uint32_t sc = pc(s, v[3].u.rgba);
            float blur = v[2].u.num;
            if (blur > 1) {
                /* Blur is approximated by a fainter offset copy. */
                sc = (sc & 0x00FFFFFF) | ((uint32_t)((sc >> 24) * 0.5f) << 24);
            }
            l_draw_text(P->L, P->c, font, s, txt, len, x + v[0].u.num, base + v[1].u.num, sc, f->spacing);
        }
    }
    l_draw_text(P->L, P->c, font, s, txt, len, x, base, color, f->spacing);
    float endx = x + f->w;
    if (f->flags & FRF_HYPHEN) {
        float hw = l_measure_raw(P->L, font, "-", 1);
        l_draw_text(P->L, P->c, font, s, "-", 1, endx - hw, base, color, 0);
    }
    if (f->flags & FRF_ELLIPSIS) {
        LFont *rf = l_font(P->L, root->b.style);
        l_draw_text(P->L, P->c, rf, root->b.style, "\xE2\x80\xA6", 3, endx, base, color, 0);
    }
    Deco d[8];
    int nd = l_font_hidden(P->L, font) ? 0 : collect_decos(f->box, d, 8); /* hidden with the text */
    for (int i = 0; i < nd; i++) {
        float th = d[i].thickness > 0 ? d[i].thickness : l_max(1, roundf(font->m.underline_thickness));
        float w = f->w;
        if (d[i].lines & AU_TDL_UNDERLINE) {
            float y = roundf(base + l_max(1, font->m.underline_pos));
            if (d[i].style == AU_TEXT_DECORATION_STYLE_WAVY)
                y = roundf(base + l_max(1, font->m.underline_pos) + th);
            deco_line(P->c, x, y, w, th, d[i].color, d[i].style);
        }
        if (d[i].lines & AU_TDL_OVERLINE)
            deco_line(P->c, x, roundf(base - font->m.ascent), w, th, d[i].color, d[i].style);
        if (d[i].lines & AU_TDL_LINE_THROUGH)
            deco_line(P->c, x, roundf(base - font->m.strike_pos - th / 2), w, th, d[i].color, d[i].style);
    }
}

/* The layered box that owns a fragment (innermost layered ancestor up to
 * the IFC root), or NULL. */
static LBox *frag_owner(const LFrag *f, const LBlock *root)
{
    for (LBox *p = f->box; p && p != &root->b; p = p->parent)
        if (p->flags & LF_LAYERED)
            return p;
    return NULL;
}

static void paint_inline_decor(PCtx *P, LFrag *f, float ox, float oy)
{
    LBox *b = f->box;
    if (b->flags & LF_HIDDEN)
        return;
    const AuStyle *s = b->style;
    AuRect box = {ox + f->x, oy + f->y, f->w, f->h};
    uint32_t bgc = pc(s, s->bg->background_color);
    if (bgc >> 24)
        au_fill_rect(P->c, box, bgc);
    size_t nl = au_style_bg_layer_count(s);
    for (size_t i = nl; i-- > 0;) {
        AuBgLayer ly;
        au_style_bg_layer(s, i, &ly);
        if (ly.image && ly.image->type == AU_CSSV_FUNCTION)
            fill_gradient(P->c, ly.image, box, box, NULL, s);
    }
    const AuStyleSurround *sr = s->surr;
    float bw[4] = {sr->border_width[0], (f->flags & FRF_END) ? sr->border_width[1] : 0, sr->border_width[2],
                   (f->flags & FRF_START) ? sr->border_width[3] : 0};
    uint32_t col[4];
    for (int i = 0; i < 4; i++)
        col[i] = pc(s, sr->border_color[i]);
    draw_borders(P->c, box, bw, col, sr->border_style, NULL);
}

static void paint_atomic(PCtx *P, LBlock *k, float ox, float oy);

/* Paints the line fragments of IFC root b (border box at ox, oy). With
 * owner set, only the fragments of that layered inline box. */
static void paint_lines(PCtx *P, LBlock *b, float ox, float oy, LBox *owner)
{
    if (!b->nlines)
        return;
    /* First line that can intersect: lines are in y order. */
    uint32_t lo = 0, hi = b->nlines;
    float top = P->dirty.y - oy;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        LLine *ln = &b->lines[mid];
        /* atomic inlines may extend beyond a line: be generous */
        if (ln->y + ln->h + 200 < top)
            lo = mid + 1;
        else
            hi = mid;
    }
    float bottom = P->dirty.y + P->dirty.h - oy;
    for (uint32_t li = lo; li < b->nlines; li++) {
        LLine *ln = &b->lines[li];
        if (ln->y - 200 > bottom)
            break;
        for (uint32_t i = ln->first; i < ln->first + ln->n; i++) {
            LFrag *f = &b->frags[i];
            if (f->flags & FRF_HIDDEN)
                continue;
            /* Fragments of a layered box are painted with that layer. */
            if (frag_owner(f, b) != owner)
                continue;
            switch (f->kind) {
            case FR_INLINE:
                if (rect_hits(&P->dirty, ox + f->x, oy + f->y, f->w, f->h))
                    paint_inline_decor(P, f, ox, oy);
                break;
            case FR_TEXT:
                if (rect_hits(&P->dirty, ox + f->x - 2, oy + f->y - 2, f->w + 20, f->h + 4))
                    paint_text_frag(P, b, f, ox, oy);
                break;
            case FR_ATOMIC: {
                LBlock *k = (LBlock *)f->box;
                if (k->b.flags & LF_LAYERED)
                    break;
                paint_atomic(P, k, ox + k->x, oy + k->y);
                break;
            }
            default:
                break;
            }
        }
    }
}

/* ---- Tree walks ---- */

static void paint_flow(PCtx *P, LBlock *b, float ox, float oy, int phase);

/* The padding-box radii of b's overflow clip, if any corner is rounded. */
static bool clip_radii(const LBlock *b, AuRadii *out)
{
    AuRadii r;
    box_radii(b, &r, b->w, b->h);
    if (au_radii_zero(&r))
        return false;
    inset_radii(&r, b->bw);
    if (au_radii_zero(&r))
        return false;
    /* In-flow content stays in the content box unless it overflows; a
     * corner the content cannot reach needs no mask (a code block with
     * padding wider than its radius, the common case). */
    float pw = b->w - b->bw[1] - b->bw[3], ph = b->h - b->bw[0] - b->bw[2];
    bool hfit = b->sw <= pw + 0.5f, vfit = b->sh <= ph + 0.5f;
    static const int hs[4] = {3, 1, 1, 3}, vs[4] = {0, 0, 2, 2};
    bool reach = false;
    for (int i = 0; i < 4 && !reach; i++) {
        if (r.rx[i] <= 0 || r.ry[i] <= 0)
            continue;
        bool safe = (vfit && b->p[vs[i]] >= r.ry[i]) || (hfit && b->p[hs[i]] >= r.rx[i]);
        reach = !safe;
    }
    if (!reach)
        return false;
    if (out)
        *out = r;
    return true;
}

/* Pushes b's overflow clip; returns the origin offset for its content. */
static bool push_clip(PCtx *P, LBlock *b, float ox, float oy, float *cx, float *cy, bool shaped)
{
    *cx = ox;
    *cy = oy;
    if (!(b->b.flags & LF_CLIPS))
        return false;
    AuRect r = box_area(b, ox, oy, AU_K_padding_box);
    const AuStyleBox *s = b->b.style->box;
    if (s->overflow_x == AU_OVERFLOW_VISIBLE) {
        r.x -= 100000;
        r.w += 200000;
    }
    if (s->overflow_y == AU_OVERFLOW_VISIBLE) {
        r.y -= 100000;
        r.h += 200000;
    }
    au_canvas_clip_push(P->c, r);
    /* A rounded padding box clips through a group masked at pop_clip. */
    if (shaped && s->overflow_x != AU_OVERFLOW_VISIBLE && s->overflow_y != AU_OVERFLOW_VISIBLE &&
        clip_radii(b, NULL))
        au_canvas_group_begin(P->c, r);
    *cx = ox - b->scroll_x;
    *cy = oy - b->scroll_y;
    return true;
}

/* Undoes push_clip (clipped is its result; ox, oy the box origin). */
static void pop_clip(PCtx *P, LBlock *b, float ox, float oy, bool clipped)
{
    if (!clipped)
        return;
    const AuStyleBox *s = b->b.style->box;
    AuRadii rr;
    if (s->overflow_x != AU_OVERFLOW_VISIBLE && s->overflow_y != AU_OVERFLOW_VISIBLE &&
        clip_radii(b, &rr)) {
        AuPath p;
        au_path_init(&p);
        au_path_rounded_rect(&p, box_area(b, ox, oy, AU_K_padding_box), &rr, false);
        au_canvas_group_mask(P->c, &p);
        au_path_free(&p);
        au_canvas_group_end(P->c, 1.0f);
    }
    au_canvas_clip_pop(P->c);
}

static void paint_scrollbars(PCtx *P, LBlock *b, float ox, float oy)
{
    if (!(b->b.flags & LF_SCROLLER) || (b->b.flags & LF_HIDDEN))
        return;
    /* scrollbar-width: none still scrolls but draws no bars (Tailwind's
     * scrollbar-hide); thin draws narrower ones. The bars overlay the
     * content, so neither changes layout. */
    uint8_t sbw = b->b.style->rare->scrollbar_width;
    if (sbw == AU_SCROLLBAR_WIDTH_NONE)
        return;
    float bw = sbw == AU_SCROLLBAR_WIDTH_THIN ? 5 : 8; /* track */
    AuRect pb = box_area(b, ox, oy, AU_K_padding_box);
    const AuStyleBox *s = b->b.style->box;
    float vw = pb.w, vh = pb.h;
    bool vbar = b->sh > vh + 0.5f || s->overflow_y == AU_OVERFLOW_SCROLL;
    bool hbar = b->sw > vw + 0.5f || s->overflow_x == AU_OVERFLOW_SCROLL;
    if (vbar && s->overflow_y != AU_OVERFLOW_HIDDEN && s->overflow_y != AU_OVERFLOW_CLIP && vh > 8) {
        float th = l_max(16, vh * vh / l_max(vh, b->sh));
        float ty = pb.y + (vh - th) * (b->sh > vh ? b->scroll_y / (b->sh - vh) : 0);
        au_fill_rect(P->c, (AuRect){pb.x + pb.w - bw, pb.y, bw, vh}, AU_RGBA(0, 0, 0, 20));
        AuRadii r = {{3, 3, 3, 3}, {3, 3, 3, 3}};
        au_fill_rounded_rect(P->c, (AuRect){pb.x + pb.w - bw + 1, ty + 1, bw - 2, th - 2}, &r, AU_RGBA(0, 0, 0, 110));
    }
    if (hbar && s->overflow_x != AU_OVERFLOW_HIDDEN && s->overflow_x != AU_OVERFLOW_CLIP && vw > 8) {
        float tw = l_max(16, vw * vw / l_max(vw, b->sw));
        float tx = pb.x + (vw - tw) * (b->sw > vw ? b->scroll_x / (b->sw - vw) : 0);
        au_fill_rect(P->c, (AuRect){pb.x, pb.y + pb.h - bw, vw, bw}, AU_RGBA(0, 0, 0, 20));
        AuRadii r = {{3, 3, 3, 3}, {3, 3, 3, 3}};
        au_fill_rounded_rect(P->c, (AuRect){tx + 1, pb.y + pb.h - bw + 1, tw - 2, bw - 2}, &r, AU_RGBA(0, 0, 0, 110));
    }
}

static void paint_table_children(PCtx *P, LTableBox *t, float ox, float oy, int phase);

/* Descendant walk over b's children for one phase. */
static void paint_children(PCtx *P, LBox *parent, float ox, float oy, int phase)
{
    bool table = parent->kind == LK_BLOCK && parent->inner == LI_TABLE && ((LTableBox *)parent)->g;
    if (table)
        paint_table_children(P, (LTableBox *)parent, ox, oy, phase);
    for (LBox *c = parent->first; c; c = c->next) {
        /* A table's rows and columns are painted through its grid. */
        if (table && c->kind == LK_BLOCK && c->inner >= LI_TABLE_ROW_GROUP && c->inner <= LI_TABLE_COLUMN_GROUP &&
            c->inner != LI_TABLE_CELL)
            continue;
        if (c->kind == LK_INLINE) {
            /* Inline boxes nest as deeply as the page's markup: the same
             * depth limit as blocks, or thousands of nested spans
             * overflow the stack. */
            if (phase == PH_FLOAT && !(c->flags & LF_LAYERED) && P->depth <= L_MAX_DEPTH) {
                P->depth++;
                paint_children(P, c, ox, oy, phase);
                P->depth--;
            }
            continue;
        }
        if (c->kind != LK_BLOCK)
            continue;
        LBlock *k = (LBlock *)c;
        if (c->flags & (LF_LAYERED | LF_TOO_DEEP))
            continue;
        float kx = ox + k->x, ky = oy + k->y;
        if (c->flags & LF_FLOAT) {
            if (phase == PH_FLOAT)
                paint_atomic(P, k, kx, ky);
            continue;
        }
        if (c->flags & LF_MARKER_OUT) {
            if (phase == PH_INLINE)
                paint_atomic(P, k, kx, ky);
            continue;
        }
        if (c->flags & LF_INLINE_LEVEL)
            continue; /* painted with its line */
        if (!rect_hits(&P->dirty, kx + k->ov.x, ky + k->ov.y, k->ov.w, k->ov.h))
            continue;
        if (phase == PH_BG) {
            paint_decor(P, k, kx, ky);
            if (k->b.inner == LI_REPLACED)
                paint_replaced(P, k, kx, ky);
        }
        paint_flow(P, k, kx, ky, phase);
    }
}

static void paint_flow(PCtx *P, LBlock *b, float ox, float oy, int phase)
{
    if (P->depth > L_MAX_DEPTH)
        return;
    P->depth++;
    float cx, cy;
    bool clipped = push_clip(P, b, ox, oy, &cx, &cy, true);
    if (phase == PH_INLINE && (b->b.flags & LF_INLINES))
        paint_lines(P, b, cx, cy, NULL);
    paint_children(P, &b->b, cx, cy, phase);
    pop_clip(P, b, ox, oy, clipped);
    if (phase == PH_INLINE) {
        paint_scrollbars(P, b, ox, oy);
        /* outlines of in-flow descendants */
        if (b->b.parent && !(b->b.flags & LF_LAYERED))
            paint_outline(P, &b->b, (AuRect){ox, oy, b->w, b->h});
    }
    P->depth--;
}

/* A float, inline-block or replaced inline: painted as if it created a
 * stacking context (its layered descendants belong to the real one). */
static void paint_atomic(PCtx *P, LBlock *k, float ox, float oy)
{
    if (k->b.flags & LF_TOO_DEEP)
        return;
    if (!rect_hits(&P->dirty, ox + k->ov.x, oy + k->ov.y, k->ov.w, k->ov.h))
        return;
    paint_decor(P, k, ox, oy);
    if (k->b.inner == LI_REPLACED) {
        paint_replaced(P, k, ox, oy);
        paint_outline(P, &k->b, (AuRect){ox, oy, k->w, k->h});
        return;
    }
    float cx, cy;
    bool clipped = push_clip(P, k, ox, oy, &cx, &cy, true);
    paint_children(P, &k->b, cx, cy, PH_BG);
    paint_children(P, &k->b, cx, cy, PH_FLOAT);
    if (k->b.flags & LF_INLINES)
        paint_lines(P, k, cx, cy, NULL);
    paint_children(P, &k->b, cx, cy, PH_INLINE);
    pop_clip(P, k, ox, oy, clipped);
    paint_scrollbars(P, k, ox, oy);
    paint_outline(P, &k->b, (AuRect){ox, oy, k->w, k->h});
}

/* ---- Tables ---- */

/* Origin of cell cb given the table's border-box origin, or false when
 * the cell is painted elsewhere (a layered row or row group, or a cell
 * outside the grid). */
static bool cell_origin(const LTableBox *t, const LCellBox *cb, float tx, float ty, float *x, float *y)
{
    const LBlock *k = &cb->k;
    if ((k->b.flags & (LF_LAYERED | LF_TOO_DEEP)) || !cb->colspan)
        return false;
    const LTableRow *r = &t->g->rows[cb->row];
    if (r->box->b.flags & LF_LAYERED)
        return false;
    float ox = tx + r->box->x, oy = ty + r->box->y;
    if (r->group) {
        if (r->group->b.flags & LF_LAYERED)
            return false;
        ox += r->group->x;
        oy += r->group->y;
    }
    *x = ox + k->x;
    *y = oy + k->y;
    return true;
}

/* Line positions of the grid in table border-box coordinates. */
static float grid_x(const LTableBox *t, uint32_t c)
{
    uint32_t n = t->g->ncols;
    return c < n ? t->colx[c] : t->colx[n - 1] + t->colw[n - 1];
}

static float grid_y(const LTableBox *t, uint32_t r)
{
    uint32_t n = t->g->nrows;
    return r < n ? t->rowy[r] : t->rowy[n - 1] + t->rowh[n - 1];
}

static const LTableEdge *hedge_at(const LTableBox *t, uint32_t r, int64_t c)
{
    if (c < 0 || c >= (int64_t)t->g->ncols)
        return NULL;
    return &t->hedge[(size_t)r * t->g->ncols + (size_t)c];
}

static const LTableEdge *vedge_at(const LTableBox *t, int64_t r, uint32_t c)
{
    if (r < 0 || r >= (int64_t)t->g->nrows)
        return NULL;
    return &t->vedge[(size_t)r * (t->g->ncols + 1) + c];
}

static int edge_rank(const LTableEdge *e)
{
    switch (e->style) {
    case AU_BORDER_STYLE_DOUBLE: return 8;
    case AU_BORDER_STYLE_SOLID: return 7;
    case AU_BORDER_STYLE_DASHED: return 6;
    case AU_BORDER_STYLE_DOTTED: return 5;
    case AU_BORDER_STYLE_RIDGE: return 4;
    case AU_BORDER_STYLE_OUTSET: return 3;
    case AU_BORDER_STYLE_GROOVE: return 2;
    case AU_BORDER_STYLE_INSET: return 1;
    default: return 0;
    }
}

static bool edge_beats(const LTableEdge *a, const LTableEdge *b)
{
    if (!b || b->w <= 0)
        return a && a->w > 0;
    if (!a || a->w <= 0)
        return false;
    if (a->w != b->w)
        return a->w > b->w;
    if (edge_rank(a) != edge_rank(b))
        return edge_rank(a) > edge_rank(b);
    return a->prec >= b->prec;
}

/* The junction of horizontal line r and vertical line c: the widths of
 * the widest segments meeting there, and whether a horizontal one wins
 * the corner (it then runs through it). */
static void junction(const LTableBox *t, uint32_t r, uint32_t c, float *hw, float *vw, bool *hwin)
{
    const LTableEdge *h0 = hedge_at(t, r, (int64_t)c - 1), *h1 = hedge_at(t, r, c);
    const LTableEdge *v0 = vedge_at(t, (int64_t)r - 1, c), *v1 = vedge_at(t, r, c);
    const LTableEdge *hb = edge_beats(h0, h1) ? h0 : h1, *vb = edge_beats(v0, v1) ? v0 : v1;
    *hw = hb ? hb->w : 0;
    *vw = vb ? vb->w : 0;
    *hwin = edge_beats(hb, vb);
}

/* One collapsed border segment: rectangle r, horizontal or vertical. */
static void paint_edge(PCtx *P, const AuStyle *ts, const LTableEdge *e, AuRect r, bool horiz)
{
    if (e->w <= 0 || r.w <= 0 || r.h <= 0)
        return;
    uint32_t col = pc(ts, e->color);
    if (!(col >> 24))
        return;
    switch (e->style) {
    case AU_BORDER_STYLE_DOUBLE:
        if (e->w >= 3) {
            float t3 = e->w / 3;
            AuRect a = r, b = r;
            if (horiz) {
                a.h = b.h = t3;
                b.y = r.y + r.h - t3;
            } else {
                a.w = b.w = t3;
                b.x = r.x + r.w - t3;
            }
            au_fill_rect(P->c, a, col);
            au_fill_rect(P->c, b, col);
            return;
        }
        break;
    case AU_BORDER_STYLE_DOTTED:
    case AU_BORDER_STYLE_DASHED: {
        float dash = e->style == AU_BORDER_STYLE_DOTTED ? e->w : 3 * e->w;
        float len = horiz ? r.w : r.h;
        for (float p = 0; p < len; p += 2 * dash) {
            float l = l_min(dash, len - p);
            AuRect d = horiz ? (AuRect){r.x + p, r.y, l, r.h} : (AuRect){r.x, r.y + p, r.w, l};
            au_fill_rect(P->c, d, col);
        }
        return;
    }
    case AU_BORDER_STYLE_GROOVE:
    case AU_BORDER_STYLE_RIDGE:
    case AU_BORDER_STYLE_INSET:
    case AU_BORDER_STYLE_OUTSET: {
        /* In the collapsing model inset draws as ridge and outset as
         * groove (CSS 2.1 17.6.2). */
        bool groove = e->style == AU_BORDER_STYLE_GROOVE || e->style == AU_BORDER_STYLE_OUTSET;
        AuRect a = r, b = r;
        if (horiz) {
            a.h = b.h = r.h / 2;
            b.y = r.y + r.h / 2;
        } else {
            a.w = b.w = r.w / 2;
            b.x = r.x + r.w / 2;
        }
        au_fill_rect(P->c, a, bevel(col, groove));
        au_fill_rect(P->c, b, bevel(col, !groove));
        return;
    }
    default: break;
    }
    au_fill_rect(P->c, r, col);
}

/* Collapsed borders, centred on the grid lines, over the cell
 * backgrounds and under the cells' content. */
static void paint_collapsed_borders(PCtx *P, LTableBox *t, float tx, float ty)
{
    LTableGrid *g = t->g;
    uint32_t nr = g->nrows, nc = g->ncols;
    if (!t->edges_ok || !t->hedge || !nr || !nc || (t->k.b.flags & LF_HIDDEN))
        return;
    const AuStyle *ts = t->k.b.style;
    /* Rows near the dirty area: lines are in y order. */
    float top = P->dirty.y - ty - 50, bottom = P->dirty.y + P->dirty.h - ty + 50;
    uint32_t lo = 0, hi = nr;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (grid_y(t, mid + 1) < top)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (uint32_t r = lo; r <= nr; r++) {
        float y = grid_y(t, r);
        if (y > bottom)
            break;
        for (uint32_t c = 0; c < nc; c++) {
            const LTableEdge *e = hedge_at(t, r, c);
            if (e->w > 0) {
                float hw0, vw0, hw1, vw1;
                bool w0, w1;
                junction(t, r, c, &hw0, &vw0, &w0);
                junction(t, r, c + 1, &hw1, &vw1, &w1);
                float x0 = grid_x(t, c) + (w0 ? -vw0 / 2 : vw0 / 2);
                float x1 = grid_x(t, c + 1) + (w1 ? vw1 / 2 : -vw1 / 2);
                paint_edge(P, ts, e, (AuRect){tx + x0, ty + y - e->w / 2, x1 - x0, e->w}, true);
            }
        }
        if (r == nr)
            break;
        float y1 = grid_y(t, r + 1);
        for (uint32_t c = 0; c <= nc; c++) {
            const LTableEdge *e = vedge_at(t, r, c);
            if (e->w <= 0)
                continue;
            float hw0, vw0, hw1, vw1;
            bool w0, w1;
            junction(t, r, c, &hw0, &vw0, &w0);
            junction(t, r + 1, c, &hw1, &vw1, &w1);
            float a = y + (w0 ? hw0 / 2 : -hw0 / 2);
            float b = y1 + (w1 ? -hw1 / 2 : hw1 / 2);
            float x = grid_x(t, c);
            paint_edge(P, ts, e, (AuRect){tx + x - e->w / 2, ty + a, e->w, b - a}, false);
        }
    }
}

/* The cells of table t (border-box origin ox, oy) for one phase, without
 * recursing through the row groups and rows: all cell backgrounds, then
 * the collapsed borders, then the cells' content. */
static void paint_table_children(PCtx *P, LTableBox *t, float ox, float oy, int phase)
{
    LTableGrid *g = t->g;
    if (!t->colx)
        return; /* not laid out */
    if (phase == PH_BG) {
        for (uint32_t i = 0; i < g->ncells; i++) {
            float x, y;
            LBlock *k = &g->cells[i]->k;
            if (cell_origin(t, g->cells[i], ox, oy, &x, &y))
                paint_decor(P, k, x, y);
        }
        if (l_table_collapsed(t))
            paint_collapsed_borders(P, t, ox, oy);
    }
    for (uint32_t i = 0; i < g->ncells; i++) {
        float x, y;
        LBlock *k = &g->cells[i]->k;
        if (!cell_origin(t, g->cells[i], ox, oy, &x, &y))
            continue;
        if (!rect_hits(&P->dirty, x + k->ov.x, y + k->ov.y, k->ov.w, k->ov.h))
            continue;
        paint_flow(P, k, x, y, phase);
    }
}

/* ---- Layers ---- */

static void paint_layer(PCtx *P, LBox *b, float ox, float oy);

float l_sticky_offset(AuLayout *L, LBlock *b, float *dx)
{
    *dx = 0;
    const AuStyleBox *s = b->b.style->box;
    /* The scrollport: the nearest scroll container, else the viewport. */
    LBlock *sc = NULL;
    for (LBox *p = b->b.parent; p; p = p->parent)
        if (p->kind == LK_BLOCK && (p->flags & LF_SCROLLER)) {
            sc = (LBlock *)p;
            break;
        }
    float bx, by;
    l_box_origin(&b->b, &bx, &by);
    float px = 0, py = 0, pw = L->vw, ph = L->vh, sx = L->view_scroll_x, sy = L->view_scroll_y;
    if (sc) {
        l_box_origin(&sc->b, &px, &py);
        px += sc->bw[3];
        py += sc->bw[0];
        pw = sc->w - sc->bw[1] - sc->bw[3];
        ph = sc->h - sc->bw[0] - sc->bw[2];
        sx = sc->scroll_x;
        sy = sc->scroll_y;
        /* positions inside the scroller are relative to its content */
    }
    /* Box position relative to the scrollport's visible area. */
    float rel_y = by - py - sy, rel_x = bx - px - sx;
    LBlock *cbk = l_geom_parent(&b->b);
    float cby = 0, cbh = 1e9f, cbx = 0, cbw = 1e9f;
    if (cbk) {
        float gx, gy;
        l_box_origin(&cbk->b, &gx, &gy);
        cby = gy + cbk->bw[0] + cbk->p[0] - py - sy;
        cbh = cbk->h - l_bp_v(cbk);
        cbx = gx + cbk->bw[3] + cbk->p[3] - px - sx;
        cbw = cbk->w - l_bp_h(cbk);
    }
    float dy = 0;
    if (s->top.kind != AU_LEN_AUTO) {
        float t = l_len(&s->top, ph);
        if (rel_y < t)
            dy = t - rel_y;
        float maxdy = cby + cbh - (rel_y + b->h + b->m[2]);
        if (dy > maxdy)
            dy = l_max(0, maxdy);
    } else if (s->bottom.kind != AU_LEN_AUTO) {
        float bt = l_len(&s->bottom, ph);
        float over = rel_y + b->h - (ph - bt);
        if (over > 0)
            dy = -over;
        float mindy = cby - rel_y + b->m[0];
        if (dy < mindy)
            dy = l_min(0, mindy);
    }
    if (s->left.kind != AU_LEN_AUTO) {
        float l = l_len(&s->left, pw);
        if (rel_x < l)
            *dx = l - rel_x;
        float maxdx = cbx + cbw - (rel_x + b->w);
        if (*dx > maxdx)
            *dx = l_max(0, maxdx);
    }
    return dy;
}

/* A rounded overflow clip pushed for a layer by layer_origin. */
#define LAYER_SHAPED_MAX 4
typedef struct LayerClips {
    int pushed;  /* clips pushed */
    int nshaped; /* of them, groups to mask with a rounded padding box */
    struct {
        LBlock *k;
        float x, y;
        int at; /* index among the pushed clips */
    } shaped[LAYER_SHAPED_MAX];
} LayerClips;

/* The radii of k's overflow clip for a layer whose ink is ink (canvas
 * coordinates), with k's border box at (x, y). False when the clip is
 * square or the ink stays clear of the rounded corners: inside the band
 * between the top and bottom corners, or between the left and right
 * ones. */
static bool layer_clip_radii(const LBlock *k, float x, float y, AuRect ink, AuRadii *out)
{
    const AuStyleBox *s = k->b.style->box;
    if (s->overflow_x == AU_OVERFLOW_VISIBLE || s->overflow_y == AU_OVERFLOW_VISIBLE)
        return false;
    AuRadii r;
    box_radii(k, &r, k->w, k->h);
    if (au_radii_zero(&r))
        return false;
    inset_radii(&r, k->bw);
    if (au_radii_zero(&r))
        return false;
    float px = x + k->bw[3], py = y + k->bw[0];
    float pw = k->w - k->bw[1] - k->bw[3], ph = k->h - k->bw[0] - k->bw[2];
    float top = l_max(r.ry[0], r.ry[1]), bottom = l_max(r.ry[2], r.ry[3]);
    float left = l_max(r.rx[0], r.rx[3]), right = l_max(r.rx[1], r.rx[2]);
    if ((ink.y >= py + top && ink.y + ink.h <= py + ph - bottom) ||
        (ink.x >= px + left && ink.x + ink.w <= px + pw - right))
        return false;
    *out = r;
    return true;
}

/* Origin of layered box b in canvas coordinates, given its stacking
 * context sc at (sx, sy); pushes the clips of the ancestors that clip it
 * (pop them with layer_clips_pop). An ancestor with rounded corners and
 * overflow clipped on both axes clips its positioned descendants to the
 * rounded padding box, as it does its in-flow content (CSS Backgrounds 3,
 * "Corner Clipping"): the layer is painted into a group that is masked
 * when the clip is popped. */
static void layer_origin(PCtx *P, LBox *sc, float sx, float sy, LBox *b, float *ox, float *oy, LayerClips *lc)
{
    LBox *chain[L_MAX_DEPTH + 1];
    int n = 0;
    for (LBox *p = b->parent; p && p != sc && n < L_MAX_DEPTH; p = p->parent)
        chain[n++] = p;
    lc->pushed = lc->nshaped = 0;
    bool fixed = (b->flags & LF_FIXED) && ((LBlock *)b)->cb == NULL;
    /* Which ancestors clip and scroll b: those in its containing block chain. */
    bool applies[L_MAX_DEPTH + 1];
    {
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
    }
    /* Two passes: the first finds the layer's place (the ink decides
     * which rounded clips need a group), the second pushes. */
    AuRect ink = {0, 0, 0, 0};
    bool have_ink = false;
    for (int pass = 0; pass < 2; pass++) {
        float x = sx, y = sy;
        for (int i = n; i-- > 0;) {
            LBox *a = chain[i];
            if (a->kind != LK_BLOCK)
                continue;
            LBlock *k = (LBlock *)a;
            x += k->x;
            y += k->y;
            if (applies[i] && (a->flags & LF_CLIPS)) {
                float cx = x - k->scroll_x, cy = y - k->scroll_y;
                if (pass == 1) {
                    AuRadii rr;
                    push_clip(P, k, x, y, &cx, &cy, false);
                    if (lc->nshaped < LAYER_SHAPED_MAX && (!have_ink || layer_clip_radii(k, x, y, ink, &rr))) {
                        AuRadii any;
                        AuRect all = {x - 1, y - 1, k->w + 2, k->h + 2};
                        if (layer_clip_radii(k, x, y, all, &any)) {
                            au_canvas_group_begin(P->c, box_area(k, x, y, AU_K_padding_box));
                            lc->shaped[lc->nshaped].k = k;
                            lc->shaped[lc->nshaped].x = x;
                            lc->shaped[lc->nshaped].y = y;
                            lc->shaped[lc->nshaped].at = lc->pushed;
                            lc->nshaped++;
                        }
                    }
                    lc->pushed++;
                }
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
                float dy = l_sticky_offset(P->L, k, &dx);
                x += dx;
                y += dy;
            }
            if (fixed) {
                x += P->scroll_x;
                y += P->scroll_y;
            }
            /* The ink is known for an untransformed block; anything else
             * takes the group whenever a corner is rounded. */
            const AuStyleRare *r = b->style->rare;
            if (!r->transform && !r->translate && !r->rotate && !r->scale) {
                float x0 = l_min(0, k->ov.x), y0 = l_min(0, k->ov.y);
                float x1 = l_max(k->w, k->ov.x + k->ov.w), y1 = l_max(k->h, k->ov.y + k->ov.h);
                ink = (AuRect){x + x0, y + y0, x1 - x0, y1 - y0};
                have_ink = true;
            }
        }
        *ox = x;
        *oy = y;
    }
}

static void layer_clips_pop(PCtx *P, LayerClips *lc)
{
    for (int i = lc->pushed; i-- > 0;) {
        if (lc->nshaped && lc->shaped[lc->nshaped - 1].at == i) {
            LBlock *k = lc->shaped[lc->nshaped - 1].k;
            float x = lc->shaped[lc->nshaped - 1].x, y = lc->shaped[lc->nshaped - 1].y;
            AuRadii rr;
            AuRect all = {x - 1, y - 1, k->w + 2, k->h + 2};
            if (layer_clip_radii(k, x, y, all, &rr)) {
                AuPath p;
                au_path_init(&p);
                au_path_rounded_rect(&p, box_area(k, x, y, AU_K_padding_box), &rr, false);
                au_canvas_group_mask(P->c, &p);
                au_path_free(&p);
            }
            au_canvas_group_end(P->c, 1.0f);
            lc->nshaped--;
        }
        au_canvas_clip_pop(P->c);
    }
}

static void paint_layer_list(PCtx *P, LBlock *sc, float sx, float sy, uint32_t from, uint32_t to)
{
    LLayers *ls = sc->layers;
    for (uint32_t i = from; i < to; i++) {
        LBox *b = ls->items[i];
        float ox, oy;
        LayerClips lc;
        layer_origin(P, &sc->b, sx, sy, b, &ox, &oy, &lc);
        paint_layer(P, b, ox, oy);
        layer_clips_pop(P, &lc);
    }
}

/* A layered inline box: its fragments on the IFC root's lines. ox, oy is
 * the IFC root's origin. */
static void paint_inline_layer(PCtx *P, LBox *b, float ox, float oy)
{
    LBlock *g = l_geom_parent(b);
    if (!g)
        return;
    paint_lines(P, g, ox, oy, b);
    /* floats inside it */
    paint_children(P, b, ox, oy, PH_FLOAT);
}

/* mask-image: the layers (images and gradients, placed like background
 * layers with the mask's size, repeat, position, origin and clip) are
 * drawn into a mask group and the box's painted group is multiplied by
 * their alpha, or by their luminance with mask-mode: luminance. A
 * url(#id) naming an SVG <mask> is not drawn (the element shows
 * unmasked). */
static void paint_mask(PCtx *P, LBlock *k, float ox, float oy, AuRect bounds)
{
    const AuStyle *s = k->b.style;
    size_t n = au_style_mask_layer_count(s);
    AuRect border = {ox, oy, k->w, k->h};
    bool luminance = false, first = true;
    au_canvas_group_begin(P->c, bounds);
    for (size_t i = n; i-- > 0;) {
        AuBgLayer ly;
        au_style_mask_layer(s, i, &ly);
        if (!ly.image || ly.image->type == AU_CSSV_KEYWORD)
            continue;
        if (first)
            luminance = ly.attachment == AU_K_luminance;
        first = false;
        draw_bg_image(P, s, &ly, box_area_of(k, border, ly.origin), box_area_of(k, border, ly.clip), NULL);
    }
    au_canvas_mask_end(P->c, luminance);
}

/* ---- clip-path basic shapes (CSS Masking 1, 5.1) ---- */

/* A computed length or percentage of basis; *ok cleared otherwise. */
static float cp_len(const AuCssValue *v, float basis, bool *ok)
{
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num * basis / 100;
    if ((v->type == AU_CSSV_DIMENSION && v->unit == AU_UNIT_PX) || (v->type == AU_CSSV_NUMBER && v->u.num == 0))
        return v->u.num;
    if (v->type == AU_CSSV_CALC) {
        /* calc(10px + 40%), as computed: au_len_resolve evaluates the
         * tree (the blob header only carries the node). */
        AuCssBlob tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.refs = 1;
        tmp.value = *v;
        AuLen l;
        memset(&l, 0, sizeof l);
        l.kind = AU_LEN_CALC;
        l.u.calc = &tmp;
        return au_len_resolve(&l, basis < 0 ? 0 : basis);
    }
    *ok = false;
    return 0;
}

/* One coordinate of an "at" position: a keyword, length or percentage. */
static float cp_pos(const AuCssValue *v, float basis, bool *ok)
{
    if (v->type == AU_CSSV_KEYWORD) {
        if (v->kw == AU_K_left || v->kw == AU_K_top)
            return 0;
        if (v->kw == AU_K_right || v->kw == AU_K_bottom)
            return basis;
        if (v->kw == AU_K_center)
            return basis / 2;
        *ok = false;
        return 0;
    }
    return cp_len(v, basis, ok);
}

/* A circle or ellipse radius against the distances to the sides. */
static float cp_radius(const AuCssValue *v, float near, float far, float basis, bool *ok)
{
    if (v->type == AU_CSSV_KEYWORD)
        return v->kw == AU_K_farthest_side ? far : v->kw == AU_K_closest_side ? near : (*ok = false, 0);
    return cp_len(v, basis, ok);
}

/* The "at" position from it[i], it[i + 1] (horizontal then vertical, or
 * a vertical keyword first) into *cx, *cy, relative to box r. */
static void cp_at(const AuCssValue *it, uint32_t i, uint32_t n, AuRect r, float *cx, float *cy, bool *ok)
{
    *cx = r.w / 2;
    *cy = r.h / 2;
    if (i >= n)
        return;
    if (it[i].type != AU_CSSV_KEYWORD || it[i].kw != AU_K_at || n - i - 1 < 1 || n - i - 1 > 2) {
        *ok = false;
        return;
    }
    const AuCssValue *a = &it[i + 1], *b = n - i - 1 == 2 ? &it[i + 2] : NULL;
    if (b && a->type == AU_CSSV_KEYWORD && (a->kw == AU_K_top || a->kw == AU_K_bottom)) {
        const AuCssValue *t = a;
        a = b;
        b = t;
    }
    if (!b && a->type == AU_CSSV_KEYWORD && (a->kw == AU_K_top || a->kw == AU_K_bottom)) {
        *cy = cp_pos(a, r.h, ok);
        return;
    }
    *cx = cp_pos(a, r.w, ok);
    if (b)
        *cy = cp_pos(b, r.h, ok);
}

/* The clip-path shape of k at ox, oy: 0 when nothing clips (none,
 * url(), a shape this does not draw), 1 with the shape in *p (in canvas
 * coordinates; the caller frees it), -1 when the shape encloses no area
 * and nothing paints (the visually-hidden polygon(0 0, 0 0, 0 0)). */
static int clip_path_build(const LBlock *k, float ox, float oy, AuPath *p)
{
    if (lb_anon(&k->b) || !k->b.style->rare->clip_path)
        return 0;
    uint32_t n;
    const AuCssValue *it = l_items(&k->b.style->rare->clip_path->value, &n);
    const AuCssValue *fn = NULL;
    int box = AU_K_border_box;
    for (uint32_t i = 0; i < n; i++) {
        if (it[i].type == AU_CSSV_FUNCTION)
            fn = &it[i];
        else if (it[i].type == AU_CSSV_KEYWORD && it[i].kw != AU_K_none)
            box = it[i].kw;
        else
            return 0; /* none, url() */
    }
    AuRect r = box_area(k, ox, oy, box == AU_K_padding_box || box == AU_K_content_box ? box : AU_K_border_box);
    if (box == AU_K_margin_box) {
        r.x -= k->m[3];
        r.y -= k->m[0];
        r.w += k->m[1] + k->m[3];
        r.h += k->m[0] + k->m[2];
    }
    bool ok = true;
    if (!fn) {
        if (r.w <= 0 || r.h <= 0)
            return -1;
        au_path_rect(p, r);
        return 1;
    }
    uint32_t na;
    const AuCssValue *a = fn->n ? l_items(&fn->u.items[0], &na) : NULL;
    if (!fn->n)
        na = 0;
    if (fn->kw == AU_K_polygon) {
        uint32_t g = 0;
        if (fn->n && na == 1 && a[0].type == AU_CSSV_KEYWORD && (a[0].kw == AU_K_nonzero || a[0].kw == AU_K_evenodd))
            g = 1;
        if (fn->n - g < 3)
            return -1;
        float area2 = 0, px0 = 0, py0 = 0, lx = 0, ly = 0;
        for (uint32_t i = g; i < fn->n && ok; i++) {
            uint32_t m;
            const AuCssValue *q = l_items(&fn->u.items[i], &m);
            if (m != 2) {
                ok = false;
                break;
            }
            float x = r.x + cp_len(&q[0], r.w, &ok), y = r.y + cp_len(&q[1], r.h, &ok);
            if (i == g) {
                au_path_move_to(p, x, y);
                px0 = x, py0 = y;
            } else {
                au_path_line_to(p, x, y);
                area2 += (lx - px0) * (y - py0) - (x - px0) * (ly - py0);
            }
            lx = x, ly = y;
        }
        au_path_close(p);
        if (ok && area2 > -0.01f && area2 < 0.01f)
            return -1;
    } else if (fn->kw == AU_K_inset && fn->n == 1) {
        float d[4] = {0, 0, 0, 0};
        uint32_t nl = 0;
        while (nl < na && !(a[nl].type == AU_CSSV_KEYWORD && a[nl].kw == AU_K_round))
            nl++;
        if (nl < 1 || nl > 4) {
            ok = false;
        } else {
            for (uint32_t s = 0; s < 4; s++) {
                /* top, right, bottom, left from 1 to 4 values */
                uint32_t src = nl == 1 ? 0 : nl == 2 ? s % 2 : nl == 3 ? (s == 3 ? 1 : s) : s;
                d[s] = cp_len(&a[src], s % 2 ? r.w : r.h, &ok);
            }
        }
        AuRect q = {r.x + d[3], r.y + d[0], r.w - d[1] - d[3], r.h - d[0] - d[2]};
        if (ok && (q.w <= 0 || q.h <= 0))
            return -1;
        AuRadii rr;
        memset(&rr, 0, sizeof rr);
        uint32_t nr = 0;
        for (uint32_t i = nl + 1; i < na && ok; i++) {
            if (a[i].type == AU_CSSV_DELIM)
                break; /* elliptical radii: the horizontal ones serve */
            if (nr < 4)
                rr.rx[nr] = rr.ry[nr] = cp_len(&a[i], q.w, &ok);
            nr++;
        }
        if (nr > 0 && nr < 4) {
            /* 1 to 3 values: TL, TR, BR, BL as for border-radius */
            float v0 = rr.rx[0], v1 = nr > 1 ? rr.rx[1] : v0, v2 = nr > 2 ? rr.rx[2] : v0;
            float c[4] = {v0, v1, v2, v1};
            for (int s = 0; s < 4; s++)
                rr.rx[s] = rr.ry[s] = c[s];
        }
        if (nr > 0) {
            au_radii_fit(&rr, q);
            au_path_rounded_rect(p, q, &rr, false);
        } else {
            au_path_rect(p, q);
        }
    } else if ((fn->kw == AU_K_circle || fn->kw == AU_K_ellipse) && fn->n <= 1) {
        bool circle = fn->kw == AU_K_circle;
        uint32_t nrad = 0;
        while (nrad < na && !(a[nrad].type == AU_CSSV_KEYWORD && a[nrad].kw == AU_K_at))
            nrad++;
        float cx, cy;
        cp_at(a, nrad, na, r, &cx, &cy, &ok);
        float nx = l_min(cx, r.w - cx), fx = l_max(cx, r.w - cx);
        float ny = l_min(cy, r.h - cy), fy = l_max(cy, r.h - cy);
        float rx, ry;
        if (circle) {
            if (nrad > 1)
                ok = false;
            float diag = sqrtf((r.w * r.w + r.h * r.h) / 2);
            rx = nrad ? cp_radius(&a[0], l_min(nx, ny), l_max(fx, fy), diag, &ok) : l_min(nx, ny);
            ry = rx;
        } else {
            if (nrad != 0 && nrad != 2)
                ok = false;
            rx = nrad == 2 ? cp_radius(&a[0], nx, fx, r.w, &ok) : nx;
            ry = nrad == 2 ? cp_radius(&a[1], ny, fy, r.h, &ok) : ny;
        }
        if (ok && (rx <= 0 || ry <= 0))
            return -1;
        au_path_ellipse(p, r.x + cx, r.y + cy, rx, ry);
    } else {
        ok = false; /* path(), rect(), xywh() */
    }
    return ok ? 1 : 0;
}

/* The clip property (CSS 2.1, 11.1.2) of an absolutely positioned k:
 * rect(top, right, bottom, left) as offsets from the border box's
 * top-left corner, auto meaning that edge of the border box. 0 when it
 * does not apply, 1 with the rectangle in *p, -1 when it encloses no
 * area (the visually-hidden clip: rect(1px, 1px, 1px, 1px)). */
static int clip_rect_build(const LBlock *k, float ox, float oy, AuPath *p)
{
    if (lb_anon(&k->b) || !(k->b.flags & LF_ABSPOS) || !k->b.style->rare->clip)
        return 0;
    const AuCssValue *v = &k->b.style->rare->clip->value;
    if (v->type != AU_CSSV_FUNCTION || v->kw != AU_K_rect)
        return 0;
    uint32_t n = v->n;
    const AuCssValue *a = v->u.items;
    if (n == 1)
        a = l_items(&v->u.items[0], &n);
    if (n != 4)
        return 0;
    AuRect r = box_area(k, ox, oy, AU_K_border_box);
    float d[4];
    float edge[4] = {0, r.w, r.h, 0}; /* top, right, bottom, left */
    bool ok = true;
    for (int i = 0; i < 4; i++)
        d[i] = a[i].type == AU_CSSV_KEYWORD && a[i].kw == AU_K_auto_ ? edge[i] : cp_len(&a[i], 0, &ok);
    if (!ok)
        return 0;
    AuRect q = {r.x + d[3], r.y + d[0], d[1] - d[3], d[2] - d[0]};
    if (q.w <= 0 || q.h <= 0)
        return -1;
    au_path_rect(p, q);
    return 1;
}

static int clip_path_shape(const LBlock *k, float ox, float oy, AuPath *p)
{
    au_path_init(p);
    int r = k->b.style && !lb_anon(&k->b) && k->b.style->rare->clip_path ? clip_path_build(k, ox, oy, p)
                                                                         : clip_rect_build(k, ox, oy, p);
    if (r <= 0)
        au_path_free(p);
    return r;
}

static bool has_bg_image(const AuStyle *s);

/* An <img> scaled or moved by its transform (no rotation or skew) with
 * nothing else to paint: no background, border, shadow, outline,
 * rounded corners or object-fit. It is drawn straight to its transformed
 * rectangle instead of into a layer that is then scaled: one resampling
 * instead of two, and no layer. Reddit's blurred post backgrounds are
 * scale(1.2) images, and their layers were most of a scroll step's
 * paint. False when the box needs the layer. */
static bool paint_scaled_image(PCtx *P, LBlock *k, const AuMatrix *tm, float ox, float oy, float opacity)
{
    if (tm->b != 0 || tm->c != 0 || tm->a <= 0 || tm->d <= 0)
        return false;
    if (k->b.inner != LI_REPLACED || k->b.replaced != LR_IMAGE || lb_anon(&k->b) || (k->b.flags & LF_HIDDEN))
        return false;
    LReplaced *r = k->rep;
    if (!r || !r->pixels || r->marker)
        return false;
    const AuStyle *s = k->b.style;
    const AuStyleRare *rare = s->rare;
    if (rare->object_fit != AU_OBJECT_FIT_FILL || rare->box_shadow || lb_anon(&k->b) ||
        au_style_mask_layer_count(s) > 0 || (rare->outline_style > AU_OUTLINE_STYLE_HIDDEN && rare->outline_width > 0))
        return false;
    if ((pc(s, s->bg->background_color) >> 24) || has_bg_image(s))
        return false;
    for (int i = 0; i < 4; i++)
        if (k->bw[i] > 0)
            return false;
    AuRadii rr;
    if (content_radii(k, &rr))
        return false;
    AuRect cb = box_area(k, ox, oy, AU_K_content_box);
    if (cb.w <= 0 || cb.h <= 0)
        return true;
    AuRect dst = {tm->a * cb.x + tm->e, tm->d * cb.y + tm->f, tm->a * cb.w, tm->d * cb.h};
    if (r->url && P->L->env.image_use && cb.w >= 1 && cb.h >= 1 && cb.w <= 8192 && cb.h <= 8192)
        P->L->env.image_use(P->L->env.ctx, r->url, au_f2px(ceilf(cb.w)), au_f2px(ceilf(cb.h)));
    const AuSurface *px = r->pixels;
    au_draw_image(P->c, px, (AuIRect){0, 0, px->width, px->height}, dst,
                  s->inhr->image_rendering == AU_IMAGE_RENDERING_PIXELATED ? AU_FILTER_NEAREST : AU_FILTER_BILINEAR,
                  opacity);
    return true;
}

static void paint_layer(PCtx *P, LBox *b, float ox, float oy)
{
    if (P->depth > L_MAX_DEPTH || (b->flags & LF_TOO_DEEP))
        return;
    if (b->kind == LK_INLINE) {
        /* ox, oy: the origin of its geometry parent: computed by layer_origin
         * as the sum up to the parent chain, which for an inline box ends at
         * the IFC root. */
        paint_inline_layer(P, b, ox, oy);
        return;
    }
    LBlock *k = (LBlock *)b;
    AuMatrix tm;
    bool affine = false;
    LArea saved = P->dirty;
    AuRect ovr;
    if (l_transform_matrix(k, &tm)) {
        if (mat_is_translation(&tm)) {
            ox += tm.e;
            oy += tm.f;
        } else {
            /* In canvas coordinates: about the box origin (ox, oy). */
            tm.e += ox - (tm.a * ox + tm.c * oy);
            tm.f += oy - (tm.b * ox + tm.d * oy);
            affine = true;
        }
    }
    ovr = (AuRect){ox + k->ov.x, oy + k->ov.y, k->ov.w, k->ov.h};
    bool cull;
    if (affine) {
        /* Cull by the transformed extent; inside, paint everything. */
        float xs[4] = {ovr.x, ovr.x + ovr.w, ovr.x + ovr.w, ovr.x};
        float ys[4] = {ovr.y, ovr.y, ovr.y + ovr.h, ovr.y + ovr.h};
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < 4; i++) {
            float X = tm.a * xs[i] + tm.c * ys[i] + tm.e, Y = tm.b * xs[i] + tm.d * ys[i] + tm.f;
            x0 = l_min(x0, X), x1 = l_max(x1, X), y0 = l_min(y0, Y), y1 = l_max(y1, Y);
        }
        cull = !rect_hits(&P->dirty, x0, y0, x1 - x0, y1 - y0);
    } else {
        cull = !rect_hits(&P->dirty, ovr.x, ovr.y, ovr.w, ovr.h);
    }
    bool sc = (b->flags & LF_STACKING) != 0;
    if (cull && (affine || !(sc && k->layers && k->layers->n)))
        return;
    P->depth++;
    float opacity = lb_anon(b) ? 1 : b->style->rare->opacity;
    bool group = opacity < 1;
    if (group && opacity <= 0) {
        P->depth--;
        return;
    }
    AuPath cpath;
    int clipped_path = clip_path_shape(k, ox, oy, &cpath);
    if (clipped_path < 0) {
        P->depth--;
        return;
    }
    if (affine && !clipped_path && paint_scaled_image(P, k, &tm, ox, oy, opacity)) {
        P->depth--;
        return;
    }
    /* The layer covers only the part of the box that the dirty area can
     * show: the dirty area mapped back through the transform, with two
     * pixels for the bilinear filter and anti-aliased edges. Painting a
     * strip of the view then paints a strip of the box, not all of it
     * (a 32-pixel scroll step over a page of transformed boxes painted
     * each of them whole). */
    AuRect need = ovr;
    AuMatrix inv;
    if (affine && au_matrix_invert(&tm, &inv)) {
        const LArea *d = &P->dirty;
        float xs[4] = {d->x, d->x + d->w, d->x + d->w, d->x};
        float ys[4] = {d->y, d->y, d->y + d->h, d->y + d->h};
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < 4; i++) {
            float X = inv.a * xs[i] + inv.c * ys[i] + inv.e, Y = inv.b * xs[i] + inv.d * ys[i] + inv.f;
            x0 = l_min(x0, X), x1 = l_max(x1, X), y0 = l_min(y0, Y), y1 = l_max(y1, Y);
        }
        x0 = l_max(x0 - 2, ovr.x), y0 = l_max(y0 - 2, ovr.y);
        x1 = l_min(x1 + 2, ovr.x + ovr.w), y1 = l_min(y1 + 2, ovr.y + ovr.h);
        if (x1 > x0 && y1 > y0)
            need = (AuRect){x0, y0, x1 - x0, y1 - y0};
    }
    if (affine && au_canvas_layer_begin(P->c, need)) {
        P->dirty = (LArea){need.x, need.y, need.w, need.h};
        group = false; /* the layer applies the opacity */
    } else {
        affine = false;
    }
    if (group)
        au_canvas_group_begin(P->c, ovr);
    bool masked = !lb_anon(b) && au_style_mask_layer_count(b->style) > 0;
    if (masked)
        au_canvas_group_begin(P->c, ovr);
    if (clipped_path)
        au_canvas_group_begin(P->c, ovr);
    if (b != P->L->root)
        paint_decor(P, k, ox, oy);
    else {
        /* The root's background was painted over the whole canvas; its
         * borders and shadows are painted here. */
        if (!lb_anon(b)) {
            AuRadii radii;
            box_radii(k, &radii, k->w, k->h);
            const AuStyleSurround *sr = b->style->surr;
            uint32_t col[4];
            for (int i = 0; i < 4; i++)
                col[i] = pc(b->style, sr->border_color[i]);
            draw_borders(P->c, (AuRect){ox, oy, k->w, k->h}, k->bw, col, sr->border_style,
                         au_radii_zero(&radii) ? NULL : &radii);
        }
    }
    if (k->b.inner == LI_REPLACED) {
        paint_replaced(P, k, ox, oy);
    } else {
        float cx, cy;
        bool clipped = push_clip(P, k, ox, oy, &cx, &cy, true);
        if (sc && k->layers)
            paint_layer_list(P, k, cx, cy, 0, k->layers->nneg);
        if (!cull) {
            paint_children(P, b, cx, cy, PH_BG);
            paint_children(P, b, cx, cy, PH_FLOAT);
            if (b->flags & LF_INLINES)
                paint_lines(P, k, cx, cy, NULL);
            paint_children(P, b, cx, cy, PH_INLINE);
        }
        if (sc && k->layers)
            paint_layer_list(P, k, cx, cy, k->layers->nneg, k->layers->n);
        pop_clip(P, k, ox, oy, clipped);
        paint_scrollbars(P, k, ox, oy);
    }
    paint_outline(P, b, (AuRect){ox, oy, k->w, k->h});
    if (clipped_path) {
        au_canvas_group_mask(P->c, &cpath);
        au_canvas_group_end(P->c, 1.0f);
        au_path_free(&cpath);
    }
    if (masked) {
        paint_mask(P, k, ox, oy, ovr);
        au_canvas_group_end(P->c, 1.0f);
    }
    if (group)
        au_canvas_group_end(P->c, opacity);
    if (affine)
        au_canvas_layer_end(P->c, &tm, opacity);
    P->dirty = saved;
    P->depth--;
}

/* ---- Entry ---- */

/* The canvas background: the root's background, or the body's when the
 * root has none (CSS 2.1 14.2). */
/* True if any background layer has an image (not "none"). */
static bool has_bg_image(const AuStyle *s)
{
    if (!s->bg->background_image)
        return false;
    size_t nl = au_style_bg_layer_count(s);
    for (size_t i = 0; i < nl; i++) {
        AuBgLayer ly;
        au_style_bg_layer(s, i, &ly);
        if (ly.image && ly.image->type != AU_CSSV_KEYWORD)
            return true;
    }
    return false;
}

static const AuStyle *canvas_style(AuLayout *L, LBlock **box)
{
    LBox *root = L->root;
    *box = (LBlock *)root;
    const AuStyle *s = root->style;
    bool none = !(s->bg->background_color & 0xFF) && !has_bg_image(s);
    if (none && root->node && au_is_html_element(root->node, AU_ATOM_html)) {
        for (LBox *c = root->first; c; c = c->next)
            if (l_is_tag(c, AU_ATOM_body) && c->kind == LK_BLOCK) {
                *box = (LBlock *)c;
                return c->style;
            }
    }
    return s;
}

void au_layout_paint(AuLayout *L, AuCanvas *c, AuRect view, float scroll_x, float scroll_y)
{
    L->need_paint = false;
    L->view_scroll_x = scroll_x;
    L->view_scroll_y = scroll_y;
    au_canvas_clip_push(c, view);
    float tx0, ty0;
    au_canvas_get_translation(c, &tx0, &ty0);
    if (!L->root) {
        if (!L->transparent)
            au_fill_rect(c, view, AU_RGB(255, 255, 255));
        au_canvas_clip_pop(c);
        return;
    }
    PCtx P;
    memset(&P, 0, sizeof P);
    P.L = L;
    P.c = c;
    P.scroll_x = scroll_x;
    P.scroll_y = scroll_y;
    P.view_x = view.x;
    P.view_y = view.y;
    /* Document point (x, y) maps to (view.x + x - scroll_x, view.y + y -
     * scroll_y) in the caller's coordinates. */
    float dx = view.x - scroll_x, dy = view.y - scroll_y;
    /* Cull against the canvas clip (the damaged part of the view), not the
     * whole view: a scroll paints only the exposed strip. */
    AuIRect cl = au_canvas_clip(c);
    P.dirty = (LArea){(float)cl.x0 - tx0, (float)cl.y0 - ty0, (float)(cl.x1 - cl.x0), (float)(cl.y1 - cl.y0)};
    if (P.dirty.w <= 0 || P.dirty.h <= 0) {
        au_canvas_clip_pop(c);
        return;
    }
    /* The whole pixels of that offset go into the canvas translation, so
     * boxes are painted at their document coordinates wherever the page
     * is scrolled to: an antialiased edge at a fractional coordinate then
     * gets the same coverage at every scroll offset, which moving pixels
     * on a scroll relies on (the canvas and the rasterizer keep whole
     * pixels of the translation apart from the coordinates). */
    float tdx = floorf(dx), tdy = floorf(dy);
    au_canvas_translate(c, tdx, tdy);
    dx -= tdx;
    dy -= tdy;
    view.x -= tdx;
    view.y -= tdy;
    P.view_x = view.x;
    P.view_y = view.y;
    P.dirty.x -= tdx;
    P.dirty.y -= tdy;
    LBlock *bgbox;
    const AuStyle *cs = canvas_style(L, &bgbox);
    uint32_t bg = pc(cs, cs->bg->background_color);
    if (!L->transparent)
        au_fill_rect(c, view, AU_RGB(255, 255, 255));
    if (bg >> 24)
        au_fill_rect(c, view, bg);
    LBlock *root = (LBlock *)L->root;
    /* Background images of the canvas: positioned against the root
     * element's box, painted over the whole canvas. */
    if (has_bg_image(cs)) {
        float bx, by;
        l_box_origin(&bgbox->b, &bx, &by);
        AuStyleBackground nobg = *cs->bg;
        (void)nobg;
        size_t nl = au_style_bg_layer_count(cs);
        for (size_t i = nl; i-- > 0;) {
            AuBgLayer ly;
            au_style_bg_layer(cs, i, &ly);
            if (!ly.image || ly.image->type == AU_CSSV_KEYWORD)
                continue;
            AuRect origin = {dx + bx, dy + by, bgbox->w, bgbox->h};
            AuRect all = {view.x, view.y, view.w, view.h};
            if (ly.attachment == AU_K_fixed)
                origin = all;
            draw_bg_image(&P, cs, &ly, ly.image->type == AU_CSSV_FUNCTION ? all : origin, all, NULL);
        }
    }
    /* The propagated background is not painted again on its own box. */
    P.canvas_box = bgbox;
    paint_layer(&P, &root->b, dx + root->x, dy + root->y);
    au_canvas_translate(c, -tdx, -tdy);
    au_canvas_clip_pop(c);
}

/* ---- Viewport-placed boxes (scrolling by moving pixels) ---- */

static void rect_union(AuRect *r, AuRect a, bool *any)
{
    if (a.w <= 0 || a.h <= 0)
        return;
    if (!*any) {
        *r = a;
        *any = true;
        return;
    }
    float x0 = l_min(r->x, a.x), y0 = l_min(r->y, a.y);
    float x1 = l_max(r->x + r->w, a.x + a.w), y1 = l_max(r->y + r->h, a.y + a.h);
    *r = (AuRect){x0, y0, x1 - x0, y1 - y0};
}

/* The bounding box of r drawn through m, a transform about (ox, oy). */
static AuRect transformed_bbox(AuRect r, float ox, float oy, const AuMatrix *m)
{
    float xs[4] = {r.x, r.x + r.w, r.x + r.w, r.x}, ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < 4; i++) {
        float lx = xs[i] - ox, ly = ys[i] - oy;
        float X = ox + m->a * lx + m->c * ly + m->e, Y = oy + m->b * lx + m->d * ly + m->f;
        x0 = l_min(x0, X), x1 = l_max(x1, X), y0 = l_min(y0, Y), y1 = l_max(y1, Y);
    }
    return (AuRect){x0, y0, x1 - x0, y1 - y0};
}

/* Whether a box below b (not b) has one of flags, or a transform. */
static bool subtree_has(LBox *b, uint32_t flags, bool transforms)
{
    LBox *d = b->first;
    int visited = 0;
    while (d && d != b) {
        if (++visited > 20000 || (d->flags & flags))
            return true;
        AuMatrix m;
        if (transforms && d->kind == LK_BLOCK && l_transform_matrix((LBlock *)d, &m))
            return true;
        if (d->first && d->kind != LK_TEXT && !(d->flags & LF_TOO_DEEP)) {
            d = d->first;
            continue;
        }
        while (d && d != b && !d->next)
            d = d->parent;
        if (d && d != b)
            d = d->next;
    }
    return false;
}

static bool is_view_box(const AuLayout *L, const LBox *b)
{
    for (uint32_t i = 0; i < L->nview_boxes; i++)
        if (L->view_boxes[i] == b)
            return true;
    return false;
}

/* A box's overflow at (x, y), with one pixel above and below for glyph
 * ink (see rect_hits); empty when the box has no area, so a box of no
 * height (a fixed toast container) adds nothing. */
static AuRect ink_rect(const LBlock *k, float x, float y)
{
    if (k->ov.w <= 0 || k->ov.h <= 0)
        return (AuRect){x, y, 0, 0};
    return (AuRect){x + k->ov.x, y + k->ov.y - 1, k->ov.w, k->ov.h + 2};
}

/* Where b and everything it paints can reach, in the frame of
 * l_box_origin (no scroll, no sticky offset), through b's own transform;
 * an empty rectangle when it paints nothing. Descendants that are view
 * boxes themselves are left out: they are damaged on their own (a fixed
 * box at the bottom inside one at the top made one rectangle the height
 * of the view between them). False when a transform elsewhere (an
 * ancestor or a descendant) makes that unknown. */
static bool view_box_extent(const AuLayout *L, LBox *b, AuRect *out)
{
    for (LBox *p = b->parent; p; p = p->parent) {
        AuMatrix m;
        if (p->kind == LK_BLOCK && l_transform_matrix((LBlock *)p, &m))
            return false;
    }
    LBlock *k = (LBlock *)b;
    float bx, by;
    l_box_origin(b, &bx, &by);
    AuRect r = ink_rect(k, bx, by);
    bool any = r.w > 0;
    /* Descendants: positioned ones are layers, outside b's overflow. A
     * transformed descendant counts with its transformed overflow when
     * nothing inside it is positioned or transformed; the content of a
     * box clipping both ways stays inside it unless something in it is
     * absolutely positioned. */
    LBox *d = b->first;
    int visited = 0;
    while (d && d != b) {
        if (++visited > 20000)
            return false;
        bool skip = false;
        if (d->kind == LK_BLOCK && is_view_box(L, d)) {
            /* damaged on its own; its subtree goes with it */
            while (d && d != b && !d->next)
                d = d->parent;
            if (d && d != b)
                d = d->next;
            continue;
        }
        if (d->kind == LK_BLOCK) {
            LBlock *dk = (LBlock *)d;
            float dx, dy;
            l_box_origin(d, &dx, &dy);
            AuRect dr = ink_rect(dk, dx, dy);
            AuMatrix m;
            if (l_transform_matrix(dk, &m)) {
                if (subtree_has(d, LF_POSITIONED | LF_ABSPOS | LF_FIXED, true))
                    return false;
                dr = transformed_bbox(dr, dx, dy, &m);
                skip = true;
            } else if ((d->flags & LF_CLIPS) && d->style->box->overflow_x != AU_OVERFLOW_VISIBLE &&
                       d->style->box->overflow_y != AU_OVERFLOW_VISIBLE &&
                       !subtree_has(d, LF_ABSPOS | LF_FIXED, false)) {
                skip = true;
            }
            rect_union(&r, dr, &any);
        }
        if (!skip && d->first && d->kind != LK_TEXT && !(d->flags & LF_TOO_DEEP)) {
            d = d->first;
            continue;
        }
        while (d && d != b && !d->next)
            d = d->parent;
        if (d && d != b)
            d = d->next;
    }
    if (!any) {
        *out = (AuRect){bx, by, 0, 0};
        return true;
    }
    AuMatrix tm;
    if (l_transform_matrix(k, &tm))
        r = transformed_bbox(r, bx, by, &tm);
    /* Anti-aliased edges. */
    out->x = r.x - 2;
    out->y = r.y - 2;
    out->w = r.w + 4;
    out->h = r.h + 4;
    return true;
}

bool l_box_paint_extent(AuLayout *L, LBox *b, AuRect *out)
{
    while (b && b->kind != LK_BLOCK) /* an inline box or text: its block */
        b = b->parent;
    if (!b)
        return false;
    for (LBox *p = b; p; p = p->parent) {
        if (p->flags & (LF_FIXED | LF_STICKY))
            return false;
        if (p != b && p->kind == LK_BLOCK && (p->flags & LF_SCROLLER))
            return false;
    }
    if (is_view_box(L, b))
        return false;
    /* A fixed or sticky descendant is left out by view_box_extent: its
     * own style change, if any, gives up above. */
    return view_box_extent(L, b, out);
}

static bool has_scroller_ancestor(const LBox *b)
{
    for (const LBox *p = b->parent; p; p = p->parent)
        if (p->kind == LK_BLOCK && (p->flags & LF_SCROLLER))
            return true;
    return false;
}

static bool add_view_rect(AuLayout *L, AuRect r, AuRect *out, int max, int *n)
{
    if (r.x >= L->vw || r.y >= L->vh || r.x + r.w <= 0 || r.y + r.h <= 0)
        return true; /* out of view */
    if (*n >= max)
        return false;
    out[(*n)++] = r;
    return true;
}

int au_layout_scroll_damage(AuLayout *L, float ox, float oy, float nx, float ny, AuRect *out, int max)
{
    if (!L->root)
        return 0;
    if (L->need_build || L->ndirty || L->need_layout || L->need_restack)
        return -1;
    /* A fixed canvas background, or a gradient over the view, moves with
     * the viewport everywhere. */
    LBlock *bgbox;
    const AuStyle *cs = canvas_style(L, &bgbox);
    if (has_bg_image(cs)) {
        size_t nl = au_style_bg_layer_count(cs);
        for (size_t i = 0; i < nl; i++) {
            AuBgLayer ly;
            au_style_bg_layer(cs, i, &ly);
            if (!ly.image || ly.image->type == AU_CSSV_KEYWORD)
                continue;
            if (ly.attachment == AU_K_fixed || ly.image->type == AU_CSSV_FUNCTION)
                return -1;
        }
    }
    if (L->nview_boxes > L_VIEW_BOXES || L->fixed_backgrounds)
        return -1;
    int n = 0;
    float dx = ox - nx, dy = oy - ny; /* how painted pixels moved */
    float save_x = L->view_scroll_x, save_y = L->view_scroll_y;
    for (uint32_t i = 0; i < L->nview_boxes && n >= 0; i++) {
        LBox *b = L->view_boxes[i];
        if (b->kind != LK_BLOCK) {
            n = -1; /* a sticky inline box */
            break;
        }
        bool sticky = (b->flags & LF_STICKY) != 0;
        if (sticky && has_scroller_ancestor(b))
            continue; /* follows its scroller, not the viewport */
        float sox = 0, soy = 0, snx = 0, sny = 0;
        if (sticky) {
            /* Not stuck, or stuck the same way, at both positions: it
             * moved with the document. */
            L->view_scroll_x = ox;
            L->view_scroll_y = oy;
            soy = l_sticky_offset(L, (LBlock *)b, &sox);
            L->view_scroll_x = nx;
            L->view_scroll_y = ny;
            sny = l_sticky_offset(L, (LBlock *)b, &snx);
            if (sox == snx && soy == sny)
                continue;
        }
        AuRect r;
        if (!view_box_extent(L, b, &r)) {
            n = -1;
            break;
        }
        if (r.w <= 0 || r.h <= 0)
            continue; /* it paints nothing */
        /* Fixed: the same viewport place before and after. Sticky: the
         * document place plus the offset, in the viewport. */
        AuRect was = r, now = r;
        if (sticky) {
            was.x += sox - ox;
            was.y += soy - oy;
            now.x += snx - nx;
            now.y += sny - ny;
        }
        was.x += dx;
        was.y += dy;
        if (!add_view_rect(L, was, out, max, &n) || !add_view_rect(L, now, out, max, &n))
            n = -1;
    }
    L->view_scroll_x = save_x;
    L->view_scroll_y = save_y;
    return n;
}

/* paint_decor skips the body background when it was propagated. */
