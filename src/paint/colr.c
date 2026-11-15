/* Colour glyphs: CPAL palettes and COLR version 0 (layers of glyphs in
 * palette colours) and version 1 (a paint graph: solid fills, linear,
 * radial and sweep gradients, glyph outlines as masks, reuse of other
 * colour glyphs, affine transforms, compositing, clip boxes), rendered to
 * premultiplied ARGB bitmaps for the glyph cache. Variable COLR (the Var
 * paint formats) draws at the default instance.
 *
 * Font data comes from the network: reads are bounds-checked, the paint
 * graph is walked with a depth limit, a check for cycles (a paint that
 * reaches itself), a budget of visited paints, and a limit on temporary
 * layers, so a malicious font costs bounded time and memory. */
#include "font_internal.h"
#include "paint_internal.h"

#include <math.h>
#include <string.h>

#define MAX_DEPTH 32        /* nested paints */
#define MAX_VISITS 4096     /* paints visited per glyph (bounds and drawing) */
#define MAX_LAYERS 8        /* temporary layers alive at once */
#define MAX_STOPS 64        /* colour stops per colour line */
#define MAX_V0_LAYERS 1024  /* layers of a version 0 glyph */
#define MAX_COLR_LAYERS 1024 /* layers of one PaintColrLayers */

struct AuColr {
    uint16_t version;
    uint32_t base0, layers0;    /* version 0 records */
    uint16_t nbase0, nlayers0;
    uint32_t base_list, layer_list, clip_list; /* version 1 lists (0: none) */
    uint32_t nbase1, nlayers1;
    uint32_t palette; /* the first colour record of palette 0 */
    uint16_t npal;
};

AuColr *au_colr_new(const AuFontFace *f)
{
    if (f->colr_len < 14 || f->cpal_len < 12)
        return NULL;
    uint32_t c = f->colr, p = f->cpal;
    uint32_t cend = c + f->colr_len;
    AuColr *k = au_calloc(sizeof *k);
    k->version = au_fd_u16(f, c);
    if (k->version > 1)
        goto bad;
    k->nbase0 = au_fd_u16(f, c + 2);
    k->base0 = au_fd_add(c, au_fd_u32(f, c + 4));
    k->layers0 = au_fd_add(c, au_fd_u32(f, c + 8));
    k->nlayers0 = au_fd_u16(f, c + 12);
    if (k->nbase0 && (k->base0 > cend || (uint64_t)k->nbase0 * 6 > cend - k->base0))
        k->nbase0 = 0;
    if (k->nlayers0 && (k->layers0 > cend || (uint64_t)k->nlayers0 * 4 > cend - k->layers0))
        k->nlayers0 = 0;
    if (k->version == 1 && f->colr_len >= 34) {
        uint32_t bl = au_fd_u32(f, c + 14), ll = au_fd_u32(f, c + 18), cl = au_fd_u32(f, c + 22);
        if (bl && bl < f->colr_len) {
            k->base_list = c + bl;
            k->nbase1 = au_fd_u32(f, k->base_list);
            if ((uint64_t)k->nbase1 * 6 > cend - k->base_list - 4)
                k->nbase1 = 0;
        }
        if (ll && ll < f->colr_len) {
            k->layer_list = c + ll;
            k->nlayers1 = au_fd_u32(f, k->layer_list);
            if ((uint64_t)k->nlayers1 * 4 > cend - k->layer_list - 4)
                k->nlayers1 = 0;
        }
        if (cl && cl < f->colr_len && au_fd_u8(f, c + cl) == 1)
            k->clip_list = c + cl;
    }
    /* CPAL: palette 0. */
    uint16_t entries = au_fd_u16(f, p + 2), npal = au_fd_u16(f, p + 4), nrec = au_fd_u16(f, p + 6);
    uint32_t recs = au_fd_add(p, au_fd_u32(f, p + 8));
    if (!npal || !entries)
        goto bad;
    uint16_t first = au_fd_u16(f, p + 12);
    if ((uint32_t)first + entries > nrec || !au_fd_in(f, recs, (uint32_t)nrec * 4) ||
        recs + (uint32_t)nrec * 4 > p + f->cpal_len)
        goto bad;
    k->palette = recs + (uint32_t)first * 4;
    k->npal = entries;
    if (!k->nbase0 && !k->nbase1)
        goto bad;
    return k;
bad:
    au_free(k);
    return NULL;
}

void au_colr_free(AuColr *c)
{
    au_free(c);
}

static const AuColr *colr_of(const AuFontFace *f)
{
    if (!f->color)
        return NULL;
    AuFontCache *c = au_font_cache(f);
    if (!c->colr_done) {
        c->colr_done = true;
        c->colr = au_colr_new(f);
    }
    return c->colr;
}

