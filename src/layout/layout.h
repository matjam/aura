/* Aura layout: the box tree, CSS layout and painting of a styled document.
 *
 * To lay out and paint a document whose styles are computed
 * (au_css_style_document):
 *
 *     AuLayoutEnv env = { .fonts = fonts };
 *     AuLayout *L = au_layout_new(&env);
 *     au_layout_set_document(L, doc, base_url);
 *     au_layout_update(L, 800, 600);           // builds boxes, lays out
 *     au_layout_paint(L, canvas, view, 0, 0);  // paints document (0,0)+
 *
 * After a DOM change call au_layout_tree_changed; after a restyle call
 * au_layout_styles_changed (it compares each box's old and new style and
 * marks only what needs it: nothing, a repaint, a relayout of the box, or
 * a rebuild of the box tree). au_layout_update then does the minimum.
 *
 * What is incremental: intrinsic sizes are cached per box; a relayout
 * skips every independent formatting context (inline-block, float,
 * absolutely positioned box, flex item, overflow/flow-root block) whose
 * content is clean and whose constraints did not change, reusing its
 * fragments; style changes that only affect painting (colours,
 * backgrounds, decorations, outlines, visibility, opacity) cause no
 * layout. A structural change (DOM, display, content, counters, text
 * transforms) rebuilds the whole box tree; layout of that tree is then
 * full.
 *
 * Coordinates are CSS pixels. Document coordinates have the origin at the
 * top-left of the initial containing block; the viewport shows the
 * document from the scroll offset. Positioned-fixed boxes stay put in the
 * viewport.
 *
 * Engine rules: no OS calls, allocation through au_alloc, no mutable
 * global state (everything hangs off AuLayout), and every recursion over
 * box depth has a guard (L_MAX_DEPTH in layout_int.h). */
#ifndef AURA_LAYOUT_H
#define AURA_LAYOUT_H

/* css.h and paint.h both define AuColor (the same type, different channel
 * orders). C99 compilers that warn about the repeated typedef are told it
 * is intended. Use l_paint_color in layout code to convert. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#endif
#include "../css/css.h"
#include "../paint/font.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

typedef struct AuLayout AuLayout;

/* What layout needs from the document's owner. */
typedef struct AuLayoutEnv {
    AuFontSet *fonts;
    void *ctx;
    /* Looks up an image by absolute URL (resolved by the caller from src
     * or url()). Returns true when its intrinsic size is known and sets
     * *w, *h; *pixels is the decoded image or NULL when only the size is
     * known (a placeholder is painted). Returns false while the image is
     * not loaded or failed. Optional. */
    bool (*image)(void *ctx, const char *url, int *w, int *h, const AuSurface **pixels);
    /* Resolves a URL attribute against the document base. Writes into out
     * (cap bytes) and returns false when it cannot. Optional; without it
     * URLs are passed through as written. */
    bool (*resolve_url)(void *ctx, const char *url, char *out, size_t cap);
    /* Called before an image is painted at dw x dh pixels (backgrounds,
     * masks, list markers, images in inline SVG), so a vector image can
     * be drawn at that size; the surface image() returned is then current.
     * Optional. */
    void (*image_use)(void *ctx, const char *url, int dw, int dh);
} AuLayoutEnv;

AuLayout *au_layout_new(const AuLayoutEnv *env);
void au_layout_free(AuLayout *L);

/* Sets the document (not owned; it must outlive the layout or be replaced
 * first). NULL clears it. */
void au_layout_set_document(AuLayout *L, AuDocument *doc);

/* The DOM changed (nodes inserted or removed, text changed). */
void au_layout_tree_changed(AuLayout *L);
/* Styles were recomputed. Call before anything else reads the layout:
 * boxes hold the previous styles until then. */
void au_layout_styles_changed(AuLayout *L);
/* An image finished loading: sizes may change. */
void au_layout_images_changed(AuLayout *L);

