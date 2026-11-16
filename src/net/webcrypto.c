/* Script's crypto.subtle operations over the library's PSA interface (see
 * webcrypto.h). */
#include "webcrypto.h"
#include "../core/core.h"
#include "tls_platform.h"

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#include <string.h>

struct AuWcKey {
    AuWcKind kind;
    bool priv;
    AuWcAlg alg;
    AuDigestAlg hash;
    size_t bits;
    psa_key_id_t id;   /* 0 for a secret key */
    uint8_t *bytes;    /* a secret key's bytes, an X25519 private key's own */
    size_t len;
};

/* A byte for the library to point at when script passes nothing: the
 * library is not asked to read through a null pointer. */
static const uint8_t g_none[1] = {0};
#define NZ(p) ((p) ? (p) : g_none)

bool au_wc_start(const uint8_t *seed, size_t len)
{
    if (au_tls_entropy_ready())
        return true;
    return au_tls_entropy_feed(seed, len);
}

bool au_wc_ready(void) { return au_tls_entropy_ready(); }

static psa_algorithm_t psa_hash(AuDigestAlg h)
{
    switch (h) {
    case AU_DIGEST_SHA1: return PSA_ALG_SHA_1;
    case AU_DIGEST_SHA256: return PSA_ALG_SHA_256;
    case AU_DIGEST_SHA384: return PSA_ALG_SHA_384;
    case AU_DIGEST_SHA512: return PSA_ALG_SHA_512;
    }
    return 0;
}

static AuWcErr err_of(psa_status_t s, AuWcErr otherwise)
{
    if (s == PSA_SUCCESS)
        return AU_WC_OK;
    if (s == PSA_ERROR_NOT_SUPPORTED)
        return AU_WC_ENOTSUP;
    if (s == PSA_ERROR_INSUFFICIENT_MEMORY)
        return AU_WC_ENOMEM;
    return otherwise;
}

void au_wc_wipe_free(void *p, size_t len)
{
    if (!p)
        return;
    mbedtls_platform_zeroize(p, len);
    au_free(p);
}

static AuWcKey *key_new(AuWcKind kind)
{
    AuWcKey *k = au_calloc(sizeof *k);
    k->kind = kind;
    return k;
}

AuWcKey *au_wc_secret(const uint8_t *bytes, size_t len)
{
    if (len > AU_WC_SECRET_MAX)
        return NULL;
    AuWcKey *k = key_new(AU_WC_SECRET);
    k->bytes = au_alloc(len ? len : 1);
    if (len)
        memcpy(k->bytes, bytes, len);
    k->len = len;
    k->bits = len * 8;
    k->priv = true;
    return k;
}

const uint8_t *au_wc_key_bytes(const AuWcKey *k, size_t *len)
{
    *len = k->bytes ? k->len : 0;
    return k->bytes;
}

AuWcKind au_wc_key_kind(const AuWcKey *k) { return k->kind; }
bool au_wc_key_private(const AuWcKey *k) { return k->priv; }
size_t au_wc_key_bits(const AuWcKey *k) { return k->bits; }

void au_wc_key_free(AuWcKey *k)
{
    if (!k)
        return;
    if (k->id)
        psa_destroy_key(k->id);
    au_wc_wipe_free(k->bytes, k->len ? k->len : 1);
    mbedtls_platform_zeroize(k, sizeof *k);
    au_free(k);
}

/* ---- Asymmetric keys ---- */

