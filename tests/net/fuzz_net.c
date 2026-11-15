/* Fuzzes the network stack's parsers with mutated seed inputs:
 *
 *   fuzz_net TARGET ITERATIONS SEED [ROOTS_FILE]
 *
 * TARGET is one of: http (response parser fed in random pieces, chunked
 * bodies included, with a check that the body survives any split),
 * dataurl, psl, roots (the root store loader; seeds from ROOTS_FILE),
 * cert (au_cert_parse on mutated test certificates), hsts (header and
 * file parsers), auth (WWW-Authenticate parser), dates (cookie dates),
 * mime (Content-Type), cache (serialized cache entries), aia (caIssuers
 * bundles: DER, PKCS#7 and PEM; the AIA URL; the intermediates pool
 * file), or all. Every input is copied to a heap block of
 * exactly its size, so a sanitizer build catches any read past the end. */
#include "../fuzz.h"
#include "net/auth.h"
#include "net/cache.h"
#include "net/dataurl.h"
#include "net/headers.h"
#include "net/hsts.h"
#include "net/http_parse.h"
#include "net/psl.h"
#include "net/roots.h"
#include "net/tls.h"
#include "fakenet.h"
#include "tls_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rng_state = 1;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

typedef struct Input {
    uint8_t *d;
    size_t n, cap;
} Input;

static const char dict[] = "\r\n:;,= \t\"%0123456789abcdefABCDEF-/.chunkedTransfer-EncodingContent-Length"
                           "HTTP/1.1 200 \x00\xff#base64data:xn--";

static void mutate(Input *in, size_t maxlen)
{
    int n = 1 + (int)(rnd() % 8);
    for (int k = 0; k < n; k++) {
        size_t pos = in->n ? rnd() % (in->n + 1) : 0;
        switch (rnd() % 6) {
        case 0:
            if (in->n)
                in->d[rnd() % in->n] ^= (uint8_t)(1u << (rnd() % 8));
            break;
        case 1:
        case 2:
            if (in->n < maxlen) {
                memmove(in->d + pos + 1, in->d + pos, in->n - pos);
                in->d[pos] = (uint8_t)dict[rnd() % (sizeof dict - 1)];
                in->n++;
            }
            break;
        case 3:
            if (in->n && pos < in->n) {
                size_t del = 1 + rnd() % 8;
                if (del > in->n - pos)
                    del = in->n - pos;
                memmove(in->d + pos, in->d + pos + del, in->n - pos - del);
                in->n -= del;
            }
            break;
        case 4: /* duplicate a span */
            if (in->n) {
                size_t from = rnd() % in->n, len = 1 + rnd() % 32;
                if (len > in->n - from)
                    len = in->n - from;
                if (in->n + len <= maxlen) {
                    memmove(in->d + pos + len, in->d + pos, in->n - pos);
                    memmove(in->d + pos, in->d + (from >= pos ? from + len : from), len);
                    in->n += len;
                }
            }
            break;
        default:
            if (in->n)
                in->d[rnd() % in->n] = (uint8_t)rnd();
        }
    }
}

static void load_seed(Input *in, const void *s, size_t n)
{
    memcpy(in->d, s, n);
    in->n = n;
}

/* An exact-size heap copy, NUL-terminated past the end only when asked. */
static uint8_t *exact(const Input *in, bool nul)
{
    uint8_t *p = malloc(in->n + (nul ? 1 : 0) + (in->n || nul ? 0 : 1));
    memcpy(p, in->d, in->n);
    if (nul)
        p[in->n] = 0;
    return p;
}

/* ---- targets ---- */
static const char *http_seeds[] = {
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-T: 1\r\n\r\n",
    "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n",
    "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\nclose-delimited body",
    "HTTP/1.1 301 Moved\r\nLocation: /x\r\nContent-Length: 0\r\nSet-Cookie: a=b\r\nSet-Cookie: c=d\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc",
};

