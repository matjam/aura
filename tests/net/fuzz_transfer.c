/* Deterministic fuzzer for the HTTP client as a whole: mutated responses
 * reach it through the fake transport, so response parsing, chunked
 * framing, the content decoders (gzip, deflate, Brotli), redirects,
 * cookies, HSTS, the cache and the loader (sniffing, charset conversion)
 * all see hostile bytes at random packet boundaries.
 *
 * Each iteration builds one to three responses (headers from the seeds
 * below; a body from the corpus, which is the inflate and Brotli vectors
 * given as arguments, framed with Content-Length, chunked or to the
 * close; then the whole mutated), serves them from one fake host and
 * fetches a URL through the HTTP client or the loader. The transfer must
 * finish within the round limit, and the body delivered for one response
 * must stay under BODY_CAP.
 *
 * usage: fuzz_transfer ITERATIONS SEED SECONDS [BODY...]
 *        fuzz_transfer replay FILE... */
#include "../fuzz.h"
#include "fakenet.h"
#include "net/cache.h"
#include "net/cookies.h"
#include "net/hsts.h"
#include "net/loader.h"
#include "net/net.h"

#define BODY_CAP (256u << 20)

static const char *const heads[] = {
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n",
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=windows-1252\r\nCache-Control: max-age=60\r\nETag: \"a\"\r\n",
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Content-Type-Options: nosniff\r\nVary: Accept-Encoding, Cookie\r\n",
    "HTTP/1.0 200 OK\r\nContent-Type: image/png\r\nSet-Cookie: a=b; Path=/; Max-Age=60\r\nSet-Cookie: c=d; Secure\r\n",
    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"x.bin\"\r\n",
    "HTTP/1.1 301 Moved\r\nLocation: /next\r\nSet-Cookie: r=1\r\n",
    "HTTP/1.1 302 Found\r\nLocation: http://b.test/other?x=1\r\n",
    "HTTP/1.1 307 Temporary\r\nLocation: //a.test/again\r\nStrict-Transport-Security: max-age=31536000; includeSubDomains\r\n",
    "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"r\", Digest realm=\"d\", nonce=\"n\", qop=\"auth\"\r\n",
    "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Type: text/css\r\n",
    "HTTP/1.1 304 Not Modified\r\nETag: \"a\"\r\nCache-Control: max-age=1, stale-while-revalidate=60\r\n",
    "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-4/10\r\nAccept-Ranges: bytes\r\n",
    "HTTP/1.1 200 OK\r\nRefresh: 0; url=/r\r\nLink: </s.css>; rel=preload\r\nExpires: Thu, 01 Jan 2099 00:00:00 GMT\r\n"
    "Last-Modified: Wed, 21 Oct 2015 07:28:00 GMT\r\nAge: 5\r\nDate: Sun, 15 Nov 2026 00:00:00 GMT\r\n",
    "HTTP/1.1 204 No Content\r\n",
    "HTTP/1.1 503 Busy\r\nRetry-After: 1\r\nConnection: close\r\n",
};

