/* The document view: parser, style sheets, images, resolver, layout,
 * painting, input. */
#include "docview_int.h"
#include "../net/cors.h"

#include "../paint/canvas2d.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

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

AuDocView *au_dv_top(AuDocView *dv)
{
    while (dv->parent)
        dv = dv->parent;
    return dv;
}

static void invalidate_all(AuDocView *dv)
{
    if (dv->cb.invalidate) {
        float w, h;
        au_layout_document_size(dv->layout, &w, &h);
        dv->cb.invalidate(dv->cb.ctx, dv, (AuIRect){0, 0, au_f2px(w) + 1, au_f2px(h) + 1});
    }
}

static bool layout_image(void *ctx, const char *url, int *w, int *h, const AuSurface **pixels);
static bool layout_resolve(void *ctx, const char *url, char *out, size_t cap);
static Request *add_request(AuDocView *dv, const char *url, AuResourceKind kind);

/* Layout paints an image at a size: an SVG image is drawn at it. */
static void layout_image_use(void *ctx, const char *url, int dw, int dh)
{
    AuDocView *dv = ctx;
    au_docimages_use(dv->imgs, url, dw, dh);
}

static int layout_canvas(void *ctx, AuElement *el, const AuSurface **px);

static int layout_embed_kind(void *ctx, AuElement *el)
{
    return au_frames_embed_kind(ctx, el);
}

static void layout_paint_frame(void *ctx, AuElement *el, AuCanvas *c, AuRect content)
{
    au_frames_paint(ctx, el, c, content);
}

/* A new layout drawing with fonts (the layout caches faces, so a change
 * of font set or a new web font needs a new one). */
static void new_layout(AuDocView *dv, AuFontSet *fonts)
{
    if (dv->layout) {
        uint32_t b, l;
        au_layout_pass_counts(dv->layout, &b, &l);
        dv->dead_builds += b;
        dv->dead_layouts += l;
        au_layout_free(dv->layout);
    }
    AuLayoutEnv env = {fonts, dv, layout_image, layout_resolve, layout_image_use, layout_embed_kind,
                       layout_paint_frame, layout_canvas};
    dv->layout = au_layout_new(&env);
    /* Canvas text is drawn with the same fonts; font lists made from the
     * old set go before the set does. */
    dv->cur_fonts = fonts;
    if (dv->canvases)
        au_canvas_set_fonts(dv->canvases, fonts);
    if (dv->parent)
        au_layout_set_transparent(dv->layout, true); /* a nested document shows its parent through */
    if (dv->doc)
        au_layout_set_document(dv->layout, dv->doc);
    if (dv->resolver)
        au_css_resolver_set_metrics(dv->resolver, au_layout_css_metrics, dv->layout);
}

static uint32_t fetch_font(void *ctx, const char *url)
{
    AuDocView *dv = ctx;
    Request *r = add_request(dv, url, AU_RES_FONT);
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
    au_docimages_set_fonts(dv->imgs, fonts); /* text in SVG images */
    new_layout(dv, fonts);
    dv->url = dup("about:blank");
    dv->base = dup("about:blank");
    dv->title = dup("");
    dv->vw = 800;
    dv->vh = 600;
    dv->goal_x = -1;
    return dv;
}

void au_docview_set_request_ex(AuDocView *dv, AuLoaderRequestEx fn)
{
    dv->request_ex = fn;
}

/* Frees a sheet source and its imports (at most SHEET_IMPORT_DEPTH deep).
 * The resolver must not hold the parsed sheet any more
 * (au_css_resolver_detach_sheets). */
#define SHEET_IMPORT_DEPTH 8
static void free_sheet_src(AuDocView *dv, SheetSrc *s)
{
    if (!s)
        return;
    for (uint32_t i = 0; i < s->nimports; i++)
        free_sheet_src(dv, s->imports[i]);
    if (s->req)
        for (uint32_t j = 0; j < dv->nreqs; j++)
            if (dv->reqs[j].sheet == s)
                dv->reqs[j].sheet = NULL; /* its data is dropped on arrival */
    if (s->deferred && dv->ndeferred)
        dv->ndeferred--;
    if (!s->parent)
        au_css_sheet_free(s->sheet); /* frees the attached imports' sheets */
    au_free(s->imports);
    au_free(s->url);
    au_free(s->media);
    au_free(s->text);
    if (s->om_sheet)
        au_css_sheet_free(s->om_sheet);
    au_free(s);
}

/* Stops request id of dv. A nested view's requests were made through
 * the top view's loader. */
static void loader_cancel(AuDocView *dv, uint32_t id)
{
    AuDocView *top = au_dv_top(dv);
    if (top->loader.cancel)
        top->loader.cancel(top->loader.ctx, id);
    for (uint32_t i = 0; i < top->nroutes; i++)
        if (top->routes[i].id == id) {
            top->routes[i] = top->routes[--top->nroutes];
            break;
        }
}

void au_dv_drop_routes(AuDocView *top, AuDocView *dv)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < top->nroutes; i++)
        if (top->routes[i].dv != dv)
            top->routes[k++] = top->routes[i];
    top->nroutes = k;
}

static void clear_document(AuDocView *dv)
{
    au_frames_clear(dv); /* nested documents first: their script contexts go before this one's */
    au_forms_clear(dv);
    if (dv->script) {
        AuDocScript *s = dv->script;
        dv->script = NULL;
        au_docscript_free(s);
    }
    dv->nbg = 0;
    for (uint32_t i = 0; i < dv->nreqs; i++) {
        loader_cancel(dv, dv->reqs[i].id);
        au_buf_free(&dv->reqs[i].data);
        au_free(dv->reqs[i].img_url);
    }
    dv->nreqs = 0;
    for (uint32_t i = 0; i < dv->ntainted; i++)
        au_free(dv->tainted_imgs[i]);
    au_free(dv->tainted_imgs);
    dv->tainted_imgs = NULL;
    dv->ntainted = 0;
    au_layout_set_document(dv->layout, NULL);
    if (dv->resolver)
        au_css_resolver_detach_sheets(dv->resolver); /* the sources own the sheets */
    for (uint32_t i = 0; i < dv->nslots; i++) {
        au_node_unref(&dv->slots[i].el->node);
        free_sheet_src(dv, dv->slots[i].src);
    }
    dv->nslots = 0;
    dv->ndeferred = 0;
    au_css_sheet_free(dv->user_sheet);
    dv->user_sheet = NULL;
    au_docimages_clear(dv->imgs);
    au_layout_scroll_free(dv->slim_scrolls, dv->nslim_scrolls);
    dv->slim_scrolls = NULL;
    dv->nslim_scrolls = 0;
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
    /* The document's canvases: script objects that outlive the set stay
     * valid and draw nothing. Before the web fonts go, for the font lists
     * the set holds. */
    au_canvas_set_free(dv->canvases);
    dv->canvases = NULL;
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
    if (dv->parent)
        au_dv_drop_routes(au_dv_top(dv), dv);
    au_free(dv->frames);
    au_free(dv->routes);
    au_free(dv->dead);
    au_layout_free(dv->layout);
    au_docimages_free(dv->imgs);
    au_free(dv->slots);
    au_free(dv->reqs);
    au_free(dv->url);
    au_free(dv->base);
    au_free(dv->title);
    au_free(dv->user_css);
    au_free(dv->user_agent);
    au_free(dv->referrer);
    au_free(dv);
}

void au_docview_set_user_css(AuDocView *dv, const char *css)
{
    if ((!css && !dv->user_css) || (css && dv->user_css && strcmp(css, dv->user_css) == 0))
        return;
    au_free(dv->user_css);
    dv->user_css = css ? dup(css) : NULL;
    if (dv->resolver)
        au_css_resolver_detach_sheets(dv->resolver);
    au_css_sheet_free(dv->user_sheet);
    dv->user_sheet = NULL;
    dv->sheets_dirty = true;
    au_frames_settings_changed(dv);
}

/* ---- Images (the cache and its bookkeeping are in docimages.c) ---- */

static void invalidate_fn(void *ctx, AuIRect r)
{
    AuDocView *dv = ctx;
    if (dv->cb.invalidate)
        dv->cb.invalidate(dv->cb.ctx, dv, r);
}

/* Notes images whose size became known (au_docview_update tells layout,
 * and repaints the page if geometry changes) and repaints the boxes of
 * images that changed. */
static void images_changed(AuDocView *dv)
{
    if (au_docimages_take_layout_changed(dv->imgs))
        dv->images_dirty = true;
    float w, h;
    au_layout_document_size(dv->layout, &w, &h);
    au_docimages_invalidate(dv->imgs, dv->layout, (AuIRect){0, 0, au_f2px(w) + 1, au_f2px(h) + 1}, invalidate_fn, dv);
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
    uint32_t fn = au_frames_animate(dv, now_ms);
    return fn < next ? fn : next;
}

bool au_docview_animating(const AuDocView *dv)
{
    return au_docimages_animating(dv->imgs) || au_frames_animating(dv);
}

