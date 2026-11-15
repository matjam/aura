/* The HTTP/1.1 client: transfers, connections and the pool, redirects,
 * authentication, proxies, cookies and HSTS on the wire, timeouts, and
 * the event loop pump. See net.h. */
#include "auth.h"
#include "../browser/credits.h"
#include "cache.h"
#include "cookies.h"
#include "hsts.h"
#include "http_parse.h"
#include "net.h"
#include "policy.h"
#include "roots.h"
#include "tls.h"
#include "../codec/brotli.h"
#include "../codec/codec.h"
#include "../url/url.h"

#include "tls_platform.h"

#include <stdio.h>
#include <string.h>

#define IN_MAX (64u * 1024)      /* plaintext received and not yet parsed, per connection */
#define OUT_LOW (16u * 1024)     /* refill the request body below this */
#define RECV_CHUNK 16384
#define MAX_DECODERS 3
#define DRAIN_MAX (256u * 1024)  /* redirect/401 bodies read and discarded up to this */
#define SESSION_CACHE 8
#define AUTH_CACHE 16
#define MAX_AUTH_ATTEMPTS 3
/* Fetching missing intermediates from caIssuers URLs (see tls.h). */
#define AIA_MAX_DEPTH 4          /* fetches per transfer */
#define AIA_MAX_BYTES 16384      /* per fetch */
#define AIA_TIMEOUT_MS 10000     /* per fetch */
#define AIA_SEEN 32              /* URLs remembered for the session */

/* ---- Structures ---- */

enum { C_RESOLVE, C_CONNECT, C_PROXY, C_TLS, C_READY, C_DEAD };

typedef struct Conn Conn;
struct Conn {
    Conn *next;
    AuNet *net;
    char *host;   /* origin host (for TLS and the pool key) */
    uint16_t port;
    bool tls;
    char *proxy;  /* proxy host when going through one, else NULL */
    uint16_t proxy_port;
    int state;
    uint32_t resolve_req, sock;
    AuTls *tls_engine;
    AuBuf in;
    size_t in_off;
    AuBuf out;
    size_t out_off;
    uint32_t started_ms, last_ms;
    AuHttpTransfer *owner;
    int served;
    bool eof;
    AuNetError err;
    AuHttpParser proxy_parser; /* CONNECT response */
    bool proxy_parser_live;
    bool session_saved;
    uint32_t id;      /* for the request log (au_http_trace) */
    AuNetAddr addr;   /* the address connected to (family 0 before DNS answers) */
};

enum { T_START, T_WAIT_CONN, T_SEND, T_RECV, T_DRAIN, T_WAIT_CRED, T_AIA, T_FAILED, T_FINISHED };

typedef struct AuthEntry {
    char *hostport, *realm, *user, *pass;
    AuAuthParams params;
    uint32_t nc;
    bool proxy;
} AuthEntry;

typedef struct SessionEntry {
    char *hostport;
    AuTlsSession s;
    AuCertInfo *cert; /* the full handshake's chain, for resumed connections */
} SessionEntry;

#define CERT_EXCEPTIONS 16

typedef struct CertException {
    char *host;
    uint8_t sha256[32];
} CertException;

struct AuHttpTransfer {
    AuHttpTransfer *next;
    AuNet *net;
    AuHttpCallbacks cb;
    void *user;
    int state;
    bool cancelled;
    AuNetError fail_err;
    /* request */
    char method[16];
    AuUrl url;
    char *original_url;
    AuHeaders extra;
    AuBodySource *body;
    AuRequestDest dest;
    AuRequestMode mode;
    char *referrer;
    AuReferrerPolicy rpolicy;
    char *top_level;
    bool user_activated, reload, omit_credentials, manual_redirect, allow_http_fallback;
    bool history; /* AuHttpRequest.history */
    bool no_cache; /* AuHttpRequest.no_cache */
    bool https_first_upgrade; /* this attempt is an HTTPS-first upgrade */
    bool was_upgraded;        /* some attempt of this transfer was an HTTPS-first upgrade */
    int64_t range_start, range_end;
    uint32_t total_timeout, started_ms, last_ms;
    /* progress */
    Conn *conn;
    bool reused;
    int retries;
    int redirects;
    bool body_done;
    bool head_sent;
    int64_t received;     /* body bytes as sent */
    size_t delivered;     /* decoded bytes */
    AuHttpParser parser;
    bool parser_live;
    bool got_bytes;       /* any response byte on this attempt */
    AuDecoder *dec[MAX_DECODERS];
    int ndec;
    bool uses_brotli;
    /* authentication */
    int auth_attempts, proxy_auth_attempts;
    AuAuthParams pending_auth;
    bool pending_proxy;
    /* response */
    AuHeaders resp_headers;
    char resp_reason[64];
    int status;
    AuBuf mime, charset, encoding;
    AuCertInfo cert;
    bool have_cert;
    AuSecurityInfo security;
    char cipher[64];
    char alpn[16];
    /* cache */
    AuCacheEntry *cache_entry; /* stale entry being revalidated */
    bool swr_background;       /* a stale-while-revalidate revalidation: no callbacks, cache_entry is marked */
    AuCacheWriter *cache_w;    /* the response being stored */
    int64_t request_wall;      /* when the request went out (seconds) */
    /* HTTPS-first: the error the https attempt failed with */
    AuNetError fallback_cause;
    bool tls_never_spoke;
    /* Missing intermediates: the caIssuers fetches this transfer made,
     * the one in progress, and its response. An AIA fetch is itself a
     * transfer (aia_internal) that never chases AIA or uses the cache. */
    bool aia_internal;
    int aia_rounds;
    char *aia_tried[AIA_MAX_DEPTH];
    char aia_url[256];
    AuHttpTrace trace;   /* au_http_trace: phase times, the connection used */
    char *request_head;  /* the request as sent, kept only with au_net_set_trace_headers */
    AuHttpTransfer *aia_child;
    bool aia_ready;  /* the fetch ended with certificates: connect again */
    bool aia_waited; /* another transfer fetched the URL: connect again */
    int aia_status;
    AuBuf aia_body;
};

typedef struct AiaSeen {
    char *url;
    bool ok; /* it gave certificates */
} AiaSeen;

struct AuNet {
    AuNetOps ops;
    AuNetConfig cfg;
    AuCookieJar *jar;
    AuHsts *hsts;
    AuHttpCache *cache;
    const AuRootStore *roots;
    AuHttpTransfer *transfers;
    Conn *conns;
    bool drbg_ready;     /* the TLS library's random generator has been seeded from the platform */
    uint32_t drbg_uses;  /* connections and nonces since the last seed material */
    SessionEntry sessions[SESSION_CACHE];
    int next_session;
    CertException exceptions[CERT_EXCEPTIONS];
    int next_exception;
    /* Hosts the user chose to visit over plain http after HTTPS-first's
     * upgrade failed; not upgraded again this session. Never saved. */
    char **http_ok;
    size_t n_http_ok;
    AuthEntry auth[AUTH_CACHE];
    int next_auth;
    /* Intermediates from caIssuers URLs, and the URLs tried this session. */
    AuCertPool pool;
    AiaSeen aia_seen[AIA_SEEN];
    int next_aia_seen;
    AuBrotliDict *brdict;
    int brdict_users;
    bool in_pump;
    bool trace_headers;    /* au_net_set_trace_headers */
    uint32_t next_conn_id;
    AuDns *dns; /* name lookups, cached and shared (dns.h) */
};

const char *au_net_error_name(AuNetError e)
{
    static const char *const names[] = {"ok",           "bad URL",          "unsupported scheme",
                                        "host not found", "connection failed", "timed out",
                                        "connection reset", "protocol error", "too large",
                                        "too many redirects", "content decoding failed", "TLS failure",
                                        "certificate error", "system clock is wrong", "proxy error",
                                        "HTTPS not available", "blocked", "cancelled"};
    return (unsigned)e < sizeof names / sizeof names[0] ? names[e] : "unknown";
}

void au_net_set_config_str(char **field, const char *value)
{
    au_free(*field);
    *field = au_strndup(value ? value : "", value ? strlen(value) : 0);
}

AuNet *au_net_new(const AuNetOps *ops)
{
    AuNet *net = au_calloc(sizeof *net);
    net->ops = *ops;
    net->dns = au_dns_new(ops);
    AuNetConfig *c = &net->cfg;
    au_net_set_config_str(&c->user_agent, AU_DEFAULT_USER_AGENT);
    au_net_set_config_str(&c->accept_language, "en-US,en;q=0.9");
    au_net_set_config_str(&c->http_proxy_host, "");
    au_net_set_config_str(&c->https_proxy_host, "");
    c->cookie_policy = AU_COOKIE_POLICY_ALL;
    c->block_third_party_cookies = true;
    c->max_per_host = 6;
    c->max_total = 16;
    c->connect_timeout_ms = 30000;
    c->idle_timeout_ms = 60000;
    c->keepalive_ms = 30000;
    c->max_redirects = 20;
    c->max_body = 64u * 1024 * 1024;
    return net;
}

AuNetConfig *au_net_config(AuNet *net) { return &net->cfg; }

void au_net_clear_dns(AuNet *net)
{
    au_dns_clear(net->dns);
}

void au_net_dns_stats(const AuNet *net, AuDnsStats *out)
{
    au_dns_stats(net->dns, out);
}
const AuNetOps *au_net_ops(AuNet *net) { return &net->ops; }
void au_net_set_cookie_jar(AuNet *net, AuCookieJar *jar) { net->jar = jar; }
void au_net_set_hsts(AuNet *net, AuHsts *hsts) { net->hsts = hsts; }
void au_net_set_cache(AuNet *net, AuHttpCache *cache) { net->cache = cache; }
AuCookieJar *au_net_cookie_jar(AuNet *net) { return net->jar; }
AuHsts *au_net_hsts(AuNet *net) { return net->hsts; }
AuHttpCache *au_net_cache(AuNet *net) { return net->cache; }
void au_net_set_roots(AuNet *net, const AuRootStore *roots) { net->roots = roots; }

static uint32_t now_ms(AuNet *net) { return net->ops.now_ms(net->ops.ctx); }
/* Milliseconds from since to now, wrap-safe; 0 when since is later (a
 * time stamp taken after now was read). */
static uint32_t elapsed(uint32_t now, uint32_t since)
{
    int32_t d = (int32_t)(now - since);
    return d > 0 ? (uint32_t)d : 0;
}
static int64_t wall(AuNet *net) { return net->ops.wall_time(net->ops.ctx); }

static void trace_init(AuHttpTransfer *t);
static void trace_note(AuHttpTransfer *t);

/* ---- Body sources ---- */

typedef struct BytesBody {
    uint8_t *data;
    size_t len, pos;
} BytesBody;

static long bytes_read(void *ctx, void *buf, size_t cap)
{
    BytesBody *b = ctx;
    size_t n = b->len - b->pos;
    if (n > cap)
        n = cap;
    memcpy(buf, b->data + b->pos, n);
    b->pos += n;
    return (long)n;
}

static bool bytes_rewind(void *ctx)
{
    ((BytesBody *)ctx)->pos = 0;
    return true;
}

static void bytes_free(void *ctx)
{
    BytesBody *b = ctx;
    au_free(b->data);
    au_free(b);
}

AuBodySource *au_body_from_bytes(const void *data, size_t len, const char *content_type)
{
    BytesBody *b = au_calloc(sizeof *b);
    b->data = au_alloc(len ? len : 1);
    memcpy(b->data, data, len);
    b->len = len;
    AuBodySource *s = au_calloc(sizeof *s);
    s->ctx = b;
    s->read = bytes_read;
    s->rewind = bytes_rewind;
    s->free = bytes_free;
    s->length = (int64_t)len;
    s->content_type = content_type ? au_strndup(content_type, strlen(content_type)) : NULL;
    return s;
}

void au_body_free(AuBodySource *b)
{
    if (!b)
        return;
    if (b->free)
        b->free(b->ctx);
    au_free(b->content_type);
    au_free(b);
}

/* ---- Requests ---- */

void au_http_request_init(AuHttpRequest *rq, const char *url)
{
    memset(rq, 0, sizeof *rq);
    rq->method = "GET";
    rq->url = url;
    au_headers_init(&rq->headers);
    rq->dest = AU_DEST_DOCUMENT;
    rq->mode = AU_MODE_NAVIGATE;
    rq->range_start = rq->range_end = -1;
}

