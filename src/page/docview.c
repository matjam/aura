/* The document view: parser, style sheets, images, resolver, layout,
 * painting, input. */
#include "page.h"
#include "docimages.h"
#include "webfont.h"

#include <stdio.h>
#include <string.h>

/* A style sheet source: its text once loaded, and its @import children. */
typedef struct SheetSrc SheetSrc;
struct SheetSrc {
    char *url;    /* base URL for the sheet (absolute) */
    char *media;  /* media attribute or @import media, NULL for all */
    char *text;   /* NULL while loading (or after a failure) */
    size_t len;
    uint32_t req; /* outstanding request, 0 if none */
    bool failed;
    SheetSrc **imports; /* one per @import of the sheet, in order */
    uint32_t nimports;
    bool imports_scanned;
};

/* A <style> or <link> element in document order. */
typedef struct SheetSlot {
    AuElement *el; /* referenced */
    SheetSrc *src;
} SheetSlot;

typedef struct Request {
    uint32_t id;
    AuResourceKind kind;
    SheetSrc *sheet; /* for style sheets */
    AuImageEntry *image; /* for images: bytes go straight to the image cache */
    bool font;           /* an @font-face source */
    AuBuf data;
} Request;

struct AuDocView {
    AuFontSet *fonts;
    AuWebFonts *webfonts; /* the document's @font-face faces, over fonts */
    AuLoader loader;
    AuDocViewCallbacks cb;
    AuHtmlParser *parser;
    AuDocument *doc;
    char *url, *base;
    char *title;
    char *user_css; /* the user style sheet, or NULL */
    AuCssFontPrefs font_prefs; /* au_docview_set_font_prefs */
    bool has_font_prefs;
    AuCssResolver *resolver;
    AuLayout *layout;
    SheetSlot *slots;
    uint32_t nslots, slots_cap;
    AuDocImages *imgs; /* docimages.c */
    Request *reqs;
    uint32_t nreqs, reqs_cap;
    int vw, vh;
    int scroll_x, scroll_y;
    bool parsing;
    bool tree_dirty, sheets_dirty, restyle_needed, style_all, images_dirty;
    bool finished_reported;
    float last_w, last_h;
    AuElement *hover, *active;
    char hover_link[2048];
    uint32_t scanned_nodes;
};

/* ---- Helpers ---- */

static char *dup(const char *s)
{
    return au_strndup(s ? s : "", strlen(s ? s : ""));
}

static const char *attr(const AuElement *el, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr(el, a) : NULL;
}

static void invalidate_all(AuDocView *dv)
{
    if (dv->cb.invalidate) {
        float w, h;
        au_layout_document_size(dv->layout, &w, &h);
        dv->cb.invalidate(dv->cb.ctx, dv, (AuIRect){0, 0, (int)w + 1, (int)h + 1});
    }
}

static bool layout_image(void *ctx, const char *url, int *w, int *h, const AuSurface **pixels);
static bool layout_resolve(void *ctx, const char *url, char *out, size_t cap);
static Request *add_request(AuDocView *dv, const char *url, AuResourceKind kind);

/* A new layout drawing with fonts (the layout caches faces, so a change
 * of font set or a new web font needs a new one). */
static void new_layout(AuDocView *dv, AuFontSet *fonts)
{
    if (dv->layout)
        au_layout_free(dv->layout);
    AuLayoutEnv env = {fonts, dv, layout_image, layout_resolve};
    dv->layout = au_layout_new(&env);
    if (dv->doc)
        au_layout_set_document(dv->layout, dv->doc);
    if (dv->resolver)
        au_css_resolver_set_metrics(dv->resolver, au_layout_css_metrics, dv->layout);
}

static uint32_t fetch_font(void *ctx, const char *url)
{
    AuDocView *dv = ctx;
    Request *r = add_request(dv, url, AU_RES_OTHER);
    if (!r)
        return 0;
    r->font = true;
    dv->finished_reported = false;
    return r->id;
}

