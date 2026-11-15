/* Borders and box shadows. */
#include "paint_internal.h"

#include <math.h>
#include <string.h>

/* The inner edge of a border: the box shrunk by the widths, with radii
 * reduced by the adjacent widths (CSS Backgrounds 3, 5.2). */
static void inner_shape(AuRect box, const float w[4], const AuRadii *outer, AuRect *in, AuRadii *inr)
{
    in->x = box.x + w[AU_SIDE_LEFT];
    in->y = box.y + w[AU_SIDE_TOP];
    in->w = box.w - w[AU_SIDE_LEFT] - w[AU_SIDE_RIGHT];
    in->h = box.h - w[AU_SIDE_TOP] - w[AU_SIDE_BOTTOM];
    /* corner -> (horizontal side width, vertical side width) */
    static const int hs[4] = {AU_SIDE_LEFT, AU_SIDE_RIGHT, AU_SIDE_RIGHT, AU_SIDE_LEFT};
    static const int vs[4] = {AU_SIDE_TOP, AU_SIDE_TOP, AU_SIDE_BOTTOM, AU_SIDE_BOTTOM};
    for (int i = 0; i < 4; i++) {
        float rx = outer ? outer->rx[i] - w[hs[i]] : 0;
        float ry = outer ? outer->ry[i] - w[vs[i]] : 0;
        inr->rx[i] = rx > 0 ? rx : 0;
        inr->ry[i] = ry > 0 ? ry : 0;
    }
}

typedef struct {
    AuSurface *s;
    uint32_t color;
    const uint8_t *ring; /* ring coverage over area */
    AuIRect area;
    uint8_t *tmp;
    int ix, iy; /* whole pixels from the area's coordinates to the surface's */
} SideCtx;

static void side_span(void *ctx, int y, int x, int n, const uint8_t *cov)
{
    SideCtx *sc = ctx;
    const uint8_t *ring = sc->ring + (size_t)(y - sc->area.y0) * (sc->area.x1 - sc->area.x0) + (x - sc->area.x0);
    for (int i = 0; i < n; i++)
        sc->tmp[i] = (uint8_t)((cov[i] * au_a256(ring[i])) >> 8);
    au_blend_solid_span(sc->s->pixels + (size_t)(y + sc->iy) * sc->s->stride + (x + sc->ix), n, sc->color, sc->tmp);
}