/* Version 0: the base glyph record of g, or 0. */
static uint32_t find_v0(const AuFontFace *f, const AuColr *k, uint16_t g)
{
    uint32_t lo = 0, hi = k->nbase0;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, r = k->base0 + mid * 6;
        uint16_t gid = au_fd_u16(f, r);
        if (gid < g)
            lo = mid + 1;
        else if (gid > g)
            hi = mid;
        else
            return r;
    }
    return 0;
}

/* Version 1: the paint of base glyph g, or 0. */
static uint32_t find_v1(const AuFontFace *f, const AuColr *k, uint16_t g)
{
    uint32_t lo = 0, hi = k->nbase1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, r = k->base_list + 4 + mid * 6;
        uint16_t gid = au_fd_u16(f, r);
        if (gid < g)
            lo = mid + 1;
        else if (gid > g)
            hi = mid;
        else {
            uint32_t off = au_fd_u32(f, r + 2);
            return off ? au_fd_add(k->base_list, off) : 0;
        }
    }
    return 0;
}

/* The clip box of g (xmin, ymin, xmax, ymax, font units), if any. */
static bool find_clip(const AuFontFace *f, const AuColr *k, uint16_t g, float box[4])
{
    if (!k->clip_list)
        return false;
    uint32_t n = au_fd_u32(f, k->clip_list + 1);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, r = au_fd_add(k->clip_list + 5, mid * 7);
        if (!au_fd_in(f, r, 7))
            return false;
        uint16_t s = au_fd_u16(f, r), e = au_fd_u16(f, r + 2);
        if (e < g)
            lo = mid + 1;
        else if (s > g)
            hi = mid;
        else {
            uint32_t cb = au_fd_add(k->clip_list, au_fd_u24(f, r + 4));
            uint8_t fmt = au_fd_u8(f, cb);
            if ((fmt != 1 && fmt != 2) || !au_fd_in(f, cb, 9))
                return false;
            for (int i = 0; i < 4; i++)
                box[i] = au_fd_s16(f, cb + 1 + (uint32_t)i * 2);
            return box[0] <= box[2] && box[1] <= box[3];
        }
    }
    return false;
}

bool au_colr_has(const AuFontFace *f, uint16_t g)
{
    const AuColr *k = colr_of(f);
    if (!k)
        return false;
    if (k->nbase1 && find_v1(f, k, g))
        return true;
    return k->nbase0 && find_v0(f, k, g);
}

/* ---- Rendering ---- */

typedef struct M {
    float a, b, c, d, e, f; /* x' = a x + c y + e, y' = b x + d y + f */
} M;

static M mul(const M *p, const M *q) /* p after q: p(q(x)) */
{
    M r;
    r.a = p->a * q->a + p->c * q->b;
    r.b = p->b * q->a + p->d * q->b;
    r.c = p->a * q->c + p->c * q->d;
    r.d = p->b * q->c + p->d * q->d;
    r.e = p->a * q->e + p->c * q->f + p->e;
    r.f = p->b * q->e + p->d * q->f + p->f;
    return r;
}

static bool mat_invert(const M *m, M *out)
{
    float det = m->a * m->d - m->b * m->c;
    if (!(fabsf(det) > 1e-12f) || det != det)
        return false;
    float id = 1 / det;
    out->a = m->d * id;
    out->b = -m->b * id;
    out->c = -m->c * id;
    out->d = m->a * id;
    out->e = -(out->a * m->e + out->c * m->f);
    out->f = -(out->b * m->e + out->d * m->f);
    return true;
}

typedef struct Ctx {
    const AuFontFace *f;
    const AuColr *k;
    AuColor fg;
    bool uses_fg;
    int w, h;
    int visits;
    int layers;
    uint32_t stack[MAX_DEPTH];
    int depth;
    bool bounds_only;
    float bx0, by0, bx1, by1; /* bounds pass: device box */
    bool have_bounds;
} Ctx;

/* Premultiplied colour of palette entry idx with extra alpha. */
static uint32_t palette_color(Ctx *x, uint16_t idx, float alpha)
{
    uint32_t c;
    if (idx == 0xFFFF) {
        x->uses_fg = true;
        c = x->fg; /* 0xAARRGGBB */
    } else if (idx < x->k->npal) {
        uint32_t r = x->k->palette + (uint32_t)idx * 4; /* B, G, R, A */
        c = ((uint32_t)au_fd_u8(x->f, r + 3) << 24) | ((uint32_t)au_fd_u8(x->f, r + 2) << 16) |
            ((uint32_t)au_fd_u8(x->f, r + 1) << 8) | au_fd_u8(x->f, r);
    } else {
        return 0;
    }
    if (!(alpha >= 0))
        alpha = 0;
    if (alpha < 1) {
        uint32_t a = (uint32_t)((c >> 24) * alpha + 0.5f);
        c = (c & 0x00FFFFFFu) | (a << 24);
    }
    return au_premul(c);
}

