/* TLS tests against the in-process BearSSL server (fakenet.c) with the
 * test certificates of tls_fixtures.c: a valid chain, an expired
 * certificate, a wrong host name, an untrusted root, a clock before the
 * root store's date, session resumption, HSTS, waiting for entropy, a
 * server that does not speak TLS, and a bulk transfer timing. */
#include "fakenet.h"
#include "net/hsts.h"
#include "net/net.h"
#include "net/roots.h"
#include "net/tls.h"
#include "tls_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define ROOTS_DATE 1700000000LL /* 2023-11-14; the fake wall clock is 1800000000 (2027) */

typedef struct Result {
    bool done;
    AuNetError err;
    int status;
    size_t body;
    AuSecurityInfo sec;
    char cipher[64], alpn[32];
    AuCertInfo cert;
    bool have_cert;
} Result;

static void on_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    Result *res = ctx;
    res->status = r->status;
    res->sec = r->security;
    /* The strings live in the transfer, which done() frees: copy them. */
    if (r->security.cipher)
        snprintf(res->cipher, sizeof res->cipher, "%s", r->security.cipher);
    snprintf(res->alpn, sizeof res->alpn, "%s", r->security.alpn ? r->security.alpn : "(null)");
    res->sec.cipher = res->sec.alpn = NULL;
    const AuCertInfo *ci = au_http_cert_info(t);
    if (ci) {
        res->cert = *ci;
        res->have_cert = true;
    }
}

static void on_data(void *ctx, AuHttpTransfer *t, const void *d, size_t n)
{
    (void)t;
    (void)d;
    ((Result *)ctx)->body += n;
}

static void on_done(void *ctx, AuHttpTransfer *t, AuNetError e)
{
    Result *res = ctx;
    res->done = true;
    res->err = e;
    const AuCertInfo *ci = au_http_cert_info(t);
    if (ci && !res->have_cert) {
        res->cert = *ci;
        res->have_cert = true;
    }
}

static void get(FakeNet *f, AuNet *net, Result *r, const char *url, int rounds)
{
    memset(r, 0, sizeof *r);
    AuHttpRequest rq;
    au_http_request_init(&rq, url);
    AuHttpCallbacks cb = {r, on_response, on_data, on_done, NULL, NULL};
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(f, net, rounds, 10);
}

static AuRootStore *test_roots(void)
{
    AuRootStore *r = au_roots_new(ROOTS_DATE);
    const TlsFixture *ca = &tls_fixtures[TLS_FIX_CA];
    if (!au_roots_add_der(r, ca->cert.data, ca->cert.data_len)) {
        printf("cannot add the test root\n");
        exit(2);
    }
    return r;
}

static FakeServer *tls_server(FakeNet *f, const char *host, int fix)
{
    FakeServer *s = fake_server(f, host, 443);
    s->tls = true;
    s->chain = &tls_fixtures[fix].cert;
    s->chain_len = 1;
    s->eckey = &tls_fixtures[fix].key;
    return s;
}

static const char OK_RESP[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";

static void test_valid(void)
{
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = test_roots();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    FakeServer *s = tls_server(&f, "tls.test", TLS_FIX_VALID);
    fake_respond(s, OK_RESP);
    Result r;
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.done && r.err == AU_NET_OK && r.status == 200 && r.body == 5, "valid: err %s status %d",
          au_net_error_name(r.err), r.status);
    CHECK(r.sec.tls && r.sec.version == 0x0303, "TLS 1.2: %d %04x", r.sec.tls, r.sec.version);
    CHECK(strstr(r.cipher, "ECDHE-ECDSA") != NULL, "ECDHE-ECDSA suite: %s", r.cipher);
    /* The test server offers no ALPN, so none is reported. */
    CHECK(!strcmp(r.alpn, ""), "no ALPN: %s", r.alpn);
    CHECK(r.have_cert && r.cert.count == 1 && strstr(r.cert.chain[0].subject, "CN=tls.test") &&
              strstr(r.cert.chain[0].issuer, "Aura Test Root") && !strcmp(r.cert.chain[0].san, "tls.test"),
          "chain details: count %d subject %s issuer %s san %s", r.cert.count, r.cert.chain[0].subject,
          r.cert.chain[0].issuer, r.cert.chain[0].san);
    CHECK(r.cert.chain[0].not_before == 1577836800LL, "not_before %lld", (long long)r.cert.chain[0].not_before);
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

static void expect_cert_error(int fix, const char *what, const char *reason_part)
{
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = test_roots();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    FakeServer *s = tls_server(&f, "tls.test", fix);
    fake_respond(s, OK_RESP);
    Result r;
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.done && r.err == AU_NET_ERR_CERT, "%s: err %s", what, au_net_error_name(r.err));
    CHECK(r.status == 0, "%s: no response delivered", what);
    CHECK(r.have_cert && r.cert.reason && strstr(r.cert.reason, reason_part), "%s: reason \"%s\"", what,
          r.have_cert && r.cert.reason ? r.cert.reason : "(none)");
    CHECK(r.have_cert && r.cert.count >= 1 && r.cert.chain[0].subject[0], "%s: chain captured", what);
    CHECK(r.have_cert && !r.cert.clock_wrong, "%s: clock not blamed", what);
    CHECK(s->nreq == 0, "%s: no request sent over the bad connection", what);
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

static void test_clock_wrong(void)
{
    FakeNet f;
    fake_init(&f);
    f.wall = 1500000000; /* 2017: before the root store's date and the certificate */
    AuRootStore *roots = test_roots();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    FakeServer *s = tls_server(&f, "tls.test", TLS_FIX_VALID);
    fake_respond(s, OK_RESP);
    Result r;
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.done && r.err == AU_NET_ERR_CLOCK, "clock wrong: err %s", au_net_error_name(r.err));
    CHECK(r.have_cert && r.cert.clock_wrong, "clock_wrong flag");
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

static void test_resumption_and_hsts(void)
{
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = test_roots();
    AuHsts *hsts = au_hsts_new();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    au_net_set_hsts(net, hsts);
    FakeServer *s = tls_server(&f, "tls.test", TLS_FIX_VALID);
    FakeResponse *a = fake_add(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                                  "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n\r\nok");
    a->close_after = true;
    fake_respond(s, OK_RESP);
    Result r;
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.err == AU_NET_OK && !r.sec.resumed, "first: err %s resumed %d", au_net_error_name(r.err),
          r.sec.resumed);
    CHECK(au_hsts_is_known(hsts, "tls.test", f.wall), "HSTS recorded from a valid https response");
    CHECK(au_hsts_is_known(hsts, "sub.tls.test", f.wall), "includeSubDomains");
    /* http:// is upgraded; the new connection resumes the session. */
    get(&f, net, &r, "http://tls.test/", 500);
    CHECK(r.err == AU_NET_OK && r.status == 200, "upgraded: err %s (%d %s)", au_net_error_name(r.err),
          r.cert.error, r.have_cert && r.cert.reason ? r.cert.reason : "");
    CHECK(r.sec.tls && r.sec.resumed, "second connection resumed: tls %d resumed %d", r.sec.tls, r.sec.resumed);
    CHECK(s->connections == 2, "two connections: %d", s->connections);
    au_net_free(net);
    au_hsts_free(hsts);
    au_roots_free(roots);
    fake_free(&f);
}

