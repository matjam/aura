/* Message digests (see digest.h). */
#include "digest.h"
#include "../core/hash.h"

#include <psa/crypto.h>
#include <string.h>

size_t au_digest_len(AuDigestAlg alg)
{
    switch (alg) {
    case AU_DIGEST_SHA1: return 20;
    case AU_DIGEST_SHA256: return 32;
    case AU_DIGEST_SHA384: return 48;
    case AU_DIGEST_SHA512: return 64;
    }
    return 0;
}

size_t au_digest_block(AuDigestAlg alg)
{
    return alg == AU_DIGEST_SHA384 || alg == AU_DIGEST_SHA512 ? 128 : au_digest_len(alg) ? 64 : 0;
}

AuDigestAlg au_digest_by_name(const char *name)
{
    static const char *const names[] = {"SHA-1", "SHA-256", "SHA-384", "SHA-512"};
    for (int i = 0; i < 4; i++)
        if (au_ieq(name, strlen(name), names[i], strlen(names[i])))
            return (AuDigestAlg)(i + 1);
    return (AuDigestAlg)0;
}

bool au_digest_parts(AuDigestAlg alg, const void *const *parts, const size_t *lens, int n, uint8_t out[AU_DIGEST_MAX])
{
    if (alg == AU_DIGEST_SHA256) {
        AuSha256 c;
        au_sha256_init(&c);
        for (int i = 0; i < n; i++)
            au_sha256_update(&c, parts[i], lens[i]);
        au_sha256_out(&c, out);
        return true;
    }
    psa_algorithm_t pa = alg == AU_DIGEST_SHA1 ? PSA_ALG_SHA_1 : alg == AU_DIGEST_SHA384 ? PSA_ALG_SHA_384
                         : alg == AU_DIGEST_SHA512 ? PSA_ALG_SHA_512 : 0;
    if (!pa)
        return false;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, pa) != PSA_SUCCESS)
        return false;
    for (int i = 0; i < n; i++)
        if (lens[i] && psa_hash_update(&op, parts[i], lens[i]) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return false;
        }
    size_t got = 0;
    if (psa_hash_finish(&op, out, AU_DIGEST_MAX, &got) != PSA_SUCCESS) {
        psa_hash_abort(&op);
        return false;
    }
    return got == au_digest_len(alg);
}

bool au_digest(AuDigestAlg alg, const void *data, size_t len, uint8_t out[AU_DIGEST_MAX])
{
    const void *parts[1] = {data};
    size_t lens[1] = {len};
    return au_digest_parts(alg, parts, lens, 1, out);
}

bool au_hmac(AuDigestAlg alg, const void *key, size_t key_len, const void *data, size_t len,
             uint8_t out[AU_DIGEST_MAX])
{
    size_t bl = au_digest_block(alg), dl = au_digest_len(alg);
    if (!bl)
        return false;
    uint8_t k[128], pad[128], inner[AU_DIGEST_MAX];
    memset(k, 0, sizeof k);
    if (key_len > bl) {
        if (!au_digest(alg, key, key_len, k))
            return false;
    } else if (key_len) {
        memcpy(k, key, key_len);
    }
    for (size_t i = 0; i < bl; i++)
        pad[i] = k[i] ^ 0x36;
    const void *p1[2] = {pad, data};
    size_t l1[2] = {bl, len};
    if (!au_digest_parts(alg, p1, l1, 2, inner))
        return false;
    for (size_t i = 0; i < bl; i++)
        pad[i] = k[i] ^ 0x5c;
    const void *p2[2] = {pad, inner};
    size_t l2[2] = {bl, dl};
    bool ok = au_digest_parts(alg, p2, l2, 2, out);
    memset(k, 0, sizeof k);
    memset(pad, 0, sizeof pad);
    return ok;
}