static void run_http(const Input *in, bool head)
{
    uint8_t *p = exact(in, false);
    /* Whole, and in random pieces: the body bytes must match. */
    AuBuf whole, split;
    au_buf_init(&whole);
    au_buf_init(&split);
    int results[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
        AuHttpParser hp;
        au_http_parser_init(&hp, head);
        AuBuf *body = pass ? &split : &whole;
        size_t off = 0;
        int r = AU_HP_NEED_MORE;
        int guard = 0;
        while (off < in->n && guard++ < 100000) {
            size_t piece = pass ? 1 + rnd() % 17 : in->n - off;
            if (piece > in->n - off)
                piece = in->n - off;
            /* Each piece in its own exact block too. */
            uint8_t *blk = malloc(piece);
            memcpy(blk, p + off, piece);
            size_t used = 0;
            AuHttpEvent ev = {0};
            r = au_http_parser_feed(&hp, blk, piece, &used, &ev);
            if (used > piece) {
                printf("parser consumed more than it was given\n");
                abort();
            }
            if (r == AU_HP_BODY) {
                if (ev.len > piece || ev.data < blk || ev.data + ev.len > blk + piece) {
                    printf("body event outside the input\n");
                    abort();
                }
                au_buf_append(body, ev.data, ev.len);
            }
            free(blk);
            off += used;
            if (r == AU_HP_ERROR || r == AU_HP_DONE)
                break;
        }
        if (r != AU_HP_ERROR && r != AU_HP_DONE)
            r = au_http_parser_eof(&hp);
        results[pass] = r;
        au_http_parser_free(&hp);
    }
    if (results[0] != results[1] || (results[0] == AU_HP_DONE &&
                                     (whole.len != split.len || (whole.len && memcmp(whole.data, split.data, whole.len))))) {
        printf("whole and split parses differ (%d/%d, %zu/%zu bytes)\n", results[0], results[1], whole.len,
               split.len);
        abort();
    }
    au_buf_free(&whole);
    au_buf_free(&split);
    free(p);
}

static const char *dataurl_seeds[] = {"data:,Hello%2C%20World!", "data:text/plain;base64,SGVsbG8=",
                                      "data:;charset=UTF-8,x", "data:image/png;BASE64,iVBORw0KGgo=",
                                      "data:text/html ; charset=\"utf-8\" ; base64 , PGI+"};

static void run_dataurl(const Input *in)
{
    char *p = (char *)exact(in, true);
    AuDataUrl d;
    au_data_url_init(&d);
    /* An invalid UTF-8 byte becomes U+FFFD (three bytes) in the URL. */
    if (au_data_url_decode(p, &d) && d.body.len > 3 * in->n) {
        printf("data URL body longer than its input\n");
        abort();
    }
    au_data_url_free(&d);
    free(p);
}

static const char *psl_seeds[] = {"www.example.co.uk", "a.b.c.kobe.jp", "user.github.io", "xn--85x722f.com.cn",
                                  "..", "com", "a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p.q.r.s.t.u.v.w.x.y.z"};

static void run_psl(const Input *in)
{
    char *p = (char *)exact(in, true);
    size_t n = strlen(p);
    size_t s = au_psl_suffix_len(p, n);
    if (s > n) {
        printf("suffix longer than host\n");
        abort();
    }
    const char *dom;
    size_t dl;
    if (au_psl_registrable(p, &dom, &dl) && (dom < p || dom + dl > p + n)) {
        printf("registrable domain outside the host\n");
        abort();
    }
    au_psl_is_public_suffix(p);
    free(p);
}

static void run_roots(const Input *in)
{
    uint8_t *p = exact(in, false);
    char err[128];
    AuRootStore *r = au_roots_load(p, in->n, err, sizeof err);
    if (r) {
        /* Every root's name can be looked up by a name of the input. */
        const uint8_t *der;
        size_t len, pos = 0;
        while (au_roots_next_by_subject(r, p, in->n < 64 ? in->n : 64, &pos, &der, &len)) {
        }
        (void)au_roots_count(r);
        au_roots_free(r);
    }
    free(p);
}

static void run_cert(const Input *in)
{
    uint8_t *p = exact(in, false);
    AuCertEntry e;
    au_cert_parse(p, in->n, &e);
    if (memchr(e.subject, 0, sizeof e.subject) == NULL || memchr(e.issuer, 0, sizeof e.issuer) == NULL ||
        memchr(e.san, 0, sizeof e.san) == NULL) {
        printf("unterminated certificate field\n");
        abort();
    }
    free(p);
}

/* caIssuers responses and the intermediates pool: names, the AIA URL, the
 * DER / PKCS#7 / PEM bundle reader, and the pool file. */
