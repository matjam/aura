/* The canvas: translation, clip stack, opacity groups, and the fill and
 * image operations. Borders and shadows are in border.c. */
#include "paint_internal.h"

#include <math.h>
#include <string.h>

typedef struct Group {
    AuSurface *surface; /* the group's pixels */
    int ox, oy;         /* canvas pixel of the surface's top-left */
    size_t clip_depth;  /* clip stack depth at begin */
} Group;

struct AuCanvas {
    AuSurface *base;
    float tx, ty;
    AuIRect *clips; /* in base-surface pixels; clips[0] is the surface */
    size_t nclips, clip_cap;
    Group *groups;
    size_t ngroups, group_cap;
};

AuCanvas *au_canvas_new(AuSurface *target)
{
    AuCanvas *c = au_calloc(sizeof *c);
    c->base = target;
    c->clip_cap = 8;
    c->clips = au_alloc(c->clip_cap * sizeof(AuIRect));
    AuIRect all = {0, 0, target->width, target->height};
    c->clips[0] = all;
    c->nclips = 1;
    return c;
}

void au_canvas_free(AuCanvas *c)
{
    if (!c)
        return;
    while (c->ngroups)
        au_canvas_group_end(c, 1.0f);
    au_free(c->clips);
    au_free(c->groups);
    au_free(c);
}

AuSurface *au_canvas_target(AuCanvas *c)
{
    return c->base;
}

void au_canvas_translate(AuCanvas *c, float dx, float dy)
{
    c->tx += dx;
    c->ty += dy;
}

void au_canvas_get_translation(const AuCanvas *c, float *dx, float *dy)
{
    *dx = c->tx;
    *dy = c->ty;
}

static int round_px(float v)
{
    if (v > 1e8f)
        return 100000000;
    if (v < -1e8f)
        return -100000000;
    return au_f2px(floorf(v + 0.5f));
}

void au_canvas_clip_push(AuCanvas *c, AuRect r)
{
    AuIRect ir = {round_px(r.x + c->tx), round_px(r.y + c->ty), round_px(r.x + r.w + c->tx),
                  round_px(r.y + r.h + c->ty)};
    if (c->nclips == c->clip_cap) {
        c->clip_cap *= 2;
        c->clips = au_realloc(c->clips, c->clip_cap * sizeof(AuIRect));
    }
    c->clips[c->nclips] = au_irect_intersect(c->clips[c->nclips - 1], ir);
    c->nclips++;
}

void au_canvas_clip_pop(AuCanvas *c)
{
    if (c->nclips > 1)
        c->nclips--;
}

AuIRect au_canvas_clip(const AuCanvas *c)
{
    return c->clips[c->nclips - 1];
}

AuSurface *au_canvas_current(AuCanvas *c, int *ox, int *oy, AuIRect *clip)
{
    AuIRect cl = c->clips[c->nclips - 1];
    if (c->ngroups) {
        Group *g = &c->groups[c->ngroups - 1];
        *ox = g->ox;
        *oy = g->oy;
        AuIRect gr = {g->ox, g->oy, g->ox + g->surface->width, g->oy + g->surface->height};
        cl = au_irect_intersect(cl, gr);
        cl.x0 -= g->ox;
        cl.x1 -= g->ox;
        cl.y0 -= g->oy;
        cl.y1 -= g->oy;
        *clip = cl;
        return g->surface;
    }
    *ox = *oy = 0;
    *clip = cl;
    return c->base;
}

/* ---- Groups ---- */

void au_canvas_group_begin(AuCanvas *c, AuRect bounds)
{
    AuIRect b = {au_f2px(floorf(bounds.x + c->tx)), au_f2px(floorf(bounds.y + c->ty)),
                 au_f2px(ceilf(bounds.x + bounds.w + c->tx)), au_f2px(ceilf(bounds.y + bounds.h + c->ty))};
    b = au_irect_intersect(b, c->clips[c->nclips - 1]);
    if (c->ngroups) {
        Group *pg = &c->groups[c->ngroups - 1];
        AuIRect pr = {pg->ox, pg->oy, pg->ox + pg->surface->width, pg->oy + pg->surface->height};
        b = au_irect_intersect(b, pr);
    }
    if (c->ngroups == c->group_cap) {
        c->group_cap = c->group_cap ? c->group_cap * 2 : 4;
        c->groups = au_realloc(c->groups, c->group_cap * sizeof(Group));
    }
    Group *g = &c->groups[c->ngroups++];
    g->surface = au_surface_new(b.x1 - b.x0, b.y1 - b.y0);
    g->ox = b.x0;
    g->oy = b.y0;
    g->clip_depth = c->nclips;
}