void au_http_request_free(AuHttpRequest *rq)
{
    au_headers_free(&rq->headers);
    au_body_free(rq->body);
    rq->body = NULL;
}

static char *dupz(const char *s) { return s ? au_strndup(s, strlen(s)) : NULL; }

static bool method_idempotent(const char *m)
{
    return !strcmp(m, "GET") || !strcmp(m, "HEAD") || !strcmp(m, "OPTIONS") || !strcmp(m, "PUT") ||
           !strcmp(m, "DELETE") || !strcmp(m, "TRACE");
}

static bool method_safe(const char *m)
{
    return !strcmp(m, "GET") || !strcmp(m, "HEAD") || !strcmp(m, "OPTIONS") || !strcmp(m, "TRACE");
}

static void fail_later(AuHttpTransfer *t, AuNetError e)
{
    t->state = T_FAILED;
    t->fail_err = e;
}

AuHttpTransfer *au_http_start(AuNet *net, AuHttpRequest *rq, const AuHttpCallbacks *cb)
{
    AuHttpTransfer *t = au_calloc(sizeof *t);
    t->net = net;
    t->cb = *cb;
    snprintf(t->method, sizeof t->method, "%s", rq->method && rq->method[0] ? rq->method : "GET");
    for (char *p = t->method; *p; p++)
        if (!au_http_is_tchar((unsigned char)*p))
            *p = 'X';
    au_headers_copy(&t->extra, &rq->headers);
    t->body = rq->body;
    rq->body = NULL;
    t->dest = rq->dest;
    t->mode = rq->mode;
    t->referrer = dupz(rq->referrer);
    t->rpolicy = rq->referrer_policy;
    t->top_level = dupz(rq->top_level_url);
    t->user_activated = rq->user_activated;
    t->reload = rq->reload;
    t->history = rq->history && !rq->reload;
    t->omit_credentials = rq->omit_credentials;
    t->no_cache = rq->no_cache;
    t->manual_redirect = rq->manual_redirect;
    t->allow_http_fallback = rq->allow_http_fallback;
    t->range_start = rq->range_start;
    t->range_end = rq->range_end;
    t->total_timeout = rq->total_timeout_ms;
    t->started_ms = t->last_ms = now_ms(net);
    trace_init(t);
    t->original_url = dupz(rq->url ? rq->url : "");
    au_headers_init(&t->resp_headers);
    au_buf_init(&t->mime);
    au_buf_init(&t->charset);
    au_buf_init(&t->encoding);
    au_buf_init(&t->aia_body);
    t->state = T_START;
    if (!rq->url || !au_url_parse(&t->url, rq->url, NULL)) {
        memset(&t->url, 0, sizeof t->url);
        fail_later(t, AU_NET_ERR_BAD_URL);
    } else if (strcmp(t->url.scheme, "http") && strcmp(t->url.scheme, "https")) {
        fail_later(t, AU_NET_ERR_SCHEME);
    } else if (t->url.host_type == AU_URL_HOST_NULL || !t->url.host[0]) {
        fail_later(t, AU_NET_ERR_BAD_URL);
    }
    /* Append (callbacks happen in list order). */
    AuHttpTransfer **pp = &net->transfers;
    while (*pp)
        pp = &(*pp)->next;
    *pp = t;
    return t;
}

void au_http_cancel(AuHttpTransfer *t)
{
    /* No more callbacks; the next pump releases it. */
    t->cancelled = true;
}

void au_http_set_user(AuHttpTransfer *t, void *user) { t->user = user; }
void *au_http_user(const AuHttpTransfer *t) { return t->user; }
const char *au_http_url(const AuHttpTransfer *t) { return t->url.href ? t->url.href : t->original_url; }
int64_t au_http_received(const AuHttpTransfer *t) { return t->received; }
const AuCertInfo *au_http_cert_info(const AuHttpTransfer *t) { return t->have_cert ? &t->cert : NULL; }

int au_net_active(const AuNet *net)
{
    int n = 0;
    for (AuHttpTransfer *t = net->transfers; t; t = t->next)
        if (!t->cancelled)
            n++;
    return n;
}

/* ---- Connections ---- */

static void conn_free(Conn *c)
{
    AuNet *net = c->net;
    if (c->resolve_req)
        au_dns_cancel(net->dns, c->resolve_req);
    if (c->sock)
        net->ops.close(net->ops.ctx, c->sock);
    au_tls_free(c->tls_engine);
    au_buf_free(&c->in);
    au_buf_free(&c->out);
    if (c->proxy_parser_live)
        au_http_parser_free(&c->proxy_parser);
    au_free(c->host);
    au_free(c->proxy);
    au_free(c);
}

