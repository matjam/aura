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
 * After a DOM change call au_layout_node_changed (or au_layout_tree_changed
 * when what changed is not known); after a restyle call
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
 * layout. A DOM change rebuilds the boxes below the nearest element whose
 * box can be built on its own (rebuild.c), and layout goes on from there.
 * A style change that alters structure (display, content, counters, text
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
    /* Resolves url against base, or against the document base when base
     * is NULL (an attribute, or a url() from an inline style). A url()
     * from a style sheet passes the sheet's URL (AuCssBlob.base_url), as
     * CSS Values 4 resolves it. Writes into out (cap bytes) and returns
     * false when it cannot. Optional; without it URLs are passed through
     * as written. */
    bool (*resolve_url)(void *ctx, const char *base, const char *url, char *out, size_t cap);
    /* Called before an image is painted at dw x dh pixels (backgrounds,
     * masks, list markers, images in inline SVG), so a vector image can
     * be drawn at that size; the surface image() returned is then current.
     * Optional. */
    void (*image_use)(void *ctx, const char *url, int dw, int dh);
    /* Nested documents (both optional). embed_kind says what an iframe,
     * frame, object or embed element shows: 0 for layout's own rules (an
     * image when the resource is one, else the fallback content), 1 a
     * nested document (a replaced box whose content paint_frame draws),
     * 2 the fallback content (an object's children). */
    int (*embed_kind)(void *ctx, AuElement *el);
    /* Paints el's nested document into its content box (canvas
     * coordinates), clipped to it. */
    void (*paint_frame)(void *ctx, AuElement *el, AuCanvas *c, AuRect content);
    /* A canvas element's bitmap (optional; without it a canvas is an
     * empty replaced box). Returns -1 when script is off for the document:
     * the element then shows its fallback content (its children). Else 0,
     * with *px the bitmap (premultiplied, the size script gave the
     * canvas) or NULL while nothing has been drawn. Layout takes the
     * box's natural size from the width and height attributes (300 x 150
     * by default) and paints the bitmap scaled into the content box. */
    int (*canvas)(void *ctx, AuElement *el, const AuSurface **px);
    /* What an image found by image() does not state of its size
     * (AU_LAYOUT_IMG_MISSING_*): an SVG with only a viewBox lacks a
     * width and a height, and the size image() gave is then only its
     * ratio. An <img> or content image with an auto size then fills its
     * containing block's width (CSS 2.1 10.3.2) and contributes 0 to
     * intrinsic sizes, and a background of auto size is fitted to its
     * box as CSS Images 3 says. Optional; without it every image has its
     * natural size. */
    unsigned (*image_missing)(void *ctx, const char *url);
    /* Whether text whose computed font-family list is families (n names,
     * as written), at weight and style, is painted invisible: its first
     * available font is a web font still in its block period
     * (font-display). The text keeps the fallback face's metrics.
     * Optional; asked again for each font after
     * au_layout_text_visibility_changed. */
    bool (*text_hidden)(void *ctx, const char *const *families, size_t n, int weight, bool italic);
    /* Samples the animation of style s at now_ms on the document's clock
     * (au_css_animation_sample with the document's resolver). Optional;
     * without it nothing animates. */
    bool (*anim_sample)(void *ctx, const AuStyle *s, uint32_t now_ms, AuCssAnimSample *out);
} AuLayoutEnv;
enum { AU_LAYOUT_IMG_MISSING_W = 1, AU_LAYOUT_IMG_MISSING_H = 2, AU_LAYOUT_IMG_MISSING_RATIO = 4 };

AuLayout *au_layout_new(const AuLayoutEnv *env);
void au_layout_free(AuLayout *L);

/* Sets the document (not owned; it must outlive the layout or be replaced
 * first). NULL clears it. */
void au_layout_set_document(AuLayout *L, AuDocument *doc);

/* Frees everything the layout can build again from the document and its
 * styles: the box tree with its lines and fragments, the stacking data,
 * the word width cache, the font lists and the scratch buffers. The
 * selection, the focus and the pass counts stay. The next
 * au_layout_update builds and lays out the whole tree. For a document
 * that is kept but not shown; inner scroll offsets live in the tree, so
 * save them first (au_layout_scroll_save). */
