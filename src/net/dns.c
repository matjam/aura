/* The DNS cache. See dns.h. */
#include "dns.h"

#include "../core/core.h"

#include <string.h>

typedef struct Answer {
    char *host; /* lower case; NULL: free slot */
    AuNetAddr addr;
    bool failed;
    uint32_t at; /* now_ms when the answer came */
} Answer;

/* A platform request, shared by the waiters for its host. */
typedef struct Lookup {
    char *host;
    uint32_t req; /* the platform's handle; 0 once finished and cancelled */
    int waiters;
    int state; /* 0 pending, 1 answered, -1 failed */
    AuNetAddr addr;
} Lookup;

typedef struct Waiter {
    uint32_t handle; /* 0: free slot */
    Lookup *lookup;  /* NULL when answered from the cache */
    int state;       /* without a lookup: 1 or -1 */
    AuNetAddr addr;
} Waiter;

struct AuDns {
    AuNetOps ops;
    Answer answers[AU_DNS_ENTRIES];
    Lookup **lookups;
    size_t nlookups;
    Waiter *waiters;
    size_t nwaiters;
    uint32_t next_handle;
    AuDnsStats stats;
};

static char *lower_dup(const char *host)
{
    size_t n = strlen(host);
    char *h = au_alloc(n + 1);
    for (size_t i = 0; i <= n; i++)
        h[i] = (char)au_lower((unsigned char)host[i]);
    return h;
}

static bool expired(const AuDns *d, const Answer *a)
{
    uint32_t age = d->ops.now_ms(d->ops.ctx) - a->at;
    return age >= (a->failed ? AU_DNS_NEGATIVE_MS : AU_DNS_TTL_MS);
}

static Answer *find_answer(AuDns *d, const char *host)
{
    for (int i = 0; i < AU_DNS_ENTRIES; i++) {
        Answer *a = &d->answers[i];
        if (!a->host || strcmp(a->host, host) != 0)
            continue;
        if (expired(d, a)) {
            au_free(a->host);
            a->host = NULL;
            return NULL;
        }
        return a;
    }
    return NULL;
}

static void remember(AuDns *d, const char *host, const AuNetAddr *addr, bool failed)
{
    uint32_t now = d->ops.now_ms(d->ops.ctx);
    Answer *slot = NULL;
    for (int i = 0; i < AU_DNS_ENTRIES && !slot; i++)
        if (d->answers[i].host && !strcmp(d->answers[i].host, host))
            slot = &d->answers[i];
    for (int i = 0; i < AU_DNS_ENTRIES && !slot; i++)
        if (!d->answers[i].host || expired(d, &d->answers[i]))
            slot = &d->answers[i];
    if (!slot) {
        /* Full: the oldest answer goes. */
        slot = &d->answers[0];
        for (int i = 1; i < AU_DNS_ENTRIES; i++)
            if (now - d->answers[i].at > now - slot->at)
                slot = &d->answers[i];
    }
    au_free(slot->host);
    slot->host = au_strndup(host, strlen(host));
    slot->failed = failed;
    slot->at = now;
    if (addr)
        slot->addr = *addr;
}

AuDns *au_dns_new(const AuNetOps *ops)
{
    AuDns *d = au_calloc(sizeof *d);
    d->ops = *ops;
    return d;
}

static void lookup_free(AuDns *d, Lookup *l)
{
    if (l->req)
        d->ops.resolve_cancel(d->ops.ctx, l->req);
    for (size_t i = 0; i < d->nlookups; i++) {
        if (d->lookups[i] == l) {
            d->lookups[i] = d->lookups[--d->nlookups];
            break;
        }
    }
    au_free(l->host);
    au_free(l);
}

void au_dns_clear(AuDns *d)
{
    for (int i = 0; i < AU_DNS_ENTRIES; i++) {
        au_free(d->answers[i].host);
        d->answers[i].host = NULL;
    }
}