static bool enter(Ctx *x, uint32_t node)
{
    if (x->depth >= MAX_DEPTH || ++x->visits > MAX_VISITS)
        return false;
    for (int i = 0; i < x->depth; i++)
        if (x->stack[i] == node)
            return false; /* a cycle */
    x->stack[x->depth++] = node;
    return true;
}

/* A colour line evaluated through a 256-entry table over its stop range.
 * (Not "Line": that is a QuickDraw call.) */
typedef struct ColorLine {
    uint32_t lut[256];
    float t0, t1;
    uint8_t extend; /* 0 pad, 1 repeat, 2 reflect */
} ColorLine;

static bool read_line(Ctx *x, uint32_t at, bool var, ColorLine *L)
{
    const AuFontFace *f = x->f;
    L->extend = au_fd_u8(f, at);
    if (L->extend > 2)
        L->extend = 0;
    uint16_t n = au_fd_u16(f, at + 1);
    if (!n)
        return false;
    if (n > MAX_STOPS)
        n = MAX_STOPS;
    uint32_t size = var ? 10 : 6;
    if (!au_fd_in(f, at + 3, (uint32_t)n * size))
        return false;
    float pos[MAX_STOPS];
    uint32_t col[MAX_STOPS];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = at + 3 + i * size;
        pos[i] = au_fd_s16(f, s) / 16384.0f;
        col[i] = palette_color(x, au_fd_u16(f, s + 2), au_fd_s16(f, s + 4) / 16384.0f);
    }
    /* Sort by position (stable insertion sort). */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && pos[j - 1] > pos[j]; j--) {
            float tp = pos[j];
            pos[j] = pos[j - 1];
            pos[j - 1] = tp;
            uint32_t tc = col[j];
            col[j] = col[j - 1];
            col[j - 1] = tc;
        }
    L->t0 = pos[0];
    L->t1 = pos[n - 1];
    float span = L->t1 - L->t0;
    for (int i = 0; i < 256; i++) {
        float t = span > 0 ? L->t0 + span * i / 255.0f : L->t0;
        int k = 0;
        while (k + 1 < n && pos[k + 1] < t)
            k++;
        uint32_t c;
        if (t <= pos[0] || n == 1)
            c = col[0];
        else if (k + 1 >= n)
            c = col[n - 1];
        else {
            float u = pos[k + 1] > pos[k] ? (t - pos[k]) / (pos[k + 1] - pos[k]) : 1;
            uint32_t w = (uint32_t)(u * 256 + 0.5f);
            if (w > 256)
                w = 256;
            c = au_px_scale_trunc(col[k], 256 - w) + au_px_scale_trunc(col[k + 1], w);
        }
        L->lut[i] = c;
    }
    return true;
}

static uint32_t line_at(const ColorLine *L, float t)
{
    float span = L->t1 - L->t0;
    if (!(span > 0))
        return t < L->t0 ? L->lut[0] : L->lut[255];
    float u = (t - L->t0) / span;
    if (u != u)
        return 0;
    if (L->extend == 1) {
        u -= floorf(u);
    } else if (L->extend == 2) {
        u = fmodf(fabsf(u), 2.0f);
        if (u > 1)
            u = 2 - u;
    }
    if (u <= 0)
        return L->lut[0];
    if (u >= 1)
        return L->lut[255];
    return L->lut[(int)(u * 255 + 0.5f)];
}

/* A fill: the colour at a glyph-space point. */
typedef struct Fill {
    int kind; /* 2 solid, 4 linear, 6 radial, 8 sweep */
    uint32_t solid;
    ColorLine line;
    float x0, y0, dx, dy, inv_len2;        /* linear */
    float cx0, cy0, r0, cdx, cdy, dr, a;   /* radial */
    float sa, ea;                          /* sweep: radians */
} Fill;

