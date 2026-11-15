/* The 2D canvas context: sets, bitmaps, canvases, the drawing state and
 * pixel access. canvas2d.h describes the interface; drawing is in
 * canvas2d_draw.c and path building in canvas2d_path.c. */
#include "canvas2d_int.h"

#include <math.h>
#include <string.h>

/* ---- Pixel runs ----
 * Word loops: newlib's memcpy and memset on Mac OS 9 work a byte at a
 * time (see copy_pixels in src/browser/present.c). */

void c2d_copy_pixels(AuPixel *d, const AuPixel *s, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        AuPixel a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];
        AuPixel f = s[i + 4], g = s[i + 5], h = s[i + 6], k = s[i + 7];
        d[i] = a;
        d[i + 1] = b;
        d[i + 2] = c;
        d[i + 3] = e;
        d[i + 4] = f;
        d[i + 5] = g;
        d[i + 6] = h;
        d[i + 7] = k;
    }
    for (; i < n; i++)
        d[i] = s[i];
}

void c2d_zero_pixels(AuPixel *d, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        d[i] = 0;
        d[i + 1] = 0;
        d[i + 2] = 0;
        d[i + 3] = 0;
        d[i + 4] = 0;
        d[i + 5] = 0;
        d[i + 6] = 0;
        d[i + 7] = 0;
    }
    for (; i < n; i++)
        d[i] = 0;
}

/* ---- Sets ---- */

AuCanvasSet *au_canvas_set_new(size_t budget)
{
    AuCanvasSet *s = au_calloc(sizeof *s);
    s->budget = budget ? budget : AU_C2D_SET_BUDGET;
    return s;
}

static void font_unref(C2DFont *f)
{
    if (f && --f->refs == 0) {
        au_free(f->families);
        au_free(f->css);
        au_free(f);
    }
}

static void drop_font_cache(AuCanvasSet *s)
{
    for (int i = 0; i < C2D_FONT_CACHE; i++) {
        if (s->fc[i].list)
            au_fontlist_free(s->fc[i].list);
        font_unref(s->fc[i].font);
        s->fc[i].list = NULL;
        s->fc[i].font = NULL;
    }
}

void au_canvas_set_fonts(AuCanvasSet *s, AuFontSet *fonts)
{
    drop_font_cache(s);
    s->fonts = fonts;
}

void au_canvas_set_free(AuCanvasSet *s)
{
    if (!s)
        return;
    drop_font_cache(s);
    /* Detach what script still holds: the objects stay valid and draw
     * nothing. Clip masks keep their bytes until their state goes. */
    for (AuCanvas2D *c = s->canvases; c;) {
        AuCanvas2D *next = c->next;
        for (int i = 0; i < c->nst; i++)
            if (c->st[i].clip)
                c->st[i].clip->set = NULL;
        c->set = NULL;
        c->prev = c->next = NULL;
        c = next;
    }
    for (AuCanvasBitmap *b = s->bitmaps; b;) {
        AuCanvasBitmap *next = b->next;
        b->set = NULL;
        b->prev = b->next = NULL;
        b = next;
    }
    au_free(s);
}

size_t au_canvas_set_bytes(const AuCanvasSet *s)
{
    return s ? s->bytes : 0;
}

bool c2d_charge(AuCanvasSet *s, size_t bytes)
{
    if (!s || bytes > s->budget || s->bytes > s->budget - bytes)
        return false;
    s->bytes += bytes;
    return true;
}

void c2d_uncharge(AuCanvasSet *s, size_t bytes)
{
    if (s)
        s->bytes = s->bytes >= bytes ? s->bytes - bytes : 0;
}

AuCanvas2D *au_canvas_set_find(const AuCanvasSet *s, const void *owner)
{
    if (!s || !owner)
        return NULL;
    for (AuCanvas2D *c = s->canvases; c; c = c->next)
        if (c->owner == owner)
            return c;
    return NULL;
}

bool au_canvas_set_dirty(const AuCanvasSet *s)
{
    return s && s->dirty;
}

int au_canvas_set_take_dirty(AuCanvasSet *s, void (*fn)(void *ctx, AuCanvas2D *c, void *owner, AuIRect area),
                             void *ctx)
{
    if (!s || !s->dirty)
        return 0;
    s->dirty = false;
    int n = 0;
    for (AuCanvas2D *c = s->canvases; c; c = c->next) {
        if (!c->dirty)
            continue;
        c->dirty = false;
        AuIRect r = c->dirty_box;
        n++;
        if (fn)
            fn(ctx, c, c->owner, r);
    }
    return n;
}

/* ---- Bitmaps ---- */

static bool size_ok(int w, int h)
{
    return w > 0 && h > 0 && w <= AU_C2D_MAX_DIM && h <= AU_C2D_MAX_DIM &&
           (size_t)w * (size_t)h * sizeof(AuPixel) <= AU_C2D_MAX_BYTES;
}

