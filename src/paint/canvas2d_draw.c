/* Drawing for the 2D canvas context: paint sources (colours, gradients,
 * patterns, images), compositing with every operator of the Compositing
 * and Blending specification, clipping, shadows, fills, strokes, text and
 * images.
 *
 * The model. A shape is a coverage (the rasterizer's spans, or a
 * rectangle's edges); a paint gives a premultiplied source pixel for each
 * destination pixel, with the global alpha already applied. For each
 * pixel the source is scaled by the coverage and combined with the
 * destination by the operator. The clip scales the coverage, except for
 * the operators that change pixels the shape does not cover (source-in,
 * source-out, destination-in, destination-atop, copy): those run over the
 * whole clip region and the clip mask mixes old and new pixels.
 *
 * Fast paths: source-over with a solid colour goes to the span blender
 * that page painting uses; an axis-aligned rectangle never builds a path;
 * an unscaled or axis-aligned image on whole pixels is copied or blended
 * row by row. */
#include "canvas2d_int.h"

#include <math.h>
#include <string.h>

#define C2D_PI_F 3.14159265358979f

/* ---- Paints ---- */

enum { PK_SOLID, PK_LINEAR, PK_RADIAL, PK_CONIC, PK_IMAGE };

typedef struct Paint {
    uint8_t kind;
    uint32_t solid;    /* premultiplied, global alpha applied */
    uint32_t alpha256; /* the global alpha, for the other kinds */
    const uint32_t *lut;
    AuMatrix inv;      /* a device pixel centre to paint space */
    float x0, y0, gx, gy;             /* linear: t = (u - x0) * gx + (v - y0) * gy */
    float r0, cdx, cdy, dr, qa;       /* radial */
    float start;                      /* conic */
    const AuSurface *img;
    int ix0, iy0, ix1, iy1;           /* the part of img that is sampled */
    bool rep_x, rep_y, clamp, smooth, opaque;
} Paint;

static inline uint32_t div255(uint32_t v)
{
    v += 128;
    return (v + (v >> 8)) >> 8;
}

static inline int lut_index(float t)
{
    if (!(t > 0))
        return 0;
    if (t >= 1)
        return 255;
    return (int)(t * 255.0f + 0.5f);
}

static inline uint32_t img_px(const Paint *p, int x, int y)
{
    uint32_t v = p->img->pixels[(size_t)y * p->img->stride + x];
    return p->opaque ? v | 0xFF000000u : v;
}

/* Wraps or clamps a sample coordinate into [lo, hi); -1 when the sample
 * lies outside an image that does not repeat. */
static inline int img_coord(int v, int lo, int hi, bool repeat, bool clamp)
{
    if (v >= lo && v < hi)
        return v;
    if (repeat) {
        int n = hi - lo;
        v = (v - lo) % n;
        if (v < 0)
            v += n;
        return lo + v;
    }
    if (clamp)
        return v < lo ? lo : hi - 1;
    return -1;
}

/* Weights are 0..256 per axis; each channel is rounded, so a flat colour
 * comes out as it went in (the page painter's faster version truncates,
 * which script reading pixels back would see). */
static inline uint32_t bilerp(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11, uint32_t fx, uint32_t fy)
{
    uint32_t w11 = fx * fy, w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w00 = (256 - fx) * (256 - fy);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t v = ((p00 >> sh) & 255) * w00 + ((p10 >> sh) & 255) * w10 + ((p01 >> sh) & 255) * w01 +
                     ((p11 >> sh) & 255) * w11;
        out |= ((v + 32768) >> 16) << sh;
    }
    return out;
}

/* The source pixels for n destination pixels from (x, y). */
static void paint_fetch(const Paint *p, int x, int y, int n, uint32_t *out)
{
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float u = p->inv.a * px + p->inv.c * py + p->inv.e;
    float v = p->inv.b * px + p->inv.d * py + p->inv.f;
    float du = p->inv.a, dv = p->inv.b;
    switch (p->kind) {
    case PK_LINEAR: {
        float t = (u - p->x0) * p->gx + (v - p->y0) * p->gy;
        float dt = du * p->gx + dv * p->gy;
        for (int i = 0; i < n; i++, t += dt)
            out[i] = p->lut[lut_index(t)];
        break;
    }
    case PK_RADIAL:
        for (int i = 0; i < n; i++, u += du, v += dv) {
            /* The largest w with radius r0 + w dr >= 0 whose circle
             * passes through the point. */
            float qx = u - p->x0, qy = v - p->y0;
            float b = qx * p->cdx + qy * p->cdy + p->r0 * p->dr;
            float cc = qx * qx + qy * qy - p->r0 * p->r0;
            float w;
            bool ok = false;
            if (fabsf(p->qa) < 1e-9f) {
                if (fabsf(b) > 1e-12f) {
                    w = cc / (2 * b);
                    ok = p->r0 + w * p->dr >= 0;
                }
            } else {
                float disc = b * b - p->qa * cc;
                if (disc >= 0) {
                    float sq = sqrtf(disc);
                    float w1 = (b + sq) / p->qa, w2 = (b - sq) / p->qa;
                    if (w2 > w1) {
                        float t = w1;
                        w1 = w2;
                        w2 = t;
                    }
                    if (p->r0 + w1 * p->dr >= 0) {
                        w = w1;
                        ok = true;
                    } else if (p->r0 + w2 * p->dr >= 0) {
                        w = w2;
                        ok = true;
                    }
                }
            }
            out[i] = ok ? p->lut[lut_index(w)] : 0;
        }
        break;
    case PK_CONIC:
        for (int i = 0; i < n; i++, u += du, v += dv) {
            float a = atan2f(v - p->y0, u - p->x0) - p->start;
            float t = a / (2 * C2D_PI_F);
            t -= floorf(t);
            out[i] = p->lut[lut_index(t)];
        }
        break;
    case PK_IMAGE:
        if (!p->smooth) {
            for (int i = 0; i < n; i++, u += du, v += dv) {
                int sx = img_coord((int)floorf(u), p->ix0, p->ix1, p->rep_x, p->clamp);
                int sy = img_coord((int)floorf(v), p->iy0, p->iy1, p->rep_y, p->clamp);
                out[i] = sx < 0 || sy < 0 ? 0 : img_px(p, sx, sy);
            }
        } else {
            u -= 0.5f;
            v -= 0.5f;
            for (int i = 0; i < n; i++, u += du, v += dv) {
                float fu = floorf(u), fv = floorf(v);
                int ix = (int)fu, iy = (int)fv;
                uint32_t wx = (uint32_t)((u - fu) * 256.0f), wy = (uint32_t)((v - fv) * 256.0f);
                int xa = img_coord(ix, p->ix0, p->ix1, p->rep_x, p->clamp);
                int xb = img_coord(ix + 1, p->ix0, p->ix1, p->rep_x, p->clamp);
                int ya = img_coord(iy, p->iy0, p->iy1, p->rep_y, p->clamp);
                int yb = img_coord(iy + 1, p->iy0, p->iy1, p->rep_y, p->clamp);
                uint32_t p00 = xa < 0 || ya < 0 ? 0 : img_px(p, xa, ya);
                uint32_t p10 = xb < 0 || ya < 0 ? 0 : img_px(p, xb, ya);
                uint32_t p01 = xa < 0 || yb < 0 ? 0 : img_px(p, xa, yb);
                uint32_t p11 = xb < 0 || yb < 0 ? 0 : img_px(p, xb, yb);
                uint32_t r = bilerp(p00, p10, p01, p11, wx > 256 ? 256 : wx, wy > 256 ? 256 : wy);
                out[i] = p->opaque ? r | 0xFF000000u : r;
            }
        }
        break;
    default:
        for (int i = 0; i < n; i++)
            out[i] = p->solid;
        return;
    }
    if (p->alpha256 < 256)
        for (int i = 0; i < n; i++)
            out[i] = au_px_scale(out[i], p->alpha256);
}