void au_docview_stats(const AuDocView *dv, AuDocViewStats *s)
{
    uint32_t b = 0, l = 0;
    *s = dv->stats;
    if (dv->layout)
        au_layout_pass_counts(dv->layout, &b, &l);
    s->tree_builds = dv->dead_builds + b;
    s->layouts = dv->dead_layouts + l;
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
    au_frames_settings_changed(dv);
}

/* ---- Requests ---- */

/* Asks the loader. Every request of the page goes through the top view's
 * loader, with the top view as its dv; a nested view's id is noted in the
 * top view's routes so the data finds its way (au_docview_resource_*). */
static uint32_t loader_start(AuDocView *dv, const AuResourceRequest *rq)
{
    AuDocView *top = au_dv_top(dv);
    uint32_t id = 0;
    if (top->request_ex)
        id = top->request_ex(top->loader.ctx, top, rq);
    else if (top->loader.request)
        id = top->loader.request(top->loader.ctx, top, rq->url, rq->kind);
    if (id && top != dv) {
        if (top->nroutes == top->routes_cap) {
            top->routes_cap = top->routes_cap ? top->routes_cap * 2 : 16;
            top->routes = au_realloc(top->routes, top->routes_cap * sizeof *top->routes);
        }
        top->routes[top->nroutes].id = id;
        top->routes[top->nroutes].dv = dv;
        top->nroutes++;
    }
    return id;
}

static bool is_network_url(const char *url);

static Request *add_request_ex(AuDocView *dv, const AuResourceRequest *rq)
{
    uint32_t id = loader_start(dv, rq);
    if (!id)
        return NULL;
    if (dv->nreqs == dv->reqs_cap) {
        dv->reqs_cap = dv->reqs_cap ? dv->reqs_cap * 2 : 16;
        dv->reqs = au_realloc(dv->reqs, dv->reqs_cap * sizeof *dv->reqs);
    }
    Request *r = &dv->reqs[dv->nreqs++];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->kind = rq->kind;
    r->script = rq->script_mode != 0 && !rq->cors_resource;
    r->cors = rq->cors_resource;
    r->cross = rq->kind != AU_RES_FRAME && is_network_url(rq->url) && !au_cors_same_origin(dv->url, rq->url);
    au_buf_init(&r->data);
    return r;
}

/* ---- Resources of other origins ----
 * A resource of the page (a style sheet, an image, a script element's
 * source, a font) is fetched without CORS unless its element has a
 * crossorigin attribute. Three rules apply when its response head
 * arrives (gate_head):
 * - with a crossorigin attribute and another origin, the response must
 *   pass the CORS check, else the load fails;
 * - without one, a response of another origin with
 *   Cross-Origin-Resource-Policy same-origin or same-site (and another
 *   site) is refused;
 * - an image of another origin that was not fetched with CORS is
 *   recorded as tainted, for au_docview_image_origin_clean.
 * Redirects of such a resource are followed by the network layer; the
 * check is made against the final response. Script's own requests (fetch,
 * XMLHttpRequest, workers) are checked hop by hop in src/js/fetch.c. */
static bool is_network_url(const char *url)
{
    return url && (!strncmp(url, "http:", 5) || !strncmp(url, "https:", 6));
}

/* The crossorigin attribute as a CORS mode: 0 none, 1 anonymous, 2
 * use-credentials. */
uint8_t au_dv_cors_attr(const AuElement *el)
{
    const char *v = el ? attr(el, "crossorigin") : NULL;
    if (!v)
        return 0;
    return au_ieq(v, strlen(v), "use-credentials", 15) ? 2 : 1;
}

/* Requests url for an element's resource with the element's CORS mode
 * (0: a plain request). A cross-origin CORS request carries Origin and,
 * in anonymous mode, no credentials. */
static Request *add_request_cors(AuDocView *dv, const char *url, AuResourceKind kind, uint8_t cors)
{
    if (!cors || !is_network_url(url) || !is_network_url(dv->url) || au_cors_same_origin(dv->url, url))
        return add_request(dv, url, kind);
    AuBuf origin;
    au_buf_init(&origin);
    au_cors_origin_of(dv->url, &origin);
    const char *hv[2] = {"Origin", au_buf_cstr(&origin)};
    AuResourceRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.kind = kind;
    rq.document_url = dv->url;
    rq.headers = hv;
    rq.nheaders = 1;
    rq.script_mode = 1;
    rq.cors_resource = cors;
    rq.omit_credentials = cors == 1;
    Request *r = add_request_ex(dv, &rq);
    au_buf_free(&origin);
    return r;
}

uint32_t au_dv_request_cors(AuDocView *dv, const char *url, uint8_t cors)
{
    Request *r = add_request_cors(dv, url, AU_RES_OTHER, cors);
    if (!r)
        return 0;
    dv->finished_reported = false;
    return r->id;
}

static void taint_image(AuDocView *dv, const char *url, bool tainted)
{
    for (uint32_t i = 0; i < dv->ntainted; i++)
        if (!strcmp(dv->tainted_imgs[i], url)) {
            if (!tainted) {
                au_free(dv->tainted_imgs[i]);
                dv->tainted_imgs[i] = dv->tainted_imgs[--dv->ntainted];
            }
            return;
        }
    if (!tainted)
        return;
    dv->tainted_imgs = au_realloc(dv->tainted_imgs, (dv->ntainted + 1) * sizeof *dv->tainted_imgs);
    dv->tainted_imgs[dv->ntainted++] = dup(url);
}

bool au_docview_image_origin_clean(AuDocView *dv, const char *url)
{
    if (!url)
        return false;
    for (uint32_t i = 0; i < dv->ntainted; i++)
        if (!strcmp(dv->tainted_imgs[i], url))
            return false;
    /* An image that was never requested here (or is still loading) is
     * clean only when its URL is of the document's origin or carries its
     * own data. */
    for (uint32_t i = 0; i < dv->nreqs; i++)
        if (dv->reqs[i].img_url && !strcmp(dv->reqs[i].img_url, url))
            return !dv->reqs[i].cross;
    return !is_network_url(url) || au_cors_same_origin(dv->url, url) || au_docimages_known(dv->imgs, url);
}

static void gate_console(AuDocView *dv, const char *what, const char *url)
{
    AuDocView *top = au_dv_top(dv);
    if (!top->cb.console)
        return;
    AuBuf m;
    au_buf_init(&m);
    au_buf_printf(&m, "%s: %s", what, url ? url : "");
    top->cb.console(top->cb.ctx, top, 2, au_buf_cstr(&m), m.len);
    au_buf_free(&m);
}

/* The response head of a page resource: false when the response may not
 * be used (the caller fails the request). */
static bool gate_head(AuDocView *dv, Request *r, const char *final_url, const char *const *headers, size_t n)
{
    if (r->script || r->kind == AU_RES_FRAME)
        return true;
    bool cross = r->cross;
    if (final_url && is_network_url(final_url))
        cross = !au_cors_same_origin(dv->url, final_url);
    bool ok = true, clean = !cross;
    if (cross && r->cors) {
        AuBuf origin;
        au_buf_init(&origin);
        au_cors_origin_of(dv->url, &origin);
        ok = clean = au_cors_check(au_buf_cstr(&origin), r->cors == 2, headers, n);
        au_buf_free(&origin);
        if (!ok)
            gate_console(dv, "Blocked by CORS (the response has no matching Access-Control-Allow-Origin)", final_url);
    } else if (cross && final_url) {
        AuBuf origin;
        au_buf_init(&origin);
        au_cors_origin_of(dv->url, &origin);
        ok = au_corp_allows(au_cors_header(headers, n, "cross-origin-resource-policy"), au_buf_cstr(&origin),
                            final_url);
        au_buf_free(&origin);
        if (!ok)
            gate_console(dv, "Blocked by Cross-Origin-Resource-Policy", final_url);
    }
    if (r->img_url)
        taint_image(dv, r->img_url, !clean);
    r->cross = cross;
    return ok;
}

uint32_t au_dv_start_request(AuDocView *dv, const AuResourceRequest *rq, AuFrame *f)
{
    Request *r = add_request_ex(dv, rq);
    if (!r)
        return 0;
    r->frame = f;
    dv->finished_reported = false;
    return r->id;
}

static Request *add_request(AuDocView *dv, const char *url, AuResourceKind kind)
{
    AuResourceRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.kind = kind;
    rq.document_url = dv->url;
    return add_request_ex(dv, &rq);
}


/* The nested view a request of the top view was made for, or NULL. */
static AuDocView *route_of(AuDocView *dv, uint32_t id, bool remove)
{
    for (uint32_t i = 0; i < dv->nroutes; i++)
        if (dv->routes[i].id == id) {
            AuDocView *t = dv->routes[i].dv;
            if (remove)
                dv->routes[i] = dv->routes[--dv->nroutes];
            return t;
        }
    return NULL;
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
    Request *r = add_request_cors(dv, s->url, AU_RES_STYLESHEET, s->cors);
    if (!r) {
        s->failed = true;
        return;
    }
    r->sheet = s;
    s->req = r->id;
}

/* The device media queries are evaluated against. */
static void view_device(const AuDocView *dv, AuCssDevice *dev)
{
    au_css_device_default(dev);
    dev->width = (float)dv->vw;
    dev->height = (float)dv->vh;
    dev->scripting = dv->script ? 2 : 0;
}