static uint32_t fill_at(const Fill *F, float x, float y)
{
    switch (F->kind) {
    case 2: return F->solid;
    case 4: return line_at(&F->line, ((x - F->x0) * F->dx + (y - F->y0) * F->dy) * F->inv_len2);
    case 6: {
        float pdx = x - F->cx0, pdy = y - F->cy0;
        float b = pdx * F->cdx + pdy * F->cdy + F->r0 * F->dr;
        float c = pdx * pdx + pdy * pdy - F->r0 * F->r0;
        float t;
        if (fabsf(F->a) < 1e-6f) {
            if (fabsf(b) < 1e-6f)
                return 0;
            t = c / (2 * b);
            if (F->r0 + t * F->dr < 0)
                return 0;
        } else {
            float disc = b * b - F->a * c;
            if (disc < 0)
                return 0;
            float s = sqrtf(disc);
            t = (b + s) / F->a;
            if (F->r0 + t * F->dr < 0) {
                t = (b - s) / F->a;
                if (F->r0 + t * F->dr < 0)
                    return 0;
            }
        }
        return line_at(&F->line, t);
    }
    case 8: {
        float ang = atan2f(y - F->cy0, x - F->cx0);
        if (ang < 0)
            ang += 6.2831853f;
        if (F->ea == F->sa)
            return ang < F->sa ? F->line.lut[0] : F->line.lut[255];
        return line_at(&F->line, (ang - F->sa) / (F->ea - F->sa));
    }
    }
    return 0;
}

/* Prepares a fill paint (formats 2 to 9); false when it is not one or is
 * malformed. */
static bool read_fill(Ctx *x, uint32_t p, Fill *F)
{
    const AuFontFace *f = x->f;
    uint8_t fmt = au_fd_u8(f, p);
    bool var = (fmt & 1) && fmt >= 3 && fmt <= 9;
    uint8_t base = var ? (uint8_t)(fmt - 1) : fmt;
    F->kind = base;
    if (base == 2) {
        if (!au_fd_in(f, p, 5))
            return false;
        F->solid = palette_color(x, au_fd_u16(f, p + 1), au_fd_s16(f, p + 3) / 16384.0f);
        return true;
    }
    if (base != 4 && base != 6 && base != 8)
        return false;
    if (!read_line(x, au_fd_add(p, au_fd_u24(f, p + 1)), var, &F->line))
        return false;
    if (base == 4) {
        if (!au_fd_in(f, p, 16))
            return false;
        float x0 = au_fd_s16(f, p + 4), y0 = au_fd_s16(f, p + 6), x1 = au_fd_s16(f, p + 8), y1 = au_fd_s16(f, p + 10);
        float x2 = au_fd_s16(f, p + 12), y2 = au_fd_s16(f, p + 14);
        /* The gradient runs from p0 to p1 projected onto the normal of
         * p0p2 (the rotation line). */
        float rx = x2 - x0, ry = y2 - y0;
        float nx = ry, ny = -rx; /* normal to the rotation line */
        float nl2 = nx * nx + ny * ny;
        float px, py;
        if (nl2 > 0) {
            float k = ((x1 - x0) * nx + (y1 - y0) * ny) / nl2;
            px = nx * k;
            py = ny * k;
        } else {
            px = x1 - x0;
            py = y1 - y0;
        }
        float l2 = px * px + py * py;
        if (!(l2 > 0))
            return false;
        F->x0 = x0;
        F->y0 = y0;
        F->dx = px;
        F->dy = py;
        F->inv_len2 = 1 / l2;
        return true;
    }
    if (base == 6) {
        if (!au_fd_in(f, p, 16))
            return false;
        F->cx0 = au_fd_s16(f, p + 4);
        F->cy0 = au_fd_s16(f, p + 6);
        F->r0 = au_fd_u16(f, p + 8);
        float cx1 = au_fd_s16(f, p + 10), cy1 = au_fd_s16(f, p + 12), r1 = au_fd_u16(f, p + 14);
        F->cdx = cx1 - F->cx0;
        F->cdy = cy1 - F->cy0;
        F->dr = r1 - F->r0;
        F->a = F->cdx * F->cdx + F->cdy * F->cdy - F->dr * F->dr;
        return true;
    }
    if (!au_fd_in(f, p, 12))
        return false;
    F->cx0 = au_fd_s16(f, p + 4);
    F->cy0 = au_fd_s16(f, p + 6);
    /* Sweep angles carry a bias of 1.0 (180 degrees per unit). */
    F->sa = (au_fd_s16(f, p + 8) / 16384.0f + 1) * 3.14159265f;
    F->ea = (au_fd_s16(f, p + 10) / 16384.0f + 1) * 3.14159265f;
    return true;
}

/* Paints dst (w x h) with F through mask (NULL: everywhere), source
 * over. m maps glyph space to pixels. */
static void fill_into(Ctx *x, uint32_t *dst, const uint8_t *mask, const Fill *F, const M *m)
{
    M inv = {1, 0, 0, 1, 0, 0};
    bool solid = F->kind == 2;
    if (!solid && !mat_invert(m, &inv))
        return;
    if (solid && !(F->solid >> 24))
        return;
    for (int y = 0; y < x->h; y++) {
        uint32_t *row = dst + (size_t)y * x->w;
        const uint8_t *mr = mask ? mask + (size_t)y * x->w : NULL;
        float py = y + 0.5f;
        for (int i = 0; i < x->w; i++) {
            uint32_t cov = mr ? mr[i] : 255;
            if (!cov)
                continue;
            uint32_t c;
            if (solid) {
                c = F->solid;
            } else {
                float px = i + 0.5f;
                c = fill_at(F, inv.a * px + inv.c * py + inv.e, inv.b * px + inv.d * py + inv.f);
            }
            if (cov < 255)
                c = au_px_scale(c, au_a256(cov));
            if (c >> 24)
                row[i] = au_px_over(c, row[i]);
        }
    }
}

