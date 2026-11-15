/* Aura's configuration of TF-PSA-Crypto, the cryptography half of Mbed TLS
 * (third_party/mbedtls/tf-psa-crypto). third_party/mbedtls.cmake passes it
 * as TF_PSA_CRYPTO_CONFIG_FILE. It enables what a TLS 1.2 and 1.3 client
 * with X.509 verification needs and nothing that touches the operating
 * system: memory, the clock and entropy come from src/net/tls_platform.c.
 *
 * AURA_MBEDTLS_SERVER (host builds only) adds what the tests' in-process
 * TLS server needs: private keys and signing. */
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
/* Encryption of the premaster secret for RSA key transport (the patch
 * in third_party/mbedtls-patches). */
#define PSA_WANT_ALG_RSA_PKCS1V15_CRYPT 1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN 1
#define PSA_WANT_ALG_RSA_PSS 1
/* SHA-1 only so that root certificates with a SHA-1 self-signature parse
 * (a third of Mozilla's roots). The verification profile and the
 * signature algorithms tls.c configures refuse SHA-1 everywhere else. */
#define PSA_WANT_ALG_SHA_1 1
#define PSA_WANT_ALG_SHA_256 1
#define PSA_WANT_ALG_SHA_384 1
#define PSA_WANT_ALG_SHA_512 1
#define PSA_WANT_ALG_TLS12_PRF 1

/* Curves: X25519 and P-256 for key exchange; P-256, P-384 and P-521 sign
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
#ifdef AURA_MBEDTLS_SERVER
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_EXPORT 1
#define PSA_WANT_ALG_DETERMINISTIC_ECDSA 1
#endif

/* Platform. The allocator is set at run time (MBEDTLS_PLATFORM_MEMORY);
 * the wall clock, the millisecond clock, the calendar conversion and the
 * entropy source are functions in src/net/tls_platform.c. No files, no
 * threads. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC

/* The random generator: the library's HMAC-DRBG over SHA-256, seeded
 * through mbedtls_platform_get_entropy (tls_platform.c), which hands out
 * what the platform's entropy pool gave the network stack. */
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_PSA_CRYPTO_RNG_HASH PSA_ALG_SHA_256

/* Modules */
#define MBEDTLS_MD_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* Implementation choices. Do not enable
 * MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED: on big-endian PowerPC its X25519
 * gave wrong shared secrets (docs/tls13-feasibility.md). */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_AES_ROM_TABLES

#endif
