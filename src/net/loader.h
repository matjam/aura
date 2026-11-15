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
 * au_net_loader_page_fetcher wraps the loader as the browser's
 * AuPageFetcher (page.h) for http and https. That interface carries no
 * request context (which document asked, what kind of resource), so the
 * adapter infers it: a request that starts when the adapter has nothing
 * outstanding and has been quiet for AU_FETCHER_QUIET_MS is a top-level
 * navigation by the user; requests while a navigation is active, or soon
 * after, are its subresources (the navigation's final URL is their
 * referrer and top-level site, and their kind is guessed from the URL's
 * extension). With one page loading at a time this is exact; a second
 * page loading at the same moment is treated as a subresource of the
 * first, which errs on the side of withholding cookies. */
#ifndef AURA_NET_LOADER_H
#define AURA_NET_LOADER_H

#include "net.h"
#include "tls.h"

typedef struct AuNetLoader AuNetLoader;
typedef struct AuNetDownload AuNetDownload;

#define AU_FETCHER_QUIET_MS 1500

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
    bool reload;
    bool allow_http_fallback;     /* HTTPS-first: the user accepted plain http */
    const char *method;           /* NULL: GET */
    AuBodySource *body;           /* ownership passes */
    const char *content_type;     /* of the body */
    bool to_utf8;                 /* transcode HTML documents (see above) */
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
    bool attachment;          /* Content-Disposition: attachment */
    const char *filename;     /* suggested file name for a download ("" when none) */
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
/* Returns a nonzero id; failures are reported through done(). */
uint32_t au_net_loader_start(AuNetLoader *l, const AuLoadRequest *rq, const AuLoadCallbacks *cb);
void au_net_loader_cancel(AuNetLoader *l, uint32_t id);
/* Pumps the network and delivers. Returns true if anything happened. */
bool au_net_loader_poll(AuNetLoader *l);
int au_net_loader_active(const AuNetLoader *l);
/* Body bytes received so far and the expected total (-1 when unknown). */
bool au_net_loader_progress(const AuNetLoader *l, uint32_t id, int64_t *received, int64_t *total);

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
/* An adapter implementing page.h's AuPageFetcher on the loader. Fill an
 * AuPageFetcher with au_net_page_fetcher_interface and register it for
 * "http" and "https" with au_browser_set_fetcher (which copies it). */
AuNetPageFetcher *au_net_page_fetcher_new(AuNetLoader *l);
void au_net_page_fetcher_free(AuNetPageFetcher *f);
void au_net_page_fetcher_interface(AuNetPageFetcher *f, struct AuPageFetcher *out);
/* The security state the adapter saw for the most recent top-level
 * navigation to url (for the address field's indicator), and its
 * certificate details (NULL when none). */
AuLoadSecurity au_net_page_fetcher_security(const AuNetPageFetcher *f, const char *url, const AuCertInfo **cert);

#endif