/* A zeroed surface from the fallible allocator, or NULL. */
static AuSurface *surface_try_new(int w, int h)
{
    AuPixel *px = au_try_alloc((size_t)w * (size_t)h * sizeof(AuPixel));
    if (!px)
        return NULL;
    c2d_zero_pixels(px, (size_t)w * (size_t)h);
    AuSurface *s = au_calloc(sizeof *s);
    s->pixels = px;
    s->width = w;
    s->height = h;
    s->stride = w;
    s->owned = true;
    return s;
}

static AuCanvasBitmap *bitmap_link(AuCanvasSet *s, AuSurface *surf, int nat_w, int nat_h, size_t bytes)
{
    AuCanvasBitmap *b = au_calloc(sizeof *b);
    b->surf = surf;
    b->nat_w = nat_w;
    b->nat_h = nat_h;
    b->clean = true;
    b->refs = 1;
    b->set = s;
    b->bytes = bytes;
    b->next = s->bitmaps;
    if (s->bitmaps)
        s->bitmaps->prev = b;
    s->bitmaps = b;
    return b;
}

AuCanvasBitmap *au_cbitmap_new(AuCanvasSet *s, int w, int h)
{
    if (!s || !size_ok(w, h))
        return NULL;
    size_t bytes = (size_t)w * (size_t)h * sizeof(AuPixel);
    if (!c2d_charge(s, bytes))
        return NULL;
    AuSurface *surf = surface_try_new(w, h);
    if (!surf) {
        c2d_uncharge(s, bytes);
        return NULL;
    }
    return bitmap_link(s, surf, w, h, bytes);
}

AuCanvasBitmap *au_cbitmap_adopt(AuCanvasSet *s, AuSurface *surf, int nat_w, int nat_h)
{
    if (!surf)
        return NULL;
    size_t bytes = (size_t)surf->width * (size_t)surf->height * sizeof(AuPixel);
    if (!s || surf->width <= 0 || surf->height <= 0 || nat_w <= 0 || nat_h <= 0 || !c2d_charge(s, bytes)) {
        au_surface_free(surf);
        return NULL;
    }
    return bitmap_link(s, surf, nat_w, nat_h, bytes);
}

AuCanvasBitmap *au_cbitmap_ref(AuCanvasBitmap *b)
{
    if (b)
        b->refs++;
    return b;
}

void au_cbitmap_unref(AuCanvasBitmap *b)
{
    if (!b || --b->refs)
        return;
    if (b->set) {
        c2d_uncharge(b->set, b->bytes);
        if (b->prev)
            b->prev->next = b->next;
        else
            b->set->bitmaps = b->next;
        if (b->next)
            b->next->prev = b->prev;
    }
    au_surface_free(b->surf);
    au_free(b);
}

/* ---- Gradients and patterns ---- */

static AuCanvasGradient *gradient_new(int kind)
{
    AuCanvasGradient *g = au_calloc(sizeof *g);
    g->refs = 1;
    g->kind = (uint8_t)kind;
    return g;
}

static float fclamp(double v)
{
    if (v != v)
        return 0;
    if (v > 1e9)
        return 1e9f;
    if (v < -1e9)
        return -1e9f;
    return (float)v;
}

AuCanvasGradient *au_cgradient_linear(double x0, double y0, double x1, double y1)
{
    AuCanvasGradient *g = gradient_new(GRAD_LINEAR);
    g->x0 = fclamp(x0);
    g->y0 = fclamp(y0);
    g->x1 = fclamp(x1);
    g->y1 = fclamp(y1);
    return g;
}

AuCanvasGradient *au_cgradient_radial(double x0, double y0, double r0, double x1, double y1, double r1)
{
    AuCanvasGradient *g = gradient_new(GRAD_RADIAL);
    g->x0 = fclamp(x0);
    g->y0 = fclamp(y0);
    g->r0 = fclamp(r0);
    g->x1 = fclamp(x1);
    g->y1 = fclamp(y1);
    g->r1 = fclamp(r1);
    return g;
}

AuCanvasGradient *au_cgradient_conic(double angle, double x, double y)
{
    AuCanvasGradient *g = gradient_new(GRAD_CONIC);
    g->x0 = fclamp(x);
    g->y0 = fclamp(y);
    g->r0 = (float)fmod(angle == angle && isfinite(angle) ? angle : 0, 6.283185307179586);
    return g;
}

void au_cgradient_add_stop(AuCanvasGradient *g, double offset, AuColor color)
{
    if (!(offset >= 0 && offset <= 1) || g->nstops >= AU_C2D_MAX_STOPS)
        return;
    if (g->nstops == g->stops_cap) {
        g->stops_cap = g->stops_cap ? (uint16_t)(g->stops_cap * 2) : 4;
        g->stops = au_realloc(g->stops, g->stops_cap * sizeof *g->stops);
    }
    /* Stops stay sorted by offset; equal offsets keep the order added. */
    int i = g->nstops;
    while (i > 0 && g->stops[i - 1].pos > (float)offset) {
        g->stops[i] = g->stops[i - 1];
        i--;
    }
    g->stops[i].pos = (float)offset;
    g->stops[i].color = color;
    g->nstops++;
    g->lut_ok = false;
}