void au_canvas_group_end(AuCanvas *c, float opacity)
{
    if (!c->ngroups)
        return;
    Group g = c->groups[--c->ngroups];
    c->nclips = g.clip_depth;
    if (opacity > 1)
        opacity = 1;
    uint32_t a = opacity > 0 ? (uint32_t)(opacity * 256.0f + 0.5f) : 0;
    int ox, oy;
    AuIRect clip;
    AuSurface *dst = au_canvas_current(c, &ox, &oy, &clip);
    /* The group surface sits at canvas pixel (g.ox, g.oy); in dst pixels
     * that is (g.ox - ox, g.oy - oy). */
    int gx = g.ox - ox, gy = g.oy - oy;
    AuIRect r = {gx, gy, gx + g.surface->width, gy + g.surface->height};
    r = au_irect_intersect(r, clip);
    if (a && !au_irect_empty(r)) {
        for (int y = r.y0; y < r.y1; y++) {
            const AuPixel *src = g.surface->pixels + (size_t)(y - gy) * g.surface->stride + (r.x0 - gx);
            AuPixel *d = dst->pixels + (size_t)y * dst->stride + r.x0;
            int n = r.x1 - r.x0;
            for (int i = 0; i < n; i++) {
                uint32_t s = src[i];
                if (!s)
                    continue;
                if (a < 256)
                    s = au_px_scale(s, a);
                d[i] = au_px_over(s, d[i]);
            }
        }
    }
    au_surface_free(g.surface);
}

void au_canvas_mask_end(AuCanvas *c, bool luminance)
{
    if (c->ngroups < 2) {
        au_canvas_group_end(c, 1.0f);
        return;
    }
    Group mg = c->groups[--c->ngroups];
    c->nclips = mg.clip_depth;
    Group *g = &c->groups[c->ngroups - 1];
    AuSurface *s = g->surface;
    for (int y = 0; y < s->height; y++) {
        AuPixel *row = s->pixels + (size_t)y * s->stride;
        int my = y + g->oy - mg.oy;
        bool in_y = my >= 0 && my < mg.surface->height;
        const AuPixel *mrow = in_y ? mg.surface->pixels + (size_t)my * mg.surface->stride : NULL;
        for (int x = 0; x < s->width; x++) {
            if (!row[x])
                continue;
            int mx = x + g->ox - mg.ox;
            uint32_t m = 0;
            if (mrow && mx >= 0 && mx < mg.surface->width) {
                uint32_t p = mrow[mx];
                if (luminance) {
                    /* Premultiplied channels: luminance times alpha. */
                    uint32_t r = (p >> 16) & 255, gg = (p >> 8) & 255, b = p & 255;
                    m = (r * 13933u + gg * 46884u + b * 4725u + 32768u) >> 16; /* 0.2125, 0.7154, 0.0721 */
                    if (m > 255)
                        m = 255;
                } else {
                    m = p >> 24;
                }
            }
            if (m == 255)
                continue;
            row[x] = m ? au_px_scale(row[x], au_a256(m)) : 0;
        }
    }
    au_surface_free(mg.surface);
}

/* ---- Transformed layers ---- */

bool au_canvas_layer_begin(AuCanvas *c, AuRect bounds)
{
    float x0 = bounds.x + c->tx, y0 = bounds.y + c->ty;
    float x1 = x0 + bounds.w, y1 = y0 + bounds.h;
    /* Written so that NaN fails. */
    if (!(x0 > -1e6f && y0 > -1e6f && x1 < 1e6f && y1 < 1e6f && bounds.w > 0 && bounds.h > 0))
        return false;
    AuIRect b = {au_f2px(floorf(x0)), au_f2px(floorf(y0)), au_f2px(ceilf(x1)), au_f2px(ceilf(y1))};
    if ((double)(b.x1 - b.x0) * (b.y1 - b.y0) > 4.0 * 1024 * 1024)
        return false;
    if (c->ngroups == c->group_cap) {
        c->group_cap = c->group_cap ? c->group_cap * 2 : 4;
        c->groups = au_realloc(c->groups, c->group_cap * sizeof(Group));
    }
    Group *g = &c->groups[c->ngroups++];
    g->surface = au_surface_new(b.x1 - b.x0, b.y1 - b.y0);
    g->ox = b.x0;
    g->oy = b.y0;
    g->clip_depth = c->nclips;
    /* The layer's own clip: its whole surface, not the clip outside. */
    if (c->nclips == c->clip_cap) {
        c->clip_cap *= 2;
        c->clips = au_realloc(c->clips, c->clip_cap * sizeof(AuIRect));
    }
    c->clips[c->nclips++] = b;
    return true;
}