/* The key store's type and policy for a key. */
static bool attributes(AuWcKind kind, bool priv, size_t bits, AuWcAlg alg, AuDigestAlg hash,
                       psa_key_attributes_t *a)
{
    *a = psa_key_attributes_init();
    psa_algorithm_t h = psa_hash(hash);
    psa_key_usage_t use = PSA_KEY_USAGE_EXPORT;
    switch (kind) {
    case AU_WC_RSA:
        psa_set_key_type(a, priv ? PSA_KEY_TYPE_RSA_KEY_PAIR : PSA_KEY_TYPE_RSA_PUBLIC_KEY);
        if (!h)
            return false;
        if (alg == AU_WC_RSASSA) {
            psa_set_key_algorithm(a, PSA_ALG_RSA_PKCS1V15_SIGN(h));
            use |= PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE;
        } else if (alg == AU_WC_PSS) {
            psa_set_key_algorithm(a, PSA_ALG_RSA_PSS(h));
            psa_set_key_enrollment_algorithm(a, PSA_ALG_RSA_PSS_ANY_SALT(h));
            use |= PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE;
        } else if (alg == AU_WC_OAEP) {
            psa_set_key_algorithm(a, PSA_ALG_RSA_OAEP(h));
            use |= PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT;
        } else {
            return false;
        }
        break;
    case AU_WC_EC:
        if (bits != 256 && bits != 384 && bits != 521)
            return false;
        psa_set_key_type(a, priv ? PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)
                                 : PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
        if (alg == AU_WC_ECDSA) {
            psa_set_key_algorithm(a, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
            use |= PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE;
        } else if (alg == AU_WC_ECDH) {
            psa_set_key_algorithm(a, PSA_ALG_ECDH);
            use |= PSA_KEY_USAGE_DERIVE;
        } else {
            return false;
        }
        break;
    case AU_WC_X25519:
        if (alg != AU_WC_ECDH)
            return false;
        bits = 255;
        psa_set_key_type(a, priv ? PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY)
                                 : PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_MONTGOMERY));
        psa_set_key_algorithm(a, PSA_ALG_ECDH);
        use |= PSA_KEY_USAGE_DERIVE;
        break;
    default:
        return false;
    }
    if (bits)
        psa_set_key_bits(a, bits);
    psa_set_key_usage_flags(a, use);
    return true;
}

static AuWcKey *asym_key(AuWcKind kind, bool priv, AuWcAlg alg, AuDigestAlg hash, psa_key_id_t id)
{
    AuWcKey *k = key_new(kind);
    k->priv = priv;
    k->alg = alg;
    k->hash = hash;
    k->id = id;
    psa_key_attributes_t a = psa_key_attributes_init();
    if (psa_get_key_attributes(id, &a) == PSA_SUCCESS)
        k->bits = psa_get_key_bits(&a);
    psa_reset_key_attributes(&a);
    return k;
}

AuWcErr au_wc_import(AuWcKind kind, bool priv, int curve_bits, AuWcAlg alg, AuDigestAlg hash, const uint8_t *data,
                     size_t len, AuWcKey **out)
{
    *out = NULL;
    if (!au_wc_ready())
        return AU_WC_EOP;
    size_t bits = 0;
    if (kind == AU_WC_EC) {
        size_t cb = ((size_t)curve_bits + 7) / 8;
        if (curve_bits != 256 && curve_bits != 384 && curve_bits != 521)
            return AU_WC_ENOTSUP;
        if (priv ? len != cb : len != 2 * cb + 1 || data[0] != 4)
            return AU_WC_EDATA;
        bits = (size_t)curve_bits;
    } else if (kind == AU_WC_X25519) {
        if (len != 32)
            return AU_WC_EDATA;
    } else if (kind == AU_WC_RSA) {
        /* DER of a 4096-bit private key is under 2,400 bytes */
        if (len == 0 || len > 4096)
            return AU_WC_EDATA;
    } else {
        return AU_WC_ENOTSUP;
    }
    psa_key_attributes_t a;
    if (!attributes(kind, priv, bits, alg, hash, &a))
        return AU_WC_ENOTSUP;
    psa_key_id_t id = 0;
    psa_status_t s = psa_import_key(&a, data, len, &id);
    psa_reset_key_attributes(&a);
    if (s != PSA_SUCCESS)
        return err_of(s, AU_WC_EDATA);
    AuWcKey *k = asym_key(kind, priv, alg, hash, id);
    if (kind == AU_WC_X25519 && priv) {
        /* The library keeps the key clamped; export gives back what came in. */
        k->bytes = au_alloc(32);
        memcpy(k->bytes, data, 32);
        k->len = 32;
    }
    *out = k;
    return AU_WC_OK;
}