AuCanvasGradient *au_cgradient_ref(AuCanvasGradient *g)
{
    if (g)
        g->refs++;
    return g;
}

void au_cgradient_unref(AuCanvasGradient *g)
{
    if (g && --g->refs == 0) {
        au_free(g->stops);
        au_free(g->lut);
        au_free(g);
    }
}

void **au_cgradient_user(AuCanvasGradient *g)
{
    return &g->user;
}

/* Colours between stops are interpolated without premultiplication, as
 * the standard says, then premultiplied. */
const uint32_t *c2d_gradient_lut(AuCanvasGradient *g)
{
    if (g->lut_ok)
        return g->lut;
    if (!g->lut)
        g->lut = au_alloc(256 * sizeof(uint32_t));
    int k = 0;
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.0f;
        AuColor col = 0;
        if (g->nstops) {
            while (k + 1 < g->nstops && g->stops[k + 1].pos <= t)
                k++;
            if (t < g->stops[0].pos) {
                col = g->stops[0].color;
            } else if (k + 1 >= g->nstops) {
                col = g->stops[g->nstops - 1].color;
            } else {
                float p0 = g->stops[k].pos, p1 = g->stops[k + 1].pos;
                AuColor c0 = g->stops[k].color, c1 = g->stops[k + 1].color;
                float f = p1 > p0 ? (t - p0) / (p1 - p0) : 0;
                uint32_t out = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    float a = (float)((c0 >> sh) & 255), b = (float)((c1 >> sh) & 255);
                    out |= (uint32_t)(a + (b - a) * f + 0.5f) << sh;
                }
                col = out;
            }
        }
        g->lut[i] = au_premul(col);
    }
    g->lut_ok = true;
    return g->lut;
}

AuCanvasPattern *au_cpattern_new(AuCanvasBitmap *b, int repeat)
{
    AuCanvasPattern *p = au_calloc(sizeof *p);
    p->refs = 1;
    p->bmp = au_cbitmap_ref(b);
    p->repeat = (uint8_t)(repeat & 3);
    p->m = (AuMatrix){1, 0, 0, 1, 0, 0};
    return p;
}

void au_cpattern_set_transform(AuCanvasPattern *p, double a, double b, double c, double d, double e, double f)
{
    if (!isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d) || !isfinite(e) || !isfinite(f))
        return;
    p->m = (AuMatrix){fclamp(a), fclamp(b), fclamp(c), fclamp(d), fclamp(e), fclamp(f)};
}

AuCanvasPattern *au_cpattern_ref(AuCanvasPattern *p)
{
    if (p)
        p->refs++;
    return p;
}

void au_cpattern_unref(AuCanvasPattern *p)
{
    if (p && --p->refs == 0) {
        au_cbitmap_unref(p->bmp);
        au_free(p);
    }
}

void **au_cpattern_user(AuCanvasPattern *p)
{
    return &p->user;
}

bool au_cpattern_clean(const AuCanvasPattern *p)
{
    return !p->bmp || p->bmp->clean;
}

/* ---- Clip masks ---- */

C2DClip *c2d_clip_new(AuCanvas2D *c)
{
    size_t n = (size_t)c->w * (size_t)c->h;
    if (!c->set || !c2d_charge(c->set, n))
        return NULL;
    C2DClip *k = au_try_alloc(sizeof *k + n);
    if (!k) {
        c2d_uncharge(c->set, n);
        return NULL;
    }
    k->refs = 1;
    k->set = c->set;
    k->bytes = n;
    memset(k->mask, 0, n);
    return k;
}

void c2d_clip_unref(C2DClip *k)
{
    if (k && --k->refs == 0) {
        c2d_uncharge(k->set, k->bytes);
        au_free(k);
    }
}

/* ---- State ---- */

static void style_release(C2DStyle *s)
{
    au_cgradient_unref(s->grad);
    au_cpattern_unref(s->pat);
    s->grad = NULL;
    s->pat = NULL;
}

static void state_release(C2DState *s)
{
    style_release(&s->style[0]);
    style_release(&s->style[1]);
    font_unref(s->font);
    c2d_clip_unref(s->clip);
    s->font = NULL;
    s->clip = NULL;
}

static C2DFont *font_new(const char *families, int weight, bool italic, float size, const char *css)
{
    C2DFont *f = au_calloc(sizeof *f);
    f->refs = 1;
    f->families = au_strndup(families, strlen(families));
    f->css = au_strndup(css, strlen(css));
    f->weight = weight;
    f->italic = italic;
    f->size = size;
    return f;
}

