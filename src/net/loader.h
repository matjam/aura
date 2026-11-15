/* The resource loader: what the browser asks for URLs, on top of AuNet.
 *
 * A loader serves http:, https: and data: URLs. Over the network stack
 * (whose cookie jar, HSTS store and cache apply to every request) it
 * adds, per request:
 *   - mixed content: from an https document, http images are upgraded to
 *     https and other http subresources are blocked (AU_NET_ERR_BLOCKED);
 *   - MIME sniffing and X-Content-Type-Options: nosniff (a style sheet or
 *     script of the wrong type under nosniff is blocked);
 *   - for HTML documents, optional transcoding to UTF-8: the encoding is
 *     detected from the BOM, the Content-Type charset and the <meta>
 *     prescan (au_detect_encoding); UTF-8 and windows-1252 documents are
 *     delivered as UTF-8 with a BOM, which the HTML parser obeys over any
 *     <meta>. Documents in other encodings pass through unchanged and
 *     AuLoadInfo.encoding names them.
 *   - the security state of documents, the certificate details on
 *     errors, progress, and downloads: a document response that should be
 *     saved rather than shown (Content-Disposition: attachment, or a type
 *     Aura does not display) is offered to callbacks.download, and the
 *     embedder may take it over with au_net_download_adopt.
 *
 * Everything is delivered from au_net_loader_poll (which also pumps the
 * AuNet), never from inside au_net_loader_start. One loader per AuNet.
 *
 * page_fetcher.c wraps the loader as the browser's AuPageFetcher
 * (page.h) for http and https. Each request carries its context (kind,
 * requesting and top-level document, referrer policy, reload mode), which
 * maps onto AuLoadRequest. */
#ifndef AURA_NET_LOADER_H
#define AURA_NET_LOADER_H

#include "net.h"
#include "tls.h"

typedef struct AuNetLoader AuNetLoader;
typedef struct AuNetDownload AuNetDownload;

typedef enum {
    AU_LOAD_SECURITY_NONE,       /* data: URLs, failures before a response */
    AU_LOAD_SECURITY_SECURE,     /* https, valid certificate */
    AU_LOAD_SECURITY_NOT_SECURE, /* http */
    AU_LOAD_SECURITY_CERT_ERROR  /* https certificate problem (or the clock is wrong) */
} AuLoadSecurity;

typedef struct AuLoadRequest {
    const char *url;
    AuRequestDest dest;           /* AU_DEST_DOCUMENT for pages */
    const char *document_url;     /* the requesting document; NULL for a navigation by the user */
    const char *top_level_url;    /* NULL when this is the top-level navigation */
    AuReferrerPolicy referrer_policy;
    bool user_activated;
    bool reload;                  /* bypass the cache (end-to-end reload) */
    bool revalidate;              /* a reload: revalidate a cached copy (Cache-Control: max-age=0) */
    bool history;                 /* Back or Forward: use any cached copy as is (AuHttpRequest.history) */
    bool allow_http_fallback;     /* HTTPS-first: the user accepted plain http */
    const char *method;           /* NULL: GET */
    AuBodySource *body;           /* ownership passes */
    const char *content_type;     /* of the body */
    bool to_utf8;                 /* transcode HTML documents (see above) */
    bool omit_credentials;        /* no cookies, no HTTP authentication (AuHttpRequest.omit_credentials) */
} AuLoadRequest;

typedef struct AuLoadInfo {
    const char *url;          /* final URL */
    int status;               /* 0 for data: URLs and failures */
    const char *mime;         /* computed MIME type (after sniffing) */
    const char *charset;      /* Content-Type charset, "" when none */
    const char *encoding;     /* documents: the detected encoding; "" otherwise */
    int64_t content_length;   /* -1 when unknown */
    AuLoadSecurity security;
    const AuCertInfo *cert;   /* certificate details (https), or NULL */
    bool from_cache;
    bool no_store;            /* Cache-Control: no-store on the response */
    bool attachment;          /* Content-Disposition: attachment */
    const char *filename;     /* suggested file name for a download ("" when none) */
    const char *csp;          /* Content-Security-Policy ("" when none) */
    const char *x_frame_options; /* X-Frame-Options ("" when none) */
    const char *referrer_policy; /* Referrer-Policy ("" when none) */
    uint16_t tls_version;     /* 0x0303 for TLS 1.2; 0 without TLS */
    const char *cipher;       /* the TLS cipher suite ("" without TLS) */
    bool mixed;               /* blocked as mixed content (err AU_NET_ERR_BLOCKED) */
    /* AU_NET_ERR_HTTPS_FALLBACK: why https failed (au_http_fallback_cause);
     * with _CERT or _CLOCK, cert holds the details, and with _TLS its
     * reason. AU_NET_OK otherwise. */
    AuNetError fallback_cause;
    /* For the developer tools' request log, in done() only: how the
     * transfer went (au_http_trace), the response headers and the request
     * as sent ("" unless au_net_set_trace_headers). trace and
     * response_headers are NULL when no transfer was made (data: URLs,
     * requests refused before one). */
    const AuHttpTrace *trace;
    const AuHeaders *response_headers;
    const char *request_head;
    /* Refused by a blocklist (err AU_NET_ERR_BLOCKED, no connection was
     * made): "blocked: LIST NAME". NULL otherwise. (Mixed content sets
     * mixed; the request log shows either as the reason.) */
    const char *blocked_reason;
} AuLoadInfo;