/* The premultiplied pixel of s at (x, y), transparent outside. */
static uint32_t px_at(const AuSurface *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->width || y >= s->height)
        return 0;
    return s->pixels[(size_t)y * s->stride + x];
}

void au_canvas_layer_end(AuCanvas *c, const AuMatrix *m, float opacity)
{
    if (!c->ngroups)
        return;
    Group g = c->groups[--c->ngroups];
    c->nclips = g.clip_depth;
    if (opacity > 1)
        opacity = 1;
    uint32_t alpha = opacity > 0 ? (uint32_t)(opacity * 256.0f + 0.5f) : 0;
    /* The matrix in canvas pixels: undo the translation, map, redo it. */
    float a = m->a, b = m->b, cc = m->c, d = m->d;
    float e = m->e + c->tx - (a * c->tx + cc * c->ty);
    float f = m->f + c->ty - (b * c->tx + d * c->ty);
    float det = a * d - b * cc;
    int ox, oy;
    AuIRect clip;
    AuSurface *dst = au_canvas_current(c, &ox, &oy, &clip);
    if (alpha && (det > 1e-6f || det < -1e-6f)) {
        float ia = d / det, ib = -b / det, ic = -cc / det, id = a / det;
        float ie = (cc * f - d * e) / det, iff = (b * e - a * f) / det;
        /* The destination extent: the corners of the layer mapped. */
        float gx0 = (float)g.ox, gy0 = (float)g.oy;
        float gx1 = gx0 + g.surface->width, gy1 = gy0 + g.surface->height;
        float xs[4] = {gx0, gx1, gx1, gx0}, ys[4] = {gy0, gy0, gy1, gy1};
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int i = 0; i < 4; i++) {
            float X = a * xs[i] + cc * ys[i] + e, Y = b * xs[i] + d * ys[i] + f;
            minx = X < minx ? X : minx;
            maxx = X > maxx ? X : maxx;
            miny = Y < miny ? Y : miny;
            maxy = Y > maxy ? Y : maxy;
        }
        if (minx < -1e6f || miny < -1e6f || maxx > 1e6f || maxy > 1e6f)
            minx = miny = maxx = maxy = 0; /* absurd: draw nothing */
        AuIRect r = {au_f2px(floorf(minx)) - ox, au_f2px(floorf(miny)) - oy, au_f2px(ceilf(maxx)) - ox + 1,
                     au_f2px(ceilf(maxy)) - oy + 1};
        r = au_irect_intersect(r, clip);
        const AuSurface *gs = g.surface;
        const int gw = gs->width, gh = gs->height;
        /* The source position steps by (ia, ib) per destination pixel:
         * in 32.32 fixed point, which a G4 adds far faster than it
         * multiplies floats (layer_end was 58% of Reddit's paint time on
         * the host). 32 bits of fraction keep a long row from drifting. */
        const double one = 4294967296.0;
        const int64_t dxs = (int64_t)((double)ia * one), dys = (int64_t)((double)ib * one);
        const int64_t wlim = (int64_t)gw << 32, hlim = (int64_t)gh << 32, neg1 = -((int64_t)1 << 32);
        for (int y = r.y0; y < r.y1; y++) {
            AuPixel *row = dst->pixels + (size_t)y * dst->stride;
            double Y = (double)(y + oy) + 0.5;
            double X0 = (double)(r.x0 + ox) + 0.5;
            /* The source position, with pixel centres at .5. */
            int64_t fsx = (int64_t)floor(((double)ia * X0 + (double)ic * Y + (double)ie - gx0 - 0.5) * one);
            int64_t fsy = (int64_t)floor(((double)ib * X0 + (double)id * Y + (double)iff - gy0 - 0.5) * one);
            for (int x = r.x0; x < r.x1; x++, fsx += dxs, fsy += dys) {
                if (fsx <= neg1 || fsy <= neg1 || fsx >= wlim || fsy >= hlim)
                    continue;
                int x0 = (int)(fsx >> 32), y0 = (int)(fsy >> 32); /* floor: arithmetic shift */
                uint32_t fx = (uint32_t)(fsx >> 24) & 255, fy = (uint32_t)(fsy >> 24) & 255;
                uint32_t p00, p10, p01, p11;
                if (x0 >= 0 && y0 >= 0 && x0 + 1 < gw && y0 + 1 < gh) {
                    const AuPixel *s0 = gs->pixels + (size_t)y0 * gs->stride + x0;
                    p00 = s0[0];
                    p10 = s0[1];
                    p01 = s0[gs->stride];
                    p11 = s0[gs->stride + 1];
                } else {
                    p00 = px_at(gs, x0, y0);
                    p10 = px_at(gs, x0 + 1, y0);
                    p01 = px_at(gs, x0, y0 + 1);
                    p11 = px_at(gs, x0 + 1, y0 + 1);
                }
                if (!(p00 | p10 | p01 | p11))
                    continue;
                uint32_t top = au_px_scale_trunc(p00, 256 - fx) + au_px_scale_trunc(p10, fx);
                uint32_t bot = au_px_scale_trunc(p01, 256 - fx) + au_px_scale_trunc(p11, fx);
                uint32_t s = au_px_scale_trunc(top, 256 - fy) + au_px_scale_trunc(bot, fy);
                if (alpha < 256)
                    s = au_px_scale(s, alpha);
                if (s)
                    row[x] = au_px_over(s, row[x]);
            }
        }
    }
    au_surface_free(g.surface);
}