AuDocView *au_docview_new(AuFontSet *fonts, const AuLoader *loader, const AuDocViewCallbacks *cb)
{
    AuDocView *dv = au_calloc(sizeof *dv);
    dv->fonts = fonts;
    if (loader)
        dv->loader = *loader;
    if (cb)
        dv->cb = *cb;
    /* The image cache lives outside the layout, so a new layout (a web
     * font arriving) keeps every image and its animation. */
    dv->imgs = au_docimages_new();
    new_layout(dv, fonts);
    dv->url = dup("about:blank");
    dv->base = dup("about:blank");
    dv->title = dup("");
    dv->vw = 800;
    dv->vh = 600;
    return dv;
}

static void free_sheet_src(SheetSrc *s)
{
    if (!s)
        return;
    for (uint32_t i = 0; i < s->nimports; i++)
        free_sheet_src(s->imports[i]);
    au_free(s->imports);
    au_free(s->url);
    au_free(s->media);
    au_free(s->text);
    au_free(s);
}

static void clear_document(AuDocView *dv)
{
    for (uint32_t i = 0; i < dv->nreqs; i++) {
        if (dv->loader.cancel)
            dv->loader.cancel(dv->loader.ctx, dv->reqs[i].id);
        au_buf_free(&dv->reqs[i].data);
    }
    dv->nreqs = 0;
    au_layout_set_document(dv->layout, NULL);
    for (uint32_t i = 0; i < dv->nslots; i++) {
        au_node_unref(&dv->slots[i].el->node);
        free_sheet_src(dv->slots[i].src);
    }
    dv->nslots = 0;
    au_docimages_clear(dv->imgs);
    if (dv->hover)
        au_node_unref(&dv->hover->node);
    if (dv->active)
        au_node_unref(&dv->active->node);
    dv->hover = dv->active = NULL;
    dv->hover_link[0] = 0;
    if (dv->doc)
        au_css_clear_document(dv->doc);
    if (dv->parser) {
        au_html_parser_free(dv->parser);
        dv->parser = NULL;
    }
    if (dv->doc) {
        au_node_unref(&dv->doc->node);
        dv->doc = NULL;
    }
    if (dv->resolver) {
        au_css_resolver_free(dv->resolver);
        dv->resolver = NULL;
    }
    if (dv->webfonts) {
        new_layout(dv, dv->fonts); /* drops the faces of the old set */
        au_webfonts_free(dv->webfonts);
        dv->webfonts = NULL;
    }
    dv->scanned_nodes = 0;
}

void au_docview_free(AuDocView *dv)
{
    if (!dv)
        return;
    clear_document(dv);
    au_layout_free(dv->layout);
    au_docimages_free(dv->imgs);
    au_free(dv->slots);
    au_free(dv->reqs);
    au_free(dv->url);
    au_free(dv->base);
    au_free(dv->title);
    au_free(dv->user_css);
    au_free(dv);
}

void au_docview_set_user_css(AuDocView *dv, const char *css)
{
    if ((!css && !dv->user_css) || (css && dv->user_css && strcmp(css, dv->user_css) == 0))
        return;
    au_free(dv->user_css);
    dv->user_css = css ? dup(css) : NULL;
    dv->sheets_dirty = true;
}

/* ---- Images (the cache and its bookkeeping are in docimages.c) ---- */

static void invalidate_fn(void *ctx, AuIRect r)
{
    AuDocView *dv = ctx;
    if (dv->cb.invalidate)
        dv->cb.invalidate(dv->cb.ctx, dv, r);
}

/* Reports images whose size became known (layout rebuilds) and repaints
 * images whose pixels changed. */
static void images_changed(AuDocView *dv)
{
    if (au_docimages_take_layout_changed(dv->imgs)) {
        dv->images_dirty = true;
        invalidate_all(dv);
    }
    float w, h;
    au_layout_document_size(dv->layout, &w, &h);
    au_docimages_invalidate(dv->imgs, dv->layout, (AuIRect){0, 0, (int)w + 1, (int)h + 1}, invalidate_fn, dv);
}

static void images_data(AuDocView *dv, AuImageEntry *e, const void *data, size_t len)
{
    au_image_entry_data(au_docimages_cache(dv->imgs), e, data, len);
    images_changed(dv);
}

void au_docview_set_image_budget(AuDocView *dv, size_t bytes)
{
    au_docimages_set_budget(dv->imgs, bytes);
}

void au_docview_set_clock(AuDocView *dv, uint32_t (*now_ms)(void *ctx), void *ctx)
{
    au_docimages_set_clock(dv->imgs, now_ms, ctx);
}