/* Brings the layout up to date for a viewport of vw x vh. Returns true if
 * any geometry changed (the caller repaints everything); false when the
 * last layout is still valid (a pure repaint change still needs a
 * repaint: see au_layout_needs_paint). */
bool au_layout_update(AuLayout *L, float vw, float vh);
/* True when something changed since the last paint. Cleared by
 * au_layout_paint. */
bool au_layout_needs_paint(const AuLayout *L);

/* The size of the document's scrollable area: at least the viewport. */
void au_layout_document_size(const AuLayout *L, float *w, float *h);

/* Paints the document region (scroll_x, scroll_y, view.w, view.h) into
 * the canvas rectangle view. Every pixel of view is painted (the canvas
 * background is the root's background or white). */
void au_layout_paint(AuLayout *L, AuCanvas *c, AuRect view, float scroll_x, float scroll_y);
/* For scrolling by moving pixels: the view scrolled from (ox, oy) to
 * (nx, ny) and the pixels painted before moved by the difference. Writes
 * the viewport areas (new viewport coordinates) that are then wrong and
 * must be painted again: where viewport-fixed boxes, and sticky boxes
 * whose offset changed, were moved to with the pixels, and where they
 * are now. Everything else paints at the same document position whatever
 * the scroll. Returns how many (at most max), or -1 when the pixels
 * cannot be moved at all (the layout is out of date, the canvas
 * background is fixed or a gradient over the view, too many boxes, or a
 * transform that makes an area unknown). */
int au_layout_scroll_damage(AuLayout *L, float ox, float oy, float nx, float ny, AuRect *out, int max);

/* ---- Hit testing ---- */
typedef struct AuHitResult {
    AuElement *element; /* the element of the topmost box under the point, or NULL */
    AuNode *node;       /* the text node under the point, or the element */
    size_t text_offset; /* byte offset into the text node's processed text when node is text */
    AuElement *link;    /* the nearest ancestor <a href> (or <area>), or NULL */
    float local_x, local_y; /* point relative to the element's border box */
} AuHitResult;

/* Finds what is at viewport point (x, y) with the document scrolled by
 * (scroll_x, scroll_y). Honors stacking order, clips, inner scroll offsets,
 * visibility and pointer-events. Returns false over nothing (the result
 * then names the root element, if any). */
bool au_layout_hit_test(AuLayout *L, float x, float y, float scroll_x, float scroll_y, AuHitResult *r);

/* The border box of el's first box in document coordinates (inline
 * elements: the union of their fragments). Returns false if el has no
 * box. */
bool au_layout_element_rect(AuLayout *L, const AuElement *el, AuRect *out);

/* Scrolls the innermost scroll container under viewport point (x, y) by
 * (dx, dy), clamped. Returns false when no inner scroller can move (the
 * caller then scrolls the viewport). */
bool au_layout_scroll_at(AuLayout *L, float x, float y, float scroll_x, float scroll_y, float dx, float dy);

/* Appends a text dump of the box tree with geometry (document
 * coordinates) to out. */
void au_layout_dump(AuLayout *L, AuBuf *out);

/* ---- Statistics ---- */
typedef struct AuLayoutStats {
    uint32_t boxes, blocks, texts, inlines; /* box counts */
    uint32_t lines, frags;
    size_t tree_bytes;     /* arena bytes of the box tree (boxes and text) */
    size_t frag_bytes;     /* heap bytes of lines and fragments */
    uint32_t word_hits, word_misses; /* text measurement cache */
    uint32_t layouts_skipped;        /* independent subtrees reused by the last layout */
    uint32_t missing_glyphs;         /* drawn as missing-glyph boxes (no face had them), since creation */
} AuLayoutStats;
void au_layout_stats(const AuLayout *L, AuLayoutStats *s);

/* Font metrics for the CSS resolver's ex and ch units
 * (au_css_resolver_set_metrics with ctx = the AuLayout). */
void au_layout_css_metrics(void *layout, const AuStyle *style, float *ex_px, float *ch_px);

#endif