typedef struct {
    uint8_t *mask;
    int x0, y0, w;
} MaskCtx;

static void mask_span(void *ctx, int y, int x, int n, const uint8_t *cov)
{
    MaskCtx *m = ctx;
    memcpy(m->mask + (size_t)(y - m->y0) * m->w + (x - m->x0), cov, (size_t)n);
}

void au_canvas_group_mask(AuCanvas *c, const AuPath *p)
{
    if (!c->ngroups)
        return;
    Group *g = &c->groups[c->ngroups - 1];
    int w = g->surface->width, h = g->surface->height;
    if (w <= 0 || h <= 0)
        return;
    MaskCtx m = {au_calloc((size_t)w * h), g->ox, g->oy, w};
    AuIRect area = {g->ox, g->oy, g->ox + w, g->oy + h};
    au_raster_path(p, c->tx, c->ty, AU_FILL_NONZERO, area, mask_span, &m);
    for (int y = 0; y < h; y++) {
        AuPixel *row = g->surface->pixels + (size_t)y * g->surface->stride;
        const uint8_t *mr = m.mask + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t cv = mr[x];
            if (cv == 255 || !row[x])
                continue;
            row[x] = cv ? au_px_scale(row[x], cv + (cv >> 7)) : 0;
        }
    }
    au_free(m.mask);
}

/* ---- Rectangles ----
 * A rectangle is filled directly, without the path rasterizer: each row
 * gets a coverage factor for its vertical overlap, and the two edge
 * columns get their horizontal overlap. */

typedef struct {
    int kind; /* 0 solid, 1 gradient */
    uint32_t color;
    AuGradientLut *lut;
} Source;

static void blend_row(AuSurface *s, int x, int y, int n, const Source *src, const uint8_t *cov)
{
    AuPixel *row = s->pixels + (size_t)y * s->stride + x;
    if (src->kind == 0)
        au_blend_solid_span(row, n, src->color, cov);
    else
        au_blend_gradient_span(row, x, y, n, src->lut, cov);
}

