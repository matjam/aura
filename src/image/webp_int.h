/* Internal interface between the WebP container (webp.c) and its two
 * bitstream decoders: VP8 (lossy, webp_vp8.c) and VP8L (lossless,
 * webp_vp8l.c). Not for use outside src/image.
 *
 * Both decoders take one complete bitstream (a chunk payload) and emit
 * rows top to bottom through an AuWebpEmit callback as non-premultiplied
 * 0xAARRGGBB pixels. The callback may change the row (the container
 * applies ALPH alpha to VP8 rows that way). They return NULL on success
 * or a short error message; rows emitted before an error are valid. */
#ifndef AURA_WEBP_INT_H
#define AURA_WEBP_INT_H

#include "image_int.h"

typedef void (*AuWebpEmit)(void *ctx, int y, uint32_t *row);

/* The largest buffer a bitstream decoder may allocate for whole-image
 * pixel data (the VP8L pixel array, the alpha plane): 32 MB, or the
 * canvas cap when that is larger. */
static inline size_t au_webp_pixel_cap(const AuImageDecoder *d)
{
    size_t cap = (size_t)32 * 1024 * 1024;
    return d->opt.max_bytes > cap ? d->opt.max_bytes : cap;
}

/* ---- VP8 (lossy) ----
 * au_webp_vp8_size reads the size from the first 10 bytes of a key frame;
 * false when they are not a valid key frame header. */
bool au_webp_vp8_size(const uint8_t *p, size_t n, int *w, int *h);
/* Decodes a key frame. truncated: data is a cut-off prefix of the chunk
 * (rows that decode completely are still emitted, then an error). */
const char *au_webp_vp8_decode(AuImageDecoder *d, const uint8_t *data, size_t len, bool truncated, AuWebpEmit emit,
                               void *ctx);

/* ---- VP8L (lossless) ----
 * au_webp_vp8l_size reads the 5-byte header; false when invalid. */
bool au_webp_vp8l_size(const uint8_t *p, size_t n, int *w, int *h, bool *alpha);
/* Decodes a VP8L bitstream with its 5-byte header. */
const char *au_webp_vp8l_decode(AuImageDecoder *d, const uint8_t *data, size_t len, AuWebpEmit emit, void *ctx);
/* Decodes a headerless VP8L stream of w x h (ALPH compression method 1):
 * the green channel of each pixel is written to alpha (w * h bytes). */
const char *au_webp_vp8l_decode_alpha(AuImageDecoder *d, const uint8_t *data, size_t len, int w, int h,
                                      uint8_t *alpha);

#endif
