/* Fake network for tests. See fakenet.h. */
#include "fakenet.h"

#include "net/tls_platform.h"

#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cache.h"
#include "mbedtls/ssl_ticket.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeSock {
    FakeServer *srv;
    bool special; /* tls, banner and connect_hang apply (see FakeServer.special_port) */
    int state; /* 0 connecting, 1 open */
    AuBuf req;      /* plaintext from the client, not yet a whole request */
    AuBuf resp;     /* plaintext to the client */
    size_t resp_off;
    size_t chunk;
    bool paced;           /* at most one chunk per clock tick */
    uint32_t last_tick;
    bool close_when_drained, reset_when_drained, closed, reset;
    /* TLS server side */
    struct FakeTls *sc;
    AuBuf s2c; /* ciphertext to the client */
    size_t s2c_off;
    AuBuf c2s; /* ciphertext from the client, not yet read by the server */
    size_t c2s_off;
    bool first_record_seen;
} FakeSock;

/* One connection's server. */
typedef struct FakeTls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    bool hs_done, dead;
    int hellos; /* ClientHello records seen */
    uint16_t groups[2];
} FakeTls;

/* What a server keeps between connections. */
typedef struct FakeTlsServer {
    mbedtls_ssl_cache_context cache;
    mbedtls_ssl_ticket_context tickets;
} FakeTlsServer;

static int srv_send(void *ctx, const unsigned char *buf, size_t len)
{
    FakeSock *k = ctx;
    au_buf_append(&k->s2c, buf, len);
    return (int)len;
}

static int srv_recv(void *ctx, unsigned char *buf, size_t len)
{
    FakeSock *k = ctx;
    size_t have = k->c2s.len - k->c2s_off;
    if (!have)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (len > have)
        len = have;
    memcpy(buf, k->c2s.data + k->c2s_off, len);
    k->c2s_off += len;
    if (k->c2s_off == k->c2s.len)
        k->c2s_off = k->c2s.len = 0;
    return (int)len;
}

void fake_tls_start(void)
{
    static const uint8_t seed[32] = {0x41, 0x75, 0x72, 0x61, 1, 2, 3, 4};
    au_tls_entropy_feed(seed, sizeof seed);
}

/* Sets up the server side of a connection. False when the library
 * refuses the certificate or key (a broken fixture). */
static bool srv_start(FakeSock *k)
{
    FakeServer *s = k->srv;
    FakeTls *t = calloc(1, sizeof *t);
    k->sc = t;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->crt);
    mbedtls_pk_init(&t->pk);
    fake_tls_start();
    bool ok = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    for (size_t i = 0; ok && i < s->chain_len; i++)
        ok = mbedtls_x509_crt_parse_der(&t->crt, s->chain[i].data, s->chain[i].data_len) == 0;
    ok = ok && mbedtls_pk_parse_key(&t->pk, s->key->der, s->key->len, NULL, 0) == 0;
    ok = ok && mbedtls_ssl_conf_own_cert(&t->conf, &t->crt, &t->pk) == 0;
    if (!ok) {
        /* A broken fixture, or the allocation failures a test injects:
         * the connection closes and the client's request fails. */
        t->dead = true;
        return false;
    }
    mbedtls_ssl_conf_min_tls_version(&t->conf, s->tls_min == 13 ? MBEDTLS_SSL_VERSION_TLS1_3
                                                                 : MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t->conf, s->tls_max == 12 ? MBEDTLS_SSL_VERSION_TLS1_2
                                                                 : MBEDTLS_SSL_VERSION_TLS1_3);
    if (s->alpn_http11) {
        static const char *const protos[] = {"http/1.1", NULL};
        mbedtls_ssl_conf_alpn_protocols(&t->conf, protos);
    }
    if (s->rsa_kex) {
        static const int rsa_only[] = {MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
                                       MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384, 0};
        static const int rsa_too[] = {MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
                                      MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
                                      MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
                                      MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
                                      MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
                                      MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384, 0};
        mbedtls_ssl_conf_ciphersuites(&t->conf, s->rsa_kex == 1 ? rsa_only : rsa_too);
        if (s->rsa_kex == 1)
            mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    }
    if (s->p256_only) {
        t->groups[0] = MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1;
        t->groups[1] = MBEDTLS_SSL_IANA_TLS_GROUP_NONE;
        mbedtls_ssl_conf_groups(&t->conf, t->groups);
    }
    if (!s->no_resume) {
        if (!s->tls_state) {
            s->tls_state = calloc(1, sizeof *s->tls_state);
            mbedtls_ssl_cache_init(&s->tls_state->cache);
            mbedtls_ssl_ticket_init(&s->tls_state->tickets);
            /* On failure (injected allocation failures) the server issues
             * no usable tickets and handshakes are full ones. */
            (void)mbedtls_ssl_ticket_setup(&s->tls_state->tickets, PSA_ALG_GCM, PSA_KEY_TYPE_AES, 256, 86400);
        }
        mbedtls_ssl_conf_session_cache(&t->conf, &s->tls_state->cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
        mbedtls_ssl_conf_session_tickets_cb(&t->conf, mbedtls_ssl_ticket_write, mbedtls_ssl_ticket_parse,
                                            &s->tls_state->tickets);
        mbedtls_ssl_conf_new_session_tickets(&t->conf, 1);
    } else {
        mbedtls_ssl_conf_new_session_tickets(&t->conf, 0);
    }
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0) {
        t->dead = true;
        return false;
    }
    mbedtls_ssl_set_bio(&t->ssl, k, srv_send, srv_recv, NULL);
    return true;
}