static uint32_t alpha256_of(float a)
{
    if (!(a > 0))
        return 0;
    if (a >= 1)
        return 256;
    return (uint32_t)(a * 256.0f + 0.5f);
}

/* A matrix with values that sampling can step through: floats in, a
 * check that the result is finite. */
static bool invert_checked(const AuMatrix *m, AuMatrix *out)
{
    if (!au_matrix_invert(m, out))
        return false;
    return isfinite(out->a) && isfinite(out->b) && isfinite(out->c) && isfinite(out->d) && isfinite(out->e) &&
           isfinite(out->f);
}

static void paint_solid(Paint *p, AuColor color, float alpha)
{
    memset(p, 0, sizeof *p);
    p->kind = PK_SOLID;
    p->alpha256 = 256;
    uint32_t px = au_premul(color);
    uint32_t a = alpha256_of(alpha);
    p->solid = a < 256 ? au_px_scale(px, a) : px;
}

/* The paint of fillStyle or strokeStyle under the current transform.
 * False when it draws nothing. */
static bool make_paint(AuCanvas2D *c, int which, Paint *p)
{
    const C2DState *st = c2d_state(c);
    const C2DStyle *s = &st->style[which];
    if (s->kind == AU_C2D_STYLE_COLOR) {
        paint_solid(p, s->color, st->alpha);
        return true;
    }
    memset(p, 0, sizeof *p);
    p->alpha256 = alpha256_of(st->alpha);
    if (s->kind == AU_C2D_STYLE_GRADIENT) {
        AuCanvasGradient *g = s->grad;
        if (!g || !g->nstops || !invert_checked(&st->m, &p->inv))
            return false;
        p->lut = c2d_gradient_lut(g);
        p->x0 = g->x0;
        p->y0 = g->y0;
        if (g->kind == GRAD_LINEAR) {
            float dx = g->x1 - g->x0, dy = g->y1 - g->y0;
            float len2 = dx * dx + dy * dy;
            if (!(len2 > 0) || !isfinite(len2))
                return false;
            p->kind = PK_LINEAR;
            p->gx = dx / len2;
            p->gy = dy / len2;
        } else if (g->kind == GRAD_RADIAL) {
            if (g->x0 == g->x1 && g->y0 == g->y1 && g->r0 == g->r1)
                return false;
            p->kind = PK_RADIAL;
            p->r0 = g->r0;
            p->cdx = g->x1 - g->x0;
            p->cdy = g->y1 - g->y0;
            p->dr = g->r1 - g->r0;
            p->qa = p->cdx * p->cdx + p->cdy * p->cdy - p->dr * p->dr;
            if (!isfinite(p->qa))
                return false;
        } else {
            p->kind = PK_CONIC;
            /* Angle 0 points along +x (the standard starts a conic
             * gradient at the given angle from the x axis). */
            p->start = g->r0;
        }
        return true;
    }
    AuCanvasPattern *pt = s->pat;
    if (!pt || !pt->bmp || !pt->bmp->surf)
        return false;
    const AuCanvasBitmap *b = pt->bmp;
    AuMatrix m = st->m;
    au_matrix_multiply(&m, &pt->m);
    /* Pattern space is in the bitmap's natural size; the surface may be
     * a reduced decode. */
    AuMatrix sc = {(float)b->nat_w / (float)b->surf->width, 0, 0, (float)b->nat_h / (float)b->surf->height, 0, 0};
    au_matrix_multiply(&m, &sc);
    if (!invert_checked(&m, &p->inv))
        return false;
    p->kind = PK_IMAGE;
    p->img = b->surf;
    p->ix0 = p->iy0 = 0;
    p->ix1 = b->surf->width;
    p->iy1 = b->surf->height;
    p->rep_x = (pt->repeat & 1) != 0;
    p->rep_y = (pt->repeat & 2) != 0;
    p->opaque = b->surf->opaque;
    /* Whole-pixel placement needs no filtering. */
    bool whole = p->inv.a == 1 && p->inv.b == 0 && p->inv.c == 0 && p->inv.d == 1 && p->inv.e == floorf(p->inv.e) &&
                 p->inv.f == floorf(p->inv.f);
    p->smooth = st->smoothing && !whole;
    return true;
}

/* ---- Compositing ---- */

static bool op_unbounded(int op)
{
    return op == OP_SOURCE_IN || op == OP_SOURCE_OUT || op == OP_DEST_IN || op == OP_DEST_ATOP || op == OP_COPY;
}

static inline uint32_t clamp255(uint32_t v)
{
    return v > 255 ? 255 : v;
}

static float lum(const float c[3])
{
    return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}

