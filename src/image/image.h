/* Aura image decoding: PNG (with APNG), JPEG, GIF, WebP, BMP and ICO.
 * Portable C99, no OS calls, no global mutable state.
 *
 * Every format is a streaming state machine behind one interface:
 *
 *     AuImageOptions o = {0};
 *     AuImageDecoder *d = au_image_decoder_new(&o);
 *     for each chunk that arrives:
 *         const uint8_t *p = chunk; size_t n = len;
 *         st = au_image_decode(d, &p, &n, last_chunk);
 *         if (au_image_info(d)->ready) ...size is known...
 *         if (au_image_take_dirty(d, &r)) ...repaint r of au_image_canvas(d)...
 *
 * The format is sniffed from the first bytes, not from a URL or a MIME
 * type. Input may be split at any byte. The decoder writes into a canvas
 * (an AuSurface, premultiplied 0xAARRGGBB) that it owns; rows appear in it
 * as they decode, and au_image_take_dirty reports the area changed since
 * the last call. Undecoded parts of the canvas are transparent.
 *
 * Status:
 *   AU_IMAGE_MORE   give more input (all of *in was consumed).
 *   AU_IMAGE_FRAME  a frame is complete in the canvas. Input after it is
 *                   left in *in; call again (with the rest) to decode the
 *                   next frame of an animation. Each call that returns
 *                   FRAME composites one frame, so the caller paces an
 *                   animation by when it calls.
 *   AU_IMAGE_DONE   the image ended; no more frames.
 *   AU_IMAGE_ERROR  corrupt, truncated (final input ended early),
 *                   unsupported or over a limit; see au_image_error. What
 *                   was decoded before the error stays in the canvas
 *                   (info.partial is set when that is anything).
 *
 * Hostile input. Every size is checked: dimensions are capped
 * (AU_IMAGE_MAX_DIM per side, max_source_pixels in all), every
 * multiplication of sizes is checked for overflow, declared lengths are
 * never trusted, and the canvas never exceeds max_bytes: a larger image is
 * decoded scaled down (JPEG through reduced IDCTs, other formats by
 * averaging rows and columns as they decode) instead of failing.
 *
 * Scaled decoding. target_w and target_h ask for an image at least that
 * large: the decoder reduces by the largest power of two that keeps the
 * canvas at least the target size, which is cheaper to decode (JPEG) and
 * smaller to keep. info.width and info.height stay the intrinsic size;
 * info.out_w and info.out_h are the canvas size.
 *
 * Animation (GIF, APNG, WebP). The canvas holds the composited current
 * frame; frames are composited with their disposal and blending rules.
 * Frames are not kept: to loop, start a new decoder over the same bytes.
 * info.frame_delay_ms is the delay of the last completed frame as the file
 * gives it; au_image_frame_delay applies the browsers' minimum. */
#ifndef AURA_IMAGE_H
#define AURA_IMAGE_H

#include "../core/core.h"
#include "../paint/paint.h"

typedef enum {
    AU_IMAGE_FORMAT_UNKNOWN,
    AU_IMAGE_FORMAT_PNG,
    AU_IMAGE_FORMAT_JPEG,
    AU_IMAGE_FORMAT_GIF,
    AU_IMAGE_FORMAT_WEBP,
    AU_IMAGE_FORMAT_BMP,
    AU_IMAGE_FORMAT_ICO,
    AU_IMAGE_FORMAT_SVG /* see svgimg.c: drawn at the target size, not reduced */
} AuImageFormat;

typedef enum { AU_IMAGE_MORE, AU_IMAGE_FRAME, AU_IMAGE_DONE, AU_IMAGE_ERROR } AuImageStatus;

/* Limits. A side longer than AU_IMAGE_MAX_DIM is refused. */
#define AU_IMAGE_MAX_DIM 32768
#define AU_IMAGE_DEFAULT_MAX_BYTES ((size_t)16 * 1024 * 1024)     /* 4 megapixels of canvas */
#define AU_IMAGE_DEFAULT_MAX_SOURCE_PIXELS ((uint32_t)64 * 1024 * 1024) /* 8192 x 8192 */

typedef struct AuImageOptions {
    int target_w, target_h;     /* 0: the intrinsic size */
    size_t max_bytes;           /* canvas cap; 0: AU_IMAGE_DEFAULT_MAX_BYTES */
    uint32_t max_source_pixels; /* intrinsic width x height cap; 0: the default */
    bool first_frame_only;      /* an animation stops (DONE) after frame 1 */
    bool header_only;           /* stop (DONE) as soon as info.ready */
    struct AuFontSet *fonts;    /* SVG: the fonts for text (NULL: no text) */
} AuImageOptions;