/* src over dst through mask (NULL: everywhere). */
static void layer_over(Ctx *x, uint32_t *dst, const uint32_t *src, const uint8_t *mask)
{
    size_t n = (size_t)x->w * x->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = src[i];
        if (mask) {
            if (!mask[i])
                continue;
            if (mask[i] < 255)
                c = au_px_scale(c, au_a256(mask[i]));
        }
        if (c >> 24)
            dst[i] = au_px_over(c, dst[i]);
    }
}

/* ---- Composite modes (premultiplied, in float) ---- */

static float blend_sep(int mode, float cb, float cs)
{
    switch (mode) {
    case 13: return cb + cs - cb * cs;                                   /* screen */
    case 14: return cb <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb); /* overlay */
    case 15: return cb < cs ? cb : cs;                                   /* darken */
    case 16: return cb > cs ? cb : cs;                                   /* lighten */
    case 17: return cb == 0 ? 0 : cs >= 1 ? 1 : fminf(1, cb / (1 - cs)); /* color dodge */
    case 18: return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - fminf(1, (1 - cb) / cs); /* color burn */
    case 19: return cs <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb); /* hard light */
    case 20: {                                                           /* soft light */
        if (cs <= 0.5f)
            return cb - (1 - 2 * cs) * cb * (1 - cb);
        float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    case 21: return fabsf(cb - cs);          /* difference */
    case 22: return cb + cs - 2 * cb * cs;   /* exclusion */
    case 23: return cb * cs;                 /* multiply */
    }
    return cs;
}

static float lum(const float c[3])
{
    return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}

static void clip_color(float c[3])
{
    float l = lum(c), n = fminf(c[0], fminf(c[1], c[2])), x = fmaxf(c[0], fmaxf(c[1], c[2]));
    for (int i = 0; i < 3; i++) {
        if (n < 0 && l - n > 0)
            c[i] = l + (c[i] - l) * l / (l - n);
        if (x > 1 && x - l > 0)
            c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}

static void set_lum(float c[3], float l)
{
    float d = l - lum(c);
    for (int i = 0; i < 3; i++)
        c[i] += d;
    clip_color(c);
}

static float sat(const float c[3])
{
    return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2]));
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
    int md = 3 - mx - mn;
    c[md] = (c[md] - c[mn]) * s / (c[mx] - c[mn]);
    c[mx] = s;
    c[mn] = 0;
}

static uint32_t composite_px(int mode, uint32_t s, uint32_t d)
{
    float sa = (s >> 24) / 255.0f, da = (d >> 24) / 255.0f;
    float sc[3] = {((s >> 16) & 255) / 255.0f, ((s >> 8) & 255) / 255.0f, (s & 255) / 255.0f};
    float dc[3] = {((d >> 16) & 255) / 255.0f, ((d >> 8) & 255) / 255.0f, (d & 255) / 255.0f};
    float fa, fb; /* Porter-Duff factors */
    float ra, rc[3];
    switch (mode) {
    case 0: return 0;
    case 1: return s;
    case 2: return d;
    case 3: fa = 1, fb = 1 - sa; break;
    case 4: fa = 1 - da, fb = 1; break;
    case 5: fa = da, fb = 0; break;
    case 6: fa = 0, fb = sa; break;
    case 7: fa = 1 - da, fb = 0; break;
    case 8: fa = 0, fb = 1 - sa; break;
    case 9: fa = da, fb = 1 - sa; break;
    case 10: fa = 1 - da, fb = sa; break;
    case 11: fa = 1 - da, fb = 1 - sa; break;
    case 12: fa = 1, fb = 1; break;
    default: {
        /* Blend modes: Cs' = (1 - ab) cs + (1 - as) cb + as ab B(Cb, Cs). */
        float ub[3], us[3], bl[3];
        for (int i = 0; i < 3; i++) {
            ub[i] = da > 0 ? dc[i] / da : 0;
            us[i] = sa > 0 ? sc[i] / sa : 0;
        }
        if (mode >= 13 && mode <= 23) {
            for (int i = 0; i < 3; i++)
                bl[i] = blend_sep(mode, ub[i], us[i]);
        } else {
            memcpy(bl, ub, sizeof bl);
            if (mode == 24) { /* hue */
                memcpy(bl, us, sizeof bl);
                set_sat(bl, sat(ub));
                set_lum(bl, lum(ub));
            } else if (mode == 25) { /* saturation */
                set_sat(bl, sat(us));
                set_lum(bl, lum(ub));
            } else if (mode == 26) { /* color */
                memcpy(bl, us, sizeof bl);
                set_lum(bl, lum(ub));
            } else if (mode == 27) { /* luminosity */
                set_lum(bl, lum(us));
            } else {
                memcpy(bl, us, sizeof bl);
            }
        }
        ra = sa + da - sa * da;
        for (int i = 0; i < 3; i++)
            rc[i] = (1 - da) * sc[i] + (1 - sa) * dc[i] + sa * da * bl[i];
        goto pack;
    }
    }
    ra = sa * fa + da * fb;
    for (int i = 0; i < 3; i++)
        rc[i] = sc[i] * fa + dc[i] * fb;
pack:;
    if (ra > 1)
        ra = 1;
    uint32_t A = (uint32_t)(ra * 255 + 0.5f), out = A << 24;
    for (int i = 0; i < 3; i++) {
        float v = rc[i] > ra ? ra : rc[i] < 0 ? 0 : rc[i];
        out |= (uint32_t)(v * 255 + 0.5f) << (16 - 8 * i);
    }
    return out;
}

