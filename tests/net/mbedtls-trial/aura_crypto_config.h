/* TF-PSA-Crypto configuration for the Mbed TLS trial
 * (docs/tls13-feasibility.md): what a TLS 1.2 and 1.3 client needs, no
 * operating system services. Pass -DTF_PSA_CRYPTO_CONFIG_FILE to use it. */
#ifndef AURA_CRYPTO_CONFIG_H
#define AURA_CRYPTO_CONFIG_H

#define TF_PSA_CRYPTO_CONFIG_VERSION 0x01000000

/* Algorithms */
#define PSA_WANT_ALG_CHACHA20_POLY1305 1
#define PSA_WANT_ALG_GCM 1
#define PSA_WANT_ALG_ECDH 1
#define PSA_WANT_ALG_ECDSA 1
#define PSA_WANT_ALG_HKDF_EXTRACT 1
#define PSA_WANT_ALG_HKDF_EXPAND 1
#define PSA_WANT_ALG_HMAC 1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN 1
#define PSA_WANT_ALG_RSA_PSS 1
/* SHA-1 only so that root certificates with a SHA-1 self-signature parse
 * (41 of the 128 roots in the test bundle); the default verification
 * profile refuses SHA-1 signatures in a chain. */
#define PSA_WANT_ALG_SHA_1 1
#define PSA_WANT_ALG_SHA_256 1
#define PSA_WANT_ALG_SHA_384 1
#define PSA_WANT_ALG_SHA_512 1
#define PSA_WANT_ALG_TLS12_PRF 1

/* Curves: X25519 and P-256 for key exchange; P-384 and P-521 also sign
 * certificates. */
#define PSA_WANT_ECC_MONTGOMERY_255 1
#define PSA_WANT_ECC_SECP_R1_256 1
#define PSA_WANT_ECC_SECP_R1_384 1
#define PSA_WANT_ECC_SECP_R1_521 1

/* Key types */
#define PSA_WANT_KEY_TYPE_AES 1
#define PSA_WANT_KEY_TYPE_CHACHA20 1
#define PSA_WANT_KEY_TYPE_DERIVE 1
#define PSA_WANT_KEY_TYPE_HMAC 1
#define PSA_WANT_KEY_TYPE_RAW_DATA 1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1
/* Mbed TLS 4.1's configuration check asks for the RSA key pair type and
 * OAEP wherever ECDHE-RSA and RSA-PSS certificates are enabled, though a
 * client only verifies. */
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC 1
#define PSA_WANT_ALG_RSA_OAEP 1
#ifdef AURA_TRIAL_SERVER /* the test server signs with an RSA key */
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT 1
#endif

/* Platform: Aura's allocator, clock and entropy; no files, no threads. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC

/* Modules */
#define MBEDTLS_MD_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* Implementation choices */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_AES_ROM_TABLES
/* X25519 from Project Everest (Apache-2.0 only) instead of the generic
 * Montgomery ladder over the bignum code. On a 32-bit target it uses its
 * emulated 128-bit integers. */
#ifdef AURA_TRIAL_EVEREST
#define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED
#endif

#endif