static void clip_color(float c[3])
{
    float l = lum(c);
    float n = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
    float x = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    for (int i = 0; i < 3; i++) {
        if (n < 0 && l - n != 0)
            c[i] = l + (c[i] - l) * l / (l - n);
        if (x > 1 && x - l != 0)
            c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}

static void set_lum(float c[3], float l)
{
    float d = l - lum(c);
    c[0] += d;
    c[1] += d;
    c[2] += d;
    clip_color(c);
}

static float sat(const float c[3])
{
    float n = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
    float x = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    return x - n;
}

static void set_sat(float c[3], float s)
{
    int mx = 0, mn = 0;
    for (int i = 1; i < 3; i++) {
        if (c[i] > c[mx])
            mx = i;
        if (c[i] < c[mn])
            mn = i;
    }
    if (mx == mn) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    int mid = 3 - mx - mn;
    if (c[mx] > c[mn]) {
        c[mid] = (c[mid] - c[mn]) * s / (c[mx] - c[mn]);
        c[mx] = s;
    } else {
        c[mid] = c[mx] = 0;
    }
    c[mn] = 0;
}

/* The blend function B(cb, cs) of a separable mode that needs division. */
static float blend_sep(int op, float cb, float cs)
{
    switch (op) {
    case OP_COLOR_DODGE:
        if (cb <= 0)
            return 0;
        if (cs >= 1)
            return 1;
        return cb / (1 - cs) > 1 ? 1 : cb / (1 - cs);
    case OP_COLOR_BURN:
        if (cb >= 1)
            return 1;
        if (cs <= 0)
            return 0;
        return 1 - ((1 - cb) / cs > 1 ? 1 : (1 - cb) / cs);
    default: { /* soft-light */
        if (cs <= 0.5f)
            return cb - (1 - 2 * cs) * cb * (1 - cb);
        float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    }
}

/* One pixel: the source s combined with the destination d by op, both
 * premultiplied (Compositing and Blending Level 1). */
static uint32_t blend_px(int op, uint32_t s, uint32_t d)
{
    uint32_t sa = s >> 24, da = d >> 24;
    if (op <= OP_XOR) {
        uint32_t fa, fb;
        switch (op) {
        case OP_SOURCE_OVER: fa = 255, fb = 255 - sa; break;
        case OP_SOURCE_IN: fa = da, fb = 0; break;
        case OP_SOURCE_OUT: fa = 255 - da, fb = 0; break;
        case OP_SOURCE_ATOP: fa = da, fb = 255 - sa; break;
        case OP_DEST_OVER: fa = 255 - da, fb = 255; break;
        case OP_DEST_IN: fa = 0, fb = sa; break;
        case OP_DEST_OUT: fa = 0, fb = 255 - sa; break;
        case OP_DEST_ATOP: fa = 255 - da, fb = sa; break;
        case OP_LIGHTER: fa = 255, fb = 255; break;
        case OP_COPY: fa = 255, fb = 0; break;
        default: fa = 255 - da, fb = 255 - sa; break; /* xor */
        }
        uint32_t out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            uint32_t v = div255(((s >> sh) & 255) * fa + ((d >> sh) & 255) * fb);
            out |= clamp255(v) << sh;
        }
        return out;
    }
    uint32_t ao = sa + da - div255(sa * da);
    uint32_t out = ao << 24;
    if (op >= OP_HUE) {
        /* Non-separable: on the colours without alpha. */
        float cs[3] = {0, 0, 0}, cb[3] = {0, 0, 0}, r[3];
        for (int i = 0; i < 3; i++) {
            int sh = 16 - 8 * i;
            if (sa)
                cs[i] = (float)((s >> sh) & 255) / (float)sa;
            if (da)
                cb[i] = (float)((d >> sh) & 255) / (float)da;
        }
        switch (op) {
        case OP_HUE:
            memcpy(r, cs, sizeof r);
            set_sat(r, sat(cb));
            set_lum(r, lum(cb));
            break;
        case OP_SATURATION:
            memcpy(r, cb, sizeof r);
            set_sat(r, sat(cs));
            set_lum(r, lum(cb));
            break;
        case OP_COLOR:
            memcpy(r, cs, sizeof r);
            set_lum(r, lum(cb));
            break;
        default:
            memcpy(r, cb, sizeof r);
            set_lum(r, lum(cs));
            break;
        }
        for (int i = 0; i < 3; i++) {
            int sh = 16 - 8 * i;
            uint32_t S = (s >> sh) & 255, D = (d >> sh) & 255;
            float mix = (float)(sa * da) * r[i] / 255.0f;
            uint32_t v = div255(S * (255 - da) + D * (255 - sa)) + (uint32_t)(mix < 0 ? 0 : mix + 0.5f);
            out |= (v > ao ? ao : v) << sh;
        }
        return out;
    }
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t S = (s >> sh) & 255, D = (d >> sh) & 255;
        uint32_t mix;
        switch (op) {
        case OP_MULTIPLY: mix = div255(S * D); break;
        case OP_SCREEN: mix = div255(S * da + D * sa - S * D); break;
        case OP_OVERLAY:
            mix = 2 * D <= da ? div255(2 * S * D) : div255(sa * da - 2 * (da - D) * (sa - S));
            break;
        case OP_DARKEN: mix = div255(S * da < D * sa ? S * da : D * sa); break;
        case OP_LIGHTEN: mix = div255(S * da > D * sa ? S * da : D * sa); break;
        case OP_HARD_LIGHT:
            mix = 2 * S <= sa ? div255(2 * S * D) : div255(sa * da - 2 * (da - D) * (sa - S));
            break;
        case OP_DIFFERENCE: mix = div255(S * da > D * sa ? S * da - D * sa : D * sa - S * da); break;
        case OP_EXCLUSION: mix = div255(S * da + D * sa - 2 * S * D); break;
        default: {
            float m = 0;
            if (sa && da)
                m = (float)(sa * da) * blend_sep(op, (float)D / (float)da, (float)S / (float)sa) / 255.0f;
            mix = (uint32_t)(m < 0 ? 0 : m + 0.5f);
            break;
        }
        }
        uint32_t v = div255(S * (255 - da) + D * (255 - sa)) + mix;
        out |= (v > ao ? ao : v) << sh;
    }
    return out;
}

typedef struct Comp {
    AuCanvas2D *c;
    const Paint *p;
    int op;
    const C2DClip *clip; /* NULL: none */
    bool force_opaque;   /* a canvas without alpha: results stay opaque */
} Comp;

/* Combines the paint with n pixels of row y from x. cov gives each
 * pixel's coverage; NULL means ucov (0..255) for all. The caller keeps
 * the run inside the clip box. */
static void comp_run(const Comp *k, int y, int x, int n, const uint8_t *cov, uint32_t ucov)
{
    AuSurface *s = k->c->surf;
    AuPixel *d = s->pixels + (size_t)y * s->stride + x;
    const Paint *p = k->p;
    const uint8_t *cm = k->clip ? k->clip->mask + (size_t)y * (size_t)k->c->w + (size_t)x : NULL;
    int op = k->op;
    if (op == OP_SOURCE_OVER && !cm && p->kind == PK_SOLID) {
        if (cov || ucov == 255)
            au_blend_solid_span(d, n, p->solid, cov);
        else if (ucov)
            au_blend_solid_span(d, n, au_px_scale(p->solid, au_a256(ucov)), NULL);
        return;
    }
    bool unbounded = op_unbounded(op);
    uint32_t buf[128];
    for (int done = 0; done < n;) {
        int m = n - done > 128 ? 128 : n - done;
        if (p->kind != PK_SOLID)
            paint_fetch(p, x + done, y, m, buf);
        for (int i = 0; i < m; i++) {
            int j = done + i;
            uint32_t src = p->kind == PK_SOLID ? p->solid : buf[i];
            uint32_t a = cov ? cov[j] : ucov;
            if (!unbounded) {
                if (cm)
                    a = div255(a * cm[j]);
                if (!a)
                    continue;
                if (a < 255)
                    src = au_px_scale(src, au_a256(a));
                if (op == OP_SOURCE_OVER) {
                    if (src)
                        d[j] = (src >> 24) == 255 ? src : au_px_over(src, d[j]);
                    continue;
                }
                uint32_t r = blend_px(op, src, d[j]);
                d[j] = k->force_opaque ? r | 0xFF000000u : r;
            } else {
                if (a < 255)
                    src = au_px_scale(src, au_a256(a));
                uint32_t r = blend_px(op, src, d[j]);
                if (cm) {
                    uint32_t t = au_a256(cm[j]);
                    r = au_px_scale_trunc(d[j], 256 - t) + au_px_scale_trunc(r, t);
                }
                d[j] = k->force_opaque ? r | 0xFF000000u : r;
            }
        }
        done += m;
    }
}

static void span_fn(void *ctx, int y, int x, int n, const uint8_t *cov)
{
    comp_run(ctx, y, x, n, cov, 0);
}