static bool media_matches(const AuDocView *dv, const SheetSrc *s)
{
    if (!s->media)
        return true;
    AuCssDevice dev;
    view_device(dv, &dev);
    return au_css_media_text_matches(s->media, strlen(s->media), &dev);
}

/* Fetches a sheet source, or defers it while its media query does not
 * match (a sheet for print, for a dark scheme or for another width is
 * neither fetched nor parsed until the device changes so that it
 * matches; see undefer_sheets). */
static void start_sheet(AuDocView *dv, SheetSrc *s)
{
    if (!media_matches(dv, s)) {
        s->deferred = true;
        dv->ndeferred++;
        return;
    }
    request_sheet(dv, s);
}

/* The text a source was parsed from: its own while it waits to be
 * parsed, then the parsed sheet's (which keeps it, packed once the
 * document has loaded). src_text_get appends it to out; src_text_is
 * compares it. */
static bool src_text_get(const SheetSrc *s, AuBuf *out)
{
    if (s->text) {
        au_buf_append(out, s->text, s->len);
        return true;
    }
    return s->sheet && au_css_sheet_text_get(s->sheet, out);
}

static bool src_text_is(const SheetSrc *s, const char *text, size_t len)
{
    if (s->text)
        return s->len == len && (!len || memcmp(s->text, text, len) == 0);
    return s->sheet && au_css_sheet_text_is(s->sheet, text, len);
}

/* A sheet of this document that was parsed from the same text for the
 * same URL and media, or NULL. A page may link one sheet several times
 * (GitHub's home page links one of 800 KB four times); they share one
 * parsed sheet. Sheets with @import rules are left out: an import is
 * attached to its parent's sheet. */
static AuCssSheet *same_sheet(AuDocView *dv, const SheetSrc *s)
{
    if (s->parent)
        return NULL;
    for (uint32_t i = 0; i < dv->nslots; i++) {
        const SheetSrc *o = dv->slots[i].src;
        if (o == s || !o->sheet || o->nimports || o->len != s->len)
            continue;
        if (au_css_sheet_import_count(o->sheet) || strcmp(o->url ? o->url : "", s->url ? s->url : "") ||
            strcmp(o->media ? o->media : "", s->media ? s->media : ""))
            continue;
        if (au_css_sheet_text_is(o->sheet, s->text, s->len))
            return o->sheet;
    }
    return NULL;
}

/* Parses a source whose text is there, once; attaches an import to its
 * parent's sheet; and starts the sheet's own @imports. The parsed sheet
 * takes the text (it parses blocks of declarations from it when their
 * rules first match, and the CSSOM reads it: src_text_get). */
static void parse_sheet(AuDocView *dv, SheetSrc *s)
{
    if (s->sheet || !s->text)
        return;
    AuCssSheet *sh = same_sheet(dv, s);
    if (sh) {
        au_css_sheet_retain(sh);
        au_free(s->text);
    } else {
        /* the buffer grew by doubling: the sheet keeps only the text */
        sh = au_css_sheet_parse_take(au_realloc(s->text, s->len + 1), s->len, AU_CSS_ORIGIN_AUTHOR, s->url, s->media);
    }
    s->text = NULL; /* len stays: the size the sheet has (au_docview_memory_estimate) */
    if (dv->webfonts && au_css_sheet_font_face_count(sh)) {
        AuBuf t;
        au_buf_init(&t);
        au_css_sheet_text_get(sh, &t);
        au_webfonts_add_sheet(dv->webfonts, sh, t.data, t.len);
        au_buf_free(&t);
    }
    dv->sheets_packed = false;
    dv->sheets_dirty = true;
    if (s->parent) {
        if (!s->parent->sheet) {
            au_css_sheet_free(sh);
            return;
        }
        au_css_sheet_set_import(s->parent->sheet, s->index, sh);
    }
    s->sheet = sh;
    size_t n = au_css_sheet_import_count(sh);
    if (n > 64)
        n = 64;
    if (s->depth + 1 >= SHEET_IMPORT_DEPTH)
        n = 0;
    s->imports = n ? au_calloc(n * sizeof *s->imports) : NULL;
    s->nimports = (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        const AuCssImport *im = au_css_sheet_import(sh, i);
        if (!im->supported)
            continue;
        char *abs = au_url_resolve_dup(s->url, im->url);
        if (!abs)
            continue;
        SheetSrc *c = au_calloc(sizeof *c);
        c->url = abs;
        c->media = im->media && *im->media ? dup(im->media) : NULL;
        c->parent = s;
        c->index = (uint32_t)i;
        c->depth = s->depth + 1;
        s->imports[i] = c;
        start_sheet(dv, c);
    }
}

/* Starts the deferred sources whose media query matches now. */
static void undefer_sheet(AuDocView *dv, SheetSrc *s)
{
    if (!s)
        return;
    if (s->deferred && media_matches(dv, s)) {
        s->deferred = false;
        dv->ndeferred--;
        if (s->text) {
            parse_sheet(dv, s); /* a <style> */
        } else {
            request_sheet(dv, s);
            dv->finished_reported = false;
        }
    }
    for (uint32_t i = 0; i < s->nimports; i++)
        undefer_sheet(dv, s->imports[i]);
}

static void undefer_sheets(AuDocView *dv)
{
    for (uint32_t i = 0; i < dv->nslots && dv->ndeferred; i++)
        undefer_sheet(dv, dv->slots[i].src);
}

void au_docview_resource_head(AuDocView *dv, uint32_t id, int status, const char *status_text,
                              const char *final_url, const char *const *headers, size_t nheaders)
{
    Request *r = find_request(dv, id);
    if (!r) {
        AuDocView *t = route_of(dv, id, false);
        if (t)
            au_docview_resource_head(t, id, status, status_text, final_url, headers, nheaders);
        return;
    }
    if (r->kind == AU_RES_FRAME) {
        if (r->frame)
            au_frames_head(dv, r->frame, status, final_url, headers, nheaders);
        return;
    }
    if (!gate_head(dv, r, final_url, headers, nheaders)) {
        /* Not to be used: the transfer stops and the load fails. */
        loader_cancel(dv, id);
        au_docview_resource_done(dv, id, false);
        return;
    }
    if (r->kind == AU_RES_OTHER && !r->font && dv->script)
        au_docscript_resource_head(dv->script, id, status, status_text, final_url, headers, nheaders,
                                   r->cross && !r->cors);
}

void au_docview_resource_data(AuDocView *dv, uint32_t id, const void *data, size_t len)
{
    Request *r = find_request(dv, id);
    if (!r) {
        AuDocView *t = route_of(dv, id, false);
        if (t)
            au_docview_resource_data(t, id, data, len);
        return;
    }
    if (r->kind == AU_RES_FRAME) {
        if (r->frame)
            au_frames_data(dv, r->frame, data, len);
        return;
    }
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
        if (dv->script) {
            /* The load event may start more loads (images, scripts). */
            au_docscript_loaded(dv->script);
            if (au_docview_loading(dv)) {
                dv->finished_reported = false;
                return;
            }
        }
        if (dv->cb.load_finished)
            dv->cb.load_finished(dv->cb.ctx, dv);
    }
}