AuWcErr au_wc_generate(AuWcKind kind, int bits, uint32_t e, AuWcAlg alg, AuDigestAlg hash, AuWcKey **out)
{
    *out = NULL;
    if (!au_wc_ready())
        return AU_WC_EOP;
    if (kind == AU_WC_RSA && (bits < AU_WC_RSA_GEN_MIN_BITS || bits > AU_WC_RSA_MAX_BITS || bits % 8 ||
                              e < 3 || !(e & 1) || e > 0x7fffffff))
        return AU_WC_EOP;
    psa_key_attributes_t a;
    if (!attributes(kind, true, kind == AU_WC_X25519 ? 255 : (size_t)bits, alg, hash, &a))
        return AU_WC_ENOTSUP;
    psa_key_id_t id = 0;
    psa_status_t s;
    if (kind == AU_WC_RSA) {
        uint8_t eb[4] = {(uint8_t)(e >> 24), (uint8_t)(e >> 16), (uint8_t)(e >> 8), (uint8_t)e};
        size_t skip = 0;
        while (skip < 3 && eb[skip] == 0)
            skip++;
        psa_set_key_bits(&a, (size_t)bits);
        psa_custom_key_parameters_t custom = PSA_CUSTOM_KEY_PARAMETERS_INIT;
        s = psa_generate_key_custom(&a, &custom, eb + skip, 4 - skip, &id);
    } else {
        s = psa_generate_key(&a, &id);
    }
    psa_reset_key_attributes(&a);
    if (s != PSA_SUCCESS)
        return err_of(s, AU_WC_EOP);
    *out = asym_key(kind, true, alg, hash, id);
    return AU_WC_OK;
}

uint8_t *au_wc_export(const AuWcKey *k, bool public_part, size_t *len)
{
    *len = 0;
    if (!k->id)
        return NULL;
    psa_key_attributes_t a = psa_key_attributes_init();
    if (psa_get_key_attributes(k->id, &a) != PSA_SUCCESS)
        return NULL;
    psa_key_type_t type = psa_get_key_type(&a);
    size_t bits = psa_get_key_bits(&a);
    psa_reset_key_attributes(&a);
    bool pub = public_part || !k->priv;
    size_t cap = pub ? PSA_EXPORT_PUBLIC_KEY_OUTPUT_SIZE(type, bits) : PSA_EXPORT_KEY_OUTPUT_SIZE(type, bits);
    if (!cap)
        return NULL;
    uint8_t *buf = au_alloc(cap);
    size_t got = 0;
    psa_status_t s = pub ? psa_export_public_key(k->id, buf, cap, &got) : psa_export_key(k->id, buf, cap, &got);
    if (s != PSA_SUCCESS || got > cap) {
        au_wc_wipe_free(buf, cap);
        return NULL;
    }
    *len = got;
    return buf;
}

size_t au_wc_pss_salt(const AuWcKey *k, AuDigestAlg hash)
{
    size_t klen = (k->bits + 7) / 8, hlen = au_digest_len(hash);
    if (klen >= 2 * hlen + 2)
        return hlen;
    return klen > hlen + 2 ? klen - hlen - 2 : 0;
}

size_t au_wc_sig_len(const AuWcKey *k)
{
    if (k->kind == AU_WC_RSA)
        return (k->bits + 7) / 8;
    if (k->kind == AU_WC_EC)
        return 2 * ((k->bits + 7) / 8);
    return 0;
}

static psa_algorithm_t sig_alg(const AuWcKey *k, AuWcAlg alg, AuDigestAlg hash, bool any_salt)
{
    psa_algorithm_t h = psa_hash(hash);
    if (!h || alg != k->alg)
        return 0;
    if (k->kind == AU_WC_RSA && hash != k->hash)
        return 0;
    switch (alg) {
    case AU_WC_RSASSA: return PSA_ALG_RSA_PKCS1V15_SIGN(h);
    case AU_WC_PSS: return any_salt ? PSA_ALG_RSA_PSS_ANY_SALT(h) : PSA_ALG_RSA_PSS(h);
    case AU_WC_ECDSA: return PSA_ALG_ECDSA(h);
    default: return 0;
    }
}