static void fill_rect_source(AuCanvas *c, AuRect r, const Source *src)
{
    if (!(r.w > 0 && r.h > 0))
        return;
    int ox, oy;
    AuIRect clip;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    /* The whole pixels of the translation are added to the integer
     * bounds, not to the coordinates: the coverage of a fractional edge
     * is then the same wherever the rectangle is translated to (see
     * raster()). */
    float tx = c->tx - (float)ox, ty = c->ty - (float)oy;
    int ix = au_f2px(floorf(tx)), iy = au_f2px(floorf(ty));
    float x0 = r.x + (tx - (float)ix), y0 = r.y + (ty - (float)iy);
    float x1 = x0 + r.w, y1 = y0 + r.h;
    if (x0 != x0 || y0 != y0 || x1 != x1 || y1 != y1)
        return; /* not numbers: the coverage below would not be either */
    if (x0 < -1e7f)
        x0 = -1e7f;
    if (y0 < -1e7f)
        y0 = -1e7f;
    if (x1 > 1e7f)
        x1 = 1e7f;
    if (y1 > 1e7f)
        y1 = 1e7f;
    /* ir is in surface pixels; x0..y1 are ix, iy short of them. */
    clip.x0 -= ix;
    clip.x1 -= ix;
    clip.y0 -= iy;
    clip.y1 -= iy;
    AuIRect ir = {au_f2px(floorf(x0)), au_f2px(floorf(y0)), au_f2px(ceilf(x1)), au_f2px(ceilf(y1))};
    ir = au_irect_intersect(ir, clip);
    if (au_irect_empty(ir))
        return;
    int n = ir.x1 - ir.x0;
    uint8_t stackbuf[512];
    uint8_t *cov = n <= (int)sizeof stackbuf ? stackbuf : au_alloc((size_t)n);
    /* Horizontal coverage of the first and last columns. */
    float lc = 1.0f - (x0 - floorf(x0)); /* left column, if partial */
    float rc = x1 - floorf(x1);           /* right column, if partial */
    int lcol = au_f2px(floorf(x0)), rcol = au_f2px(floorf(x1));
    for (int y = ir.y0; y < ir.y1; y++) {
        float top = (float)y > y0 ? (float)y : y0;
        float bot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float vy = bot - top;
        if (vy <= 0)
            continue;
        bool partial = vy < 0.999f || (lcol >= ir.x0 && lc < 0.999f) || (rcol < ir.x1 && rc > 0.001f);
        if (!partial) {
            blend_row(s, ir.x0 + ix, y + iy, n, src, NULL);
            continue;
        }
        uint8_t full = (uint8_t)(vy * 255.0f + 0.5f);
        memset(cov, full, (size_t)n);
        if (lcol == rcol) {
            /* Narrower than one pixel. */
            if (lcol >= ir.x0 && lcol < ir.x1)
                cov[lcol - ir.x0] = (uint8_t)((x1 - x0) * vy * 255.0f + 0.5f);
        } else {
            if (lcol >= ir.x0 && lcol < ir.x1)
                cov[lcol - ir.x0] = (uint8_t)(lc * vy * 255.0f + 0.5f);
            if (rcol >= ir.x0 && rcol < ir.x1)
                cov[rcol - ir.x0] = (uint8_t)(rc * vy * 255.0f + 0.5f);
        }
        blend_row(s, ir.x0 + ix, y + iy, n, src, cov);
    }
    if (cov != stackbuf)
        au_free(cov);
}

void au_fill_rect(AuCanvas *c, AuRect r, AuColor color)
{
    Source src = {0, au_premul(color), NULL};
    if (!(src.color >> 24))
        return;
    fill_rect_source(c, r, &src);
}

void au_fill_rect_gradient(AuCanvas *c, AuRect r, const AuGradient *g)
{
    int ox, oy;
    AuIRect clip;
    au_canvas_current(c, &ox, &oy, &clip);
    AuGradientLut *lut = au_alloc(sizeof *lut);
    au_gradient_prepare(g, c->tx - ox, c->ty - oy, lut);
    Source src = {1, 0, lut};
    fill_rect_source(c, r, &src);
    au_free(lut);
}

/* ---- Paths ---- */

typedef struct {
    AuSurface *s;
    const Source *src;
} PathCtx;

static void path_span(void *ctx, int y, int x, int n, const uint8_t *cov)
{
    PathCtx *p = ctx;
    blend_row(p->s, x, y, n, p->src, cov);
}

static void fill_path_source(AuCanvas *c, const AuPath *p, AuFillRule rule, const Source *src)
{
    int ox, oy;
    AuIRect clip;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    PathCtx ctx = {s, src};
    au_raster_path(p, c->tx - ox, c->ty - oy, rule, clip, path_span, &ctx);
}

void au_fill_path(AuCanvas *c, const AuPath *p, AuFillRule rule, AuColor color)
{
    Source src = {0, au_premul(color), NULL};
    if (!(src.color >> 24))
        return;
    fill_path_source(c, p, rule, &src);
}

void au_fill_path_gradient(AuCanvas *c, const AuPath *p, AuFillRule rule, const AuGradient *g)
{
    int ox, oy;
    AuIRect clip;
    au_canvas_current(c, &ox, &oy, &clip);
    AuGradientLut *lut = au_alloc(sizeof *lut);
    au_gradient_prepare(g, c->tx - ox, c->ty - oy, lut);
    Source src = {1, 0, lut};
    fill_path_source(c, p, rule, &src);
    au_free(lut);
}