static void state_default(AuCanvas2D *c, C2DState *s)
{
    memset(s, 0, sizeof *s);
    s->m = (AuMatrix){1, 0, 0, 1, 0, 0};
    s->style[0].color = s->style[1].color = 0xFF000000u;
    s->alpha = 1;
    s->op = OP_SOURCE_OVER;
    s->line_width = 1;
    s->miter = 10;
    s->font = font_new("sans-serif", 400, false, 10, "10px sans-serif");
    s->smoothing = true;
    s->kerning = 0;
    s->clip_box = (AuIRect){0, 0, c->w, c->h};
}

static void states_reset(AuCanvas2D *c)
{
    for (int i = 0; i < c->nst; i++)
        state_release(&c->st[i]);
    if (!c->st) {
        c->st_cap = 4;
        c->st = au_alloc((size_t)c->st_cap * sizeof *c->st);
    }
    c->nst = 1;
    state_default(c, &c->st[0]);
}

void au_c2d_save(AuCanvas2D *c)
{
    if (c->nst >= AU_C2D_MAX_STATES)
        return;
    if (c->nst == c->st_cap) {
        c->st_cap *= 2;
        c->st = au_realloc(c->st, (size_t)c->st_cap * sizeof *c->st);
    }
    C2DState *s = &c->st[c->nst];
    *s = c->st[c->nst - 1];
    for (int i = 0; i < 2; i++) {
        au_cgradient_ref(s->style[i].grad);
        au_cpattern_ref(s->style[i].pat);
    }
    s->font->refs++;
    if (s->clip)
        s->clip->refs++;
    c->nst++;
}

void au_c2d_restore(AuCanvas2D *c)
{
    if (c->nst <= 1)
        return;
    state_release(&c->st[--c->nst]);
}

/* ---- Canvases ---- */

static void drop_surface(AuCanvas2D *c)
{
    if (c->pc)
        au_canvas_free(c->pc);
    c->pc = NULL;
    if (c->surf) {
        c2d_uncharge(c->set, (size_t)c->surf->width * (size_t)c->surf->height * sizeof(AuPixel));
        au_surface_free(c->surf);
        c->surf = NULL;
    }
}

static void set_size(AuCanvas2D *c, int w, int h)
{
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    c->inert = (w > 0 && h > 0) && !size_ok(w, h);
    /* An inert canvas keeps its size for script; nothing reads pixels. */
    c->w = w;
    c->h = h;
}

AuCanvas2D *au_c2d_new(AuCanvasSet *s, void *owner, int w, int h, bool alpha)
{
    AuCanvas2D *c = au_calloc(sizeof *c);
    c->set = s;
    c->owner = owner;
    c->alpha = alpha;
    c->clean = true;
    set_size(c, w, h);
    au_cpath_init(&c->path);
    au_path_init(&c->tmp);
    au_path_init(&c->tmp2);
    au_glyph_run_init(&c->run);
    states_reset(c);
    if (s) {
        c->next = s->canvases;
        if (s->canvases)
            s->canvases->prev = c;
        s->canvases = c;
    }
    return c;
}

void au_c2d_free(AuCanvas2D *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->nst; i++)
        state_release(&c->st[i]);
    au_free(c->st);
    drop_surface(c);
    if (c->set) {
        if (c->prev)
            c->prev->next = c->next;
        else
            c->set->canvases = c->next;
        if (c->next)
            c->next->prev = c->prev;
    }
    au_cpath_free(&c->path);
    au_path_free(&c->tmp);
    au_path_free(&c->tmp2);
    au_glyph_run_free(&c->run);
    au_free(c);
}

/* Transparent black; opaque black on a canvas without alpha, whose pixels
 * always stay opaque. */
static void clear_all(AuCanvas2D *c)
{
    size_t n = (size_t)c->w * (size_t)c->h;
    if (c->alpha) {
        c2d_zero_pixels(c->surf->pixels, n);
    } else {
        AuPixel *d = c->surf->pixels;
        for (size_t i = 0; i < n; i++)
            d[i] = 0xFF000000u;
    }
}

void au_c2d_reset(AuCanvas2D *c)
{
    if (c->surf) {
        clear_all(c);
        c2d_touch(c, (AuIRect){0, 0, c->w, c->h});
    }
    au_cpath_clear(&c->path);
    states_reset(c);
}

void au_c2d_resize(AuCanvas2D *c, int w, int h)
{
    bool had = c->surf != NULL;
    AuIRect old = {0, 0, c->w, c->h};
    for (int i = 0; i < c->nst; i++)
        state_release(&c->st[i]);
    c->nst = 0;
    drop_surface(c);
    set_size(c, w, h);
    c->clean = true; /* a new bitmap holds nothing from another origin */
    au_cpath_clear(&c->path);
    states_reset(c);
    if (had) {
        /* What was shown is gone: repaint the element. */
        c->dirty = true;
        c->dirty_box = (AuIRect){0, 0, c->w > old.x1 ? c->w : old.x1, c->h > old.y1 ? c->h : old.y1};
        if (c->set)
            c->set->dirty = true;
    }
}

