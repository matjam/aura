/* The document view's images. See docimages.h. */
#include "docimages.h"

#include "page.h"

#include <string.h>

typedef struct Tracked {
    AuElement *el; /* referenced */
    AuImageEntry *entry;
} Tracked;

struct AuDocImages {
    AuImageCache *cache;
    size_t budget;
    uint32_t (*clock)(void *ctx);
    void *clock_ctx;
    AuFontSet *fonts; /* text in SVG images */
    Tracked *t;
    uint32_t nt, tcap;
    bool scan_needed;
    bool in_paint;
};

AuDocImages *au_docimages_new(void)
{
    AuDocImages *m = au_calloc(sizeof *m);
    m->budget = AU_IMAGE_CACHE_DEFAULT_BUDGET;
    m->cache = au_image_cache_new(m->budget);
    m->scan_needed = true;
    return m;
}

static void untrack(AuDocImages *m)
{
    for (uint32_t i = 0; i < m->nt; i++)
        au_node_unref(&m->t[i].el->node);
    m->nt = 0;
}

void au_docimages_free(AuDocImages *m)
{
    if (!m)
        return;
    untrack(m);
    au_free(m->t);
    au_image_cache_free(m->cache);
    au_free(m);
}

void au_docimages_clear(AuDocImages *m)
{
    untrack(m);
    au_image_cache_free(m->cache);
    m->cache = au_image_cache_new(m->budget);
    if (m->clock)
        au_image_cache_set_clock(m->cache, m->clock, m->clock_ctx);
    au_image_cache_set_fonts(m->cache, m->fonts);
    m->scan_needed = true;
}

void au_docimages_set_fonts(AuDocImages *m, AuFontSet *fonts)
{
    m->fonts = fonts;
    au_image_cache_set_fonts(m->cache, fonts);
}

void au_docimages_use(AuDocImages *m, const char *url, int dw, int dh)
{
    AuImageEntry *e = au_image_cache_find(m->cache, url);
    if (e && m->in_paint)
        au_image_entry_use(m->cache, e, dw, dh);
}

void au_docimages_set_budget(AuDocImages *m, size_t bytes)
{
    m->budget = bytes ? bytes : AU_IMAGE_CACHE_DEFAULT_BUDGET;
    au_image_cache_set_budget(m->cache, m->budget);
}

void au_docimages_set_clock(AuDocImages *m, uint32_t (*now_ms)(void *ctx), void *ctx)
{
    m->clock = now_ms;
    m->clock_ctx = ctx;
    au_image_cache_set_clock(m->cache, now_ms, ctx);
}

AuImageCache *au_docimages_cache(AuDocImages *m) { return m->cache; }

AuDocImageResult au_docimages_lookup(AuDocImages *m, const char *url, int *w, int *h, const AuSurface **pixels,
                                     AuImageEntry **entry, bool *created)
{
    *created = false;
    AuImageEntry *e = au_image_cache_find(m->cache, url);
    if (!e) {
        if (au_image_cache_count(m->cache) >= 4096)
            return AU_DOCIMG_FAILED;
        e = au_image_cache_add(m->cache, url);
        *created = true;
    }
    *entry = e;
    switch (au_image_entry_state(e)) {
    case AU_IMAGE_FAILED:
        return AU_DOCIMG_FAILED;
    case AU_IMAGE_PENDING:
        return AU_DOCIMG_PENDING;
    default:
        break;
    }
    if (m->in_paint)
        au_image_entry_use(m->cache, e, 0, 0); /* painted now: decode it */
    if (au_image_entry_state(e) == AU_IMAGE_FAILED)
        return AU_DOCIMG_FAILED;
    au_image_entry_size(e, w, h);
    *pixels = au_image_entry_surface(e);
    return AU_DOCIMG_SIZED;
}

void au_docimages_tree_changed(AuDocImages *m) { m->scan_needed = true; }

static const char *get_attr(const AuElement *el, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr(el, a) : NULL;
}