typedef struct AiaSink {
    const uint8_t *base;
    size_t n;
    bool pem;
    AuCertPool *pool;
    int calls;
} AiaSink;

static void aia_sink(void *ctx, const uint8_t *der, size_t len)
{
    AiaSink *s = ctx;
    s->calls++;
    if (!s->pem && (der < s->base || len > s->n || (size_t)(der - s->base) > s->n - len)) {
        printf("bundle certificate outside the input\n");
        abort();
    }
    au_cert_pool_add(s->pool, der, len);
}

static void run_aia(const Input *in)
{
    uint8_t *p = exact(in, false);
    AuDerSpan iss, subj;
    if (au_x509_names(p, in->n, &iss, &subj) &&
        (iss.p < p || iss.len > in->n || subj.p < p || subj.len > in->n || (size_t)(subj.p - p) > in->n - subj.len)) {
        printf("names outside the certificate\n");
        abort();
    }
    char url[64];
    if (au_x509_ca_issuers(p, in->n, url, sizeof url)) {
        size_t ul = strnlen(url, sizeof url);
        bool ok = ul < sizeof url && ul >= 7 && au_lower(url[0]) == 'h' && url[4] == ':';
        for (size_t i = 0; ok && i < ul; i++)
            ok = url[i] > 0x20 && url[i] < 0x7F;
        if (!ok) {
            printf("bad caIssuers URL\n");
            abort();
        }
    }
    AuCertPool pool;
    au_cert_pool_init(&pool);
    AiaSink s = {p, in->n, in->n > 0 && p[0] != 0x30, &pool, 0};
    int n = au_cert_bundle_parse(p, in->n, 4, aia_sink, &s);
    if (n != s.calls || n > 4) {
        printf("bundle count %d, calls %d\n", n, s.calls);
        abort();
    }
    AuBuf b;
    au_buf_init(&b);
    au_cert_pool_serialize(&pool, &b);
    AuCertPool back;
    au_cert_pool_init(&back);
    if (!au_cert_pool_parse(&back, b.data, b.len) || back.n != pool.n) {
        printf("pool round trip lost entries\n");
        abort();
    }
    au_cert_pool_parse(&back, p, in->n); /* hostile pool file */
    au_cert_pool_free(&back);
    au_cert_pool_free(&pool);
    au_buf_free(&b);
    free(p);
}

static void aia_seed(Input *in)
{
    int k = (int)(rnd() % 4);
    const TlsFixture *fx = &tls_fixtures[rnd() % TLS_FIX_COUNT];
    if (k == 0) {
        load_seed(in, fx->cert.data, fx->cert.data_len);
    } else if (k == 1) {
        load_seed(in, tls_fix_inter2_p7, tls_fix_inter2_p7_len);
    } else if (k == 2) {
        AuBuf b;
        au_buf_init(&b);
        au_buf_puts(&b, "-----BEGIN CERTIFICATE-----\n");
        au_base64_encode(fx->cert.data, fx->cert.data_len, &b);
        au_buf_puts(&b, "\n-----END CERTIFICATE-----\n");
        load_seed(in, b.data, b.len);
        au_buf_free(&b);
    } else {
        AuCertPool pool;
        au_cert_pool_init(&pool);
        au_cert_pool_add(&pool, fx->cert.data, fx->cert.data_len);
        au_cert_pool_add(&pool, tls_fixtures[TLS_FIX_INTER].cert.data, tls_fixtures[TLS_FIX_INTER].cert.data_len);
        AuBuf b;
        au_buf_init(&b);
        au_cert_pool_serialize(&pool, &b);
        load_seed(in, b.data, b.len);
        au_buf_free(&b);
        au_cert_pool_free(&pool);
    }
}

static const char *hsts_seeds[] = {"max-age=31536000; includeSubDomains; preload", "max-age=\"0\"",
                                   "# Aura HSTS 1\na.example\t1900000000\t1\nb.example\t1900000000\t0\n"};

static void run_hsts(const Input *in)
{
    char *p = (char *)exact(in, true);
    int64_t ma;
    bool sub;
    au_hsts_parse(p, &ma, &sub);
    AuHsts *h = au_hsts_new();
    au_hsts_process(h, "fuzz.example", p, 1800000000);
    au_hsts_parse_file(h, p, in->n, 1800000000);
    AuBuf b;
    au_buf_init(&b);
    au_hsts_serialize(h, &b, 1800000000);
    au_buf_free(&b);
    au_hsts_free(h);
    free(p);
}

