/* The image cache of one document: images keyed by absolute URL, shared
 * by every element that uses them, with a memory budget.
 *
 * An entry collects the image's bytes as they arrive and reads only its
 * header (the intrinsic size) until the image is used: decoding is lazy.
 * au_image_entry_use says the image is visible (or near the viewport) at
 * a display size; the first use decodes it, at the smallest power-of-two
 * reduction that is still at least the display size (JPEG decodes
 * directly at that size through its reduced IDCTs), and later uses at a
 * larger size decode it again larger. Data that arrives while an image is
 * in use is decoded as it arrives, so large images fill in.
 *
 * Memory. Each image's decoded size is capped (AU_IMAGE_CACHE_MAX_IMAGE,
 * or half the budget when that is smaller); larger images are decoded
 * scaled down instead of failing. When the decoded images exceed the
 * budget, those not used since the last au_image_cache_frame_begin are
 * evicted, least recently used first: their pixels are freed and their
 * compressed bytes kept, so they decode again when next used. Images in
 * use are never evicted. A still image whose compressed bytes are larger
 * than its full-size pixels (BMP, tiny PNGs) keeps the pixels and drops
 * the bytes instead, and is then not evicted.
 *
 * Surfaces. au_image_entry_surface returns a surface whose address stays
 * the same for the life of the entry (layout keeps it): its pixels and
 * size change as the image decodes, is decoded again at another size,
 * animates or is evicted. Until there are pixels it is one transparent
 * pixel, so painting it draws nothing.
 *
 * Animation. Animated images (GIF, APNG, WebP) advance in
 * au_image_cache_tick, frame by frame at their delays (with the
 * browsers' minimum), looping as their loop count says, and only while
 * in use: an image not used in the last frame stops where it is and
 * resumes when used again. Frames are decoded from the compressed bytes
 * each time round; they are not kept.
 *
 * Portable C99, no OS calls, no global mutable state. */
#ifndef AURA_IMAGE_CACHE_H
#define AURA_IMAGE_CACHE_H

#include "image.h"

#define AU_IMAGE_CACHE_DEFAULT_BUDGET ((size_t)8 * 1024 * 1024)
#define AU_IMAGE_CACHE_MAX_IMAGE ((size_t)16 * 1024 * 1024)

typedef enum {
    AU_IMAGE_PENDING, /* loading; size not known yet */
    AU_IMAGE_SIZED,   /* size known; pixels not decoded (or evicted) */
    AU_IMAGE_READY,   /* pixels available (perhaps still filling in) */
    AU_IMAGE_FAILED   /* the load failed or the data is not an image */
} AuImageState;

typedef struct AuImageCache AuImageCache;
typedef struct AuImageEntry AuImageEntry;

typedef struct AuImageCacheStats {
    uint32_t entries, decoded, animating;
    size_t decoded_bytes;    /* canvases */
    size_t decoder_bytes;    /* decoders alive (images filling in or animating) */
    size_t compressed_bytes; /* bytes kept for decoding again */
    size_t budget;
    uint32_t decodes, evictions; /* since creation */
    uint32_t decode_ms;          /* total time spent decoding, when a clock is set */
} AuImageCacheStats;

AuImageCache *au_image_cache_new(size_t budget);
void au_image_cache_free(AuImageCache *c);
void au_image_cache_set_budget(AuImageCache *c, size_t budget);
/* An optional millisecond clock, for decode timings in the stats. */
void au_image_cache_set_clock(AuImageCache *c, uint32_t (*now_ms)(void *ctx), void *ctx);
/* The fonts text in SVG images is drawn with (NULL: none). The set must
 * outlive the cache or be replaced first. */
void au_image_cache_set_fonts(AuImageCache *c, struct AuFontSet *fonts);

AuImageEntry *au_image_cache_find(AuImageCache *c, const char *url);
AuImageEntry *au_image_cache_add(AuImageCache *c, const char *url); /* or the existing one */
const char *au_image_entry_url(const AuImageEntry *e);

/* Bytes of the image as they arrive, then the end of the load. */
void au_image_entry_data(AuImageCache *c, AuImageEntry *e, const void *data, size_t len);
void au_image_entry_done(AuImageCache *c, AuImageEntry *e, bool ok);

AuImageState au_image_entry_state(const AuImageEntry *e);
/* The intrinsic size (after EXIF orientation), when known. */
bool au_image_entry_size(const AuImageEntry *e, int *w, int *h);
const AuSurface *au_image_entry_surface(AuImageEntry *e);
bool au_image_entry_animated(const AuImageEntry *e);

/* Visibility frames: everything used between begin and the next begin
 * counts as visible. */
void au_image_cache_frame_begin(AuImageCache *c);
/* The image is visible or near the viewport, shown at dw x dh pixels (0
 * for unknown: the intrinsic size). Decodes as needed. */
void au_image_entry_use(AuImageCache *c, AuImageEntry *e, int dw, int dh);
/* Evicts down to the budget (also done after each decode). */
void au_image_cache_trim(AuImageCache *c);

/* Advances animations that are due. Returns the milliseconds until the
 * next frame is due, or UINT32_MAX when nothing animates. */
uint32_t au_image_cache_tick(AuImageCache *c, uint32_t now_ms);
/* True (once) when the entry's pixels changed since the last call. */
bool au_image_entry_take_changed(AuImageEntry *e);
/* An entry whose size became known or that failed since the last call:
 * layout must be told (au_layout_images_changed). */
bool au_image_cache_take_layout_changed(AuImageCache *c);

uint32_t au_image_cache_count(const AuImageCache *c);
AuImageEntry *au_image_cache_entry(AuImageCache *c, uint32_t i);
void au_image_cache_stats(const AuImageCache *c, AuImageCacheStats *s);

#endif