AuWcErr au_wc_sign(const AuWcKey *k, AuWcAlg alg, AuDigestAlg hash, const uint8_t *data, size_t len, uint8_t *sig,
                   size_t cap, size_t *sig_len)
{
    *sig_len = 0;
    psa_algorithm_t pa = sig_alg(k, alg, hash, false);
    if (!pa || !k->priv)
        return AU_WC_EOP;
    if (cap < au_wc_sig_len(k))
        return AU_WC_EOP;
    psa_status_t s = psa_sign_message(k->id, pa, NZ(data), len, sig, cap, sig_len);
    if (s == PSA_SUCCESS && *sig_len > cap)
        s = PSA_ERROR_CORRUPTION_DETECTED;
    return err_of(s, AU_WC_EOP);
}

AuWcErr au_wc_verify(const AuWcKey *k, AuWcAlg alg, AuDigestAlg hash, bool any_salt, const uint8_t *data, size_t len,
                     const uint8_t *sig, size_t sig_len, bool *ok)
{
    *ok = false;
    psa_algorithm_t pa = sig_alg(k, alg, hash, any_salt);
    if (!pa)
        return AU_WC_EOP;
    /* A signature of the wrong length is one that does not verify. */
    if (sig_len != au_wc_sig_len(k))
        return AU_WC_OK;
    psa_status_t s = psa_verify_message(k->id, pa, NZ(data), len, sig, sig_len);
    if (s == PSA_SUCCESS) {
        *ok = true;
        return AU_WC_OK;
    }
    if (s == PSA_ERROR_INVALID_SIGNATURE || s == PSA_ERROR_INVALID_ARGUMENT || s == PSA_ERROR_INVALID_PADDING)
        return AU_WC_OK;
    return err_of(s, AU_WC_EOP);
}

AuWcErr au_wc_oaep(const AuWcKey *k, bool decrypt, AuDigestAlg hash, const uint8_t *label, size_t label_len,
                   const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    psa_algorithm_t h = psa_hash(hash);
    if (k->kind != AU_WC_RSA || k->alg != AU_WC_OAEP || hash != k->hash || !h || (decrypt && !k->priv))
        return AU_WC_EOP;
    if (cap < (k->bits + 7) / 8)
        return AU_WC_EOP;
    psa_algorithm_t pa = PSA_ALG_RSA_OAEP(h);
    psa_status_t s = decrypt ? psa_asymmetric_decrypt(k->id, pa, NZ(in), len, NZ(label), label_len, out, cap, out_len)
                             : psa_asymmetric_encrypt(k->id, pa, NZ(in), len, NZ(label), label_len, out, cap, out_len);
    if (s == PSA_SUCCESS && *out_len > cap)
        s = PSA_ERROR_CORRUPTION_DETECTED;
    if (s != PSA_SUCCESS)
        *out_len = 0;
    return err_of(s, AU_WC_EOP);
}

AuWcErr au_wc_agree(const AuWcKey *priv, const AuWcKey *peer, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (!priv->priv || priv->alg != AU_WC_ECDH || peer->alg != AU_WC_ECDH || priv->kind != peer->kind ||
        priv->bits != peer->bits)
        return AU_WC_EOP;
    size_t plen = 0;
    uint8_t *pub = au_wc_export(peer, true, &plen);
    if (!pub)
        return AU_WC_EOP;
    psa_status_t s = psa_raw_key_agreement(PSA_ALG_ECDH, priv->id, pub, plen, out, cap, out_len);
    au_wc_wipe_free(pub, plen);
    if (s == PSA_SUCCESS && *out_len > cap)
        s = PSA_ERROR_CORRUPTION_DETECTED;
    if (s != PSA_SUCCESS)
        *out_len = 0;
    return err_of(s, AU_WC_EOP);
}

/* ---- AES ---- */

/* A key store copy of an AES key for one operation. */
static psa_status_t aes_key(const AuWcKey *k, psa_algorithm_t alg, psa_key_id_t *id)
{
    *id = 0;
    if (k->kind != AU_WC_SECRET || (k->len != 16 && k->len != 24 && k->len != 32))
        return PSA_ERROR_INVALID_ARGUMENT;
    psa_key_attributes_t a = psa_key_attributes_init();
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, k->len * 8);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, alg);
    psa_status_t s = psa_import_key(&a, k->bytes, k->len, id);
    psa_reset_key_attributes(&a);
    return s;
}

static bool gcm_tag_ok(unsigned t) { return t == 4 || t == 8 || (t >= 12 && t <= 16); }