static AuIRect clip_box_of(const AuCanvas2D *c)
{
    return au_irect_intersect(c2d_state(c)->clip_box, (AuIRect){0, 0, c->w, c->h});
}

static void comp_init(Comp *k, AuCanvas2D *c, const Paint *p, int op)
{
    k->c = c;
    k->p = p;
    k->op = op;
    k->clip = c2d_state(c)->clip;
    k->force_opaque = !c->alpha;
}

/* ---- Shadows ---- */

static bool shadow_on(const AuCanvas2D *c, int op)
{
    const C2DState *st = c2d_state(c);
    return (st->shadow_color >> 24) != 0 && op != OP_COPY &&
           (st->shadow_blur != 0 || st->shadow_x != 0 || st->shadow_y != 0);
}

#define SHADOW_MAX_BLUR 256.0f
#define SHADOW_MAX_AREA ((size_t)16 * 1024 * 1024)

/* Draws the shadow of the shape whose coverage rasterizes from dev (the
 * paint's alpha included): a blurred mask in the shadow colour, moved by
 * the shadow offset, under the same operator and clip. Offsets are
 * rounded to whole pixels. */
static void draw_shadow(AuCanvas2D *c, const AuPath *dev, AuFillRule rule, const Paint *p, int op)
{
    const C2DState *st = c2d_state(c);
    AuIRect box = clip_box_of(c);
    float b[4];
    if (au_irect_empty(box) || !au_path_bounds(dev, b))
        return;
    float blur = st->shadow_blur > SHADOW_MAX_BLUR ? SHADOW_MAX_BLUR : st->shadow_blur;
    float sigma = blur / 2;
    int pad = sigma > 0.25f ? (int)ceilf(sigma * 3) + 1 : 0;
    float fx = st->shadow_x, fy = st->shadow_y;
    if (fx > 100000)
        fx = 100000;
    if (fx < -100000)
        fx = -100000;
    if (fy > 100000)
        fy = 100000;
    if (fy < -100000)
        fy = -100000;
    int ox = (int)floorf(fx + 0.5f), oy = (int)floorf(fy + 0.5f);
    /* The mask covers the shape plus the blur, where its shadow can
     * reach the clip box. */
    AuIRect shape = {(int)floorf(b[0]) - pad, (int)floorf(b[1]) - pad, (int)ceilf(b[2]) + pad + 1,
                     (int)ceilf(b[3]) + pad + 1};
    AuIRect reach = {box.x0 - ox - pad, box.y0 - oy - pad, box.x1 - ox + pad, box.y1 - oy + pad};
    AuIRect area = au_irect_intersect(shape, reach);
    if (au_irect_empty(area))
        return;
    size_t aw = (size_t)(area.x1 - area.x0), ah = (size_t)(area.y1 - area.y0);
    if (aw * ah > SHADOW_MAX_AREA)
        return;
    uint8_t *mask = au_try_alloc(aw * ah);
    if (!mask)
        return;
    memset(mask, 0, aw * ah);
    au_raster_mask(dev, rule, area, mask);
    /* The shape's alpha: coverage times the paint's. */
    if (p->kind == PK_SOLID) {
        uint32_t a = p->solid >> 24;
        if (a < 255)
            for (size_t i = 0; i < aw * ah; i++)
                mask[i] = (uint8_t)div255(mask[i] * a);
    } else {
        uint32_t buf[128];
        for (size_t y = 0; y < ah; y++) {
            uint8_t *row = mask + y * aw;
            for (size_t x = 0; x < aw;) {
                int m = aw - x > 128 ? 128 : (int)(aw - x);
                paint_fetch(p, area.x0 + (int)x, area.y0 + (int)y, m, buf);
                for (int i = 0; i < m; i++)
                    row[x + (size_t)i] = (uint8_t)div255(row[x + (size_t)i] * (buf[i] >> 24));
                x += (size_t)m;
            }
        }
    }
    au_blur_mask(mask, (int)aw, (int)ah, sigma);
    Paint sp;
    paint_solid(&sp, st->shadow_color, 1);
    Comp k;
    comp_init(&k, c, &sp, op);
    AuIRect dst = {area.x0 + ox, area.y0 + oy, area.x1 + ox, area.y1 + oy};
    AuIRect vis = au_irect_intersect(dst, box);
    if (!op_unbounded(op)) {
        for (int y = vis.y0; y < vis.y1; y++)
            comp_run(&k, y, vis.x0, vis.x1 - vis.x0, mask + (size_t)(y - dst.y0) * aw + (size_t)(vis.x0 - dst.x0), 0);
        c2d_touch(c, vis);
    } else {
        uint8_t row[AU_C2D_MAX_DIM];
        int bw = box.x1 - box.x0;
        for (int y = box.y0; y < box.y1; y++) {
            memset(row, 0, (size_t)bw);
            if (y >= vis.y0 && y < vis.y1 && vis.x1 > vis.x0)
                memcpy(row + (vis.x0 - box.x0), mask + (size_t)(y - dst.y0) * aw + (size_t)(vis.x0 - dst.x0),
                       (size_t)(vis.x1 - vis.x0));
            comp_run(&k, y, box.x0, bw, row, 0);
        }
        c2d_touch(c, box);
    }
    au_free(mask);
}

/* ---- Shapes ---- */

/* Fills a path given in device space with the paint. */
static void draw_dev_path(AuCanvas2D *c, const AuPath *dev, AuFillRule rule, const Paint *p, int op, bool shadow)
{
    if (!c2d_ensure_surface(c))
        return;
    AuIRect box = clip_box_of(c);
    if (au_irect_empty(box) || au_path_edge_count(dev) > AU_C2D_MAX_EDGES)
        return;
    if (shadow && shadow_on(c, op))
        draw_shadow(c, dev, rule, p, op);
    Comp k;
    comp_init(&k, c, p, op);
    if (!op_unbounded(op)) {
        float b[4];
        if (!au_path_bounds(dev, b))
            return;
        au_raster_path(dev, 0, 0, rule, box, span_fn, &k);
        AuIRect r = {(int)floorf(b[0]) - 1, (int)floorf(b[1]) - 1, (int)ceilf(b[2]) + 2, (int)ceilf(b[3]) + 2};
        c2d_touch(c, au_irect_intersect(r, box));
        return;
    }
    size_t bw = (size_t)(box.x1 - box.x0), bh = (size_t)(box.y1 - box.y0);
    uint8_t *mask = au_try_alloc(bw * bh);
    if (!mask)
        return;
    memset(mask, 0, bw * bh);
    au_raster_mask(dev, rule, box, mask);
    for (int y = box.y0; y < box.y1; y++)
        comp_run(&k, y, box.x0, (int)bw, mask + (size_t)(y - box.y0) * bw, 0);
    au_free(mask);
    c2d_touch(c, box);
}

/* Fills the device rectangle (x0, y0)-(x1, y1), x0 <= x1 and y0 <= y1,
 * without a path: the edges' coverage is worked out per row. Only for
 * operators that leave uncovered pixels alone. */