static void srv_free(FakeSock *k)
{
    if (!k->sc)
        return;
    mbedtls_ssl_free(&k->sc->ssl);
    mbedtls_ssl_config_free(&k->sc->conf);
    mbedtls_x509_crt_free(&k->sc->crt);
    mbedtls_pk_free(&k->sc->pk);
    free(k->sc);
    k->sc = NULL;
}

/* The attacker of FakeServer.strip_tls13: in a ClientHello record, turns
 * the TLS 1.3 entry of supported_versions into TLS 1.2. */
static void strip_tls13(uint8_t *rec, size_t len)
{
    if (len < 5 + 4 + 2 + 32 + 1 || rec[0] != 22 || rec[5] != 1)
        return;
    size_t p = 5 + 4 + 2 + 32;
    p += 1 + (size_t)rec[p]; /* session id */
    if (p + 2 > len)
        return;
    p += 2 + (((size_t)rec[p] << 8) | rec[p + 1]); /* cipher suites */
    if (p + 1 > len)
        return;
    p += 1 + (size_t)rec[p]; /* compression */
    if (p + 2 > len)
        return;
    size_t end = p + 2 + (((size_t)rec[p] << 8) | rec[p + 1]);
    p += 2;
    if (end > len)
        end = len;
    while (p + 4 <= end) {
        unsigned type = ((unsigned)rec[p] << 8) | rec[p + 1];
        size_t el = ((size_t)rec[p + 2] << 8) | rec[p + 3];
        if (type == 0x002b && p + 4 + el <= end) {
            for (size_t i = p + 5; i + 1 < p + 4 + el; i += 2)
                if (rec[i] == 3 && rec[i + 1] == 4)
                    rec[i + 1] = 3;
            return;
        }
        p += 4 + el;
    }
}

static FakeNet *fn(void *ctx) { return ctx; }

static FakeServer *find_server(FakeNet *f, const char *host)
{
    for (int i = 0; i < f->nservers; i++)
        if (!strcmp(f->servers[i].host, host))
            return &f->servers[i];
    return NULL;
}

/* Resolve: the address encodes the server index. */
static uint32_t f_resolve_start(void *ctx, const char *host)
{
    FakeNet *f = fn(ctx);
    FakeServer *s = find_server(f, host);
    uint32_t id = ++f->resolve_next;
    (void)s;
    return id | ((uint32_t)(s ? (s - f->servers) + 1 : 0) << 16);
}