size_t au_wc_aes_out_max(AuWcAes mode, bool decrypt, size_t len, unsigned param)
{
    switch (mode) {
    case AU_WC_GCM:
        if (!gcm_tag_ok(param))
            return 0;
        if (decrypt)
            return len >= param ? len - param + 1 : 0;
        return len <= (size_t)-1 - 32 ? len + param : 0;
    case AU_WC_CBC:
        if (decrypt)
            return len && len % 16 == 0 ? len : 0;
        return len <= (size_t)-1 - 32 ? len - len % 16 + 16 : 0;
    case AU_WC_CTR:
        return len + 1;
    case AU_WC_KW:
        if (len % 8 || len < (decrypt ? 24u : 16u) || len > (size_t)-1 - 16)
            return 0;
        return decrypt ? len - 8 : len + 8;
    }
    return 0;
}

/* The multipart cipher interface over one segment: iv, then all of in. */
static psa_status_t cipher_run(psa_key_id_t id, psa_algorithm_t alg, bool decrypt, const uint8_t *iv, size_t iv_len,
                               const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t a = 0, b = 0;
    psa_status_t s = decrypt ? psa_cipher_decrypt_setup(&op, id, alg) : psa_cipher_encrypt_setup(&op, id, alg);
    if (s == PSA_SUCCESS && iv_len)
        s = psa_cipher_set_iv(&op, iv, iv_len);
    if (s == PSA_SUCCESS && len)
        s = psa_cipher_update(&op, in, len, out, cap, &a);
    if (s == PSA_SUCCESS && a > cap)
        s = PSA_ERROR_CORRUPTION_DETECTED;
    if (s == PSA_SUCCESS)
        s = psa_cipher_finish(&op, out + a, cap - a, &b);
    if (s == PSA_SUCCESS && b > cap - a)
        s = PSA_ERROR_CORRUPTION_DETECTED;
    if (s != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        *out_len = 0;
        return s;
    }
    *out_len = a + b;
    return s;
}

/* AES-CTR with a counter of `bits` low bits (spec: the counter block's
 * rightmost bits count and wrap; the rest stays). The library counts over
 * all 128 bits, so the input is cut where the low bits come round to 0. */
static psa_status_t aes_ctr(psa_key_id_t id, const uint8_t *ctr, unsigned bits, const uint8_t *in, size_t len,
                            uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    uint64_t blocks = ((uint64_t)len + 15) / 16;
    /* The low 64 bits of the counter, and whether bits 64 to bits-1 are all
     * ones (only then can a wrap come within reach of these blocks). */
    uint64_t low = 0;
    for (int i = 8; i < 16; i++)
        low = low << 8 | ctr[i];
    uint64_t until_wrap = 0; /* blocks before the low bits wrap; 0: more than any input */
    if (bits < 64) {
        uint64_t m = ((uint64_t)1 << bits) - 1, span = (uint64_t)1 << bits;
        if (blocks > span)
            return PSA_ERROR_INVALID_ARGUMENT; /* the counter would repeat */
        until_wrap = span - (low & m);
    } else {
        bool ones = true;
        for (unsigned bit = 64; bit < bits && ones; bit++)
            ones = (ctr[15 - bit / 8] >> (bit % 8)) & 1;
        if (ones && low != 0)
            until_wrap = ~low + 1;
    }
    if (until_wrap == 0 || until_wrap >= blocks)
        return cipher_run(id, PSA_ALG_CTR, false, ctr, 16, in, len, out, cap, out_len);
    size_t first = (size_t)until_wrap * 16, a = 0, b = 0;
    psa_status_t s = cipher_run(id, PSA_ALG_CTR, false, ctr, 16, in, first, out, cap, &a);
    if (s != PSA_SUCCESS)
        return s;
    if (a != first)
        return PSA_ERROR_CORRUPTION_DETECTED;
    uint8_t c2[16];
    memcpy(c2, ctr, 16);
    for (unsigned bit = 0; bit < bits; bit++)
        c2[15 - bit / 8] &= (uint8_t)~(1u << (bit % 8));
    s = cipher_run(id, PSA_ALG_CTR, false, c2, 16, in + first, len - first, out + first, cap - first, &b);
    if (s == PSA_SUCCESS)
        *out_len = a + b;
    return s;
}