void au_draw_border(AuCanvas *c, AuRect box, const AuBorder *b, const AuRadii *radii)
{
    if (!(box.w > 0 && box.h > 0))
        return;
    float w[4];
    bool any = false, same = true;
    int ref = -1; /* the first side with a width: the others compare with it */
    for (int i = 0; i < 4; i++) {
        w[i] = b->width[i] > 0 ? b->width[i] : 0;
        if (w[i] > 0 && (b->color[i] >> 24))
            any = true;
        if (w[i] > 0 && ref < 0)
            ref = i;
        if (w[i] > 0 && b->color[i] != b->color[ref])
            same = false;
    }
    if (!any)
        return;
    /* Widths larger than the box are scaled down to fit it. */
    if (w[AU_SIDE_LEFT] + w[AU_SIDE_RIGHT] > box.w) {
        float f = box.w / (w[AU_SIDE_LEFT] + w[AU_SIDE_RIGHT]);
        w[AU_SIDE_LEFT] *= f;
        w[AU_SIDE_RIGHT] *= f;
    }
    if (w[AU_SIDE_TOP] + w[AU_SIDE_BOTTOM] > box.h) {
        float f = box.h / (w[AU_SIDE_TOP] + w[AU_SIDE_BOTTOM]);
        w[AU_SIDE_TOP] *= f;
        w[AU_SIDE_BOTTOM] *= f;
    }
    AuRadii outer = {{0}, {0}};
    bool rounded = !au_radii_zero(radii);
    if (rounded) {
        outer = *radii;
        au_radii_fit(&outer, box);
    }
    AuColor first = 0;
    for (int i = 0; i < 4; i++)
        if (w[i] > 0) {
            first = b->color[i];
            break;
        }
    AuRect in;
    AuRadii inr;
    inner_shape(box, w, rounded ? &outer : NULL, &in, &inr);

    /* Square corners and one colour per side: four rectangles, mitred by
     * the side order (top and bottom span the full width). Only when all
     * sides share a colour, so the mitres are invisible. */
    if (!rounded && same) {
        au_fill_rect(c, (AuRect){box.x, box.y, box.w, w[AU_SIDE_TOP]}, first);
        au_fill_rect(c, (AuRect){box.x, box.y + box.h - w[AU_SIDE_BOTTOM], box.w, w[AU_SIDE_BOTTOM]}, first);
        float my = box.y + w[AU_SIDE_TOP], mh = box.h - w[AU_SIDE_TOP] - w[AU_SIDE_BOTTOM];
        au_fill_rect(c, (AuRect){box.x, my, w[AU_SIDE_LEFT], mh}, first);
        au_fill_rect(c, (AuRect){box.x + box.w - w[AU_SIDE_RIGHT], my, w[AU_SIDE_RIGHT], mh}, first);
        return;
    }

    AuPath ring;
    au_path_init(&ring);
    au_path_rounded_rect(&ring, box, rounded ? &outer : NULL, false);
    if (in.w > 0 && in.h > 0)
        au_path_rounded_rect(&ring, in, &inr, true);
    if (same) {
        au_fill_path(c, &ring, AU_FILL_NONZERO, first);
        au_path_free(&ring);
        return;
    }

    /* Different colours: rasterize the ring once into a mask, then each
     * side's wedge (bounded by the lines from the outer corners through
     * the inner corners) through it. */
    int ox, oy;
    AuIRect clip;
    float tx, ty;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    au_canvas_get_translation(c, &tx, &ty);
    /* Work with the fraction of the translation; its whole pixels are
     * added when blending (see raster() in raster.c). */
    int ix = au_f2px(floorf(tx - (float)ox)), iy = au_f2px(floorf(ty - (float)oy));
    float dx = tx - (float)ox - (float)ix, dy = ty - (float)oy - (float)iy;
    clip.x0 -= ix;
    clip.x1 -= ix;
    clip.y0 -= iy;
    clip.y1 -= iy;
    AuIRect area = {au_f2px(floorf(box.x + dx)), au_f2px(floorf(box.y + dy)), au_f2px(ceilf(box.x + box.w + dx)) + 1,
                    au_f2px(ceilf(box.y + box.h + dy)) + 1};
    area = au_irect_intersect(area, clip);
    if (au_irect_empty(area)) {
        au_path_free(&ring);
        return;
    }
    size_t aw = (size_t)(area.x1 - area.x0), ah = (size_t)(area.y1 - area.y0);
    uint8_t *mask = au_calloc(aw * ah);
    /* au_raster_mask takes surface pixels: move the ring there first. */
    for (size_t i = 0; i < ring.npts; i += 2) {
        ring.pts[i] += dx;
        ring.pts[i + 1] += dy;
    }
    au_raster_mask(&ring, AU_FILL_NONZERO, area, mask);
    float bx0 = box.x + dx, by0 = box.y + dy, bx1 = bx0 + box.w, by1 = by0 + box.h;
    /* How far along each corner's diagonal (outer corner to inner corner)
     * the wedges reach: far enough to cover the ring, not past the box. */
    float kh = w[AU_SIDE_LEFT] + w[AU_SIDE_RIGHT] > 0 ? box.w / (w[AU_SIDE_LEFT] + w[AU_SIDE_RIGHT]) : 1e6f;
    float kv = w[AU_SIDE_TOP] + w[AU_SIDE_BOTTOM] > 0 ? box.h / (w[AU_SIDE_TOP] + w[AU_SIDE_BOTTOM]) : 1e6f;
    float k = kh < kv ? kh : kv;
    float wt = w[AU_SIDE_TOP] * k, wr = w[AU_SIDE_RIGHT] * k, wb = w[AU_SIDE_BOTTOM] * k, wl = w[AU_SIDE_LEFT] * k;
    /* The four inner reach points, on the diagonals. */
    float tlx = bx0 + wl, tly = by0 + wt, trx = bx1 - wr, try_ = by0 + wt;
    float brx = bx1 - wr, bry = by1 - wb, blx = bx0 + wl, bly = by1 - wb;
    float quads[4][8] = {
        {bx0, by0, bx1, by0, trx, try_, tlx, tly}, /* top */
        {bx1, by0, bx1, by1, brx, bry, trx, try_}, /* right */
        {bx1, by1, bx0, by1, blx, bly, brx, bry},  /* bottom */
        {bx0, by1, bx0, by0, tlx, tly, blx, bly},  /* left */
    };
    uint8_t *tmp = au_alloc(aw);
    for (int side = 0; side < 4; side++) {
        if (!(w[side] > 0) || !(b->color[side] >> 24))
            continue;
        AuPath q;
        au_path_init(&q);
        au_path_move_to(&q, quads[side][0], quads[side][1]);
        au_path_line_to(&q, quads[side][2], quads[side][3]);
        au_path_line_to(&q, quads[side][4], quads[side][5]);
        au_path_line_to(&q, quads[side][6], quads[side][7]);
        au_path_close(&q);
        SideCtx sc = {s, au_premul(b->color[side]), mask, area, tmp, ix, iy};
        au_raster_path(&q, 0, 0, AU_FILL_NONZERO, area, side_span, &sc);
        au_path_free(&q);
    }
    au_free(tmp);
    au_free(mask);
    au_path_free(&ring);
}