int au_c2d_width(const AuCanvas2D *c) { return c->w; }
int au_c2d_height(const AuCanvas2D *c) { return c->h; }
bool au_c2d_alpha(const AuCanvas2D *c) { return c->alpha; }
const AuSurface *au_c2d_surface(const AuCanvas2D *c) { return c->surf; }
bool au_c2d_inert(const AuCanvas2D *c) { return c->inert; }
bool au_c2d_clean(const AuCanvas2D *c) { return c->clean; }
void au_c2d_taint(AuCanvas2D *c) { c->clean = false; }

bool c2d_ensure_surface(AuCanvas2D *c)
{
    if (c->surf)
        return true;
    if (c->inert || !c->set || c->w <= 0 || c->h <= 0)
        return false;
    size_t bytes = (size_t)c->w * (size_t)c->h * sizeof(AuPixel);
    if (!c2d_charge(c->set, bytes))
        return false;
    c->surf = surface_try_new(c->w, c->h);
    if (!c->surf) {
        c2d_uncharge(c->set, bytes);
        return false;
    }
    c->surf->opaque = !c->alpha;
    clear_all(c);
    return true;
}

bool au_c2d_materialize(AuCanvas2D *c)
{
    return c2d_ensure_surface(c);
}

void c2d_touch(AuCanvas2D *c, AuIRect r)
{
    r = au_irect_intersect(r, (AuIRect){0, 0, c->w, c->h});
    if (au_irect_empty(r))
        return;
    if (!c->dirty) {
        c->dirty = true;
        c->dirty_box = r;
    } else {
        if (r.x0 < c->dirty_box.x0)
            c->dirty_box.x0 = r.x0;
        if (r.y0 < c->dirty_box.y0)
            c->dirty_box.y0 = r.y0;
        if (r.x1 > c->dirty_box.x1)
            c->dirty_box.x1 = r.x1;
        if (r.y1 > c->dirty_box.y1)
            c->dirty_box.y1 = r.y1;
    }
    if (c->set)
        c->set->dirty = true;
}

/* ---- Transforms ---- */

static bool fin6(double a, double b, double cc, double d, double e, double f)
{
    return isfinite(a) && isfinite(b) && isfinite(cc) && isfinite(d) && isfinite(e) && isfinite(f);
}

static void set_matrix(AuCanvas2D *c, double a, double b, double cc, double d, double e, double f)
{
    /* Floats hold the matrix; values beyond their range are clamped so
     * later products stay finite. */
    c2d_state(c)->m = (AuMatrix){fclamp(a), fclamp(b), fclamp(cc), fclamp(d), fclamp(e), fclamp(f)};
}

void au_c2d_transform(AuCanvas2D *c, double a, double b, double cc, double d, double e, double f)
{
    if (!fin6(a, b, cc, d, e, f))
        return;
    const AuMatrix *m = &c2d_state(c)->m;
    set_matrix(c, (double)m->a * a + (double)m->c * b, (double)m->b * a + (double)m->d * b,
               (double)m->a * cc + (double)m->c * d, (double)m->b * cc + (double)m->d * d,
               (double)m->a * e + (double)m->c * f + m->e, (double)m->b * e + (double)m->d * f + m->f);
}

void au_c2d_set_transform(AuCanvas2D *c, double a, double b, double cc, double d, double e, double f)
{
    if (!fin6(a, b, cc, d, e, f))
        return;
    set_matrix(c, a, b, cc, d, e, f);
}

void au_c2d_get_transform(const AuCanvas2D *c, double m[6])
{
    const AuMatrix *t = &c2d_state(c)->m;
    m[0] = t->a;
    m[1] = t->b;
    m[2] = t->c;
    m[3] = t->d;
    m[4] = t->e;
    m[5] = t->f;
}

void au_c2d_scale(AuCanvas2D *c, double x, double y)
{
    au_c2d_transform(c, x, 0, 0, y, 0, 0);
}

void au_c2d_rotate(AuCanvas2D *c, double angle)
{
    if (!isfinite(angle))
        return;
    double s = sin(angle), k = cos(angle);
    au_c2d_transform(c, k, s, -s, k, 0, 0);
}

void au_c2d_translate(AuCanvas2D *c, double x, double y)
{
    au_c2d_transform(c, 1, 0, 0, 1, x, y);
}

const AuMatrix *au_c2d_matrix(const AuCanvas2D *c)
{
    return &c2d_state(c)->m;
}

AuCanvasPath *au_c2d_path(AuCanvas2D *c)
{
    return &c->path;
}

void au_c2d_begin_path(AuCanvas2D *c)
{
    au_cpath_clear(&c->path);
}

/* ---- Names ---- */

static const char *const op_names[OP_COUNT] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over", "destination-in",
    "destination-out", "destination-atop", "lighter", "copy", "xor", "multiply", "screen", "overlay", "darken",
    "lighten", "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
    "saturation", "color", "luminosity"};
