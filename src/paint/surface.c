/* Surfaces and the span blending loops. */
#include "paint_internal.h"

#include <math.h>
#include <string.h>

AuSurface *au_surface_new(int width, int height)
{
    AuSurface *s = au_calloc(sizeof *s);
    if (width < 0)
        width = 0;
    if (height < 0)
        height = 0;
    s->width = width;
    s->height = height;
    s->stride = width;
    s->owned = true;
    s->pixels = au_calloc((size_t)width * (size_t)height * sizeof(AuPixel));
    return s;
}

AuSurface *au_surface_wrap(void *pixels, int width, int height, int stride_bytes)
{
    AuSurface *s = au_calloc(sizeof *s);
    s->pixels = pixels;
    s->width = width;
    s->height = height;
    s->stride = stride_bytes / 4;
    s->owned = false;
    return s;
}

void au_surface_free(AuSurface *s)
{
    if (!s)
        return;
    if (s->owned)
        au_free(s->pixels);
    au_free(s);
}

void au_surface_clear(AuSurface *s, AuColor c)
{
    uint32_t p = au_premul(c);
    for (int y = 0; y < s->height; y++) {
        AuPixel *row = s->pixels + (size_t)y * s->stride;
        if (p == 0) {
            memset(row, 0, (size_t)s->width * sizeof(AuPixel));
        } else {
            for (int x = 0; x < s->width; x++)
                row[x] = p;
        }
    }
}

/* ---- Compact formats ----
 * Opaque images the image cache keeps in 16 or 8 bits (paint.h). A
 * channel of L levels is quantized with a 4 x 4 ordered dither: level
 * floor((v * (L - 1) + t) / 255), t from the Bayer matrix spread over
 * 0..254, so the average over a tile keeps the source value. */

static const uint8_t bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

static inline uint32_t quant(uint32_t v, uint32_t levels, uint32_t t)
{
    return (v * (levels - 1) + t) / 255;
}

static inline AuPixel rgb565_px(uint16_t v)
{
    uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

static AuPixel index8_lut[256];
static bool index8_ready;

static void index8_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint32_t r = 0, g = 0, b = 0;
        if (i < 252) {
            r = (uint32_t)(i / 42) * 51;
            g = (uint32_t)((i / 6) % 7) * 255 / 6;
            b = (uint32_t)(i % 6) * 51;
        }
        index8_lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    index8_ready = true;
}

AuPixel au_index8_colour(uint8_t i)
{
    if (!index8_ready)
        index8_init();
    return index8_lut[i];
}

size_t au_surface_bytes(const AuSurface *s)
{
    size_t per = s->format == AU_SURF_RGB565 ? 2 : s->format == AU_SURF_INDEX8 ? 1 : 4;
    return (size_t)s->stride * (size_t)s->height * per;
}

AuSurface *au_surface_convert(const AuSurface *s, uint8_t format)
{
    if (!s || s->format != AU_SURF_ARGB32 || !s->opaque || (format != AU_SURF_RGB565 && format != AU_SURF_INDEX8))
        return NULL;
    int w = s->width, h = s->height;
    size_t per = format == AU_SURF_RGB565 ? 2 : 1;
    void *data = au_alloc((size_t)w * (size_t)h * per + 1);
    for (int y = 0; y < h; y++) {
        const AuPixel *row = s->pixels + (size_t)y * s->stride;
        const uint8_t *bm = bayer4 + (y & 3) * 4;
        if (format == AU_SURF_RGB565) {
            uint16_t *o = (uint16_t *)data + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                uint32_t p = row[x], t = (uint32_t)bm[x & 3] * 255 / 16;
                o[x] = (uint16_t)((quant((p >> 16) & 255, 32, t) << 11) | (quant((p >> 8) & 255, 64, t) << 5) |
                                  quant(p & 255, 32, t));
            }
        } else {
            uint8_t *o = (uint8_t *)data + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                uint32_t p = row[x], t = (uint32_t)bm[x & 3] * 255 / 16;
                o[x] = (uint8_t)(quant((p >> 16) & 255, 6, t) * 42 + quant((p >> 8) & 255, 7, t) * 6 +
                                 quant(p & 255, 6, t));
            }
        }
    }
    AuSurface *c = au_calloc(sizeof *c);
    c->pixels = data;
    c->width = w;
    c->height = h;
    c->stride = w;
    c->owned = true;
    c->opaque = true;
    c->format = format;
    return c;
}

