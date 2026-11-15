/* Shared internals of src/paint. Not for use outside the folder. */
#ifndef AURA_PAINT_INTERNAL_H
#define AURA_PAINT_INTERNAL_H

#include "paint.h"

/* ---- Pixel arithmetic ----
 * Two channels at a time: (p & 0x00FF00FF) holds red and blue, (p >> 8) &
 * 0x00FF00FF holds alpha and green. Scale factors are 0..256 so that a
 * multiply and a shift replace a divide by 255.
 * AltiVec: these become vec_mule/vec_mulo over four pixels. */

/* Rounds to nearest, so that "over" an opaque pixel stays opaque. A
 * product is at most 255 * 256 + 128, which fits its 16-bit field. */
static inline uint32_t au_px_scale(uint32_t p, uint32_t a256)
{
    uint32_t rb = ((((p & 0x00FF00FFu) * a256) + 0x00800080u) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((p >> 8) & 0x00FF00FFu) * a256) + 0x00800080u) & 0xFF00FF00u;
    return rb | ag;
}

/* Truncating scale, for interpolation: scale(a, 256 - t) + scale(b, t)
 * must not exceed 255 in any channel, which rounding both terms could. */
static inline uint32_t au_px_scale_trunc(uint32_t p, uint32_t a256)
{
    uint32_t rb = (((p & 0x00FF00FFu) * a256) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((p >> 8) & 0x00FF00FFu) * a256) & 0xFF00FF00u;
    return rb | ag;
}

/* Source over destination, both premultiplied. */
static inline uint32_t au_px_over(uint32_t src, uint32_t dst)
{
    uint32_t sa = src >> 24;
    return src + au_px_scale(dst, 256 - (sa + (sa >> 7)));
}

/* 0..255 to 0..256. */
static inline uint32_t au_a256(uint32_t a)
{
    return a + (a >> 7);
}

static inline uint32_t au_premul(AuColor c)
{
    uint32_t a = c >> 24;
    if (a == 255)
        return c;
    return (au_px_scale(c | 0xFF000000u, au_a256(a)) & 0x00FFFFFFu) | (a << 24);
}

static inline int au_imin(int a, int b) { return a < b ? a : b; }
static inline int au_imax(int a, int b) { return a > b ? a : b; }

static inline AuIRect au_irect_intersect(AuIRect a, AuIRect b)
{
    AuIRect r = {au_imax(a.x0, b.x0), au_imax(a.y0, b.y0), au_imin(a.x1, b.x1), au_imin(a.y1, b.y1)};
    if (r.x1 < r.x0)
        r.x1 = r.x0;
    if (r.y1 < r.y0)
        r.y1 = r.y0;
    return r;
}

static inline bool au_irect_empty(AuIRect r)
{
    return r.x1 <= r.x0 || r.y1 <= r.y0;
}

/* ---- Span blending (surface.c) ---- */

/* Blend premultiplied colour src over n pixels, each scaled by cov[i]
 * (0..255); cov NULL means full coverage. */
void au_blend_solid_span(AuPixel *dst, int n, uint32_t src, const uint8_t *cov);

/* A gradient prepared for drawing: a 256-entry premultiplied lookup
 * table and the projection onto the gradient line. */
typedef struct AuGradientLut {
    uint32_t lut[256];
    float ox, oy;   /* gradient start, surface pixels */
    float dx, dy;   /* t = (px - ox) * dx + (py - oy) * dy, 0..1 */
    float irx, iry; /* radial: t = |((px - ox) * irx, (py - oy) * iry)| */
    bool radial, repeat;
} AuGradientLut;

void au_gradient_prepare(const AuGradient *g, float tx, float ty, AuGradientLut *out);
void au_blend_gradient_span(AuPixel *dst, int x, int y, int n, const AuGradientLut *g,
                            const uint8_t *cov);

/* ---- Canvas internals used by text.c ---- */

/* The target surface of the innermost group (or the base surface), the
 * offset from canvas pixels to that surface, and the clip in its pixels. */
AuSurface *au_canvas_current(AuCanvas *c, int *ox, int *oy, AuIRect *clip);

/* ---- Path flattening (path.c) ---- */

typedef struct AuEdge {
    float x0, y0, x1, y1;
} AuEdge;

/* Flattens p translated by (dx, dy) into line edges (horizontal ones
 * dropped; subpaths closed). Returns the count; *edges is au_alloc'd. The
 * bounding box of all points goes in *bounds (x0, y0, x1, y1). */
size_t au_path_flatten(const AuPath *p, float dx, float dy, AuEdge **edges, float bounds[4]);

#endif
