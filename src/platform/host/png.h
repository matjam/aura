/* PNG output for the host tools, with its own zlib stream writer (stored
 * deflate blocks: no compression, which is all a test image needs). A real
 * inflate, for PNG decoding and HTTP, will be written with networking. */
#ifndef AURA_HOST_PNG_H
#define AURA_HOST_PNG_H

#include "paint/paint.h"

#include "codec/codec.h" /* au_crc32, au_adler32 */

/* Encodes the surface as an 8-bit RGBA PNG (alpha un-premultiplied). */
void au_png_encode(const AuSurface *s, AuBuf *out);
bool au_png_write(const char *path, const AuSurface *s);

/* Reads back a PNG written by au_png_encode: 8-bit RGBA, unfiltered rows,
 * stored deflate blocks only. Returns NULL for anything else. Tests use
 * this for golden images. */
AuSurface *au_png_read_stored(const uint8_t *data, size_t len);

#endif
