/* HTTP client tests against the in-process fake transport: framing,
 * keep-alive and retry, content codings, redirects, authentication,
 * proxies, request headers, timeouts, cancellation, and pathological
 * servers. No network. */
#include "fakenet.h"
#include "net/auth.h"
#include "net/http_parse.h"
#include "net/net.h"
#include "net/policy.h"

#include <stdio.h>
#include <stdlib.h>
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

typedef struct Result {
    int status;
    int responses;
    AuBuf body;
    bool done;
    AuNetError err;
    int redirects;
    char url[512];
    char mime[64];
    int calls_after_done;
    /* auth */
    const char *user, *pass;
    int asked;
    bool cancel_on_data;
    AuHttpTransfer *t;
} Result;

static void on_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    Result *res = ctx;
    (void)t;
    if (res->done)
        res->calls_after_done++;
    res->status = r->status;
    res->responses++;
    res->redirects = r->redirects;
    snprintf(res->url, sizeof res->url, "%s", r->url);
    snprintf(res->mime, sizeof res->mime, "%s", r->mime);
}

static void on_data(void *ctx, AuHttpTransfer *t, const void *d, size_t n)
{
    Result *res = ctx;
    if (res->done)
        res->calls_after_done++;
    au_buf_append(&res->body, d, n);
    if (res->cancel_on_data)
        au_http_cancel(t);
}

static void on_done(void *ctx, AuHttpTransfer *t, AuNetError e)
{
    Result *res = ctx;
    (void)t;
    if (res->done)
        res->calls_after_done++;
    res->done = true;
    res->err = e;
}

static int on_cred(void *ctx, AuHttpTransfer *t, const AuAuthChallenge *ch, AuBuf *user, AuBuf *pass)
{
    Result *res = ctx;
    (void)t;
    (void)ch;
    res->asked++;
    if (!res->user)
        return 0;
    au_buf_puts(user, res->user);
    au_buf_puts(pass, res->pass);
    return 1;
}

static AuHttpCallbacks callbacks(Result *r)
{
    AuHttpCallbacks cb = {r, on_response, on_data, on_done, NULL, on_cred};
    return cb;
}

static void result_init(Result *r)
{
    memset(r, 0, sizeof *r);
    au_buf_init(&r->body);
}

static void result_free(Result *r) { au_buf_free(&r->body); }

/* Starts a GET (or method with body) and runs until done. */
static void fetch(FakeNet *f, AuNet *net, Result *r, const char *method, const char *url, const char *body)
{
    AuHttpRequest rq;
    au_http_request_init(&rq, url);
    rq.method = method;
    if (body)
        rq.body = au_body_from_bytes(body, strlen(body), "application/x-www-form-urlencoded");
    AuHttpCallbacks cb = callbacks(r);
    r->t = au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(f, net, 2000, 10);
}

static const char *last_request(FakeServer *s)
{
    return s->nreq ? s->requests[s->nreq - 1] : "";
}

#define BIN(s) s, sizeof(s) - 1

static void setup(FakeNet *f, AuNet **net)
{
    fake_init(f);
    *net = au_net_new(&f->ops);
}

static void teardown(FakeNet *f, AuNet *net)
{
    au_net_free(net);
    CHECK(f->open_socks == 0, "sockets left open: %d", f->open_socks);
    fake_free(f);
}

static void test_basic(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 5\r\n\r\nhello");
    Result r;
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/p?q=1#frag", NULL);
    CHECK(r.done && r.err == AU_NET_OK && r.status == 200, "basic: err %d status %d", r.err, r.status);
    CHECK(r.body.len == 5 && !memcmp(r.body.data, "hello", 5), "basic body");
    CHECK(!strcmp(r.mime, "text/html"), "mime %s", r.mime);
    const char *q = last_request(s);
    CHECK(!strncmp(q, "GET /p?q=1 HTTP/1.1\r\n", 21), "request line: %.40s", q);
    CHECK(strstr(q, "\r\nHost: a.test\r\n") && strstr(q, "\r\nUser-Agent: ") && strstr(q, "Accept-Encoding: gzip, deflate, br"),
          "request headers: %s", q);
    CHECK(strstr(q, "Upgrade-Insecure-Requests: 1") != NULL, "UIR");
    CHECK(!strstr(q, "Sec-Fetch-"), "no Sec-Fetch to http");
    CHECK(!strstr(q, "Referer"), "no referer");
    CHECK(r.calls_after_done == 0, "calls after done");
    result_free(&r);
    teardown(&f, net);
}