uint32_t au_docview_animate(AuDocView *dv, uint32_t now_ms)
{
    uint32_t next = au_docimages_tick(dv->imgs, now_ms);
    images_changed(dv);
    return next;
}

bool au_docview_animating(const AuDocView *dv)
{
    return au_docimages_animating(dv->imgs);
}

void au_docview_image_stats(const AuDocView *dv, AuImageCacheStats *s)
{
    au_image_cache_stats(au_docimages_cache(dv->imgs), s);
}

void au_docview_set_font_prefs(AuDocView *dv, const AuCssFontPrefs *p)
{
    if (dv->has_font_prefs && !memcmp(&dv->font_prefs, p, sizeof *p))
        return;
    dv->font_prefs = *p;
    dv->has_font_prefs = true;
    if (dv->resolver) {
        au_css_resolver_set_font_prefs(dv->resolver, p);
        dv->style_all = true;
    }
}

/* ---- Requests ---- */

static Request *add_request(AuDocView *dv, const char *url, AuResourceKind kind)
{
    if (!dv->loader.request)
        return NULL;
    uint32_t id = dv->loader.request(dv->loader.ctx, dv, url, kind);
    if (!id)
        return NULL;
    if (dv->nreqs == dv->reqs_cap) {
        dv->reqs_cap = dv->reqs_cap ? dv->reqs_cap * 2 : 16;
        dv->reqs = au_realloc(dv->reqs, dv->reqs_cap * sizeof *dv->reqs);
    }
    Request *r = &dv->reqs[dv->nreqs++];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->kind = kind;
    au_buf_init(&r->data);
    return r;
}

static Request *find_request(AuDocView *dv, uint32_t id)
{
    for (uint32_t i = 0; i < dv->nreqs; i++)
        if (dv->reqs[i].id == id)
            return &dv->reqs[i];
    return NULL;
}

static void request_sheet(AuDocView *dv, SheetSrc *s)
{
    Request *r = add_request(dv, s->url, AU_RES_STYLESHEET);
    if (!r) {
        s->failed = true;
        return;
    }
    r->sheet = s;
    s->req = r->id;
}

/* Parses a loaded sheet once to find its @imports and requests them. */
static void scan_imports(AuDocView *dv, SheetSrc *s)
{
    if (s->imports_scanned || !s->text)
        return;
    s->imports_scanned = true;
    AuCssSheet *sh = au_css_sheet_parse(s->text, s->len, AU_CSS_ORIGIN_AUTHOR, s->url, s->media);
    size_t n = au_css_sheet_import_count(sh);
    if (n > 64)
        n = 64;
    s->imports = n ? au_calloc(n * sizeof *s->imports) : NULL;
    s->nimports = (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        const AuCssImport *im = au_css_sheet_import(sh, i);
        if (!im->supported)
            continue;
        char abs[2048];
        if (!au_url_resolve(s->url, im->url, abs, sizeof abs))
            continue;
        SheetSrc *c = au_calloc(sizeof *c);
        c->url = dup(abs);
        c->media = im->media && *im->media ? dup(im->media) : NULL;
        s->imports[i] = c;
        request_sheet(dv, c);
    }
    au_css_sheet_free(sh);
}

void au_docview_resource_data(AuDocView *dv, uint32_t id, const void *data, size_t len)
{
    Request *r = find_request(dv, id);
    if (!r)
        return;
    if (r->kind == AU_RES_IMAGE && r->image) {
        images_data(dv, r->image, data, len);
        return;
    }
    if (r->data.len + len > (size_t)64 * 1024 * 1024)
        return; /* refuse absurd resources */
    au_buf_append(&r->data, data, len);
}

static void maybe_finished(AuDocView *dv)
{
    if (!au_docview_loading(dv) && !dv->finished_reported) {
        dv->finished_reported = true;
        if (dv->cb.load_finished)
            dv->cb.load_finished(dv->cb.ctx, dv);
    }
}