void au_fill_rounded_rect(AuCanvas *c, AuRect r, const AuRadii *radii, AuColor color)
{
    if (au_radii_zero(radii)) {
        au_fill_rect(c, r, color);
        return;
    }
    AuPath p;
    au_path_init(&p);
    au_path_rounded_rect(&p, r, radii, false);
    au_fill_path(c, &p, AU_FILL_NONZERO, color);
    au_path_free(&p);
}

/* ---- Masks ---- */

/* Coverage gamma for text: linear coverage makes dark-on-light text look
 * thin; this lifts mid values a little. */
static uint8_t text_gamma[256];
static bool text_gamma_ready;

const uint8_t *au_text_gamma(void)
{
    if (!text_gamma_ready) {
        for (int i = 0; i < 256; i++)
            text_gamma[i] = (uint8_t)(powf(i / 255.0f, 1.0f / 1.2f) * 255.0f + 0.5f);
        text_gamma_ready = true;
    }
    return text_gamma;
}

void au_draw_coverage(AuCanvas *c, const uint8_t *cov, int w, int h, int stride, float x, float y, AuColor color)
{
    uint32_t src = au_premul(color);
    if (!(src >> 24) || w <= 0 || h <= 0)
        return;
    int ox, oy;
    AuIRect clip;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    int px = round_px(x + c->tx) - ox, py = round_px(y + c->ty) - oy;
    AuIRect r = {px, py, px + w, py + h};
    r = au_irect_intersect(r, clip);
    if (au_irect_empty(r))
        return;
    int n = r.x1 - r.x0;
    for (int yy = r.y0; yy < r.y1; yy++)
        au_blend_solid_span(s->pixels + (size_t)yy * s->stride + r.x0, n, src,
                            cov + (size_t)(yy - py) * stride + (r.x0 - px));
}

void au_draw_mask(AuCanvas *c, const uint8_t *mask, int w, int h, int stride, float x, float y,
                  AuColor color)
{
    uint32_t src = au_premul(color);
    if (!(src >> 24) || w <= 0 || h <= 0)
        return;
    au_text_gamma();
    int ox, oy;
    AuIRect clip;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    int px = round_px(x + c->tx) - ox, py = round_px(y + c->ty) - oy;
    AuIRect r = {px, py, px + w, py + h};
    r = au_irect_intersect(r, clip);
    if (au_irect_empty(r))
        return;
    uint8_t buf[256];
    int n = r.x1 - r.x0;
    uint8_t *cov = n <= (int)sizeof buf ? buf : au_alloc((size_t)n);
    for (int yy = r.y0; yy < r.y1; yy++) {
        const uint8_t *m = mask + (size_t)(yy - py) * stride + (r.x0 - px);
        for (int i = 0; i < n; i++)
            cov[i] = text_gamma[m[i]];
        au_blend_solid_span(s->pixels + (size_t)yy * s->stride + r.x0, n, src, cov);
    }
    if (cov != buf)
        au_free(cov);
}

/* ---- Images ----
 * Destination pixels map back to source coordinates in 16.16 fixed point.
 * Bilinear filtering samples pixel centres and clamps at the edges of the
 * source rectangle.
 * AltiVec: the bilinear blend is four multiplies per pixel and vectorises
 * well. */

static uint32_t bilerp(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11, uint32_t fx, uint32_t fy)
{
    uint32_t top = au_px_scale_trunc(p00, 256 - fx) + au_px_scale_trunc(p10, fx);
    uint32_t bot = au_px_scale_trunc(p01, 256 - fx) + au_px_scale_trunc(p11, fx);
    return au_px_scale_trunc(top, 256 - fy) + au_px_scale_trunc(bot, fy);
}

/* Source pixels a destination pixel covers along one axis, reduced to at
 * most RED_TAPS evenly spaced samples. */
#define RED_TAPS 16
typedef struct {
    int32_t start, step; /* first sample; 16.16 distance between samples */
    int32_t count;
} RedSpan;

static RedSpan red_span(int64_t i, int64_t n_dst, int32_t n_src)
{
    int32_t a = (int32_t)(i * n_src / n_dst);
    int32_t b = (int32_t)((i + 1) * n_src / n_dst);
    if (b > n_src)
        b = n_src;
    if (b <= a)
        b = a + 1 <= n_src ? a + 1 : n_src;
    if (a >= n_src)
        a = n_src - 1;
    int32_t n = b - a;
    RedSpan sp;
    sp.count = n > RED_TAPS ? RED_TAPS : n;
    sp.step = (int32_t)(((int64_t)n << 16) / sp.count);
    sp.start = a;
    return sp;
}