static void conn_remove(AuNet *net, Conn *c)
{
    for (Conn **pp = &net->conns; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    conn_free(c);
}

static void conn_kill(Conn *c, AuNetError e)
{
    if (c->state != C_DEAD) {
        c->state = C_DEAD;
        c->err = e;
    }
}

static size_t conn_pending_in(const Conn *c) { return c->in.len - c->in_off; }

static void conn_compact(AuBuf *b, size_t *off)
{
    if (*off && *off == b->len) {
        b->len = 0;
        *off = 0;
    } else if (*off > 32768) {
        memmove(b->data, b->data + *off, b->len - *off);
        b->len -= *off;
        *off = 0;
    }
}

static void conn_queue(Conn *c, const void *data, size_t n)
{
    au_buf_append(&c->out, data, n);
}

static bool via_proxy(AuNet *net, bool tls, const char **ph, int *pp)
{
    const char *h = tls ? net->cfg.https_proxy_host : net->cfg.http_proxy_host;
    int p = tls ? net->cfg.https_proxy_port : net->cfg.http_proxy_port;
    if (!h || !h[0] || p <= 0 || p > 65535)
        return false;
    *ph = h;
    *pp = p;
    return true;
}

static void session_key(const char *host, uint16_t port, char *out, size_t cap)
{
    snprintf(out, cap, "%s:%u", host, (unsigned)port);
}

static SessionEntry *session_entry(AuNet *net, const char *host, uint16_t port)
{
    char key[300];
    session_key(host, port, key, sizeof key);
    for (int i = 0; i < SESSION_CACHE; i++)
        if (net->sessions[i].hostport && !strcmp(net->sessions[i].hostport, key) && net->sessions[i].s.valid)
            return &net->sessions[i];
    return NULL;
}

static const AuTlsSession *session_find(AuNet *net, const char *host, uint16_t port)
{
    SessionEntry *e = session_entry(net, host, port);
    return e ? &e->s : NULL;
}

static void session_store(AuNet *net, const char *host, uint16_t port, const AuTlsSession *s, const AuCertInfo *ci)
{
    char key[300];
    session_key(host, port, key, sizeof key);
    int slot = -1;
    for (int i = 0; i < SESSION_CACHE; i++)
        if (net->sessions[i].hostport && !strcmp(net->sessions[i].hostport, key))
            slot = i;
    if (slot < 0) {
        slot = net->next_session;
        net->next_session = (net->next_session + 1) % SESSION_CACHE;
        au_free(net->sessions[slot].hostport);
        net->sessions[slot].hostport = au_strndup(key, strlen(key));
    }
    net->sessions[slot].s = *s;
    if (!net->sessions[slot].cert)
        net->sessions[slot].cert = au_alloc(sizeof *net->sessions[slot].cert);
    *net->sessions[slot].cert = *ci;
}

/* Keeps the connection's session for resumption when the engine has a
 * new one: after a full TLS 1.2 handshake, and whenever a TLS 1.3 server
 * sends a ticket (after the handshake). A resumed connection has no
 * chain of its own: its new ticket replaces the old one and the entry
 * keeps the chain of the full handshake. A connection the user's
 * exception let through is not cached, so every later connection is
 * checked again. */
static void session_keep(Conn *c)
{
    AuNet *net = c->net;
    AuTlsSession s;
    if (!au_tls_save_session(c->tls_engine, &s))
        return;
    const AuCertInfo *ci = au_tls_cert_info(c->tls_engine);
    if (ci->overridden)
        return;
    if (ci->count > 0) {
        session_store(net, c->host, c->port, &s, ci);
    } else {
        SessionEntry *e = session_entry(net, c->host, c->port);
        if (e && e->cert)
            e->s = s;
    }
    memset(&s, 0, sizeof s);
}

/* ---- Certificate exceptions ---- */

static const CertException *exception_for(AuNet *net, const char *host)
{
    for (int i = 0; i < CERT_EXCEPTIONS; i++)
        if (net->exceptions[i].host && !strcmp(net->exceptions[i].host, host))
            return &net->exceptions[i];
    return NULL;
}

bool au_net_allow_cert(AuNet *net, const char *host, const uint8_t sha256[32])
{
    if (!host || !*host || au_hsts_is_known(net->hsts, host, wall(net)))
        return false;
    int slot = -1;
    for (int i = 0; i < CERT_EXCEPTIONS; i++)
        if (net->exceptions[i].host && !strcmp(net->exceptions[i].host, host))
            slot = i;
    if (slot < 0) {
        slot = net->next_exception;
        net->next_exception = (net->next_exception + 1) % CERT_EXCEPTIONS;
        au_free(net->exceptions[slot].host);
        net->exceptions[slot].host = au_strndup(host, strlen(host));
    }
    memcpy(net->exceptions[slot].sha256, sha256, 32);
    /* Sessions resumed later must not skip the check the exception
     * replaces; drop a cached session for the host. */
    for (int i = 0; i < SESSION_CACHE; i++) {
        SessionEntry *e = &net->sessions[i];
        if (e->hostport && !strncmp(e->hostport, host, strlen(host)) && e->hostport[strlen(host)] == ':')
            e->s.valid = false;
    }
    return true;
}

void au_net_clear_cert_exceptions(AuNet *net)
{
    for (int i = 0; i < CERT_EXCEPTIONS; i++) {
        au_free(net->exceptions[i].host);
        net->exceptions[i].host = NULL;
    }
}

/* Gives the TLS library's random generator seed material from the
 * platform, once and again every 1000 uses; false while entropy is short
 * (the first TLS handshake waits for it). */
static bool drbg_ready(AuNet *net)
{
    if (net->drbg_ready && net->drbg_uses < 1000)
        return true;
    uint8_t seed[48];
    if (!net->ops.entropy || !net->ops.entropy(net->ops.ctx, seed, sizeof seed))
        return net->drbg_ready; /* keep using the old state if reseeding must wait */
    bool ok = au_tls_entropy_feed(seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    if (!ok)
        return net->drbg_ready;
    net->drbg_ready = true;
    net->drbg_uses = 0;
    return true;
}

static Conn *conn_new(AuNet *net, const char *host, uint16_t port, bool tls)
{
    Conn *c = au_calloc(sizeof *c);
    c->net = net;
    c->host = au_strndup(host, strlen(host));
    c->port = port;
    c->tls = tls;
    const char *ph;
    int pp;
    if (via_proxy(net, tls, &ph, &pp)) {
        c->proxy = au_strndup(ph, strlen(ph));
        c->proxy_port = (uint16_t)pp;
    }
    au_buf_init(&c->in);
    au_buf_init(&c->out);
    c->started_ms = c->last_ms = now_ms(net);
    c->id = ++net->next_conn_id;
    c->state = C_RESOLVE;
    /* IPv6 literal hosts come bracketed from the URL parser; resolvers
     * want them bare. */
    const char *rh = c->proxy ? c->proxy : host;
    char bare[300];
    size_t rl = strlen(rh);
    if (rh[0] == '[' && rl >= 2 && rl < sizeof bare) {
        memcpy(bare, rh + 1, rl - 2);
        bare[rl - 2] = 0;
        rh = bare;
    }
    c->resolve_req = au_dns_start(net->dns, rh);
    if (!c->resolve_req)
        conn_kill(c, AU_NET_ERR_DNS);
    c->next = net->conns;
    net->conns = c;
    return c;
}

static int count_conns(AuNet *net, const char *host, uint16_t port, bool tls, int *total)
{
    int n = 0;
    *total = 0;
    for (Conn *c = net->conns; c; c = c->next) {
        (*total)++;
        if (c->port == port && c->tls == tls && !strcmp(c->host, host))
            n++;
    }
    return n;
}

static void start_tls(Conn *c)
{
    AuNet *net = c->net;
    uint8_t seed[32];
    int64_t now = wall(net);
    au_tls_tick(now_ms(net));
    if (!au_tls_random(seed, sizeof seed)) {
        conn_kill(c, AU_NET_ERR_TLS);
        return;
    }
    net->drbg_uses++;
    c->tls_engine = au_tls_new(net->roots, c->host, now, seed, session_find(net, c->host, c->port));
    memset(seed, 0, sizeof seed);
    if (!c->tls_engine) {
        conn_kill(c, AU_NET_ERR_CERT);
        return;
    }
    const CertException *ex = exception_for(net, c->host);
    if (ex && !au_hsts_is_known(net->hsts, c->host, now))
        au_tls_allow_cert(c->tls_engine, ex->sha256);
    au_tls_set_pool(c->tls_engine, &net->pool);
    c->state = C_TLS;
}

static void send_connect(Conn *c)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_printf(&b, "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n", c->host, (unsigned)c->port, c->host,
                  (unsigned)c->port);
    /* Proxy credentials from the cache. */
    AuNet *net = c->net;
    char key[300];
    session_key(c->proxy, c->proxy_port, key, sizeof key);
    for (int i = 0; i < AUTH_CACHE; i++) {
        AuthEntry *e = &net->auth[i];
        if (e->hostport && e->proxy && !strcmp(e->hostport, key) && !strcmp(e->params.scheme, "Basic")) {
            au_buf_puts(&b, "Proxy-Authorization: ");
            au_auth_basic(e->user, e->pass, &b);
            au_buf_puts(&b, "\r\n");
            break;
        }
    }
    au_buf_printf(&b, "User-Agent: %s\r\n\r\n", net->cfg.user_agent);
    conn_queue(c, b.data, b.len);
    au_buf_free(&b);
    au_http_parser_init(&c->proxy_parser, true);
    c->proxy_parser_live = true;
    c->state = C_PROXY;
}

/* Moves bytes between the socket, the TLS engine and the plaintext
 * buffers. Returns true if anything moved. */
static bool conn_io(Conn *c)
{
    AuNet *net = c->net;
    bool any = false;
    uint8_t buf[RECV_CHUNK];
    if (!c->sock || c->state == C_DEAD)
        return false;
    bool tls_active = c->tls_engine && (c->state == C_TLS || c->state == C_READY);
    for (int round = 0; round < 8; round++) {
        bool progress = false;
        if (tls_active) {
            /* plaintext out -> engine */
            if (c->out.len > c->out_off) {
                size_t cap;
                uint8_t *w = au_tls_write(c->tls_engine, &cap);
                if (w && cap) {
                    size_t n = c->out.len - c->out_off;
                    if (n > cap)
                        n = cap;
                    memcpy(w, c->out.data + c->out_off, n);
                    au_tls_write_done(c->tls_engine, n);
                    c->out_off += n;
                    conn_compact(&c->out, &c->out_off);
                    if (c->out.len == c->out_off)
                        au_tls_flush(c->tls_engine);
                    progress = true;
                }
            }
            /* ciphertext out -> socket */
            size_t len;
            const uint8_t *o = au_tls_out(c->tls_engine, &len);
            if (o && len) {
                long s = net->ops.send(net->ops.ctx, c->sock, o, len);
                if (s < 0) {
                    conn_kill(c, AU_NET_ERR_RESET);
                    return true;
                }
                if (s > 0) {
                    au_tls_out_done(c->tls_engine, (size_t)s);
                    progress = true;
                }
            }
            /* socket -> engine */
            size_t cap;
            uint8_t *in = au_tls_in(c->tls_engine, &cap);
            if (in && cap && !c->eof) {
                long r = net->ops.recv(net->ops.ctx, c->sock, in, cap);
                if (r > 0) {
                    au_tls_in_done(c->tls_engine, (size_t)r);
                    progress = true;
                } else if (r == AU_NETIO_CLOSED) {
                    c->eof = true;
                    au_tls_close(c->tls_engine);
                    progress = true;
                } else if (r < 0) {
                    conn_kill(c, AU_NET_ERR_RESET);
                    return true;
                }
            }
            /* engine -> plaintext in */
            const uint8_t *app = au_tls_read(c->tls_engine, &len);
            if (app && len && conn_pending_in(c) < IN_MAX) {
                size_t n = len;
                if (n > IN_MAX - conn_pending_in(c))
                    n = IN_MAX - conn_pending_in(c);
                au_buf_append(&c->in, app, n);
                au_tls_read_done(c->tls_engine, n);
                progress = true;
            }
            AuTlsState ts = au_tls_state(c->tls_engine);
            if (c->state == C_READY && ts == AU_TLS_OPEN)
                session_keep(c);
            if (ts == AU_TLS_FAILED) {
                conn_kill(c, au_tls_net_error(c->tls_engine));
                return true;
            }
            if (ts == AU_TLS_CLOSED && !c->eof)
                c->eof = true;
        } else {
            if (c->out.len > c->out_off) {
                long s = net->ops.send(net->ops.ctx, c->sock, c->out.data + c->out_off, c->out.len - c->out_off);
                if (s < 0) {
                    conn_kill(c, AU_NET_ERR_RESET);
                    return true;
                }
                if (s > 0) {
                    c->out_off += (size_t)s;
                    conn_compact(&c->out, &c->out_off);
                    progress = true;
                }
            }
            if (!c->eof && conn_pending_in(c) < IN_MAX) {
                size_t cap = IN_MAX - conn_pending_in(c);
                if (cap > sizeof buf)
                    cap = sizeof buf;
                long r = net->ops.recv(net->ops.ctx, c->sock, buf, cap);
                if (r > 0) {
                    au_buf_append(&c->in, buf, (size_t)r);
                    progress = true;
                } else if (r == AU_NETIO_CLOSED) {
                    c->eof = true;
                    progress = true;
                } else if (r < 0) {
                    conn_kill(c, AU_NET_ERR_RESET);
                    return true;
                }
            }
        }
        if (!progress)
            break;
        any = true;
        c->last_ms = now_ms(net);
    }
    return any;
}

/* The connection to the cached address failed: the next lookup of the
 * host asks the resolver again (the host may have moved). */
static void forget_address(Conn *c)
{
    const char *rh = c->proxy ? c->proxy : c->host;
    char bare[300];
    size_t rl = strlen(rh);
    if (rh[0] == '[' && rl >= 2 && rl < sizeof bare) {
        memcpy(bare, rh + 1, rl - 2);
        bare[rl - 2] = 0;
        rh = bare;
    }
    au_dns_forget(c->net->dns, rh);
}

/* Advances a connection through resolve, connect, proxy CONNECT and the
 * TLS handshake. Returns true if anything happened. */
static bool conn_step(Conn *c)
{
    AuNet *net = c->net;
    uint32_t now = now_ms(net);
    switch (c->state) {
    case C_RESOLVE: {
        AuNetAddr a;
        int r = au_dns_poll(net->dns, c->resolve_req, &a);
        if (r == 0) {
            if (elapsed(now, c->started_ms) > net->cfg.connect_timeout_ms)
                conn_kill(c, AU_NET_ERR_TIMEOUT);
            return false;
        }
        au_dns_cancel(net->dns, c->resolve_req);
        c->resolve_req = 0;
        if (r < 0) {
            conn_kill(c, c->proxy ? AU_NET_ERR_PROXY : AU_NET_ERR_DNS);
            return true;
        }
        c->addr = a;
        c->sock = net->ops.connect(net->ops.ctx, &a, c->proxy ? c->proxy_port : c->port);
        if (!c->sock) {
            forget_address(c);
            conn_kill(c, c->proxy ? AU_NET_ERR_PROXY : AU_NET_ERR_CONNECT);
            return true;
        }
        c->state = C_CONNECT;
        return true;
    }
    case C_CONNECT: {
        int r = net->ops.connect_poll(net->ops.ctx, c->sock);
        if (r == 0) {
            if (elapsed(now, c->started_ms) > net->cfg.connect_timeout_ms)
                conn_kill(c, AU_NET_ERR_TIMEOUT);
            return false;
        }
        if (r < 0) {
            forget_address(c);
            conn_kill(c, c->proxy ? AU_NET_ERR_PROXY : AU_NET_ERR_CONNECT);
            return true;
        }
        c->last_ms = now;
        if (c->proxy && c->tls)
            send_connect(c);
        else if (c->tls)
            start_tls(c);
        else
            c->state = C_READY;
        return true;
    }
    case C_PROXY: {
        bool moved = conn_io(c);
        if (c->state == C_DEAD)
            return true;
        while (conn_pending_in(c)) {
            size_t used;
            AuHttpEvent ev;
            AuHttpParseResult r = au_http_parser_feed(&c->proxy_parser, (const uint8_t *)c->in.data + c->in_off,
                                                      conn_pending_in(c), &used, &ev);
            c->in_off += used;
            if (r == AU_HP_ERROR) {
                conn_kill(c, AU_NET_ERR_PROXY);
                return true;
            }
            if (r == AU_HP_HEADERS) {
                int st = c->proxy_parser.status;
                if (st == 407 && c->owner) {
                    /* Hand the challenge to the transfer. */
                    const char *pa = au_headers_get(&c->proxy_parser.headers, "proxy-authenticate");
                    AuAuthParams p;
                    if (pa && au_auth_parse(pa, &p) && !strcmp(p.scheme, "Basic")) {
                        c->owner->pending_auth = p;
                        c->owner->pending_proxy = true;
                    }
                    conn_kill(c, AU_NET_ERR_PROXY);
                    return true;
                }
                if (st < 200 || st > 299) {
                    conn_kill(c, AU_NET_ERR_PROXY);
                    return true;
                }
                /* Tunnel open: the rest is TLS. */
                c->in.len = 0;
                c->in_off = 0;
                au_http_parser_free(&c->proxy_parser);
                c->proxy_parser_live = false;
                start_tls(c);
                return true;
            }
            if (r == AU_HP_NEED_MORE)
                break;
        }
        conn_compact(&c->in, &c->in_off);
        if (c->eof) {
            conn_kill(c, AU_NET_ERR_PROXY);
            return true;
        }
        if (elapsed(now, c->last_ms) > net->cfg.connect_timeout_ms)
            conn_kill(c, AU_NET_ERR_TIMEOUT);
        return moved;
    }
    case C_TLS: {
        bool moved = conn_io(c);
        if (c->state == C_DEAD)
            return true;
        if (au_tls_state(c->tls_engine) == AU_TLS_OPEN) {
            c->state = C_READY;
            session_keep(c);
            return true;
        }
        if (c->eof) {
            conn_kill(c, AU_NET_ERR_TLS);
            return true;
        }
        if (elapsed(now, c->last_ms) > net->cfg.connect_timeout_ms)
            conn_kill(c, AU_NET_ERR_TIMEOUT);
        return moved;
    }
    case C_READY:
        return conn_io(c);
    default:
        return false;
    }
}

/* ---- Request building ---- */

static void request_target(AuHttpTransfer *t, bool absolute, AuBuf *out)
{
    if (absolute) {
        au_url_serialize_no_fragment(&t->url, out);
        return;
    }
    au_buf_clear(out);
    au_buf_puts(out, t->url.path[0] ? t->url.path : "/");
    if (t->url.query) {
        au_buf_putc(out, '?');
        au_buf_puts(out, t->url.query);
    }
    au_buf_cstr(out);
}

static void hostport(const AuUrl *u, AuBuf *out)
{
    au_buf_clear(out);
    au_buf_puts(out, u->host);
    if (u->port >= 0)
        au_buf_printf(out, ":%d", u->port);
    au_buf_cstr(out);
}

static AuthEntry *auth_find(AuNet *net, const char *hp, bool proxy, const char *realm)
{
    for (int i = 0; i < AUTH_CACHE; i++) {
        AuthEntry *e = &net->auth[i];
        if (e->hostport && e->proxy == proxy && !strcmp(e->hostport, hp) && (!realm || !strcmp(e->realm, realm)))
            return e;
    }
    return NULL;
}

static void auth_store(AuNet *net, const char *hp, bool proxy, const AuAuthParams *p, const char *user,
                       const char *pass)
{
    AuthEntry *e = auth_find(net, hp, proxy, p->realm);
    if (!e) {
        e = &net->auth[net->next_auth];
        net->next_auth = (net->next_auth + 1) % AUTH_CACHE;
        au_free(e->hostport);
        au_free(e->realm);
        au_free(e->user);
        au_free(e->pass);
        e->hostport = dupz(hp);
        e->realm = dupz(p->realm);
        e->user = e->pass = NULL;
    }
    au_free(e->user);
    au_free(e->pass);
    e->user = dupz(user);
    e->pass = dupz(pass);
    e->params = *p;
    e->nc = 0;
    e->proxy = proxy;
}

static void add_auth_header(AuHttpTransfer *t, AuBuf *b, AuthEntry *e, const char *name, const char *target)
{
    au_buf_printf(b, "%s: ", name);
    if (!strcmp(e->params.scheme, "Digest")) {
        uint8_t r[8];
        char cnonce[17];
        if (drbg_ready(t->net) && au_tls_random(r, sizeof r)) {
            t->net->drbg_uses++;
        } else {
            /* No entropy yet: the client nonce needs uniqueness, not
             * secrecy. */
            uint32_t a = now_ms(t->net), b = e->nc * 2654435761u;
            for (int i = 0; i < 4; i++) {
                r[i] = (uint8_t)(a >> (8 * i));
                r[4 + i] = (uint8_t)(b >> (8 * i));
            }
        }
        for (int i = 0; i < 8; i++)
            sprintf(cnonce + 2 * i, "%02x", r[i]);
        e->nc++;
        au_auth_digest(&e->params, e->user, e->pass, t->method, target, e->nc, cnonce, b);
    } else {
        au_auth_basic(e->user, e->pass, b);
    }
    au_buf_puts(b, "\r\n");
}

static void cookie_request(AuHttpTransfer *t, AuCookieRequest *cr, AuUrl *site, AuUrl *init, bool *have_site,
                           bool *have_init)
{
    memset(cr, 0, sizeof *cr);
    cr->url = &t->url;
    *have_site = t->top_level && au_url_parse(site, t->top_level, NULL);
    cr->site_for_cookies = *have_site ? site : NULL;
    *have_init = t->referrer && au_url_parse(init, t->referrer, NULL);
    cr->initiator = *have_init ? init : NULL;
    cr->top_level_nav = (t->dest == AU_DEST_DOCUMENT) && !t->top_level;
    cr->safe_method = method_safe(t->method);
    cr->now = wall(t->net);
}

static void build_request(AuHttpTransfer *t, Conn *c)
{
    AuNet *net = t->net;
    AuBuf b, tmp, target;
    au_buf_init(&b);
    au_buf_init(&tmp);
    au_buf_init(&target);
    bool absolute = c->proxy && !c->tls;
    request_target(t, absolute, &target);
    au_buf_printf(&b, "%s %s HTTP/1.1\r\n", t->method, au_buf_cstr(&target));
    hostport(&t->url, &tmp);
    au_buf_printf(&b, "Host: %s\r\n", au_buf_cstr(&tmp));
    au_buf_printf(&b, "User-Agent: %s\r\n", net->cfg.user_agent);
    if (!au_headers_get(&t->extra, "accept"))
        au_buf_printf(&b, "Accept: %s\r\n", au_accept_for(t->dest));
    if (net->cfg.accept_language[0] && !au_headers_get(&t->extra, "accept-language"))
        au_buf_printf(&b, "Accept-Language: %s\r\n", net->cfg.accept_language);
    if (t->range_start < 0)
        au_buf_puts(&b, "Accept-Encoding: gzip, deflate, br\r\n");
    else {
        /* au_put_i64, not %lld: newlib on Mac OS 9 has no %lld. */
        au_buf_puts(&b, "Range: bytes=");
        au_put_i64(&b, t->range_start);
        au_buf_putc(&b, '-');
        if (t->range_end >= 0)
            au_put_i64(&b, t->range_end);
        au_buf_puts(&b, "\r\n");
    }
    if (net->cfg.send_dnt)
        au_buf_puts(&b, "DNT: 1\r\n");
    if (net->cfg.send_gpc)
        au_buf_puts(&b, "Sec-GPC: 1\r\n");
    /* Referer */
    if (au_referrer_compute(t->referrer, &t->url, t->rpolicy, &tmp))
        au_buf_printf(&b, "Referer: %s\r\n", au_buf_cstr(&tmp));
    /* Origin: for requests other than GET/HEAD, and for CORS. */
    if (t->referrer && (t->mode == AU_MODE_CORS || (strcmp(t->method, "GET") && strcmp(t->method, "HEAD")))) {
        AuUrl r;
        if (au_url_parse(&r, t->referrer, NULL)) {
            AuOrigin o;
            au_url_origin(&r, &o);
            au_buf_clear(&tmp);
            /* A cross-origin request from a secure page to an insecure one
             * sends "null" under the default policy's spirit. */
            au_origin_serialize(&o, &tmp);
            au_buf_printf(&b, "Origin: %s\r\n", au_buf_cstr(&tmp));
            au_origin_free(&o);
            au_url_free(&r);
        }
    }
    /* Fetch metadata: only to potentially trustworthy URLs. */
    if (au_url_trustworthy(&t->url)) {
        au_buf_printf(&b, "Sec-Fetch-Site: %s\r\n", au_sec_fetch_site(t->referrer, &t->url));
        au_buf_printf(&b, "Sec-Fetch-Mode: %s\r\n", au_sec_fetch_mode(t->mode));
        au_buf_printf(&b, "Sec-Fetch-Dest: %s\r\n", au_sec_fetch_dest(t->dest));
        if (t->mode == AU_MODE_NAVIGATE && t->user_activated)
            au_buf_puts(&b, "Sec-Fetch-User: ?1\r\n");
    }
    if (t->mode == AU_MODE_NAVIGATE)
        au_buf_puts(&b, "Upgrade-Insecure-Requests: 1\r\n");
    if (t->reload)
        au_buf_puts(&b, "Cache-Control: no-cache\r\nPragma: no-cache\r\n");
    /* Revalidating a stale cached response. */
    if (t->cache_entry) {
        const char *etag = au_cache_entry_etag(t->cache_entry);
        const char *lm = au_cache_entry_last_modified(t->cache_entry);
        if (etag && !au_headers_get(&t->extra, "if-none-match"))
            au_buf_printf(&b, "If-None-Match: %s\r\n", etag);
        if (lm && !au_headers_get(&t->extra, "if-modified-since"))
            au_buf_printf(&b, "If-Modified-Since: %s\r\n", lm);
    }
    t->request_wall = wall(net);
    /* Cookies */
    if (net->jar && !t->omit_credentials) {
        AuCookieRequest cr;
        AuUrl site, init;
        bool hs, hi;
        cookie_request(t, &cr, &site, &init, &hs, &hi);
        if (au_cookie_jar_header(net->jar, &cr, &tmp))
            au_buf_printf(&b, "Cookie: %s\r\n", au_buf_cstr(&tmp));
        if (hs)
            au_url_free(&site);
        if (hi)
            au_url_free(&init);
    }
    /* Authorization from the cache (same host and port). */
    if (!t->omit_credentials && !au_headers_get(&t->extra, "authorization")) {
        hostport(&t->url, &tmp);
        AuthEntry *e = auth_find(net, au_buf_cstr(&tmp), false, NULL);
        if (e)
            add_auth_header(t, &b, e, "Authorization", au_buf_cstr(&target));
    }
    if (absolute) {
        char key[300];
        session_key(c->proxy, c->proxy_port, key, sizeof key);
        AuthEntry *e = auth_find(net, key, true, NULL);
        if (e)
            add_auth_header(t, &b, e, "Proxy-Authorization", au_buf_cstr(&target));
    }
    /* Extra headers from the caller (never hop-by-hop or framing ones). */
    for (size_t i = 0; i < t->extra.n; i++) {
        const char *n = t->extra.items[i].name, *v = t->extra.items[i].value;
        static const char *const banned[] = {"host", "content-length", "transfer-encoding", "connection",
                                             "keep-alive", "upgrade", "te", "trailer", "expect", "cookie",
                                             "proxy-authorization"};
        bool skip = false;
        for (size_t k = 0; k < sizeof banned / sizeof banned[0]; k++)
            if (au_ieq(n, strlen(n), banned[k], strlen(banned[k])))
                skip = true;
        for (const char *p = n; *p && !skip; p++)
            if (!au_http_is_tchar((unsigned char)*p))
                skip = true;
        for (const char *p = v; *p && !skip; p++)
            if (*p == '\r' || *p == '\n')
                skip = true;
        if (!skip)
            au_buf_printf(&b, "%s: %s\r\n", n, v);
    }
    if (t->body) {
        if (t->body->content_type && !au_headers_get(&t->extra, "content-type"))
            au_buf_printf(&b, "Content-Type: %s\r\n", t->body->content_type);
        if (t->body->length >= 0) {
            au_buf_puts(&b, "Content-Length: ");
            au_put_i64(&b, t->body->length);
            au_buf_puts(&b, "\r\n");
        } else
            au_buf_puts(&b, "Transfer-Encoding: chunked\r\n");
    } else if (!strcmp(t->method, "POST") || !strcmp(t->method, "PUT")) {
        au_buf_puts(&b, "Content-Length: 0\r\n");
    }
    au_buf_puts(&b, "\r\n");
    conn_queue(c, b.data, b.len);
    t->trace.connection = c->id;
    t->trace.remote = c->addr;
    t->trace.remote_port = c->proxy ? c->proxy_port : c->port;
    if (net->trace_headers) {
        au_free(t->request_head);
        t->request_head = au_strndup(b.data, b.len < 4096 ? b.len : 4096);
    }
    au_buf_free(&b);
    au_buf_free(&tmp);
    au_buf_free(&target);
    t->head_sent = true;
    t->body_done = t->body == NULL;
}

/* Streams the request body into the connection's output. */
static bool pump_body(AuHttpTransfer *t, Conn *c)
{
    if (t->body_done)
        return false;
    bool any = false;
    uint8_t buf[8192];
    while (c->out.len - c->out_off < OUT_LOW) {
        long n = t->body->read(t->body->ctx, buf, sizeof buf);
        if (n < 0) {
            fail_later(t, AU_NET_ERR_RESET);
            return true;
        }
        any = true;
        bool chunked = t->body->length < 0;
        if (n == 0) {
            if (chunked)
                conn_queue(c, "0\r\n\r\n", 5);
            t->body_done = true;
            break;
        }
        if (chunked) {
            char h[16];
            int hl = snprintf(h, sizeof h, "%lx\r\n", (unsigned long)n);
            conn_queue(c, h, (size_t)hl);
            conn_queue(c, buf, (size_t)n);
            conn_queue(c, "\r\n", 2);
        } else {
            conn_queue(c, buf, (size_t)n);
        }
    }
    return any;
}

/* ---- Finishing ---- */

static void release_decoders(AuHttpTransfer *t)
{
    for (int i = 0; i < t->ndec; i++)
        au_decoder_free(t->dec[i]);
    t->ndec = 0;
    if (t->uses_brotli) {
        t->uses_brotli = false;
        if (--t->net->brdict_users == 0) {
            au_brotli_dict_free(t->net->brdict);
            t->net->brdict = NULL;
        }
    }
}

/* Gives the connection back: to the pool when reusable, else closed. */
static void detach_conn(AuHttpTransfer *t, bool reusable)
{
    Conn *c = t->conn;
    if (!c)
        return;
    t->conn = NULL;
    c->owner = NULL;
    if (!reusable || c->state != C_READY || c->eof || c->in.len > c->in_off) {
        conn_remove(t->net, c);
        return;
    }
    c->served++;
    c->last_ms = now_ms(t->net);
    c->in.len = c->in_off = 0;
}

/* Releases the entry being revalidated (a background revalidation also
 * takes its mark off). */
static void drop_cache_entry(AuHttpTransfer *t)
{
    if (!t->cache_entry)
        return;
    if (t->swr_background)
        au_cache_entry_set_revalidating(t->cache_entry, false);
    au_cache_entry_release(t->net->cache, t->cache_entry);
    t->cache_entry = NULL;
}

static void cache_drop(AuHttpTransfer *t)
{
    if (t->cache_w) {
        au_cache_abort(t->cache_w);
        t->cache_w = NULL;
    }
    drop_cache_entry(t);
}

/* Ends the transfer's caIssuers fetch, if one runs: it is not wanted any
 * more, and its callbacks name this transfer. */
static void aia_drop_child(AuHttpTransfer *t)
{
    if (t->aia_child) {
        au_http_cancel(t->aia_child);
        t->aia_child = NULL;
    }
}

static void transfer_free(AuHttpTransfer *t)
{
    aia_drop_child(t);
    for (int i = 0; i < AIA_MAX_DEPTH; i++)
        au_free(t->aia_tried[i]);
    au_buf_free(&t->aia_body);
    cache_drop(t);
    release_decoders(t);
    if (t->parser_live)
        au_http_parser_free(&t->parser);
    au_url_free(&t->url);
    au_free(t->original_url);
    au_headers_free(&t->extra);
    au_body_free(t->body);
    au_free(t->referrer);
    au_free(t->top_level);
    au_headers_free(&t->resp_headers);
    au_buf_free(&t->mime);
    au_buf_free(&t->charset);
    au_buf_free(&t->encoding);
    au_free(t->request_head);
    au_free(t);
}

static void finish(AuHttpTransfer *t, AuNetError err)
{
    if (t->state == T_FINISHED)
        return;
    aia_drop_child(t);
    detach_conn(t, err == AU_NET_OK && t->parser_live && t->parser.keep_alive);
    t->state = T_FINISHED;
    if (t->cache_w && err == AU_NET_OK && !t->cancelled) {
        au_cache_commit(t->cache_w);
        t->cache_w = NULL;
    }
    cache_drop(t);
    trace_note(t);
    if (!t->cancelled && t->cb.done)
        t->cb.done(t->cb.ctx, t, err);
}

/* ---- Response handling ---- */

static void reset_attempt(AuHttpTransfer *t)
{
    if (t->cache_w) {
        au_cache_abort(t->cache_w);
        t->cache_w = NULL;
    }
    release_decoders(t);
    if (t->parser_live)
        au_http_parser_free(&t->parser);
    t->parser_live = false;
    t->got_bytes = false;
    t->head_sent = false;
    t->body_done = false;
    t->received = 0;
}

static bool deliver_decoded(AuHttpTransfer *t, int stage, const uint8_t *data, size_t len);

/* Body bytes as sent -> decoders -> callback. Returns false on a decode
 * error or when the body cap is exceeded (the transfer is failed). */
static bool deliver_body(AuHttpTransfer *t, const uint8_t *data, size_t len)
{
    t->received += (int64_t)len;
    if (t->ndec == 0)
        return deliver_decoded(t, -1, data, len);
    return deliver_decoded(t, t->ndec - 1, data, len);
}

static bool emit(AuHttpTransfer *t, const uint8_t *data, size_t len)
{
    if (!len)
        return true;
    t->delivered += len;
    if (t->delivered > t->net->cfg.max_body) {
        fail_later(t, AU_NET_ERR_TOO_LARGE);
        return false;
    }
    if (t->cache_w && !au_cache_write(t->cache_w, data, len))
        t->cache_w = NULL; /* too large: the writer aborted itself */
    if (!t->cancelled && t->cb.data)
        t->cb.data(t->cb.ctx, t, data, len);
    return true;
}

static bool deliver_decoded(AuHttpTransfer *t, int stage, const uint8_t *data, size_t len)
{
    if (stage < 0)
        return emit(t, data, len);
    uint8_t out[8192];
    AuDecoder *d = t->dec[stage];
    for (;;) {
        uint8_t *op = out;
        size_t ol = sizeof out;
        AuCodecStatus st = au_decoder_run(d, &data, &len, &op, &ol);
        size_t made = sizeof out - ol;
        if (made && !deliver_decoded(t, stage - 1, out, made))
            return false;
        if (st == AU_CODEC_ERROR) {
            fail_later(t, AU_NET_ERR_DECODE);
            return false;
        }
        if (st == AU_CODEC_DONE)
            return true; /* trailing bytes after the stream are ignored */
        if (!len && ol)
            return true; /* needs more input */
    }
}

static void setup_decoders(AuHttpTransfer *t)
{
    const char *ce = au_headers_get(&t->parser.headers, "content-encoding");
    au_buf_clear(&t->encoding);
    if (!ce || t->parser.framing == AU_BODY_NONE)
        return;
    AuBuf all;
    au_buf_init(&all);
    au_headers_get_combined(&t->parser.headers, "content-encoding", &all);
    au_buf_puts(&t->encoding, au_buf_cstr(&all));
    const char *s = au_buf_cstr(&all), *item;
    size_t il;
    while (au_http_list_next(&s, &item, &il)) {
        AuDecoder *d = NULL;
        if (au_ieq(item, il, "gzip", 4) || au_ieq(item, il, "x-gzip", 6)) {
            d = au_inflate_decoder(AU_INFLATE_GZIP);
        } else if (au_ieq(item, il, "deflate", 7)) {
            d = au_inflate_decoder(AU_INFLATE_HTTP_DEFLATE);
        } else if (au_ieq(item, il, "br", 2)) {
            AuNet *net = t->net;
            if (!net->brdict)
                net->brdict = au_brotli_dict_new();
            if (net->brdict) {
                if (!t->uses_brotli) {
                    t->uses_brotli = true;
                    net->brdict_users++;
                }
                d = au_brotli_decoder(net->brdict, 0);
            }
        } else if (au_ieq(item, il, "identity", 8)) {
            continue;
        }
        if (!d || t->ndec == MAX_DECODERS) {
            /* Unknown or too many codings: deliver as sent. */
            au_decoder_free(d);
            release_decoders(t);
            break;
        }
        t->dec[t->ndec++] = d;
    }
    au_buf_free(&all);
}

static void store_cookies(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    if (!net->jar || t->omit_credentials)
        return;
    AuCookieRequest cr;
    AuUrl site, init;
    bool hs, hi;
    cookie_request(t, &cr, &site, &init, &hs, &hi);
    size_t pos = 0;
    const char *v;
    while ((v = au_headers_next(&t->parser.headers, "set-cookie", &pos)) != NULL)
        au_cookie_jar_set(net->jar, &cr, v, NULL);
    if (hs)
        au_url_free(&site);
    if (hi)
        au_url_free(&init);
}

static void deliver_response(AuHttpTransfer *t)
{
    au_headers_free(&t->resp_headers);
    au_headers_copy(&t->resp_headers, &t->parser.headers);
    t->status = t->parser.status;
    snprintf(t->resp_reason, sizeof t->resp_reason, "%s", t->parser.reason);
    const char *ct = au_headers_get(&t->resp_headers, "content-type");
    if (!ct || !au_http_parse_mime(ct, &t->mime, &t->charset)) {
        au_buf_clear(&t->mime);
        au_buf_clear(&t->charset);
    }
    au_buf_cstr(&t->mime);
    au_buf_cstr(&t->charset);
    au_buf_cstr(&t->encoding);
    AuHttpResponse r;
    memset(&r, 0, sizeof r);
    r.status = t->status;
    r.reason = t->resp_reason;
    r.headers = &t->resp_headers;
    r.url = t->url.href;
    r.redirects = t->redirects;
    r.content_length = t->parser.content_length;
    r.mime = t->mime.data;
    r.charset = t->charset.data;
    r.content_encoding = t->encoding.data;
    r.security = t->security;
    if (!t->cancelled && t->cb.response)
        t->cb.response(t->cb.ctx, t, &r);
}

/* ---- Cache ---- */

/* Whether the request may use and fill the cache: GET without a body or
 * a range. */
static bool cache_applies(const AuHttpTransfer *t)
{
    return t->net->cache && !strcmp(t->method, "GET") && !t->body && t->range_start < 0 && !t->aia_internal &&
           !t->no_cache;
}

static void cache_key(const AuUrl *u, AuBuf *out)
{
    au_buf_clear(out);
    au_url_serialize_no_fragment(u, out);
    au_buf_cstr(out);
}

/* The request's fields as sent, for Vary matching. */
static void request_fields(AuHttpTransfer *t, AuHeaders *out)
{
    AuNet *net = t->net;
    au_headers_copy(out, &t->extra);
    if (!au_headers_get(out, "accept"))
        au_headers_add(out, "Accept", au_accept_for(t->dest));
    if (net->cfg.accept_language[0] && !au_headers_get(out, "accept-language"))
        au_headers_add(out, "Accept-Language", net->cfg.accept_language);
    au_headers_add(out, "Accept-Encoding", "gzip, deflate, br");
    au_headers_add(out, "User-Agent", net->cfg.user_agent);
    if (net->jar && !t->omit_credentials) {
        AuCookieRequest cr;
        AuUrl site, init;
        bool hs, hi;
        AuBuf tmp;
        au_buf_init(&tmp);
        cookie_request(t, &cr, &site, &init, &hs, &hi);
        if (au_cookie_jar_header(net->jar, &cr, &tmp))
            au_headers_add(out, "Cookie", au_buf_cstr(&tmp));
        if (hs)
            au_url_free(&site);
        if (hi)
            au_url_free(&init);
        au_buf_free(&tmp);
    }
}

/* Delivers a cached entry's response and body through the callbacks. */
static void deliver_cached(AuHttpTransfer *t, AuCacheEntry *e, bool revalidated)
{
    au_headers_free(&t->resp_headers);
    au_headers_copy(&t->resp_headers, au_cache_entry_headers(e));
    t->status = au_cache_entry_status(e);
    snprintf(t->resp_reason, sizeof t->resp_reason, "%s", au_cache_entry_reason(e));
    const char *ct = au_headers_get(&t->resp_headers, "content-type");
    if (!ct || !au_http_parse_mime(ct, &t->mime, &t->charset)) {
        au_buf_clear(&t->mime);
        au_buf_clear(&t->charset);
    }
    au_buf_clear(&t->encoding);
    size_t len;
    const uint8_t *body = au_cache_entry_body(e, &len);
    AuHttpResponse r;
    memset(&r, 0, sizeof r);
    r.status = t->status;
    r.reason = t->resp_reason;
    r.headers = &t->resp_headers;
    r.url = t->url.href;
    r.redirects = t->redirects;
    r.content_length = (int64_t)len;
    r.mime = au_buf_cstr(&t->mime);
    r.charset = au_buf_cstr(&t->charset);
    r.content_encoding = au_buf_cstr(&t->encoding);
    r.from_cache = true;
    r.revalidated = revalidated;
    if (revalidated) {
        r.security = t->security; /* from the connection that revalidated */
    } else {
        r.security.tls = !strcmp(t->url.scheme, "https");
        r.security.version = r.security.tls ? 0x0303 : 0;
        r.security.cipher = "";
        r.security.alpn = "";
    }
    if (!t->cancelled && t->cb.response)
        t->cb.response(t->cb.ctx, t, &r);
    /* The body in pieces, as the network would deliver it. */
    size_t off = 0;
    while (off < len && !t->cancelled) {
        size_t n = len - off > 16384 ? 16384 : len - off;
        t->delivered += n;
        if (t->cb.data)
            t->cb.data(t->cb.ctx, t, body + off, n);
        off += n;
    }
}

/* stale-while-revalidate: starts a transfer of its own that revalidates
 * entry e as t's request would have (same URL, fields and context) and
 * stores the result; nobody listens to it. The entry is marked until that
 * transfer lets go of it, so one revalidation runs at a time. */
static void revalidate_in_background(AuHttpTransfer *t, AuCacheEntry *e)
{
    AuHttpRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "GET";
    rq.url = t->url.href;
    au_headers_copy(&rq.headers, &t->extra);
    rq.dest = t->dest;
    rq.mode = t->mode;
    rq.referrer = t->referrer;
    rq.referrer_policy = t->rpolicy;
    rq.top_level_url = t->top_level;
    rq.omit_credentials = t->omit_credentials;
    rq.allow_http_fallback = t->allow_http_fallback;
    rq.range_start = rq.range_end = -1;
    rq.total_timeout_ms = 60000;
    AuHttpCallbacks cb;
    memset(&cb, 0, sizeof cb);
    AuHttpTransfer *b = au_http_start(t->net, &rq, &cb);
    au_headers_free(&rq.headers);
    b->swr_background = true;
    b->cache_entry = au_cache_entry_ref(e); /* T_START then skips the lookup and sends the conditional request */
    au_cache_entry_set_revalidating(e, true);
}

/* Looks the request up before connecting. Returns true when a fresh entry
 * was delivered (the transfer is finished). */
static bool cache_try(AuHttpTransfer *t)
{
    if (!cache_applies(t) || t->reload)
        return false;
    AuNet *net = t->net;
    AuBuf key;
    au_buf_init(&key);
    cache_key(&t->url, &key);
    AuHeaders req;
    request_fields(t, &req);
    AuCacheEntry *e = NULL;
    AuCacheStats before, after;
    au_cache_stats(net->cache, &before);
    AuCacheState st = t->history ? au_cache_lookup_history(net->cache, au_buf_cstr(&key), &req, &e)
                                 : au_cache_lookup(net->cache, au_buf_cstr(&key), &req, wall(net), &e);
    au_headers_free(&req);
    au_buf_free(&key);
    if (st == AU_CACHE_FRESH) {
        au_cache_stats(net->cache, &after);
        t->trace.source = t->history                                   ? AU_HTTP_FROM_HISTORY
                          : after.from_storage != before.from_storage ? AU_HTTP_FROM_DISK
                                                                      : AU_HTTP_FROM_MEMORY;
        deliver_cached(t, e, false);
        au_cache_entry_release(net->cache, e);
        finish(t, AU_NET_OK);
        return true;
    }
    if (st == AU_CACHE_STALE_USABLE) {
        /* stale-while-revalidate: a subresource is served stale now and
         * revalidated in the background; a document waits for the
         * revalidation, as the user asked for the page as it is now. */
        if (t->dest == AU_DEST_DOCUMENT || t->dest == AU_DEST_IFRAME) {
            t->cache_entry = e;
            return false;
        }
        if (!au_cache_entry_revalidating(e))
            revalidate_in_background(t, e);
        t->trace.source = AU_HTTP_FROM_MEMORY; /* the request log: a stale copy, served as it is */
        deliver_cached(t, e, false);
        au_cache_entry_release(net->cache, e);
        finish(t, AU_NET_OK);
        return true;
    }
    if (st == AU_CACHE_STALE)
        t->cache_entry = e; /* revalidate */
    return false;
}

/* After a successful unsafe request: drop the cached responses it may
 * have changed (RFC 9111 4.4). */
static void cache_invalidate_for(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    AuBuf key;
    au_buf_init(&key);
    cache_key(&t->url, &key);
    au_cache_invalidate(net->cache, au_buf_cstr(&key));
    static const char *fields[] = {"location", "content-location"};
    for (int i = 0; i < 2; i++) {
        const char *v = au_headers_get(&t->parser.headers, fields[i]);
        AuUrl u;
        if (v && au_url_parse(&u, v, &t->url)) {
            AuOrigin a, b;
            au_url_origin(&t->url, &a);
            au_url_origin(&u, &b);
            if (au_origin_same(&a, &b)) {
                cache_key(&u, &key);
                au_cache_invalidate(net->cache, au_buf_cstr(&key));
            }
            au_origin_free(&a);
            au_origin_free(&b);
            au_url_free(&u);
        }
    }
    au_buf_free(&key);
}

/* Starts storing the response just delivered, when allowed. */
static void cache_begin(AuHttpTransfer *t)
{
    if (!cache_applies(t))
        return;
    /* A coding left undecoded would be stored without its label. */
    if (t->encoding.len && t->ndec == 0)
        return;
    AuNet *net = t->net;
    AuBuf key;
    au_buf_init(&key);
    cache_key(&t->url, &key);
    AuHeaders req;
    request_fields(t, &req);
    t->cache_w = au_cache_begin(net->cache, au_buf_cstr(&key), &req, t->parser.status, t->parser.reason,
                                &t->parser.headers, t->request_wall, wall(net));
    au_headers_free(&req);
    au_buf_free(&key);
}

/* Starts the request again at the (new) URL on a fresh attempt. */
static void restart(AuHttpTransfer *t, bool keep_conn)
{
    detach_conn(t, keep_conn && t->parser_live && t->parser.keep_alive);
    reset_attempt(t);
    t->state = T_START;
}

static void record_tls(AuHttpTransfer *t, Conn *c)
{
    if (!c->tls_engine)
        return;
    au_tls_security(c->tls_engine, &t->security);
    if (t->security.cipher)
        snprintf(t->cipher, sizeof t->cipher, "%s", t->security.cipher);
    if (t->security.alpn)
        snprintf(t->alpn, sizeof t->alpn, "%s", t->security.alpn);
    t->security.cipher = t->cipher;
    t->security.alpn = t->alpn;
    const AuCertInfo *ci = au_tls_cert_info(c->tls_engine);
    if (ci && ci->count == 0 && t->security.resumed) {
        const SessionEntry *e = session_entry(t->net, c->host, c->port);
        if (e && e->cert)
            ci = e->cert;
    }
    if (ci) {
        t->cert = *ci;
        t->have_cert = true;
    }
}

/* Handles a final response head. Returns true when the transfer went on
 * to something else (redirect, auth retry) instead of delivering. */
static bool handle_head(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    int st = t->parser.status;
    store_cookies(t);
    /* HSTS: only from a secure connection without certificate errors. */
    if (net->hsts && !strcmp(t->url.scheme, "https") && t->url.host_type == AU_URL_HOST_DOMAIN &&
        !(t->have_cert && t->cert.overridden)) {
        const char *sts = au_headers_get(&t->parser.headers, "strict-transport-security");
        if (sts)
            au_hsts_process(net->hsts, t->url.host, sts, wall(net));
    }
    const char *rp = au_headers_get(&t->parser.headers, "referrer-policy");
    if (rp)
        t->rpolicy = au_referrer_policy_parse(rp, t->rpolicy);
    /* Cache: invalidation after unsafe methods; a 304 confirming the
     * stale entry being revalidated. */
    if (net->cache && !method_safe(t->method) && st < 400)
        cache_invalidate_for(t);
    if (st == 304 && t->cache_entry) {
        int64_t now = wall(net);
        au_cache_update(net->cache, t->cache_entry, &t->parser.headers, t->request_wall, now);
        t->trace.source = AU_HTTP_FROM_304;
        deliver_cached(t, t->cache_entry, true);
        drop_cache_entry(t);
        t->state = T_RECV; /* the 304 has no body; DONE finishes */
        return true;
    }
    if (t->cache_entry && st != 401 && st != 407) /* (an auth retry asks for the same URL again) */
        drop_cache_entry(t);
    /* Redirects */
    if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && !t->manual_redirect) {
        const char *loc = au_headers_get(&t->parser.headers, "location");
        if (loc) {
            AuUrl nu;
            if (!au_url_parse(&nu, loc, &t->url) ||
                (strcmp(nu.scheme, "http") && strcmp(nu.scheme, "https")) || !nu.host[0]) {
                if (nu.href)
                    au_url_free(&nu);
                fail_later(t, AU_NET_ERR_REDIRECTS);
                return true;
            }
            if (++t->redirects > net->cfg.max_redirects) {
                au_url_free(&nu);
                fail_later(t, AU_NET_ERR_REDIRECTS);
                return true;
            }
            /* A fragment-less Location inherits the current fragment. */
            if (!nu.fragment && t->url.fragment) {
                AuBuf s;
                au_buf_init(&s);
                au_buf_printf(&s, "%s#%s", nu.href, t->url.fragment);
                AuUrl nf;
                if (au_url_parse(&nf, au_buf_cstr(&s), NULL)) {
                    au_url_free(&nu);
                    nu = nf;
                }
                au_buf_free(&s);
            }
            if (t->cb.redirect && !t->cancelled && !t->cb.redirect(t->cb.ctx, t, st, nu.href)) {
                au_url_free(&nu);
                return false;
            }
            /* Method and body. */
            bool to_get = ((st == 301 || st == 302) && !strcmp(t->method, "POST")) ||
                          (st == 303 && strcmp(t->method, "HEAD"));
            if (to_get) {
                snprintf(t->method, sizeof t->method, "GET");
                au_body_free(t->body);
                t->body = NULL;
                au_headers_remove(&t->extra, "content-type");
                au_headers_remove(&t->extra, "content-encoding");
                au_headers_remove(&t->extra, "content-language");
                au_headers_remove(&t->extra, "content-location");
            } else if (t->body) {
                if (!t->body->rewind || !t->body->rewind(t->body->ctx)) {
                    au_url_free(&nu);
                    fail_later(t, AU_NET_ERR_REDIRECTS);
                    return true;
                }
            }
            /* Credentials do not cross origins. */
            AuOrigin a, b;
            au_url_origin(&t->url, &a);
            au_url_origin(&nu, &b);
            if (!au_origin_same(&a, &b))
                au_headers_remove(&t->extra, "authorization");
            au_origin_free(&a);
            au_origin_free(&b);
            /* A site that answers its HTTPS address with a redirect to
             * plain HTTP on the same host (www.macintoshrepository.org
             * does, for old browsers) has chosen HTTP itself, over a
             * connection whose certificate was checked. Upgrading that
             * address again would redirect for ever, so the host is
             * loaded over HTTP as if the user had confirmed it. Hosts
             * known to HSTS are still upgraded. */
            if (t->was_upgraded && !strcmp(t->url.scheme, "https") && !strcmp(nu.scheme, "http") &&
                !strcmp(t->url.host, nu.host))
                t->allow_http_fallback = true;
            /* The redirecting URL becomes the referrer context only for
             * policy; the referrer stays the original document. */
            au_url_free(&t->url);
            t->url = nu;
            t->https_first_upgrade = false;
            t->state = T_DRAIN;
            return true;
        }
    }
    /* Authentication */
    if ((st == 401 || st == 407) && t->cb.credentials && !t->omit_credentials && !t->cancelled) {
        bool proxy = st == 407;
        const char *wa = au_headers_get(&t->parser.headers, proxy ? "proxy-authenticate" : "www-authenticate");
        int *attempts = proxy ? &t->proxy_auth_attempts : &t->auth_attempts;
        AuAuthParams p;
        if (wa && au_auth_parse(wa, &p) && *attempts < MAX_AUTH_ATTEMPTS) {
            /* A stale Digest nonce: retry with the cached credentials. */
            AuBuf hp;
            au_buf_init(&hp);
            hostport(&t->url, &hp);
            AuthEntry *e = auth_find(net, au_buf_cstr(&hp), proxy, p.realm);
            if (e && p.stale && !strcmp(p.scheme, "Digest")) {
                e->params = p;
                e->nc = 0;
                au_buf_free(&hp);
                t->state = T_DRAIN;
                return true;
            }
            au_buf_free(&hp);
            (*attempts)++;
            t->pending_auth = p;
            t->pending_proxy = proxy;
            AuAuthChallenge ch = {proxy, p.scheme, p.realm, t->url.host, *attempts};
            AuBuf user, pass;
            au_buf_init(&user);
            au_buf_init(&pass);
            int r = t->cb.credentials(t->cb.ctx, t, &ch, &user, &pass);
            if (r == 1) {
                au_buf_init(&hp);
                if (proxy) {
                    char key[300];
                    session_key(t->conn && t->conn->proxy ? t->conn->proxy : "", t->conn ? t->conn->proxy_port : 0,
                                key, sizeof key);
                    au_buf_puts(&hp, key);
                } else {
                    hostport(&t->url, &hp);
                }
                auth_store(net, au_buf_cstr(&hp), proxy, &p, au_buf_cstr(&user), au_buf_cstr(&pass));
                au_buf_free(&hp);
                memset(pass.data, 0, pass.len);
                au_buf_free(&user);
                au_buf_free(&pass);
                if (t->body && t->body->rewind && !t->body->rewind(t->body->ctx)) {
                    fail_later(t, AU_NET_ERR_PROXY);
                    return true;
                }
                t->state = T_DRAIN;
                return true;
            }
            au_buf_free(&user);
            au_buf_free(&pass);
            if (r < 0) {
                t->state = T_WAIT_CRED;
                detach_conn(t, false);
                reset_attempt(t);
                return true;
            }
        }
    }
    return false;
}

