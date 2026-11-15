/* Test certificates and keys (tls_fixtures.c, from gen_tls_fixtures.py). */
#ifndef AURA_TEST_TLS_FIXTURES_H
#define AURA_TEST_TLS_FIXTURES_H

#include <stddef.h>

enum { TLS_FIX_CA, TLS_FIX_OTHER_CA, TLS_FIX_VALID, TLS_FIX_EXPIRED, TLS_FIX_WRONGHOST, TLS_FIX_UNTRUSTED,
       /* Authority Information Access cases (see gen_tls_fixtures.py) */
       TLS_FIX_INTER, TLS_FIX_INTER2, TLS_FIX_AIA_LEAF, TLS_FIX_AIA_LEAF2, TLS_FIX_NOAIA_LEAF,
       TLS_FIX_UNTRUSTED_AIA, TLS_FIX_EXPIRED_AIA, TLS_FIX_WRONGHOST_AIA,
       /* Other key types: an RSA leaf and a P-384 leaf under the EC root,
        * and an RSA root with an RSA leaf. */
       TLS_FIX_VALID_RSA, TLS_FIX_VALID_P384, TLS_FIX_RSA_ROOT, TLS_FIX_RSA_LEAF,
       /* An RSA leaf whose key usage allows key encipherment (RSA key exchange). */
       TLS_FIX_RSA_KEX, TLS_FIX_COUNT };

/* A certificate in DER. */
typedef struct TlsCert {
    const unsigned char *data;
    size_t data_len;
} TlsCert;

/* A private key in DER (SEC1 for EC keys, PKCS#1 for RSA). */
typedef struct TlsKey {
    const unsigned char *der;
    size_t len;
} TlsKey;

typedef struct TlsFixture {
    TlsCert cert;
    TlsKey key;
} TlsFixture;

extern const TlsFixture tls_fixtures[TLS_FIX_COUNT];
/* TLS_FIX_INTER2 in a PKCS#7 certs-only SignedData (DER). */
extern const unsigned char *const tls_fix_inter2_p7;
extern const size_t tls_fix_inter2_p7_len;

#endif
