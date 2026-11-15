/* Aura network stack: HTTP/1.1 over TCP and TLS, driven by an event loop.
 *
 * An AuNet holds everything shared between requests: the platform's
 * socket table (AuNetOps), the connection pool, the TLS configuration and
 * root store, and the optional policy stores (cookie jar, HSTS, cache).
 * There is no global state; one AuNet per application.
 *
 *     AuNet *net = au_net_new(&ops);
 *     AuHttpRequest rq;
 *     au_http_request_init(&rq, "https://example.com/");
 *     AuHttpTransfer *t = au_http_start(net, &rq, &callbacks);
 *     ...
 *     every event loop turn: au_net_pump(net);
 *
 * Callbacks are made only from au_net_pump, never from au_http_start or
 * au_http_cancel. After done() the transfer is freed; the caller must not
 * use the pointer again. After au_http_cancel no callback is made (no
 * done()) and the caller must not use the pointer again; the transfer
 * is freed on the next pump.
 *
 * The protocol code is sans I/O: response parsing (http_parse.h), TLS
 * (tls.h), cookies, HSTS and the cache are state machines over bytes and
 * values; only au_net_pump talks to AuNetOps. Everything runs on the main
 * task (see CLAUDE.md, Networking). */
#ifndef AURA_NET_H
#define AURA_NET_H

#include "../platform/platform.h"
#include "headers.h"

typedef struct AuNet AuNet;
typedef struct AuHttpTransfer AuHttpTransfer;
typedef struct AuCookieJar AuCookieJar;
typedef struct AuHsts AuHsts;
typedef struct AuHttpCache AuHttpCache;
typedef struct AuRootStore AuRootStore;
typedef struct AuCertInfo AuCertInfo;

typedef enum {
    AU_NET_OK = 0,
    AU_NET_ERR_BAD_URL,        /* the URL does not parse, or has no host */
    AU_NET_ERR_SCHEME,         /* not http or https */
    AU_NET_ERR_DNS,            /* the host name did not resolve */
    AU_NET_ERR_CONNECT,        /* the connection was refused or failed */
    AU_NET_ERR_TIMEOUT,        /* connect, idle or total timeout */
    AU_NET_ERR_RESET,          /* the connection broke during the exchange */
    AU_NET_ERR_PROTOCOL,       /* malformed HTTP response */
    AU_NET_ERR_TOO_LARGE,      /* a limit was exceeded */
    AU_NET_ERR_REDIRECTS,      /* too many redirects, or one to a bad URL */
    AU_NET_ERR_DECODE,         /* corrupt Content-Encoding data */
    AU_NET_ERR_TLS,            /* the TLS handshake or connection failed */
    AU_NET_ERR_CERT,           /* the certificate is not valid (see au_http_cert_info) */
    AU_NET_ERR_CLOCK,          /* the system clock is implausible, so certificates cannot be checked */
    AU_NET_ERR_PROXY,          /* the proxy refused or failed the request */
    AU_NET_ERR_HTTPS_FALLBACK, /* HTTPS-first: https failed; the embedder may confirm http */
    AU_NET_ERR_BLOCKED,        /* refused by policy (mixed content, HSTS host over http) */
    AU_NET_ERR_CANCELLED
} AuNetError;

const char *au_net_error_name(AuNetError e);

/* ---- Configuration ----
 * Defaults are set by au_net_new; au_net_apply_prefs (net_prefs.c) copies
 * the user's preferences in. Strings are copied. */
typedef enum { AU_COOKIE_POLICY_ALL, AU_COOKIE_POLICY_SAME_SITE, AU_COOKIE_POLICY_NONE } AuNetCookiePolicy;

typedef struct AuNetConfig {
    char *user_agent;
    char *accept_language;
    bool send_dnt, send_gpc;
    bool https_first;
    AuNetCookiePolicy cookie_policy;
    bool block_third_party_cookies;
    char *http_proxy_host;  /* "" for none */
    int http_proxy_port;
    char *https_proxy_host; /* "" for none */
    int https_proxy_port;
    int max_per_host;       /* connections per host (6) */
    int max_total;          /* connections in all (16) */
    uint32_t connect_timeout_ms; /* 30 s */
    uint32_t idle_timeout_ms;    /* no bytes while waiting: 60 s */
    uint32_t keepalive_ms;       /* pooled idle connections close after 30 s */
    int max_redirects;           /* 20 */
    size_t max_body;             /* decoded body cap per response: 64 MB */
} AuNetConfig;

AuNet *au_net_new(const AuNetOps *ops);
void au_net_free(AuNet *net); /* cancels everything, closes sockets */
AuNetConfig *au_net_config(AuNet *net);
const AuNetOps *au_net_ops(AuNet *net);
void au_net_set_config_str(char **field, const char *value);