void au_dns_free(AuDns *d)
{
    if (!d)
        return;
    while (d->nlookups)
        lookup_free(d, d->lookups[0]);
    au_dns_clear(d);
    au_free(d->lookups);
    au_free(d->waiters);
    au_free(d);
}

void au_dns_forget(AuDns *d, const char *host)
{
    char *h = lower_dup(host);
    for (int i = 0; i < AU_DNS_ENTRIES; i++) {
        if (d->answers[i].host && !strcmp(d->answers[i].host, h)) {
            au_free(d->answers[i].host);
            d->answers[i].host = NULL;
        }
    }
    au_free(h);
}

static Waiter *new_waiter(AuDns *d)
{
    Waiter *w = NULL;
    for (size_t i = 0; i < d->nwaiters && !w; i++)
        if (!d->waiters[i].handle)
            w = &d->waiters[i];
    if (!w) {
        d->waiters = au_realloc(d->waiters, (d->nwaiters + 1) * sizeof *d->waiters);
        w = &d->waiters[d->nwaiters++];
    }
    memset(w, 0, sizeof *w);
    if (!++d->next_handle)
        d->next_handle = 1;
    w->handle = d->next_handle;
    return w;
}

static Waiter *find_waiter(AuDns *d, uint32_t handle)
{
    for (size_t i = 0; handle && i < d->nwaiters; i++)
        if (d->waiters[i].handle == handle)
            return &d->waiters[i];
    return NULL;
}

uint32_t au_dns_start(AuDns *d, const char *host)
{
    char *h = lower_dup(host);
    d->stats.lookups++;
    Answer *a = find_answer(d, h);
    if (a) {
        d->stats.hits++;
        Waiter *w = new_waiter(d);
        w->state = a->failed ? -1 : 1;
        w->addr = a->addr;
        au_free(h);
        return w->handle;
    }
    Lookup *l = NULL;
    for (size_t i = 0; i < d->nlookups && !l; i++)
        if (!strcmp(d->lookups[i]->host, h))
            l = d->lookups[i];
    if (l) {
        d->stats.shared++;
        au_free(h);
    } else {
        uint32_t req = d->ops.resolve_start(d->ops.ctx, h);
        if (!req) {
            remember(d, h, NULL, true);
            au_free(h);
            return 0;
        }
        d->stats.requests++;
        l = au_calloc(sizeof *l);
        l->host = h;
        l->req = req;
        d->lookups = au_realloc(d->lookups, (d->nlookups + 1) * sizeof *d->lookups);
        d->lookups[d->nlookups++] = l;
    }
    Waiter *w = new_waiter(d);
    w->lookup = l;
    l->waiters++;
    return w->handle;
}

int au_dns_poll(AuDns *d, uint32_t handle, AuNetAddr *addr)
{
    Waiter *w = find_waiter(d, handle);
    if (!w)
        return -1;
    Lookup *l = w->lookup;
    if (l && l->state == 0) {
        int r = d->ops.resolve_poll(d->ops.ctx, l->req, &l->addr);
        if (r == 0)
            return 0;
        /* Finished: the platform's request ends here, and the answer is
         * remembered for the waiters still to poll and for later lookups. */
        d->ops.resolve_cancel(d->ops.ctx, l->req);
        l->req = 0;
        l->state = r > 0 ? 1 : -1;
        remember(d, l->host, r > 0 ? &l->addr : NULL, r < 0);
    }
    int state = l ? l->state : w->state;
    if (state > 0)
        *addr = l ? l->addr : w->addr;
    return state;
}

void au_dns_cancel(AuDns *d, uint32_t handle)
{
    Waiter *w = find_waiter(d, handle);
    if (!w)
        return;
    Lookup *l = w->lookup;
    w->handle = 0;
    w->lookup = NULL;
    if (l && --l->waiters == 0)
        lookup_free(d, l);
}

void au_dns_stats(const AuDns *d, AuDnsStats *out)
{
    *out = d->stats;
    out->entries = 0;
    for (int i = 0; i < AU_DNS_ENTRIES; i++)
        if (d->answers[i].host && !expired(d, &d->answers[i]))
            out->entries++;
}