void au_layout_release(AuLayout *L);
/* The scroll offsets of the document's scroll containers (overflow:
 * auto or scroll boxes of elements), which are lost when the tree goes.
 * au_layout_scroll_save returns an au_alloc'd array of the containers
 * that are scrolled, each holding a reference to its element, and sets
 * *n (NULL and 0 when none is scrolled). au_layout_scroll_restore puts
 * them back on a tree that is laid out, clamped to what each box can
 * scroll now; au_layout_scroll_free drops the references and the array. */
typedef struct AuLayoutScroll {
    AuElement *el;
    float x, y;
} AuLayoutScroll;
AuLayoutScroll *au_layout_scroll_save(AuLayout *L, uint32_t *n);
void au_layout_scroll_restore(AuLayout *L, const AuLayoutScroll *s, uint32_t n);
void au_layout_scroll_free(AuLayoutScroll *s, uint32_t n);

/* The DOM changed in a way that may change any box: the whole box tree
 * is built at the next update. */
void au_layout_tree_changed(AuLayout *L);
/* The children of n were inserted or removed, the data of text node n
 * changed, or an attribute that the box tree reads changed on a child of
 * n (pass the element's parent then). n is referenced until the next
 * update, which rebuilds only the boxes of the nearest enclosing element
 * whose box can be built on its own (or the whole tree when there is
 * none). Call it after the change, and restyle before the update: the
 * new boxes take the current styles. */
void au_layout_node_changed(AuLayout *L, AuNode *n);
/* The differential check: after every update that rebuilt a subtree, a
 * whole tree is built and laid out beside the incremental one and
 * compared box for box (a build with -DAURA_INCR_CHECK checks every
 * layout and aborts on a difference). au_layout_check_failures returns
 * the number of differences found and appends the first one to msg
 * (may be NULL). */
void au_layout_set_check(AuLayout *L, bool on);
uint32_t au_layout_check_failures(const AuLayout *L, AuBuf *msg);
/* Subtree rebuilds since creation, the boxes they built, and the checks
 * the differential check ran. */
void au_layout_subtree_counts(const AuLayout *L, uint32_t *rebuilds, uint32_t *boxes, uint32_t *checks);
/* Styles were recomputed. Call before anything else reads the layout:
 * boxes hold the previous styles until then. */
void au_layout_styles_changed(AuLayout *L);
/* The loader's answer for some image changed (its size became known, it
 * failed, its pixels moved). Image boxes ask the loader again; one whose
 * size or presence changed is laid out again at the next update, and the
 * box tree is kept. The tree is built again only when its shape depends
 * on images (an object or embed element with a resource). */
void au_layout_images_changed(AuLayout *L);
/* What text_hidden answers may have changed (a web font's block period
 * ended or its face failed): text is painted again, with no layout. */
void au_layout_text_visibility_changed(AuLayout *L);
/* Appends to out the URL (as written, not resolved) that el shows: for
 * an <img>, the choice among srcset candidates (density 1; width
 * descriptors against sizes and the viewport of the last layout) and
 * the <source> elements of a <picture> parent, else src; for other
 * elements the src attribute (a video's poster). Returns false when there
 * is none. Layout asks the loader for the same URL. density (may be
 * NULL) is the chosen candidate's: the natural size is the image's
 * divided by it. Needs no layout pass. */
bool au_layout_image_source(AuLayout *L, const AuElement *el, AuBuf *out, float *density);
/* Calls fn with the document rectangle of each box that shows the image
 * at absolute URL url: image boxes, boxes with it as a background or
 * mask layer, and every inline SVG (one may hold it in an <image>).
 * Returns the number of boxes, or -1 when the caller must repaint the
 * whole page instead (the canvas background, a box that is fixed, sticky,
 * transformed or inside a scrolled box, or a box tree that is out of
 * date). */
int au_layout_image_users(AuLayout *L, const char *url, void (*fn)(void *ctx, AuRect r), void *ctx);