/* Finds the <img> and <input type=image> elements and their entries. */
static void scan(AuDocImages *m, AuDocument *doc, const char *base)
{
    untrack(m);
    m->scan_needed = false;
    if (!doc)
        return;
    for (AuNode *n = au_node_next(&doc->node, &doc->node); n; n = au_node_next(n, &doc->node)) {
        if (n->type != AU_NODE_ELEMENT)
            continue;
        AuElement *el = au_as_element(n);
        bool img = au_is_html_element(n, AU_ATOM_img);
        if (!img && au_is_html_element(n, AU_ATOM_input)) {
            const char *ty = get_attr(el, "type");
            img = ty && au_ieq(ty, strlen(ty), "image", 5);
        }
        if (!img)
            continue;
        const char *src = get_attr(el, "src");
        char abs[2048];
        if (!src || !*src || !au_url_resolve(base, src, abs, sizeof abs))
            continue;
        AuImageEntry *e = au_image_cache_find(m->cache, abs);
        if (!e)
            continue; /* layout has not asked for it yet */
        if (m->nt == m->tcap) {
            m->tcap = m->tcap ? m->tcap * 2 : 32;
            m->t = au_realloc(m->t, m->tcap * sizeof *m->t);
        }
        au_node_ref(n);
        m->t[m->nt].el = el;
        m->t[m->nt].entry = e;
        m->nt++;
    }
}

void au_docimages_prepare(AuDocImages *m, AuDocument *doc, AuLayout *L, const char *base_url, AuRect view)
{
    /* Rescan when the tree changed, or when layout asked for images the
     * last scan did not know (they were not in the cache yet). */
    if (m->scan_needed)
        scan(m, doc, base_url);
    au_image_cache_frame_begin(m->cache);
    /* Near the viewport: one viewport above and below, half across. */
    float y0 = view.y - view.h, y1 = view.y + 2 * view.h;
    float x0 = view.x - view.w / 2, x1 = view.x + view.w * 1.5f;
    for (uint32_t i = 0; i < m->nt; i++) {
        AuRect r;
        if (!au_layout_element_rect(L, m->t[i].el, &r))
            continue;
        if (r.x + r.w < x0 || r.x > x1 || r.y + r.h < y0 || r.y > y1)
            continue;
        int dw = (int)(r.w + 0.5f), dh = (int)(r.h + 0.5f);
        au_image_entry_use(m->cache, m->t[i].entry, dw > 0 ? dw : 1, dh > 0 ? dh : 1);
    }
}

void au_docimages_paint_begin(AuDocImages *m) { m->in_paint = true; }

void au_docimages_paint_end(AuDocImages *m)
{
    m->in_paint = false;
    /* Images decoded during this paint keep their changed mark, so that
     * images_changed repaints them where they show: a paint that started
     * a decode drew the image without pixels, and a partial paint (a
     * scrolled strip) did not draw it everywhere. */
    au_image_cache_trim(m->cache);
}

uint32_t au_docimages_tick(AuDocImages *m, uint32_t now_ms) { return au_image_cache_tick(m->cache, now_ms); }

void au_docimages_invalidate(AuDocImages *m, AuLayout *L, AuIRect doc_all, void (*fn)(void *ctx, AuIRect r),
                             void *ctx)
{
    uint32_t n = au_image_cache_count(m->cache);
    for (uint32_t i = 0; i < n; i++) {
        AuImageEntry *e = au_image_cache_entry(m->cache, i);
        if (!au_image_entry_take_changed(e))
            continue;
        bool any = false;
        for (uint32_t k = 0; k < m->nt; k++) {
            AuRect r;
            if (m->t[k].entry != e || !au_layout_element_rect(L, m->t[k].el, &r))
                continue;
            any = true;
            fn(ctx, (AuIRect){(int)r.x - 1, (int)r.y - 1, (int)(r.x + r.w) + 2, (int)(r.y + r.h) + 2});
        }
        if (!any)
            fn(ctx, doc_all);
    }
}

bool au_docimages_take_layout_changed(AuDocImages *m)
{
    bool ch = au_image_cache_take_layout_changed(m->cache);
    if (ch)
        m->scan_needed = true; /* new entries may now be tracked */
    return ch;
}

bool au_docimages_animating(const AuDocImages *m)
{
    AuImageCacheStats s;
    au_image_cache_stats(m->cache, &s);
    return s.animating > 0;
}
