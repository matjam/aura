/* The DNS cache (src/net/dns.h) over a scripted resolver: answers are
 * reused for their lifetime, failures for a short one, lookups of one
 * host that overlap share one platform request, every platform request is
 * cancelled exactly once, and the table is bounded. Then the same through
 * the HTTP client on the fake transport. */
#include "fakenet.h"

#include "net/dns.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

/* A resolver that answers when told to. */
typedef struct Res {
    uint32_t now;
    int starts, cancels, open;
    bool ready, fail, refuse;
    char last[64];
} Res;

static uint32_t r_start(void *ctx, const char *host)
{
    Res *r = ctx;
    if (r->refuse)
        return 0;
    r->starts++;
    r->open++;
    snprintf(r->last, sizeof r->last, "%s", host);
    return (uint32_t)r->starts;
}

static int r_poll(void *ctx, uint32_t req, AuNetAddr *a)
{
    Res *r = ctx;
    if (!r->ready)
        return 0;
    if (r->fail)
        return -1;
    memset(a, 0, sizeof *a);
    a->family = 4;
    a->addr[0] = 10;
    a->addr[3] = (uint8_t)req;
    return 1;
}

static void r_cancel(void *ctx, uint32_t req)
{
    Res *r = ctx;
    (void)req;
    r->cancels++;
    r->open--;
}

static uint32_t r_now(void *ctx) { return ((Res *)ctx)->now; }

static void test_cache(void)
{
    Res r;
    memset(&r, 0, sizeof r);
    r.now = 5000;
    AuNetOps ops;
    memset(&ops, 0, sizeof ops);
    ops.ctx = &r;
    ops.resolve_start = r_start;
    ops.resolve_poll = r_poll;
    ops.resolve_cancel = r_cancel;
    ops.now_ms = r_now;
    AuDns *d = au_dns_new(&ops);
    AuNetAddr a, b;

    /* Three lookups of one host while the first is pending: one request. */
    uint32_t h1 = au_dns_start(d, "Example.test");
    uint32_t h2 = au_dns_start(d, "example.test");
    uint32_t h3 = au_dns_start(d, "example.TEST");
    CHECK(h1 && h2 && h3 && h1 != h2 && r.starts == 1 && !strcmp(r.last, "example.test"),
          "overlapping lookups share a request: %d", r.starts);
    CHECK(au_dns_poll(d, h1, &a) == 0 && au_dns_poll(d, h2, &a) == 0, "pending");
    au_dns_cancel(d, h3); /* one waiter leaves; the request goes on */
    CHECK(r.cancels == 0, "the shared request survives a waiter leaving");
    r.ready = true;
    CHECK(au_dns_poll(d, h2, &b) == 1 && au_dns_poll(d, h1, &a) == 1 && !memcmp(&a, &b, sizeof a) && a.addr[3] == 1,
          "both waiters get the answer");
    CHECK(r.cancels == 1 && r.open == 0, "the platform request is cancelled once: %d", r.cancels);
    au_dns_cancel(d, h1);
    au_dns_cancel(d, h2);
    CHECK(r.cancels == 1, "cancelling finished waiters cancels nothing more");

    /* Within the lifetime: no request. After it: a new one. */
    r.now += AU_DNS_TTL_MS - 1;
    uint32_t h = au_dns_start(d, "example.test");
    CHECK(r.starts == 1 && au_dns_poll(d, h, &a) == 1 && a.addr[3] == 1, "answered from the cache");
    au_dns_cancel(d, h);
    r.now += 2;
    h = au_dns_start(d, "example.test");
    CHECK(r.starts == 2 && au_dns_poll(d, h, &a) == 1 && a.addr[3] == 2, "asked again after 5 minutes");
    au_dns_cancel(d, h);

    /* forget drops one host; clear drops all. */
    au_dns_forget(d, "EXAMPLE.test");
    h = au_dns_start(d, "example.test");
    CHECK(r.starts == 3, "forget: asked again");
    au_dns_poll(d, h, &a);
    au_dns_cancel(d, h);
    au_dns_clear(d);
    h = au_dns_start(d, "example.test");
    CHECK(r.starts == 4, "clear: asked again");
    au_dns_poll(d, h, &a);
    au_dns_cancel(d, h);

    /* A failure is remembered for a short time only. */
    r.fail = true;
    h = au_dns_start(d, "nowhere.test");
    CHECK(au_dns_poll(d, h, &a) == -1 && r.starts == 5, "failure");
    au_dns_cancel(d, h);
    h = au_dns_start(d, "nowhere.test");
    CHECK(r.starts == 5 && au_dns_poll(d, h, &a) == -1, "the failure is cached");
    au_dns_cancel(d, h);
    r.fail = false;
    r.now += AU_DNS_NEGATIVE_MS + 1;
    h = au_dns_start(d, "nowhere.test");
    CHECK(r.starts == 6 && au_dns_poll(d, h, &a) == 1, "and asked again after 10 seconds");
    au_dns_cancel(d, h);

    /* A lookup cancelled while pending cancels the platform request and
     * caches nothing. */
    r.ready = false;
    h = au_dns_start(d, "slow.test");
    int c0 = r.cancels;
    au_dns_cancel(d, h);
    CHECK(r.cancels == c0 + 1 && r.open == 0, "a pending lookup is cancelled");
    r.ready = true;
    h = au_dns_start(d, "slow.test");
    CHECK(r.starts == 8, "nothing was cached for it");
    au_dns_poll(d, h, &a);
    au_dns_cancel(d, h);

    /* The table is bounded. */
    for (int i = 0; i < AU_DNS_ENTRIES + 20; i++) {
        char host[32];
        snprintf(host, sizeof host, "h%d.test", i);
        r.now += 10;
        h = au_dns_start(d, host);
        au_dns_poll(d, h, &a);
        au_dns_cancel(d, h);
    }
    AuDnsStats st;
    au_dns_stats(d, &st);
    CHECK(st.entries == AU_DNS_ENTRIES, "at most %d answers: %u", AU_DNS_ENTRIES, st.entries);
    int s0 = r.starts;
    h = au_dns_start(d, "h83.test"); /* the newest is still there */
    au_dns_cancel(d, h);
    h = au_dns_start(d, "h0.test"); /* the oldest went */
    au_dns_poll(d, h, &a);
    au_dns_cancel(d, h);
    CHECK(r.starts == s0 + 1, "the oldest answers go first");

    /* The resolver refusing to start is a failure, briefly cached. */
    r.refuse = true;
    CHECK(au_dns_start(d, "refused.test") == 0, "refused");
    r.refuse = false;
    h = au_dns_start(d, "refused.test");
    CHECK(h && au_dns_poll(d, h, &a) == -1, "cached refusal");
    au_dns_cancel(d, h);

    /* Freeing with a lookup pending cancels it. */
    r.ready = false;
    h = au_dns_start(d, "pending.test");
    (void)h;
    au_dns_free(d);
    CHECK(r.open == 0 && r.cancels == r.starts, "every request cancelled exactly once: %d of %d", r.cancels, r.starts);
}

