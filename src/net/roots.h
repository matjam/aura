/* The root certificate store: trust anchors for TLS, loaded from a binary
 * file that can be replaced without rebuilding Aura.
 *
 * The file is made by src/net/gen_roots.py from Mozilla's root set as
 * published by the curl project (cacert.pem). Format (integers big-endian):
 *
 *     "AURR"            magic
 *     u8   version      2
 *     u8   reserved     0
 *     u16  count        number of anchors (at most AU_ROOTS_MAX)
 *     s64  date         the root set's date, seconds since 1970 UTC: the
 *                       clock check refuses system dates before it
 *     count times:
 *       u16  len        1..AU_ROOT_CERT_MAX
 *       the root certificate, DER
 *
 * Anything malformed or out of range makes the whole file invalid.
 * (Version 1 held only each root's name and public key.)
 *
 * The store keeps the certificates as bytes and an index of their subject
 * names. A certificate is handed to the TLS library, which parses it,
 * only when a chain names it as an issuer (au_roots_next_by_subject), so
 * a handshake parses one or two roots, not all of them.
 *
 * Only root certificates belong here. Never add intermediates: every
 * certificate in the store is trusted to sign for any site. */
#ifndef AURA_NET_ROOTS_H
#define AURA_NET_ROOTS_H

#include "net.h"

#define AU_ROOTS_MAX 1024
#define AU_ROOT_CERT_MAX 8192

/* Parses a root store file. Returns NULL (and a reason in err) when it is
 * malformed. The data is copied. */
AuRootStore *au_roots_load(const void *data, size_t len, char *err, size_t errcap);
/* An empty store with the given date, for adding anchors (tests, or a
 * user-added root later). */
AuRootStore *au_roots_new(int64_t date);
/* Adds a certificate (DER) as a trust anchor. Returns false when its
 * names cannot be read or it is too large. */
bool au_roots_add_der(AuRootStore *r, const void *der, size_t len);
void au_roots_free(AuRootStore *r);
int64_t au_roots_date(const AuRootStore *r);
size_t au_roots_count(const AuRootStore *r);
/* The anchors whose subject Name (a whole DER TLV) equals dn, one per
 * call: pass *pos = 0 first; returns false when there are no more. */
bool au_roots_next_by_subject(const AuRootStore *r, const uint8_t *dn, size_t dn_len, size_t *pos,
                              const uint8_t **der, size_t *der_len);
/* Whether some anchor has this subject Name. */
bool au_roots_has_subject(const AuRootStore *r, const uint8_t *dn, size_t dn_len);

#endif