static const char *const cap_names[] = {"butt", "round", "square"};
static const char *const join_names[] = {"miter", "round", "bevel"};
static const char *const align_names[] = {"start", "end", "left", "right", "center"};
static const char *const baseline_names[] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};
static const char *const direction_names[] = {"inherit", "ltr", "rtl"};
static const char *const kerning_names[] = {"auto", "normal", "none"};
static const char *const quality_names[] = {"low", "medium", "high"};

static int find_name(const char *const *names, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name))
            return i;
    return -1;
}

#define NAMED(fn_set, fn_get, field, table)                                      \
    bool fn_set(AuCanvas2D *c, const char *name)                                 \
    {                                                                            \
        int i = find_name(table, (int)(sizeof table / sizeof table[0]), name);   \
        if (i < 0)                                                               \
            return false;                                                        \
        c2d_state(c)->field = (uint8_t)i;                                        \
        return true;                                                             \
    }                                                                            \
    const char *fn_get(const AuCanvas2D *c) { return table[c2d_state(c)->field]; }

NAMED(au_c2d_set_composite, au_c2d_get_composite, op, op_names)
NAMED(au_c2d_set_line_cap, au_c2d_get_line_cap, cap, cap_names)
NAMED(au_c2d_set_line_join, au_c2d_get_line_join, join, join_names)
NAMED(au_c2d_set_text_align, au_c2d_get_text_align, align, align_names)
NAMED(au_c2d_set_text_baseline, au_c2d_get_text_baseline, baseline, baseline_names)
NAMED(au_c2d_set_direction, au_c2d_get_direction, direction, direction_names)
NAMED(au_c2d_set_kerning, au_c2d_get_kerning, kerning, kerning_names)
NAMED(au_c2d_set_smoothing_quality, au_c2d_get_smoothing_quality, smoothing_quality, quality_names)

/* ---- Scalar state ---- */

void au_c2d_set_alpha(AuCanvas2D *c, double a)
{
    if (a >= 0 && a <= 1)
        c2d_state(c)->alpha = (float)a;
}

double au_c2d_get_alpha(const AuCanvas2D *c) { return c2d_state(c)->alpha; }
void au_c2d_set_smoothing(AuCanvas2D *c, bool on) { c2d_state(c)->smoothing = on; }
bool au_c2d_get_smoothing(const AuCanvas2D *c) { return c2d_state(c)->smoothing; }

void au_c2d_set_color(AuCanvas2D *c, int which, AuColor color)
{
    C2DStyle *s = &c2d_state(c)->style[which & 1];
    style_release(s);
    s->kind = AU_C2D_STYLE_COLOR;
    s->color = color;
}

void au_c2d_set_gradient(AuCanvas2D *c, int which, AuCanvasGradient *g)
{
    C2DStyle *s = &c2d_state(c)->style[which & 1];
    au_cgradient_ref(g);
    style_release(s);
    s->kind = AU_C2D_STYLE_GRADIENT;
    s->grad = g;
}

void au_c2d_set_pattern(AuCanvas2D *c, int which, AuCanvasPattern *p)
{
    C2DStyle *s = &c2d_state(c)->style[which & 1];
    au_cpattern_ref(p);
    style_release(s);
    s->kind = AU_C2D_STYLE_PATTERN;
    s->pat = p;
}

int au_c2d_style_kind(const AuCanvas2D *c, int which) { return c2d_state(c)->style[which & 1].kind; }
AuColor au_c2d_style_color(const AuCanvas2D *c, int which) { return c2d_state(c)->style[which & 1].color; }
AuCanvasGradient *au_c2d_style_gradient(const AuCanvas2D *c, int which) { return c2d_state(c)->style[which & 1].grad; }
AuCanvasPattern *au_c2d_style_pattern(const AuCanvas2D *c, int which) { return c2d_state(c)->style[which & 1].pat; }

void au_c2d_set_line_width(AuCanvas2D *c, double w)
{
    if (w > 0 && isfinite(w))
        c2d_state(c)->line_width = fclamp(w);
}

double au_c2d_get_line_width(const AuCanvas2D *c) { return c2d_state(c)->line_width; }

void au_c2d_set_miter_limit(AuCanvas2D *c, double m)
{
    if (m > 0 && isfinite(m))
        c2d_state(c)->miter = fclamp(m);
}

double au_c2d_get_miter_limit(const AuCanvas2D *c) { return c2d_state(c)->miter; }

void au_c2d_set_line_dash(AuCanvas2D *c, const double *d, size_t n)
{
    size_t total = (n & 1) ? n * 2 : n;
    if (total > AU_STROKE_MAX_DASHES)
        return;
    for (size_t i = 0; i < n; i++)
        if (!(d[i] >= 0) || !isfinite(d[i]))
            return;
    C2DState *s = c2d_state(c);
    for (size_t i = 0; i < total; i++)
        s->dash[i] = fclamp(d[i % n]);
    s->ndash = (uint8_t)total;
}

size_t au_c2d_get_line_dash(const AuCanvas2D *c, const float **d)
{
    *d = c2d_state(c)->dash;
    return c2d_state(c)->ndash;
}

