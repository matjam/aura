/* Test certificates and keys (tls_fixtures.c, from gen_tls_fixtures.py). */
#ifndef AURA_TEST_TLS_FIXTURES_H
#define AURA_TEST_TLS_FIXTURES_H

#include "bearssl.h"

#include <stddef.h>

enum { TLS_FIX_CA, TLS_FIX_OTHER_CA, TLS_FIX_VALID, TLS_FIX_EXPIRED, TLS_FIX_WRONGHOST, TLS_FIX_UNTRUSTED,
       /* Authority Information Access cases (see gen_tls_fixtures.py) */
       TLS_FIX_INTER, TLS_FIX_INTER2, TLS_FIX_AIA_LEAF, TLS_FIX_AIA_LEAF2, TLS_FIX_NOAIA_LEAF,
       TLS_FIX_UNTRUSTED_AIA, TLS_FIX_EXPIRED_AIA, TLS_FIX_WRONGHOST_AIA, TLS_FIX_COUNT };

typedef struct TlsFixture {
    br_x509_certificate cert;
    br_ec_private_key key;
} TlsFixture;

extern const TlsFixture tls_fixtures[TLS_FIX_COUNT];
/* TLS_FIX_INTER2 in a PKCS#7 certs-only SignedData (DER). */
extern const unsigned char *const tls_fix_inter2_p7;
extern const size_t tls_fix_inter2_p7_len;

#endif