static const char *auth_seeds[] = {
    "Basic realm=\"x\"", "Digest realm=\"r\", nonce=\"n\", qop=\"auth,auth-int\", algorithm=SHA-256, opaque=\"o\"",
    "Digest realm=r,nonce=\"a\\\"b\",stale=TRUE,userhash=true", "Newauth realm=\"apps\", type=1, Basic realm=\"s\""};

static void run_auth(const Input *in)
{
    char *p = (char *)exact(in, true);
    AuAuthParams a;
    if (au_auth_parse(p, &a)) {
        AuBuf out;
        au_buf_init(&out);
        au_auth_basic("user", "pass", &out);
        au_auth_digest(&a, "user", "pass", "GET", "/x", 1, "0a4f113b", &out);
        au_buf_free(&out);
    }
    AuBuf dec;
    au_buf_init(&dec);
    au_base64_decode(p, in->n, &dec);
    au_buf_free(&dec);
    free(p);
}

static const char *date_seeds[] = {"Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT",
                                   "Sun Nov  6 08:49:37 1994", "Thu, 01-Jan-2026 00:00:00 GMT", "31 Dec 70 23:59:59"};
static const char *mime_seeds[] = {"text/html; charset=UTF-8", "text/plain;charset=\"utf-8\";format=flowed",
                                   "multipart/form-data; boundary=\"---x\"", "*/*"};

static void run_dates(const Input *in)
{
    char *p = (char *)exact(in, true);
    int64_t t;
    if (au_http_parse_date(p, &t)) {
        char buf[32];
        au_http_format_date(t, buf);
    }
    AuBuf e, c;
    au_buf_init(&e);
    au_buf_init(&c);
    au_http_parse_mime(p, &e, &c);
    au_buf_free(&e);
    au_buf_free(&c);
    free(p);
}

/* Cache entries: a serialized seed entry, mutated. */
static void cache_seed(Input *in)
{
    AuHttpCache *c = au_cache_new(1 << 20);
    AuHeaders req, resp;
    au_headers_init(&req);
    au_headers_init(&resp);
    au_headers_add(&req, "Accept", "text/html");
    au_headers_add(&resp, "Date", "Fri, 15 Jan 2027 08:00:00 GMT");
    au_headers_add(&resp, "Cache-Control", "max-age=60");
    au_headers_add(&resp, "ETag", "\"abc\"");
    au_headers_add(&resp, "Vary", "Accept");
    AuCacheWriter *w = au_cache_begin(c, "https://x.test/a?b", &req, 200, "OK", &resp, 1800000000, 1800000000);
    au_cache_write(w, "<p>cached body</p>", 18);
    au_cache_commit(w);
    AuCacheEntry *e;
    au_cache_lookup(c, "https://x.test/a?b", &req, 1800000000, &e);
    AuBuf b;
    au_buf_init(&b);
    au_cache_entry_serialize(e, &b);
    au_cache_entry_release(c, e);
    load_seed(in, b.data, b.len);
    au_buf_free(&b);
    au_headers_free(&req);
    au_headers_free(&resp);
    au_cache_free(c);
}

static void run_cache(const Input *in)
{
    uint8_t *p = exact(in, false);
    AuCacheEntry *e = au_cache_entry_parse(p, in->n);
    if (e) {
        size_t n;
        au_cache_entry_body(e, &n);
        au_cache_entry_etag(e);
        au_cache_freshness_lifetime(au_cache_entry_status(e), au_cache_entry_headers(e));
        AuBuf b;
        au_buf_init(&b);
        au_cache_entry_serialize(e, &b);
        au_buf_free(&b);
        au_cache_entry_free(e);
    }
    free(p);
}

#define N(a) (sizeof(a) / sizeof *(a))

/* ---- The TLS handshake, through tls.c ----
 *
 * A request over the fake transport to the in-process TLS server, with
 * the server's bytes damaged on the way to the client: bit flips, bytes
 * replaced, at a rate drawn per run from "one early byte" to "most
 * bytes". TLS 1.2 sends its ServerHello, certificates and key exchange
 * in the clear, so damage there reaches the library's handshake and
 * X.509 parsers and tls.c's chain handling; in TLS 1.3 only the
 * ServerHello is in the clear and the rest fails its integrity check.
 * Every request must end, and one whose bytes were not touched must
 * succeed. */