static void test_hsts_not_from_bad_cert(void)
{
    /* An STS header over a connection with certificate errors is never
     * seen (the request fails), and over http it is ignored. */
    FakeNet f;
    fake_init(&f);
    AuHsts *hsts = au_hsts_new();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_hsts(net, hsts);
    FakeServer *s = fake_server(&f, "plain.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nStrict-Transport-Security: max-age=100\r\n\r\n");
    Result r;
    get(&f, net, &r, "http://plain.test/", 200);
    CHECK(r.err == AU_NET_OK && !au_hsts_is_known(hsts, "plain.test", f.wall), "STS over http ignored");
    au_net_free(net);
    au_hsts_free(hsts);
    fake_free(&f);
}

static void test_entropy_wait(void)
{
    FakeNet f;
    fake_init(&f);
    f.entropy_ready = false;
    AuRootStore *roots = test_roots();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    FakeServer *s = tls_server(&f, "tls.test", TLS_FIX_VALID);
    fake_respond(s, OK_RESP);
    Result r;
    get(&f, net, &r, "https://tls.test/", 50);
    CHECK(!r.done && s->connections <= 1, "waits for entropy: done %d", r.done);
    f.entropy_ready = true;
    fake_run(&f, net, 500, 10);
    CHECK(r.done && r.err == AU_NET_OK, "completes once entropy is ready: %s", au_net_error_name(r.err));
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

static void test_not_tls(void)
{
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = test_roots();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    FakeServer *s = fake_server(&f, "tls.test", 443); /* speaks first, in plain text */
    s->banner = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    Result r;
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.done && r.err == AU_NET_ERR_TLS, "plain server on 443: %s", au_net_error_name(r.err));
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

static void test_bulk_timing(void)
{
    enum { BODY = 4 << 20 };
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = test_roots();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    FakeServer *s = tls_server(&f, "tls.test", TLS_FIX_VALID);
    char head[128];
    int hl = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", BODY);
    char *resp = malloc((size_t)hl + BODY);
    memcpy(resp, head, (size_t)hl);
    memset(resp + hl, 'x', BODY);
    FakeResponse *b = fake_add(s, resp);
    b->len = (size_t)hl + BODY;
    clock_t c0 = clock();
    Result r;
    get(&f, net, &r, "https://tls.test/", 200000);
    double sec = (double)(clock() - c0) / CLOCKS_PER_SEC;
    CHECK(r.err == AU_NET_OK && r.body == BODY, "bulk: err %s body %zu", au_net_error_name(r.err), r.body);
    /* Both ends run here, so this is about half the client's speed. */
    printf("TLS bulk (%s), client and server together: %.1f MB/s, handshake included\n", r.cipher,
           BODY / 1048576.0 / (sec > 0 ? sec : 1e-9));
    free(resp);
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

int main(void)
{
    test_valid();
    expect_cert_error(TLS_FIX_EXPIRED, "expired", "expired");
    expect_cert_error(TLS_FIX_WRONGHOST, "wrong host", "different site");
    expect_cert_error(TLS_FIX_UNTRUSTED, "untrusted root", "trust");
    test_clock_wrong();
    test_resumption_and_hsts();
    test_hsts_not_from_bad_cert();
    test_entropy_wait();
    test_not_tls();
    test_bulk_timing();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