void au_docview_resource_done(AuDocView *dv, uint32_t id, bool ok)
{
    Request *r = find_request(dv, id);
    if (!r)
        return;
    Request req = *r;
    *r = dv->reqs[--dv->nreqs];
    if (req.kind == AU_RES_STYLESHEET && req.sheet) {
        SheetSrc *s = req.sheet;
        s->req = 0;
        if (ok) {
            size_t n;
            s->text = au_buf_take(&req.data, &n);
            s->len = n;
            scan_imports(dv, s);
        } else {
            s->failed = true;
        }
        dv->sheets_dirty = true;
    } else if (req.kind == AU_RES_IMAGE && req.image) {
        au_image_entry_done(au_docimages_cache(dv->imgs), req.image, ok);
        images_changed(dv);
    } else if (req.font && dv->webfonts) {
        au_webfonts_done(dv->webfonts, req.id, req.data.data, req.data.len, ok, fetch_font, dv);
    }
    au_buf_free(&req.data);
    invalidate_all(dv);
    maybe_finished(dv);
}

uint32_t au_docview_pending(const AuDocView *dv)
{
    return dv->nreqs;
}

bool au_docview_loading(const AuDocView *dv)
{
    return dv->parsing || dv->nreqs > 0;
}

/* ---- Layout callbacks ---- */

static bool layout_resolve(void *ctx, const char *url, char *out, size_t cap)
{
    AuDocView *dv = ctx;
    return au_url_resolve(dv->base, url, out, cap);
}

/* Layout's image lookup. A pending image reports a size of 0 x 0 and a
 * transparent surface (an empty box, or the size the page gives it); a
 * failed one returns false, so layout shows its alt text. */
static bool layout_image(void *ctx, const char *url, int *w, int *h, const AuSurface **pixels)
{
    AuDocView *dv = ctx;
    AuImageEntry *e;
    bool created;
    AuDocImageResult res = au_docimages_lookup(dv->imgs, url, w, h, pixels, &e, &created);
    if (created) {
        Request *r = add_request(dv, url, AU_RES_IMAGE);
        if (r) {
            r->image = e;
            dv->finished_reported = false;
        } else {
            au_image_entry_done(au_docimages_cache(dv->imgs), e, false);
            au_docimages_take_layout_changed(dv->imgs);
            return false;
        }
    }
    if (res == AU_DOCIMG_FAILED)
        return false;
    if (res == AU_DOCIMG_PENDING) {
        *w = *h = 0;
        *pixels = au_image_entry_surface(e);
    }
    return true;
}

/* ---- Document scanning: sheets, base, title ---- */

static bool is_stylesheet_link(const AuElement *el)
{
    const char *rel = attr(el, "rel");
    if (!rel)
        return false;
    /* rel is a space-separated token list containing "stylesheet" but not "alternate" */
    bool sheet = false, alt = false;
    const char *p = rel;
    while (*p) {
        while (*p && au_is_space((unsigned char)*p))
            p++;
        const char *s = p;
        while (*p && !au_is_space((unsigned char)*p))
            p++;
        size_t n = (size_t)(p - s);
        if (au_ieq(s, n, "stylesheet", 10))
            sheet = true;
        if (au_ieq(s, n, "alternate", 9))
            alt = true;
    }
    return sheet && !alt;
}

static void text_content(AuNode *n, AuBuf *out)
{
    for (AuNode *c = au_node_next(n, n); c; c = au_node_next(c, n))
        if (c->type == AU_NODE_TEXT)
            au_buf_append(out, au_as_text(c)->data, au_as_text(c)->len);
}

static bool has_slot(AuDocView *dv, AuElement *el)
{
    for (uint32_t i = 0; i < dv->nslots; i++)
        if (dv->slots[i].el == el)
            return true;
    return false;
}

static void add_slot(AuDocView *dv, AuElement *el, uint32_t index)
{
    if (dv->nslots == dv->slots_cap) {
        dv->slots_cap = dv->slots_cap ? dv->slots_cap * 2 : 8;
        dv->slots = au_realloc(dv->slots, dv->slots_cap * sizeof *dv->slots);
    }
    memmove(&dv->slots[index + 1], &dv->slots[index], (dv->nslots - index) * sizeof *dv->slots);
    dv->nslots++;
    SheetSlot *s = &dv->slots[index];
    s->el = el;
    au_node_ref(&el->node);
    s->src = au_calloc(sizeof *s->src);
    const char *media = attr(el, "media");
    s->src->media = media && *media ? dup(media) : NULL;
    if (au_is_html_element(&el->node, AU_ATOM_style)) {
        AuBuf t;
        au_buf_init(&t);
        text_content(&el->node, &t);
        size_t n;
        s->src->text = au_buf_take(&t, &n);
        s->src->len = n;
        s->src->url = dup(dv->base);
        scan_imports(dv, s->src);
    } else {
        char abs[2048];
        const char *href = attr(el, "href");
        if (href && au_url_resolve(dv->base, href, abs, sizeof abs)) {
            s->src->url = dup(abs);
            request_sheet(dv, s->src);
            dv->finished_reported = false;
        } else {
            s->src->url = dup(dv->base);
            s->src->failed = true;
        }
    }
    dv->sheets_dirty = true;
}