/* Downscaling by 2 or more: each destination pixel is the average of the
 * source pixels it covers (a box filter; at most 16 x 16 samples, evenly
 * spaced, for larger reductions). Bilinear sampling would skip pixels and
 * alias. */
static void draw_image_reduce(AuSurface *s, const AuSurface *img, AuIRect src, AuIRect d, AuIRect r, uint32_t a,
                              bool store)
{
    int sw = src.x1 - src.x0, sh = src.y1 - src.y0;
    int dw = d.x1 - d.x0, dh = d.y1 - d.y0;
    int ncol = r.x1 - r.x0;
    RedSpan colbuf[64];
    RedSpan *cols = ncol <= 64 ? colbuf : au_alloc((size_t)ncol * sizeof *cols);
    for (int i = 0; i < ncol; i++)
        cols[i] = red_span(r.x0 - d.x0 + i, dw, sw);
    int32_t ys[RED_TAPS];
    /* A compact image (au_surface_convert): the rows an output row reads,
     * expanded once */
    const AuPixel *srows[RED_TAPS];
    AuPixel *tmp = img->format != AU_SURF_ARGB32 ? au_alloc((size_t)RED_TAPS * (size_t)sw * sizeof *tmp) : NULL;
    for (int y = r.y0; y < r.y1; y++) {
        RedSpan rs = red_span(y - d.y0, dh, sh);
        for (int k = 0; k < rs.count; k++) {
            ys[k] = src.y0 + rs.start + (int32_t)(((int64_t)k * rs.step + rs.step / 2) >> 16);
            srows[k] = au_surface_row(img, ys[k], src.x0, sw, tmp ? tmp + (size_t)k * sw : NULL);
        }
        AuPixel *drow = s->pixels + (size_t)y * s->stride;
        for (int i = 0; i < ncol; i++) {
            const RedSpan *cs = &cols[i];
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
            for (int ky = 0; ky < rs.count; ky++) {
                const AuPixel *row = srows[ky] + cs->start;
                int32_t fx = cs->step / 2;
                for (int kx = 0; kx < cs->count; kx++, fx += cs->step) {
                    uint32_t p = row[fx >> 16];
                    sa += p >> 24;
                    sr += (p >> 16) & 255;
                    sg += (p >> 8) & 255;
                    sb += p & 255;
                }
            }
            uint32_t n = (uint32_t)(rs.count * cs->count);
            uint32_t inv = (65536u + n / 2) / n;
            uint32_t p = (((sa * inv + 32768) >> 16) << 24) | (((sr * inv + 32768) >> 16) << 16) |
                         (((sg * inv + 32768) >> 16) << 8) | ((sb * inv + 32768) >> 16);
            int x = r.x0 + i;
            if (store) {
                drow[x] = p | 0xFF000000u;
                continue;
            }
            if (!p)
                continue;
            if (a < 256)
                p = au_px_scale(p, a);
            drow[x] = au_px_over(p, drow[x]);
        }
    }
    if (cols != colbuf)
        au_free(cols);
    au_free(tmp);
}

