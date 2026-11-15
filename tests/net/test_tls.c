/* TLS tests against the in-process BearSSL server (fakenet.c) with the
 * test certificates of tls_fixtures.c: a valid chain, an expired
 * certificate, a wrong host name, an untrusted root, a clock before the
 * root store's date, session resumption, HSTS, waiting for entropy, a
 * server that does not speak TLS, and a bulk transfer timing. */
#include "fakenet.h"
#include "net/auth.h"
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

/* The user's session exception: only for the leaf with the fingerprint
 * accepted, never for an HSTS host, never cached as a session, and no
 * HSTS from an overridden connection. */
static void expect_exception(int fix, const char *what)
{
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = test_roots();
    AuHsts *hsts = au_hsts_new();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    au_net_set_hsts(net, hsts);
    FakeServer *s = tls_server(&f, "tls.test", fix);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nStrict-Transport-Security: max-age=1000\r\n\r\nhello");
    fake_respond(s, OK_RESP);
    Result r;
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.err == AU_NET_ERR_CERT && r.have_cert, "%s: fails first", what);
    uint8_t wrong[32];
    memcpy(wrong, r.cert.chain[0].sha256, 32);
    wrong[0] ^= 1;
    CHECK(au_net_allow_cert(net, "tls.test", wrong), "%s: exception recorded", what);
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.err == AU_NET_ERR_CERT, "%s: another fingerprint does not pass: %s", what, au_net_error_name(r.err));
    CHECK(au_net_allow_cert(net, "tls.test", r.cert.chain[0].sha256), "%s: exception for the leaf", what);
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.err == AU_NET_OK && r.status == 200 && r.body == 5, "%s: continues: %s", what, au_net_error_name(r.err));
    CHECK(r.have_cert && r.cert.overridden && r.cert.error != 0, "%s: marked overridden", what);
    CHECK(!au_hsts_is_known(hsts, "tls.test", f.wall), "%s: no HSTS from an overridden connection", what);
    /* A later connection is checked again (no resumed session), and still passes. */
    s->resp[s->next - 1].close_after = true;
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.err == AU_NET_OK && !r.sec.resumed && r.cert.overridden, "%s: second: %s resumed %d", what,
          au_net_error_name(r.err), r.sec.resumed);
    au_net_clear_cert_exceptions(net);
    au_net_close_idle(net);
    fake_respond(s, OK_RESP);
    get(&f, net, &r, "https://tls.test/", 500);
    CHECK(r.err == AU_NET_ERR_CERT, "%s: cleared: %s", what, au_net_error_name(r.err));
    /* HSTS hosts get no exception. */
    au_hsts_process(hsts, "tls.test", "max-age=1000", f.wall);
    CHECK(!au_net_allow_cert(net, "tls.test", r.cert.chain[0].sha256), "%s: refused for HSTS", what);
    au_net_free(net);
    au_hsts_free(hsts);
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

/* ---- Incomplete chains: caIssuers (Authority Information Access) ---- */

static char *held[64];
static int nheld;

/* Queues a 200 response carrying body on s (the bytes outlive the test). */
static void serve_bytes(FakeServer *s, const void *body, size_t len, const char *status)
{
    char head[160];
    int hl = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Type: application/pkix-cert\r\nContent-Length: %u\r\n\r\n",
                      status, (unsigned)len);
    char *resp = malloc((size_t)hl + len + 1);
    memcpy(resp, head, (size_t)hl);
    memcpy(resp + hl, body, len);
    if (nheld < 64)
        held[nheld++] = resp;
    FakeResponse *r = fake_add(s, resp);
    r->len = (size_t)hl + len;
}

static void serve_fix(FakeServer *s, int fix)
{
    serve_bytes(s, tls_fixtures[fix].cert.data, tls_fixtures[fix].cert.data_len, "200 OK");
}

static void free_held(void)
{
    for (int i = 0; i < nheld; i++)
        free(held[i]);
    nheld = 0;
}

typedef struct AiaEnv {
    FakeNet f;
    AuRootStore *roots;
    AuNet *net;
    FakeServer *tls, *ca;
} AiaEnv;

static void aia_env(AiaEnv *e, int leaf)
{
    fake_init(&e->f);
    e->roots = test_roots();
    e->net = au_net_new(&e->f.ops);
    au_net_set_roots(e->net, e->roots);
    e->tls = tls_server(&e->f, "tls.test", leaf);
    e->ca = fake_server(&e->f, "ca.test", 80);
}

