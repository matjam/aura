/* SHA-256 and MD5 for the engine's own uses: certificate fingerprints,
 * HTTP Digest authentication, and the Mac OS 9 entropy pool. TLS uses the
 * TLS library's implementations, not these.
 *
 * MD5 is here only because Digest authentication (RFC 7616) still names
 * it; do not use it for anything else. */
#ifndef AURA_CORE_HASH_H
#define AURA_CORE_HASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct AuSha256 {
    uint32_t h[8];
    uint64_t len;    /* bytes hashed */
    uint8_t buf[64]; /* a partial block */
} AuSha256;

void au_sha256_init(AuSha256 *c);
void au_sha256_update(AuSha256 *c, const void *data, size_t len);
/* Writes the 32-byte digest. The context is unchanged, so hashing may
 * continue. */
void au_sha256_out(const AuSha256 *c, uint8_t out[32]);
void au_sha256(const void *data, size_t len, uint8_t out[32]);

typedef struct AuMd5 {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
} AuMd5;

void au_md5_init(AuMd5 *c);
void au_md5_update(AuMd5 *c, const void *data, size_t len);
void au_md5_out(const AuMd5 *c, uint8_t out[16]);

#endif