static void draw_dev_rect(AuCanvas2D *c, float x0, float y0, float x1, float y1, const Paint *p, int op)
{
    if (!c2d_ensure_surface(c))
        return;
    AuIRect box = clip_box_of(c);
    if (au_irect_empty(box))
        return;
    if (x0 < (float)box.x0)
        x0 = (float)box.x0;
    if (y0 < (float)box.y0)
        y0 = (float)box.y0;
    if (x1 > (float)box.x1)
        x1 = (float)box.x1;
    if (y1 > (float)box.y1)
        y1 = (float)box.y1;
    if (!(x1 > x0) || !(y1 > y0))
        return;
    Comp k;
    comp_init(&k, c, p, op);
    int ix0 = (int)floorf(x0), ix1 = (int)ceilf(x1), iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
    /* Columns: a partial one at each end (the same when the rectangle is
     * narrower than a pixel), whole ones between. */
    float lc = ((float)(ix0 + 1) < x1 ? (float)(ix0 + 1) : x1) - x0;
    float rc = x1 - (float)(ix1 - 1);
    bool one_col = ix1 - ix0 == 1;
    int mx0 = lc >= 1 ? ix0 : ix0 + 1;
    int mx1 = one_col ? (lc >= 1 ? ix1 : mx0) : (rc >= 1 ? ix1 : ix1 - 1);
    for (int y = iy0; y < iy1; y++) {
        float top = (float)y > y0 ? (float)y : y0, bot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float cy = bot - top;
        if (!(cy > 0))
            continue;
        uint32_t cv = cy >= 1 ? 255 : (uint32_t)(cy * 255.0f + 0.5f);
        if (lc < 1) {
            uint32_t a = (uint32_t)(cy * lc * 255.0f + 0.5f);
            if (a)
                comp_run(&k, y, ix0, 1, NULL, a > 255 ? 255 : a);
        }
        if (mx1 > mx0 && cv)
            comp_run(&k, y, mx0, mx1 - mx0, NULL, cv);
        if (!one_col && rc < 1) {
            uint32_t a = (uint32_t)(cy * rc * 255.0f + 0.5f);
            if (a)
                comp_run(&k, y, ix1 - 1, 1, NULL, a > 255 ? 255 : a);
        }
    }
    c2d_touch(c, (AuIRect){ix0, iy0, ix1, iy1});
}

/* Copies src into dst mapped through m (NULL: none), clamped. False when
 * a point does not map to a number. */
static bool path_map(AuPath *dst, const AuPath *src, const AuMatrix *m)
{
    au_path_clear(dst);
    size_t pi = 0;
    for (size_t i = 0; i < src->nops; i++) {
        uint8_t op = src->ops[i];
        int np = op == AU_PATH_CLOSE ? 0 : op == AU_PATH_QUAD ? 2 : op == AU_PATH_CUBIC ? 3 : 1;
        float q[6] = {0};
        for (int k = 0; k < np; k++)
            if (!c2d_map(m, src->pts[pi + 2 * k], src->pts[pi + 2 * k + 1], &q[2 * k], &q[2 * k + 1]))
                return false;
        pi += (size_t)np * 2;
        switch (op) {
        case AU_PATH_MOVE: au_path_move_to(dst, q[0], q[1]); break;
        case AU_PATH_LINE: au_path_line_to(dst, q[0], q[1]); break;
        case AU_PATH_QUAD: au_path_quad_to(dst, q[0], q[1], q[2], q[3]); break;
        case AU_PATH_CUBIC: au_path_cubic_to(dst, q[0], q[1], q[2], q[3], q[4], q[5]); break;
        default: au_path_close(dst); break;
        }
    }
    return true;
}

/* The path a call draws, in device space: the current path, or a Path2D
 * through the transform (into c->tmp). NULL when it cannot be mapped. */
static const AuPath *dev_path(AuCanvas2D *c, const AuCanvasPath *path)
{
    if (!path)
        return &c->path.p;
    if (!path_map(&c->tmp, &path->p, &c2d_state(c)->m))
        return NULL;
    return &c->tmp;
}

void au_c2d_fill(AuCanvas2D *c, const AuCanvasPath *path, AuFillRule rule)
{
    const AuPath *dev = dev_path(c, path);
    Paint p;
    if (!dev || !dev->nops || !make_paint(c, AU_C2D_FILL, &p))
        return;
    draw_dev_path(c, dev, rule, &p, c2d_state(c)->op, true);
}

/* The outline of dev (device space) stroked with the line state, into
 * c->tmp2 in device space. The stroke is made in user space, so a
 * non-uniform transform scales the pen as the standard says. dev must not
 * be c->tmp2. */
static bool stroke_outline(AuCanvas2D *c, const AuPath *dev)
{
    const C2DState *st = c2d_state(c);
    AuMatrix inv;
    if (!invert_checked(&st->m, &inv))
        return false;
    /* User space, in a path of its own: dev may be c->tmp. */
    AuPath user;
    au_path_init(&user);
    bool ok = path_map(&user, dev, &inv);
    float det = st->m.a * st->m.d - st->m.b * st->m.c;
    float scale = sqrtf(fabsf(det));
    float tol = scale > 0 ? 0.1f / scale : 0.1f;
    if (tol < 1e-4f)
        tol = 1e-4f;
    if (tol > 1e3f)
        tol = 1e3f;
    if (ok && au_path_stroke_points(&user, tol) > AU_C2D_MAX_STROKE_POINTS)
        ok = false;
    if (ok) {
        AuStrokeStyle ss;
        memset(&ss, 0, sizeof ss);
        ss.width = st->line_width;
        ss.cap = st->cap;
        ss.join = st->join;
        ss.miter_limit = st->miter;
        ss.dashes = st->ndash ? st->dash : NULL;
        ss.ndashes = st->ndash;
        ss.dash_offset = st->dash_offset;
        ss.tolerance = tol;
        AuPath out;
        au_path_init(&out);
        au_path_stroke_style(&user, &ss, &out);
        ok = out.nops > 0 && out.nops <= 8 * (size_t)AU_C2D_MAX_STROKE_POINTS && path_map(&c->tmp2, &out, &st->m);
        au_path_free(&out);
    }
    au_path_free(&user);
    return ok;
}

void au_c2d_stroke(AuCanvas2D *c, const AuCanvasPath *path)
{
    const AuPath *dev = dev_path(c, path);
    Paint p;
    if (!dev || !dev->nops || !make_paint(c, AU_C2D_STROKE, &p) || !stroke_outline(c, dev))
        return;
    draw_dev_path(c, &c->tmp2, AU_FILL_NONZERO, &p, c2d_state(c)->op, true);
}

