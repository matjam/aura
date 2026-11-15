/* stale-while-revalidate (RFC 5861) through the HTTP client on the fake
 * transport: within the window a subresource is served stale at once and
 * revalidated by a transfer of its own; a document waits for the
 * revalidation; one revalidation runs at a time; and the policy in
 * au_cache_lookup. */
#include "fakenet.h"

#include "net/cache.h"

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

typedef struct Got {
    int done;
    AuNetError err;
    bool from_cache, revalidated;
    char body[64];
    int reqs_at_done; /* requests the server had seen when the transfer ended */
    FakeServer *s;
} Got;

static void on_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    (void)t;
    Got *g = ctx;
    g->from_cache = r->from_cache;
    g->revalidated = r->revalidated;
}

static void on_data(void *ctx, AuHttpTransfer *t, const void *data, size_t len)
{
    (void)t;
    Got *g = ctx;
    size_t have = strlen(g->body);
    if (have + len < sizeof g->body) {
        memcpy(g->body + have, data, len);
        g->body[have + len] = 0;
    }
}

static void on_done(void *ctx, AuHttpTransfer *t, AuNetError err)
{
    (void)t;
    Got *g = ctx;
    g->done++;
    g->err = err;
    g->reqs_at_done = g->s->nreq;
}

static void start(AuNet *net, FakeServer *s, const char *url, AuRequestDest dest, Got *g)
{
    memset(g, 0, sizeof *g);
    g->s = s;
    AuHttpRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.dest = dest;
    rq.range_start = rq.range_end = -1;
    AuHttpCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = g;
    cb.response = on_response;
    cb.data = on_data;
    cb.done = on_done;
    au_http_start(net, &rq, &cb);
}

static void fetch(FakeNet *f, AuNet *net, FakeServer *s, const char *url, AuRequestDest dest, Got *g)
{
    start(net, s, url, dest, g);
    fake_run(f, net, 2000, 10);
}