void au_draw_image(AuCanvas *c, const AuSurface *img, AuIRect src, AuRect dst, AuFilter filter,
                   float opacity)
{
    AuIRect all = {0, 0, img->width, img->height};
    src = au_irect_intersect(src, all);
    if (au_irect_empty(src) || !(dst.w > 0 && dst.h > 0) || !(opacity > 0))
        return;
    if (opacity > 1)
        opacity = 1;
    uint32_t a = (uint32_t)(opacity * 256.0f + 0.5f);
    int ox, oy;
    AuIRect clip;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    float x0 = dst.x + c->tx - ox, y0 = dst.y + c->ty - oy;
    /* Images snap to whole pixels, as browsers paint them. */
    AuIRect d = {round_px(x0), round_px(y0), round_px(x0 + dst.w), round_px(y0 + dst.h)};
    if (d.x1 <= d.x0)
        d.x1 = d.x0 + 1;
    if (d.y1 <= d.y0)
        d.y1 = d.y0 + 1;
    AuIRect r = au_irect_intersect(d, clip);
    if (au_irect_empty(r))
        return;
    int sw = src.x1 - src.x0, sh = src.y1 - src.y0;
    int dw = d.x1 - d.x0, dh = d.y1 - d.y0;
    /* Source step per destination pixel, 16.16. */
    int32_t stepx = (int32_t)(((int64_t)sw << 16) / dw);
    int32_t stepy = (int32_t)(((int64_t)sh << 16) / dh);
    /* Opaque pixels at full opacity replace the destination. */
    bool store = img->opaque && a >= 256;
    /* A compact image's rows are expanded as they are read: up to two
     * rows of the source's width. */
    AuPixel *tmp = img->format != AU_SURF_ARGB32 ? au_alloc((size_t)2 * (size_t)sw * sizeof *tmp) : NULL;
    if (sw == dw && sh == dh) {
        /* Unscaled: copy or blend straight across. */
        for (int y = r.y0; y < r.y1; y++) {
            AuPixel *drow = s->pixels + (size_t)y * s->stride + r.x0;
            int n = r.x1 - r.x0;
            const AuPixel *srow = au_surface_row(img, src.y0 + y - d.y0, src.x0 + (r.x0 - d.x0), n, tmp);
            if (store) {
                memcpy(drow, srow, (size_t)n * sizeof *drow);
                continue;
            }
            for (int i = 0; i < n; i++) {
                uint32_t p = srow[i];
                if (!p)
                    continue;
                if (a < 256)
                    p = au_px_scale(p, a);
                drow[i] = (p >> 24) == 255 ? p : au_px_over(p, drow[i]);
            }
        }
        au_free(tmp);
        return;
    }
    if (filter == AU_FILTER_BILINEAR && (stepx >= 2 << 16 || stepy >= 2 << 16)) {
        au_free(tmp);
        draw_image_reduce(s, img, src, d, r, a, store);
        return;
    }
    for (int y = r.y0; y < r.y1; y++) {
        AuPixel *drow = s->pixels + (size_t)y * s->stride;
        if (filter == AU_FILTER_NEAREST) {
            int32_t sy = (int32_t)(((int64_t)(y - d.y0) * stepy + stepy / 2) >> 16);
            if (sy >= sh)
                sy = sh - 1;
            const AuPixel *srow = au_surface_row(img, src.y0 + sy, src.x0, sw, tmp);
            int32_t fx = (r.x0 - d.x0) * stepx + stepx / 2;
            for (int x = r.x0; x < r.x1; x++, fx += stepx) {
                int32_t sx = fx >> 16;
                if (sx >= sw)
                    sx = sw - 1;
                uint32_t p = srow[sx];
                if (store) {
                    drow[x] = p;
                    continue;
                }
                if (!p)
                    continue;
                if (a < 256)
                    p = au_px_scale(p, a);
                drow[x] = au_px_over(p, drow[x]);
            }
        } else {
            /* Centre of destination pixel, mapped, minus half a source
             * pixel, gives the top-left sample. */
            int32_t fy = (int32_t)((int64_t)(y - d.y0) * stepy + stepy / 2) - 32768;
            int32_t iy = fy >> 16;
            uint32_t wy = (uint32_t)(fy & 0xFFFF) >> 8;
            int32_t y0i = iy < 0 ? 0 : (iy >= sh ? sh - 1 : iy);
            int32_t y1i = iy + 1 < 0 ? 0 : (iy + 1 >= sh ? sh - 1 : iy + 1);
            const AuPixel *r0 = au_surface_row(img, src.y0 + y0i, src.x0, sw, tmp);
            const AuPixel *r1 = y1i == y0i ? r0 : au_surface_row(img, src.y0 + y1i, src.x0, sw, tmp ? tmp + sw : NULL);
            int32_t fx = (r.x0 - d.x0) * stepx + stepx / 2 - 32768;
            for (int x = r.x0; x < r.x1; x++, fx += stepx) {
                int32_t ix = fx >> 16;
                uint32_t wx = (uint32_t)(fx & 0xFFFF) >> 8;
                int32_t x0i = ix < 0 ? 0 : (ix >= sw ? sw - 1 : ix);
                int32_t x1i = ix + 1 < 0 ? 0 : (ix + 1 >= sw ? sw - 1 : ix + 1);
                uint32_t p = bilerp(r0[x0i], r0[x1i], r1[x0i], r1[x1i], wx, wy);
                if (store) {
                    /* Truncation can leave alpha at 254: keep it opaque. */
                    drow[x] = p | 0xFF000000u;
                    continue;
                }
                if (!p)
                    continue;
                if (a < 256)
                    p = au_px_scale(p, a);
                drow[x] = au_px_over(p, drow[x]);
            }
        }
    }
    au_free(tmp);
}