typedef struct AuLoadCallbacks {
    void *ctx;
    /* The response is known (before any data). Optional. */
    void (*response)(void *ctx, uint32_t id, const AuLoadInfo *info);
    void (*data)(void *ctx, uint32_t id, const void *bytes, size_t len);
    /* The end. info is valid during the call. */
    void (*done)(void *ctx, uint32_t id, AuNetError err, const AuLoadInfo *info);
    /* A document that should be saved rather than shown. Return true to
     * take it (call au_net_download_adopt now; no further callbacks for
     * id), false to receive it as usual. Optional (false). */
    bool (*download)(void *ctx, uint32_t id, const AuLoadInfo *info);
} AuLoadCallbacks;

AuNetLoader *au_net_loader_new(AuNet *net);
void au_net_loader_free(AuNetLoader *l); /* cancels everything */
AuNet *au_net_loader_net(AuNetLoader *l);
/* The blocklists requests are checked against before anything else
 * (blocklist.h); NULL for none. The loader does not own it. */
struct AuBlocklist;
void au_net_loader_set_blocklist(AuNetLoader *l, struct AuBlocklist *bl);
struct AuBlocklist *au_net_loader_blocklist(AuNetLoader *l);
/* Returns a nonzero id; failures are reported through done(). */
uint32_t au_net_loader_start(AuNetLoader *l, const AuLoadRequest *rq, const AuLoadCallbacks *cb);
void au_net_loader_cancel(AuNetLoader *l, uint32_t id);
/* Pumps the network and delivers. Returns true if anything happened. */
bool au_net_loader_poll(AuNetLoader *l);
int au_net_loader_active(const AuNetLoader *l);
/* Body bytes received so far and the expected total (-1 when unknown). */
bool au_net_loader_progress(const AuNetLoader *l, uint32_t id, int64_t *received, int64_t *total);
/* What request id is doing (AU_PHASE_DONE once its response is complete). */
bool au_net_loader_phase(const AuNetLoader *l, uint32_t id, AuHttpPhase *phase);

/* ---- Downloads ----
 * Shaped for the browser's AuTransferSource (downloads.h): start, poll,
 * stop. The bytes go to the sink (the platform writes the file). */
typedef struct AuDownloadSink {
    void *ctx;
    /* Returns false when the bytes cannot be written (the download fails). */
    bool (*write)(void *ctx, const void *bytes, size_t len);
} AuDownloadSink;

/* Starts a download of url from byte offset (a Range request when > 0).
 * Returns NULL and an au_alloc'd message in *error on failure. */
AuNetDownload *au_net_download_start(AuNetLoader *l, const char *url, int64_t offset, const AuDownloadSink *sink,
                                     char **error);
/* Takes over the loader request id (from callbacks.download): the bytes
 * so far and the rest go to the sink. */
AuNetDownload *au_net_download_adopt(AuNetLoader *l, uint32_t id, const AuDownloadSink *sink);
/* Bytes received in all (including the offset). Sets *total when known,
 * *done at the end, or *error (au_alloc'd) on failure. Pumps nothing:
 * call au_net_loader_poll from the event loop. */
int64_t au_net_download_poll(AuNetDownload *d, int64_t *total, bool *done, char **error);
void au_net_download_stop(AuNetDownload *d);

/* ---- The browser's fetcher interface (page_fetcher.c) ---- */
typedef struct AuNetPageFetcher AuNetPageFetcher;
struct AuPageFetcher;
struct AuFetchCert;
/* An adapter implementing page.h's AuPageFetcher on the loader. Fill an
 * AuPageFetcher with au_net_page_fetcher_interface and register it for
 * "http" and "https" with au_browser_set_fetcher (which copies it). */
AuNetPageFetcher *au_net_page_fetcher_new(AuNetLoader *l);
void au_net_page_fetcher_free(AuNetPageFetcher *f);
void au_net_page_fetcher_interface(AuNetPageFetcher *f, struct AuPageFetcher *out);
AuNetLoader *au_net_page_fetcher_loader(AuNetPageFetcher *f);
/* The platform's entropy source, for AuFetchStatus while https waits for
 * it: bits() returns the bits credited so far, needed the threshold. */
void au_net_page_fetcher_set_entropy(AuNetPageFetcher *f, unsigned (*bits)(void *ctx), void *ctx, unsigned needed);
/* Optional: why() returns a sentence when the machine has no usable
 * TCP/IP (Open Transport missing or not configured), else NULL; a DNS or
 * connection failure is then reported as AU_FETCH_ERR_NO_NETWORK. */
void au_net_page_fetcher_set_offline(AuNetPageFetcher *f, const char *(*why)(void *ctx), void *ctx);
/* A document response a page handed to the download manager (sink
 * download(), its handoff token): attaches the file's sink, writes the
 * bytes held so far, and returns the transfer (poll it with
 * au_net_download_poll); NULL when the token is unknown (the caller then
 * starts the download afresh). Release it with
 * au_net_page_fetcher_release_download (not au_net_download_stop). */
AuNetDownload *au_net_page_fetcher_take_download(AuNetPageFetcher *f, uint32_t token, const AuDownloadSink *sink,
                                                 char **error);
void au_net_page_fetcher_release_download(AuNetPageFetcher *f, uint32_t token);
/* ---- Developer tools (net_report.c) ----
 * The open connections as text, grouped by host: "example.com:443 x2
 * (1 busy), cdn.example.com:443 x1"; *total and *busy count them. */
void au_net_connections_text(const AuNet *net, AuBuf *out, int *total, int *busy);
/* Fills the network part of the Performance report's AuDevHostInfo
 * (src/browser/devtools.h): the HTTP cache's size and counts and the
 * connections; conn_text holds the string h then points to. */
struct AuDevHostInfo;
void au_net_dev_info(AuNet *net, struct AuDevHostInfo *h, AuBuf *conn_text);

/* Converts certificate details for page.h. */
void au_net_fetch_cert_from(struct AuFetchCert *out, const AuCertInfo *ci, uint16_t version, const char *cipher);

#endif