/* Finds new <style>, <link rel=stylesheet>, <base> and <title> elements.
 * A <style> is taken once its end tag has been seen (the parser has moved
 * past it), so its text is complete. */
static void scan_document(AuDocView *dv, bool final)
{
    if (!dv->doc)
        return;
    uint32_t index = 0;
    bool base_seen = false, title_seen = false;
    for (AuNode *n = au_node_next(&dv->doc->node, &dv->doc->node); n; n = au_node_next(n, &dv->doc->node)) {
        if (n->type != AU_NODE_ELEMENT)
            continue;
        AuElement *el = au_as_element(n);
        if (el->ns != AU_NS_HTML)
            continue;
        bool complete = final || n->next || (n->parent && n->parent->next);
        if (el->tag == AU_ATOM_style || (el->tag == AU_ATOM_link && is_stylesheet_link(el))) {
            /* index counts the slots before this element: slots are kept in
             * document order, an incomplete <style> has none yet. */
            if (has_slot(dv, el)) {
                index++;
            } else if (complete || el->tag != AU_ATOM_style) {
                add_slot(dv, el, index);
                index++;
            }
        } else if (el->tag == AU_ATOM_base && !base_seen) {
            const char *href = attr(el, "href");
            if (href) {
                base_seen = true;
                char abs[2048];
                if (au_url_resolve(dv->url, href, abs, sizeof abs) && strcmp(abs, dv->base) != 0) {
                    au_free(dv->base);
                    dv->base = dup(abs);
                }
            }
        } else if (el->tag == AU_ATOM_title && !title_seen && complete) {
            title_seen = true;
            AuBuf t, c;
            au_buf_init(&t);
            au_buf_init(&c);
            text_content(n, &t);
            bool sp = true;
            for (size_t i = 0; i < t.len; i++) {
                char ch = t.data[i];
                if (au_is_space((unsigned char)ch)) {
                    if (!sp)
                        au_buf_putc(&c, ' ');
                    sp = true;
                } else {
                    au_buf_putc(&c, ch);
                    sp = false;
                }
            }
            while (c.len && c.data[c.len - 1] == ' ')
                c.len--;
            au_buf_cstr(&c);
            if (strcmp(c.data ? c.data : "", dv->title) != 0) {
                au_free(dv->title);
                dv->title = dup(c.data ? c.data : "");
                if (dv->cb.title_changed)
                    dv->cb.title_changed(dv->cb.ctx, dv, dv->title);
            }
            au_buf_free(&t);
            au_buf_free(&c);
        }
    }
}

/* ---- Loading ---- */

void au_docview_begin(AuDocView *dv, const char *url)
{
    clear_document(dv);
    dv->webfonts = au_webfonts_new(dv->fonts);
    new_layout(dv, au_webfonts_set(dv->webfonts));
    au_free(dv->url);
    au_free(dv->base);
    dv->url = dup(url);
    dv->base = dup(url);
    if (dv->title[0]) {
        au_free(dv->title);
        dv->title = dup("");
        if (dv->cb.title_changed)
            dv->cb.title_changed(dv->cb.ctx, dv, "");
    }
    AuHtmlOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.scripting = false;
    opts.encoding = AU_HTML_ENC_SNIFF;
    dv->parser = au_html_parser_new(&opts);
    dv->doc = au_html_parser_document(dv->parser);
    au_node_ref(&dv->doc->node);
    dv->resolver = au_css_resolver_new();
    au_css_resolver_set_metrics(dv->resolver, au_layout_css_metrics, dv->layout);
    if (dv->has_font_prefs)
        au_css_resolver_set_font_prefs(dv->resolver, &dv->font_prefs);
    au_layout_set_document(dv->layout, dv->doc);
    dv->parsing = true;
    dv->tree_dirty = dv->sheets_dirty = true;
    dv->finished_reported = false;
    dv->scroll_x = dv->scroll_y = 0;
}