static int f_resolve_poll(void *ctx, uint32_t req, AuNetAddr *addr)
{
    FakeNet *f = fn(ctx);
    int idx = (int)(req >> 16);
    if (idx == 0 || f->servers[idx - 1].dns_fail)
        return -1;
    memset(addr, 0, sizeof *addr);
    addr->family = 4;
    addr->addr[0] = 10;
    addr->addr[3] = (uint8_t)idx;
    return 1;
}

static void f_resolve_cancel(void *ctx, uint32_t req) { (void)ctx; (void)req; }

static uint32_t f_connect(void *ctx, const AuNetAddr *addr, uint16_t port)
{
    FakeNet *f = fn(ctx);
    int idx = addr->addr[3];
    if (idx < 1 || idx > f->nservers)
        return 0;
    FakeServer *s = &f->servers[idx - 1];
    if (s->refuse || (s->port && s->port != port))
        return 0;
    for (uint32_t i = 1; i < FAKE_MAX_SOCKS; i++) {
        if (f->socks[i])
            continue;
        FakeSock *k = calloc(1, sizeof *k);
        k->srv = s;
        k->special = !s->special_port || s->special_port == port;
        au_buf_init(&k->req);
        au_buf_init(&k->resp);
        au_buf_init(&k->s2c);
        s->connections++;
        f->open_socks++;
        au_buf_init(&k->c2s);
        if (s->tls && k->special) {
            srv_start(k);
        } else if (s->banner && k->special) {
            au_buf_puts(&k->resp, s->banner);
        }
        f->socks[i] = k;
        return i;
    }
    return 0;
}

static int f_connect_poll(void *ctx, uint32_t sock)
{
    FakeNet *f = fn(ctx);
    FakeSock *k = sock < FAKE_MAX_SOCKS ? f->socks[sock] : NULL;
    if (!k)
        return -1;
    if (k->srv->connect_hang && k->special)
        return 0;
    k->state = 1;
    return 1;
}

/* Length of a complete request at the start of b, or 0. */
static size_t request_len(const AuBuf *b)
{
    if (!b->len)
        return 0;
    const char *d = b->data;
    const char *end = NULL;
    for (size_t i = 0; i + 3 < b->len; i++)
        if (!memcmp(d + i, "\r\n\r\n", 4)) {
            end = d + i + 4;
            break;
        }
    if (!end)
        return 0;
    size_t head = (size_t)(end - d);
    /* Content-Length or chunked */
    char *h = au_strndup(d, head);
    for (char *p = h; *p; p++)
        *p = (char)au_lower((unsigned char)*p);
    size_t total = head;
    char *cl = strstr(h, "\r\ncontent-length:");
    if (cl) {
        total += (size_t)strtoul(cl + 17, NULL, 10);
    } else if (strstr(h, "\r\ntransfer-encoding: chunked")) {
        const char *z = NULL;
        for (size_t i = head; i + 4 < b->len + 1; i++)
            if (b->len - i >= 5 && !memcmp(d + i, "0\r\n\r\n", 5) && (i == head || d[i - 1] == '\n')) {
                z = d + i + 5;
                break;
            }
        au_free(h);
        return z ? (size_t)(z - d) : 0;
    }
    au_free(h);
    return b->len >= total ? total : 0;
}

/* Takes complete requests and queues responses. */
static void serve(FakeSock *k)
{
    for (;;) {
        size_t n = request_len(&k->req);
        if (!n)
            return;
        FakeServer *s = k->srv;
        if (s->nreq < FAKE_MAX_RESP)
            s->requests[s->nreq++] = au_strndup(k->req.data, n);
        memmove(k->req.data, k->req.data + n, k->req.len - n);
        k->req.len -= n;
        if (s->next >= s->nresp) {
            k->closed = true;
            return;
        }
        FakeResponse *r = &s->resp[s->next++];
        if (r->close_before) {
            k->closed = true;
            return;
        }
        if (r->silent)
            return;
        au_buf_append(&k->resp, r->data, r->len ? r->len : strlen(r->data));
        k->chunk = r->chunk;
        k->paced = r->paced;
        k->close_when_drained = r->close_after;
        k->reset_when_drained = r->reset_after;
    }
}