static const char *const dict[] = {
    "\r\n", "\r\n\r\n", "\n", "\r", "HTTP/1.1 ", "HTTP/1.0 200 OK\r\n", "HTTP/0.9", "HTTP/2 200\r\n", "200", "100", "999", "304",
    "Content-Length: ", "Content-Length: 0\r\n", "Content-Length: 18446744073709551616\r\n", "Content-Length: -1\r\n",
    "Transfer-Encoding: chunked\r\n", "Transfer-Encoding: gzip, chunked\r\n", "Transfer-Encoding: identity\r\n",
    "Content-Encoding: gzip\r\n", "Content-Encoding: br\r\n", "Content-Encoding: deflate\r\n", "Content-Encoding: gzip, br, gzip\r\n",
    "Content-Encoding: zstd\r\n", "Content-Encoding: br, br, br, br, br, br, br, br\r\n", "Connection: close\r\n",
    "Connection: keep-alive\r\n", "Location: ", "Location: http://a.test/\r\n", "Location: https://a.test/\r\n",
    "Set-Cookie: ", "Set-Cookie: x=y; Domain=a.test; Expires=Wed, 09 Jun 2099 10:18:14 GMT\r\n", "Cache-Control: ",
    "no-store", "no-cache", "max-age=", "must-revalidate", "private", "immutable", "Vary: *\r\n", "ETag: W/\"", "Age: ",
    "Content-Type: ", "text/html", "; charset=", "utf-16", "shift_jis", "iso-8859-2", "image/svg+xml", "multipart/x-mixed-replace",
    "Strict-Transport-Security: max-age=0\r\n", "Content-Range: bytes ", "0-", "/*", "\t", " ", ":", ";", ",", "=", "\"",
    "0\r\n\r\n", "1\r\nx\r\n", "ffffffffffffffff\r\n", "FFFFFFF0\r\n", "7fffffff;ext=\"a\"\r\n", "-1\r\n", "5;a=b\r\n",
    "\x1f\x8b\x08\x00", "\x1f\x8b\x08\x1c", "\x78\x9c", "\x00\x00\xff\xff", "<!DOCTYPE html><meta charset=", "\xEF\xBB\xBF",
    "\xFF\xFE", "\x89PNG\r\n\x1a\n", "GIF89a", "<svg", "%PDF-", "\0",
};
#define NDICT ((int)(sizeof dict / sizeof dict[0]))

typedef struct Run {
    bool done;
    size_t body; /* bytes since the last response head */
    int responses;
} Run;

static void fail(const char *what)
{
    printf("fuzz_transfer: %s\n", what);
    abort();
}

static void h_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    Run *run = (Run *)ctx;
    (void)t, (void)r;
    if (run->done)
        fail("a response after done");
    run->responses++;
    run->body = 0;
}

static void h_data(void *ctx, AuHttpTransfer *t, const void *d, size_t n)
{
    Run *run = (Run *)ctx;
    (void)t, (void)d;
    if (run->done)
        fail("data after done");
    run->body += n;
    if (run->body > BODY_CAP)
        fail("a response body over the cap");
}

static void h_done(void *ctx, AuHttpTransfer *t, AuNetError e)
{
    Run *run = (Run *)ctx;
    (void)t, (void)e;
    if (run->done)
        fail("done twice");
    run->done = true;
}

static void l_response(void *ctx, uint32_t id, const AuLoadInfo *info)
{
    Run *run = (Run *)ctx;
    (void)id, (void)info;
    run->responses++;
    run->body = 0;
}

static void l_data(void *ctx, uint32_t id, const void *d, size_t n)
{
    Run *run = (Run *)ctx;
    (void)id, (void)d;
    if (run->done)
        fail("loader data after done");
    run->body += n;
    if (run->body > BODY_CAP)
        fail("a loader body over the cap");
}

static void l_done(void *ctx, uint32_t id, AuNetError e, const AuLoadInfo *info)
{
    Run *run = (Run *)ctx;
    (void)id, (void)e, (void)info;
    if (run->done)
        fail("loader done twice");
    run->done = true;
}

/* An input is up to three responses, each preceded by a four-byte header:
 * 'R', flags (1 close after, 2 reset after, 4 close before), and the
 * packet size (two bytes, 0 for all at once). So a saved input replays
 * as it ran. */
