/* The document view's images: the image cache (src/image/cache.h) and
 * the bookkeeping that ties it to the document and its layout.
 *
 * - Layout looks images up by URL (au_docimages_lookup). Outside painting
 *   a lookup only reports the size (or that the image is still pending
 *   or failed) and the entry's stable surface; it never decodes. During
 *   painting (CSS backgrounds, list-style images, generated content) a
 *   lookup also uses the image, so it decodes when first painted.
 * - Before each paint, au_docimages_prepare finds the <img> and <input
 *   type=image> elements whose boxes are within one viewport of the
 *   visible area and uses their images at their box size: images decode
 *   when they come near the viewport, at the size they are shown.
 * - Images changed by arriving bytes or animation are reported as
 *   rectangles to repaint: the boxes of the elements showing them, or
 *   the whole view for images used only by CSS. */
#ifndef AURA_DOCIMAGES_H
#define AURA_DOCIMAGES_H

#include "../image/cache.h"
#include "../layout/layout.h"

typedef struct AuDocImages AuDocImages;

typedef enum {
    AU_DOCIMG_FAILED,  /* layout shows alt text */
    AU_DOCIMG_PENDING, /* no size yet: a request may be needed (*entry set) */
    AU_DOCIMG_SIZED    /* size and surface set */
} AuDocImageResult;

AuDocImages *au_docimages_new(void);
void au_docimages_free(AuDocImages *m);
/* A new document: forgets every image. */
void au_docimages_clear(AuDocImages *m);
void au_docimages_set_budget(AuDocImages *m, size_t bytes);
void au_docimages_set_clock(AuDocImages *m, uint32_t (*now_ms)(void *ctx), void *ctx);
/* The fonts text in SVG images is drawn with. */
void au_docimages_set_fonts(AuDocImages *m, AuFontSet *fonts);
/* While painting: the image at url is drawn at dw x dh (an SVG image is
 * drawn again at that size when needed). */
void au_docimages_use(AuDocImages *m, const char *url, int dw, int dh);
AuImageCache *au_docimages_cache(AuDocImages *m);

/* *created is set when the URL is new: the caller requests it and feeds
 * the entry through the cache's data/done calls. */
AuDocImageResult au_docimages_lookup(AuDocImages *m, const char *url, int *w, int *h, const AuSurface **pixels,
                                     AuImageEntry **entry, bool *created);

/* Whether the cache holds an image for url. */
bool au_docimages_known(AuDocImages *m, const char *url);

/* The DOM changed: the tracked elements are found again before the next
 * paint. */
void au_docimages_tree_changed(AuDocImages *m);
/* Before painting the document rectangle view (document coordinates). */
void au_docimages_prepare(AuDocImages *m, AuDocument *doc, AuLayout *L, const char *base_url, AuRect view);
void au_docimages_paint_begin(AuDocImages *m);
/* After painting. Images decoded during the paint stay marked changed
 * (au_docimages_invalidate repaints them). */
void au_docimages_paint_end(AuDocImages *m);

/* Advances animations; returns ms until the next frame (UINT32_MAX for
 * none). */
uint32_t au_docimages_tick(AuDocImages *m, uint32_t now_ms);
/* Calls fn with a document rectangle for every image that changed since
 * the last call (doc_all when only CSS shows it). */
void au_docimages_invalidate(AuDocImages *m, AuLayout *L, AuIRect doc_all, void (*fn)(void *ctx, AuIRect r),
                             void *ctx);
/* Whether an image size became known or an image failed: layout must
 * rebuild (au_layout_images_changed). */
bool au_docimages_take_layout_changed(AuDocImages *m);
/* Whether an animation is running (the shell should call idle soon). */
bool au_docimages_animating(const AuDocImages *m);

#endif
