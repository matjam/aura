/* A PNG encoder, for canvas.toDataURL and toBlob: 8-bit RGBA, one filter
 * chosen per row (none, sub or up), deflate with fixed Huffman codes and
 * a 32 KB window. It works a row at a time: memory is two rows, a 64 KB
 * window, a 16 KB hash table and the output. */
#ifndef AURA_PNGENC_H
#define AURA_PNGENC_H

#include "../core/core.h"

/* Writes row y (0..h-1, in order) as w * 4 bytes R, G, B, A, not
 * premultiplied. */
typedef void (*AuPngRowFn)(void *ctx, int y, uint8_t *rgba);

/* Appends a PNG of w x h pixels to out. False (out unchanged) when the
 * size is not positive. */
bool au_png_encode_rgba(int w, int h, AuPngRowFn row, void *ctx, AuBuf *out);

#endif