void au_docview_feed(AuDocView *dv, const char *bytes, size_t len)
{
    if (!dv->parser)
        au_docview_begin(dv, dv->url);
    au_html_parser_feed(dv->parser, bytes, len);
    dv->tree_dirty = true;
    scan_document(dv, false);
}

void au_docview_finish(AuDocView *dv)
{
    if (!dv->parser)
        au_docview_begin(dv, dv->url);
    au_html_parser_finish(dv->parser);
    dv->parsing = false;
    dv->tree_dirty = true;
    scan_document(dv, true);
    invalidate_all(dv);
    maybe_finished(dv);
}

void au_docview_load(AuDocView *dv, const char *bytes, size_t len, const char *url)
{
    au_docview_begin(dv, url);
    au_docview_feed(dv, bytes, len);
    au_docview_finish(dv);
}

const char *au_docview_url(const AuDocView *dv) { return dv->url; }
const char *au_docview_base_url(const AuDocView *dv) { return dv->base; }
const char *au_docview_title(const AuDocView *dv) { return dv->title; }
AuDocument *au_docview_document(const AuDocView *dv) { return dv->doc; }
AuLayout *au_docview_layout(const AuDocView *dv) { return dv->layout; }

/* ---- Styles ---- */

static AuCssSheet *build_sheet(AuDocView *dv, SheetSrc *s, int depth)
{
    AuCssSheet *sh = au_css_sheet_parse(s->text, s->len, AU_CSS_ORIGIN_AUTHOR, s->url, s->media);
    if (dv->webfonts)
        au_webfonts_add_sheet(dv->webfonts, sh, s->text, s->len);
    if (depth < 8) {
        size_t n = au_css_sheet_import_count(sh);
        for (size_t i = 0; i < n && i < s->nimports; i++) {
            SheetSrc *c = s->imports[i];
            if (c && c->text)
                au_css_sheet_set_import(sh, i, build_sheet(dv, c, depth + 1));
        }
    }
    return sh;
}

static void rebuild_resolver(AuDocView *dv)
{
    if (dv->resolver)
        au_css_resolver_free(dv->resolver);
    dv->resolver = au_css_resolver_new();
    au_css_resolver_set_metrics(dv->resolver, au_layout_css_metrics, dv->layout);
    if (dv->has_font_prefs)
        au_css_resolver_set_font_prefs(dv->resolver, &dv->font_prefs);
    AuCssDevice dev;
    au_css_device_default(&dev);
    dev.width = (float)dv->vw;
    dev.height = (float)dv->vh;
    dev.scripting = 0;
    au_css_resolver_set_device(dv->resolver, &dev);
    if (dv->user_css)
        au_css_resolver_add_sheet(dv->resolver, au_css_sheet_parse(dv->user_css, strlen(dv->user_css),
                                                                   AU_CSS_ORIGIN_USER, dv->url, NULL));
    for (uint32_t i = 0; i < dv->nslots; i++) {
        SheetSrc *s = dv->slots[i].src;
        if (s->text)
            au_css_resolver_add_sheet(dv->resolver, build_sheet(dv, s, 0));
    }
    au_css_resolver_sheets_changed(dv->resolver);
    dv->style_all = true;
}