void au_c2d_clip(AuCanvas2D *c, const AuCanvasPath *path, AuFillRule rule)
{
    C2DState *st = c2d_state(c);
    const AuPath *dev = dev_path(c, path);
    AuIRect full = {0, 0, c->w, c->h};
    AuIRect none = {0, 0, 0, 0};
    float b[4];
    if (!dev || !dev->nops || !au_path_bounds(dev, b) || au_path_edge_count(dev) > AU_C2D_MAX_EDGES) {
        st->clip_box = none; /* an empty clip: nothing draws */
        return;
    }
    AuIRect r;
    if (c2d_path_irect(dev, &r)) {
        st->clip_box = au_irect_intersect(au_irect_intersect(st->clip_box, r), full);
        return;
    }
    AuIRect pb = {(int)floorf(b[0]), (int)floorf(b[1]), (int)ceilf(b[2]) + 1, (int)ceilf(b[3]) + 1};
    AuIRect box = au_irect_intersect(au_irect_intersect(st->clip_box, pb), full);
    if (au_irect_empty(box) || c->inert) {
        st->clip_box = none;
        return;
    }
    C2DClip *k = c2d_clip_new(c);
    if (!k) {
        st->clip_box = none;
        return;
    }
    au_raster_mask(dev, rule, full, k->mask);
    if (st->clip) {
        const uint8_t *old = st->clip->mask;
        for (int y = box.y0; y < box.y1; y++) {
            uint8_t *row = k->mask + (size_t)y * (size_t)c->w;
            const uint8_t *orow = old + (size_t)y * (size_t)c->w;
            for (int x = box.x0; x < box.x1; x++)
                row[x] = (uint8_t)div255((uint32_t)row[x] * orow[x]);
        }
    }
    c2d_clip_unref(st->clip);
    st->clip = k;
    st->clip_box = box;
}

/* The winding number of the flattened path at a point decides. */
static bool point_in(const AuPath *dev, float x, float y, AuFillRule rule)
{
    if (!dev->nops || au_path_edge_count(dev) > AU_C2D_MAX_EDGES)
        return false;
    AuEdge *e;
    float b[4];
    size_t n = au_path_flatten(dev, 0, 0, &e, b);
    int wind = 0;
    for (size_t i = 0; i < n; i++) {
        float x0 = e[i].x0, y0 = e[i].y0, x1 = e[i].x1, y1 = e[i].y1;
        if (y0 <= y && y < y1) {
            if ((x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) < 0)
                wind++;
        } else if (y1 <= y && y < y0) {
            if ((x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) > 0)
                wind--;
        }
    }
    au_free(e);
    return rule == AU_FILL_EVENODD ? (wind & 1) != 0 : wind != 0;
}

bool au_c2d_point_in_path(AuCanvas2D *c, const AuCanvasPath *path, double x, double y, AuFillRule rule)
{
    if (!isfinite(x) || !isfinite(y))
        return false;
    const AuPath *dev = dev_path(c, path);
    return dev && point_in(dev, (float)x, (float)y, rule);
}

bool au_c2d_point_in_stroke(AuCanvas2D *c, const AuCanvasPath *path, double x, double y)
{
    if (!isfinite(x) || !isfinite(y))
        return false;
    const AuPath *dev = dev_path(c, path);
    if (!dev || !dev->nops || !stroke_outline(c, dev))
        return false;
    return point_in(&c->tmp2, (float)x, (float)y, AU_FILL_NONZERO);
}

/* ---- Rectangles ---- */

/* The device rectangle of a user rectangle when the transform keeps it
 * axis-aligned. */
static bool axis_rect(const AuMatrix *m, double x, double y, double w, double h, float r[4])
{
    if (m->b != 0 || m->c != 0)
        return false;
    float ax, ay, bx, by;
    if (!c2d_map(m, x, y, &ax, &ay) || !c2d_map(m, x + w, y + h, &bx, &by))
        return false;
    r[0] = ax < bx ? ax : bx;
    r[1] = ay < by ? ay : by;
    r[2] = ax < bx ? bx : ax;
    r[3] = ay < by ? by : ay;
    return true;
}

static void rect_path(AuCanvas2D *c, AuPath *out, double x, double y, double w, double h)
{
    AuCanvasPath tmp;
    tmp.p = *out;
    au_cpath_clear(&tmp);
    au_cpath_rect(&tmp, &c2d_state(c)->m, x, y, w, h);
    *out = tmp.p;
}

static void fill_rect_with(AuCanvas2D *c, double x, double y, double w, double h, const Paint *p, int op,
                           bool shadow)
{
    float r[4];
    if (!op_unbounded(op) && !(shadow && shadow_on(c, op)) && axis_rect(&c2d_state(c)->m, x, y, w, h, r)) {
        draw_dev_rect(c, r[0], r[1], r[2], r[3], p, op);
        return;
    }
    rect_path(c, &c->tmp, x, y, w, h);
    draw_dev_path(c, &c->tmp, AU_FILL_NONZERO, p, op, shadow);
}

void au_c2d_fill_rect(AuCanvas2D *c, double x, double y, double w, double h)
{
    Paint p;
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w == 0 || h == 0)
        return;
    if (!make_paint(c, AU_C2D_FILL, &p))
        return;
    fill_rect_with(c, x, y, w, h, &p, c2d_state(c)->op, true);
}

void au_c2d_clear_rect(AuCanvas2D *c, double x, double y, double w, double h)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w == 0 || h == 0)
        return;
    if (!c->surf)
        return; /* nothing drawn yet: already clear */
    /* Erasing is destination-out with an opaque source; on a canvas
     * without alpha the result is forced opaque, which is black. */
    Paint p;
    paint_solid(&p, 0xFF000000u, 1);
    fill_rect_with(c, x, y, w, h, &p, OP_DEST_OUT, false);
}

void au_c2d_stroke_rect(AuCanvas2D *c, double x, double y, double w, double h)
{
    Paint p;
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || (w == 0 && h == 0))
        return;
    if (!make_paint(c, AU_C2D_STROKE, &p))
        return;
    AuCanvasPath tmp;
    tmp.p = c->tmp;
    au_cpath_clear(&tmp);
    const AuMatrix *m = &c2d_state(c)->m;
    if (w == 0 || h == 0) {
        au_cpath_move_to(&tmp, m, x, y);
        au_cpath_line_to(&tmp, m, x + w, y + h);
    } else {
        au_cpath_rect(&tmp, m, x, y, w, h);
    }
    c->tmp = tmp.p;
    if (!stroke_outline(c, &c->tmp))
        return;
    draw_dev_path(c, &c->tmp2, AU_FILL_NONZERO, &p, c2d_state(c)->op, true);
}

/* ---- Text ---- */

typedef struct TextLayout {
    AuFontList *list;
    AuFontFace *face; /* the primary face */
    float size, width; /* width with spacing, before max_width */
    float ascent, descent, em_ascent, em_descent;
    float base;       /* added to y: the alphabetic baseline below the given one */
    float anchor;     /* subtracted from x: where the text starts, before the given point */
    char *owned;      /* the text with white space replaced, when it had any */
    const char *text;
    size_t len;
} TextLayout;

/* Shapes the text into c->run and works out the alignment. False when
 * there is nothing to draw or measure with. */
