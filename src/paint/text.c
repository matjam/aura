/* Text: metrics, shaping (cmap, advances, kern), the glyph cache and
 * drawing. */
#include "font_internal.h"
#include "paint_internal.h"

#include <math.h>
#include <string.h>

/* ---- Metrics ---- */

void au_font_metrics(const AuFontFace *f, float size, AuFontMetrics *m)
{
    float s = size / f->units_per_em;
    m->size = size;
    m->ascent = f->ascender * s;
    m->descent = -f->descender * s;
    if (m->descent < 0)
        m->descent = -m->descent;
    m->line_gap = f->line_gap > 0 ? f->line_gap * s : 0;
    m->x_height = f->x_height * s;
    m->cap_height = f->cap_height * s;
    uint16_t zero = au_font_glyph(f, '0');
    m->ch_advance = zero ? au_font_advance_units(f, zero) * s : size / 2;
    m->avg_advance = f->avg_width > 0 ? f->avg_width * s : m->ch_advance;
    m->space_advance = au_font_advance_units(f, au_font_glyph(f, ' ')) * s;
    m->underline_pos = -f->underline_pos * s;
    m->underline_thickness = f->underline_thickness * s;
    m->strike_pos = f->strike_pos * s;
    m->strike_thickness = f->strike_thickness * s;
}

/* ---- Shaping ---- */

void au_glyph_run_init(AuGlyphRun *r)
{
    memset(r, 0, sizeof *r);
}

void au_glyph_run_free(AuGlyphRun *r)
{
    au_free(r->glyphs);
    memset(r, 0, sizeof *r);
}

void au_shape_text(const AuFontFace *f, float size, const char *text, size_t len, AuGlyphRun *r)
{
    r->n = 0;
    r->width = 0;
    r->missing = 0;
    float s = size / f->units_per_em;
    float pen = 0;
    size_t i = 0;
    /* GSUB (ligatures, contextual forms) and GPOS (kerning, marks) will be
     * applied here, between the cmap lookup and the advances. */
    while (i < len) {
        size_t adv;
        uint32_t cp = au_utf8_decode(text + i, len - i, &adv);
        uint16_t id = au_font_glyph(f, cp);
        if (!id && au_uc_is_ignorable(cp)) {
            i += adv;
            continue;
        }
        if (r->n == r->cap) {
            r->cap = r->cap ? r->cap * 2 : 32;
            r->glyphs = au_realloc(r->glyphs, r->cap * sizeof(AuGlyph));
        }
        AuGlyph *g = &r->glyphs[r->n++];
        g->id = id;
        g->face = NULL;
        g->y = 0;
        if (!g->id)
            r->missing++;
        g->cluster = (uint32_t)i;
        g->x = pen;
        g->advance = au_font_advance_units(f, g->id) * s;
        if (r->n > 1) {
            AuGlyph *prev = g - 1;
            float k = au_font_kern_units(f, prev->id, g->id) * s;
            if (k) {
                prev->advance += k;
                pen += k;
                g->x = pen;
            }
        }
        pen += g->advance;
        i += adv;
    }
    r->width = pen;
}

float au_text_width(const AuFontFace *f, float size, const char *text, size_t len)
{
    AuGlyphRun r;
    au_glyph_run_init(&r);
    au_shape_text(f, size, text, len, &r);
    float w = r.width;
    au_glyph_run_free(&r);
    return w;
}

/* ---- Glyph cache ---- */

#define SUBPIXELS 4
#define BUCKETS 1024

/* An entry holds an 8-bit coverage mask, or (color) premultiplied ARGB
 * pixels of a colour glyph. A colour glyph that draws the foreground
 * colour (uses_fg) is cached per foreground colour; others are shared by
 * every text colour. */
typedef struct GlyphEntry {
    struct GlyphEntry *hnext;        /* hash chain */
    struct GlyphEntry *prev, *next;  /* LRU list, most recent first */
    uint32_t face, size_q;
    uint32_t fg;
    uint16_t glyph;
    uint8_t sub;
    uint8_t color, uses_fg;
    int16_t left, top; /* bitmap offset from the pen, pixels (top: y down) */
    uint16_t w, h;
    uint32_t bits_[]; /* aligned for colour pixels */
} GlyphEntry;

