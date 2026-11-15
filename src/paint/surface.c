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

AuColor au_surface_get(const AuSurface *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->width || y >= s->height)
        return 0;
    uint32_t p = s->pixels[(size_t)y * s->stride + x];
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
    out->repeat = g->repeat;
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
            c = lerp_px(col[s - 1], col[s], (uint32_t)(f * 256.0f + 0.5f));
        }
        out->lut[i] = c;
    }
}

/* The table index for gradient position t (1.0 = the end of the line). */
static int lut_index(const AuGradientLut *g, float t)
{
    if (g->repeat) {
        float f = t - floorf(t);
        int i = (int)(f * 256.0f);
        return i < 0 ? 0 : i > 255 ? 255 : i;
    }
    if (!(t > 0))
        return 0;
    return t >= 1 ? 255 : (int)(t * 255.0f);
}

void au_blend_gradient_span(AuPixel *dst, int x, int y, int n, const AuGradientLut *g,
                            const uint8_t *cov)
{
    if (g->radial || g->repeat) {
        /* Per-pixel position: elliptical distance or the wrapped line
         * position. Rarer than plain linear gradients; kept simple. */
        float py = ((float)y + 0.5f - g->oy);
        for (int i = 0; i < n; i++) {
            uint32_t c = cov ? cov[i] : 255;
            if (c == 0)
                continue;
            float px = (float)(x + i) + 0.5f - g->ox;
            float t;
            if (g->radial) {
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