/* Policy stores, all optional and owned by the caller (they outlive the
 * AuNet). Without a cookie jar no cookies are sent or stored; without
 * HSTS no upgrade happens; without a cache every request goes out. */
void au_net_set_cookie_jar(AuNet *net, AuCookieJar *jar);
void au_net_set_hsts(AuNet *net, AuHsts *hsts);
void au_net_set_cache(AuNet *net, AuHttpCache *cache);
AuCookieJar *au_net_cookie_jar(AuNet *net);
AuHsts *au_net_hsts(AuNet *net);
AuHttpCache *au_net_cache(AuNet *net);
/* The root store for certificate validation (owned by the caller). Without
 * one, every https request fails with AU_NET_ERR_CERT. */
void au_net_set_roots(AuNet *net, const AuRootStore *roots);

/* Drives all transfers: DNS, connections, TLS, sending and receiving,
 * timeouts. Cheap when nothing is ready. Returns true if anything
 * happened (the shell may then pump again before sleeping). */
bool au_net_pump(AuNet *net);
/* Transfers in progress (the shell polls faster while there are any). */
int au_net_active(const AuNet *net);
/* Closes idle pooled connections (for example before sleep or quit). */
void au_net_close_idle(AuNet *net);

/* ---- Requests ---- */
typedef enum {
    AU_DEST_DOCUMENT,
    AU_DEST_IFRAME,
    AU_DEST_STYLE,
    AU_DEST_SCRIPT,
    AU_DEST_IMAGE,
    AU_DEST_FONT,
    AU_DEST_EMPTY /* fetch(), XHR, downloads */
} AuRequestDest;

typedef enum { AU_MODE_NAVIGATE, AU_MODE_NO_CORS, AU_MODE_CORS, AU_MODE_SAME_ORIGIN } AuRequestMode;

typedef enum {
    AU_RP_DEFAULT, /* strict-origin-when-cross-origin */
    AU_RP_NO_REFERRER,
    AU_RP_NO_REFERRER_WHEN_DOWNGRADE,
    AU_RP_SAME_ORIGIN,
    AU_RP_ORIGIN,
    AU_RP_STRICT_ORIGIN,
    AU_RP_ORIGIN_WHEN_CROSS_ORIGIN,
    AU_RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN,
    AU_RP_UNSAFE_URL
} AuReferrerPolicy;

/* A request body. read() fills buf and returns the byte count, 0 at the
 * end, or -1 on error; rewind() starts again (for redirects 307/308 and
 * retries; NULL when the body cannot be replayed). length is -1 when
 * unknown (the body is then sent chunked). The transfer frees it. */
typedef struct AuBodySource {
    void *ctx;
    long (*read)(void *ctx, void *buf, size_t cap);
    bool (*rewind)(void *ctx);
    void (*free)(void *ctx);
    int64_t length;
    char *content_type; /* au_alloc'd or NULL */
} AuBodySource;

/* A body from a copy of bytes. */
AuBodySource *au_body_from_bytes(const void *data, size_t len, const char *content_type);
void au_body_free(AuBodySource *b);

typedef struct AuHttpRequest {
    const char *method; /* "GET" */
    const char *url;
    AuHeaders headers;  /* extra request headers (owned; freed by au_http_request_free) */
    AuBodySource *body; /* NULL for none; the transfer takes ownership */
    AuRequestDest dest;
    AuRequestMode mode;
    /* The document that caused the request: its URL (for Referer and
     * Sec-Fetch-Site) and policy. NULL for browser-initiated requests
     * (typed URL, bookmark), which send no Referer and Sec-Fetch-Site:
     * none. */
    const char *referrer;
    AuReferrerPolicy referrer_policy;
    /* The top-level document's URL, for third-party cookie decisions and
     * partitioned cookies; NULL when this request is itself the top-level
     * navigation. */
    const char *top_level_url;
    bool user_activated; /* Sec-Fetch-User: ?1 for navigations by the user */
    bool reload;         /* bypass the cache (end-to-end reload) */
    bool omit_credentials; /* no cookies, no HTTP auth */
    bool manual_redirect;  /* deliver 3xx responses instead of following */
    bool allow_http_fallback; /* HTTPS-first: the user confirmed plain http */
    int64_t range_start, range_end; /* bytes=start-end; -1: none / open */
    uint32_t total_timeout_ms;      /* 0: none */
} AuHttpRequest;

void au_http_request_init(AuHttpRequest *rq, const char *url);
void au_http_request_free(AuHttpRequest *rq); /* frees headers (and body if never started) */

