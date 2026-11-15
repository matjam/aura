/* Internal interface between the generic decoder (image.c) and the
 * format decoders. Not for use outside src/image.
 *
 * A format decoder is an AuImgCodec. image.c sniffs the format, creates
 * the codec's state and calls run() with input as it arrives. run()
 * consumes what it can (advancing *in, decreasing *len) and returns:
 *
 *   AU_IMAGE_MORE   it needs more input (it must have consumed all of
 *                   *in, keeping what it needs in its own state);
 *   AU_IMAGE_FRAME  a frame is complete (au_img_frame_done was called);
 *                   unconsumed input stays in *in;
 *   AU_IMAGE_DONE   the image ended;
 *   AU_IMAGE_ERROR  after au_img_fail.
 *
 * With final set, no more input follows: a codec that cannot finish
 * returns au_img_fail(d, "truncated"). image.c never calls run() again
 * after DONE or ERROR.
 *
 * Output. Once the header is parsed the codec calls au_img_header (size,
 * orientation, alpha), then au_img_begin with the size of the rows it
 * will emit (its coded size after any reduction it does itself, as the
 * JPEG reduced IDCT does) and the remaining reduction that the generic
 * code applies by averaging. au_img_scale tells it the total reduction
 * wanted. Rows are emitted as non-premultiplied 0xAARRGGBB uint32_t,
 * native byte order:
 *
 *   au_img_row        one full row of a still image, top to bottom
 *                     (orientation and averaging apply);
 *   au_img_row_at     a full row out of order (interlaced passes; point
 *                     sampled when reducing), and fill rows below it for
 *                     a progressive preview;
 *   au_img_frame_row  part of a row of an animation frame at (x, y),
 *                     with a blend mode (no orientation; point sampled
 *                     when reducing).
 *
 * The codec owns no canvas: it reads and writes the canvas only through
 * these helpers (and au_img_canvas for disposal snapshots). Every
 * coordinate given to a helper is clipped against the frame, so a codec
 * cannot write outside the canvas through them. */
#ifndef AURA_IMAGE_INT_H
#define AURA_IMAGE_INT_H

#include "image.h"

#include <string.h>

typedef struct AuImgCodec {
    void *(*create)(AuImageDecoder *d);
    AuImageStatus (*run)(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final);
    void (*destroy)(void *st);
} AuImgCodec;

extern const AuImgCodec au_img_png_codec;
extern const AuImgCodec au_img_jpeg_codec;
extern const AuImgCodec au_img_gif_codec;
extern const AuImgCodec au_img_webp_codec;
extern const AuImgCodec au_img_bmp_codec;
extern const AuImgCodec au_img_ico_codec;
extern const AuImgCodec au_img_svg_codec;

enum {
    AU_IMG_BLEND_SRC,  /* replace the canvas pixel */
    AU_IMG_BLEND_OVER, /* source over canvas (alpha) */
    AU_IMG_BLEND_KEY   /* replace, except that alpha 0 leaves the canvas (GIF) */
};

struct AuImageDecoder {
    AuImageOptions opt;
    AuImageInfo info;
    const AuImgCodec *codec;
    void *st;
    bool done, failed, stopped;
    char err[80];
    uint8_t head[16]; /* bytes held while sniffing */
    size_t nhead;

    /* Output. */
    AuSurface *canvas;
    int cw, ch;     /* coded size of the rows the codec emits */
    int red;        /* generic reduction applied to them (1 = none) */
    int orient;     /* 1..8 */
    uint32_t *acc;  /* averaging: out row accumulators, 4 per pixel */
    int acc_n;      /* rows accumulated */
    int next_row;   /* au_img_row: the next coded row expected */
    bool any_dirty;
    AuIRect dirty;
    size_t codec_bytes; /* the codec's own allocations, as it reports them */
};

/* Fails the decode with a message (kept short). Returns AU_IMAGE_ERROR. */
AuImageStatus au_img_fail(AuImageDecoder *d, const char *msg);
/* The header: intrinsic size as coded (before orientation), the EXIF
 * orientation (1..8; 5..8 swap width and height), and whether the image
 * can be transparent. Checks the limits; false after failing. */
bool au_img_header(AuImageDecoder *d, int w, int h, int orient, bool alpha);
/* The total reduction (1, 2, 4 ... 256) wanted for the image whose
 * header was given: the largest power of two that keeps the canvas at
 * least the target size, raised until the canvas fits max_bytes. */
int au_img_scale(const AuImageDecoder *d);
/* Allocates the canvas for rows of coded_w x coded_h that are reduced
 * by red more. false after failing. */
bool au_img_begin(AuImageDecoder *d, int coded_w, int coded_h, int red);
void au_img_row(AuImageDecoder *d, int y, const uint32_t *argb);
/* fill: coded rows y+1 .. y+fill-1 get the same row too (progressive
 * preview of interlaced images); 1 for none. */
void au_img_row_at(AuImageDecoder *d, int y, int fill, const uint32_t *argb);
void au_img_frame_row(AuImageDecoder *d, int x, int y, int w, const uint32_t *argb, int blend);
/* Clears a rectangle (coded coordinates) to transparent: disposal to
 * background. */
void au_img_clear_rect(AuImageDecoder *d, int x, int y, int w, int h);
/* Disposal to previous: saves the canvas pixels under a rectangle (coded
 * coordinates) and puts them back. The saved block is au_alloc'd and
 * freed by restore (or au_free). */
uint32_t *au_img_save_rect(AuImageDecoder *d, int x, int y, int w, int h);
void au_img_restore_rect(AuImageDecoder *d, uint32_t *saved, int x, int y, int w, int h);
/* A frame is complete: delay in ms (0 for still images). Sets the
 * canvas's opaque flag and the frame count. */
void au_img_frame_done(AuImageDecoder *d, uint32_t delay_ms);
void au_img_set_animation(AuImageDecoder *d, bool animated, int loop_count);
/* Memory accounting: the codec reports allocations it keeps. */
void au_img_mem(AuImageDecoder *d, ptrdiff_t delta);

/* Checked size arithmetic: false on overflow. */
static inline bool au_img_mul(size_t a, size_t b, size_t *out)
{
    if (a && b > (size_t)-1 / a)
        return false;
    *out = a * b;
    return true;
}
static inline bool au_img_add(size_t a, size_t b, size_t *out)
{
    if (b > (size_t)-1 - a)
        return false;
    *out = a + b;
    return true;
}

static inline uint32_t au_img_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint32_t au_img_be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t au_img_le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t au_img_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Accumulates bytes until a wanted count is reached: the common shape of
 * "read a fixed-size header from a stream that arrives in pieces". */
typedef struct AuImgHold {
    uint8_t *buf;
    size_t len, cap;
} AuImgHold;
/* Appends from *in until hold has want bytes. true when it has them. */
bool au_img_hold(AuImgHold *h, size_t want, const uint8_t **in, size_t *len);
void au_img_hold_free(AuImgHold *h);

/* Whole-image decoding of an embedded stream (an ICO's PNG entry): a
 * nested decoder that writes into the outer canvas is not supported, so
 * an ICO decodes the entry with its own AuImageDecoder and copies rows. */

#endif