void au_c2d_set_dash_offset(AuCanvas2D *c, double o)
{
    if (isfinite(o))
        c2d_state(c)->dash_offset = fclamp(o);
}

double au_c2d_get_dash_offset(const AuCanvas2D *c) { return c2d_state(c)->dash_offset; }
void au_c2d_set_shadow_color(AuCanvas2D *c, AuColor color) { c2d_state(c)->shadow_color = color; }
AuColor au_c2d_get_shadow_color(const AuCanvas2D *c) { return c2d_state(c)->shadow_color; }

void au_c2d_set_shadow_blur(AuCanvas2D *c, double b)
{
    if (b >= 0 && isfinite(b))
        c2d_state(c)->shadow_blur = fclamp(b);
}

double au_c2d_get_shadow_blur(const AuCanvas2D *c) { return c2d_state(c)->shadow_blur; }

void au_c2d_set_shadow_offset(AuCanvas2D *c, int axis, double v)
{
    if (!isfinite(v))
        return;
    if (axis)
        c2d_state(c)->shadow_y = fclamp(v);
    else
        c2d_state(c)->shadow_x = fclamp(v);
}

double au_c2d_get_shadow_offset(const AuCanvas2D *c, int axis)
{
    return axis ? c2d_state(c)->shadow_y : c2d_state(c)->shadow_x;
}

void au_c2d_set_spacing(AuCanvas2D *c, int which, double px)
{
    if (isfinite(px))
        c2d_state(c)->spacing[which & 1] = fclamp(px);
}

double au_c2d_get_spacing(const AuCanvas2D *c, int which) { return c2d_state(c)->spacing[which & 1]; }

/* ---- Fonts ---- */

void au_c2d_set_font(AuCanvas2D *c, const char *families, int weight, bool italic, double size, const char *css)
{
    if (!families || !css || !(size >= 0) || !isfinite(size))
        return;
    if (size > 10000)
        size = 10000;
    if (weight < 1)
        weight = 1;
    if (weight > 1000)
        weight = 1000;
    C2DState *s = c2d_state(c);
    font_unref(s->font);
    s->font = font_new(families, weight, italic, (float)size, css);
}

const char *au_c2d_get_font(const AuCanvas2D *c)
{
    return c2d_state(c)->font->css;
}

/* The font list for the current font, from the set's small cache: a list
 * is matched once per distinct font, not per text call. */
AuFontList *c2d_font_list(AuCanvas2D *c)
{
    AuCanvasSet *s = c->set;
    if (!s || !s->fonts)
        return NULL;
    C2DFont *f = c2d_state(c)->font;
    int slot = 0;
    for (int i = 0; i < C2D_FONT_CACHE; i++) {
        C2DFont *g = s->fc[i].font;
        if (g && (g == f || (g->weight == f->weight && g->italic == f->italic && !strcmp(g->families, f->families)))) {
            s->fc[i].used = ++s->fc_clock;
            return s->fc[i].list;
        }
    }
    /* The least recently used slot, or an empty one. */
    for (int i = 0; i < C2D_FONT_CACHE; i++) {
        if (!s->fc[i].font) {
            slot = i;
            break;
        }
        if (s->fc[i].used < s->fc[slot].used)
            slot = i;
    }
    if (s->fc[slot].list)
        au_fontlist_free(s->fc[slot].list);
    font_unref(s->fc[slot].font);
    s->fc[slot].font = f;
    f->refs++;
    s->fc[slot].list = au_fontlist_parse(s->fonts, f->families, f->weight, f->italic);
    s->fc[slot].used = ++s->fc_clock;
    return s->fc[slot].list;
}

/* ---- Pixel conversion ---- */

bool au_c2d_native_big_endian(void)
{
    const uint16_t probe = 0x0100;
    return *(const uint8_t *)&probe == 1;
}

/* 255 * 65536 / a, rounded, for dividing a premultiplied channel by its
 * alpha. Filled on first use; the values never change. */
static uint32_t unpremul_tab[256];

static const uint32_t *unpremul_table(void)
{
    if (!unpremul_tab[255]) {
        for (uint32_t a = 1; a < 256; a++)
            unpremul_tab[a] = (255u * 65536u + a / 2) / a;
    }
    return unpremul_tab;
}

void au_c2d_row_to_rgba(const AuPixel *src, uint8_t *rgba, size_t n, bool opaque, bool big_endian)
{
    const uint32_t *tab = unpremul_table();
    for (size_t i = 0; i < n; i++) {
        uint32_t p = src[i];
        uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        if (opaque) {
            a = 255;
        } else if (a == 0) {
            r = g = b = 0;
        } else if (a != 255) {
            uint32_t k = tab[a];
            r = (r * k + 0x8000u) >> 16;
            g = (g * k + 0x8000u) >> 16;
            b = (b * k + 0x8000u) >> 16;
            if (r > 255)
                r = 255;
            if (g > 255)
                g = 255;
            if (b > 255)
                b = 255;
        }
        /* The four bytes R, G, B, A as one word of the given byte order. */
        uint32_t w = big_endian ? (r << 24) | (g << 16) | (b << 8) | a : (a << 24) | (b << 16) | (g << 8) | r;
        memcpy(rgba + 4 * i, &w, 4);
    }
}

