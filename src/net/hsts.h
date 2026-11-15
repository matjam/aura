/* HTTP Strict Transport Security (RFC 6797): the known-hosts store, the
 * Strict-Transport-Security header parser, a built-in preload list, and
 * a persistent text format.
 *
 * The stack upgrades http URLs of HSTS hosts to https before connecting
 * (port 80 becomes 443; another explicit port is kept) and refuses to
 * let certificate errors on HSTS hosts be overridden (AuCertInfo.hsts).
 *
 * The preload list compiled in is small: the version 1 acceptance sites
 * and their subdomains (see hsts.c). Chromium's full list (about 150 000
 * entries, several MB as text) would need a compact trie of roughly 1 MB
 * or more; the lookup here takes any sorted list of names, so a larger
 * generated list can replace it later. */
#ifndef AURA_NET_HSTS_H
#define AURA_NET_HSTS_H

#include "net.h"

#define AU_HSTS_MAX_ENTRIES 4096

typedef struct AuHstsEntry {
    char *host;
    int64_t expires; /* seconds since 1970 */
    bool include_subdomains;
} AuHstsEntry;

AuHsts *au_hsts_new(void);
void au_hsts_free(AuHsts *h);
/* Whether host (lower case ASCII domain) is a known HSTS host, through
 * the store or the preload list, at time now. IP addresses never are. */
bool au_hsts_is_known(AuHsts *h, const char *host, int64_t now);
/* Processes a Strict-Transport-Security value received over a secure,
 * error-free connection to host. Returns false when the header is
 * malformed (it is then ignored). max-age=0 removes the host. */
bool au_hsts_process(AuHsts *h, const char *host, const char *value, int64_t now);
/* Parses a header value without storing: max-age in seconds and the
 * includeSubDomains flag. */
bool au_hsts_parse(const char *value, int64_t *max_age, bool *include_subdomains);
void au_hsts_remove(AuHsts *h, const char *host);
size_t au_hsts_count(const AuHsts *h);
const AuHstsEntry *au_hsts_at(const AuHsts *h, size_t i);
/* Whether host is in the built-in preload list (exactly or as a
 * subdomain of an includeSubDomains entry). */
bool au_hsts_preloaded(const char *host);

/* File format: "# Aura HSTS 1", then "host\texpires\tsub" per line
 * (sub is 0 or 1). Expired entries are dropped. */
void au_hsts_serialize(const AuHsts *h, AuBuf *out, int64_t now);
bool au_hsts_parse_file(AuHsts *h, const char *data, size_t len, int64_t now);
bool au_hsts_dirty(const AuHsts *h);
void au_hsts_clear_dirty(AuHsts *h);

#endif