void au_docview_resource_done(AuDocView *dv, uint32_t id, bool ok)
{
    Request *r = find_request(dv, id);
    if (!r) {
        AuDocView *t = route_of(dv, id, true);
        if (t)
            au_docview_resource_done(t, id, ok);
        au_frames_idle(dv);
        return;
    }
    Request req = *r;
    *r = dv->reqs[--dv->nreqs];
    if (req.background)
        dv->nbg--;
    if (req.img_url) {
        /* a loader that reports no head: the URL decides */
        if (req.cross && !req.cors)
            taint_image(dv, req.img_url, true);
        au_free(req.img_url);
    }
    if (req.kind == AU_RES_FRAME) {
        if (req.frame)
            au_frames_done(dv, req.frame, ok);
        au_buf_free(&req.data);
        maybe_finished(dv);
        au_frames_idle(dv);
        return;
    }
    if (req.kind == AU_RES_OTHER && !req.font) {
        if (dv->script)
            au_docscript_resource_done(dv->script, req.id, &req.data, ok);
        au_buf_free(&req.data);
        maybe_finished(dv);
        return;
    }
    if (req.kind == AU_RES_STYLESHEET && req.sheet) {
        SheetSrc *s = req.sheet;
        s->req = 0;
        if (ok) {
            size_t n;
            s->text = au_buf_take(&req.data, &n);
            s->len = n;
            if (!s->text) /* an empty response */
                s->text = dup("");
            parse_sheet(dv, s);
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
    /* images_changed has invalidated where an image shows. */
    if (!(req.kind == AU_RES_IMAGE && req.image))
        invalidate_all(dv);
    maybe_finished(dv);
}

uint32_t au_docview_pending(const AuDocView *dv)
{
    return dv->nreqs;
}

bool au_docview_loading(const AuDocView *dv)
{
    return dv->parsing || dv->nreqs > dv->nbg || (dv->script && au_docscript_busy(dv->script)) ||
           au_frames_loading(dv);
}

/* ---- Keeping a document while it is not shown ---- */

bool au_docview_can_persist(AuDocView *dv)
{
    if (!dv->doc || au_docview_loading(dv) || dv->nreqs)
        return false;
    return au_docscript_can_persist(dv->script) && au_frames_can_persist(dv);
}

bool au_docview_has_state(AuDocView *dv)
{
    if (!dv->doc)
        return false;
    /* A canvas with a bitmap: what script or the user drew cannot be
     * told apart, and neither comes back with a reload. */
    if (dv->canvases && au_canvas_set_bytes(dv->canvases) > 0)
        return true;
    if (au_docscript_workers(dv->script) > 0)
        return true;
    /* A form control the user changed or gave files. */
    for (AuNode *c = au_node_next(&dv->doc->node, &dv->doc->node); c; c = au_node_next(c, &dv->doc->node)) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        const AuControl *ctl = au_ctl_peek((const AuElement *)c);
        if (ctl && (ctl->user_interacted || ctl->nfiles))
            return true;
    }
    return au_frames_has_state(dv);
}

void au_docview_set_hidden(AuDocView *dv, bool hidden)
{
    au_frames_set_hidden(dv, hidden); /* its frames pause and resume with it */
    au_docscript_page_transition(dv->script, !hidden);
    /* A hidden document gives up its decoded image pixels (the compressed
     * bytes stay, and images decode again when they are next painted):
     * they are most of a kept document's memory. A new visibility frame
     * marks every image unused, and a budget of one byte evicts them. */
    AuImageCache *c = au_docimages_cache(dv->imgs);
    if (hidden) {
        AuImageCacheStats st;
        au_image_cache_stats(c, &st);
        dv->hidden_image_budget = st.budget;
        au_image_cache_frame_begin(c);
        au_image_cache_set_budget(c, 1);
        /* The layout goes too: boxes, lines, fragments, stacking data,
         * measured words and font lists are built again from the tree and
         * its styles by the first update after the view is shown. Inner
         * scroll offsets live in the boxes, so they are saved. */
        if (!dv->slim_scrolls)
            dv->slim_scrolls = au_layout_scroll_save(dv->layout, &dv->nslim_scrolls);
        au_layout_release(dv->layout);
        /* Its style sheet text is packed now rather than by the idle
         * pass, which a hidden view no longer gets: a document left within
         * a few seconds of loading would otherwise keep the raw text (about
         * 2 MB for a site with large sheets). One sheet a call, bounded. */
        for (int n = 0; n < 64 && au_docview_pack_sheets(dv); n++) {
        }
    } else if (dv->hidden_image_budget) {
        au_image_cache_set_budget(c, dv->hidden_image_budget);
        dv->hidden_image_budget = 0;
    }
}

void au_docview_storage_event(AuDocView *dv, const AuDocStorageEvent *ev)
{
    au_frames_storage_event(dv, ev); /* frames nest at most AU_FRAMES_DEPTH deep */
    if (dv != ev->writer)
        au_docscript_storage_event(dv->script, ev);
}

/* Packs the text of the first sheet of a source (or of its imports) that
 * is not packed yet; true if it packed one. */
static bool pack_src(SheetSrc *s)
{
    if (!s)
        return false;
    if (s->sheet && au_css_sheet_pack_text(s->sheet))
        return true;
    if (s->om_sheet && au_css_sheet_pack_text(s->om_sheet))
        return true;
    for (uint32_t i = 0; i < s->nimports; i++)
        if (pack_src(s->imports[i]))
            return true;
    return false;
}

bool au_docview_pack_sheets(AuDocView *dv)
{
    if (dv->sheets_packed)
        return false;
    for (uint32_t i = 0; i < dv->nslots; i++)
        if (pack_src(dv->slots[i].src))
            return true; /* one sheet a call: a large one takes a moment */
    if (dv->user_sheet && au_css_sheet_pack_text(dv->user_sheet))
        return true;
    dv->sheets_packed = true;
    return false;
}

static void sheet_bytes(const SheetSrc *s, size_t *n)
{
    /* @import nesting is shallow and capped by the loader; one level of
     * recursion per import depth. A parsed sheet holds its text and its
     * rules; a source still waiting holds its text. */
    *n += s->sheet ? au_css_sheet_memory(s->sheet) : s->text ? s->len : 0;
    for (uint32_t i = 0; i < s->nimports; i++)
        if (s->imports[i])
            sheet_bytes(s->imports[i], n);
}

size_t au_docview_memory_estimate(AuDocView *dv)
{
    size_t n = sizeof *dv;
    if (dv->doc) {
        /* Per element: the node, its attributes and its computed style;
         * per text node: the node and its characters. */
        for (AuNode *c = au_node_next(&dv->doc->node, &dv->doc->node); c; c = au_node_next(c, &dv->doc->node)) {
            if (c->type == AU_NODE_ELEMENT)
                n += 480;
            else if (c->type == AU_NODE_TEXT)
                n += 64 + ((const AuText *)c)->len;
            else
                n += 64;
        }
    }
    size_t css = 0;
    for (uint32_t i = 0; i < dv->nslots; i++) {
        if (!dv->slots[i].src)
            continue;
        /* sources that share a parsed sheet count it once */
        bool counted = false;
        for (uint32_t k = 0; k < i && !counted; k++)
            counted = dv->slots[k].src && dv->slots[k].src->sheet &&
                      dv->slots[k].src->sheet == dv->slots[i].src->sheet;
        if (!counted)
            sheet_bytes(dv->slots[i].src, &css);
    }
    n += css;
    AuLayoutStats ls;
    au_layout_stats(dv->layout, &ls);
    n += ls.tree_bytes + ls.frag_bytes;
    AuImageCacheStats is;
    au_image_cache_stats(au_docimages_cache(dv->imgs), &is);
    n += is.decoded_bytes + is.decoder_bytes + is.compressed_bytes;
    n += au_docscript_memory(dv->script);
    n += au_frames_memory(dv);
    n += au_canvas_set_bytes(dv->canvases); /* canvas bitmaps, ImageBitmaps and clip masks */
    return n;
}

/* ---- Layout callbacks ---- */

static bool layout_resolve(void *ctx, const char *url, char *out, size_t cap)
{
    AuDocView *dv = ctx;
    return au_url_resolve(dv->base, url, out, cap);
}

/* The CORS mode an image is fetched with: that of the first <img> with a
 * crossorigin attribute whose src is this URL (0 when there is none). The
 * scan runs only in documents where some element has the attribute. */
static uint8_t image_cors_mode(AuDocView *dv, const char *url)
{
    AuAtom co = au_atom_lookup("crossorigin", 11);
    if (!co || !dv->doc || !is_network_url(url))
        return 0;
    for (AuNode *n = au_node_next(&dv->doc->node, &dv->doc->node); n; n = au_node_next(n, &dv->doc->node)) {
        if (!au_is_html_element(n, AU_ATOM_img))
            continue;
        AuElement *el = au_as_element(n);
        if (!au_element_get_attr(el, co))
            continue;
        char abs[2048];
        const char *src = attr(el, "src");
        if (src && au_url_resolve(dv->base, src, abs, sizeof abs) && !strcmp(abs, url))
            return au_dv_cors_attr(el);
    }
    return 0;
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
        Request *r = add_request_cors(dv, url, AU_RES_IMAGE, image_cors_mode(dv, url));
        if (r) {
            r->image = e;
            r->img_url = dup(url);
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
        if (!s->src->text)
            s->src->text = dup("");
        s->src->url = dup(dv->base);
        if (media_matches(dv, s->src)) {
            parse_sheet(dv, s->src);
        } else {
            s->src->deferred = true;
            dv->ndeferred++;
        }
    } else {
        const char *href = attr(el, "href");
        char *abs = href ? au_url_resolve_dup(dv->base, href) : NULL;
        if (abs) {
            s->src->url = abs;
            s->src->cors = au_dv_cors_attr(el);
            start_sheet(dv, s->src);
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
        if (el->tag == AU_ATOM_style || (el->tag == AU_ATOM_link && is_stylesheet_link(el) && !attr(el, "disabled"))) {
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
    au_perf_reset(&dv->perf);
    dv->webfonts = au_webfonts_new(dv->fonts);
    new_layout(dv, au_webfonts_set(dv->webfonts));
    au_free(dv->url);
    au_free(dv->base);
    dv->url = dup(url);
    /* A srcdoc or about:blank document in a frame resolves its URLs
     * against its parent's base URL (HTML: the fallback base URL). */
    dv->base = dup(dv->parent && (!strcmp(url, "about:srcdoc") || !strcmp(url, "about:blank")) ? dv->parent->base : url);
    if (dv->title[0]) {
        au_free(dv->title);
        dv->title = dup("");
        if (dv->cb.title_changed)
            dv->cb.title_changed(dv->cb.ctx, dv, "");
    }
    AuHtmlOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.scripting = dv->scripting;
    opts.encoding = AU_HTML_ENC_SNIFF;
    if (dv->scripting) {
        /* The parser keeps opts.ctx; the script object follows it. */
        opts.script_ready = au_docscript_script_ready;
        opts.ctx = dv->script = au_docscript_new(dv);
    }
    dv->parser = au_html_parser_new(&opts);
    dv->doc = au_html_parser_document(dv->parser);
    au_node_ref(&dv->doc->node);
    if (dv->script)
        au_docscript_set_parser(dv->script, dv->parser, dv->doc);
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
    dv->frames_dirty = true;
    au_frames_sync(dv);
}

void au_docview_finish(AuDocView *dv)
{
    if (!dv->parser)
        au_docview_begin(dv, dv->url);
    au_html_parser_finish(dv->parser);
    if (dv->script && !au_html_parser_done(dv->parser)) {
        /* Paused on a script: the end of parsing comes with the resume. */
        dv->tree_dirty = true;
        scan_document(dv, false);
        return;
    }
    dv->parsing = false;
    dv->tree_dirty = true;
    scan_document(dv, true);
    dv->frames_dirty = true;
    au_frames_sync(dv);
    invalidate_all(dv);
    au_forms_autofocus(dv);
    if (dv->script)
        au_docscript_parse_done(dv->script);
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

/* Gives the resolver the sheets as they are now: the user sheet, then the
 * parsed sources in document order. Sheets are parsed once, as they
 * arrive (parse_sheet); the resolver borrows them and only builds its
 * index again. */
static void rebuild_resolver(AuDocView *dv)
{
    if (!dv->resolver) {
        dv->resolver = au_css_resolver_new();
        au_css_resolver_set_metrics(dv->resolver, au_layout_css_metrics, dv->layout);
        if (dv->has_font_prefs)
            au_css_resolver_set_font_prefs(dv->resolver, &dv->font_prefs);
    } else {
        au_css_resolver_detach_sheets(dv->resolver);
    }
    AuCssDevice dev;
    view_device(dv, &dev);
    au_css_resolver_set_device(dv->resolver, &dev);
    undefer_sheets(dv); /* the device may have changed (scripting) */
    if (dv->user_css) {
        if (!dv->user_sheet)
            dv->user_sheet = au_css_sheet_parse(dv->user_css, strlen(dv->user_css), AU_CSS_ORIGIN_USER, dv->url, NULL);
        au_css_resolver_add_sheet(dv->resolver, dv->user_sheet);
    }
    for (uint32_t i = 0; i < dv->nslots; i++) {
        SheetSrc *s = dv->slots[i].src;
        /* CSSOM: a disabled sheet is left out; a sheet whose rules script
         * changed is the one parsed from the text script wrote back */
        AuCssSheet *sh = s->om_sheet ? s->om_sheet : s->sheet;
        if (sh && !s->disabled)
            au_css_resolver_add_sheet(dv->resolver, sh);
    }
    au_css_resolver_sheets_changed(dv->resolver);
    dv->style_all = true;
}

static bool same_str(const char *a, const char *b)
{
    return !strcmp(a ? a : "", b ? b : "");
}

/* A slot whose element no longer matches it: a <style> whose text or
 * media changed, a <link> whose href, rel, media or disabled changed. */
static bool slot_stale(AuDocView *dv, const SheetSlot *s)
{
    const char *media = attr(s->el, "media");
    if (!same_str(media && *media ? media : NULL, s->src->media))
        return true;
    if (s->el->tag == AU_ATOM_style) {
        AuBuf t;
        au_buf_init(&t);
        text_content(&s->el->node, &t);
        bool changed = !src_text_is(s->src, t.data ? t.data : "", t.len);
        au_buf_free(&t);
        return changed;
    }
    if (!is_stylesheet_link(s->el) || attr(s->el, "disabled"))
        return true;
    const char *href = attr(s->el, "href");
    char *abs = href ? au_url_resolve_dup(dv->base, href) : NULL;
    if (!abs)
        return !s->src->failed;
    bool same = same_str(abs, s->src->url);
    au_free(abs);
    return !same;
}

static void prune_slots(AuDocView *dv)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < dv->nslots; i++) {
        SheetSlot *s = &dv->slots[i];
        if (au_node_is_connected(&s->el->node) && !slot_stale(dv, s)) {
            dv->slots[k++] = *s;
            continue;
        }
        if (dv->resolver)
            au_css_resolver_detach_sheets(dv->resolver); /* until rebuild_resolver, which follows */
        au_node_unref(&s->el->node);
        free_sheet_src(dv, s->src);
        dv->sheets_dirty = true;
    }
    dv->nslots = k;
}

/* The embedder's clock for the time counters; 0 without one (the fallback
 * clock advances when read, so it is not used for measuring). */
static uint32_t perf_now(AuDocView *dv)
{
    if (!dv->cb.now_ms)
        return 0;
    /* The low 32 bits of the milliseconds: a clock past 2^32 ms (49 days)
     * is outside uint32_t's range, where a cast is undefined. */
    double ms = fmod(dv->cb.now_ms(dv->cb.ctx, dv), 4294967296.0);
    return ms >= 0 ? (uint32_t)ms : 0;
}

AuPerf *au_docview_perf(AuDocView *dv)
{
    return &dv->perf;
}

uint32_t au_dv_perf_now(AuDocView *dv)
{
    return perf_now(dv);
}

bool au_docview_content_loaded(const AuDocView *dv)
{
    return dv->doc && !dv->parsing && (!dv->script || au_docscript_content_loaded(dv->script));
}

void au_docview_script_memory(const AuDocView *dv, size_t *used, size_t *limit)
{
    *used = *limit = 0;
    if (dv->script)
        au_docscript_heap(dv->script, used, limit);
}

void au_docview_script_summary(const AuDocView *dv, AuBuf *out)
{
    if (dv->script)
        au_docscript_memory_summary(dv->script, out);
}

void au_docview_script_peaks(const AuDocView *dv, size_t *page_peak, size_t *worker_peak)
{
    *page_peak = *worker_peak = 0;
    if (dv->script)
        au_docscript_peaks(dv->script, page_peak, worker_peak);
}

/* Brings styles up to date; returns true if the tree was restyled. */
static bool update_style(AuDocView *dv)
{
    bool changed = false;
    if (dv->frames_dirty)
        au_frames_sync(dv); /* what an object or embed shows decides its box */
    if (dv->sheets_rescan) {
        dv->sheets_rescan = false;
        prune_slots(dv);
        scan_document(dv, !dv->parsing);
    }
    if (dv->sheets_dirty) {
        rebuild_resolver(dv);
        dv->sheets_dirty = false;
    }
    if (dv->webfonts && au_webfonts_take_changed(dv->webfonts)) {
        /* A web font arrived: lay out again with it (font-display: swap). */
        new_layout(dv, au_webfonts_set(dv->webfonts));
        dv->style_all = true; /* ex and ch units */
    }
    bool any_style = dv->viewport_restyle || dv->style_all || dv->tree_dirty || dv->restyle_needed;
    uint32_t styled0 = any_style ? au_css_resolver_styled_count(dv->resolver) : 0;
    if (any_style)
        dv->stats.style_passes++;
    if (dv->viewport_restyle && !dv->style_all && !dv->tree_dirty) {
        /* Only the viewport changed: the document is the same, so the
         * layout compares each box's style and keeps the box tree unless
         * a media query changed what is displayed. */
        /* The resolver knows what the viewport change reaches: nothing
         * (no media query changed its result, no style uses viewport
         * units), the elements that use viewport units, or everything. */
        if (dv->restyle_needed || au_css_resolver_viewport_restyle(dv->resolver) != AU_CSS_VIEWPORT_NONE) {
            au_css_restyle(dv->resolver, dv->doc);
            dv->stats.styled_elements += au_css_resolver_styled_count(dv->resolver) - styled0;
            au_layout_styles_changed(dv->layout);
            changed = true;
        } else {
            dv->stats.style_passes--; /* counted above; none ran */
        }
        dv->viewport_restyle = false;
        dv->restyle_needed = false;
    } else if (dv->style_all || dv->tree_dirty) {
        dv->viewport_restyle = false;
        au_css_style_document(dv->resolver, dv->doc);
        dv->stats.styled_elements += au_css_resolver_styled_count(dv->resolver) - styled0;
        au_layout_tree_changed(dv->layout);
        au_docimages_tree_changed(dv->imgs);
        dv->style_all = dv->tree_dirty = false;
        dv->restyle_needed = false;
        changed = true;
        if (dv->webfonts)
            au_webfonts_scan(dv->webfonts, dv->doc, fetch_font, dv);
    } else if (dv->restyle_needed) {
        au_css_restyle(dv->resolver, dv->doc);
        dv->stats.styled_elements += au_css_resolver_styled_count(dv->resolver) - styled0;
        au_layout_styles_changed(dv->layout);
        dv->restyle_needed = false;
    }
    return changed;
}

/* Once the document has loaded, the sheets' texts go cold. They are
 * packed one sheet a pass, on passes that had nothing else to do, after
 * SHEET_QUIET_MS in which no block was parsed late (a page that script
 * builds after its load is still matching rules for the first time, and
 * each such parse would inflate a piece of packed text). Without a clock:
 * after SHEET_QUIET_PASSES such passes. */
#define SHEET_QUIET_MS 5000
#define SHEET_QUIET_PASSES 32
static void pack_when_quiet(AuDocView *dv)
{
    uint32_t count = au_css_late_parse_count(), t = perf_now(dv);
    if (!dv->pack_watching || count != dv->pack_seen) {
        dv->pack_watching = true;
        dv->pack_seen = count;
        dv->pack_since = t;
        dv->pack_passes = 0;
        return;
    }
    dv->pack_passes++;
    if (dv->cb.now_ms ? t - dv->pack_since >= SHEET_QUIET_MS : dv->pack_passes >= SHEET_QUIET_PASSES)
        au_docview_pack_sheets(dv);
}

bool au_docview_update(AuDocView *dv)
{
    if (!dv->doc)
        return false;
    /* Timed for the Performance window: a pass counts when it had work. */
    bool style_work = dv->sheets_rescan || dv->sheets_dirty || dv->style_all || dv->tree_dirty || dv->restyle_needed;
    uint32_t t0 = perf_now(dv);
    bool changed = update_style(dv);
    uint32_t t1 = perf_now(dv);
    if (style_work)
        au_perf_add(&dv->perf, AU_PERF_STYLE, t1 - t0, NULL, t1);
    if (au_docimages_take_layout_changed(dv->imgs))
        dv->images_dirty = true;
    bool images = dv->images_dirty;
    if (images) {
        au_layout_images_changed(dv->layout);
        dv->images_dirty = false;
    }
    bool laid_out = au_layout_update(dv->layout, (float)dv->vw, (float)dv->vh);
    if (dv->slim_scrolls) {
        /* The first layout after the view was hidden: its scroll
         * containers return to where they were. */
        au_layout_scroll_restore(dv->layout, dv->slim_scrolls, dv->nslim_scrolls);
        au_layout_scroll_free(dv->slim_scrolls, dv->nslim_scrolls);
        dv->slim_scrolls = NULL;
        dv->nslim_scrolls = 0;
    }
    if (laid_out) {
        changed = true;
        if (images)
            invalidate_all(dv); /* an image's size moved other boxes */
    }
    uint32_t t2 = perf_now(dv);
    if (laid_out || t2 != t1)
        au_perf_add(&dv->perf, AU_PERF_LAYOUT, t2 - t1, NULL, t2);
    if (changed)
        dv->layout_ran = true;
    if (!style_work && !changed && dv->finished_reported && !dv->sheets_packed && !au_docview_loading(dv))
        pack_when_quiet(dv);
    if (dv->nframes)
        au_frames_layout(dv);
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
        view_device(dv, &dev);
        au_css_resolver_set_device(dv->resolver, &dev);
        /* Media queries and viewport units may change. */
        dv->viewport_restyle = true;
    }
    undefer_sheets(dv); /* sheets whose media query matches at this size */
    if (dv->script)
        au_docscript_resized(dv->script);
}

void au_docview_content_size(AuDocView *dv, int *width, int *height)
{
    au_docview_update(dv);
    float w, h;
    au_layout_document_size(dv->layout, &w, &h);
    *width = au_f2px(w + 0.99f);
    *height = au_f2px(h + 0.99f);
}

void au_docview_paint(AuDocView *dv, AuCanvas *c, AuRect view, int scroll_x, int scroll_y)
{
    if (!dv->parent)
        au_frames_idle(dv);
    au_docview_update(dv);
    dv->scroll_x = scroll_x;
    dv->scroll_y = scroll_y;
    /* Images near the viewport (not just this paint's rectangle) decode
     * now, at the size they are shown. */
    au_docimages_prepare(dv->imgs, dv->doc, dv->layout, dv->base,
                         (AuRect){(float)scroll_x, (float)scroll_y, (float)dv->vw, (float)dv->vh});
    uint32_t t0 = perf_now(dv);
    au_docimages_paint_begin(dv->imgs);
    au_layout_paint(dv->layout, c, view, (float)scroll_x, (float)scroll_y);
    au_docimages_paint_end(dv->imgs);
    uint32_t t1 = perf_now(dv);
    au_perf_add(&dv->perf, AU_PERF_PAINT, t1 - t0, NULL, t1);
    if (view.w > 0 && view.h > 0)
        au_perf_pixels(&dv->perf, (uint32_t)view.w * (uint32_t)view.h);
    images_changed(dv); /* an image found broken while painting */
}

int au_docview_scroll_damage(AuDocView *dv, int ox, int oy, int nx, int ny, AuIRect *out, int max)
{
    if (!dv->doc || max <= 0)
        return -1;
    if (au_docview_update(dv))
        return -1; /* geometry moved: everything is painted again */
    if (au_layout_needs_paint(dv->layout))
        return -1; /* a change not painted yet */
    AuRect r[32];
    int n = au_layout_scroll_damage(dv->layout, (float)ox, (float)oy, (float)nx, (float)ny, r, max < 32 ? max : 32);
    for (int i = 0; i < n; i++) {
        /* Outward to whole pixels (one more than needed on exact edges),
         * clamped to keep the conversion defined. */
        float v[4] = {r[i].x, r[i].y, r[i].x + r[i].w, r[i].y + r[i].h};
        int iv[4];
        for (int k = 0; k < 4; k++) {
            float f = v[k] < -1e6f ? -1e6f : v[k] > 1e6f ? 1e6f : v[k];
            iv[k] = au_f2px(f) + (k < 2 ? -1 : 1);
        }
        out[i] = (AuIRect){iv[0], iv[1], iv[2], iv[3]};
    }
    return n;
}

/* ---- Input ---- */

void au_docview_set_scroll(AuDocView *dv, int scroll_x, int scroll_y)
{
    if (dv->script && (scroll_x != dv->scroll_x || scroll_y != dv->scroll_y))
        au_docscript_scrolled(dv->script);
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
    /* only the elements below the common ancestor change state */
    au_css_move_state(dv->resolver, *slot, el, bit);
    if (*slot)
        au_node_unref(&(*slot)->node);
    *slot = el;
    if (el)
        au_node_ref(&el->node);
    dv->restyle_needed = true;
}

static bool node_contains(const AuNode *a, const AuNode *b)
{
    for (; b; b = b->parent)
        if (a == b)
            return true;
    return false;
}

static void link_url(AuDocView *dv, AuElement *link, char *out, size_t cap)
{
    out[0] = 0;
    const char *href = link ? attr(link, "href") : NULL;
    if (href && !au_url_resolve(dv->base, href, out, cap))
        out[0] = 0;
}

void au_dv_set_hover(AuDocView *dv, AuElement *el)
{
    if (dv->doc && dv->resolver)
        set_state(dv, &dv->hover, el, AU_STATE_HOVER);
}

/* The pointer left the view (it moved out of the frame). */
void au_dv_mouse_leave(AuDocView *dv)
{
    if (!dv->doc)
        return;
    bool f;
    AuEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = AU_EVENT_MOUSE_MOVE;
    ev.x = ev.y = -100000;
    if (dv->hover_frame)
        au_frames_mouse(dv, &ev, &f);
    if (dv->script && dv->hover)
        au_docscript_hover(dv->script, dv->hover, NULL, -1, -1);
    if (!dv->doc)
        return;
    set_state(dv, &dv->hover, NULL, AU_STATE_HOVER);
    if (dv->hover_link[0]) {
        dv->hover_link[0] = 0;
        if (dv->cb.status_text)
            dv->cb.status_text(dv->cb.ctx, dv, "");
    }
    if (dv->restyle_needed) {
        au_docview_update(dv);
        if (au_layout_needs_paint(dv->layout))
            invalidate_all(dv);
    }
}

void au_dv_tree_dirty(AuDocView *dv)
{
    dv->tree_dirty = true;
    invalidate_all(dv);
}

/* Follows a link: into a frame, in the tab or in a new tab, as its
 * target says. */
static void follow_link(AuDocView *dv, const char *url, const char *target, bool new_tab)
{
    AuDocView *to = NULL;
    int where = au_frames_route(dv, target, new_tab, false, &to);
    if (where == 1) {
        au_frames_navigate(to, url, NULL);
    } else if (where >= 2) {
        AuDocView *top = au_dv_top(dv);
        if (top->cb.open_url)
            top->cb.open_url(top->cb.ctx, top, url, where == 3);
    }
}

/* Whether a popover of doc is showing (in the top layer). */
static bool popover_open(const AuDocument *doc)
{
    for (uint32_t i = 0; i < doc->ntop; i++)
        if (doc->top_layer[i]->state & AU_STATE_POPOVER_OPEN)
            return true;
    return false;
}

bool au_docview_mouse(AuDocView *dv, const AuEvent *ev)
{
    if (!dv->doc)
        return false;
    bool in_frame;
    if (au_frames_mouse(dv, ev, &in_frame)) {
        if (!dv->parent)
            au_frames_idle(dv);
        return in_frame;
    }
    AuHitResult r;
    au_docview_hit_test(dv, ev->x, ev->y, &r);
    if (dv->script && (ev->type == AU_EVENT_MOUSE_DOWN || ev->type == AU_EVENT_MOUSE_UP) && popover_open(dv->doc)) {
        /* Light dismiss: a press and release outside an auto popover
         * hide it, before the page sees the events. */
        AuElement *target = r.element;
        if (target)
            au_node_ref(&target->node);
        au_docscript_light_dismiss(dv->script, au_dv_alive(dv, target) ? target : NULL, ev->type == AU_EVENT_MOUSE_UP);
        if (target)
            au_node_unref(&target->node);
        if (!dv->doc)
            return true;
        au_docview_hit_test(dv, ev->x, ev->y, &r);
    }
    if (dv->script) {
        /* Listeners may change or replace the document: the target is
         * held across the hover events (whose listeners may remove it
         * before the event itself is fired), and the hit test is made
         * again afterwards, since r's pointers are not held. */
        static const char *const types[] = {"mousemove", "mousedown", "mouseup"};
        int t = ev->type == AU_EVENT_MOUSE_MOVE ? 0 : ev->type == AU_EVENT_MOUSE_DOWN ? 1 : ev->type == AU_EVENT_MOUSE_UP ? 2 : -1;
        if (t >= 0) {
            AuElement *target = r.element;
            if (target)
                au_node_ref(&target->node);
            if (t == 0 && target != dv->hover)
                au_docscript_hover(dv->script, dv->hover, target, ev->x - dv->scroll_x, ev->y - dv->scroll_y);
            if (dv->script && au_dv_alive(dv, target))
                au_docscript_mouse(dv->script, ev, target, types[t], ev->x - dv->scroll_x, ev->y - dv->scroll_y);
            if (target)
                au_node_unref(&target->node);
            if (!dv->doc)
                return true;
            au_docview_hit_test(dv, ev->x, ev->y, &r);
        }
    }
    bool used = false;
    if (ev->type == AU_EVENT_MOUSE_DOWN || ev->type == AU_EVENT_MOUSE_MOVE || ev->type == AU_EVENT_MOUSE_UP) {
        /* Focus, caret placement, drags in fields, pop-ups, buttons. Its
         * events (focus, blur, change, input) may have changed the page,
         * also when no control used the press: r is made again whenever
         * script ran. */
        uint32_t runs = au_dv_script_runs(dv);
        used = au_forms_mouse(dv, ev, &r);
        if (!dv->doc)
            return true;
        if ((used && ev->type != AU_EVENT_MOUSE_UP) || au_dv_script_runs(dv) != runs)
            au_docview_hit_test(dv, ev->x, ev->y, &r);
    }
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
        used = used || (dv->restyle_needed && !before);
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
        /* Held across the click: a listener may remove the link, and a
         * new element at its address must not be taken for it. */
        if (down_link)
            au_node_ref(&down_link->node);
        bool follow = true;
        if (down && r.element) {
            /* click goes to the nearest common ancestor of the press and
             * release targets */
            AuElement *t = r.element;
            while (t && !node_contains(&t->node, &down->node))
                t = au_is_element(t->node.parent) ? au_as_element(t->node.parent) : NULL;
            if (t) {
                au_node_ref(&t->node);
                if (au_docscript_running(dv->script)) {
                    /* the bindings run the activation behaviour */
                    follow = au_docscript_mouse(dv->script, ev, t, "click", ev->x - dv->scroll_x, ev->y - dv->scroll_y);
                } else if (dv->doc) {
                    AuRect b;
                    int ix = 0, iy = 0;
                    if (au_layout_element_rect(dv->layout, t, &b)) {
                        ix = ev->x - au_f2px(b.x);
                        iy = ev->y - au_f2px(b.y);
                    }
                    au_forms_activate(dv, t, ix, iy);
                }
                au_node_unref(&t->node);
            }
            if (!dv->doc) {
                if (down_link)
                    au_node_unref(&down_link->node);
                return true;
            }
            au_docview_hit_test(dv, ev->x, ev->y, &r); /* listeners may have changed the page */
        }
        set_state(dv, &dv->active, NULL, AU_STATE_ACTIVE);
        bool same_link = r.link && r.link == down_link;
        if (down_link)
            au_node_unref(&down_link->node); /* r.link, when it is the same, is in the document */
        if (follow && same_link) {
            char url[2048];
            link_url(dv, r.link, url, sizeof url);
            const char *target = attr(r.link, "target");
            bool new_tab = (ev->mods & AU_MOD_COMMAND) || (target && au_ieq(target, strlen(target), "_blank", 6));
            if (au_ieq(url, strlen(url) < 11 ? strlen(url) : 11, "javascript:", 11)) {
                /* Runs only in this document, and only with scripting on. */
                if (dv->script)
                    au_docscript_javascript_url(dv->script, url);
            } else if (url[0]) {
                follow_link(dv, url, target, new_tab);
            }
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
    if (dv->nframes && au_frames_scroll(dv, x, y, dx, dy))
        return true;
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
                return au_f2px(r.y);
        return -1;
    }
    return au_f2px(r.y);
}

/* ---- Scripts (see script.c) ---- */

void au_docview_set_scripting(AuDocView *dv, bool on)
{
    dv->scripting = on;
}

void au_docview_set_script_memory(AuDocView *dv, size_t page_bytes, size_t worker_bytes)
{
    dv->script_mem = page_bytes;
    dv->worker_mem = worker_bytes;
}

void au_docview_set_user_agent(AuDocView *dv, const char *ua)
{
    au_free(dv->user_agent);
    dv->user_agent = NULL;
    if (ua && ua[0]) {
        size_t n = strlen(ua) + 1;
        dv->user_agent = au_alloc(n);
        memcpy(dv->user_agent, ua, n);
    }
}

const char *au_dv_user_agent(const AuDocView *dv) { return dv->user_agent; }

void au_docview_set_referrer(AuDocView *dv, const char *referrer)
{
    au_free(dv->referrer);
    dv->referrer = referrer && referrer[0] ? au_strndup(referrer, strlen(referrer)) : NULL;
}

const char *au_dv_referrer(const AuDocView *dv) { return dv->referrer; }

bool au_docview_scripting(const AuDocView *dv)
{
    return dv->scripting;
}

bool au_docview_run_scripts(AuDocView *dv, uint32_t budget_ms)
{
    if (!dv->parent)
        au_frames_idle(dv);
    bool ran = dv->nframes && au_frames_run_scripts(dv, budget_ms);
    if (!dv->script)
        return ran;
    if (!dv->cb.now_ms)
        dv->clock += budget_ms;
    if (dv->layout_ran) {
        dv->layout_ran = false;
        au_docscript_layout_done(dv->script);
    }
    if (dv->script && au_docscript_run(dv->script, budget_ms))
        ran = true;
    if (!dv->parent)
        au_frames_idle(dv);
    return ran;
}

uint32_t au_docview_scripts_due(AuDocView *dv)
{
    uint32_t due = dv->script ? au_docscript_due(dv->script) : UINT32_MAX;
    uint32_t fd = dv->nframes ? au_frames_scripts_due(dv) : UINT32_MAX;
    if (au_dv_top(dv)->frames_work)
        return 0;
    return fd < due ? fd : due;
}

bool au_docview_eval(AuDocView *dv, const char *src, AuBuf *out)
{
    return dv->script && au_docscript_eval(dv->script, src, out);
}

uint32_t au_dv_request(AuDocView *dv, const char *url, bool background)
{
    Request *r = add_request(dv, url, AU_RES_OTHER);
    if (!r)
        return 0;
    r->background = background;
    if (background)
        dv->nbg++;
    else
        dv->finished_reported = false;
    return r->id;
}

uint32_t au_dv_request_ex(AuDocView *dv, const AuResourceRequest *rq, bool background)
{
    Request *r = add_request_ex(dv, rq);
    if (!r)
        return 0;
    r->background = background;
    if (background)
        dv->nbg++;
    else
        dv->finished_reported = false;
    return r->id;
}

void au_dv_cancel(AuDocView *dv, uint32_t id)
{
    Request *r = find_request(dv, id);
    if (!r)
        return;
    loader_cancel(dv, id);
    if (r->background)
        dv->nbg--;
    au_buf_free(&r->data);
    au_free(r->img_url);
    *r = dv->reqs[--dv->nreqs];
}

void au_dv_dom_changed(AuDocView *dv, AuNode *n, AuAtom attr_name, int kind)
{
    if (!dv->doc)
        return;
    AuElement *el = au_is_element(n) ? au_as_element(n) : NULL;
    /* Frame elements may have come or gone (children), or changed what
     * they show (an attribute of one). */
    if (kind == 1 || (kind == 2 && el && el->ns == AU_NS_HTML &&
                      (el->tag == AU_ATOM_iframe || el->tag == AU_ATOM_frame || el->tag == AU_ATOM_object ||
                       el->tag == AU_ATOM_embed)))
        dv->frames_dirty = true;
    if (kind == 2 && el && dv->resolver) { /* attribute */
        /* Attributes the box tree never reads: they reach layout only
         * through selectors and attr(), which the restyle covers. Scripts
         * set these on every keystroke (aria-expanded and
         * aria-activedescendant on a search field, the value of a hidden
         * input that mirrors it). */
        const char *an = attr_name ? au_atom_str(attr_name) : "";
        const char *ty = au_element_get_attr(el, AU_ATOM_type);
        bool hidden_value = !strcmp(an, "value") && el->ns == AU_NS_HTML && el->tag == AU_ATOM_input && ty &&
                            (ty[0] == 'h' || ty[0] == 'H') && strlen(ty) == 6;
        bool style_only = attr_name == AU_ATOM_class || attr_name == AU_ATOM_id || attr_name == AU_ATOM_style ||
                          !strncmp(an, "aria-", 5) || !strncmp(an, "data-", 5) || !strcmp(an, "role") ||
                          !strcmp(an, "title") || !strcmp(an, "tabindex") || !strcmp(an, "autocomplete") || hidden_value;
        au_css_invalidate(dv->resolver, el,
                          attr_name == AU_ATOM_style ? AU_CSS_CHANGE_INLINE_STYLE : AU_CSS_CHANGE_ATTR, attr_name);
        dv->restyle_needed = true;
        if (!style_only)
            dv->tree_dirty = true; /* src, colspan, value...: content may change */
        if (el->tag == AU_ATOM_link || el->tag == AU_ATOM_style || el->tag == AU_ATOM_base)
            dv->sheets_rescan = true;
        /* Removing open takes a modal dialog out of the top layer
         * (au_element_remove_attr clears AU_STATE_MODAL): :modal changes. */
        if (el->tag == AU_ATOM_dialog && !strcmp(an, "open"))
            au_css_invalidate(dv->resolver, el, AU_CSS_CHANGE_STATE, 0);
    } else if (kind == 5) { /* a control's value or selection: repaint only */
        if (el)
            au_dv_invalidate_element(dv, el);
    } else if (kind == 4 && el && dv->resolver) { /* state: checked, focus... */
        au_css_invalidate(dv->resolver, el, AU_CSS_CHANGE_STATE, 0);
        dv->restyle_needed = true;
        dv->tree_dirty = true;
    } else {
        dv->tree_dirty = true;
        dv->sheets_rescan = true;
    }
}

/* ---- CSSOM (script.c): the sheet of a style or link element ---- */

static SheetSrc *sheet_of(AuDocView *dv, AuElement *el)
{
    if (!dv->doc)
        return NULL;
    if (dv->sheets_rescan) { /* script changed the tree: find new sheets first */
        dv->sheets_rescan = false;
        prune_slots(dv);
        scan_document(dv, !dv->parsing);
    }
    for (uint32_t i = 0; i < dv->nslots; i++)
        if (dv->slots[i].el == el)
            return dv->slots[i].src;
    return NULL;
}

int au_dv_sheet_text(AuDocView *dv, AuElement *el, AuBuf *text, AuBuf *url)
{
    SheetSrc *s = sheet_of(dv, el);
    if (!s || s->failed)
        return 0;
    /* not fetched yet, or held back until its media query matches */
    if (!s->text && !s->sheet)
        return 1;
    au_buf_puts(url, s->url ? s->url : "");
    if (!(s->om_sheet ? au_css_sheet_text_get(s->om_sheet, text) : src_text_get(s, text)))
        return 3; /* a sheet that could not keep its text */
    return 2;
}

void au_dv_sheet_set_text(AuDocView *dv, AuElement *el, const char *text, size_t len)
{
    SheetSrc *s = sheet_of(dv, el);
    if (!s || (!s->text && !s->sheet && !s->om_sheet))
        return;
    /* The resolver borrows the parsed sheets: it lets go of them before
     * one is freed, and update_style hands them back (sheets_dirty). */
    if (dv->resolver)
        au_css_resolver_detach_sheets(dv->resolver);
    if (s->om_sheet)
        au_css_sheet_free(s->om_sheet);
    s->om_sheet = au_css_sheet_parse(text, len, AU_CSS_ORIGIN_AUTHOR, s->url, s->media); /* keeps a copy of the text */
    dv->sheets_packed = false;
    dv->sheets_dirty = true;
}

void au_dv_sheet_disable(AuDocView *dv, AuElement *el, bool disabled)
{
    SheetSrc *s = sheet_of(dv, el);
    if (!s || s->disabled == disabled)
        return;
    s->disabled = disabled;
    dv->sheets_dirty = true;
}

void au_dv_parsed_more(AuDocView *dv)
{
    if (!dv->parser)
        return;
    dv->tree_dirty = true;
    if (dv->parsing && au_html_parser_done(dv->parser)) {
        dv->parsing = false;
        scan_document(dv, true);
        invalidate_all(dv);
        au_forms_autofocus(dv);
        if (dv->script)
            au_docscript_parse_done(dv->script);
        maybe_finished(dv);
    } else {
        scan_document(dv, false);
    }
}

void au_dv_check_finished(AuDocView *dv)
{
    maybe_finished(dv);
}

/* ---- Canvases ---- */

struct AuCanvasSet *au_dv_canvas_set(AuDocView *dv)
{
    if (!dv->canvases) {
        dv->canvases = au_canvas_set_new(0);
        au_canvas_set_fonts(dv->canvases, dv->cur_fonts);
    }
    return dv->canvases;
}

/* The pixels of an image element for drawImage: the image cache's entry
 * for the source layout chose, decoded at its natural size where the
 * cache's per-image limit allows. */
bool au_dv_canvas_image(AuDocView *dv, AuElement *img, const AuSurface **px, int *w, int *h, char *url,
                        size_t url_cap)
{
    AuBuf src;
    au_buf_init(&src);
    bool ok = false;
    char *abs = au_layout_image_source(dv->layout, img, &src) ? au_url_resolve_dup(dv->base, au_buf_cstr(&src)) : NULL;
    if (abs) {
        AuImageCache *cache = au_docimages_cache(dv->imgs);
        AuImageEntry *e = au_image_cache_find(cache, abs);
        int iw = 0, ih = 0;
        if (e && au_image_entry_state(e) != AU_IMAGE_FAILED && au_image_entry_size(e, &iw, &ih) && iw > 0 && ih > 0) {
            au_image_entry_use(cache, e, iw, ih);
            if (au_image_entry_state(e) == AU_IMAGE_READY) {
                *px = au_image_entry_surface(e);
                *w = iw;
                *h = ih;
                snprintf(url, url_cap, "%s", abs);
                ok = true;
            }
        }
        au_free(abs);
    }
    au_buf_free(&src);
    return ok;
}

/* Layout's canvas lookup: the bitmap of a canvas element. */
static int layout_canvas(void *ctx, AuElement *el, const AuSurface **px)
{
    AuDocView *dv = ctx;
    *px = NULL;
    if (!dv->script)
        return -1; /* script is off: the fallback content shows */
    AuCanvas2D *c = au_canvas_set_find(dv->canvases, el);
    if (c) {
        if (!au_c2d_alpha(c))
            au_c2d_materialize(c); /* a canvas without alpha is opaque black from the start */
        *px = au_c2d_surface(c);
    }
    return 0;
}

/* Script drew to a canvas: repaint the part of its box that changed. */
static void canvas_dirty(void *ctx, AuCanvas2D *c, void *owner, AuIRect area)
{
    AuDocView *dv = ctx;
    AuElement *el = owner;
    AuRect cb;
    int cw = au_c2d_width(c), ch = au_c2d_height(c);
    if (!el || !au_layout_element_content_rect(dv->layout, el, &cb))
        return;
    if (cw <= 0 || ch <= 0 || !(cb.w > 0) || !(cb.h > 0)) {
        au_dv_invalidate_element(dv, el);
        return;
    }
    float sx = cb.w / (float)cw, sy = cb.h / (float)ch;
    AuIRect r = {au_f2px(floorf(cb.x + (float)area.x0 * sx)) - 1, au_f2px(floorf(cb.y + (float)area.y0 * sy)) - 1,
                 au_f2px(ceilf(cb.x + (float)area.x1 * sx)) + 1, au_f2px(ceilf(cb.y + (float)area.y1 * sy)) + 1};
    au_dv_invalidate_element_rect(dv, el, r);
}

void au_dv_turn_end(AuDocView *dv)
{
    /* Canvases script drew to: one repaint of each, however many calls. */
    if (dv->canvases && au_canvas_set_dirty(dv->canvases))
        au_canvas_set_take_dirty(dv->canvases, canvas_dirty, dv);
    /* While a key is handled, au_docview_key decides at its end: it
     * updates, and repaints the page only if something changed. */
    if (dv->in_key)
        return;
    if (dv->tree_dirty || dv->restyle_needed || dv->sheets_dirty || dv->sheets_rescan)
        invalidate_all(dv);
}

void au_dv_invalidate_all(AuDocView *dv)
{
    invalidate_all(dv);
}

void au_dv_update_style(AuDocView *dv)
{
    if (dv->doc)
        update_style(dv);
}

const AuDocViewCallbacks *au_dv_callbacks(const AuDocView *dv)
{
    return &dv->cb;
}

void au_dv_scroll(const AuDocView *dv, int *x, int *y, int *vw, int *vh)
{
    *x = dv->scroll_x;
    *y = dv->scroll_y;
    *vw = dv->vw;
    *vh = dv->vh;
}

AuCssResolver *au_dv_resolver(const AuDocView *dv)
{
    return dv->resolver;
}

double au_dv_now(AuDocView *dv)
{
    if (dv->cb.now_ms)
        return dv->cb.now_ms(dv->cb.ctx, dv);
    return dv->clock += 1; /* every read advances it, so time limits still end loops */
}
