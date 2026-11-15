/* Aura document view: one HTML document from bytes to pixels.
 *
 * An AuDocView owns a document and everything derived from it: the
 * parser (fed incrementally, scripting off, so <noscript> content shows
 * and <script> is ignored), the style sheets (<style>, <link
 * rel=stylesheet>, @import) and images fetched through a loader, the CSS
 * resolver, the layout, hover and active state, and link activation.
 *
 * To show a page:
 *
 *     AuDocView *dv = au_docview_new(fonts, &loader, &callbacks);
 *     au_docview_begin(dv, "file:///path/page.html");
 *     au_docview_feed(dv, bytes, len);         // as often as data arrives
 *     au_docview_finish(dv);
 *     au_docview_set_viewport(dv, 800, 600);
 *     au_docview_paint(dv, canvas, view, scroll_x, scroll_y);
 *
 * Resources. The loader is asynchronous in shape: request() only starts a
 * fetch and returns an id; the bytes arrive in later calls to
 * au_docview_resource_data and au_docview_resource_done (never from inside
 * request()). Style sheets are applied in document order whatever order
 * they arrive in. Images change layout when their size becomes known.
 *
 * Work is lazy: feeding, resources and input mark what is out of date;
 * au_docview_update (called by paint, hit testing and the size queries)
 * re-parses sheets, restyles and relays out only what is needed.
 *
 * Engine rules: no OS calls; no global mutable state. */
#ifndef AURA_PAGE_H
#define AURA_PAGE_H

#include "../html/html.h"
#include "../layout/layout.h"
#include "../platform/platform.h" /* AuEvent: a plain struct */

typedef struct AuDocView AuDocView;

typedef enum { AU_RES_STYLESHEET, AU_RES_IMAGE, AU_RES_OTHER } AuResourceKind;

typedef struct AuLoader {
    void *ctx;
    /* Starts fetching an absolute URL. Returns a nonzero id, or 0 when the
     * URL cannot be fetched (no data will come). */
    uint32_t (*request)(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind);
    /* Stops a fetch; no more data arrives for id. Optional. */
    void (*cancel)(void *ctx, uint32_t id);
    /* Decodes image bytes: the intrinsic size and, when the format is
     * supported, the pixels (a surface from au_surface_new that the view
     * frees). Returns false for data that is not an image. Optional:
     * without it images keep a placeholder size. */
    bool (*decode_image)(void *ctx, const void *data, size_t len, int *w, int *h, AuSurface **pixels);
} AuLoader;

typedef struct AuDocViewCallbacks {
    void *ctx;
    void (*title_changed)(void *ctx, AuDocView *dv, const char *title);
    /* Something needs repainting (document coordinates). */
    void (*invalidate)(void *ctx, AuDocView *dv, AuIRect r);
    /* The document size changed. */
    void (*size_changed)(void *ctx, AuDocView *dv);
    /* Status text: a hovered link's URL, or "" to clear. */
    void (*status_text)(void *ctx, AuDocView *dv, const char *text);
    /* The user activated a link: url is absolute; new_tab for
     * Command-click or target=_blank. */
    void (*open_url)(void *ctx, AuDocView *dv, const char *url, bool new_tab);
    /* Parsing finished and no resources are pending. */
    void (*load_finished)(void *ctx, AuDocView *dv);
} AuDocViewCallbacks;

AuDocView *au_docview_new(AuFontSet *fonts, const AuLoader *loader, const AuDocViewCallbacks *cb);
void au_docview_free(AuDocView *dv);

/* ---- Loading ---- */
/* Starts a new, empty document whose URL (and initial base URL) is url.
 * Pending resource requests of the previous document are cancelled. */
void au_docview_begin(AuDocView *dv, const char *url);
void au_docview_feed(AuDocView *dv, const char *bytes, size_t len);
void au_docview_finish(AuDocView *dv);
/* begin + feed + finish. */
void au_docview_load(AuDocView *dv, const char *bytes, size_t len, const char *url);

void au_docview_resource_data(AuDocView *dv, uint32_t id, const void *data, size_t len);
void au_docview_resource_done(AuDocView *dv, uint32_t id, bool ok);
/* Parsing is not finished or resources are pending. */
bool au_docview_loading(const AuDocView *dv);
/* Requests still outstanding. */
uint32_t au_docview_pending(const AuDocView *dv);

const char *au_docview_url(const AuDocView *dv);      /* never NULL */
const char *au_docview_base_url(const AuDocView *dv); /* never NULL */
const char *au_docview_title(const AuDocView *dv);    /* "" when none */
AuDocument *au_docview_document(const AuDocView *dv);
AuLayout *au_docview_layout(const AuDocView *dv);

/* Sets the user style sheet (the reader's defaults: font size, family),
 * cascaded below the author's sheets; NULL removes it. It applies to
 * this and later documents. */
void au_docview_set_user_css(AuDocView *dv, const char *css);
/* Sets the font preferences (default sizes, text zoom, minimum size; see
 * au_css_resolver_set_font_prefs) for this and later documents. A change
 * restyles the document at the next update. */
void au_docview_set_font_prefs(AuDocView *dv, const AuCssFontPrefs *p);

/* ---- Geometry and painting ---- */
void au_docview_set_viewport(AuDocView *dv, int width, int height);
/* Brings style and layout up to date. Returns true if geometry changed. */
bool au_docview_update(AuDocView *dv);
void au_docview_content_size(AuDocView *dv, int *width, int *height);
/* Paints the document so that document point (scroll_x, scroll_y) is at
 * view's top-left; paints every pixel of view. */
void au_docview_paint(AuDocView *dv, AuCanvas *c, AuRect view, int scroll_x, int scroll_y);

/* ---- Input ----
 * Mouse events carry document coordinates in ev->x, ev->y; the view uses
 * the scroll offset of the last paint (or au_docview_set_scroll) for
 * positioned-fixed content. Mouse move updates hover state and the status
 * text; a click on a link calls open_url. Returns true when the event did
 * something. */
void au_docview_set_scroll(AuDocView *dv, int scroll_x, int scroll_y);
bool au_docview_mouse(AuDocView *dv, const AuEvent *ev);
/* Scrolls an inner scroll container under document point (x, y). Returns
 * false when the viewport should scroll instead. */
bool au_docview_scroll_at(AuDocView *dv, int x, int y, int dx, int dy);
/* The hovered link's absolute URL, or "". */
const char *au_docview_hover_link(const AuDocView *dv);
bool au_docview_hit_test(AuDocView *dv, int x, int y, AuHitResult *r);

/* ---- Fragments ---- */
/* The element a fragment identifier names (id, or <a name>), or NULL.
 * frag is without the '#'. */
AuElement *au_docview_find_fragment(AuDocView *dv, const char *frag);
/* The document y to scroll to for a fragment, or -1 when not found. */
int au_docview_fragment_y(AuDocView *dv, const char *frag);

/* ---- URLs ----
 * Resolves rel against base into out (cap bytes). A small, limited
 * resolver for the forms pages use (absolute, scheme-relative,
 * path-absolute, path-relative with dot segments, query-only,
 * fragment-only); it does not percent-encode, apply IDNA or validate
 * hosts. A full WHATWG URL parser comes with networking. Returns false if
 * the result does not fit. */
bool au_url_resolve(const char *base, const char *rel, char *out, size_t cap);

#endif