static void aia_env_free(AiaEnv *e)
{
    au_net_free(e->net);
    au_roots_free(e->roots);
    fake_free(&e->f);
    free_held();
}

static void test_aia_fetch(void)
{
    AiaEnv e;
    aia_env(&e, TLS_FIX_AIA_LEAF);
    fake_respond(e.tls, OK_RESP);
    serve_fix(e.ca, TLS_FIX_INTER);
    Result r;
    get(&e.f, e.net, &r, "https://tls.test/", 2000);
    CHECK(r.done && r.err == AU_NET_OK && r.status == 200, "AIA: err %s status %d", au_net_error_name(r.err),
          r.status);
    CHECK(r.have_cert && r.cert.count == 2 && r.cert.added == 1 && strstr(r.cert.chain[1].subject, "Intermediate"),
          "AIA: completed chain shown: count %d added %d", r.cert.count, r.cert.added);
    CHECK(e.ca->nreq == 1, "AIA: one fetch: %d", e.ca->nreq);
    if (e.ca->nreq >= 1) {
        const char *q = e.ca->requests[0];
        CHECK(!strncmp(q, "GET /inter.crt HTTP/1.1\r\n", 25), "AIA request line: %.40s", q);
        CHECK(!strstr(q, "Cookie:") && !strstr(q, "Referer:") && !strstr(q, "Authorization:"),
              "AIA request carries no cookies or referrer: %s", q);
    }
    CHECK(au_net_intermediates_count(e.net) == 1 && au_net_intermediates_dirty(e.net), "pool holds it: %d",
          au_net_intermediates_count(e.net));
    /* A new full handshake (the server forgets sessions; the pooled
     * connection is closed) uses the pool without fetching again. */
    e.tls->no_resume = true;
    au_net_close_idle(e.net);
    fake_respond(e.tls, OK_RESP);
    get(&e.f, e.net, &r, "https://tls.test/", 2000);
    CHECK(r.have_cert && r.cert.count == 2 && !r.sec.resumed, "full handshake again: count %d resumed %d",
          r.cert.count, r.sec.resumed);
    CHECK(r.err == AU_NET_OK && e.ca->nreq == 1, "pooled intermediate: err %s fetches %d", au_net_error_name(r.err),
          e.ca->nreq);
    /* The pool survives a save and load into another stack. */
    AuBuf b;
    au_buf_init(&b);
    au_net_intermediates_serialize(e.net, &b);
    CHECK(!au_net_intermediates_dirty(e.net), "saved: not dirty");
    AuNet *net2 = au_net_new(&e.f.ops);
    au_net_set_roots(net2, e.roots);
    CHECK(au_net_intermediates_parse(net2, b.data, b.len) && au_net_intermediates_count(net2) == 1, "loaded pool");
    fake_respond(e.tls, OK_RESP);
    get(&e.f, net2, &r, "https://tls.test/", 2000);
    CHECK(r.err == AU_NET_OK && e.ca->nreq == 1, "loaded pool completes the chain: %s fetches %d",
          au_net_error_name(r.err), e.ca->nreq);
    au_net_free(net2);
    au_buf_free(&b);
    aia_env_free(&e);
}

static void test_aia_depth_pkcs7(void)
{
    AiaEnv e;
    aia_env(&e, TLS_FIX_AIA_LEAF2);
    fake_respond(e.tls, OK_RESP);
    serve_bytes(e.ca, tls_fix_inter2_p7, tls_fix_inter2_p7_len, "200 OK");
    serve_fix(e.ca, TLS_FIX_INTER);
    Result r;
    get(&e.f, e.net, &r, "https://tls.test/", 4000);
    CHECK(r.err == AU_NET_OK && r.cert.count == 3 && r.cert.added == 2, "two levels (PKCS#7, then DER): %s count %d",
          au_net_error_name(r.err), r.cert.count);
    CHECK(e.ca->nreq == 2 && e.ca->requests[0] && strstr(e.ca->requests[0], "/inter2.p7c") && e.ca->requests[1] &&
              strstr(e.ca->requests[1], "/inter.crt"),
          "fetch order: %d", e.ca->nreq);
    aia_env_free(&e);
}