void au_http_answer_credentials(AuHttpTransfer *t, const char *user, const char *pass)
{
    if (t->state != T_WAIT_CRED)
        return;
    if (!user) {
        /* Gave up: report the failure (the 401 body was not kept). */
        fail_later(t, t->pending_proxy ? AU_NET_ERR_PROXY : AU_NET_ERR_CANCELLED);
        return;
    }
    AuBuf hp;
    au_buf_init(&hp);
    hostport(&t->url, &hp);
    auth_store(t->net, au_buf_cstr(&hp), t->pending_proxy, &t->pending_auth, user, pass ? pass : "");
    au_buf_free(&hp);
    t->state = T_START;
}

/* ---- Hosts accepted over plain http ---- */

bool au_net_http_allowed(const AuNet *net, const char *host)
{
    for (size_t i = 0; i < net->n_http_ok; i++)
        if (!strcmp(net->http_ok[i], host))
            return true;
    return false;
}

void au_net_allow_http(AuNet *net, const char *host)
{
    if (!host || !*host || au_net_http_allowed(net, host))
        return;
    net->http_ok = au_realloc(net->http_ok, (net->n_http_ok + 1) * sizeof *net->http_ok);
    net->http_ok[net->n_http_ok++] = au_strndup(host, strlen(host));
}