void au_c2d_row_from_rgba(const uint8_t *rgba, AuPixel *dst, size_t n, bool big_endian)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t w;
        memcpy(&w, rgba + 4 * i, 4);
        uint32_t r, g, b, a;
        if (big_endian) {
            r = w >> 24;
            g = (w >> 16) & 255;
            b = (w >> 8) & 255;
            a = w & 255;
        } else {
            a = w >> 24;
            b = (w >> 16) & 255;
            g = (w >> 8) & 255;
            r = w & 255;
        }
        if (a == 255) {
            dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        } else if (a == 0) {
            dst[i] = 0;
        } else {
            /* x * a / 255, rounded. */
            uint32_t tr = r * a + 128, tg = g * a + 128, tb = b * a + 128;
            r = (tr + (tr >> 8)) >> 8;
            g = (tg + (tg >> 8)) >> 8;
            b = (tb + (tb >> 8)) >> 8;
            dst[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* A run of n pixels a canvas without alpha shows before anything was
 * drawn: opaque black. */
static void opaque_black(uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 0;
        out[4 * i + 3] = 255;
    }
}

void au_c2d_get_image_data(AuCanvas2D *c, int x, int y, int w, int h, uint8_t *rgba)
{
    if (w <= 0 || h <= 0)
        return;
    bool be = au_c2d_native_big_endian();
    const AuSurface *s = c->surf;
    /* The part of the request that lies on the canvas. An inert canvas
     * has no pixels at all. */
    AuIRect in = {0, 0, 0, 0};
    if (!c->inert) {
        /* 64-bit: x + w may pass INT_MAX */
        int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
        in.x0 = x > 0 ? x : 0;
        in.y0 = y > 0 ? y : 0;
        in.x1 = x1 < c->w ? (int)x1 : c->w;
        in.y1 = y1 < c->h ? (int)y1 : c->h;
    }
    bool any = in.x1 > in.x0 && in.y1 > in.y0;
    for (int row = 0; row < h; row++) {
        uint8_t *out = rgba + (size_t)row * (size_t)w * 4;
        int64_t sy = (int64_t)y + row;
        if (!any || sy < in.y0 || sy >= in.y1) {
            memset(out, 0, (size_t)w * 4);
            continue;
        }
        int left = in.x0 - x, n = in.x1 - in.x0;
        if (left > 0)
            memset(out, 0, (size_t)left * 4);
        if (s)
            au_c2d_row_to_rgba(s->pixels + (size_t)sy * s->stride + in.x0, out + (size_t)left * 4, (size_t)n, !c->alpha,
                               be);
        else if (c->alpha)
            memset(out + (size_t)left * 4, 0, (size_t)n * 4);
        else
            opaque_black(out + (size_t)left * 4, (size_t)n);
        if (left + n < w)
            memset(out + (size_t)(left + n) * 4, 0, (size_t)(w - left - n) * 4);
    }
}

void au_c2d_put_image_data(AuCanvas2D *c, const uint8_t *rgba, int w, int h, int x, int y, int dx, int dy,
                           int dw, int dh)
{
    if (w <= 0 || h <= 0)
        return;
    /* The dirty rectangle, by the standard's rules: negative sizes turn
     * round, then it is cut to the data. 64-bit sums: any int may come. */
    int64_t x0 = dx, y0 = dy, x1 = (int64_t)dx + dw, y1 = (int64_t)dy + dh;
    if (x1 < x0) {
        int64_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        int64_t t = y0;
        y0 = y1;
        y1 = t;
    }
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;
    /* And to the canvas. */
    if ((int64_t)x + x0 < 0)
        x0 = -(int64_t)x;
    if ((int64_t)y + y0 < 0)
        y0 = -(int64_t)y;
    if ((int64_t)x + x1 > c->w)
        x1 = (int64_t)c->w - x;
    if ((int64_t)y + y1 > c->h)
        y1 = (int64_t)c->h - y;
    if (x1 <= x0 || y1 <= y0 || !c2d_ensure_surface(c))
        return;
    bool be = au_c2d_native_big_endian();
    AuSurface *s = c->surf;
    size_t n = (size_t)(x1 - x0);
    for (int64_t row = y0; row < y1; row++) {
        AuPixel *d = s->pixels + (size_t)(y + row) * s->stride + (size_t)(x + x0);
        au_c2d_row_from_rgba(rgba + ((size_t)row * (size_t)w + (size_t)x0) * 4, d, n, be);
        if (!c->alpha)
            for (size_t i = 0; i < n; i++)
                d[i] |= 0xFF000000u; /* an opaque canvas shows the premultiplied colour */
    }
    c2d_touch(c, (AuIRect){(int)(x + x0), (int)(y + y0), (int)(x + x1), (int)(y + y1)});
}