/* Brings the layout up to date for a viewport of vw x vh. Returns true if
 * any geometry changed (the caller repaints everything); false when the
 * last layout is still valid (a pure repaint change still needs a
 * repaint: see au_layout_needs_paint). */
bool au_layout_update(AuLayout *L, float vw, float vh);
/* True when something changed since the last paint. Cleared by
 * au_layout_paint. */
bool au_layout_needs_paint(const AuLayout *L);
/* False after a restyle that no au_layout_update has followed (a script's
 * getComputedStyle): the box tree then holds boxes the stacking lists do
 * not, and the other way round, so it must not be painted, hit-tested or
 * scrolled by moving pixels until the update. */
bool au_layout_settled(const AuLayout *L);
/* Whether the viewport's overflow (the root's, or the body's) is hidden
 * or clip across and down: the user does not scroll the page along such
 * an axis; script still does. */
void au_layout_viewport_overflow(const AuLayout *L, bool *lock_x, bool *lock_y);
/* What changed on screen since the last call, whoever ran the updates:
 * -1 for everything (a build, a layout, a new paint order, or a change
 * whose place depends on scrolling or a transform), else n rectangles in
 * document coordinates (at most max), the paint extents of the boxes a
 * restyle changed for paint only (colours, opacity, outlines that keep
 * their width). 0 when a restyle computed what the boxes had. Changes
 * that invalidate on their own (images, canvases, form controls, the
 * caret, selection) are not counted here. The damage is then cleared. */
#define AU_LAYOUT_DAMAGE 16 /* the most rectangles it keeps */
int au_layout_take_damage(AuLayout *L, AuRect *out, int max);

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

/* The content box of el's box in document coordinates (a block-level or
 * replaced element; false for an inline element or one without a box). */
bool au_layout_element_content_rect(AuLayout *L, const AuElement *el, AuRect *out);
/* The canvas background is left unpainted (a nested document shows its
 * parent through, unless its own root or body paints a background). */
void au_layout_set_transparent(AuLayout *L, bool on);
/* The colour scheme (prefers-color-scheme) the media conditions of
 * <picture> sources and sizes attributes are matched against; a change
 * builds the tree again when a choice depended on one. */
void au_layout_set_dark(AuLayout *L, bool dark);

/* ---- Form fields (controls.c) ----
 * Geometry of text fields (single-line inputs and textareas) for the
 * editing code in src/page. Offsets are byte offsets into the control's
 * value (src/dom/control.h). Each function returns false (or does
 * nothing) when el has no laid-out text field box. */
/* The focused element and whether its caret is shown (blinking); NULL
 * for none. Painting draws the caret of a focused text field. */
void au_layout_set_focus(AuLayout *L, AuElement *el, bool caret_on);
/* The value offset at point (lx, ly) relative to el's border box. */
bool au_layout_field_offset_at(AuLayout *L, AuElement *el, float lx, float ly, uint32_t *off);
/* The caret rectangle for offset off in document coordinates (unscrolled
 * by the viewport: as au_layout_element_rect). */
bool au_layout_field_caret_rect(AuLayout *L, AuElement *el, uint32_t off, AuRect *out);
/* The offset lines visual lines down (negative: up) from off, keeping
 * *goal_x (pass a negative value to start from off's x; it is updated).
 * In a single-line field, up goes to the start and down to the end. */
uint32_t au_layout_field_vmove(AuLayout *L, AuElement *el, uint32_t off, int lines, float *goal_x);
/* The start (dir < 0) or end (dir > 0) of the visual line holding off. */
uint32_t au_layout_field_line_edge(AuLayout *L, AuElement *el, uint32_t off, int dir);
/* Scrolls the field's text so the caret is visible. Returns true when the
 * scroll offset changed. */
bool au_layout_field_reveal(AuLayout *L, AuElement *el);
/* Scrolls a textarea's text by dy pixels; false when it cannot move. */
bool au_layout_field_scroll(AuLayout *L, AuElement *el, float dy);
/* The list box row at point (lx, ly) relative to el's border box (a
 * select with multiple or size > 1), counting option groups; -1 when
 * none. Rows scroll with the control's scroll_y. */