const AuPixel *au_surface_row(const AuSurface *s, int y, int x0, int n, AuPixel *tmp)
{
    if (s->format == AU_SURF_RGB565) {
        const uint16_t *in = (const uint16_t *)(const void *)s->pixels + (size_t)y * s->stride + x0;
        for (int i = 0; i < n; i++)
            tmp[i] = rgb565_px(in[i]);
        return tmp;
    }
    if (s->format == AU_SURF_INDEX8) {
        if (!index8_ready)
            index8_init();
        const uint8_t *in = (const uint8_t *)(const void *)s->pixels + (size_t)y * s->stride + x0;
        for (int i = 0; i < n; i++)
            tmp[i] = index8_lut[in[i]];
        return tmp;
    }
    return s->pixels + (size_t)y * s->stride + x0;
}

AuSurface *au_surface_expand(const AuSurface *s)
{
    AuPixel *data = au_alloc((size_t)s->width * (size_t)s->height * sizeof(AuPixel) + 1);
    for (int y = 0; y < s->height; y++) {
        AuPixel *o = data + (size_t)y * s->width;
        const AuPixel *r = au_surface_row(s, y, 0, s->width, o);
        if (r != o)
            memcpy(o, r, (size_t)s->width * sizeof *o);
    }
    AuSurface *c = au_calloc(sizeof *c);
    c->pixels = data;
    c->width = s->width;
    c->height = s->height;
    c->stride = s->width;
    c->owned = true;
    c->opaque = s->opaque;
    return c;
}

AuColor au_surface_get(const AuSurface *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->width || y >= s->height)
        return 0;
    AuPixel tmp;
    uint32_t p = *au_surface_row(s, y, x, 1, &tmp);
    uint32_t a = p >> 24;
    if (a == 0)
        return 0;
    if (a == 255)
        return p;
    uint32_t r = (((p >> 16) & 255) * 255 + a / 2) / a;
    uint32_t g = (((p >> 8) & 255) * 255 + a / 2) / a;
    uint32_t b = ((p & 255) * 255 + a / 2) / a;
    if (r > 255)
        r = 255;
    if (g > 255)
        g = 255;
    if (b > 255)
        b = 255;
    return AU_RGBA(r, g, b, a);
}

/* ---- Span blending ----
 * These are the innermost loops of all painting. Full coverage of an opaque
 * colour is a plain store; otherwise one scale and one "over" per pixel.
 * AltiVec: four pixels per iteration with vec_perm to splat coverage. */

void au_blend_solid_span(AuPixel *dst, int n, uint32_t src, const uint8_t *cov)
{
    uint32_t sa = src >> 24;
    if (!cov) {
        if (sa == 255) {
            for (int i = 0; i < n; i++)
                dst[i] = src;
        } else if (sa) {
            uint32_t inv = 256 - au_a256(sa);
            for (int i = 0; i < n; i++)
                dst[i] = src + au_px_scale(dst[i], inv);
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        uint32_t c = cov[i];
        if (c == 0)
            continue;
        if (c == 255) {
            if (sa == 255)
                dst[i] = src;
            else
                dst[i] = au_px_over(src, dst[i]);
        } else {
            dst[i] = au_px_over(au_px_scale(src, au_a256(c)), dst[i]);
        }
    }
}

static uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t t256)
{
    return au_px_scale_trunc(a, 256 - t256) + au_px_scale_trunc(b, t256);
}