void au_net_clear_http_allowed(AuNet *net)
{
    for (size_t i = 0; i < net->n_http_ok; i++)
        au_free(net->http_ok[i]);
    au_free(net->http_ok);
    net->http_ok = NULL;
    net->n_http_ok = 0;
}

/* ---- The transfer state machine ---- */

static bool hsts_upgrade(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    if (strcmp(t->url.scheme, "http"))
        return false;
    bool known = t->url.host_type == AU_URL_HOST_DOMAIN && au_hsts_is_known(net->hsts, t->url.host, wall(net));
    /* A navigation the user confirmed over http: remember the host, so
     * the next page on that site does not ask again. */
    if (!known && t->allow_http_fallback && t->dest == AU_DEST_DOCUMENT && t->url.host_type == AU_URL_HOST_DOMAIN)
        au_net_allow_http(net, t->url.host);
    bool first = net->cfg.https_first && !t->allow_http_fallback && t->url.host_type == AU_URL_HOST_DOMAIN &&
                 strcmp(t->url.host, "localhost") && !au_net_http_allowed(net, t->url.host);
    if (!known && !first)
        return false;
    au_url_upgrade_to_https(&t->url);
    t->https_first_upgrade = !known;
    if (!known)
        t->was_upgraded = true;
    return true;
}

