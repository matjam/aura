/* Aura brotli decoder: internal declarations shared by brotli.c and
 * brotli_tables.c. Not part of the public interface. */
#ifndef AURA_BROTLI_INT_H
#define AURA_BROTLI_INT_H

#include <stdint.h>

/* RFC 7932 section 7.1: the context lookup tables. */
extern const uint8_t au_brotli_lut0[256];
extern const uint8_t au_brotli_lut1[256];
extern const uint8_t au_brotli_lut2[256];

/* RFC 7932 section 8 and Appendix B: dictionary word transforms. */
#define AU_BROTLI_NUM_TRANSFORMS 121
/* Room for the longest prefix (5), word (24) and suffix (8), plus the
 * bytes past the word that the uppercase step may touch. */
#define AU_BROTLI_WORD_BUF 48

/* Writes the transformed word to dst (AU_BROTLI_WORD_BUF bytes) and
 * returns its length. word is len (4..24) bytes; id is below
 * AU_BROTLI_NUM_TRANSFORMS. */
uint32_t au_brotli_transform(uint8_t *dst, const uint8_t *word, uint32_t len, uint32_t id);

#endif
