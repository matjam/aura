/* Aura brotli decoder (RFC 7932). Portable C99, no OS calls, no global
 * mutable state. Streaming, with the calling convention described in
 * codec.h.
 *
 * The static dictionary (122784 bytes) is not linked in raw. It is kept
 * zlib-compressed in brotli_dict_data.c; au_brotli_dict_new inflates it
 * into a heap block that the caller shares between decoders and frees
 * when no brotli stream is active, so it costs memory only while brotli
 * responses are being decoded.
 *
 * Memory: the decoder allocates its ring buffer at the size the stream
 * needs, at most 2^max_window_bits bytes (the stream's WBITS beyond that
 * is an error), plus Huffman tables bounded by the RFC's limits. */
#ifndef AURA_BROTLI_H
#define AURA_BROTLI_H

#include "codec.h"

#define AU_BROTLI_DICT_SIZE 122784u

/* The default cap on the window: 2^22 = 4 MB, what encoders use by
 * default (lgwin 22). Large-window brotli (RFC 7932 allows up to 24 bits;
 * the non-standard large-window extension more) is refused above this. */
#define AU_BROTLI_DEFAULT_MAX_WINDOW_BITS 22

typedef struct AuBrotliDict AuBrotliDict;

/* Inflates the built-in dictionary and checks its CRC-32. Returns NULL
 * only if the built-in data is damaged. */
AuBrotliDict *au_brotli_dict_new(void);
void au_brotli_dict_free(AuBrotliDict *d);
const uint8_t *au_brotli_dict_bytes(const AuBrotliDict *d); /* AU_BROTLI_DICT_SIZE bytes */

typedef struct AuBrotli AuBrotli;

/* dict may be NULL: a stream that refers to the dictionary is then an
 * error. The dictionary must outlive the decoder. max_window_bits is
 * 10..24; 0 means AU_BROTLI_DEFAULT_MAX_WINDOW_BITS. */
AuBrotli *au_brotli_new(const AuBrotliDict *dict, int max_window_bits);
void au_brotli_free(AuBrotli *b);
AuCodecStatus au_brotli_run(AuBrotli *b, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len);
const char *au_brotli_error(const AuBrotli *b);
/* The decoder as an AuDecoder (freeing it frees the decoder, not dict). */
AuDecoder *au_brotli_decoder(const AuBrotliDict *dict, int max_window_bits);

#endif