static Conn *find_idle(AuNet *net, const char *host, uint16_t port, bool tls)
{
    for (Conn *c = net->conns; c; c = c->next)
        if (!c->owner && c->state == C_READY && c->port == port && c->tls == tls && !strcmp(c->host, host) &&
            !c->eof)
            return c;
    return NULL;
}

/* Closes the least recently used idle connection; false when none. */
static bool evict_idle(AuNet *net)
{
    Conn *victim = NULL;
    for (Conn *c = net->conns; c; c = c->next)
        if (!c->owner && (!victim || (int32_t)(c->last_ms - victim->last_ms) < 0))
            victim = c;
    if (!victim)
        return false;
    conn_remove(net, victim);
    return true;
}

static bool acquire_conn(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    bool tls = !strcmp(t->url.scheme, "https");
    uint16_t port = (uint16_t)au_url_effective_port(&t->url);
    if (tls && !net->roots) {
        fail_later(t, AU_NET_ERR_CERT);
        return true;
    }
    Conn *c = find_idle(net, t->url.host, port, tls);
    if (c) {
        /* A reused connection the peer closed meanwhile: drop it. */
        conn_io(c);
        if (c->eof || c->state == C_DEAD || c->in.len > c->in_off) {
            conn_remove(net, c);
            return false;
        }
        t->reused = true;
    } else {
        int total;
        int n = count_conns(net, t->url.host, port, tls, &total);
        if (n >= net->cfg.max_per_host)
            return false;
        if (total >= net->cfg.max_total && !evict_idle(net))
            return false;
        if (tls && !drbg_ready(net))
            return false; /* wait for entropy */
        c = conn_new(net, t->url.host, port, tls);
        t->reused = false;
    }
    c->owner = t;
    t->conn = c;
    t->state = T_SEND;
    return true;
}

