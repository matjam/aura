/* The coverage rasterizer.
 *
 * Signed-area accumulation, as in font-rs and stb_truetype 2: each edge
 * adds, to the cells it crosses, the signed area it sweeps to its right
 * within each row. A running sum along a row then gives the winding-weighted
 * coverage of every pixel, exact for antialiasing purposes, with no
 * per-pixel edge sorting.
 *
 * Fill rules: nonzero takes min(|sum|, 1). Even-odd folds the sum into
 * 0..1 (|sum| mod 2, mirrored above 1), exact inside shapes and a close
 * approximation on pixels where several edges of opposite sense meet.
 *
 * Memory: the work area is one strip of STRIP rows by the clipped width,
 * so a full-screen path needs a few tens of kilobytes, not a full-size
 * float buffer. Edges are sorted by top and kept in an active list as the
 * strips move down. */
#include "paint_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STRIP 16

typedef struct {
    float *acc;   /* STRIP rows of (w + 2) cells */
    int w, rows;  /* strip width in pixels, rows in this strip */
    int stride;   /* w + 2 */
} Strip;

/* Accumulates one edge, in strip coordinates (x in 0..w, y in 0..rows,
 * clipped by the caller in x; y may extend past the strip). */
static void acc_line(Strip *s, float x0, float y0, float x1, float y1)
{
    float dir = 1.0f;
    if (y0 > y1) {
        float t;
        t = x0, x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
        dir = -1.0f;
    }
    /* An edge flatter than this covers nothing that shows, and its
     * slope would overflow: dx / dy must stay finite, or the cell indices
     * below are not numbers (coordinates are below 1e6, see add_edge). */
    if (y1 <= 0 || y0 >= s->rows || !(y1 - y0 > 1e-7f))
        return;
    float dxdy = (x1 - x0) / (y1 - y0);
    int ys = y0 < 0 ? 0 : (int)y0;
    int ye = (int)ceilf(y1);
    if (ye > s->rows)
        ye = s->rows;
    float ystart = y0 < (float)ys ? (float)ys : y0;
    float x = x0 + (ystart - y0) * dxdy;
    float maxx = (float)s->w;
    for (int y = ys; y < ye; y++) {
        float *row = s->acc + (size_t)y * s->stride;
        float ytop = (float)y > y0 ? (float)y : y0;
        float ybot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = ybot - ytop;
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        if (xa < 0)
            xa = 0;
        if (xb < 0)
            xb = 0;
        if (xa > maxx)
            xa = maxx;
        if (xb > maxx)
            xb = maxx;
        float xaf = floorf(xa);
        int xai = (int)xaf;
        float xbc = ceilf(xb);
        int xbi = (int)xbc;
        if (xbi <= xai + 1) {
            /* Within one cell: split by the mean x. */
            float xm = 0.5f * (xa + xb) - xaf;
            row[xai] += d - d * xm;
            row[xai + 1] += d * xm;
        } else {
            float sl = 1.0f / (xb - xa);
            float x0f = xa - xaf;
            float a0 = 0.5f * sl * (1.0f - x0f) * (1.0f - x0f);
            float x1f = xb - xbc + 1.0f;
            float am = 0.5f * sl * x1f * x1f;
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = sl * (1.5f - x0f);
                row[xai + 1] += d * (a1 - a0);
                float ds = d * sl;
                for (int xi = xai + 2; xi < xbi - 1; xi++)
                    row[xi] += ds;
                float a2 = a1 + (float)(xbi - xai - 3) * sl;
                row[xbi - 1] += d * (1.0f - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xnext;
    }
}

/* Clips an edge to x >= 0 in strip coordinates. The part left of the strip
 * becomes a vertical edge at x = 0, which carries the same winding into
 * the row. The part right of w is clamped by acc_line (it lands in the
 * cells past w, which are never read). */
static void add_clipped(Strip *s, float x0, float y0, float x1, float y1)
{
    if (x0 >= 0 && x1 >= 0) {
        acc_line(s, x0, y0, x1, y1);
        return;
    }
    if (x0 < 0 && x1 < 0) {
        acc_line(s, 0, y0, 0, y1);
        return;
    }
    float t = (0 - x0) / (x1 - x0);
    float ym = y0 + (y1 - y0) * t;
    if (x0 < 0) {
        acc_line(s, 0, y0, 0, ym);
        acc_line(s, 0, ym, x1, y1);
    } else {
        acc_line(s, x0, y0, 0, ym);
        acc_line(s, 0, ym, 0, y1);
    }
}

static int cmp_edge_top(const void *a, const void *b)
{
    const AuEdge *ea = a, *eb = b;
    float ta = ea->y0 < ea->y1 ? ea->y0 : ea->y1;
    float tb = eb->y0 < eb->y1 ? eb->y0 : eb->y1;
    return ta < tb ? -1 : ta > tb;
}

static inline float etop(const AuEdge *e) { return e->y0 < e->y1 ? e->y0 : e->y1; }
static inline float ebot(const AuEdge *e) { return e->y0 < e->y1 ? e->y1 : e->y0; }

/* Small paths (glyphs, most of all) use work areas on the stack: up to
 * RASTER_STACK_W pixels wide and RASTER_STACK_EDGES edges. */
#define RASTER_STACK_W 62
#define RASTER_STACK_EDGES 96

/* Rasterizes p. Spans go to fn, or, when mask is not NULL, coverage bytes
 * go straight into mask: one byte per pixel of clip, rows of clip's
 * width. Only covered pixels are written, as the span callback does. */
static void raster(const AuPath *p, float dx, float dy, AuFillRule rule, AuIRect clip, AuSpanFn fn, void *ctx,
                   uint8_t *mask)
{
    AuEdge *edges;
    float b[4];
    /* The whole pixels of the offset move the result and nothing else:
     * the path is flattened with the fraction only and the area and the
     * spans are shifted. Adding the offset to the coordinates would round
     * them differently at each offset, and the same shape painted after a
     * scroll would differ in a coverage level here and there (scrolling
     * moves pixels and compares equal to a fresh paint only if drawing
     * does not depend on where the page is scrolled to). */
    int ix = (int)floorf(dx), iy = (int)floorf(dy);
    clip.x0 -= ix;
    clip.x1 -= ix;
    clip.y0 -= iy;
    clip.y1 -= iy;
    size_t ne = au_path_flatten(p, dx - (float)ix, dy - (float)iy, &edges, b);
    if (ne == 0) {
        au_free(edges);
        return;
    }
    AuIRect area = {(int)floorf(b[0]), (int)floorf(b[1]), (int)ceilf(b[2]) + 1, (int)ceilf(b[3]) + 1};
    area = au_irect_intersect(area, clip);
    if (au_irect_empty(area)) {
        au_free(edges);
        return;
    }
    qsort(edges, ne, sizeof(AuEdge), cmp_edge_top);

    Strip s;
    s.w = area.x1 - area.x0;
    s.stride = s.w + 2;
    float acc_stack[(RASTER_STACK_W + 2) * STRIP];
    uint8_t cov_stack[RASTER_STACK_W];
    size_t active_stack[RASTER_STACK_EDGES];
    bool small = s.w <= RASTER_STACK_W;
    if (small) {
        s.acc = acc_stack;
        memset(acc_stack, 0, (size_t)s.stride * STRIP * sizeof(float));
    } else {
        s.acc = au_calloc((size_t)s.stride * STRIP * sizeof(float));
    }
    uint8_t *cov = small ? cov_stack : au_alloc((size_t)s.w);
    size_t *active = ne <= RASTER_STACK_EDGES ? active_stack : au_alloc(ne * sizeof(size_t));
    int mask_w = clip.x1 - clip.x0;
    size_t nactive = 0, next = 0;
    bool evenodd = rule == AU_FILL_EVENODD;
    float fx0 = (float)area.x0;

    for (int sy = area.y0; sy < area.y1; sy += STRIP) {
        s.rows = au_imin(STRIP, area.y1 - sy);
        float fsy = (float)sy, fsy1 = (float)(sy + s.rows);
        /* Drop edges that ended above this strip; add ones that start in it. */
        size_t k = 0;
        for (size_t i = 0; i < nactive; i++)
            if (ebot(&edges[active[i]]) > fsy)
                active[k++] = active[i];
        nactive = k;
        while (next < ne && etop(&edges[next]) < fsy1) {
            if (ebot(&edges[next]) > fsy)
                active[nactive++] = next;
            next++;
        }
        if (nactive == 0)
            continue;
        for (size_t i = 0; i < nactive; i++) {
            const AuEdge *e = &edges[active[i]];
            add_clipped(&s, e->x0 - fx0, e->y0 - fsy, e->x1 - fx0, e->y1 - fsy);
        }
        /* Resolve each row: a running sum, then the fill rule.
         * AltiVec: the prefix sum is serial, but the conversion to bytes
         * (vec_cts, vec_packs) can take four cells at a time. */
        for (int r = 0; r < s.rows; r++) {
            float *row = s.acc + (size_t)r * s.stride;
            float sum = 0;
            int run = -1;
            if (mask && !evenodd) {
                /* Bytes straight into the mask. Empty and full cells, most
                 * of a glyph's box, skip the conversion to an integer (the
                 * same value either way). */
                uint8_t *out = mask + (size_t)(sy + r - clip.y0) * (size_t)mask_w + (size_t)(area.x0 - clip.x0);
                for (int x = 0; x < s.w; x++) {
                    sum += row[x];
                    row[x] = 0;
                    float a = sum < 0 ? -sum : sum;
                    if (a >= 1.0f) {
                        out[x] = 255;
                    } else if (a > 0) {
                        uint8_t c = (uint8_t)(a * 255.0f + 0.5f);
                        if (c)
                            out[x] = c;
                    }
                }
                row[s.w] = row[s.w + 1] = 0;
                continue;
            }
            for (int x = 0; x < s.w; x++) {
                sum += row[x];
                row[x] = 0;
                float a = sum < 0 ? -sum : sum;
                if (evenodd) {
                    a = fmodf(a, 2.0f);
                    if (a > 1.0f)
                        a = 2.0f - a;
                } else if (a > 1.0f) {
                    a = 1.0f;
                }
                if (!(a >= 0))
                    a = 0; /* not a number: nothing */
                uint8_t c = (uint8_t)(a * 255.0f + 0.5f);
                cov[x] = c;
                if (c) {
                    if (run < 0)
                        run = x;
                } else if (run >= 0) {
                    fn(ctx, sy + r + iy, area.x0 + run + ix, x - run, cov + run);
                    run = -1;
                }
            }
            row[s.w] = row[s.w + 1] = 0;
            if (run >= 0)
                fn(ctx, sy + r + iy, area.x0 + run + ix, s.w - run, cov + run);
        }
    }
    if (active != active_stack)
        au_free(active);
    if (!small) {
        au_free(cov);
        au_free(s.acc);
    }
    au_free(edges);
}

void au_raster_path(const AuPath *p, float dx, float dy, AuFillRule rule, AuIRect clip, AuSpanFn fn,
                    void *ctx)
{
    raster(p, dx, dy, rule, clip, fn, ctx, NULL);
}

typedef struct {
    uint8_t *mask;
    AuIRect area;
} MaskCtx;

static void mask_span(void *ctx, int y, int x, int n, const uint8_t *cov)
{
    MaskCtx *m = ctx;
    uint8_t *dst = m->mask + (size_t)(y - m->area.y0) * (m->area.x1 - m->area.x0) + (x - m->area.x0);
    memcpy(dst, cov, (size_t)n);
}

void au_raster_mask(const AuPath *p, AuFillRule rule, AuIRect area, uint8_t *mask)
{
    MaskCtx m = {mask, area};
    raster(p, 0, 0, rule, area, mask_span, &m, mask);
}