static void test_aia_pem(void)
{
    AiaEnv e;
    aia_env(&e, TLS_FIX_AIA_LEAF);
    fake_respond(e.tls, OK_RESP);
    AuBuf pem;
    au_buf_init(&pem);
    au_buf_puts(&pem, "-----BEGIN CERTIFICATE-----\n");
    au_base64_encode(tls_fixtures[TLS_FIX_INTER].cert.data, tls_fixtures[TLS_FIX_INTER].cert.data_len, &pem);
    au_buf_puts(&pem, "\n-----END CERTIFICATE-----\n");
    serve_bytes(e.ca, pem.data, pem.len, "200 OK");
    au_buf_free(&pem);
    Result r;
    get(&e.f, e.net, &r, "https://tls.test/", 2000);
    CHECK(r.err == AU_NET_OK, "PEM caIssuers: %s", au_net_error_name(r.err));
    aia_env_free(&e);
}

/* What AIA must not rescue, and the limits on what it fetches. */
static void aia_expect_fail(int leaf, const char *what, int body_kind, int want_fetches, const char *reason)
{
    AiaEnv e;
    aia_env(&e, leaf);
    fake_respond(e.tls, OK_RESP);
    if (body_kind == 1)
        serve_fix(e.ca, TLS_FIX_OTHER_CA);
    else if (body_kind == 2)
        serve_fix(e.ca, leaf); /* the leaf itself: no progress */
    else if (body_kind == 3) {
        static char big[20000];
        memset(big, 0x30, sizeof big);
        serve_bytes(e.ca, big, sizeof big, "200 OK");
    } else if (body_kind == 4)
        serve_bytes(e.ca, "gone", 4, "404 Not Found");
    Result r;
    get(&e.f, e.net, &r, "https://tls.test/", 3000);
    CHECK(r.done && r.err == AU_NET_ERR_CERT, "%s: err %s", what, au_net_error_name(r.err));
    CHECK(e.ca->nreq == want_fetches, "%s: fetches %d, want %d", what, e.ca->nreq, want_fetches);
    CHECK(r.have_cert && r.cert.reason && strstr(r.cert.reason, reason), "%s: reason %s", what,
          r.cert.reason ? r.cert.reason : "(none)");
    CHECK(r.cert.added == 0, "%s: nothing added", what);
    if (body_kind == 4) {
        /* A URL that failed is not fetched again this session. */
        fake_respond(e.tls, OK_RESP);
        get(&e.f, e.net, &r, "https://tls.test/", 3000);
        CHECK(r.err == AU_NET_ERR_CERT && e.ca->nreq == 1, "%s: not fetched again: %d", what, e.ca->nreq);
    }
    aia_env_free(&e);
}

static void test_aia_chain_order(void)
{
    /* The complete chain sent out of order (leaf, inter, inter2): the path
     * is rebuilt from what the server sent, with no fetch. */
    AiaEnv e;
    aia_env(&e, TLS_FIX_AIA_LEAF2);
    br_x509_certificate chain[3] = {tls_fixtures[TLS_FIX_AIA_LEAF2].cert, tls_fixtures[TLS_FIX_INTER].cert,
                                    tls_fixtures[TLS_FIX_INTER2].cert};
    e.tls->chain = chain;
    e.tls->chain_len = 3;
    fake_respond(e.tls, OK_RESP);
    Result r;
    get(&e.f, e.net, &r, "https://tls.test/", 2000);
    CHECK(r.err == AU_NET_OK && e.ca->nreq == 0 && r.cert.count == 3 && r.cert.added == 0 &&
              strstr(r.cert.chain[1].subject, "Intermediate 2"),
          "reordered chain: %s fetches %d count %d", au_net_error_name(r.err), e.ca->nreq, r.cert.count);
    aia_env_free(&e);
    /* The complete chain in order needs nothing. */
    aia_env(&e, TLS_FIX_AIA_LEAF);
    br_x509_certificate full[2] = {tls_fixtures[TLS_FIX_AIA_LEAF].cert, tls_fixtures[TLS_FIX_INTER].cert};
    e.tls->chain = full;
    e.tls->chain_len = 2;
    fake_respond(e.tls, OK_RESP);
    get(&e.f, e.net, &r, "https://tls.test/", 2000);
    CHECK(r.err == AU_NET_OK && e.ca->nreq == 0 && r.cert.added == 0, "full chain: %s", au_net_error_name(r.err));
    aia_env_free(&e);
}