static void run_one(const FzBuf *in)
{
    FakeNet f;
    AuNet *net;
    AuHttpCache *cache = au_cache_new(256 << 10);
    AuCookieJar *jar = au_cookie_jar_new();
    AuHsts *hsts = au_hsts_new();
    FakeServer *s, *b;
    const uint8_t *p = in->d, *end = in->d + in->n;
    int nresp = 0, pass;
    fake_init(&f);
    net = au_net_new(&f.ops);
    au_net_set_cookie_jar(net, jar);
    au_net_set_hsts(net, hsts);
    au_net_set_cache(net, cache);
    s = fake_server(&f, "a.test", 0);
    b = fake_server(&f, "b.test", 0);
    while (p + 4 <= end && nresp < 6) {
        const uint8_t *q = p + 4;
        FakeResponse *r;
        while (q + 4 <= end && !(q[0] == 'R' && q[1] < 8 && memcmp(q + 4, "HTTP", 4) == 0))
            q++;
        if (q + 4 > end)
            q = end;
        r = fake_add(nresp % 3 == 2 ? b : s, (const char *)p + 4);
        r->len = (size_t)(q - (p + 4));
        if (!r->len) {
            r->data = "";
            r->close_before = true;
        }
        r->close_after = (p[1] & 1) != 0;
        r->reset_after = (p[1] & 2) != 0;
        if (p[1] & 4)
            r->close_before = true;
        r->chunk = (size_t)(p[2] | (p[3] << 8));
        nresp++;
        p = q;
    }
    /* Twice: the second fetch finds what the first left in the cache,
     * the cookie jar and the HSTS store. */
    for (pass = 0; pass < 2; pass++) {
        Run run;
        memset(&run, 0, sizeof run);
        if (fz_rnd(2)) {
            AuHttpRequest rq;
            AuHttpCallbacks cb;
            memset(&cb, 0, sizeof cb);
            cb.ctx = &run;
            cb.response = h_response;
            cb.data = h_data;
            cb.done = h_done;
            au_http_request_init(&rq, pass ? "http://a.test/next" : "http://a.test/page");
            rq.dest = fz_rnd(2) ? AU_DEST_DOCUMENT : AU_DEST_IMAGE;
            rq.reload = fz_rnd(8) == 0;
            rq.history = fz_rnd(8) == 0;
            rq.manual_redirect = fz_rnd(8) == 0;
            au_http_start(net, &rq, &cb);
            au_http_request_free(&rq);
            fake_run(&f, net, 600, 500);
            /* A server that stops answering is ended by the client's
             * timeouts: five minutes of fake time is more than any. */
            if (!run.done)
                fail("the transfer did not finish");
        } else {
            AuNetLoader *l = au_net_loader_new(net);
            AuLoadRequest rq;
            AuLoadCallbacks cb;
            int i;
            memset(&rq, 0, sizeof rq);
            memset(&cb, 0, sizeof cb);
            rq.url = pass ? "http://a.test/next" : "http://a.test/page";
            rq.dest = fz_rnd(3) ? AU_DEST_DOCUMENT : fz_rnd(2) ? AU_DEST_STYLE : AU_DEST_IMAGE;
            rq.document_url = fz_rnd(2) ? "http://a.test/" : NULL;
            rq.top_level_url = rq.document_url;
            rq.to_utf8 = fz_rnd(4) != 0;
            cb.ctx = &run;
            cb.response = l_response;
            cb.data = l_data;
            cb.done = l_done;
            au_net_loader_start(l, &rq, &cb);
            for (i = 0; i < 600 && !run.done; i++) {
                au_net_loader_poll(l);
                f.now += 500;
            }
            if (!run.done)
                fail("the load did not finish");
            au_net_loader_free(l);
        }
    }
    au_net_free(net);
    if (f.open_socks != 0)
        fail("sockets left open");
    fake_free(&f);
    au_cache_free(cache);
    au_cookie_jar_free(jar);
    au_hsts_free(hsts);
}

static void put_chunked(FzBuf *out, const uint8_t *d, size_t n)
{
    size_t pos = 0;
    char line[32];
    while (pos < n) {
        size_t c = 1 + fz_rnd(fz_rnd(3) ? 64 : 4096);
        if (c > n - pos)
            c = n - pos;
        snprintf(line, sizeof line, "%lx%s\r\n", (unsigned long)c, fz_rnd(8) ? "" : ";x=y");
        fz_puts(out, line);
        fz_insert(out, out->n, d + pos, c);
        fz_puts(out, "\r\n");
        pos += c;
    }
    fz_puts(out, fz_rnd(4) ? "0\r\n\r\n" : "0\r\nX-Trailer: 1\r\n\r\n");
}