#define ENTRY_BITS(e) ((uint8_t *)(e)->bits_)

static struct {
    GlyphEntry *buckets[BUCKETS];
    GlyphEntry *head, *tail;
    size_t limit, bytes, entries, hits, misses, evictions;
} cache = {.limit = 1024 * 1024};

static uint32_t glyph_hash(uint32_t face, uint16_t glyph, uint32_t size_q, uint8_t sub)
{
    uint32_t h = face * 0x9E3779B1u;
    h ^= glyph * 0x85EBCA77u;
    h ^= size_q * 0xC2B2AE3Du;
    h ^= sub * 0x27D4EB2Fu;
    h ^= h >> 15;
    return h & (BUCKETS - 1);
}

static size_t entry_bytes(const GlyphEntry *e)
{
    return sizeof(GlyphEntry) + (size_t)e->w * e->h * (e->color ? 4 : 1);
}

/* Colour glyphs: sub is always 0 (they are not positioned to subpixels),
 * so this value marks them in the key. */
#define SUB_COLOR 0xFF

static void lru_unlink(GlyphEntry *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        cache.head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        cache.tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_front(GlyphEntry *e)
{
    e->prev = NULL;
    e->next = cache.head;
    if (cache.head)
        cache.head->prev = e;
    cache.head = e;
    if (!cache.tail)
        cache.tail = e;
}

static void remove_entry(GlyphEntry *e)
{
    uint32_t b = glyph_hash(e->face, e->glyph, e->size_q, e->sub);
    GlyphEntry **pp = &cache.buckets[b];
    while (*pp && *pp != e)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = e->hnext;
    lru_unlink(e);
    cache.bytes -= entry_bytes(e);
    cache.entries--;
    au_free(e);
}

static void evict_to(size_t limit)
{
    while (cache.bytes > limit && cache.tail) {
        remove_entry(cache.tail);
        cache.evictions++;
    }
}

void au_glyph_cache_set_limit(size_t bytes)
{
    cache.limit = bytes;
    evict_to(bytes);
}

void au_glyph_cache_clear(void)
{
    evict_to(0);
}

void au_glyph_cache_stats(AuGlyphCacheStats *s)
{
    s->entries = cache.entries;
    s->bytes = cache.bytes;
    s->hits = cache.hits;
    s->misses = cache.misses;
    s->evictions = cache.evictions;
}

void au_glyph_cache_forget_face(const AuFontFace *f)
{
    GlyphEntry *e = cache.head;
    while (e) {
        GlyphEntry *next = e->next;
        if (e->face == f->id)
            remove_entry(e);
        e = next;
    }
}

/* Vertical scale: at text sizes, nudged so the x-height lands on a whole
 * pixel, which keeps lowercase tops crisp without hinting. */
static float vertical_scale(const AuFontFace *f, float size)
{
    float s = size / f->units_per_em;
    if (size > 48 || f->x_height <= 0)
        return s;
    float xh = f->x_height * s;
    float snapped = floorf(xh + 0.5f);
    if (snapped < 1)
        snapped = 1;
    return snapped / f->x_height;
}

static GlyphEntry *rasterize(const AuFontFace *f, uint16_t glyph, float size, uint32_t size_q, uint8_t sub)
{
    float sx = size / f->units_per_em, sy = vertical_scale(f, size);
    AuPath p;
    au_path_init(&p);
    GlyphEntry *e;
    if (!au_font_glyph_path(f, glyph, sx, sy, (float)sub / SUBPIXELS, 0, &p) || p.npts == 0) {
        e = au_calloc(sizeof *e);
    } else {
        /* Control points bound the curves. */
        float x0 = p.pts[0], x1 = x0, y0 = p.pts[1], y1 = y0;
        for (size_t i = 2; i < p.npts; i += 2) {
            float x = p.pts[i], y = p.pts[i + 1];
            if (x < x0)
                x0 = x;
            if (x > x1)
                x1 = x;
            if (y < y0)
                y0 = y;
            if (y > y1)
                y1 = y;
        }
        AuIRect area = {au_f2px(floorf(x0)), au_f2px(floorf(y0)), au_f2px(ceilf(x1)) + 1, au_f2px(ceilf(y1)) + 1};
        int w = area.x1 - area.x0, h = area.y1 - area.y0;
        /* A sane bound: glyphs larger than this (bad fonts, absurd sizes)
         * are not drawn. */
        if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (size_t)w * h > 1024 * 1024) {
            e = au_calloc(sizeof *e);
        } else {
            e = au_calloc(sizeof *e + (size_t)w * h);
            e->w = (uint16_t)w;
            e->h = (uint16_t)h;
            e->left = (int16_t)area.x0;
            e->top = (int16_t)area.y0;
            au_raster_mask(&p, AU_FILL_NONZERO, area, ENTRY_BITS(e));
            /* Stored with the text coverage table applied, once, instead
             * of at every draw. */
            const uint8_t *gamma = au_text_gamma();
            uint8_t *bits = ENTRY_BITS(e);
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
                bits[i] = gamma[bits[i]];
        }
    }
    au_path_free(&p);
    e->face = f->id;
    e->glyph = glyph;
    e->size_q = size_q;
    e->sub = sub;
    return e;
}