bool au_docview_update(AuDocView *dv)
{
    if (!dv->doc)
        return false;
    bool changed = false;
    if (dv->sheets_dirty) {
        rebuild_resolver(dv);
        dv->sheets_dirty = false;
    }
    if (dv->webfonts && au_webfonts_take_changed(dv->webfonts)) {
        /* A web font arrived: lay out again with it (font-display: swap). */
        new_layout(dv, au_webfonts_set(dv->webfonts));
        dv->style_all = true; /* ex and ch units */
    }
    if (dv->style_all || dv->tree_dirty) {
        au_css_style_document(dv->resolver, dv->doc);
        au_layout_tree_changed(dv->layout);
        au_docimages_tree_changed(dv->imgs);
        dv->style_all = dv->tree_dirty = false;
        dv->restyle_needed = false;
        changed = true;
        if (dv->webfonts)
            au_webfonts_scan(dv->webfonts, dv->doc, fetch_font, dv);
    } else if (dv->restyle_needed) {
        au_css_restyle(dv->resolver, dv->doc);
        au_layout_styles_changed(dv->layout);
        dv->restyle_needed = false;
    }
    if (au_docimages_take_layout_changed(dv->imgs))
        dv->images_dirty = true;
    if (dv->images_dirty) {
        au_layout_images_changed(dv->layout);
        dv->images_dirty = false;
    }
    if (au_layout_update(dv->layout, (float)dv->vw, (float)dv->vh))
        changed = true;
    float w, h;
    au_layout_document_size(dv->layout, &w, &h);
    if ((w != dv->last_w || h != dv->last_h)) {
        dv->last_w = w;
        dv->last_h = h;
        if (dv->cb.size_changed)
            dv->cb.size_changed(dv->cb.ctx, dv);
    }
    return changed;
}

void au_docview_set_viewport(AuDocView *dv, int width, int height)
{
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (width == dv->vw && height == dv->vh)
        return;
    dv->vw = width;
    dv->vh = height;
    if (dv->resolver) {
        AuCssDevice dev;
        au_css_device_default(&dev);
        dev.width = (float)width;
        dev.height = (float)height;
        dev.scripting = 0;
        au_css_resolver_set_device(dv->resolver, &dev);
        /* Media queries and viewport units may change. */
        dv->style_all = true;
    }
}

void au_docview_content_size(AuDocView *dv, int *width, int *height)
{
    au_docview_update(dv);
    float w, h;
    au_layout_document_size(dv->layout, &w, &h);
    *width = (int)(w + 0.99f);
    *height = (int)(h + 0.99f);
}

void au_docview_paint(AuDocView *dv, AuCanvas *c, AuRect view, int scroll_x, int scroll_y)
{
    au_docview_update(dv);
    dv->scroll_x = scroll_x;
    dv->scroll_y = scroll_y;
    /* Images near the viewport (not just this paint's rectangle) decode
     * now, at the size they are shown. */
    au_docimages_prepare(dv->imgs, dv->doc, dv->layout, dv->base,
                         (AuRect){(float)scroll_x, (float)scroll_y, (float)dv->vw, (float)dv->vh});
    au_docimages_paint_begin(dv->imgs);
    au_layout_paint(dv->layout, c, view, (float)scroll_x, (float)scroll_y);
    au_docimages_paint_end(dv->imgs);
    images_changed(dv); /* an image found broken while painting */
}

/* ---- Input ---- */

void au_docview_set_scroll(AuDocView *dv, int scroll_x, int scroll_y)
{
    dv->scroll_x = scroll_x;
    dv->scroll_y = scroll_y;
}

bool au_docview_hit_test(AuDocView *dv, int x, int y, AuHitResult *r)
{
    au_docview_update(dv);
    return au_layout_hit_test(dv->layout, (float)(x - dv->scroll_x), (float)(y - dv->scroll_y), (float)dv->scroll_x,
                              (float)dv->scroll_y, r);
}

static void set_state(AuDocView *dv, AuElement **slot, AuElement *el, uint32_t bit)
{
    if (*slot == el)
        return;
    if (*slot) {
        au_element_set_state(*slot, bit, false);
        if (dv->resolver)
            au_css_invalidate(dv->resolver, *slot, AU_CSS_CHANGE_STATE, 0);
        au_node_unref(&(*slot)->node);
    }
    *slot = el;
    if (el) {
        au_node_ref(&el->node);
        au_element_set_state(el, bit, true);
        if (dv->resolver)
            au_css_invalidate(dv->resolver, el, AU_CSS_CHANGE_STATE, 0);
    }
    dv->restyle_needed = true;
}

static void link_url(AuDocView *dv, AuElement *link, char *out, size_t cap)
{
    out[0] = 0;
    const char *href = link ? attr(link, "href") : NULL;
    if (href && !au_url_resolve(dv->base, href, out, cap))
        out[0] = 0;
}