static void tls_pump(FakeSock *k)
{
    FakeTls *t = k->sc;
    FakeServer *s = k->srv;
    for (int round = 0; round < 64 && !t->dead; round++) {
        bool progress = false;
        if (!t->hs_done) {
            int rc = mbedtls_ssl_handshake(&t->ssl);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
                return;
            if (rc != 0) {
                t->dead = true; /* the alert, if any, is in s2c */
                return;
            }
            t->hs_done = true;
            s->handshakes++;
            s->last_version = mbedtls_ssl_get_version_number(&t->ssl) == MBEDTLS_SSL_VERSION_TLS1_3 ? 13 : 12;
            s->last_hrr = t->hellos > 1;
            snprintf(s->last_suite, sizeof s->last_suite, "%s", mbedtls_ssl_get_ciphersuite(&t->ssl));
            progress = true;
        }
        unsigned char buf[4096];
        int n = mbedtls_ssl_read(&t->ssl, buf, sizeof buf);
        if (n > 0) {
            au_buf_append(&k->req, buf, (size_t)n);
            serve(k);
            progress = true;
        } else if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
            t->dead = true; /* close_notify, or an error */
            return;
        }
        while (k->resp.len > k->resp_off) {
            size_t want = k->resp.len - k->resp_off;
            int w = mbedtls_ssl_write(&t->ssl, (const unsigned char *)k->resp.data + k->resp_off, want);
            if (w <= 0)
                break;
            k->resp_off += (size_t)w;
            progress = true;
        }
        if (!progress)
            return;
    }
}

static long f_send(void *ctx, uint32_t sock, const void *data, size_t len)
{
    FakeNet *f = fn(ctx);
    FakeSock *k = sock < FAKE_MAX_SOCKS ? f->socks[sock] : NULL;
    if (!k || k->reset)
        return AU_NETIO_ERROR;
    if (k->closed)
        return (long)len; /* swallowed; the close shows up on recv */
    if (k->sc) {
        if (k->sc->dead)
            return (long)len; /* swallowed; the close shows up on recv */
        size_t at = k->c2s.len;
        au_buf_append(&k->c2s, data, len);
        /* Before the handshake ends, look at each record the client sent
         * for a ClientHello (the client sends whole records per call). */
        if (!k->sc->hs_done) {
            uint8_t *d = (uint8_t *)k->c2s.data + at;
            for (size_t o = 0; o + 6 <= len;) {
                size_t rl = ((size_t)d[o + 3] << 8) | d[o + 4];
                if (d[o] == 22 && d[o + 5] == 1) {
                    k->sc->hellos++;
                    if (k->srv->strip_tls13)
                        strip_tls13(d + o, len - o < 5 + rl ? len - o : 5 + rl);
                }
                o += 5 + rl;
            }
        }
        tls_pump(k);
        return (long)len;
    }
    au_buf_append(&k->req, data, len);
    serve(k);
    return (long)len;
}

static long f_recv(void *ctx, uint32_t sock, void *buf, size_t cap)
{
    FakeNet *f = fn(ctx);
    FakeSock *k = sock < FAKE_MAX_SOCKS ? f->socks[sock] : NULL;
    if (!k)
        return AU_NETIO_ERROR;
    if (k->reset)
        return AU_NETIO_ERROR;
    if (k->sc) {
        tls_pump(k);
        size_t n = k->s2c.len - k->s2c_off;
        if (n) {
            if (n > cap)
                n = cap;
            memcpy(buf, k->s2c.data + k->s2c_off, n);
            k->s2c_off += n;
            if (k->srv->mangle)
                k->srv->mangle(k->srv->mangle_ctx, buf, n);
            return (long)n;
        }
        if (k->resp.len == k->resp_off && (k->close_when_drained || k->closed))
            return AU_NETIO_CLOSED;
        if (k->resp.len == k->resp_off && k->reset_when_drained) {
            k->reset = true;
            return AU_NETIO_ERROR;
        }
        if (k->sc->dead)
            return AU_NETIO_CLOSED;
        return AU_NETIO_WOULD_BLOCK;
    }
    size_t n = k->resp.len - k->resp_off;
    if (n && k->paced) {
        if (k->last_tick == f->now + 1)
            return AU_NETIO_WOULD_BLOCK;
        k->last_tick = f->now + 1;
    }
    if (n) {
        if (k->chunk && n > k->chunk)
            n = k->chunk;
        if (n > cap)
            n = cap;
        memcpy(buf, k->resp.data + k->resp_off, n);
        k->resp_off += n;
        return (long)n;
    }
    if (k->closed || k->close_when_drained)
        return AU_NETIO_CLOSED;
    if (k->reset_when_drained) {
        k->reset = true;
        return AU_NETIO_ERROR;
    }
    return AU_NETIO_WOULD_BLOCK;
}