static void test_framing(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    /* chunked with extension and trailers */
    fake_respond(s, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n");
    /* close-delimited */
    FakeResponse *c = fake_add(s, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil the end");
    c->close_after = true;
    Result r;
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/1", NULL);
    CHECK(r.err == AU_NET_OK && r.body.len == 11 && !memcmp(r.body.data, "hello world", 11), "chunked: %d '%.*s'", r.err,
          (int)r.body.len, r.body.data);
    result_free(&r);
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/2", NULL);
    CHECK(r.err == AU_NET_OK && r.body.len == 13, "close-delimited: %d %zu", r.err, r.body.len);
    CHECK(s->connections == 1, "chunked response kept the connection: %d", s->connections);
    result_free(&r);
    teardown(&f, net);
}

static void test_keepalive_and_retry(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na");
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nb");
    FakeResponse *stale = fake_add(s, "");
    stale->close_before = true;
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nc");
    Result r;
    for (int i = 0; i < 3; i++) {
        result_init(&r);
        fetch(&f, net, &r, "GET", "http://a.test/", NULL);
        CHECK(r.err == AU_NET_OK && r.body.len == 1 && r.body.data[0] == "abc"[i], "keepalive %d: err %d", i, r.err);
        result_free(&r);
    }
    CHECK(s->connections == 2, "reuse then one retry on a fresh connection: %d connections", s->connections);
    /* A POST is not retried on a stale connection. */
    FakeResponse *stale2 = fake_add(s, "");
    stale2->close_before = true;
    result_init(&r);
    fetch(&f, net, &r, "POST", "http://a.test/form", "a=1");
    CHECK(r.err == AU_NET_ERR_RESET, "POST on a stale connection fails: %d", r.err);
    result_free(&r);
    /* Idle connections expire. */
    teardown(&f, net);
}

static void test_codings(void)
{
    static const char gz[] = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 36\r\n\r\n"
                             "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\x13\xcb\x48\xcd\xc9\xc9\x57\x28\xcf\x2f\xca\x49\xe1"
                             "\xca\x18\x01\x6c\x00\xbc\x9e\x9d\x2e\xf0\x00\x00\x00";
    static const char zl[] = "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 24\r\n\r\n"
                             "\x78\x9c\xcb\x48\xcd\xc9\xc9\x57\x28\xcf\x2f\xca\x49\xe1\xca\x18\x01\x6c\x00\x8f\xbf\x57"
                             "\xf9";
    static const char raw[] = "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 17\r\n\r\n"
                              "\xcb\x48\xcd\xc9\xc9\x57\x28\xcf\x2f\xca\x49\xe1\xca\x18\x01\x6c\x00";
    static const char br[] = "HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nTransfer-Encoding: chunked\r\n\r\n"
                             "1a\r\n\x1f\xef\x00\xf8\x8d\x94\x6e\xde\x44\x55\x86\xd6\x0a\x20\x6c\x6f\x6a\x96\xc5\x6a\x81"
                             "\x0c\xa8\x76\x0f\x00\r\n0\r\n\r\n";
    static const char bad[] = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 12\r\n\r\n"
                              "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\x13\xff\xff";
    static const char unknown[] = "HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 3\r\n\r\nabc";
    const char *resp[] = {gz, zl, raw, br, bad, unknown};
    size_t lens[] = {sizeof gz - 1, sizeof zl - 1, sizeof raw - 1, sizeof br - 1, sizeof bad - 1, sizeof unknown - 1};
    char expect[241];
    for (int i = 0; i < 20; i++)
        memcpy(expect + 12 * i, "hello world\n", 12);
    for (int i = 0; i < 6; i++) {
        FakeNet f;
        AuNet *net;
        setup(&f, &net);
        FakeServer *s = fake_server(&f, "a.test", 80);
        FakeResponse *fr = fake_add(s, resp[i]);
        fr->len = lens[i];
        fr->chunk = i == 1 ? 1 : 0; /* one at a time for the zlib case */
        Result r;
        result_init(&r);
        fetch(&f, net, &r, "GET", "http://a.test/", NULL);
        if (i < 4)
            CHECK(r.err == AU_NET_OK && r.body.len == 240 && !memcmp(r.body.data, expect, 240),
                  "coding %d: err %d len %zu", i, r.err, r.body.len);
        else if (i == 4)
            CHECK(r.err == AU_NET_ERR_DECODE, "corrupt gzip: %d", r.err);
        else
            CHECK(r.err == AU_NET_OK && r.body.len == 3, "unknown coding delivered as sent: %d", r.err);
        result_free(&r);
        teardown(&f, net);
    }
}

static void test_redirects(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    FakeServer *b = fake_server(&f, "b.test", 80);
    fake_respond(s, "HTTP/1.1 301 Moved\r\nLocation: /two\r\nContent-Length: 4\r\n\r\nmove");
    fake_respond(s, "HTTP/1.1 302 Found\r\nLocation: http://b.test/three\r\nContent-Length: 0\r\n\r\n");
    fake_respond(b, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    Result r;
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/one#keep", NULL);
    CHECK(r.err == AU_NET_OK && r.status == 200 && r.redirects == 2, "chain: %d %d %d", r.err, r.status, r.redirects);
    CHECK(!strcmp(r.url, "http://b.test/three#keep"), "final url %s", r.url);
    CHECK(r.responses == 1, "only the final response is reported");
    result_free(&r);

    /* POST + 303 -> GET without a body; 307 keeps POST and body. */
    fake_respond(s, "HTTP/1.1 303 See Other\r\nLocation: /got\r\nContent-Length: 0\r\n\r\n");
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    result_init(&r);
    fetch(&f, net, &r, "POST", "http://a.test/form", "x=1");
    CHECK(r.err == AU_NET_OK, "303: %d", r.err);
    CHECK(!strncmp(last_request(s), "GET /got ", 9) && !strstr(last_request(s), "Content-Length: 3"), "303 GET: %s",
          last_request(s));
    result_free(&r);
    fake_respond(s, "HTTP/1.1 307 Temporary\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n");
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    result_init(&r);
    fetch(&f, net, &r, "POST", "http://a.test/form", "x=1");
    CHECK(!strncmp(last_request(s), "POST /again ", 12) && strstr(last_request(s), "\r\n\r\nx=1"), "307 POST: %s",
          last_request(s));
    result_free(&r);

    /* Authorization does not cross origins. */
    fake_respond(s, "HTTP/1.1 302 Found\r\nLocation: http://b.test/x\r\nContent-Length: 0\r\n\r\n");
    fake_respond(b, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    AuHttpRequest rq;
    au_http_request_init(&rq, "http://a.test/auth");
    au_headers_add(&rq.headers, "Authorization", "Bearer secret");
    result_init(&r);
    AuHttpCallbacks cb = callbacks(&r);
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(&f, net, 1000, 10);
    CHECK(strstr(last_request(s), "Bearer secret") && !strstr(last_request(b), "Bearer secret"),
          "credentials stripped across origins");
    result_free(&r);

    /* Endless redirects. */
    for (int i = 0; i < 25; i++)
        fake_respond(s, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n");
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/loop", NULL);
    CHECK(r.err == AU_NET_ERR_REDIRECTS, "endless redirects: %d", r.err);
    result_free(&r);

    /* A redirect to another scheme is refused. */
    FakeServer *c = fake_server(&f, "c.test", 80);
    fake_respond(c, "HTTP/1.1 302 Found\r\nLocation: ftp://c.test/x\r\nContent-Length: 0\r\n\r\n");
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://c.test/", NULL);
    CHECK(r.err == AU_NET_ERR_REDIRECTS, "ftp redirect: %d", r.err);
    result_free(&r);
    teardown(&f, net);
}

/* One response, one expected error. */
static void expect_err(const char *name, const char *resp, size_t len, size_t chunk, bool close, AuNetError want)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    FakeResponse *fr = fake_add(s, resp);
    fr->len = len;
    fr->chunk = chunk;
    fr->close_after = close;
    Result r;
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/", NULL);
    CHECK(r.done && r.err == want, "%s: got %d (%s), want %d", name, r.err, au_net_error_name(r.err), want);
    result_free(&r);
    teardown(&f, net);
}

static void test_pathological(void)
{
    /* Oversized headers */
    size_t big = 120 * 1024;
    char *h = malloc(big + 100);
    int n = sprintf(h, "HTTP/1.1 200 OK\r\nX-Big: ");
    memset(h + n, 'a', big);
    memcpy(h + n + big, "\r\n\r\n", 5);
    expect_err("oversized header", h, strlen(h), 0, false, AU_NET_ERR_TOO_LARGE);
    free(h);
    expect_err("bad chunk size", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nab\r\n0\r\n\r\n", 0, 0,
               false, AU_NET_ERR_PROTOCOL);
    expect_err("huge chunk size", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffffff\r\n", 0, 0,
               false, AU_NET_ERR_PROTOCOL);
    expect_err("missing CRLF after chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabX\r\n0\r\n\r\n",
               0, 0, false, AU_NET_ERR_PROTOCOL);
    expect_err("conflicting lengths", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", 0, 0,
               false, AU_NET_ERR_PROTOCOL);
    expect_err("negative length", "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", 0, 0, false, AU_NET_ERR_PROTOCOL);
    expect_err("obs-fold", "HTTP/1.1 200 OK\r\nX-A: 1\r\n continued\r\nContent-Length: 0\r\n\r\n", 0, 0, false,
               AU_NET_ERR_PROTOCOL);
    expect_err("space before colon", "HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n", 0, 0, false, AU_NET_ERR_PROTOCOL);
    expect_err("HTTP/0.9", "<html>hello</html>", 0, 0, true, AU_NET_ERR_PROTOCOL);
    expect_err("truncated body", "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", 0, 0, true, AU_NET_ERR_RESET);
    expect_err("truncated chunked", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", 0, 0, true,
               AU_NET_ERR_RESET);
    expect_err("slow server", "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", 0, 1, false, AU_NET_OK);
    expect_err("NUL in header", BIN("HTTP/1.1 200 OK\r\nX-A: a\0b\r\nContent-Length: 0\r\n\r\n"), 0, false,
               AU_NET_ERR_PROTOCOL);
    expect_err("101 switch", "HTTP/1.1 101 Switching\r\nUpgrade: x\r\n\r\n", 0, 0, false, AU_NET_ERR_PROTOCOL);
    expect_err("interim then final", "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early\r\nLink: x\r\n\r\n"
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
               0, 3, false, AU_NET_OK);
    expect_err("gzip in TE", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", 0, 0, false,
               AU_NET_ERR_PROTOCOL);
    expect_err("empty", "", 0, 0, true, AU_NET_ERR_RESET);

    /* TE and CL together: chunked wins and the connection is not reused. */
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n");
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    Result r;
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/", NULL);
    CHECK(r.err == AU_NET_OK && r.body.len == 2, "TE+CL: %d %zu", r.err, r.body.len);
    result_free(&r);
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/", NULL);
    CHECK(s->connections == 2, "TE+CL connection not reused: %d", s->connections);
    result_free(&r);
    teardown(&f, net);
}

static void test_errors_and_timeouts(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *d = fake_server(&f, "nodns.test", 80);
    d->dns_fail = true;
    FakeServer *rf = fake_server(&f, "refuse.test", 80);
    rf->refuse = true;
    FakeServer *hang = fake_server(&f, "hang.test", 80);
    hang->connect_hang = true;
    FakeServer *silent = fake_server(&f, "silent.test", 80);
    fake_add(silent, "")->silent = true;
    Result r;
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://nodns.test/", NULL);
    CHECK(r.err == AU_NET_ERR_DNS, "dns: %d", r.err);
    result_free(&r);
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://unknown.test/", NULL);
    CHECK(r.err == AU_NET_ERR_DNS, "unknown host: %d", r.err);
    result_free(&r);
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://refuse.test/", NULL);
    CHECK(r.err == AU_NET_ERR_CONNECT, "refused: %d", r.err);
    result_free(&r);
    result_init(&r);
    {
        AuHttpRequest rq;
        au_http_request_init(&rq, "http://hang.test/");
        AuHttpCallbacks cb = callbacks(&r);
        au_http_start(net, &rq, &cb);
        au_http_request_free(&rq);
        fake_run(&f, net, 2000, 100); /* past the 30 s connect timeout */
    }
    CHECK(r.err == AU_NET_ERR_TIMEOUT, "connect timeout: %d", r.err);
    result_free(&r);
    result_init(&r);
    {
        AuHttpRequest rq;
        au_http_request_init(&rq, "http://silent.test/");
        AuHttpCallbacks cb = callbacks(&r);
        au_http_start(net, &rq, &cb);
        au_http_request_free(&rq);
        fake_run(&f, net, 2000, 100); /* past the 60 s idle timeout */
    }
    CHECK(r.err == AU_NET_ERR_TIMEOUT, "idle timeout: %d", r.err);
    result_free(&r);
    result_init(&r);
    fetch(&f, net, &r, "GET", "ftp://x.test/", NULL);
    CHECK(r.err == AU_NET_ERR_SCHEME, "scheme: %d", r.err);
    result_free(&r);
    result_init(&r);
    fetch(&f, net, &r, "GET", "not a url", NULL);
    CHECK(r.err == AU_NET_ERR_BAD_URL, "bad url: %d", r.err);
    result_free(&r);
    /* https without a root store */
    result_init(&r);
    fetch(&f, net, &r, "GET", "https://x.test/", NULL);
    CHECK(r.err == AU_NET_ERR_CERT, "https without roots: %d", r.err);
    result_free(&r);
    teardown(&f, net);
}

static void test_cancel(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    FakeResponse *fr = fake_add(s, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");
    fr->chunk = 2;
    Result r;
    result_init(&r);
    r.cancel_on_data = true;
    fetch(&f, net, &r, "GET", "http://a.test/", NULL);
    CHECK(!r.done && r.body.len > 0 && r.body.len < 10, "cancel stops callbacks: done %d len %zu", r.done, r.body.len);
    CHECK(au_net_active(net) == 0, "cancelled transfer reaped");
    result_free(&r);
    /* Cancel before anything happens. */
    result_init(&r);
    AuHttpRequest rq;
    au_http_request_init(&rq, "http://a.test/");
    AuHttpCallbacks cb = callbacks(&r);
    AuHttpTransfer *t = au_http_start(net, &rq, &cb);
    au_http_cancel(t);
    fake_run(&f, net, 10, 10);
    CHECK(!r.done && r.responses == 0, "early cancel");
    au_http_request_free(&rq);
    result_free(&r);
    teardown(&f, net);
}

static void test_auth(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    fake_respond(s, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"x\"\r\nContent-Length: 4\r\n\r\nnope");
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nin");
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nin");
    Result r;
    result_init(&r);
    r.user = "Aladdin";
    r.pass = "open sesame";
    fetch(&f, net, &r, "GET", "http://a.test/secret", NULL);
    CHECK(r.err == AU_NET_OK && r.status == 200 && r.asked == 1, "basic auth: %d %d %d", r.err, r.status, r.asked);
    CHECK(strstr(last_request(s), "Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==") != NULL, "basic header: %s",
          last_request(s));
    result_free(&r);
    /* The cached credentials go preemptively next time. */
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://a.test/other", NULL);
    CHECK(r.asked == 0 && strstr(last_request(s), "Authorization: Basic") != NULL, "preemptive basic");
    result_free(&r);
    /* Declined: the 401 itself is delivered. */
    FakeServer *b = fake_server(&f, "b.test", 80);
    fake_respond(b, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest realm=\"r\", nonce=\"n\", qop=\"auth\"\r\n"
                    "Content-Length: 4\r\n\r\nnope");
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://b.test/", NULL);
    CHECK(r.err == AU_NET_OK && r.status == 401 && r.body.len == 4, "declined auth: %d %d", r.err, r.status);
    result_free(&r);
    teardown(&f, net);

    /* RFC 2617 3.5 example. */
    AuAuthParams p;
    CHECK(au_auth_parse("Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
                        "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"",
                        &p),
          "parse digest");
    AuBuf out;
    au_buf_init(&out);
    au_auth_digest(&p, "Mufasa", "Circle Of Life", "GET", "/dir/index.html", 1, "0a4f113b", &out);
    CHECK(strstr(au_buf_cstr(&out), "response=\"6629fae49393a05397450978507c4ef1\"") != NULL, "digest: %s", out.data);
    au_buf_free(&out);
    CHECK(au_auth_parse("Newauth realm=\"x\", Basic realm=\"y\"", &p) && !strcmp(p.scheme, "Basic") &&
              !strcmp(p.realm, "y"),
          "picks Basic among unknown schemes");
    CHECK(!au_auth_parse("Negotiate abc", &p), "unsupported scheme");
    /* base64 */
    au_buf_init(&out);
    CHECK(au_base64_decode("SGVs bG8=", 9, &out) && out.len == 5 && !memcmp(out.data, "Hello", 5), "base64");
    au_buf_clear(&out);
    CHECK(!au_base64_decode("SGV=sbG8", 8, &out), "base64 padding in the middle");
    CHECK(!au_base64_decode("A", 1, &out), "base64 length 1 mod 4");
    au_buf_free(&out);
}

static void test_proxy_and_headers(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    AuNetConfig *c = au_net_config(net);
    au_net_set_config_str(&c->http_proxy_host, "proxy.test");
    c->http_proxy_port = 3128;
    c->send_dnt = c->send_gpc = true;
    FakeServer *p = fake_server(&f, "proxy.test", 3128);
    fake_respond(p, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    fake_respond(p, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    Result r;
    result_init(&r);
    AuHttpRequest rq;
    au_http_request_init(&rq, "http://far.test/page?x");
    rq.referrer = "http://near.test/from/here?secret";
    rq.dest = AU_DEST_IMAGE;
    rq.mode = AU_MODE_NO_CORS;
    rq.range_start = 100;
    AuHttpCallbacks cb = callbacks(&r);
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(&f, net, 1000, 10);
    const char *q = last_request(p);
    CHECK(r.err == AU_NET_OK && !strncmp(q, "GET http://far.test/page?x HTTP/1.1\r\n", 37), "absolute form: %.60s", q);
    CHECK(strstr(q, "DNT: 1\r\n") && strstr(q, "Sec-GPC: 1\r\n"), "DNT and GPC");
    CHECK(strstr(q, "Referer: http://near.test/\r\n") != NULL, "cross-origin referer is the origin: %s", q);
    CHECK(strstr(q, "Range: bytes=100-\r\n") && !strstr(q, "Accept-Encoding"), "range");
    CHECK(strstr(q, "Accept: image/") != NULL, "image accept");
    CHECK(!strstr(q, "Upgrade-Insecure-Requests"), "no UIR for subresources");
    result_free(&r);
    /* POST: Origin. */
    result_init(&r);
    au_http_request_init(&rq, "http://far.test/post");
    rq.method = "POST";
    rq.referrer = "http://far.test/form";
    rq.body = au_body_from_bytes("a=b", 3, "application/x-www-form-urlencoded");
    cb = callbacks(&r);
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(&f, net, 1000, 10);
    q = last_request(p);
    CHECK(strstr(q, "Origin: http://far.test\r\n") && strstr(q, "Referer: http://far.test/form\r\n") &&
              strstr(q, "Content-Length: 3\r\n") && strstr(q, "Content-Type: application/x-www-form-urlencoded"),
          "POST headers: %s", q);
    result_free(&r);
    teardown(&f, net);
}

static void test_https_first(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    au_net_config(net)->https_first = true;
    FakeServer *s = fake_server(&f, "plain.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    Result r;
    result_init(&r);
    fetch(&f, net, &r, "GET", "http://plain.test/", NULL);
    /* No roots: the https attempt fails as a certificate error, which
     * HTTPS-first reports as a fallback. */
    CHECK(r.err == AU_NET_ERR_HTTPS_FALLBACK || r.err == AU_NET_ERR_CERT, "https-first: %d", r.err);
    result_free(&r);
    result_init(&r);
    AuHttpRequest rq;
    au_http_request_init(&rq, "http://plain.test/");
    rq.allow_http_fallback = true;
    AuHttpCallbacks cb = callbacks(&r);
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(&f, net, 1000, 10);
    CHECK(r.err == AU_NET_OK && r.status == 200, "confirmed http: %d", r.err);
    result_free(&r);
    teardown(&f, net);
}

/* A body source of unknown length: sent chunked. */
typedef struct Counter {
    int left;
} Counter;
static long counter_read(void *ctx, void *buf, size_t cap)
{
    Counter *c = ctx;
    if (!c->left || cap < 4)
        return 0;
    c->left--;
    memcpy(buf, "abcd", 4);
    return 4;
}
static void counter_free(void *ctx) { free(ctx); }

static void test_upload_chunked(void)
{
    FakeNet f;
    AuNet *net;
    setup(&f, &net);
    FakeServer *s = fake_server(&f, "a.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    AuHttpRequest rq;
    au_http_request_init(&rq, "http://a.test/up");
    rq.method = "PUT";
    AuBodySource *b = au_calloc(sizeof *b);
    Counter *ctr = malloc(sizeof *ctr);
    ctr->left = 3;
    b->ctx = ctr;
    b->read = counter_read;
    b->free = counter_free;
    b->length = -1;
    rq.body = b;
    Result r;
    result_init(&r);
    AuHttpCallbacks cb = callbacks(&r);
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(&f, net, 1000, 10);
    const char *q = last_request(s);
    CHECK(r.err == AU_NET_OK && strstr(q, "Transfer-Encoding: chunked") &&
              strstr(q, "\r\n\r\n4\r\nabcd\r\n4\r\nabcd\r\n4\r\nabcd\r\n0\r\n\r\n"),
          "chunked upload: %d %s", r.err, q);
    result_free(&r);
    teardown(&f, net);
}

static void test_dates_and_parsers(void)
{
    int64_t t;
    CHECK(au_http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT", &t) && t == 784111777, "IMF date");
    CHECK(au_http_parse_date("Sunday, 06-Nov-94 08:49:37 GMT", &t) && t == 784111777, "RFC 850 date");
    CHECK(au_http_parse_date("Sun Nov  6 08:49:37 1994", &t) && t == 784111777, "asctime date");
    CHECK(!au_http_parse_date("garbage", &t), "bad date");
    char d[32];
    au_http_format_date(784111777, d);
    CHECK(!strcmp(d, "Sun, 06 Nov 1994 08:49:37 GMT"), "format date %s", d);
    AuBuf e, c;
    au_buf_init(&e);
    au_buf_init(&c);
    CHECK(au_http_parse_mime("Text/HTML ; Charset=\"ISO-8859-1\"", &e, &c) && !strcmp(e.data, "text/html") &&
              !strcmp(c.data, "ISO-8859-1"),
          "mime parse: %s %s", e.data, c.data);
    CHECK(!au_http_parse_mime("text", &e, &c), "bad mime");
    au_buf_free(&e);
    au_buf_free(&c);
}

int main(void)
{
    test_basic();
    test_framing();
    test_keepalive_and_retry();
    test_codings();
    test_redirects();
    test_pathological();
    test_errors_and_timeouts();
    test_cancel();
    test_auth();
    test_proxy_and_headers();
    test_https_first();
    test_upload_chunked();
    test_dates_and_parsers();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