/* One AES block through an ECB operation that is set up. */
static psa_status_t ecb_block(psa_cipher_operation_t *op, const uint8_t in[16], uint8_t out[16])
{
    uint8_t tmp[32];
    size_t got = 0;
    psa_status_t s = psa_cipher_update(op, in, 16, tmp, sizeof tmp, &got);
    if (s == PSA_SUCCESS && got != 16)
        s = PSA_ERROR_CORRUPTION_DETECTED;
    if (s == PSA_SUCCESS)
        memcpy(out, tmp, 16);
    mbedtls_platform_zeroize(tmp, sizeof tmp);
    return s;
}

static const uint8_t kw_iv[8] = {0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};

/* RFC 3394, section 2.2.1 (wrap) and 2.2.2 (unwrap), in place in out. */
static psa_status_t aes_kw(psa_key_id_t id, bool decrypt, const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                           size_t *out_len)
{
    *out_len = 0;
    if (len % 8 || len < (decrypt ? 24u : 16u) || cap < (decrypt ? len - 8 : len + 8))
        return PSA_ERROR_INVALID_ARGUMENT;
    size_t n = decrypt ? len / 8 - 1 : len / 8;
    uint8_t A[8], B[16];
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    psa_status_t s = decrypt ? psa_cipher_decrypt_setup(&op, id, PSA_ALG_ECB_NO_PADDING)
                             : psa_cipher_encrypt_setup(&op, id, PSA_ALG_ECB_NO_PADDING);
    if (s != PSA_SUCCESS)
        return s;
    uint8_t *R = decrypt ? out : out + 8; /* R[1..n], 8 bytes each */
    if (decrypt) {
        memcpy(A, in, 8);
        memmove(R, in + 8, n * 8);
    } else {
        memcpy(A, kw_iv, 8);
        memmove(R, in, n * 8);
    }
    for (int jj = 0; jj < 6 && s == PSA_SUCCESS; jj++) {
        int j = decrypt ? 5 - jj : jj;
        for (size_t ii = 0; ii < n && s == PSA_SUCCESS; ii++) {
            size_t i = decrypt ? n - 1 - ii : ii; /* 0-based: R[i + 1] */
            uint64_t t = (uint64_t)n * (uint64_t)j + (uint64_t)i + 1;
            if (decrypt) {
                for (int b = 0; b < 8; b++)
                    A[7 - b] ^= (uint8_t)(t >> (8 * b));
            }
            memcpy(B, A, 8);
            memcpy(B + 8, R + 8 * i, 8);
            s = ecb_block(&op, B, B);
            memcpy(A, B, 8);
            memcpy(R + 8 * i, B + 8, 8);
            if (!decrypt) {
                for (int b = 0; b < 8; b++)
                    A[7 - b] ^= (uint8_t)(t >> (8 * b));
            }
        }
    }
    psa_cipher_abort(&op);
    if (s == PSA_SUCCESS) {
        if (decrypt) {
            uint8_t diff = 0;
            for (int b = 0; b < 8; b++)
                diff |= (uint8_t)(A[b] ^ kw_iv[b]);
            if (diff)
                s = PSA_ERROR_INVALID_SIGNATURE;
        } else {
            memcpy(out, A, 8);
        }
    }
    mbedtls_platform_zeroize(A, sizeof A);
    mbedtls_platform_zeroize(B, sizeof B);
    if (s != PSA_SUCCESS) {
        mbedtls_platform_zeroize(out, decrypt ? len - 8 : len + 8);
        return s;
    }
    *out_len = decrypt ? len - 8 : len + 8;
    return s;
}

