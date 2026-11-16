/* The cryptography behind script's crypto.subtle (src/js/natives_crypto.c
 * and src/js/subtle.js): AES-GCM, AES-CBC, AES-CTR, AES-KW, RSASSA-PKCS1-v1_5,
 * RSA-PSS, RSA-OAEP, ECDSA, ECDH, X25519, PBKDF2 and HKDF, through the TLS
 * library's PSA interface (TF-PSA-Crypto). src/js reaches the library only
 * through this file and digest.c.
 *
 * Keys arrive here in the forms the library takes: secret bytes, PKCS#1
 * DER for RSA (RSAPrivateKey, RSAPublicKey), the big-endian private
 * scalar or the uncompressed point for P-256, P-384 and P-521, and 32
 * bytes for X25519. Script's formats (spki, pkcs8, jwk) are converted in
 * subtle.js. Every length is the caller's to check against the limits
 * below; the functions here check again and never write past a buffer.
 *
 * Key material is held by the library's key store or in blocks that are
 * zeroed when the key is freed. */
#ifndef AURA_NET_WEBCRYPTO_H
#define AURA_NET_WEBCRYPTO_H

#include "digest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { AU_WC_SECRET = 1, AU_WC_RSA, AU_WC_EC, AU_WC_X25519 } AuWcKind;
/* What an asymmetric key may be used for (its policy in the key store). */
typedef enum { AU_WC_RSASSA = 1, AU_WC_PSS, AU_WC_OAEP, AU_WC_ECDSA, AU_WC_ECDH } AuWcAlg;
typedef enum { AU_WC_GCM = 1, AU_WC_CBC, AU_WC_CTR, AU_WC_KW } AuWcAes;

typedef enum {
    AU_WC_OK = 0,
    AU_WC_EDATA,    /* the key data is malformed (DataError) */
    AU_WC_EOP,      /* the operation failed (OperationError) */
    AU_WC_ENOTSUP,  /* the library cannot do it (NotSupportedError) */
    AU_WC_ENOMEM,   /* the library ran out of memory */
    AU_WC_ECANCEL,  /* a key generation was abandoned through its yield */
} AuWcErr;

/* The most bytes of secret key material a key may hold (an HMAC key or a
 * password; AES keys are 16, 24 or 32). */
#define AU_WC_SECRET_MAX 65536
/* RSA moduli the library takes: 1024 to 4096 bits for generation, up to
 * 4096 for import, in whole bytes. */
#define AU_WC_RSA_MAX_BITS 4096
#define AU_WC_RSA_GEN_MIN_BITS 1024

typedef struct AuWcKey AuWcKey;

/* Starts the library from seed bytes of the platform's entropy source
 * when nothing has started it yet. False when it cannot start. */
bool au_wc_start(const uint8_t *seed, size_t len);
bool au_wc_ready(void);

/* A secret key holding a copy of bytes (at most AU_WC_SECRET_MAX). */
AuWcKey *au_wc_secret(const uint8_t *bytes, size_t len);
/* The secret bytes of a secret key, or the 32 private bytes of an X25519
 * private key as they were given; NULL for other keys. */
const uint8_t *au_wc_key_bytes(const AuWcKey *k, size_t *len);
AuWcKind au_wc_key_kind(const AuWcKey *k);
bool au_wc_key_private(const AuWcKey *k);
/* The modulus or curve size in bits, or the secret's length in bits. */
size_t au_wc_key_bits(const AuWcKey *k);
/* Destroys the key in the library's store and zeroes what it held. */
void au_wc_key_free(AuWcKey *k);

/* Imports an asymmetric key. curve_bits is 256, 384 or 521 for AU_WC_EC
 * and ignored otherwise; hash is the RSA key's hash (0 for EC keys). */
AuWcErr au_wc_import(AuWcKind kind, bool priv, int curve_bits, AuWcAlg alg, AuDigestAlg hash, const uint8_t *data,
                     size_t len, AuWcKey **out);
