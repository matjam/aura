/* The root certificate store: trust anchors for TLS, loaded from a
 * compact binary file that can be replaced without rebuilding Aura.
 *
 * The file is made by src/net/gen_roots.py from Mozilla's root set as
 * published by the curl project (cacert.pem). Format (integers big-endian):
 *
 *     "AURR"            magic
 *     u8   version      1
 *     u8   reserved     0
 *     u16  count        number of anchors (at most 1024)
 *     s64  date         the root set's date, seconds since 1970 UTC: the
 *                       clock check refuses system dates before it
 *     count times:
 *       u8   flags      bit 0: CA
 *       u16  dn_len     subject DN (DER), 1..1024 bytes
 *       dn bytes
 *       u8   key_type   1 RSA, 2 EC
 *       RSA: u16 n_len (1..1024), n bytes, u16 e_len (1..16), e bytes
 *       EC:  u8 curve (BearSSL curve id: 23 P-256, 24 P-384, 25 P-521),
 *            u16 q_len (1..133), q bytes (uncompressed point)
 *
 * Anything malformed or out of range makes the whole file invalid. */
#ifndef AURA_NET_ROOTS_H
#define AURA_NET_ROOTS_H

#include "net.h"

#define AU_ROOTS_MAX 1024

/* Parses a root store file. Returns NULL (and a reason in err) when it is
 * malformed. The data is copied. */
AuRootStore *au_roots_load(const void *data, size_t len, char *err, size_t errcap);
/* An empty store with the given date, for adding anchors (tests, or a
 * user-added root later). */
AuRootStore *au_roots_new(int64_t date);
/* Adds a certificate (DER) as a trust anchor (CA). Returns false when it
 * does not decode. */
bool au_roots_add_der(AuRootStore *r, const void *der, size_t len);
void au_roots_free(AuRootStore *r);
int64_t au_roots_date(const AuRootStore *r);
size_t au_roots_count(const AuRootStore *r);
/* BearSSL trust anchors (br_x509_trust_anchor *), for tls.c. */
const void *au_roots_anchors(const AuRootStore *r, size_t *n);

#endif