int au_layout_listbox_row_at(AuLayout *L, AuElement *el, float lx, float ly);
/* The number of rows a list box shows. */
int au_layout_listbox_visible(AuLayout *L, AuElement *el);
/* The range slider's value fraction (0..1) at lx, or -1. */
float au_layout_range_frac_at(AuLayout *L, AuElement *el, float lx);

/* Scrolls the innermost scroll container under viewport point (x, y) by
 * (dx, dy), clamped. Returns false when no inner scroller can move (the
 * caller then scrolls the viewport). */
bool au_layout_scroll_at(AuLayout *L, float x, float y, float scroll_x, float scroll_y, float dx, float dy);

/* For script (getBoundingClientRect, IntersectionObserver): el's border
 * box where it is drawn, in document coordinates with the document
 * scrolled to (view_x, view_y): moved by the scroll offsets of the
 * scroll containers it is in, by sticky and fixed positioning, and
 * mapped through transforms (the bounding box). False without a box. */
bool au_layout_element_visual_rect(AuLayout *L, const AuElement *el, float view_x, float view_y, AuRect *out);
/* el's scroll state when its box clips (overflow auto, scroll or
 * hidden): scrollLeft, scrollTop, scrollWidth, scrollHeight and the
 * padding box's width and height (clientWidth, clientHeight). False when
 * el has no such box. */
bool au_layout_element_scroll(AuLayout *L, const AuElement *el, float out[6]);
/* The absolute URL an <img> box asked the image loader for, or NULL (no
 * image box, or no source). */
const char *au_layout_image_request(AuLayout *L, const AuElement *el);

/* CSS animations that repeat forever (au_css_animation_sample): moves
 * them to now_ms on the document's clock and calls fn with each document
 * rectangle to repaint (where each animated box was and now is, with the
 * document scrolled to view_x, view_y). Returns the milliseconds until
 * the next frame, or UINT32_MAX when nothing animates. */
uint32_t au_layout_anim_tick(AuLayout *L, uint32_t now_ms, float view_x, float view_y,
                             void (*fn)(void *ctx, AuIRect r), void *ctx);
bool au_layout_animating(const AuLayout *L);
/* Scrolls el's box to (x, y), clamped, then to a snap position when it
 * has scroll-snap-type (by: the scroll was by a distance, scrollBy, so
 * only positions past the old offset count). True when the offset
 * changed (the caller repaints). */
bool au_layout_element_scroll_to(AuLayout *L, const AuElement *el, float x, float y, bool by);
/* Scroll snapping for the viewport (the root element's
 * scroll-snap-type): moves (*x, *y), where a scroll from (ox, oy) of a
 * vw by vh viewport would end, to a snap position. directional: the
 * scroll was by a distance (wheel, keys, scrollBy). True when it moved
 * them. */
bool au_layout_snap_viewport(AuLayout *L, float vw, float vh, float ox, float oy, float *x, float *y,
                             bool directional);

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
    uint32_t tree_builds;            /* box tree builds, since creation */
    uint32_t layout_passes;          /* layout passes over the document, since creation */
    uint32_t intrinsic_runs;         /* intrinsic size computations (cache misses), since creation */
} AuLayoutStats;
void au_layout_stats(const AuLayout *L, AuLayoutStats *s);
/* Box tree builds and layout passes since the layout was created (for
 * tests and timings: what a change cost). */
void au_layout_pass_counts(const AuLayout *L, uint32_t *builds, uint32_t *layouts);

/* Font metrics for the CSS resolver's ex and ch units
 * (au_css_resolver_set_metrics with ctx = the AuLayout). */
void au_layout_css_metrics(void *layout, const AuStyle *style, float *ex_px, float *ch_px);
/* The content-box size of el's box in the last layout, for the CSS
 * resolver's container queries (au_css_resolver_set_container_size with
 * ctx = the AuLayout); false for an element without a laid-out block or
 * atomic inline box. */
bool au_layout_css_container_size(void *layout, const AuElement *el, float *w, float *h);

#endif
