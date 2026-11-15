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
 * offsets and positioned-fixed adjustments applied on the way. Transforms
 * are limited to translations (translate, translateX/Y, matrix(...)
 * offsets, the translate property): the canvas has no affine transform,
 * so scale, rotate and skew are ignored. Clips are rectangles: rounded
 * overflow clipping is drawn as a square clip. */
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
        r->filter || r->mix_blend_mode != AU_MIX_BLEND_MODE_NORMAL || r->clip_path || (r->contain & AU_CONTAIN_PAINT))
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

static void sort_layers(LLayers *ls)
{
    /* Stable: insertion sort by z (lists are short; tree order kept). */
    for (uint32_t i = 1; i < ls->n; i++) {
        LBox *x = ls->items[i];
        int32_t zx = zof(x);
        uint32_t j = i;
        while (j > 0 && zof(ls->items[j - 1]) > zx) {
            ls->items[j] = ls->items[j - 1];
            j--;
        }
        ls->items[j] = x;
    }
    ls->nneg = ls->nzero = 0;
    for (uint32_t i = 0; i < ls->n; i++) {
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

static bool rect_hits(const LArea *d, float x, float y, float w, float h)
{
    return x < d->x + d->w && x + w > d->x && y < d->y + d->h && y + h > d->y;
}

static uint32_t pc(const AuStyle *s, uint32_t css)
{
    return l_paint_color(au_style_color(s, css));
}

/* ---- Transforms (translation only) ---- */

void l_transform_offset(const LBlock *b, float *dx, float *dy);
void l_transform_offset(const LBlock *b, float *dx, float *dy)
{
    *dx = *dy = 0;
    if (lb_anon(&b->b))
        return;
    const AuStyleRare *r = b->b.style->rare;
    if (r->translate) {
        uint32_t n;
        const AuCssValue *it = l_items(&r->translate->value, &n);
        for (uint32_t i = 0; i < n && i < 2; i++) {
            float v = it[i].type == AU_CSSV_PERCENT ? it[i].u.num * (i ? b->h : b->w) / 100
                                                    : it[i].type == AU_CSSV_DIMENSION ? it[i].u.num : 0;
            if (i == 0)
                *dx += v;
            else
                *dy += v;
        }
    }
    if (!r->transform)
        return;
    uint32_t n;
    const AuCssValue *it = l_items(&r->transform->value, &n);
    for (uint32_t i = 0; i < n; i++) {
        const AuCssValue *f = &it[i];
        if (f->type != AU_CSSV_FUNCTION)
            continue;
        float a[6] = {0};
        for (uint32_t k = 0; k < f->n && k < 6; k++) {
            const AuCssValue *v = &f->u.items[k];
            if (v->type == AU_CSSV_PERCENT)
                a[k] = v->u.num * ((k == 1 || f->kw == AU_K_translatey) ? b->h : b->w) / 100;
            else if (v->type == AU_CSSV_DIMENSION || v->type == AU_CSSV_NUMBER)
                a[k] = v->u.num;
        }
        switch (f->kw) {
        case AU_K_translate:
        case AU_K_translate3d:
            *dx += a[0];
            *dy += a[1];
            break;
        case AU_K_translatex: *dx += a[0]; break;
        case AU_K_translatey: *dy += a[0]; break;
        case AU_K_matrix:
            *dx += a[4];
            *dy += a[5];
            break;
        default: break; /* scale, rotate, skew: not supported by the canvas */
        }
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

static uint32_t shade(uint32_t argb, float f)
{
    uint32_t a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    if (f < 1) {
        r = (uint32_t)(r * f);
        g = (uint32_t)(g * f);
        b = (uint32_t)(b * f);
    } else {
        r = r + (uint32_t)((255 - r) * (f - 1));
        g = g + (uint32_t)((255 - g) * (f - 1));
        b = b + (uint32_t)((255 - b) * (f - 1));
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void dashed_side(AuCanvas *c, AuRect box, int side, float w, uint32_t col, bool dotted)
{
    float len = side == AU_SIDE_TOP || side == AU_SIDE_BOTTOM ? box.w : box.h;
    float dash = dotted ? w : 3 * w, gap = dotted ? w : 2 * w;
    if (dash <= 0)
        return;
    int n = (int)((len + gap) / (dash + gap));
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

/* Draws borders with widths bw, colours col (paint ARGB) and styles st. */
static void draw_borders(AuCanvas *c, AuRect box, const float *bw, const uint32_t *col, const uint8_t *st,
                         const AuRadii *radii)
{
    AuBorder solid = {{0}, {0}};
    bool any_solid = false;
    bool has_radii = radii && !au_radii_zero(radii);
    for (int i = 0; i < 4; i++) {
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
            cc = tl ? shade(cc, 0.6f) : cc;
        else if (s == AU_BORDER_STYLE_OUTSET)
            cc = tl ? cc : shade(cc, 0.6f);
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
            uint32_t dark = shade(solid.color[i], 0.6f), light = solid.color[i];
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

/* Fills r (canvas coordinates) with a linear gradient value. Radial and
 * conic gradients are approximated with the average of their colours. */
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
    au_gradient_css_line(&g, area, angle);
    float glen = sqrtf((g.x1 - g.x0) * (g.x1 - g.x0) + (g.y1 - g.y0) * (g.y1 - g.y0));
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
    if (!linear) {
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

static void draw_bg_image(PCtx *P, const AuStyle *s, const AuBgLayer *ly, AuRect origin_box, AuRect clip_box,
                          const AuRadii *radii)
{
    const AuCssValue *im = ly->image;
    if (!im)
        return;
    if (im->type == AU_CSSV_FUNCTION) {
        fill_gradient(P->c, im, origin_box, clip_box, radii, s);
        return;
    }
    if (im->type != AU_CSSV_URL || !P->L->env.image)
        return;
    char abs[2048];
    const char *base = s->bg->background_image ? s->bg->background_image->base_url : NULL;
    (void)base;
    if (!P->L->env.resolve_url || !P->L->env.resolve_url(P->L->env.ctx, im->u.str, abs, sizeof abs)) {
        size_t n = strlen(im->u.str);
        if (n >= sizeof abs)
            return;
        memcpy(abs, im->u.str, n + 1);
    }
    int iw = 0, ih = 0;
    const AuSurface *px = NULL;
    if (!P->L->env.image(P->L->env.ctx, abs, &iw, &ih, &px) || !px || iw <= 0 || ih <= 0)
        return;
    /* Tile size. */
    float tw = (float)iw, th = (float)ih;
    const AuCssValue *sz = ly->size;
    if (sz && sz->type == AU_CSSV_KEYWORD && (sz->kw == AU_K_cover || sz->kw == AU_K_contain)) {
        float sx = origin_box.w / iw, sy = origin_box.h / ih;
        float sc = sz->kw == AU_K_cover ? l_max(sx, sy) : l_min(sx, sy);
        tw = iw * sc;
        th = ih * sc;
    } else if (sz && sz->type == AU_CSSV_LIST && sz->n == 2) {
        const AuCssValue *a = &sz->u.items[0], *b = &sz->u.items[1];
        bool aa = a->type == AU_CSSV_KEYWORD, ba = b->type == AU_CSSV_KEYWORD;
        if (!aa)
            tw = bg_len(a, origin_box.w);
        if (!ba)
            th = bg_len(b, origin_box.h);
        if (aa && !ba)
            tw = th * iw / ih;
        if (ba && !aa)
            th = tw * ih / iw;
    }
    if (tw < 0.5f || th < 0.5f)
        return;
    float px0 = origin_box.x + bg_len(ly->position_x, origin_box.w - tw) * 1;
    float py0 = origin_box.y + bg_len(ly->position_y, origin_box.h - th);
    if (ly->position_x && ly->position_x->type == AU_CSSV_PERCENT)
        px0 = origin_box.x + (origin_box.w - tw) * ly->position_x->u.num / 100;
    if (ly->position_y && ly->position_y->type == AU_CSSV_PERCENT)
        py0 = origin_box.y + (origin_box.h - th) * ly->position_y->u.num / 100;
    bool rx = ly->repeat_x != AU_K_no_repeat, ry = ly->repeat_y != AU_K_no_repeat;
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
            au_draw_image(P->c, px, (AuIRect){0, 0, px->width, px->height}, (AuRect){x, y, tw, th},
                          AU_FILTER_BILINEAR, 1);
        }
    au_canvas_clip_pop(P->c);
}

static AuRect box_area(const LBlock *b, float ox, float oy, int which)
{
    AuRect r = {ox, oy, b->w, b->h};
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
        AuRect r = box_area(b, border_box.x, border_box.y, last.clip);
        if (rounded) {
            AuRadii rr = radii;
            float in[4] = {0};
            if (last.clip != AU_K_border_box)
                for (int i = 0; i < 4; i++)
                    in[i] = b->bw[i] + (last.clip == AU_K_content_box ? b->p[i] : 0);
            inset_radii(&rr, in);
            au_fill_rounded_rect(P->c, r, &rr, bgc);
        } else {
            au_fill_rect(P->c, r, bgc);
        }
    }
    for (size_t i = nl; i-- > 0;) {
        AuBgLayer ly;
        au_style_bg_layer(s, i, &ly);
        if (!ly.image || (ly.image->type == AU_CSSV_KEYWORD))
            continue;
        AuRect clip = box_area(b, border_box.x, border_box.y, ly.clip);
        AuRect origin = box_area(b, border_box.x, border_box.y, ly.origin);
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

/* Background, shadows and borders of a block box at (ox, oy). */
static void paint_decor(PCtx *P, LBlock *b, float ox, float oy)
{
    if (lb_anon(&b->b) || (b->b.flags & LF_HIDDEN))
        return;
    const AuStyle *s = b->b.style;
    if (b->b.inner == LI_TABLE_CELL && s->inhr->empty_cells == AU_EMPTY_CELLS_HIDE && !b->b.first)
        return;
    AuRect box = {ox, oy, b->w, b->h};
    if (!rect_hits(&P->dirty, ox + b->ov.x, oy + b->ov.y, b->ov.w, b->ov.h))
        return;
    AuRadii radii;
    box_radii(b, &radii, b->w, b->h);
    bool rounded = !au_radii_zero(&radii);
    paint_shadows(P, s, box, rounded ? &radii : NULL, false);
    if (b != (LBlock *)P->L->root && b != P->canvas_box)
        paint_background(P, s, b, box, false);
    paint_shadows(P, s, box, rounded ? &radii : NULL, true);
    const AuStyleSurround *sr = s->surr;
    uint32_t col[4];
    for (int i = 0; i < 4; i++)
        col[i] = pc(s, sr->border_color[i]);
    draw_borders(P->c, box, b->bw, col, sr->border_style, rounded ? &radii : NULL);
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

static void paint_label(PCtx *P, LBlock *b, const char *t, AuRect area, uint32_t color, int align)
{
    if (!t || !*t)
        return;
    LFont *f = l_font(P->L, b->b.style);
    size_t n = strlen(t);
    float w = l_measure(P->L, f, b->b.style, t, n);
    float x = area.x;
    if (align == 1)
        x = area.x + (area.w - w) / 2;
    float y = area.y + (area.h - (f->m.ascent + f->m.descent)) / 2 + f->m.ascent;
    au_canvas_clip_push(P->c, area);
    l_draw_text(P->L, P->c, f, b->b.style, t, n, x, y, color, 0);
    au_canvas_clip_pop(P->c);
}

static void paint_replaced(PCtx *P, LBlock *b, float ox, float oy)
{
    if (b->b.flags & LF_HIDDEN)
        return;
    AuRect cb = box_area(b, ox, oy, AU_K_content_box);
    const AuStyle *s = b->b.style;
    uint32_t color = pc(s, s->inh->color);
    LReplaced *r = b->rep;
    switch (b->b.replaced) {
    case LR_IMAGE:
        if (r && r->pixels) {
            const AuSurface *px = r->pixels;
            au_draw_image(P->c, px, (AuIRect){0, 0, px->width, px->height}, cb,
                          s->inhr->image_rendering == AU_IMAGE_RENDERING_PIXELATED ? AU_FILTER_NEAREST
                                                                                 : AU_FILTER_BILINEAR,
                          1);
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
                paint_label(P, b, r->label, (AuRect){cb.x + 2, cb.y, cb.w - 4, cb.h}, color, 0);
        }
        break;
    case LR_TEXT_INPUT: {
        bool placeholder = !l_attr(&b->b, au_atom_lookup("value", 5)) || !*l_attr(&b->b, au_atom_lookup("value", 5));
        paint_label(P, b, r ? r->label : NULL, cb, placeholder ? AU_RGB(0x75, 0x75, 0x75) : color, 0);
        break;
    }
    case LR_BUTTON:
        paint_label(P, b, r ? r->label : NULL, cb, color, 1);
        break;
    case LR_SELECT: {
        float aw = s->inh->font_size * 1.2f;
        paint_label(P, b, r ? r->label : NULL, (AuRect){cb.x, cb.y, cb.w - aw, cb.h}, color, 0);
        AuPath p;
        au_path_init(&p);
        float cx = cb.x + cb.w - aw / 2, cy = cb.y + cb.h / 2, a = s->inh->font_size * 0.25f;
        au_path_move_to(&p, cx - a, cy - a / 2);
        au_path_line_to(&p, cx + a, cy - a / 2);
        au_path_line_to(&p, cx, cy + a / 2);
        au_path_close(&p);
        au_fill_path(P->c, &p, AU_FILL_NONZERO, color);
        au_path_free(&p);
        break;
    }
    case LR_TEXTAREA:
        if (r && r->label) {
            LFont *f = l_font(P->L, s);
            float lh = l_line_height(s, f);
            const char *t = r->label;
            float y = cb.y + f->m.ascent + (lh - f->m.ascent - f->m.descent) / 2;
            au_canvas_clip_push(P->c, cb);
            while (*t && y < cb.y + cb.h + lh) {
                const char *e = strchr(t, '\n');
                size_t n = e ? (size_t)(e - t) : strlen(t);
                l_draw_text(P->L, P->c, f, s, t, n, cb.x, y, color, 0);
                y += lh;
                t += n + (e ? 1 : 0);
            }
            au_canvas_clip_pop(P->c);
        }
        break;
    case LR_CHECKBOX:
    case LR_RADIO: {
        AuRect bx = {ox, oy, b->w, b->h};
        bool checked = b->b.node && au_is_element(b->b.node) &&
                       ((au_as_element(b->b.node)->state & AU_STATE_CHECKED_SET)
                            ? (au_as_element(b->b.node)->state & AU_STATE_CHECKED) != 0
                            : l_attr(&b->b, au_atom_lookup("checked", 7)) != NULL);
        uint32_t edge = AU_RGB(0x76, 0x76, 0x76);
        if (b->b.replaced == LR_RADIO) {
            AuPath p;
            au_path_init(&p);
            float rr = l_min(bx.w, bx.h) / 2;
            au_path_ellipse(&p, bx.x + bx.w / 2, bx.y + bx.h / 2, rr, rr);
            au_fill_path(P->c, &p, AU_FILL_NONZERO, AU_RGB(255, 255, 255));
            AuPath st;
            au_path_init(&st);
            au_path_stroke(&p, 1, &st);
            au_fill_path(P->c, &st, AU_FILL_NONZERO, edge);
            au_path_free(&st);
            if (checked) {
                au_path_clear(&p);
                au_path_ellipse(&p, bx.x + bx.w / 2, bx.y + bx.h / 2, rr * 0.45f, rr * 0.45f);
                au_fill_path(P->c, &p, AU_FILL_NONZERO, AU_RGB(0x10, 0x60, 0xE0));
            }
            au_path_free(&p);
        } else {
            AuRadii rr = {{2, 2, 2, 2}, {2, 2, 2, 2}};
            au_fill_rounded_rect(P->c, bx, &rr, checked ? AU_RGB(0x10, 0x60, 0xE0) : AU_RGB(255, 255, 255));
            float w1[4] = {1, 1, 1, 1};
            uint32_t es[4] = {edge, edge, edge, edge};
            uint8_t st[4] = {AU_BORDER_STYLE_SOLID, AU_BORDER_STYLE_SOLID, AU_BORDER_STYLE_SOLID, AU_BORDER_STYLE_SOLID};
            if (!checked)
                draw_borders(P->c, bx, w1, es, st, &rr);
            if (checked) {
                AuPath p, sp;
                au_path_init(&p);
                au_path_init(&sp);
                au_path_move_to(&p, bx.x + bx.w * 0.22f, bx.y + bx.h * 0.52f);
                au_path_line_to(&p, bx.x + bx.w * 0.42f, bx.y + bx.h * 0.72f);
                au_path_line_to(&p, bx.x + bx.w * 0.78f, bx.y + bx.h * 0.3f);
                au_path_stroke(&p, l_max(1.5f, bx.w / 7), &sp);
                au_fill_path(P->c, &sp, AU_FILL_NONZERO, AU_RGB(255, 255, 255));
                au_path_free(&p);
                au_path_free(&sp);
            }
        }
        break;
    }
    case LR_RANGE: {
        float cy = cb.y + cb.h / 2;
        au_fill_rect(P->c, (AuRect){cb.x, cy - 2, cb.w, 4}, AU_RGB(0xC0, 0xC0, 0xC0));
        AuPath p;
        au_path_init(&p);
        au_path_ellipse(&p, cb.x + cb.w / 2, cy, 7, 7);
        au_fill_path(P->c, &p, AU_FILL_NONZERO, AU_RGB(0x10, 0x60, 0xE0));
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
        if (l_is_tag(&b->b, AU_ATOM_meter) || l_is_tag(&b->b, AU_ATOM_progress)) {
            au_fill_rect(P->c, cb, AU_RGB(0xE0, 0xE0, 0xE0));
            AuAtom va = au_atom_lookup("value", 5), ma = au_atom_lookup("max", 3);
            const char *v = va ? l_attr(&b->b, va) : NULL, *m = ma ? l_attr(&b->b, ma) : NULL;
            float fv = v ? (float)atof(v) : 0, fm = m ? (float)atof(m) : 1;
            if (fm <= 0)
                fm = 1;
            float frac = l_max(0, l_min(1, fv / fm));
            au_fill_rect(P->c, (AuRect){cb.x, cb.y, cb.w * frac, cb.h}, AU_RGB(0x10, 0x60, 0xE0));
        }
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
    int nd = collect_decos(f->box, d, 8);
    for (int i = 0; i < nd; i++) {
        float th = d[i].thickness > 0 ? d[i].thickness : l_max(1, roundf(font->m.underline_thickness));
        float w = f->w;
        if (d[i].lines & AU_TDL_UNDERLINE) {
            float y = roundf(base + l_max(1, font->m.underline_pos));
            au_fill_rect(P->c, (AuRect){x, y, w, th}, d[i].color);
            if (d[i].style == AU_TEXT_DECORATION_STYLE_DOUBLE)
                au_fill_rect(P->c, (AuRect){x, y + th * 2, w, th}, d[i].color);
        }
        if (d[i].lines & AU_TDL_OVERLINE)
            au_fill_rect(P->c, (AuRect){x, roundf(base - font->m.ascent), w, th}, d[i].color);
        if (d[i].lines & AU_TDL_LINE_THROUGH)
            au_fill_rect(P->c, (AuRect){x, roundf(base - font->m.strike_pos - th / 2), w, th}, d[i].color);
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

/* Pushes b's overflow clip; returns the origin offset for its content. */
static bool push_clip(PCtx *P, LBlock *b, float ox, float oy, float *cx, float *cy)
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
    *cx = ox - b->scroll_x;
    *cy = oy - b->scroll_y;
    return true;
}

static void paint_scrollbars(PCtx *P, LBlock *b, float ox, float oy)
{
    if (!(b->b.flags & LF_SCROLLER) || (b->b.flags & LF_HIDDEN))
        return;
    AuRect pb = box_area(b, ox, oy, AU_K_padding_box);
    const AuStyleBox *s = b->b.style->box;
    float vw = pb.w, vh = pb.h;
    bool vbar = b->sh > vh + 0.5f || s->overflow_y == AU_OVERFLOW_SCROLL;
    bool hbar = b->sw > vw + 0.5f || s->overflow_x == AU_OVERFLOW_SCROLL;
    if (vbar && s->overflow_y != AU_OVERFLOW_HIDDEN && s->overflow_y != AU_OVERFLOW_CLIP && vh > 8) {
        float th = l_max(16, vh * vh / l_max(vh, b->sh));
        float ty = pb.y + (vh - th) * (b->sh > vh ? b->scroll_y / (b->sh - vh) : 0);
        au_fill_rect(P->c, (AuRect){pb.x + pb.w - 8, pb.y, 8, vh}, AU_RGBA(0, 0, 0, 20));
        AuRadii r = {{3, 3, 3, 3}, {3, 3, 3, 3}};
        au_fill_rounded_rect(P->c, (AuRect){pb.x + pb.w - 7, ty + 1, 6, th - 2}, &r, AU_RGBA(0, 0, 0, 110));
    }
    if (hbar && s->overflow_x != AU_OVERFLOW_HIDDEN && s->overflow_x != AU_OVERFLOW_CLIP && vw > 8) {
        float tw = l_max(16, vw * vw / l_max(vw, b->sw));
        float tx = pb.x + (vw - tw) * (b->sw > vw ? b->scroll_x / (b->sw - vw) : 0);
        au_fill_rect(P->c, (AuRect){pb.x, pb.y + pb.h - 8, vw, 8}, AU_RGBA(0, 0, 0, 20));
        AuRadii r = {{3, 3, 3, 3}, {3, 3, 3, 3}};
        au_fill_rounded_rect(P->c, (AuRect){tx + 1, pb.y + pb.h - 7, tw - 2, 6}, &r, AU_RGBA(0, 0, 0, 110));
    }
}

/* Descendant walk over b's children for one phase. */
static void paint_children(PCtx *P, LBox *parent, float ox, float oy, int phase)
{
    for (LBox *c = parent->first; c; c = c->next) {
        if (c->kind == LK_INLINE) {
            if (phase == PH_FLOAT && !(c->flags & LF_LAYERED))
                paint_children(P, c, ox, oy, phase);
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
    bool clipped = push_clip(P, b, ox, oy, &cx, &cy);
    if (phase == PH_INLINE && (b->b.flags & LF_INLINES))
        paint_lines(P, b, cx, cy, NULL);
    paint_children(P, &b->b, cx, cy, phase);
    if (clipped)
        au_canvas_clip_pop(P->c);
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
    bool clipped = push_clip(P, k, ox, oy, &cx, &cy);
    paint_children(P, &k->b, cx, cy, PH_BG);
    paint_children(P, &k->b, cx, cy, PH_FLOAT);
    if (k->b.flags & LF_INLINES)
        paint_lines(P, k, cx, cy, NULL);
    paint_children(P, &k->b, cx, cy, PH_INLINE);
    if (clipped)
        au_canvas_clip_pop(P->c);
    paint_scrollbars(P, k, ox, oy);
    paint_outline(P, &k->b, (AuRect){ox, oy, k->w, k->h});
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

/* Origin of layered box b in canvas coordinates, given its stacking
 * context sc at (sx, sy); pushes the clips of the ancestors that clip it
 * and returns how many were pushed. */
static int layer_origin(PCtx *P, LBox *sc, float sx, float sy, LBox *b, float *ox, float *oy)
{
    LBox *chain[L_MAX_DEPTH + 1];
    int n = 0;
    for (LBox *p = b->parent; p && p != sc && n < L_MAX_DEPTH; p = p->parent)
        chain[n++] = p;
    float x = sx, y = sy;
    int pushed = 0;
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
    for (int i = n; i-- > 0;) {
        LBox *a = chain[i];
        if (a->kind != LK_BLOCK)
            continue;
        LBlock *k = (LBlock *)a;
        x += k->x;
        y += k->y;
        if ((a->flags & LF_STICKY) && !(a->flags & LF_LAYERED)) {
            /* not reached: sticky boxes are layers */
        }
        if (applies[i] && (a->flags & LF_CLIPS)) {
            float cx, cy;
            push_clip(P, k, x, y, &cx, &cy);
            pushed++;
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
    }
    *ox = x;
    *oy = y;
    return pushed;
}

static void paint_layer_list(PCtx *P, LBlock *sc, float sx, float sy, uint32_t from, uint32_t to)
{
    LLayers *ls = sc->layers;
    for (uint32_t i = from; i < to; i++) {
        LBox *b = ls->items[i];
        float ox, oy;
        int pushed = layer_origin(P, &sc->b, sx, sy, b, &ox, &oy);
        paint_layer(P, b, ox, oy);
        while (pushed--)
            au_canvas_clip_pop(P->c);
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
    float tdx, tdy;
    l_transform_offset(k, &tdx, &tdy);
    ox += tdx;
    oy += tdy;
    LArea saved = P->dirty;
    bool cull = !rect_hits(&P->dirty, ox + k->ov.x, oy + k->ov.y, k->ov.w, k->ov.h);
    bool sc = (b->flags & LF_STACKING) != 0;
    if (cull && !(sc && k->layers && k->layers->n))
        return;
    P->depth++;
    float opacity = lb_anon(b) ? 1 : b->style->rare->opacity;
    bool group = opacity < 1;
    if (group && opacity <= 0) {
        P->depth--;
        return;
    }
    if (group)
        au_canvas_group_begin(P->c, (AuRect){ox + k->ov.x, oy + k->ov.y, k->ov.w, k->ov.h});
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
        bool clipped = push_clip(P, k, ox, oy, &cx, &cy);
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
        if (clipped)
            au_canvas_clip_pop(P->c);
        paint_scrollbars(P, k, ox, oy);
    }
    paint_outline(P, b, (AuRect){ox, oy, k->w, k->h});
    if (group)
        au_canvas_group_end(P->c, opacity);
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
    /* Canvas coordinates: document point (x, y) maps to (view.x + x -
     * scroll_x, view.y + y - scroll_y). */
    float dx = view.x - scroll_x, dy = view.y - scroll_y;
    P.dirty = (LArea){view.x, view.y, view.w, view.h};
    LBlock *bgbox;
    const AuStyle *cs = canvas_style(L, &bgbox);
    uint32_t bg = pc(cs, cs->bg->background_color);
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
    au_canvas_clip_pop(c);
    (void)tx0;
    (void)ty0;
}

/* paint_decor skips the body background when it was propagated. */