/* The largest colour glyph bitmap: 1 MB of pixels, as a mask may be. */
#define MAX_COLOR_PIXELS (256 * 1024)

/* A colour glyph's entry, or NULL when the glyph does not render in
 * colour after all (the caller draws its outline). */
static GlyphEntry *rasterize_color(const AuFontFace *f, uint16_t glyph, float size, uint32_t size_q, AuColor fg)
{
    float s = size / f->units_per_em;
    AuColrBitmap bm;
    if (!au_colr_render(f, glyph, s, s, 0, fg, MAX_COLOR_PIXELS, &bm))
        return NULL;
    GlyphEntry *e = au_calloc(sizeof *e + (size_t)bm.w * bm.h * 4);
    memcpy(e->bits_, bm.px, (size_t)bm.w * bm.h * 4);
    au_free(bm.px);
    e->w = (uint16_t)bm.w;
    e->h = (uint16_t)bm.h;
    e->left = (int16_t)bm.left;
    e->top = (int16_t)bm.top;
    e->color = 1;
    e->uses_fg = bm.uses_fg;
    e->fg = bm.uses_fg ? fg : 0;
    e->face = f->id;
    e->glyph = glyph;
    e->size_q = size_q;
    e->sub = SUB_COLOR;
    return e;
}

static void cache_insert(GlyphEntry *e, uint32_t b)
{
    e->hnext = cache.buckets[b];
    cache.buckets[b] = e;
    lru_push_front(e);
    cache.bytes += entry_bytes(e);
    cache.entries++;
    /* Keep the new entry even when it alone exceeds the limit: the caller
     * is about to draw it. */
    while (cache.bytes > cache.limit && cache.tail && cache.tail != e) {
        remove_entry(cache.tail);
        cache.evictions++;
    }
}

static GlyphEntry *cache_find(uint32_t b, uint32_t face, uint16_t glyph, uint32_t size_q, uint8_t sub, AuColor fg)
{
    for (GlyphEntry *e = cache.buckets[b]; e; e = e->hnext) {
        if (e->face == face && e->glyph == glyph && e->size_q == size_q && e->sub == sub &&
            (!e->uses_fg || e->fg == fg)) {
            cache.hits++;
            if (cache.head != e) {
                lru_unlink(e);
                lru_push_front(e);
            }
            return e;
        }
    }
    return NULL;
}

/* The colour bitmap of a glyph, or NULL when it has none. */
static const GlyphEntry *cache_get_color(const AuFontFace *f, uint16_t glyph, float size, AuColor fg)
{
    uint32_t size_q = (uint32_t)(size * 64.0f + 0.5f);
    uint32_t b = glyph_hash(f->id, glyph, size_q, SUB_COLOR);
    GlyphEntry *e = cache_find(b, f->id, glyph, size_q, SUB_COLOR, fg);
    if (e)
        return e->w ? e : NULL;
    cache.misses++;
    e = rasterize_color(f, glyph, size, size_q, fg);
    if (!e) {
        /* Remember the failure (an empty colour entry) so the outline is
         * drawn without trying again. */
        e = au_calloc(sizeof *e);
        e->color = 1;
        e->face = f->id;
        e->glyph = glyph;
        e->size_q = size_q;
        e->sub = SUB_COLOR;
    }
    cache_insert(e, b);
    return e->w ? e : NULL;
}