void au_gradient_prepare(const AuGradient *g, float tx, float ty, AuGradientLut *out)
{
    float vx = g->x1 - g->x0, vy = g->y1 - g->y0;
    float len2 = vx * vx + vy * vy;
    out->ox = g->x0 + tx;
    out->oy = g->y0 + ty;
    out->radial = g->radial;
    out->repeat = g->repeat || g->spread == AU_SPREAD_REPEAT;
    out->reflect = !out->repeat && g->spread == AU_SPREAD_REFLECT;
    out->has_matrix = false;
    out->focal = false;
    if (g->has_matrix) {
        /* Surface pixels to gradient space: the inverse of the canvas
         * translation after the gradient's matrix. */
        AuMatrix fwd = {1, 0, 0, 1, tx, ty}, inv;
        au_matrix_multiply(&fwd, &g->matrix);
        if (au_matrix_invert(&fwd, &inv)) {
            out->has_matrix = true;
            out->m[0] = inv.a, out->m[1] = inv.b, out->m[2] = inv.c;
            out->m[3] = inv.d, out->m[4] = inv.e, out->m[5] = inv.f;
            out->ox = g->x0;
            out->oy = g->y0;
        } else {
            /* A degenerate matrix: the gradient collapses to its last
             * colour (SVG paints the last stop's colour). */
            out->has_matrix = true;
            memset(out->m, 0, sizeof out->m);
            out->ox = g->x0 - 1e9f;
            out->oy = g->y0;
        }
    }
    if (g->radial && g->focal) {
        float r = g->rx > 1e-3f ? g->rx : 1e-3f;
        float fx = g->fx, fy = g->fy;
        if (!g->has_matrix) {
            fx += tx;
            fy += ty;
        }
        float ddx = fx - out->ox, ddy = fy - out->oy;
        float d = sqrtf(ddx * ddx + ddy * ddy);
        if (d > r * 0.99f) {
            /* SVG 1.1: a focal point outside the circle moves onto it
             * (just inside, so the quadratic stays solvable). */
            float s = r * 0.99f / d;
            fx = out->ox + ddx * s;
            fy = out->oy + ddy * s;
            ddx *= s;
            ddy *= s;
        }
        out->focal = true;
        out->fx = fx;
        out->fy = fy;
        out->r = r;
        /* e = centre - focal; a = |e|^2 - r^2 (negative inside). */
        out->fa = ddx * ddx + ddy * ddy - r * r;
    }
    out->slow = out->radial || out->repeat || out->reflect || out->has_matrix;
    out->irx = g->rx > 1e-3f ? 1 / g->rx : 1e3f;
    out->iry = g->ry > 1e-3f ? 1 / g->ry : 1e3f;
    if (len2 < 1e-6f) {
        out->dx = out->dy = 0;
    } else {
        out->dx = vx / len2;
        out->dy = vy / len2;
    }
    /* Stops: positions are made non-decreasing as CSS requires. Colours
     * interpolate in premultiplied space. */
    int n = g->nstops;
    if (n <= 0) {
        for (int i = 0; i < 256; i++)
            out->lut[i] = 0;
        return;
    }
    float pos[AU_GRADIENT_MAX_STOPS];
    uint32_t col[AU_GRADIENT_MAX_STOPS];
    float last = 0;
    for (int i = 0; i < n; i++) {
        float p = g->stops[i].pos;
        if (i == 0 || p > last)
            last = p;
        pos[i] = last;
        col[i] = au_premul(g->stops[i].color);
    }
    int s = 0;
    /* A repeating gradient's table covers one period [0, 1). */
    float div = g->repeat ? 256.0f : 255.0f;
    for (int i = 0; i < 256; i++) {
        float t = i / div;
        while (s < n && pos[s] <= t)
            s++;
        uint32_t c;
        if (s == 0) {
            c = col[0];
        } else if (s == n) {
            c = col[n - 1];
        } else {
            float span = pos[s] - pos[s - 1];
            float f = span > 0 ? (t - pos[s - 1]) / span : 1.0f;
            /* f is 0 to 1 for ordered stops; a page's stops may not be numbers. */
            c = lerp_px(col[s - 1], col[s], f >= 1 ? 256u : f > 0 ? (uint32_t)(f * 256.0f + 0.5f) : 0u);
        }
        out->lut[i] = c;
    }
}