typedef struct Mangle {
    uint32_t rate; /* one byte in rate is damaged; 0 for none */
    size_t skip;   /* bytes to pass before any damage */
    size_t seen, damaged;
} Mangle;

static void mangle(void *ctx, uint8_t *data, size_t len)
{
    Mangle *m = ctx;
    for (size_t i = 0; i < len; i++, m->seen++) {
        if (!m->rate || m->seen < m->skip || rnd() % m->rate)
            continue;
        uint32_t r = rnd();
        data[i] = (r & 3) == 0 ? (uint8_t)(r >> 8) : (uint8_t)(data[i] ^ (1u << ((r >> 4) & 7)));
        m->damaged++;
    }
}

typedef struct TlsDone {
    bool done;
    AuNetError err;
} TlsDone;

static void tls_done(void *ctx, AuHttpTransfer *t, AuNetError e)
{
    (void)t;
    ((TlsDone *)ctx)->done = true;
    ((TlsDone *)ctx)->err = e;
}

static void run_tls(long *completed, long *failed)
{
    static const int leaves[] = {TLS_FIX_VALID, TLS_FIX_VALID_RSA, TLS_FIX_VALID_P384, TLS_FIX_AIA_LEAF,
                                 TLS_FIX_EXPIRED, TLS_FIX_WRONGHOST};
    static const uint32_t rates[] = {0, 2, 16, 64, 256, 1024, 4096};
    FakeNet f;
    fake_init(&f);
    AuRootStore *roots = au_roots_new(1700000000LL);
    au_roots_add_der(roots, tls_fixtures[TLS_FIX_CA].cert.data, tls_fixtures[TLS_FIX_CA].cert.data_len);
    AuNet *net = au_net_new(&f.ops);
    au_net_set_roots(net, roots);
    FakeServer *s = fake_server(&f, "tls.test", 443);
    int fix = leaves[rnd() % N(leaves)];
    TlsCert chain[2] = {tls_fixtures[fix].cert, tls_fixtures[TLS_FIX_INTER].cert};
    s->tls = true;
    s->chain = chain;
    s->chain_len = fix == TLS_FIX_AIA_LEAF && (rnd() & 1) ? 2 : 1;
    s->key = &tls_fixtures[fix].key;
    s->tls_max = rnd() % 3 ? 12 : 13;
    s->p256_only = (rnd() & 7) == 0;
    Mangle m = {rates[rnd() % N(rates)], rnd() % 3 ? rnd() % 1600 : 0, 0, 0};
    s->mangle = mangle;
    s->mangle_ctx = &m;
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    TlsDone d = {false, AU_NET_OK};
    AuHttpRequest rq;
    au_http_request_init(&rq, "https://tls.test/");
    AuHttpCallbacks cb = {&d, NULL, NULL, tls_done, NULL, NULL};
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(&f, net, 3000, 50);
    if (!d.done) {
        printf("tls: a request did not end (damage rate %u after %lu bytes)\n", (unsigned)m.rate,
               (unsigned long)m.skip);
        abort();
    }
    bool want_ok = fix != TLS_FIX_EXPIRED && fix != TLS_FIX_WRONGHOST && !(fix == TLS_FIX_AIA_LEAF && s->chain_len == 1);
    if (m.damaged == 0 && want_ok && d.err != AU_NET_OK) {
        printf("tls: an undamaged handshake failed: %s\n", au_net_error_name(d.err));
        abort();
    }
    if (m.damaged == 0 && !want_ok && d.err == AU_NET_OK) {
        printf("tls: a bad certificate was accepted\n");
        abort();
    }
    if (d.err == AU_NET_OK)
        (*completed)++;
    else
        (*failed)++;
    au_net_free(net);
    au_roots_free(roots);
    fake_free(&f);
}

typedef struct Target {
    const char *name;
    const char **seeds;
    size_t nseeds;
    size_t maxlen;
} Target;