static void test_http(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    AuHttpCache *cache = au_cache_new(1 << 20);
    au_net_set_cache(net, cache);
    FakeServer *s = fake_server(&f, "site.test", 80);
    Got g, g2;

    fake_respond(s, "HTTP/1.1 200 OK\r\nCache-Control: max-age=10, stale-while-revalidate=100\r\nETag: \"v1\"\r\n"
                    "Content-Length: 3\r\n\r\none");
    fetch(&f, net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    CHECK(g.err == AU_NET_OK && !strcmp(g.body, "one") && !g.from_cache && s->nreq == 1, "first fetch");
    f.wall += 5;
    fetch(&f, net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    CHECK(g.from_cache && s->nreq == 1, "fresh: no request");

    /* Stale, within the window: served at once, then revalidated. */
    f.wall += 20;
    fake_respond(s, "HTTP/1.1 200 OK\r\nCache-Control: max-age=10, stale-while-revalidate=100\r\nETag: \"v2\"\r\n"
                    "Content-Length: 3\r\n\r\ntwo");
    start(net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    start(net, s, "http://site.test/s.css", AU_DEST_STYLE, &g2);
    fake_run(&f, net, 2000, 10);
    CHECK(g.err == AU_NET_OK && g.from_cache && !g.revalidated && !strcmp(g.body, "one"), "the stale copy is served: %s",
          g.body);
    CHECK(g.reqs_at_done == 1, "before the revalidation went out (%d requests then)", g.reqs_at_done);
    CHECK(g2.from_cache && !strcmp(g2.body, "one"), "a second request also gets the stale copy");
    CHECK(s->nreq == 2 && strstr(s->requests[1], "If-None-Match: \"v1\"\r\n") != NULL,
          "one conditional request in the background: %d", s->nreq);
    AuCacheStats cs;
    au_cache_stats(cache, &cs);
    CHECK(cs.stale_while_revalidate == 2, "counted: %lu", cs.stale_while_revalidate);
    CHECK(au_net_active(net) == 0, "the background transfer ended");
    fetch(&f, net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    CHECK(g.from_cache && !strcmp(g.body, "two") && s->nreq == 2, "the next request gets the new copy: %s", g.body);

    /* A 304 in the background freshens the entry. */
    f.wall += 20;
    fake_respond(s, "HTTP/1.1 304 Not Modified\r\nCache-Control: max-age=10, stale-while-revalidate=100\r\n"
                    "ETag: \"v2\"\r\n\r\n");
    fetch(&f, net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    CHECK(!strcmp(g.body, "two") && s->nreq == 3, "stale served, 304 in the background");
    fetch(&f, net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    CHECK(g.from_cache && s->nreq == 3, "fresh again after the 304: %d", s->nreq);

    /* Past the window: an ordinary revalidation, waited for. */
    f.wall += 200;
    fake_respond(s, "HTTP/1.1 200 OK\r\nCache-Control: max-age=10, stale-while-revalidate=100\r\nETag: \"v3\"\r\n"
                    "Content-Length: 5\r\n\r\nthree");
    fetch(&f, net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    CHECK(!g.from_cache && !strcmp(g.body, "three") && g.reqs_at_done == 4, "past the window the request waits: %s",
          g.body);

    /* A document within the window waits for the revalidation too. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nCache-Control: max-age=10, stale-while-revalidate=100\r\nETag: \"d1\"\r\n"
                    "Content-Type: text/html\r\nContent-Length: 4\r\n\r\ndoc1");
    fetch(&f, net, s, "http://site.test/page", AU_DEST_DOCUMENT, &g);
    f.wall += 20;
    fake_respond(s, "HTTP/1.1 200 OK\r\nCache-Control: max-age=10, stale-while-revalidate=100\r\nETag: \"d2\"\r\n"
                    "Content-Type: text/html\r\nContent-Length: 4\r\n\r\ndoc2");
    fetch(&f, net, s, "http://site.test/page", AU_DEST_DOCUMENT, &g);
    CHECK(!strcmp(g.body, "doc2") && !g.from_cache, "a document is revalidated first: %s", g.body);

    /* A background revalidation that fails leaves the stale copy usable. */
    f.wall += 20;
    fake_add(s, "")->close_before = true;
    fake_add(s, "")->close_before = true;
    fake_add(s, "")->close_before = true;
    fetch(&f, net, s, "http://site.test/s.css", AU_DEST_STYLE, &g);
    CHECK(g.err == AU_NET_OK && !strcmp(g.body, "three"), "stale served while the server is down");
    CHECK(au_net_active(net) == 0, "the failed revalidation ended");

    au_net_free(net);
    au_cache_free(cache);
    fake_free(&f);
}

/* ---- The policy ---- */

#define T0 1800000000LL

static AuCacheState look(AuHttpCache *c, const char *resp_cc, const char *req_cc, int64_t age)
{
    static int serial;
    char url[64];
    snprintf(url, sizeof url, "http://p.test/%d", serial++);
    AuHeaders req, resp;
    au_headers_init(&req);
    au_headers_init(&resp);
    au_headers_add(&resp, "Cache-Control", resp_cc);
    au_headers_add(&resp, "ETag", "\"x\"");
    AuCacheWriter *w = au_cache_begin(c, url, &req, 200, "OK", &resp, T0, T0);
    if (w) {
        au_cache_write(w, "body", 4);
        au_cache_commit(w);
    }
    if (req_cc)
        au_headers_add(&req, "Cache-Control", req_cc);
    AuCacheEntry *e = NULL;
    AuCacheState st = au_cache_lookup(c, url, &req, T0 + age, &e);
    if (e)
        au_cache_entry_release(c, e);
    au_headers_free(&req);
    au_headers_free(&resp);
    return st;
}

static void test_policy(void)
{
    AuHttpCache *c = au_cache_new(1 << 20);
    CHECK(look(c, "max-age=10, stale-while-revalidate=30", NULL, 5) == AU_CACHE_FRESH, "fresh");
    CHECK(look(c, "max-age=10, stale-while-revalidate=30", NULL, 20) == AU_CACHE_STALE_USABLE, "in the window");
    CHECK(look(c, "max-age=10, stale-while-revalidate=30", NULL, 40) == AU_CACHE_STALE_USABLE, "at its end");
    CHECK(look(c, "max-age=10, stale-while-revalidate=30", NULL, 41) == AU_CACHE_STALE, "past the window");
    CHECK(look(c, "max-age=10", NULL, 20) == AU_CACHE_STALE, "without the directive");
    CHECK(look(c, "max-age=10, stale-while-revalidate=30, must-revalidate", NULL, 20) == AU_CACHE_STALE,
          "must-revalidate forbids it");
    CHECK(look(c, "max-age=10, stale-while-revalidate=30, no-cache", NULL, 20) == AU_CACHE_STALE,
          "no-cache forbids it");
    CHECK(look(c, "max-age=10, stale-while-revalidate=30", "no-cache", 20) == AU_CACHE_STALE,
          "a request with no-cache revalidates");
    CHECK(look(c, "max-age=10, stale-while-revalidate=30", "max-age=0", 20) == AU_CACHE_STALE,
          "a request with max-age revalidates");
    CHECK(look(c, "max-age=10, stale-while-revalidate=junk", NULL, 20) == AU_CACHE_STALE, "a bad value is ignored");
    au_cache_free(c);
}

int main(void)
{
    test_policy();
    test_http();
    printf("swr: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