/* ---- The paint graph ---- */

static void paint_node(Ctx *x, uint32_t p, const M *m, uint32_t *dst);

/* A glyph outline as a coverage mask of the bitmap (au_alloc'd), or NULL
 * when it is empty; in the bounds pass, grows the bounds instead. */
static uint8_t *glyph_mask(Ctx *x, uint16_t g, const M *m)
{
    AuPath path;
    au_path_init(&path);
    float mm[6] = {m->a, m->b, m->c, m->d, m->e, m->f};
    uint8_t *mask = NULL;
    if (au_font_glyph_path_m(x->f, g, mm, &path) && path.npts) {
        if (x->bounds_only) {
            for (size_t i = 0; i + 1 < path.npts; i += 2) { /* npts counts floats */
                float px = path.pts[i], py = path.pts[i + 1];
                if (!x->have_bounds) {
                    x->bx0 = x->bx1 = px;
                    x->by0 = x->by1 = py;
                    x->have_bounds = true;
                }
                x->bx0 = fminf(x->bx0, px);
                x->bx1 = fmaxf(x->bx1, px);
                x->by0 = fminf(x->by0, py);
                x->by1 = fmaxf(x->by1, py);
            }
        } else {
            mask = au_calloc((size_t)x->w * x->h);
            au_raster_mask(&path, AU_FILL_NONZERO, (AuIRect){0, 0, x->w, x->h}, mask);
        }
    }
    au_path_free(&path);
    return mask;
}

/* Draws child (a fill directly, anything else through a layer) into dst
 * through mask. */
static void paint_masked(Ctx *x, uint32_t child, const M *m, const uint8_t *mask, uint32_t *dst)
{
    Fill F;
    uint8_t fmt = au_fd_u8(x->f, child);
    if (fmt >= 2 && fmt <= 9) {
        if (x->bounds_only)
            return;
        if (enter(x, child)) {
            if (read_fill(x, child, &F))
                fill_into(x, dst, mask, &F, m);
            x->depth--;
        }
        return;
    }
    if (x->bounds_only) {
        paint_node(x,child, m, dst);
        return;
    }
    if (x->layers >= MAX_LAYERS)
        return;
    x->layers++;
    uint32_t *tmp = au_calloc((size_t)x->w * x->h * 4);
    paint_node(x, child, m, tmp);
    layer_over(x, dst, tmp, mask);
    au_free(tmp);
    x->layers--;
}

/* A clip box (font units) as a mask through m. */
static uint8_t *box_mask(Ctx *x, const float box[4], const M *m)
{
    if (x->bounds_only)
        return NULL;
    AuPath path;
    au_path_init(&path);
    float pts[4][2] = {{box[0], box[1]}, {box[2], box[1]}, {box[2], box[3]}, {box[0], box[3]}};
    for (int i = 0; i < 4; i++) {
        float px = m->a * pts[i][0] + m->c * pts[i][1] + m->e, py = m->b * pts[i][0] + m->d * pts[i][1] + m->f;
        if (i == 0)
            au_path_move_to(&path, px, py);
        else
            au_path_line_to(&path, px, py);
    }
    au_path_close(&path);
    uint8_t *mask = au_calloc((size_t)x->w * x->h);
    au_raster_mask(&path, AU_FILL_NONZERO, (AuIRect){0, 0, x->w, x->h}, mask);
    au_path_free(&path);
    return mask;
}