int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: fuzz_net TARGET ITERATIONS SEED [ROOTS_FILE]\n"
                        "       fuzz_net tls ITERATIONS SEED [SECONDS]\n");
        return 2;
    }
    const char *target = argv[1];
    long iters = atol(argv[2]);
    rng_state = (uint32_t)atol(argv[3]);
    if (!rng_state)
        rng_state = 1;
    if (!strcmp(target, "tls")) {
        /* Its own loop: it is not part of "all" (a handshake costs far
         * more than a parser run), and it stops at the time cap. */
        long cap = argc > 4 ? atol(argv[4]) : 0, n = 0, completed = 0, failed = 0;
        time_t t0 = time(NULL);
        for (; n < iters && (!cap || time(NULL) - t0 < cap); n++)
            run_tls(&completed, &failed);
        printf("tls: %ld handshakes fuzzed (%ld completed, %ld failed)\n", n, completed, failed);
        return 0;
    }
    bool all = !strcmp(target, "all");
    Input in;
    in.cap = 1 << 18;
    in.d = malloc(in.cap);
    in.n = 0;

    /* Roots: seed from the file when given. */
    uint8_t *roots_data = NULL;
    size_t roots_len = 0;
    if (argc > 4) {
        FILE *f = fopen(argv[4], "rb");
        if (f) {
            roots_data = malloc(in.cap);
            roots_len = fread(roots_data, 1, in.cap, f);
            fclose(f);
        }
    }
    long done = 0;
    for (long it = 0; it < iters; it++) {
        if (fz_capped()) { /* AURA_FUZZ_SECONDS */
            iters = it;
            break;
        }
        int which = all ? (int)(rnd() % 11) : -1;
        if ((all && which == 10) || !strcmp(target, "aia")) {
            aia_seed(&in);
            mutate(&in, 8192);
            run_aia(&in);
            done++;
            continue;
        }
        if ((all && which == 9) || !strcmp(target, "cache")) {
            cache_seed(&in);
            mutate(&in, 4096);
            run_cache(&in);
            done++;
            continue;
        }
        if ((all && which == 0) || !strcmp(target, "http")) {
            const char *s = http_seeds[rnd() % N(http_seeds)];
            load_seed(&in, s, strlen(s));
            mutate(&in, 4096);
            run_http(&in, (rnd() % 8) == 0);
        } else if ((all && which == 1) || !strcmp(target, "dataurl")) {
            const char *s = dataurl_seeds[rnd() % N(dataurl_seeds)];
            load_seed(&in, s, strlen(s));
            mutate(&in, 4096);
            run_dataurl(&in);
        } else if ((all && which == 2) || !strcmp(target, "psl")) {
            const char *s = psl_seeds[rnd() % N(psl_seeds)];
            load_seed(&in, s, strlen(s));
            mutate(&in, 512);
            run_psl(&in);
        } else if ((all && which == 3) || !strcmp(target, "roots")) {
            if (roots_data) {
                load_seed(&in, roots_data, roots_len);
                /* Keep runs fast: often truncate. */
                if (rnd() & 1)
                    in.n = rnd() % (in.n + 1);
            } else {
                load_seed(&in, "AURR\x00\x01", 6);
            }
            mutate(&in, in.cap);
            run_roots(&in);
        } else if ((all && which == 4) || !strcmp(target, "cert")) {
            const TlsFixture *fx = &tls_fixtures[rnd() % TLS_FIX_COUNT];
            load_seed(&in, fx->cert.data, fx->cert.data_len);
            mutate(&in, 8192);
            run_cert(&in);
        } else if ((all && which == 5) || !strcmp(target, "hsts")) {
            const char *s = hsts_seeds[rnd() % N(hsts_seeds)];
            load_seed(&in, s, strlen(s));
            mutate(&in, 2048);
            run_hsts(&in);
        } else if ((all && which == 6) || !strcmp(target, "auth")) {
            const char *s = auth_seeds[rnd() % N(auth_seeds)];
            load_seed(&in, s, strlen(s));
            mutate(&in, 2048);
            run_auth(&in);
        } else if ((all && which >= 7) || !strcmp(target, "dates") || !strcmp(target, "mime")) {
            const char *s = (rnd() & 1) ? date_seeds[rnd() % N(date_seeds)] : mime_seeds[rnd() % N(mime_seeds)];
            load_seed(&in, s, strlen(s));
            mutate(&in, 512);
            run_dates(&in);
        } else {
            fprintf(stderr, "unknown target %s\n", target);
            return 2;
        }
        done++;
    }
    printf("fuzz_net %s: %ld iterations, seed %s\n", target, done, argv[3]);
    free(in.d);
    free(roots_data);
    return 0;
}
