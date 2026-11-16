/* Aura compression: inflate (RFC 1951, with zlib RFC 1950 and gzip
 * RFC 1952 framing), brotli decoding (RFC 7932) and a deflate encoder
 * (at the end of this file). Portable C99, no OS calls, no global
 * mutable state.
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

/* ---- Deflate (the encoder) ----
 * Compresses to raw deflate, zlib or gzip. For bytes Aura holds but
 * rarely reads (a page's source, a kept style sheet's text): compress
 * when they go cold, inflate on demand.
 *
 * To compress a buffer in one call:
 *
 *     AuBuf z;
 *     au_buf_init(&z);
 *     au_deflate_all(AU_DEFLATE_RAW, 1, text, len, &z);       // appends to z
 *     ...
 *     au_inflate_all(AU_INFLATE_RAW, z.data, z.len, len, &text, &n);
 *
 * To compress a stream, call au_deflate_run as often as there is input
 * or output space, with finish set once no more input will follow the
 * bytes passed; it returns AU_CODEC_OK until the stream is complete and
 * AU_CODEC_DONE then. It never returns AU_CODEC_ERROR: any input is
 * valid, and input and output may be split anywhere, as for the
 * decoders. Before finish, the encoder may hold input back (up to the
 * window and one block): output appears a block at a time.
 *
 * Levels: 0 stores (no compression, 5 bytes per 64 KB); 1 is fast
 * (greedy matching, chains of 8); 2 compresses better and takes about
 * twice as long (lazy matching, chains of 96). Blocks are stored, fixed
 * or dynamic Huffman, whichever is smallest, so incompressible input
 * grows by 5 bytes per 16 KB at most, plus the wrapper (zlib 6 bytes,
 * gzip 18).
 *
 * Memory: about 300 KB per encoder while it lives (64 KB window, two
 * 64 KB hash tables, 48 KB of symbols, the pending block). An encoder
 * can be reused with au_deflate_reset. The gzip header carries no name
 * or time, so equal input gives equal output. */
typedef enum { AU_DEFLATE_RAW, AU_DEFLATE_ZLIB, AU_DEFLATE_GZIP } AuDeflateFormat;

typedef struct AuDeflate AuDeflate;

AuDeflate *au_deflate_new(AuDeflateFormat fmt, int level);
void au_deflate_free(AuDeflate *z);
void au_deflate_reset(AuDeflate *z);
AuCodecStatus au_deflate_run(AuDeflate *z, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len,
                             bool finish);
/* Compresses len bytes at data and appends the result to out. */
void au_deflate_all(AuDeflateFormat fmt, int level, const void *data, size_t len, AuBuf *out);

/* ---- Brotli ---- see brotli.h */

#endif