AuWcErr au_wc_aes(const AuWcKey *k, AuWcAes mode, bool decrypt, const uint8_t *iv, size_t iv_len, const uint8_t *aad,
                  size_t aad_len, unsigned param, const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                  size_t *out_len)
{
    *out_len = 0;
    size_t need = au_wc_aes_out_max(mode, decrypt, len, param);
    if (!need || cap < need)
        return AU_WC_EOP;
    psa_algorithm_t alg;
    switch (mode) {
    case AU_WC_GCM:
        if (!iv_len)
            return AU_WC_EOP;
        alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, param);
        break;
    case AU_WC_CBC:
        if (iv_len != 16)
            return AU_WC_EOP;
        alg = PSA_ALG_CBC_PKCS7;
        break;
    case AU_WC_CTR:
        if (iv_len != 16 || param < 1 || param > 128)
            return AU_WC_EOP;
        alg = PSA_ALG_CTR;
        break;
    case AU_WC_KW:
        alg = PSA_ALG_ECB_NO_PADDING;
        break;
    default:
        return AU_WC_ENOTSUP;
    }
    if (!au_wc_ready())
        return AU_WC_EOP;
    psa_key_id_t id;
    psa_status_t s = aes_key(k, alg, &id);
    if (s != PSA_SUCCESS)
        return err_of(s, AU_WC_EOP);
    switch (mode) {
    case AU_WC_GCM:
        s = decrypt ? psa_aead_decrypt(id, alg, iv, iv_len, NZ(aad), aad_len, NZ(in), len, out, cap, out_len)
                    : psa_aead_encrypt(id, alg, iv, iv_len, NZ(aad), aad_len, NZ(in), len, out, cap, out_len);
        break;
    case AU_WC_CBC:
        s = cipher_run(id, alg, decrypt, iv, 16, NZ(in), len, out, cap, out_len);
        break;
    case AU_WC_CTR:
        s = aes_ctr(id, iv, param, NZ(in), len, out, cap, out_len);
        break;
    default:
        s = aes_kw(id, decrypt, in, len, out, cap, out_len);
        break;
    }
    psa_destroy_key(id);
    if (s == PSA_SUCCESS && *out_len > cap)
        s = PSA_ERROR_CORRUPTION_DETECTED;
    if (s != PSA_SUCCESS)
        *out_len = 0;
    return err_of(s, AU_WC_EOP);
}

/* ---- Key derivation ---- */

static AuWcErr derive(psa_algorithm_t alg, bool pbkdf2, const AuWcKey *secret, const uint8_t *salt, size_t salt_len,
                      uint32_t iterations, const uint8_t *info, size_t info_len, uint8_t *out, size_t n)
{
    if (secret->kind != AU_WC_SECRET || !au_wc_ready())
        return AU_WC_EOP;
    if (n == 0)
        return AU_WC_OK;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t s = psa_key_derivation_setup(&op, alg);
    if (pbkdf2) {
        if (s == PSA_SUCCESS)
            s = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations);
        if (s == PSA_SUCCESS)
            s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, NZ(salt), salt_len);
        if (s == PSA_SUCCESS)
            s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, NZ(secret->bytes),
                                               secret->len);
    } else {
        if (s == PSA_SUCCESS)
            s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, NZ(salt), salt_len);
        if (s == PSA_SUCCESS)
            s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, NZ(secret->bytes),
                                               secret->len);
        if (s == PSA_SUCCESS)
            s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, NZ(info), info_len);
    }
    if (s == PSA_SUCCESS)
        s = psa_key_derivation_output_bytes(&op, out, n);
    psa_key_derivation_abort(&op);
    if (s != PSA_SUCCESS)
        mbedtls_platform_zeroize(out, n);
    return err_of(s, AU_WC_EOP);
}

AuWcErr au_wc_pbkdf2(AuDigestAlg hash, const AuWcKey *password, const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t *out, size_t n)
{
    psa_algorithm_t h = psa_hash(hash);
    if (!h || iterations == 0)
        return AU_WC_EOP;
    return derive(PSA_ALG_PBKDF2_HMAC(h), true, password, salt, salt_len, iterations, NULL, 0, out, n);
}

AuWcErr au_wc_hkdf(AuDigestAlg hash, const AuWcKey *ikm, const uint8_t *salt, size_t salt_len, const uint8_t *info,
                   size_t info_len, uint8_t *out, size_t n)
{
    psa_algorithm_t h = psa_hash(hash);
    if (!h || n > 255 * au_digest_len(hash))
        return AU_WC_EOP;
    return derive(PSA_ALG_HKDF(h), false, ikm, salt, salt_len, 0, info, info_len, out, n);
}