static float fword(const AuFontFace *f, uint32_t at)
{
    return au_fd_s16(f, at);
}

static float f2(const AuFontFace *f, uint32_t at)
{
    return au_fd_s16(f, at) / 16384.0f;
}

static M around(M t, float cx, float cy)
{
    M to = {1, 0, 0, 1, cx, cy}, back = {1, 0, 0, 1, -cx, -cy};
    M r = mul(&to, &t);
    return mul(&r, &back);
}

static void paint_node(Ctx *x, uint32_t p, const M *m, uint32_t *dst)
{
    const AuFontFace *f = x->f;
    if (!p || !au_fd_in(f, p, 1) || !enter(x, p))
        return;
    uint8_t fmt = au_fd_u8(f, p);
    switch (fmt) {
    case 1: { /* PaintColrLayers */
        uint8_t n = au_fd_u8(f, p + 1);
        uint32_t first = au_fd_u32(f, p + 2);
        for (uint32_t i = 0; i < n && i < MAX_COLR_LAYERS; i++) {
            uint32_t li = first + i;
            if (li < first || li >= x->k->nlayers1)
                break;
            uint32_t off = au_fd_u32(f, x->k->layer_list + 4 + li * 4);
            if (off)
                paint_node(x,au_fd_add(x->k->layer_list, off), m, dst);
        }
        break;
    }
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: {
        /* A fill outside any glyph covers the whole bitmap. */
        Fill F;
        if (!x->bounds_only && read_fill(x, p, &F))
            fill_into(x, dst, NULL, &F, m);
        break;
    }
    case 10: { /* PaintGlyph */
        uint32_t child = au_fd_add(p, au_fd_u24(f, p + 1));
        uint16_t g = au_fd_u16(f, p + 4);
        uint8_t *mask = glyph_mask(x, g, m);
        if (mask || x->bounds_only)
            paint_masked(x, child, m, mask, dst);
        au_free(mask);
        break;
    }
    case 11: { /* PaintColrGlyph */
        uint16_t g = au_fd_u16(f, p + 1);
        uint32_t base = find_v1(f, x->k, g);
        float box[4];
        if (!base)
            break;
        if (find_clip(f, x->k, g, box) && !x->bounds_only) {
            uint8_t *mask = box_mask(x, box, m);
            if (x->layers < MAX_LAYERS) {
                x->layers++;
                uint32_t *tmp = au_calloc((size_t)x->w * x->h * 4);
                paint_node(x,base, m, tmp);
                layer_over(x, dst, tmp, mask);
                au_free(tmp);
                x->layers--;
            }
            au_free(mask);
        } else {
            paint_node(x,base, m, dst);
        }
        break;
    }
    case 12: case 13: { /* PaintTransform */
        uint32_t child = au_fd_add(p, au_fd_u24(f, p + 1));
        uint32_t t = au_fd_add(p, au_fd_u24(f, p + 4));
        if (!au_fd_in(f, t, 24))
            break;
        M tm = {au_fd_s32(f, t) / 65536.0f,      au_fd_s32(f, t + 4) / 65536.0f,
                au_fd_s32(f, t + 8) / 65536.0f,  au_fd_s32(f, t + 12) / 65536.0f,
                au_fd_s32(f, t + 16) / 65536.0f, au_fd_s32(f, t + 20) / 65536.0f};
        M r = mul(m, &tm);
        paint_node(x,child, &r, dst);
        break;
    }
    case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25:
    case 26: case 27: case 28: case 29: case 30: case 31: {
        uint8_t b = (fmt & 1) ? (uint8_t)(fmt - 1) : fmt;
        uint32_t child = au_fd_add(p, au_fd_u24(f, p + 1));
        uint32_t q = p + 4;
        M t = {1, 0, 0, 1, 0, 0};
        switch (b) {
        case 14: t.e = fword(f, q), t.f = fword(f, q + 2); break;
        case 16: t.a = f2(f, q), t.d = f2(f, q + 2); break;
        case 18: t.a = f2(f, q), t.d = f2(f, q + 2), t = around(t, fword(f, q + 4), fword(f, q + 6)); break;
        case 20: t.a = t.d = f2(f, q); break;
        case 22: t.a = t.d = f2(f, q), t = around(t, fword(f, q + 2), fword(f, q + 4)); break;
        case 24: case 26: {
            float r = f2(f, q) * 3.14159265f, cs = cosf(r), sn = sinf(r);
            t.a = cs, t.b = sn, t.c = -sn, t.d = cs;
            if (b == 26)
                t = around(t, fword(f, q + 2), fword(f, q + 4));
            break;
        }
        case 28: case 30: {
            float xs = f2(f, q) * 3.14159265f, ys = f2(f, q + 2) * 3.14159265f;
            t.b = tanf(ys);
            t.c = -tanf(xs);
            if (b == 30)
                t = around(t, fword(f, q + 4), fword(f, q + 6));
            break;
        }
        }
        M r = mul(m, &t);
        paint_node(x,child, &r, dst);
        break;
    }
    case 32: { /* PaintComposite */
        uint32_t src = au_fd_add(p, au_fd_u24(f, p + 1));
        uint8_t mode = au_fd_u8(f, p + 4);
        uint32_t back = au_fd_add(p, au_fd_u24(f, p + 5));
        if (x->bounds_only) {
            paint_node(x,src, m, dst);
            paint_node(x,back, m, dst);
            break;
        }
        if (mode > 27 || x->layers + 2 > MAX_LAYERS)
            break;
        x->layers += 2;
        size_t n = (size_t)x->w * x->h;
        uint32_t *s = au_calloc(n * 4), *d = au_calloc(n * 4);
        paint_node(x,back, m, d);
        paint_node(x,src, m, s);
        for (size_t i = 0; i < n; i++)
            if (s[i] || mode != 3)
                d[i] = mode == 3 ? au_px_over(s[i], d[i]) : composite_px(mode, s[i], d[i]);
        layer_over(x, dst, d, NULL);
        au_free(s);
        au_free(d);
        x->layers -= 2;
        break;
    }
    default:
        break;
    }
    x->depth--;
}