/* ---- Box shadows ---- */

/* One box blur pass of radius r over n values with stride, zero outside.
 * tmp holds n bytes. Running sum: O(n) whatever the radius.
 * AltiVec: blur columns four at a time instead of one. */
static void box_pass(uint8_t *p, int n, int stride, int r, uint8_t *tmp)
{
    if (r <= 0 || n <= 0)
        return;
    int w = 2 * r + 1;
    uint32_t div = (uint32_t)w;
    /* Fixed-point reciprocal for the divide: sum * inv >> 16. */
    uint32_t inv = (65536u + div / 2) / div;
    uint32_t sum = 0;
    for (int i = 0; i < n; i++)
        tmp[i] = p[(size_t)i * stride];
    for (int i = 0; i < r && i < n; i++)
        sum += tmp[i];
    for (int i = 0; i < n; i++) {
        int add = i + r, sub = i - r - 1;
        if (add < n)
            sum += tmp[add];
        if (sub >= 0)
            sum -= tmp[sub];
        uint32_t v = (sum * inv + 32768) >> 16;
        p[(size_t)i * stride] = (uint8_t)(v > 255 ? 255 : v);
    }
}

void au_blur_mask(uint8_t *m, int w, int h, float sigma);
void au_blur_mask(uint8_t *m, int w, int h, float sigma)
{
    if (!(sigma > 0.25f))
        return;
    /* Three boxes of width d approximate a Gaussian of variance
     * 3 * (d^2 - 1) / 12 = sigma^2. */
    float d = sqrtf(4.0f * sigma * sigma + 1.0f);
    int r = (int)floorf((d - 1.0f) / 2.0f + 0.5f);
    if (r < 1)
        r = 1;
    uint8_t *tmp = au_alloc((size_t)(w > h ? w : h));
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++)
            box_pass(m + (size_t)y * w, w, 1, r, tmp);
        for (int x = 0; x < w; x++)
            box_pass(m + x, h, w, r, tmp);
    }
    au_free(tmp);
}

static AuRadii spread_radii(const AuRadii *r, float spread)
{
    AuRadii o = {{0}, {0}};
    if (!r)
        return o;
    for (int i = 0; i < 4; i++) {
        /* CSS: a spread radius grows a non-zero corner radius by the
         * spread (and shrinks it for negative spread). */
        if (r->rx[i] > 0 && r->ry[i] > 0) {
            o.rx[i] = r->rx[i] + spread > 0 ? r->rx[i] + spread : 0;
            o.ry[i] = r->ry[i] + spread > 0 ? r->ry[i] + spread : 0;
        }
    }
    return o;
}