static void f_close(void *ctx, uint32_t sock)
{
    FakeNet *f = fn(ctx);
    FakeSock *k = sock < FAKE_MAX_SOCKS ? f->socks[sock] : NULL;
    if (!k)
        return;
    au_buf_free(&k->req);
    au_buf_free(&k->resp);
    au_buf_free(&k->s2c);
    au_buf_free(&k->c2s);
    srv_free(k);
    free(k);
    f->socks[sock] = NULL;
    f->open_socks--;
}

static uint32_t f_now(void *ctx) { return fn(ctx)->now; }
static int64_t f_wall(void *ctx) { return fn(ctx)->wall; }

static bool f_entropy(void *ctx, void *buf, size_t len)
{
    FakeNet *f = fn(ctx);
    if (!f->entropy_ready)
        return false;
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++)
        p[i] = (uint8_t)(f->entropy_counter++ * 167 + 13);
    return true;
}

void fake_init(FakeNet *f)
{
    memset(f, 0, sizeof *f);
    f->ops.ctx = f;
    f->ops.resolve_start = f_resolve_start;
    f->ops.resolve_poll = f_resolve_poll;
    f->ops.resolve_cancel = f_resolve_cancel;
    f->ops.connect = f_connect;
    f->ops.connect_poll = f_connect_poll;
    f->ops.send = f_send;
    f->ops.recv = f_recv;
    f->ops.close = f_close;
    f->ops.now_ms = f_now;
    f->ops.wall_time = f_wall;
    f->ops.entropy = f_entropy;
    f->now = 1000;
    f->wall = 1800000000; /* 2027 */
    f->entropy_ready = true;
    fake_tls_start();
}

void fake_free(FakeNet *f)
{
    for (int i = 0; i < FAKE_MAX_SOCKS; i++)
        if (f->socks[i])
            f_close(f, (uint32_t)i);
    for (int i = 0; i < f->nservers; i++) {
        for (int j = 0; j < f->servers[i].nreq; j++)
            au_free(f->servers[i].requests[j]);
        if (f->servers[i].tls_state) {
            mbedtls_ssl_cache_free(&f->servers[i].tls_state->cache);
            mbedtls_ssl_ticket_free(&f->servers[i].tls_state->tickets);
            free(f->servers[i].tls_state);
            f->servers[i].tls_state = NULL;
        }
    }
}

FakeServer *fake_server(FakeNet *f, const char *host, uint16_t port)
{
    FakeServer *s = &f->servers[f->nservers++];
    memset(s, 0, sizeof *s);
    snprintf(s->host, sizeof s->host, "%s", host);
    s->port = port;
    return s;
}

FakeResponse *fake_add(FakeServer *s, const char *data)
{
    FakeResponse *r = &s->resp[s->nresp++];
    memset(r, 0, sizeof *r);
    r->data = data;
    return r;
}

void fake_respond(FakeServer *s, const char *data) { fake_add(s, data); }

int fake_run(FakeNet *f, AuNet *net, int max_rounds, uint32_t step_ms)
{
    int r = 0;
    for (; r < max_rounds; r++) {
        au_net_pump(net);
        if (!au_net_active(net))
            break;
        f->now += step_ms;
    }
    return r;
}