/* Version 0: each layer's glyph filled with its palette colour. */
static void paint_v0(Ctx *x, uint32_t rec, const M *m, uint32_t *dst)
{
    const AuFontFace *f = x->f;
    uint16_t first = au_fd_u16(f, rec + 2), n = au_fd_u16(f, rec + 4);
    for (uint32_t i = 0; i < n && i < MAX_V0_LAYERS; i++) {
        uint32_t li = (uint32_t)first + i;
        if (li >= x->k->nlayers0)
            break;
        uint32_t lr = x->k->layers0 + li * 4;
        uint16_t g = au_fd_u16(f, lr), pi = au_fd_u16(f, lr + 2);
        uint8_t *mask = glyph_mask(x, g, m);
        if (mask) {
            Fill F;
            F.kind = 2;
            F.solid = palette_color(x, pi, 1);
            fill_into(x, dst, mask, &F, m);
            au_free(mask);
        }
    }
}

bool au_colr_render(const AuFontFace *f, uint16_t g, float sx, float sy, float ox, AuColor fg, size_t max_pixels,
                    AuColrBitmap *out)
{
    memset(out, 0, sizeof *out);
    const AuColr *k = colr_of(f);
    if (!k || !(sx > 0) || !(sy > 0))
        return false;
    uint32_t p1 = k->nbase1 ? find_v1(f, k, g) : 0;
    uint32_t r0 = !p1 && k->nbase0 ? find_v0(f, k, g) : 0;
    if (!p1 && !r0)
        return false;
    Ctx x;
    memset(&x, 0, sizeof x);
    x.f = f;
    x.k = k;
    x.fg = fg;
    /* Glyph space (font units, y up) to pixels relative to the pen. */
    M m0 = {sx, 0, 0, -sy, ox, 0};
    float box[4];
    float bx0, by0, bx1, by1;
    if (p1 && find_clip(f, k, g, box)) {
        bx0 = box[0] * sx + ox;
        bx1 = box[2] * sx + ox;
        by0 = -box[3] * sy;
        by1 = -box[1] * sy;
    } else {
        x.bounds_only = true;
        if (p1)
            paint_node(&x, p1, &m0, NULL);
        else
            paint_v0(&x, r0, &m0, NULL);
        if (!x.have_bounds)
            return false;
        bx0 = x.bx0, by0 = x.by0, bx1 = x.bx1, by1 = x.by1;
        x.bounds_only = false;
        x.visits = 0;
        x.depth = 0;
    }
    if (!(bx0 > -1e6f && bx1 < 1e6f && by0 > -1e6f && by1 < 1e6f))
        return false;
    int left = (int)floorf(bx0), top = (int)floorf(by0);
    int w = (int)ceilf(bx1) - left + 1, h = (int)ceilf(by1) - top + 1;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (size_t)w * h > max_pixels)
        return false;
    x.w = w;
    x.h = h;
    M m = m0;
    m.e -= left;
    m.f -= top;
    uint32_t *px = au_calloc((size_t)w * h * 4);
    if (p1)
        paint_node(&x, p1, &m, px);
    else
        paint_v0(&x, r0, &m, px);
    out->left = left;
    out->top = top;
    out->w = w;
    out->h = h;
    out->px = px;
    out->uses_fg = x.uses_fg;
    return true;
}