/* The table index for gradient position t (1.0 = the end of the line). */
static int lut_index(const AuGradientLut *g, float t)
{
    if (g->repeat) {
        float f = t - floorf(t); /* not a number when t is infinite */
        int i = au_f2i(f * 256.0f);
        return i < 0 ? 0 : i > 255 ? 255 : i;
    }
    if (g->reflect && isfinite(t)) {
        float f = fmodf(fabsf(t), 2.0f);
        t = f > 1 ? 2 - f : f;
    }
    if (!(t > 0))
        return 0;
    return t >= 1 ? 255 : (int)(t * 255.0f);
}

void au_blend_gradient_span(AuPixel *dst, int x, int y, int n, const AuGradientLut *g,
                            const uint8_t *cov)
{
    if (g->slow) {
        /* Per-pixel position: elliptical distance or the wrapped line
         * position. Rarer than plain linear gradients; kept simple. */
        float py = ((float)y + 0.5f - g->oy);
        for (int i = 0; i < n; i++) {
            uint32_t c = cov ? cov[i] : 255;
            if (c == 0)
                continue;
            float px = (float)(x + i) + 0.5f - g->ox;
            if (g->has_matrix) {
                float sx = (float)(x + i) + 0.5f, sy = (float)y + 0.5f;
                px = g->m[0] * sx + g->m[2] * sy + g->m[4] - g->ox;
                py = g->m[1] * sx + g->m[3] * sy + g->m[5] - g->oy;
            }
            float t;
            if (g->focal) {
                /* The circle through this point, of the family centred
                 * f + t (c - f) with radius t r: solve
                 * (|e|^2 - r^2) t^2 - 2 (d.e) t + |d|^2 = 0 for t >= 0,
                 * d = p - f, e = c - f. */
                float dx = px + g->ox - g->fx, dy = py + g->oy - g->fy;
                float ex = g->ox - g->fx, ey = g->oy - g->fy;
                float de = dx * ex + dy * ey, dd = dx * dx + dy * dy;
                float disc = de * de - g->fa * dd;
                t = disc >= 0 && g->fa < 0 ? (de - sqrtf(disc)) / g->fa : 0;
            } else if (g->radial) {
                float u = px * g->irx, v = py * g->iry;
                t = sqrtf(u * u + v * v);
            } else {
                t = px * g->dx + py * g->dy;
            }
            uint32_t src = g->lut[lut_index(g, t)];
            if (c != 255)
                src = au_px_scale(src, au_a256(c));
            dst[i] = au_px_over(src, dst[i]);
        }
        return;
    }
    /* t moves by g->dx per pixel; step it in 16.16 fixed point. */
    float t0 = ((float)x + 0.5f - g->ox) * g->dx + ((float)y + 0.5f - g->oy) * g->dy;
    float t0c = t0 * 255.0f * 65536.0f;
    float stc = g->dx * 255.0f * 65536.0f;
    /* 64-bit so long spans of a short gradient cannot overflow; the values
     * are clamped to stay far from the int64 limits. */
    if (t0c > 1e12f)
        t0c = 1e12f;
    if (t0c < -1e12f)
        t0c = -1e12f;
    if (stc > 1e9f)
        stc = 1e9f;
    if (stc < -1e9f)
        stc = -1e9f;
    /* NaN passes the clamps above (a gradient with a NaN stop). */
    if (t0c != t0c)
        t0c = 0;
    if (stc != stc)
        stc = 0;
    int64_t t = (int64_t)t0c, step = (int64_t)stc;
    for (int i = 0; i < n; i++, t += step) {
        uint32_t c = cov ? cov[i] : 255;
        if (c == 0)
            continue;
        int64_t idx = t >> 16;
        if (idx < 0)
            idx = 0;
        else if (idx > 255)
            idx = 255;
        uint32_t src = g->lut[idx];
        if (c != 255)
            src = au_px_scale(src, au_a256(c));
        dst[i] = au_px_over(src, dst[i]);
    }
}