static bool text_layout(AuCanvas2D *c, const char *text, size_t len, TextLayout *t)
{
    memset(t, 0, sizeof *t);
    const C2DState *st = c2d_state(c);
    t->list = c2d_font_list(c);
    t->size = st->font->size;
    if (!t->list || !(t->size > 0))
        return false;
    t->face = au_fontlist_primary(t->list);
    if (!t->face)
        return false;
    if (len > AU_C2D_MAX_TEXT) {
        len = AU_C2D_MAX_TEXT;
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80)
            len--;
    }
    /* Tabs and line breaks draw as spaces. */
    t->text = text;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r') {
            if (!t->owned) {
                t->owned = au_alloc(len + 1);
                memcpy(t->owned, text, len);
                t->owned[len] = 0;
                t->text = t->owned;
            }
            t->owned[i] = ' ';
        }
    }
    t->len = len;
    au_shape_text_list(t->list, t->size, t->text, len, &c->run);
    /* letterSpacing after every glyph, wordSpacing after every space. */
    float ls = st->spacing[0], ws = st->spacing[1];
    if (ls != 0 || ws != 0) {
        float add = 0;
        for (size_t i = 0; i < c->run.n; i++) {
            AuGlyph *g = &c->run.glyphs[i];
            g->x += add;
            add += ls;
            if (g->cluster < len && t->text[g->cluster] == ' ')
                add += ws;
        }
        c->run.width += add;
    }
    t->width = c->run.width;
    AuFontMetrics m;
    au_font_metrics(t->face, t->size, &m);
    t->ascent = m.ascent;
    t->descent = m.descent;
    float sum = m.ascent + m.descent;
    t->em_ascent = sum > 0 ? t->size * m.ascent / sum : t->size;
    t->em_descent = t->size - t->em_ascent;
    switch (st->baseline) {
    case 1: t->base = t->em_ascent; break;                       /* top */
    case 2: t->base = m.ascent * 0.8f; break;                    /* hanging */
    case 3: t->base = (t->em_ascent - t->em_descent) / 2; break; /* middle */
    case 4:                                                      /* ideographic */
    case 5: t->base = -t->em_descent; break;                     /* bottom */
    default: t->base = 0; break;
    }
    bool rtl = st->direction == 2;
    int align = st->align; /* start, end, left, right, center */
    if (align == 0)
        align = rtl ? 3 : 2;
    else if (align == 1)
        align = rtl ? 2 : 3;
    t->anchor = align == 3 ? t->width : align == 4 ? t->width / 2 : 0;
    return true;
}

static void text_done(TextLayout *t)
{
    au_free(t->owned);
}

void au_c2d_measure_text(AuCanvas2D *c, const char *text, size_t len, AuTextMetrics2D *out)
{
    memset(out, 0, sizeof *out);
    TextLayout t;
    if (!text_layout(c, text, len, &t)) {
        text_done(&t);
        return;
    }
    out->width = t.width;
    /* The ink box, from each glyph's box in its font. */
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool any = false;
    for (size_t i = 0; i < c->run.n; i++) {
        const AuGlyph *g = &c->run.glyphs[i];
        const AuFontFace *f = g->face ? g->face : t.face;
        int16_t box[4];
        if (!f->units_per_em || !au_font_glyph_box(f, g->id, box))
            continue;
        float k = t.size / (float)f->units_per_em;
        float gx0 = g->x + box[0] * k, gx1 = g->x + box[2] * k;
        float gy0 = g->y - box[3] * k, gy1 = g->y - box[1] * k; /* y down */
        if (!any) {
            x0 = gx0, x1 = gx1, y0 = gy0, y1 = gy1;
            any = true;
        } else {
            x0 = gx0 < x0 ? gx0 : x0;
            x1 = gx1 > x1 ? gx1 : x1;
            y0 = gy0 < y0 ? gy0 : y0;
            y1 = gy1 > y1 ? gy1 : y1;
        }
    }
    out->left = t.anchor - x0;
    out->right = x1 - t.anchor;
    out->ascent = -y0 - t.base;
    out->descent = y1 + t.base;
    out->font_ascent = t.ascent - t.base;
    out->font_descent = t.descent + t.base;
    out->em_ascent = t.em_ascent - t.base;
    out->em_descent = t.em_descent + t.base;
    out->alphabetic = 0 - t.base; /* 0, not -0, for the alphabetic baseline */
    out->hanging = t.ascent * 0.8f - t.base;
    out->ideographic = -t.em_descent - t.base;
    text_done(&t);
}

void au_c2d_draw_text(AuCanvas2D *c, const char *text, size_t len, double x, double y, double max_width,
                      bool stroke)
{
    if (!isfinite(x) || !isfinite(y) || max_width != max_width || (max_width >= 0 && max_width == 0))
        return;
    Paint p;
    if (!make_paint(c, stroke ? AU_C2D_STROKE : AU_C2D_FILL, &p))
        return;
    TextLayout t;
    if (!text_layout(c, text, len, &t)) {
        text_done(&t);
        return;
    }
    /* A maximum width condenses the text horizontally to fit. */
    float xs = 1;
    if (max_width >= 0 && isfinite(max_width) && t.width > (float)max_width)
        xs = (float)max_width / t.width;
    /* Glyph outlines in user space (c->tmp), then through the transform. */
    au_path_clear(&c->tmp);
    float ox = (float)x - t.anchor * xs, oy = (float)y + t.base;
    if (isfinite(ox) && isfinite(oy) && fabsf(ox) < 1e7f && fabsf(oy) < 1e7f) {
        for (size_t i = 0; i < c->run.n && c->tmp.nops < AU_C2D_MAX_PATH_OPS; i++) {
            const AuGlyph *g = &c->run.glyphs[i];
            const AuFontFace *f = g->face ? g->face : t.face;
            if (!f->units_per_em)
                continue;
            float k = t.size / (float)f->units_per_em;
            au_font_glyph_path(f, g->id, k * xs, k, ox + g->x * xs, oy + g->y, &c->tmp);
        }
    }
    text_done(&t);
    if (!c->tmp.nops)
        return;
    const C2DState *st = c2d_state(c);
    AuPath dev;
    au_path_init(&dev);
    if (path_map(&dev, &c->tmp, &st->m)) {
        if (!stroke)
            draw_dev_path(c, &dev, AU_FILL_NONZERO, &p, st->op, true);
        else if (stroke_outline(c, &dev))
            draw_dev_path(c, &c->tmp2, AU_FILL_NONZERO, &p, st->op, true);
    }
    au_path_free(&dev);
}

/* ---- Images ---- */

static bool near_int(float v)
{
    return fabsf(v - floorf(v + 0.5f)) < 1e-3f;
}

/* Row-by-row source-over of an unscaled source rectangle. */
static void blit(AuCanvas2D *c, const AuSurface *src, bool opaque, int sx, int sy, AuIRect d, AuIRect vis)
{
    AuSurface *s = c->surf;
    int n = vis.x1 - vis.x0;
    for (int y = vis.y0; y < vis.y1; y++) {
        AuPixel *drow = s->pixels + (size_t)y * s->stride + vis.x0;
        const AuPixel *srow = src->pixels + (size_t)(sy + y - d.y0) * src->stride + sx + (vis.x0 - d.x0);
        if (opaque && src->opaque) {
            c2d_copy_pixels(drow, srow, (size_t)n);
        } else if (opaque) {
            for (int i = 0; i < n; i++)
                drow[i] = srow[i] | 0xFF000000u;
        } else {
            for (int i = 0; i < n; i++) {
                uint32_t px = srow[i];
                if (px)
                    drow[i] = (px >> 24) == 255 ? px : au_px_over(px, drow[i]);
            }
        }
    }
}