void au_draw_box_shadow(AuCanvas *c, AuRect box, const AuRadii *radii, float dx, float dy,
                        float blur, float spread, AuColor color, bool inset)
{
    uint32_t src = au_premul(color);
    if (!(src >> 24) || !(box.w > 0 && box.h > 0))
        return;
    if (!(blur >= 0))
        blur = 0;
    /* The work area grows with the blur: a page must not size it. */
    if (blur > AU_SHADOW_MAX_BLUR)
        blur = AU_SHADOW_MAX_BLUR;
    int ox, oy;
    AuIRect clip;
    float tx, ty;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    au_canvas_get_translation(c, &tx, &ty);
    /* The fraction of the translation; whole pixels are added when
     * blending (see raster() in raster.c). */
    int ix = (int)floorf(tx - (float)ox), iy = (int)floorf(ty - (float)oy);
    float px = tx - (float)ox - (float)ix, py = ty - (float)oy - (float)iy;
    clip.x0 -= ix;
    clip.x1 -= ix;
    clip.y0 -= iy;
    clip.y1 -= iy;
    AuRect b = {box.x + px, box.y + py, box.w, box.h};
    AuRadii br = {{0}, {0}};
    if (radii) {
        br = *radii;
        au_radii_fit(&br, b);
    }
    float sigma = blur / 2.0f;
    int pad = (int)ceilf(blur * 1.5f) + 2;

    /* The shadow shape: the box moved by the offset and grown by the spread
     * (shrunk for inset). */
    float sp = inset ? -spread : spread;
    AuRect sh = {b.x + dx - sp, b.y + dy - sp, b.w + 2 * sp, b.h + 2 * sp};
    AuRadii shr = spread_radii(radii ? &br : NULL, sp);

    AuIRect area;
    if (inset) {
        area = (AuIRect){au_f2px(floorf(b.x)), au_f2px(floorf(b.y)), au_f2px(ceilf(b.x + b.w)), au_f2px(ceilf(b.y + b.h))};
    } else {
        area = (AuIRect){au_f2px(floorf(sh.x)) - pad, au_f2px(floorf(sh.y)) - pad, au_f2px(ceilf(sh.x + sh.w)) + pad,
                         au_f2px(ceilf(sh.y + sh.h)) + pad};
    }
    AuIRect vis = au_irect_intersect(area, clip);
    if (au_irect_empty(vis))
        return;
    /* The blur reads up to pad pixels beyond what is visible. */
    AuIRect work = {vis.x0 - pad, vis.y0 - pad, vis.x1 + pad, vis.y1 + pad};
    int ww = work.x1 - work.x0, wh = work.y1 - work.y0;
    uint8_t *shape = au_calloc((size_t)ww * wh);
    uint8_t *boxm = au_calloc((size_t)ww * wh);
    AuPath p;
    au_path_init(&p);
    if (sh.w > 0 && sh.h > 0)
        au_path_rounded_rect(&p, sh, &shr, false);
    au_raster_mask(&p, AU_FILL_NONZERO, work, shape);
    au_path_clear(&p);
    au_path_rounded_rect(&p, b, radii ? &br : NULL, false);
    au_raster_mask(&p, AU_FILL_NONZERO, work, boxm);
    au_path_free(&p);
    au_blur_mask(shape, ww, wh, sigma);

    int n = vis.x1 - vis.x0;
    uint8_t *cov = au_alloc((size_t)n);
    for (int y = vis.y0; y < vis.y1; y++) {
        const uint8_t *sm = shape + (size_t)(y - work.y0) * ww + (vis.x0 - work.x0);
        const uint8_t *bm = boxm + (size_t)(y - work.y0) * ww + (vis.x0 - work.x0);
        for (int i = 0; i < n; i++) {
            uint32_t v;
            if (inset)
                v = ((255u - sm[i]) * au_a256(bm[i])) >> 8;
            else
                v = (sm[i] * (256u - au_a256(bm[i]))) >> 8;
            cov[i] = (uint8_t)v;
        }
        au_blend_solid_span(s->pixels + (size_t)(y + iy) * s->stride + (vis.x0 + ix), n, src, cov);
    }
    au_free(cov);
    au_free(shape);
    au_free(boxm);
}