static void test_aia_hostile_pool(void)
{
    /* A pool file holding the untrusted root and a leaf does not make an
     * untrusted chain valid, and junk entries are skipped. */
    AiaEnv e;
    aia_env(&e, TLS_FIX_UNTRUSTED);
    fake_respond(e.tls, OK_RESP);
    AuCertPool p;
    au_cert_pool_init(&p);
    au_cert_pool_add(&p, tls_fixtures[TLS_FIX_OTHER_CA].cert.data, tls_fixtures[TLS_FIX_OTHER_CA].cert.data_len);
    au_cert_pool_add(&p, tls_fixtures[TLS_FIX_VALID].cert.data, tls_fixtures[TLS_FIX_VALID].cert.data_len);
    CHECK(!au_cert_pool_add(&p, (const uint8_t *)"\x30\x03\x02\x01\x00", 5), "junk refused");
    AuBuf b;
    au_buf_init(&b);
    au_cert_pool_serialize(&p, &b);
    au_buf_append(&b, "\x00\x05garbage", 9);
    CHECK(au_net_intermediates_parse(e.net, b.data, b.len) && au_net_intermediates_count(e.net) == 2,
          "hostile pool loads (%d)", au_net_intermediates_count(e.net));
    Result r;
    get(&e.f, e.net, &r, "https://tls.test/", 2000);
    CHECK(r.err == AU_NET_ERR_CERT && r.cert.reason && strstr(r.cert.reason, "trusted"),
          "pool cannot grant trust: %s", au_net_error_name(r.err));
    CHECK(!au_net_intermediates_parse(e.net, "AURX", 4), "not a pool file");
    au_buf_free(&b);
    au_cert_pool_free(&p);
    aia_env_free(&e);
}

/* HTTPS-first: a site whose HTTPS address redirects to plain HTTP on the
 * same host is loaded over HTTP instead of being upgraded again for
 * ever (www.macintoshrepository.org). */
static void test_https_first_downgrade_redirect(void)
{
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = test_roots();
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    au_net_config(net)->https_first = true;
    /* One host: TLS on 443, plain http on any other port. Responses are
     * served in order: the redirect over https, then the page over http. */
    FakeServer *s = tls_server(&f, "tls.test", TLS_FIX_VALID);
    s->port = 0;
    s->special_port = 443;
    FakeServer *p = s;
    fake_respond(s, "HTTP/1.1 302 Found\r\nLocation: http://tls.test/\r\nContent-Length: 0\r\n\r\n");
    fake_respond(p, OK_RESP);
    Result r;
    get(&f, net, &r, "http://tls.test/", 800);
    CHECK(r.done && r.err == AU_NET_OK && r.status == 200 && r.body == 5,
          "https-first follows a redirect back to http: err %s status %d", au_net_error_name(r.err), r.status);
    CHECK(au_net_http_allowed(net, "tls.test"), "the host is remembered as HTTP for the session");
    /* The next page on the site goes straight to http. */
    fake_respond(p, OK_RESP);
    get(&f, net, &r, "http://tls.test/next", 800);
    CHECK(r.done && r.err == AU_NET_OK && r.status == 200, "second page over http: err %s",
          au_net_error_name(r.err));
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

int main(void)
{
    test_https_first_downgrade_redirect();
    test_aia_fetch();
    test_aia_depth_pkcs7();
    test_aia_pem();
    test_aia_chain_order();
    test_aia_hostile_pool();
    aia_expect_fail(TLS_FIX_UNTRUSTED_AIA, "untrusted root via AIA", 1, 1, "trusted");
    aia_expect_fail(TLS_FIX_AIA_LEAF, "caIssuers serves the leaf", 2, 1, "trusted");
    aia_expect_fail(TLS_FIX_AIA_LEAF, "caIssuers response too large", 3, 1, "trusted");
    aia_expect_fail(TLS_FIX_AIA_LEAF, "caIssuers 404", 4, 1, "trusted");
    aia_expect_fail(TLS_FIX_EXPIRED_AIA, "expired leaf with AIA", 0, 0, "expired");
    aia_expect_fail(TLS_FIX_WRONGHOST_AIA, "wrong host with AIA", 0, 0, "different site");
    aia_expect_fail(TLS_FIX_NOAIA_LEAF, "no AIA extension", 0, 0, "trusted");
    test_valid();
    expect_cert_error(TLS_FIX_EXPIRED, "expired", "expired");
    expect_cert_error(TLS_FIX_WRONGHOST, "wrong host", "different site");
    expect_cert_error(TLS_FIX_UNTRUSTED, "untrusted root", "trust");
    expect_exception(TLS_FIX_EXPIRED, "expired");
    expect_exception(TLS_FIX_WRONGHOST, "wrong host");
    expect_exception(TLS_FIX_UNTRUSTED, "untrusted");
    test_clock_wrong();
    test_resumption_and_hsts();
    test_hsts_not_from_bad_cert();
    test_entropy_wait();
    test_not_tls();
    test_bulk_timing();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