static void build(FzBuf *in, const FzCorpus *bodies)
{
    int n = 1 + (int)fz_rnd(3), i;
    in->n = 0;
    for (i = 0; i < n; i++) {
        FzBuf r = {0};
        const FzBuf *body = &bodies->v[fz_rnd((uint32_t)bodies->n)];
        const char *name = (const char *)body->d + body->n + 1; /* the coding, stored after the bytes */
        uint8_t hdr[4];
        char line[64];
        uint32_t chunk = fz_rnd(3) == 0 ? 0 : 1 + fz_rnd(fz_rnd(2) ? 16 : 2000);
        fz_puts(&r, heads[fz_rnd(sizeof heads / sizeof heads[0])]);
        if (*name && fz_rnd(8)) {
            snprintf(line, sizeof line, "Content-Encoding: %s\r\n", name);
            fz_puts(&r, line);
        }
        switch (fz_rnd(3)) {
        case 0:
            snprintf(line, sizeof line, "Content-Length: %lu\r\n\r\n", (unsigned long)body->n);
            fz_puts(&r, line);
            fz_insert(&r, r.n, body->d, body->n);
            break;
        case 1:
            fz_puts(&r, "Transfer-Encoding: chunked\r\n\r\n");
            put_chunked(&r, body->d, body->n);
            break;
        default:
            fz_puts(&r, "Connection: close\r\n\r\n");
            fz_insert(&r, r.n, body->d, body->n);
            break;
        }
        if (fz_rnd(6))
            fz_mutate(&r, dict, NDICT, bodies, 64 << 10);
        hdr[0] = 'R';
        hdr[1] = (uint8_t)(fz_rnd(4) ? fz_rnd(2) : fz_rnd(8));
        hdr[2] = (uint8_t)chunk;
        hdr[3] = (uint8_t)(chunk >> 8);
        fz_insert(in, in->n, hdr, 4);
        fz_insert(in, in->n, r.d, r.n);
        free(r.d);
    }
}

/* Adds a body with its content coding (from the file name) kept after
 * the bytes. */
static void add_body(FzCorpus *c, const void *d, size_t n, const char *coding)
{
    FzBuf b = {0};
    size_t cl = strlen(coding);
    fz_reserve(&b, n + cl + 2);
    if (n)
        memcpy(b.d, d, n);
    b.d[n] = 0;
    memcpy(b.d + n + 1, coding, cl + 1);
    b.n = n;
    c->v = (FzBuf *)realloc(c->v, (size_t)(c->n + 1) * sizeof *c->v);
    c->v[c->n++] = b;
}

static int fuzz_main(int argc, char **argv)
{
    long iters = 0, it;
    int first, i;
    FzCorpus bodies = {0};
    FzBuf in = {0};

    if (fz_args(argc, argv, &iters, &first)) {
        for (i = first; i < argc; i++) {
            if (!fz_read_file(argv[i], &in, 0))
                continue;
            run_one(&in);
            printf("%s: ok\n", argv[i]);
        }
        return 0;
    }
    add_body(&bodies, "hello, world", 12, "");
    add_body(&bodies, "<!DOCTYPE html><meta charset=windows-1252><title>t</title><p>caf\xE9", 62, "");
    add_body(&bodies, "", 0, "");
    for (i = first; i < argc; i++) {
        FzBuf f;
        size_t n = strlen(argv[i]);
        const char *coding = "";
        if (n > 3 && !strcmp(argv[i] + n - 3, ".br"))
            coding = "br";
        else if (n > 3 && !strcmp(argv[i] + n - 3, ".gz"))
            coding = "gzip";
        else if (n > 2 && !strcmp(argv[i] + n - 2, ".z"))
            coding = "deflate";
        else if (n > 4 && !strcmp(argv[i] + n - 4, ".raw"))
            coding = "deflate";
        if (!fz_read_file(argv[i], &f, 48 << 10))
            continue;
        add_body(&bodies, f.d, f.n, coding);
        free(f.d);
    }
    for (it = 0; it < iters && !fz_expired(); it++) {
        build(&in, &bodies);
        fz_save(in.d, in.n);
        run_one(&in);
    }
    printf("fuzz_transfer: %ld iterations\n", it);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