static AuNetError conn_error_for(AuHttpTransfer *t, AuNetError e)
{
    if (t->https_first_upgrade &&
        (e == AU_NET_ERR_CONNECT || e == AU_NET_ERR_TLS || e == AU_NET_ERR_CERT || e == AU_NET_ERR_CLOCK ||
         e == AU_NET_ERR_TIMEOUT || e == AU_NET_ERR_RESET || e == AU_NET_ERR_DNS)) {
        if (e == AU_NET_ERR_DNS)
            return e;
        t->fallback_cause = e == AU_NET_ERR_TLS && t->tls_never_spoke ? AU_NET_ERR_PROTOCOL : e;
        return AU_NET_ERR_HTTPS_FALLBACK;
    }
    return e;
}

AuNetError au_http_fallback_cause(const AuHttpTransfer *t) { return t->fallback_cause; }

/* ---- Missing intermediates (Authority Information Access) ----
 * A certificate error whose chain stops for want of an issuer, where the
 * last certificate names a caIssuers URL: fetch that URL over plain http
 * (no cookies, no referrer, no cache, a size cap and a timeout), put what
 * it holds in the intermediates pool, and connect again; the TLS layer
 * then builds the path with it and validates it in full. At most
 * AIA_MAX_DEPTH fetches per transfer, each URL once. */

static AiaSeen *aia_seen(AuNet *net, const char *url)
{
    for (int i = 0; i < AIA_SEEN; i++)
        if (net->aia_seen[i].url && !strcmp(net->aia_seen[i].url, url))
            return &net->aia_seen[i];
    return NULL;
}

static void aia_remember(AuNet *net, const char *url, bool ok)
{
    AiaSeen *s = aia_seen(net, url);
    if (!s) {
        s = &net->aia_seen[net->next_aia_seen];
        net->next_aia_seen = (net->next_aia_seen + 1) % AIA_SEEN;
        au_free(s->url);
        s->url = au_strndup(url, strlen(url));
    }
    s->ok = ok;
}

static void aia_response(void *ctx, AuHttpTransfer *child, const AuHttpResponse *r)
{
    (void)child;
    AuHttpTransfer *t = ctx;
    t->aia_status = r->status;
}

static void aia_data(void *ctx, AuHttpTransfer *child, const void *data, size_t len)
{
    AuHttpTransfer *t = ctx;
    if (t->aia_body.len + len > AIA_MAX_BYTES) {
        /* Too large for a certificate bundle: give up on this URL. */
        au_http_cancel(child);
        t->aia_child = NULL;
        aia_remember(t->net, t->aia_url, false);
        fail_later(t, conn_error_for(t, AU_NET_ERR_CERT));
        return;
    }
    au_buf_append(&t->aia_body, data, len);
}

static void aia_add_cert(void *ctx, const uint8_t *der, size_t len)
{
    au_cert_pool_add(ctx, der, len);
}

static void aia_done(void *ctx, AuHttpTransfer *child, AuNetError err)
{
    (void)child;
    AuHttpTransfer *t = ctx;
    t->aia_child = NULL;
    int found = 0;
    if (err == AU_NET_OK && t->aia_status == 200)
        found = au_cert_bundle_parse((const uint8_t *)t->aia_body.data, t->aia_body.len, 4, aia_add_cert,
                                     &t->net->pool);
    au_buf_free(&t->aia_body);
    au_buf_init(&t->aia_body);
    aia_remember(t->net, t->aia_url, found > 0);
    if (found > 0)
        t->aia_ready = true;
    else
        fail_later(t, conn_error_for(t, AU_NET_ERR_CERT));
}

/* After a certificate error: true when a caIssuers fetch may complete the
 * chain, and the transfer now waits for it (T_AIA). */
static bool aia_begin(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    if (t->aia_internal || !t->have_cert || !t->cert.missing_issuer || !t->cert.aia_url[0] ||
        t->aia_rounds >= AIA_MAX_DEPTH)
        return false;
    const char *u = t->cert.aia_url;
    for (int i = 0; i < t->aia_rounds; i++)
        if (!strcmp(t->aia_tried[i], u))
            return false; /* fetched once already and still short: a loop */
    const AiaSeen *s = aia_seen(net, u);
    if (s && !s->ok)
        return false; /* failed earlier this session */
    t->aia_tried[t->aia_rounds++] = au_strndup(u, strlen(u));
    snprintf(t->aia_url, sizeof t->aia_url, "%s", u);
    t->aia_ready = t->aia_waited = false;
    /* A URL that already gave certificates (another connection's fetch,
     * finished after this handshake began): connect again at once. */
    if (s && s->ok)
        t->aia_waited = true;
    detach_conn(t, false);
    reset_attempt(t);
    t->state = T_AIA;
    return true;
}

static bool aia_step(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    if (t->aia_child)
        return false;
    if (t->aia_ready || t->aia_waited) {
        t->aia_ready = t->aia_waited = false;
        t->state = T_START;
        return true;
    }
    for (AuHttpTransfer *o = net->transfers; o; o = o->next)
        if (o != t && o->aia_child && !o->cancelled && !strcmp(o->aia_url, t->aia_url)) {
            t->aia_waited = true; /* connect again once that fetch ends */
            return false;
        }
    AuHttpRequest rq;
    au_http_request_init(&rq, t->aia_url);
    rq.dest = AU_DEST_EMPTY;
    rq.mode = AU_MODE_NO_CORS;
    rq.omit_credentials = true;
    rq.allow_http_fallback = true; /* caIssuers URLs are plain http; HTTPS-first does not apply */
    rq.total_timeout_ms = AIA_TIMEOUT_MS;
    AuHttpCallbacks cb = {t, aia_response, aia_data, aia_done, NULL, NULL};
    t->aia_status = 0;
    au_buf_clear(&t->aia_body);
    t->aia_child = au_http_start(net, &rq, &cb);
    t->aia_child->aia_internal = true;
    au_http_request_free(&rq);
    return true;
}

/* ---- Intermediates for the embedder ---- */

bool au_net_intermediates_dirty(const AuNet *net) { return net->pool.dirty; }
int au_net_intermediates_count(const AuNet *net) { return net->pool.n; }

void au_net_intermediates_serialize(AuNet *net, AuBuf *out)
{
    au_cert_pool_serialize(&net->pool, out);
    net->pool.dirty = false;
}

bool au_net_intermediates_parse(AuNet *net, const void *data, size_t len)
{
    return au_cert_pool_parse(&net->pool, data, len);
}

/* A connection failed under the transfer: retry once on a fresh one when
 * that is safe, else fail. */
static void conn_failed(AuHttpTransfer *t, AuNetError e)
{
    Conn *c = t->conn;
    if (c && c->tls_engine)
        record_tls(t, c);
    /* A "TLS failure" from a server that never spoke TLS (it closed, or
     * answered in plain text) means it has no HTTPS. */
    if (e == AU_NET_ERR_TLS && c && c->tls_engine && !au_tls_peer_spoke_tls(c->tls_engine))
        t->tls_never_spoke = true;
    if (c && c->tls_engine && (e == AU_NET_ERR_CERT)) {
        int64_t now = wall(t->net);
        if (t->net->roots && now < au_roots_date(t->net->roots)) {
            e = AU_NET_ERR_CLOCK;
            t->cert.clock_wrong = true;
        }
        t->cert.hsts = au_hsts_is_known(t->net->hsts, t->url.host, now);
        if (e == AU_NET_ERR_CERT && aia_begin(t))
            return;
    }
    bool safe = method_idempotent(t->method) && (!t->body || (t->body->rewind && t->body->rewind(t->body->ctx)));
    if (t->reused && !t->got_bytes && safe && t->retries < 1 &&
        (e == AU_NET_ERR_RESET || e == AU_NET_ERR_PROTOCOL)) {
        t->retries++;
        detach_conn(t, false);
        reset_attempt(t);
        t->state = T_START;
        return;
    }
    detach_conn(t, false);
    fail_later(t, conn_error_for(t, e));
}