static const GlyphEntry *cache_get(const AuFontFace *f, uint16_t glyph, float size, uint8_t sub)
{
    uint32_t size_q = (uint32_t)(size * 64.0f + 0.5f);
    uint32_t b = glyph_hash(f->id, glyph, size_q, sub);
    GlyphEntry *hit = cache_find(b, f->id, glyph, size_q, sub, 0);
    if (hit)
        return hit;
    cache.misses++;
    GlyphEntry *e = rasterize(f, glyph, size, size_q, sub);
    cache_insert(e, b);
    return e;
}

/* ---- Drawing ---- */

/* Draws premultiplied pixels with their top-left at device pixel (dx, dy)
 * of the canvas (translation already applied), source over, within the
 * clip of the current group or layer. */
static void draw_pixels(AuCanvas *c, const uint32_t *px, int w, int h, int dx, int dy)
{
    int ox, oy;
    AuIRect clip;
    AuSurface *s = au_canvas_current(c, &ox, &oy, &clip);
    int x0 = dx - ox, y0 = dy - oy;
    AuIRect r = au_irect_intersect((AuIRect){x0, y0, x0 + w, y0 + h}, clip);
    for (int yy = r.y0; yy < r.y1; yy++) {
        const uint32_t *src = px + (size_t)(yy - y0) * w + (r.x0 - x0);
        AuPixel *dst = s->pixels + (size_t)yy * s->stride + r.x0;
        for (int i = 0; i < r.x1 - r.x0; i++) {
            uint32_t p = src[i];
            if (!(p >> 24))
                continue;
            dst[i] = (p >> 24) == 255 ? p : au_px_over(p, dst[i]);
        }
    }
}

void au_draw_glyph_run(AuCanvas *c, const AuFontFace *f, float size, const AuGlyphRun *r, float x,
                       float y, AuColor color)
{
    if (!(size > 0) || size > 2000 || !(color >> 24))
        return;
    float tx, ty;
    au_canvas_get_translation(c, &tx, &ty);
    /* The baseline snaps to a whole device pixel. */
    float by = floorf(y + ty + 0.5f);
    for (size_t i = 0; i < r->n; i++) {
        const AuGlyph *g = &r->glyphs[i];
        const AuFontFace *gf = g->face ? g->face : f;
        float gx = x + g->x + tx;
        float gy = g->y ? floorf(g->y + 0.5f) : 0;
        /* A position that is not a number, or is far outside any surface
         * (the page set it: a transform, an offset of 1e38px), draws
         * nothing; the conversions below then stay in int's range. */
        if (!(gx > -1e8f && gx < 1e8f && by + gy > -1e8f && by + gy < 1e8f))
            continue;
        if (gf->color && au_colr_has(gf, g->id)) {
            /* Colour glyphs keep their own colours (the text colour is
             * only the foreground palette entry) and whole-pixel
             * positions. */
            const GlyphEntry *ce = cache_get_color(gf, g->id, size, color | 0xFF000000u);
            if (ce) {
                int px = (int)floorf(gx + 0.5f) + ce->left, py = (int)(by + gy) + ce->top;
                draw_pixels(c, ce->bits_, ce->w, ce->h, px, py);
                continue;
            }
        }
        float fx = floorf(gx);
        int sub = (int)((gx - fx) * SUBPIXELS + 0.5f);
        if (sub >= SUBPIXELS) {
            sub = 0;
            fx += 1;
        }
        const GlyphEntry *e = cache_get(gf, g->id, size, (uint8_t)sub);
        if (!e->w)
            continue;
        au_draw_coverage(c, ENTRY_BITS(e), e->w, e->h, e->w, fx + e->left - tx, by + gy + e->top - ty, color);
    }
}

float au_draw_text(AuCanvas *c, const AuFontFace *f, float size, float x, float y, const char *text,
                   size_t len, AuColor color)
{
    AuGlyphRun r;
    au_glyph_run_init(&r);
    au_shape_text(f, size, text, len, &r);
    au_draw_glyph_run(c, f, size, &r, x, y, color);
    float w = r.width;
    au_glyph_run_free(&r);
    return w;
}