typedef struct AuImageInfo {
    AuImageFormat format;
    bool ready;            /* the header is parsed: width, height, out_w, out_h valid */
    int width, height;     /* intrinsic size, after EXIF orientation */
    int out_w, out_h;      /* canvas size */
    int scale_denom;       /* intrinsic / canvas, a power of two (1 = full size) */
    bool has_alpha;        /* the format can carry transparency for this image */
    bool animated;         /* more than one frame (may become true after frame 1) */
    bool partial;          /* an error came after some rows were decoded */
    int loop_count;        /* animations: 0 = forever, else the number of plays */
    uint32_t frame_count;  /* frames completed so far */
    uint32_t frame_delay_ms; /* delay after the last completed frame, as given */
    int orientation;       /* EXIF orientation applied (1 = none) */
    size_t mem_bytes;      /* the decoder's memory now: state, tables and canvas */
    size_t peak_bytes;     /* the most it has held */
    /* What the image does not state of its size (AU_IMAGE_MISSING_*): 0
     * for raster images. An SVG document may give no width or height,
     * only a viewBox ratio or nothing; width and height above are then
     * its default size (300 x 150, or the ratio at that width). Layout
     * reads it through AuLayoutEnv.image_missing: an <img> with only a
     * ratio fills its containing block, and a CSS background is sized by
     * CSS Images 3's default sizing algorithm. */
    uint8_t missing;
} AuImageInfo;
enum { AU_IMAGE_MISSING_W = 1, AU_IMAGE_MISSING_H = 2, AU_IMAGE_MISSING_RATIO = 4 };

typedef struct AuImageDecoder AuImageDecoder;

AuImageFormat au_image_sniff(const void *data, size_t len); /* UNKNOWN needs more bytes or is not an image */
const char *au_image_format_name(AuImageFormat f);           /* "PNG", "JPEG", ... */

/* opt may be NULL for the defaults. */
AuImageDecoder *au_image_decoder_new(const AuImageOptions *opt);
void au_image_decoder_free(AuImageDecoder *d);
/* final: no input follows what is given now. */
AuImageStatus au_image_decode(AuImageDecoder *d, const uint8_t **in, size_t *in_len, bool final);
const AuImageInfo *au_image_info(const AuImageDecoder *d);
const char *au_image_error(const AuImageDecoder *d); /* "" when none */
/* The canvas, or NULL before the header. Its opaque flag is set when a
 * completed frame has no transparent pixel. The decoder owns it. */
const AuSurface *au_image_canvas(const AuImageDecoder *d);
/* Takes the canvas: the caller owns it and the decoder stops. */
AuSurface *au_image_take_canvas(AuImageDecoder *d);
/* The canvas area changed since the last call, if any. */
bool au_image_take_dirty(AuImageDecoder *d, AuIRect *r);

/* The delay to show a frame for: the browsers' rule plays delays of
 * 10 ms or less at 100 ms. */
static inline uint32_t au_image_frame_delay(uint32_t ms) { return ms <= 10 ? 100 : ms; }

/* Decodes the first frame of a whole buffer. Returns the canvas (the
 * caller frees it with au_surface_free) or NULL; info (optional) is
 * filled in either case. A truncated image with decoded rows is returned
 * with info->partial set. */
AuSurface *au_image_decode_buffer(const void *data, size_t len, const AuImageOptions *opt, AuImageInfo *info);
/* Reads only the header: true when the size is known. */
bool au_image_probe(const void *data, size_t len, AuImageInfo *info);

/* A copy of src (premultiplied) resampled to w x h: area averaging where
 * it shrinks (every source pixel counts, so photos do not alias), linear
 * interpolation where it grows. NULL for an empty size or one over
 * AU_IMAGE_MAX_DIM. The opaque flag carries over. */
AuSurface *au_image_resample(const AuSurface *src, int w, int h);

/* ---- Favicons ----
 * Decodes an icon (ICO with BMP or PNG entries, or any other format) to a
 * size x size bitmap for a tab or a bookmark: the ICO entry closest to
 * size (the smallest at least as large, else the largest), scaled with
 * averaging and centred. Returns NULL for data that is not an image. */
AuSurface *au_image_decode_favicon(const void *data, size_t len, int size);

#endif