/* Generates a key pair: bits is the RSA modulus size or the curve size
 * (255 for X25519); e is the RSA public exponent. An RSA generation calls
 * the yield set with au_tls_set_yield between its steps. */
AuWcErr au_wc_generate(AuWcKind kind, int bits, uint32_t e, AuWcAlg alg, AuDigestAlg hash, AuWcKey **out);
/* The key as the library exports it (RSAPrivateKey or RSAPublicKey DER,
 * the private scalar or the uncompressed point, X25519's 32 bytes), or
 * its public part. An au_alloc'd block the caller frees with
 * au_wc_wipe_free; NULL on failure. */
uint8_t *au_wc_export(const AuWcKey *k, bool public_part, size_t *len);
void au_wc_wipe_free(void *p, size_t len);

/* The salt length the library uses for an RSA-PSS signature with the
 * hash and this key (the hash length when the key has room). */
size_t au_wc_pss_salt(const AuWcKey *k, AuDigestAlg hash);
/* The length of a signature by the key: the modulus length, or twice the
 * curve's byte length. */
size_t au_wc_sig_len(const AuWcKey *k);
/* Signs the message (the library hashes it). sig has room for
 * au_wc_sig_len bytes. */
AuWcErr au_wc_sign(const AuWcKey *k, AuWcAlg alg, AuDigestAlg hash, const uint8_t *data, size_t len, uint8_t *sig,
                   size_t cap, size_t *sig_len);
/* Verifies; any_salt accepts an RSA-PSS signature with any salt length. A
 * signature that does not verify is AU_WC_OK with *ok false. */
AuWcErr au_wc_verify(const AuWcKey *k, AuWcAlg alg, AuDigestAlg hash, bool any_salt, const uint8_t *data, size_t len,
                     const uint8_t *sig, size_t sig_len, bool *ok);
/* RSA-OAEP. out has room for the modulus length. */
AuWcErr au_wc_oaep(const AuWcKey *k, bool decrypt, AuDigestAlg hash, const uint8_t *label, size_t label_len,
                   const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);
/* ECDH or X25519 between a private key and a public key of the same
 * curve: the shared secret (the x coordinate, or X25519's 32 bytes). */
AuWcErr au_wc_agree(const AuWcKey *priv, const AuWcKey *peer, uint8_t *out, size_t cap, size_t *out_len);

/* AES with a secret key of 16, 24 or 32 bytes.
 * - GCM: iv of any non-zero length, aad, param the tag length in bytes
 *   (4, 8 or 12 to 16); the output is the ciphertext and the tag.
 * - CBC: iv of 16 bytes, PKCS#7 padding.
 * - CTR: iv is the 16-byte counter block, param the number of its low
 *   bits that count (1 to 128); a counter that would come round to its
 *   first value again fails.
 * - KW: RFC 3394 key wrap (iv and param unused); the input is a multiple
 *   of 8 bytes, at least 16 to wrap and 24 to unwrap.
 * out must have room for au_wc_aes_out_max bytes. */
AuWcErr au_wc_aes(const AuWcKey *k, AuWcAes mode, bool decrypt, const uint8_t *iv, size_t iv_len, const uint8_t *aad,
                  size_t aad_len, unsigned param, const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                  size_t *out_len);
/* The most bytes au_wc_aes may write for an input of len bytes; 0 when the
 * input cannot be valid. */
size_t au_wc_aes_out_max(AuWcAes mode, bool decrypt, size_t len, unsigned param);

/* PBKDF2 with HMAC (the password is a secret key) and HKDF (the input
 * keying material is a secret key): n bytes into out. */
AuWcErr au_wc_pbkdf2(AuDigestAlg hash, const AuWcKey *password, const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t *out, size_t n);
AuWcErr au_wc_hkdf(AuDigestAlg hash, const AuWcKey *ikm, const uint8_t *salt, size_t salt_len, const uint8_t *info,
                   size_t info_len, uint8_t *out, size_t n);

#endif
