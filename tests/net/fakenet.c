/* Fake network for tests. See fakenet.h. */
#include "fakenet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeSock {
    FakeServer *srv;
    int state; /* 0 connecting, 1 open */
    AuBuf req;      /* plaintext from the client, not yet a whole request */
    AuBuf resp;     /* plaintext to the client */
    size_t resp_off;
    size_t chunk;
    bool close_when_drained, reset_when_drained, closed, reset;
    /* TLS server side */
    br_ssl_server_context *sc;
    uint8_t *iobuf;
    AuBuf s2c; /* ciphertext to the client */
    size_t s2c_off;
} FakeSock;

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
        au_buf_init(&k->req);
        au_buf_init(&k->resp);
        au_buf_init(&k->s2c);
        s->connections++;
        f->open_socks++;
        if (s->tls) {
            k->sc = calloc(1, sizeof *k->sc);
            br_ssl_server_init_full_ec(k->sc, s->chain, s->chain_len, BR_KEYTYPE_EC, s->eckey);
            k->iobuf = malloc(BR_SSL_BUFSIZE_BIDI);
            br_ssl_engine_set_buffer(&k->sc->eng, k->iobuf, BR_SSL_BUFSIZE_BIDI, 1);
            uint8_t seed[32] = {1, 2, 3};
            seed[4] = (uint8_t)i;
            br_ssl_engine_inject_entropy(&k->sc->eng, seed, sizeof seed);
            br_ssl_server_reset(k->sc);
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
    if (k->srv->connect_hang)
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
        k->close_when_drained = r->close_after;
        k->reset_when_drained = r->reset_after;
    }
}

static void tls_pump(FakeSock *k)
{
    br_ssl_engine_context *e = &k->sc->eng;
    for (int round = 0; round < 64; round++) {
        bool progress = false;
        unsigned st = br_ssl_engine_current_state(e);
        if (st == BR_SSL_CLOSED)
            return;
        if (st & BR_SSL_RECVAPP) {
            size_t len;
            uint8_t *p = br_ssl_engine_recvapp_buf(e, &len);
            au_buf_append(&k->req, p, len);
            br_ssl_engine_recvapp_ack(e, len);
            serve(k);
            progress = true;
        }
        if ((st & BR_SSL_SENDAPP) && k->resp.len > k->resp_off) {
            size_t len;
            uint8_t *p = br_ssl_engine_sendapp_buf(e, &len);
            size_t n = k->resp.len - k->resp_off;
            if (n > len)
                n = len;
            memcpy(p, k->resp.data + k->resp_off, n);
            br_ssl_engine_sendapp_ack(e, n);
            k->resp_off += n;
            br_ssl_engine_flush(e, 0);
            progress = true;
        }
        if (st & BR_SSL_SENDREC) {
            size_t len;
            uint8_t *p = br_ssl_engine_sendrec_buf(e, &len);
            au_buf_append(&k->s2c, p, len);
            br_ssl_engine_sendrec_ack(e, len);
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
        const uint8_t *p = data;
        size_t left = len;
        while (left) {
            unsigned st = br_ssl_engine_current_state(&k->sc->eng);
            if (st == BR_SSL_CLOSED || !(st & BR_SSL_RECVREC)) {
                tls_pump(k);
                st = br_ssl_engine_current_state(&k->sc->eng);
                if (st == BR_SSL_CLOSED || !(st & BR_SSL_RECVREC))
                    break;
            }
            size_t cap;
            uint8_t *b = br_ssl_engine_recvrec_buf(&k->sc->eng, &cap);
            size_t n = left < cap ? left : cap;
            memcpy(b, p, n);
            br_ssl_engine_recvrec_ack(&k->sc->eng, n);
            p += n;
            left -= n;
            tls_pump(k);
        }
        return (long)(len - left);
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
            return (long)n;
        }
        if (k->resp.len == k->resp_off && (k->close_when_drained || k->closed))
            return AU_NETIO_CLOSED;
        if (k->resp.len == k->resp_off && k->reset_when_drained) {
            k->reset = true;
            return AU_NETIO_ERROR;
        }
        if (br_ssl_engine_current_state(&k->sc->eng) == BR_SSL_CLOSED)
            return AU_NETIO_CLOSED;
        return AU_NETIO_WOULD_BLOCK;
    }
    size_t n = k->resp.len - k->resp_off;
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
    free(k->sc);
    free(k->iobuf);
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
}

void fake_free(FakeNet *f)
{
    for (int i = 0; i < FAKE_MAX_SOCKS; i++)
        if (f->socks[i])
            f_close(f, (uint32_t)i);
    for (int i = 0; i < f->nservers; i++)
        for (int j = 0; j < f->servers[i].nreq; j++)
            au_free(f->servers[i].requests[j]);
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
