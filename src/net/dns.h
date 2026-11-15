/* The DNS cache: name lookups through the platform's resolver
 * (AuNetOps.resolve_*), with answers remembered and concurrent lookups
 * of one host shared.
 *
 * The platform gives no TTL, so a positive answer is used for
 * AU_DNS_TTL_MS (5 minutes) and a failure for AU_DNS_NEGATIVE_MS (10
 * seconds, so a typing mistake or a short outage is not asked about for
 * every resource of a page). At most AU_DNS_ENTRIES answers are kept; the
 * oldest goes first. au_dns_forget drops a host's answer (a connection to
 * its address failed: the next lookup asks again), au_dns_clear all of
 * them.
 *
 * A lookup: au_dns_start returns a handle (0 when the lookup fails at
 * once); au_dns_poll returns 1 and the address, 0 while pending, or -1;
 * every handle is passed to au_dns_cancel exactly once, finished or not.
 * Lookups of one host that overlap share one platform request.
 *
 * Internal to src/net (http.c owns one per AuNet); times come from
 * AuNetOps.now_ms. */
#ifndef AURA_NET_DNS_H
#define AURA_NET_DNS_H

#include "../platform/platform.h"

#define AU_DNS_TTL_MS 300000u
#define AU_DNS_NEGATIVE_MS 10000u
#define AU_DNS_ENTRIES 64

typedef struct AuDns AuDns;

typedef struct AuDnsStats {
    unsigned long lookups;  /* au_dns_start calls */
    unsigned long hits;     /* answered from the cache (positive or negative) */
    unsigned long shared;   /* joined a lookup already under way */
    unsigned long requests; /* platform requests started */
    unsigned entries;       /* answers held now */
} AuDnsStats;

/* ops is copied. */
AuDns *au_dns_new(const AuNetOps *ops);
void au_dns_free(AuDns *d);
uint32_t au_dns_start(AuDns *d, const char *host);
int au_dns_poll(AuDns *d, uint32_t handle, AuNetAddr *addr);
void au_dns_cancel(AuDns *d, uint32_t handle);
void au_dns_forget(AuDns *d, const char *host);
void au_dns_clear(AuDns *d);
void au_dns_stats(const AuDns *d, AuDnsStats *out);

#endif