/* Security state of a response. */
typedef struct AuSecurityInfo {
    bool tls;
    uint16_t version;       /* 0x0303 for TLS 1.2 */
    const char *cipher;     /* "ECDHE-RSA-AES128-GCM-SHA256", ... */
    const char *alpn;       /* "http/1.1" or "" */
    bool resumed;
} AuSecurityInfo;

typedef struct AuHttpResponse {
    int status;
    const char *reason;
    const AuHeaders *headers;
    const char *url;          /* the final URL, after redirects */
    int redirects;
    int64_t content_length;   /* of the body as sent (-1 when unknown) */
    const char *mime;         /* Content-Type essence, lower case; "" when absent */
    const char *charset;      /* Content-Type charset parameter; "" when absent */
    const char *content_encoding; /* as sent; "" when identity */
    bool from_cache;
    bool revalidated;         /* a cached response confirmed by 304 */
    AuSecurityInfo security;
} AuHttpResponse;

/* What a credentials callback is asked. */
typedef struct AuAuthChallenge {
    bool proxy;          /* 407 */
    const char *scheme;  /* "Basic" or "Digest" */
    const char *realm;
    const char *host;    /* host[:port] asking */
    int attempt;         /* 1 for the first ask; more after a rejection */
} AuAuthChallenge;

typedef struct AuHttpCallbacks {
    void *ctx;
    /* The final response's status and headers (after redirects and
     * authentication). r and every string it points to are valid only
     * during the call; copy what is needed. */
    void (*response)(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r);
    /* Decoded body bytes. */
    void (*data)(void *ctx, AuHttpTransfer *t, const void *data, size_t len);
    /* The end: AU_NET_OK or an error. The transfer is freed after this. */
    void (*done)(void *ctx, AuHttpTransfer *t, AuNetError err);
    /* Optional: a redirect is about to be followed; return false to stop
     * and deliver the 3xx response itself. */
    bool (*redirect)(void *ctx, AuHttpTransfer *t, int status, const char *url);
    /* Optional: credentials for a 401/407. Return 1 after filling user and
     * pass, 0 to give up (the 401 response is delivered), or -1 to answer
     * later with au_http_answer_credentials. */
    int (*credentials)(void *ctx, AuHttpTransfer *t, const AuAuthChallenge *ch, AuBuf *user, AuBuf *pass);
} AuHttpCallbacks;

/* Starts a request (copies what it needs from rq; the body's ownership
 * passes). Never NULL: a request that cannot start fails through done()
 * on the next pump. */
AuHttpTransfer *au_http_start(AuNet *net, AuHttpRequest *rq, const AuHttpCallbacks *cb);
/* Cancels a transfer: no further callbacks; freed on the next pump. */
void au_http_cancel(AuHttpTransfer *t);
/* Answers a deferred credentials request; user NULL gives up. */
void au_http_answer_credentials(AuHttpTransfer *t, const char *user, const char *pass);

/* About a transfer, valid until done(). */
const char *au_http_url(const AuHttpTransfer *t);   /* current URL */
int64_t au_http_received(const AuHttpTransfer *t);  /* body bytes so far, as sent */
/* Certificate details after AU_NET_ERR_CERT or AU_NET_ERR_CLOCK (and for
 * a successful https response): NULL when none. Valid until done()
 * returns; copy what is needed. */
const AuCertInfo *au_http_cert_info(const AuHttpTransfer *t);
/* A user pointer. */
void au_http_set_user(AuHttpTransfer *t, void *user);
void *au_http_user(const AuHttpTransfer *t);

/* What a transfer is doing, for the status bar. */
typedef enum {
    AU_PHASE_QUEUED,     /* waiting to start, or for a free connection */
    AU_PHASE_ENTROPY,    /* https: waiting for the platform's entropy source */
    AU_PHASE_RESOLVING,  /* looking up the host name */
    AU_PHASE_CONNECTING, /* opening the TCP connection (or the proxy tunnel) */
    AU_PHASE_SECURING,   /* the TLS handshake */
    AU_PHASE_SENDING,    /* sending the request */
    AU_PHASE_WAITING,    /* sent; no response bytes yet */
    AU_PHASE_RECEIVING,  /* response bytes arriving */
    AU_PHASE_DONE
} AuHttpPhase;
AuHttpPhase au_http_phase(const AuHttpTransfer *t);

/* ---- Certificate exceptions ----
 * The user chose to continue past a certificate error on host: later
 * connections accept a chain whose leaf certificate has this SHA-256
 * fingerprint (and no other) despite the error, until
 * au_net_clear_cert_exceptions or au_net_free (exceptions are never
 * saved). Responses over such connections carry AuCertInfo.overridden.
 * Refused (false) for an HSTS host. */
bool au_net_allow_cert(AuNet *net, const char *host, const uint8_t sha256[32]);
void au_net_clear_cert_exceptions(AuNet *net);

#endif