bool au_docview_mouse(AuDocView *dv, const AuEvent *ev)
{
    if (!dv->doc)
        return false;
    AuHitResult r;
    au_docview_hit_test(dv, ev->x, ev->y, &r);
    bool used = false;
    switch (ev->type) {
    case AU_EVENT_MOUSE_MOVE: {
        bool before = dv->restyle_needed;
        set_state(dv, &dv->hover, r.element, AU_STATE_HOVER);
        char url[2048];
        link_url(dv, r.link, url, sizeof url);
        if (strcmp(url, dv->hover_link) != 0) {
            strcpy(dv->hover_link, url);
            if (dv->cb.status_text)
                dv->cb.status_text(dv->cb.ctx, dv, url);
        }
        used = dv->restyle_needed && !before;
        break;
    }
    case AU_EVENT_MOUSE_DOWN:
        set_state(dv, &dv->active, r.element, AU_STATE_ACTIVE);
        used = true;
        break;
    case AU_EVENT_MOUSE_UP: {
        AuElement *down = dv->active;
        AuHitResult dr;
        AuElement *down_link = NULL;
        if (down) {
            memset(&dr, 0, sizeof dr);
            for (AuElement *e = down; e; e = au_is_element(e->node.parent) ? au_as_element(e->node.parent) : NULL)
                if ((au_is_html_element(&e->node, AU_ATOM_a) || au_is_html_element(&e->node, AU_ATOM_area)) &&
                    attr(e, "href")) {
                    down_link = e;
                    break;
                }
        }
        set_state(dv, &dv->active, NULL, AU_STATE_ACTIVE);
        if (r.link && r.link == down_link) {
            char url[2048];
            link_url(dv, r.link, url, sizeof url);
            const char *target = attr(r.link, "target");
            bool new_tab = (ev->mods & AU_MOD_COMMAND) || (target && au_ieq(target, strlen(target), "_blank", 6));
            if (url[0] && dv->cb.open_url)
                dv->cb.open_url(dv->cb.ctx, dv, url, new_tab);
            used = true;
        }
        break;
    }
    default:
        break;
    }
    if (dv->restyle_needed) {
        au_docview_update(dv);
        if (au_layout_needs_paint(dv->layout))
            invalidate_all(dv);
    }
    return used;
}

bool au_docview_scroll_at(AuDocView *dv, int x, int y, int dx, int dy)
{
    au_docview_update(dv);
    bool ok = au_layout_scroll_at(dv->layout, (float)(x - dv->scroll_x), (float)(y - dv->scroll_y),
                                  (float)dv->scroll_x, (float)dv->scroll_y, (float)dx, (float)dy);
    if (ok)
        invalidate_all(dv);
    return ok;
}

const char *au_docview_hover_link(const AuDocView *dv)
{
    return dv->hover_link;
}

/* ---- Fragments ---- */

AuElement *au_docview_find_fragment(AuDocView *dv, const char *frag)
{
    if (!dv->doc || !frag || !*frag)
        return NULL;
    size_t n = strlen(frag);
    AuAtom id = au_atom_lookup(frag, n);
    AuElement *named = NULL;
    for (AuNode *c = au_node_next(&dv->doc->node, &dv->doc->node); c; c = au_node_next(c, &dv->doc->node)) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        AuElement *el = au_as_element(c);
        if (id && el->id == id)
            return el;
        if (!named && au_is_html_element(c, AU_ATOM_a)) {
            const char *nm = attr(el, "name");
            if (nm && strcmp(nm, frag) == 0)
                named = el;
        }
    }
    return named;
}

int au_docview_fragment_y(AuDocView *dv, const char *frag)
{
    if (frag && frag[0] == '#')
        frag++;
    if (frag && (!*frag || strcmp(frag, "top") == 0))
        return 0;
    AuElement *el = au_docview_find_fragment(dv, frag);
    if (!el)
        return -1;
    au_docview_update(dv);
    AuRect r;
    if (!au_layout_element_rect(dv->layout, el, &r)) {
        /* No box (display: contents or none): try the first descendant with one. */
        for (AuNode *c = au_node_next(&el->node, &el->node); c; c = au_node_next(c, &el->node))
            if (au_is_element(c) && au_layout_element_rect(dv->layout, au_as_element(c), &r))
                return (int)r.y;
        return -1;
    }
    return (int)r.y;
}
