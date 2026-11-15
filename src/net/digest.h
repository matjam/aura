/* Message digests for the engine's script interface (crypto.subtle.digest,
 * HMAC, subresource integrity): SHA-1, SHA-256, SHA-384 and SHA-512.
 *
 * SHA-256 is the engine's own (src/core/hash.c). The others are the TLS
 * library's (TF-PSA-Crypto's PSA hash interface); src/js reaches them only
 * through this file. */
#ifndef AURA_NET_DIGEST_H
#define AURA_NET_DIGEST_H

#include "../core/core.h"

typedef enum { AU_DIGEST_SHA1 = 1, AU_DIGEST_SHA256, AU_DIGEST_SHA384, AU_DIGEST_SHA512 } AuDigestAlg;

#define AU_DIGEST_MAX 64

/* The digest length in bytes (20, 32, 48, 64), the block length HMAC uses
 * (64 or 128); 0 for an unknown algorithm. */
size_t au_digest_len(AuDigestAlg alg);
size_t au_digest_block(AuDigestAlg alg);
/* The algorithm of a Web Crypto name ("SHA-256", any case), or 0. */
AuDigestAlg au_digest_by_name(const char *name);
/* Hashes the parts one after another (n of them) into out. Returns false
 * when the algorithm is unknown or the library refuses. */
bool au_digest_parts(AuDigestAlg alg, const void *const *parts, const size_t *lens, int n,
                     uint8_t out[AU_DIGEST_MAX]);
bool au_digest(AuDigestAlg alg, const void *data, size_t len, uint8_t out[AU_DIGEST_MAX]);
/* HMAC (RFC 2104) with the digest. */
bool au_hmac(AuDigestAlg alg, const void *key, size_t key_len, const void *data, size_t len,
             uint8_t out[AU_DIGEST_MAX]);

#endif