void au_c2d_draw_image(AuCanvas2D *c, const AuSurface *src, int nat_w, int nat_h, bool opaque, double sx,
                       double sy, double sw, double sh, double dx, double dy, double dw, double dh)
{
    if (!src || src->width <= 0 || src->height <= 0 || nat_w <= 0 || nat_h <= 0)
        return;
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(sw) || !isfinite(sh) || !isfinite(dx) || !isfinite(dy) ||
        !isfinite(dw) || !isfinite(dh))
        return;
    /* Negative sizes name the same rectangle from its other corner. */
    if (sw < 0) {
        sx += sw;
        sw = -sw;
    }
    if (sh < 0) {
        sy += sh;
        sh = -sh;
    }
    if (dw < 0) {
        dx += dw;
        dw = -dw;
    }
    if (dh < 0) {
        dy += dh;
        dh = -dh;
    }
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0)
        return;
    /* The source rectangle is cut to the image and the destination with
     * it, in proportion. */
    double kx = dw / sw, ky = dh / sh;
    if (sx < 0) {
        dx -= sx * kx;
        sw += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy -= sy * ky;
        sh += sy;
        sy = 0;
    }
    if (sx + sw > nat_w)
        sw = nat_w - sx;
    if (sy + sh > nat_h)
        sh = nat_h - sy;
    if (!(sw > 0) || !(sh > 0))
        return;
    dw = sw * kx;
    dh = sh * ky;
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(dw) || !isfinite(dh) || !(dw > 0) || !(dh > 0))
        return;
    if (!c2d_ensure_surface(c))
        return;
    /* A canvas drawn onto itself: draw from a copy. */
    AuSurface *copy = NULL;
    if (src == c->surf) {
        size_t n = (size_t)src->width * (size_t)src->height;
        AuPixel *px = au_try_alloc(n * sizeof(AuPixel));
        if (!px)
            return;
        for (int y = 0; y < src->height; y++)
            c2d_copy_pixels(px + (size_t)y * (size_t)src->width, src->pixels + (size_t)y * src->stride,
                            (size_t)src->width);
        copy = au_surface_wrap(px, src->width, src->height, src->width * 4);
        copy->opaque = src->opaque;
        src = copy;
    }
    const C2DState *st = c2d_state(c);
    /* Natural units to surface pixels. */
    double fx = (double)src->width / nat_w, fy = (double)src->height / nat_h;
    double ssx = sx * fx, ssy = sy * fy, ssw = sw * fx, ssh = sh * fy;
    opaque = opaque || src->opaque;
    float r[4];
    AuIRect box = clip_box_of(c);
    bool plain = st->op == OP_SOURCE_OVER && st->alpha >= 1 && !st->clip && !shadow_on(c, st->op);
    bool whole = plain && st->m.a > 0 && st->m.d > 0 && axis_rect(&st->m, dx, dy, dw, dh, r) && near_int(r[0]) &&
                 near_int(r[1]) && near_int(r[2]) && near_int(r[3]) && near_int((float)ssx) && near_int((float)ssy) &&
                 near_int((float)ssw) && near_int((float)ssh);
    if (whole) {
        /* Whole pixels: a copy when unscaled; the page painter's scaler
         * when it does not interpolate (nearest, or an averaging
         * reduction by 2 or more). Interpolation goes the general way,
         * which rounds. */
        float tw = r[2] - r[0], th = r[3] - r[1];
        bool same = fabsf(tw - (float)ssw) < 1e-3f && fabsf(th - (float)ssh) < 1e-3f;
        bool reduce = (float)ssw >= 2 * tw || (float)ssh >= 2 * th;
        whole = same || !st->smoothing || reduce;
    }
    if (whole) {
        AuIRect d = {(int)floorf(r[0] + 0.5f), (int)floorf(r[1] + 0.5f), (int)floorf(r[2] + 0.5f),
                     (int)floorf(r[3] + 0.5f)};
        AuIRect sr = {(int)floor(ssx + 0.5), (int)floor(ssy + 0.5), (int)floor(ssx + ssw + 0.5),
                      (int)floor(ssy + ssh + 0.5)};
        sr = au_irect_intersect(sr, (AuIRect){0, 0, src->width, src->height});
        AuIRect vis = au_irect_intersect(d, box);
        if (!au_irect_empty(vis) && !au_irect_empty(sr)) {
            if (d.x1 - d.x0 == sr.x1 - sr.x0 && d.y1 - d.y0 == sr.y1 - sr.y0) {
                blit(c, src, opaque, sr.x0, sr.y0, d, vis);
            } else {
                /* Scaled on whole pixels: the page painter's scaler, which
                 * averages when it reduces. */
                if (!c->pc)
                    c->pc = au_canvas_new(c->surf);
                AuSurface view = *src;
                view.opaque = opaque;
                au_canvas_clip_push(c->pc, (AuRect){(float)box.x0, (float)box.y0, (float)(box.x1 - box.x0),
                                                     (float)(box.y1 - box.y0)});
                au_draw_image(c->pc, &view, sr,
                              (AuRect){(float)d.x0, (float)d.y0, (float)(d.x1 - d.x0), (float)(d.y1 - d.y0)},
                              st->smoothing ? AU_FILTER_BILINEAR : AU_FILTER_NEAREST, 1);
                au_canvas_clip_pop(c->pc);
            }
            c2d_touch(c, vis);
        }
    } else {
        /* Any transform, operator, alpha, clip or shadow: the image is a
         * paint for the destination rectangle. */
        Paint p;
        memset(&p, 0, sizeof p);
        p.kind = PK_IMAGE;
        p.alpha256 = alpha256_of(st->alpha);
        p.img = src;
        p.opaque = opaque;
        p.clamp = true;
        p.ix0 = (int)floor(ssx);
        p.iy0 = (int)floor(ssy);
        p.ix1 = (int)ceil(ssx + ssw);
        p.iy1 = (int)ceil(ssy + ssh);
        if (p.ix0 < 0)
            p.ix0 = 0;
        if (p.iy0 < 0)
            p.iy0 = 0;
        if (p.ix1 > src->width)
            p.ix1 = src->width;
        if (p.iy1 > src->height)
            p.iy1 = src->height;
        /* Source pixel = (user - d) * (ss / dsize) + ss origin. */
        AuMatrix m = st->m;
        AuMatrix place = {(float)(dw / ssw), 0, 0, (float)(dh / ssh), (float)(dx - ssx * dw / ssw),
                          (float)(dy - ssy * dh / ssh)};
        au_matrix_multiply(&m, &place);
        if (p.ix1 > p.ix0 && p.iy1 > p.iy0 && invert_checked(&m, &p.inv)) {
            bool whole = p.inv.a == 1 && p.inv.b == 0 && p.inv.c == 0 && p.inv.d == 1 && near_int(p.inv.e) &&
                         near_int(p.inv.f);
            p.smooth = st->smoothing && !whole;
            fill_rect_with(c, dx, dy, dw, dh, &p, st->op, true);
        }
    }
    if (copy) {
        au_free(copy->pixels);
        au_surface_free(copy);
    }
}