/* Parses what the connection received. Returns true on progress. */
static bool recv_step(AuHttpTransfer *t)
{
    Conn *c = t->conn;
    bool any = false;
    while (conn_pending_in(c) && (t->state == T_RECV || t->state == T_DRAIN || t->state == T_SEND)) {
        size_t used;
        AuHttpEvent ev;
        t->got_bytes = true;
        AuHttpParseResult r = au_http_parser_feed(&t->parser, (const uint8_t *)c->in.data + c->in_off,
                                                  conn_pending_in(c), &used, &ev);
        c->in_off += used;
        any = true;
        t->last_ms = now_ms(t->net);
        switch (r) {
        case AU_HP_ERROR:
            conn_failed(t, strstr(t->parser.error ? t->parser.error : "", "large") ? AU_NET_ERR_TOO_LARGE
                                                                                  : AU_NET_ERR_PROTOCOL);
            return true;
        case AU_HP_HEADERS:
            if (c->tls_engine)
                record_tls(t, c);
            if (!handle_head(t)) {
                if (t->state == T_FAILED)
                    return true;
                setup_decoders(t);
                deliver_response(t);
                cache_begin(t);
                t->state = T_RECV;
            } else if (t->state != T_DRAIN) {
                return true;
            }
            if (au_http_parser_have_headers(&t->parser) && t->parser.framing == AU_BODY_NONE) {
                /* fall through to DONE on the next feed */
            }
            break;
        case AU_HP_BODY:
            if (t->state == T_DRAIN) {
                t->received += (int64_t)ev.len;
                if (t->received > (int64_t)DRAIN_MAX) {
                    restart(t, false);
                    return true;
                }
            } else if (!deliver_body(t, ev.data, ev.len)) {
                return true;
            }
            break;
        case AU_HP_DONE:
            if (t->state == T_DRAIN) {
                restart(t, true);
                return true;
            }
            finish(t, AU_NET_OK);
            return true;
        case AU_HP_NEED_MORE:
            break;
        }
        if (t->state == T_FAILED || t->state == T_FINISHED || t->cancelled)
            return true;
    }
    /* A message with no body ends right after its head. */
    if (t->conn && t->parser_live && au_http_parser_have_headers(&t->parser)) {
        size_t used;
        AuHttpEvent ev;
        if (t->parser.framing == AU_BODY_NONE || (t->parser.framing == AU_BODY_LENGTH && t->parser.remaining == 0)) {
            if (au_http_parser_feed(&t->parser, NULL, 0, &used, &ev) == AU_HP_DONE) {
                if (t->state == T_DRAIN)
                    restart(t, true);
                else
                    finish(t, AU_NET_OK);
                return true;
            }
        }
    }
    if (c->in_off)
        conn_compact(&c->in, &c->in_off);
    if (t->conn && c->eof && !conn_pending_in(c)) {
        AuHttpParseResult r = au_http_parser_eof(&t->parser);
        if (r == AU_HP_DONE) {
            t->parser.keep_alive = false;
            if (t->state == T_DRAIN)
                restart(t, false);
            else
                finish(t, AU_NET_OK);
        } else {
            conn_failed(t, AU_NET_ERR_RESET);
        }
        return true;
    }
    return any;
}

static bool step(AuHttpTransfer *t)
{
    AuNet *net = t->net;
    uint32_t now = now_ms(net);
    if (t->cancelled) {
        aia_drop_child(t);
        detach_conn(t, false);
        t->state = T_FINISHED;
        return true;
    }
    if (t->total_timeout && elapsed(now, t->started_ms) > t->total_timeout && t->state != T_FINISHED &&
        t->state != T_FAILED) {
        detach_conn(t, false);
        fail_later(t, AU_NET_ERR_TIMEOUT);
    }
    switch (t->state) {
    case T_START:
        hsts_upgrade(t);
        if (!t->cache_entry && cache_try(t))
            return true;
        t->state = T_WAIT_CONN;
        return true;
    case T_WAIT_CONN:
        return acquire_conn(t);
    case T_SEND:
    case T_RECV:
    case T_DRAIN: {
        Conn *c = t->conn;
        bool any = false;
        if (c->state == C_DEAD) {
            if (t->pending_proxy && t->cb.credentials && !t->cancelled && t->proxy_auth_attempts < MAX_AUTH_ATTEMPTS) {
                /* A 407 to CONNECT: ask, then try again. */
                t->pending_proxy = false;
                t->proxy_auth_attempts++;
                AuAuthChallenge ch = {true, t->pending_auth.scheme, t->pending_auth.realm, c->proxy,
                                      t->proxy_auth_attempts};
                AuBuf user, pass;
                au_buf_init(&user);
                au_buf_init(&pass);
                char key[300];
                session_key(c->proxy, c->proxy_port, key, sizeof key);
                int r = t->cb.credentials(t->cb.ctx, t, &ch, &user, &pass);
                if (r == 1)
                    auth_store(net, key, true, &t->pending_auth, au_buf_cstr(&user), au_buf_cstr(&pass));
                au_buf_free(&user);
                au_buf_free(&pass);
                detach_conn(t, false);
                reset_attempt(t);
                if (r == 1)
                    t->state = T_START;
                else
                    fail_later(t, AU_NET_ERR_PROXY);
                return true;
            }
            conn_failed(t, c->err);
            return true;
        }
        if (c->state != C_READY) {
            any = conn_step(c);
            if (c->state == C_DEAD || c->state != C_READY)
                return any;
        }
        if (!t->head_sent) {
            if (!t->parser_live) {
                au_http_parser_init(&t->parser, !strcmp(t->method, "HEAD"));
                t->parser_live = true;
            }
            build_request(t, c);
            t->last_ms = now;
            any = true;
        }
        if (t->state == T_SEND) {
            any |= pump_body(t, c);
            if (t->state == T_FAILED)
                return true;
            if (t->body_done && c->out.len == c->out_off)
                t->state = T_RECV;
        }
        any |= conn_io(c);
        if (c->state == C_DEAD)
            return true;
        if (t->state == T_SEND && t->body_done && c->out.len == c->out_off)
            t->state = T_RECV;
        any |= recv_step(t);
        if (any)
            t->last_ms = now;
        if ((t->state == T_SEND || t->state == T_RECV || t->state == T_DRAIN) && t->conn &&
            elapsed(now, t->last_ms) > net->cfg.idle_timeout_ms) {
            detach_conn(t, false);
            fail_later(t, AU_NET_ERR_TIMEOUT);
            return true;
        }
        return any;
    }
    case T_WAIT_CRED:
        return false;
    case T_AIA:
        return aia_step(t);
    case T_FAILED:
        finish(t, t->fail_err);
        return true;
    default:
        return false;
    }
}

bool au_net_pump(AuNet *net)
{
    if (net->in_pump)
        return false;
    net->in_pump = true;
    bool any = false;
    /* Idle connections: notice closes, expire. */
    uint32_t now = now_ms(net);
    au_tls_tick(now); /* the TLS library's millisecond clock (ticket ages) */
    for (Conn *c = net->conns, *next; c; c = next) {
        next = c->next;
        if (c->owner)
            continue;
        if (c->state == C_READY)
            conn_io(c);
        if (c->state == C_DEAD || c->eof || elapsed(now, c->last_ms) > net->cfg.keepalive_ms ||
            c->in.len > c->in_off) {
            conn_remove(net, c);
            any = true;
        }
    }
    for (AuHttpTransfer *t = net->transfers; t; t = t->next) {
        for (int k = 0; k < 16 && t->state != T_FINISHED; k++) {
            bool moved = step(t);
            trace_note(t); /* the request log's phase times */
            if (!moved)
                break;
            any = true;
        }
    }
    /* Reap. */
    for (AuHttpTransfer **pp = &net->transfers; *pp;) {
        AuHttpTransfer *t = *pp;
        if (t->state == T_FINISHED || (t->cancelled && !t->conn)) {
            if (t->state != T_FINISHED)
                detach_conn(t, false);
            *pp = t->next;
            transfer_free(t);
            any = true;
        } else {
            pp = &t->next;
        }
    }
    net->in_pump = false;
    return any;
}

void au_net_close_idle(AuNet *net)
{
    for (Conn *c = net->conns, *next; c; c = next) {
        next = c->next;
        if (!c->owner)
            conn_remove(net, c);
    }
}

void au_net_free(AuNet *net)
{
    if (!net)
        return;
    while (net->transfers) {
        AuHttpTransfer *t = net->transfers;
        net->transfers = t->next;
        detach_conn(t, false);
        transfer_free(t);
    }
    while (net->conns)
        conn_remove(net, net->conns);
    au_dns_free(net->dns);
    for (int i = 0; i < SESSION_CACHE; i++) {
        au_free(net->sessions[i].hostport);
        au_free(net->sessions[i].cert);
    }
    au_net_clear_cert_exceptions(net);
    au_net_clear_http_allowed(net);
    au_cert_pool_free(&net->pool);
    for (int i = 0; i < AIA_SEEN; i++)
        au_free(net->aia_seen[i].url);
    for (int i = 0; i < AUTH_CACHE; i++) {
        au_free(net->auth[i].hostport);
        au_free(net->auth[i].realm);
        au_free(net->auth[i].user);
        au_free(net->auth[i].pass);
    }
    au_brotli_dict_free(net->brdict);
    AuNetConfig *c = &net->cfg;
    au_free(c->user_agent);
    au_free(c->accept_language);
    au_free(c->http_proxy_host);
    au_free(c->https_proxy_host);
    au_free(net);
}

AuHttpPhase au_http_phase(const AuHttpTransfer *t)
{
    switch (t->state) {
    case T_START: return AU_PHASE_QUEUED;
    case T_WAIT_CONN:
        return !strcmp(t->url.scheme, "https") && !t->net->drbg_ready ? AU_PHASE_ENTROPY : AU_PHASE_QUEUED;
    case T_SEND:
    case T_RECV:
    case T_DRAIN:
        if (!t->conn)
            return AU_PHASE_QUEUED;
        switch (t->conn->state) {
        case C_RESOLVE: return AU_PHASE_RESOLVING;
        case C_CONNECT:
        case C_PROXY: return AU_PHASE_CONNECTING;
        case C_TLS: return AU_PHASE_SECURING;
        default: break;
        }
        if (t->state == T_SEND)
            return AU_PHASE_SENDING;
        return t->got_bytes ? AU_PHASE_RECEIVING : AU_PHASE_WAITING;
    case T_WAIT_CRED: return AU_PHASE_WAITING;
    case T_AIA: return AU_PHASE_SECURING;
    default: return AU_PHASE_DONE;
    }
}

/* ---- Tracing (the developer tools' request log) ---- */

static void trace_init(AuHttpTransfer *t)
{
    for (int i = 0; i <= AU_PHASE_DONE; i++)
        t->trace.at[i] = -1;
    t->trace.at[AU_PHASE_QUEUED] = 0;
    t->trace.phase = AU_PHASE_QUEUED;
}

/* Stamps the phase the transfer is in when it changed since the last
 * look. A phase earlier than the last one is a new attempt (a redirect,
 * a retry): the later stamps are dropped. */
static void trace_note(AuHttpTransfer *t)
{
    AuHttpPhase ph = au_http_phase(t);
    AuHttpTrace *tr = &t->trace;
    if (ph == tr->phase)
        return;
    int32_t dt = (int32_t)elapsed(now_ms(t->net), t->started_ms);
    if (ph < tr->phase)
        for (int i = ph + 1; i <= AU_PHASE_DONE; i++)
            tr->at[i] = -1;
    if (ph != AU_PHASE_QUEUED)
        tr->at[ph] = dt;
    tr->phase = ph;
}

void au_http_trace(const AuHttpTransfer *t, AuHttpTrace *out)
{
    *out = t->trace;
    out->reused = t->reused && t->trace.connection != 0;
    out->transfer_bytes = t->trace.source == AU_HTTP_FROM_NETWORK ? t->received : 0;
    out->redirects = t->redirects;
}

const AuHeaders *au_http_response_headers(const AuHttpTransfer *t) { return &t->resp_headers; }
const char *au_http_request_head(const AuHttpTransfer *t) { return t->request_head ? t->request_head : ""; }
void au_net_set_trace_headers(AuNet *net, bool on) { net->trace_headers = on; }
bool au_net_trace_headers(const AuNet *net) { return net->trace_headers; }

int au_net_connections(const AuNet *net, AuNetConnInfo *out, int max)
{
    int n = 0;
    for (const Conn *c = net->conns; c; c = c->next) {
        if (c->state == C_DEAD)
            continue;
        if (n < max) {
            snprintf(out[n].host, sizeof out[n].host, "%s", c->host);
            out[n].port = c->port;
            out[n].tls = c->tls;
            out[n].busy = c->owner != NULL;
            out[n].id = c->id;
            out[n].served = c->served;
        }
        n++;
    }
    return n;
}
