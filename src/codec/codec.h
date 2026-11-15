/* Aura decompression: inflate (RFC 1951, with zlib RFC 1950 and gzip
 * RFC 1952 framing) and brotli (RFC 7932). Portable C99, no OS calls, no
 * global mutable state.
 *
 * Every decoder is a streaming state machine with the same calling
 * convention:
 *
 *     AuCodecStatus st = au_inflate_run(z, &in, &in_len, &out, &out_len);
 *
 * It consumes input from *in (advancing *in, decreasing *in_len) and
 * writes output to *out (advancing *out, decreasing *out_len) until it
 * needs more input, has filled the output space, reaches the end of the
 * stream or finds an error. Input may be split at any byte; output space
 * may be any size, including one byte. Memory is bounded: a decoder holds
 * its window and tables and nothing that grows with the stream.
 *
 * Status:
 *   AU_CODEC_OK     progress possible: give more input when *in_len is 0,
 *                   more output space when *out_len is 0.
 *   AU_CODEC_DONE   the stream ended. Bytes after it are left in *in.
 *   AU_CODEC_ERROR  the stream is corrupt (or exceeds a limit); see
 *                   au_codec_error(). The decoder stays in the error state.
 *
 * A caller that has no more input to give and gets AU_CODEC_OK with
 * output space left has a truncated stream. */
#ifndef AURA_CODEC_H
#define AURA_CODEC_H

#include "../core/core.h"

typedef enum { AU_CODEC_OK, AU_CODEC_DONE, AU_CODEC_ERROR } AuCodecStatus;

/* A decoder behind a common interface, for code (HTTP Content-Encoding)
 * that chains decoders without caring which kind each is. */
typedef struct AuDecoder AuDecoder;
struct AuDecoder {
    AuCodecStatus (*run)(AuDecoder *d, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len);
    void (*free)(AuDecoder *d);
    /* A short English description of the last error, or "". */
    const char *(*error)(const AuDecoder *d);
};

static inline AuCodecStatus au_decoder_run(AuDecoder *d, const uint8_t **in, size_t *in_len, uint8_t **out,
                                           size_t *out_len)
{
    return d->run(d, in, in_len, out, out_len);
}
static inline void au_decoder_free(AuDecoder *d)
{
    if (d)
        d->free(d);
}
static inline const char *au_decoder_error(const AuDecoder *d) { return d->error(d); }

/* ---- CRC-32 (ISO 3309, as gzip uses) and Adler-32 ---- */
uint32_t au_codec_crc32(uint32_t crc, const void *data, size_t len); /* start with 0 */
uint32_t au_codec_adler32(uint32_t adler, const void *data, size_t len); /* start with 1 */

/* ---- Inflate ---- */
typedef enum {
    AU_INFLATE_RAW,  /* bare deflate blocks */
    AU_INFLATE_ZLIB, /* RFC 1950 header and Adler-32 trailer */
    AU_INFLATE_GZIP, /* RFC 1952 header and CRC-32/size trailer; members may follow */
    AU_INFLATE_HTTP_DEFLATE /* Content-Encoding: deflate: zlib, or raw when the
                               first two bytes are not a zlib header (servers
                               send both) */
} AuInflateFormat;

typedef struct AuInflate AuInflate;

/* About 45 KB: the 32 KB window plus tables. */
AuInflate *au_inflate_new(AuInflateFormat fmt);
void au_inflate_free(AuInflate *z);
void au_inflate_reset(AuInflate *z);
AuCodecStatus au_inflate_run(AuInflate *z, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len);
const char *au_inflate_error(const AuInflate *z);
/* The decoder as an AuDecoder (freeing it frees the inflater). */
AuDecoder *au_inflate_decoder(AuInflateFormat fmt);

/* Decompresses a whole buffer into a new au_alloc'd buffer of at most
 * max_out bytes. Returns false (and *out NULL) for a corrupt or truncated
 * stream or when the output would exceed max_out. */
bool au_inflate_all(AuInflateFormat fmt, const void *in, size_t in_len, size_t max_out, uint8_t **out,
                    size_t *out_len);

/* ---- Brotli ---- see brotli.h */

#endif