/* ---- Through the HTTP client ---- */

typedef struct Got {
    int done;
    AuNetError err;
} Got;

static void on_done(void *ctx, AuHttpTransfer *t, AuNetError err)
{
    (void)t;
    Got *g = ctx;
    g->done++;
    g->err = err;
}

static AuNetError fetch(FakeNet *f, AuNet *net, const char *url)
{
    Got g = {0, AU_NET_OK};
    AuHttpRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    AuHttpCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &g;
    cb.done = on_done;
    au_http_start(net, &rq, &cb);
    fake_run(f, net, 2000, 10);
    return g.done ? g.err : AU_NET_ERR_TIMEOUT;
}

static void test_http(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    FakeServer *s = fake_server(&f, "site.test", 80);
    for (int i = 0; i < 4; i++)
        fake_add(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")->close_after = true;
    CHECK(fetch(&f, net, "http://site.test/1") == AU_NET_OK, "first fetch");
    CHECK(fetch(&f, net, "http://site.test/2") == AU_NET_OK, "second fetch");
    AuDnsStats st;
    au_net_dns_stats(net, &st);
    CHECK(s->connections == 2 && st.requests == 1 && st.hits == 1, "two connections, one lookup: %lu requests, %lu hits",
          st.requests, st.hits);
    au_net_clear_dns(net);
    CHECK(fetch(&f, net, "http://site.test/3") == AU_NET_OK, "third fetch");
    au_net_dns_stats(net, &st);
    CHECK(st.requests == 2, "after a clear the host is looked up again: %lu", st.requests);
    /* A refused connection drops the cached address. */
    s->refuse = true;
    CHECK(fetch(&f, net, "http://site.test/4") == AU_NET_ERR_CONNECT, "refused");
    s->refuse = false;
    CHECK(fetch(&f, net, "http://site.test/4") == AU_NET_OK, "fourth fetch");
    au_net_dns_stats(net, &st);
    CHECK(st.requests == 3, "a failed connection makes the next lookup ask again: %lu", st.requests);
    /* A name that does not resolve fails again from the cache. */
    FakeServer *bad = fake_server(&f, "gone.test", 80);
    bad->dns_fail = true;
    CHECK(fetch(&f, net, "http://gone.test/") == AU_NET_ERR_DNS, "no such host");
    CHECK(fetch(&f, net, "http://gone.test/") == AU_NET_ERR_DNS, "again");
    au_net_dns_stats(net, &st);
    CHECK(st.requests == 4, "the failure was cached: %lu", st.requests);
    au_net_free(net);
    fake_free(&f);
}

int main(void)
{
    test_cache();
    test_http();
    printf("dns: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
