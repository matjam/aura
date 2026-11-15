/* The page interface: what the browser chrome needs from whatever shows a
 * web page in a tab. Portable C99.
 *
 * page_doc.c implements it on top of the document view (src/page): local
 * files and generated about: pages go through the real engine (parse,
 * style, layout, paint, hit testing); about:scene paints the paint test
 * scene directly; http: and https: use a fetcher registered for the
 * scheme (AuPageEnv.fetcher) and show a placeholder page while none is.
 *
 * Ownership and threading: one AuPage per tab, created and freed by the
 * browser (src/browser/browser.c). Everything runs on the event loop's
 * thread. The page never calls the OS: it gets fonts, preferences, the
 * clock, local file reading and fetchers through AuPageEnv, and reports
 * to the browser through AuPageCallbacks. A callback may be called from
 * inside any au_page_* function, including au_page_navigate.
 *
 * Loading is asynchronous: au_page_navigate starts a load and returns;
 * the bytes of files (and of every style sheet and image they use) are
 * read and delivered from au_page_idle. Generated about: pages load
 * inside au_page_navigate.
 *
 * Coordinates: document coordinates are CSS pixels from the top-left of
 * the document. The browser owns the scroll position: it passes it to
 * au_page_paint and au_page_set_scroll, and the page asks it to scroll
 * (to a fragment, or back to where a history entry was left) with the
 * scroll_to callback. Input events arrive in document coordinates.
 *
 * Navigation and the back/forward list belong to the browser: the page
 * never pushes history. When the user clicks a link the page asks the
 * browser with open_url; the browser records the navigation and calls
 * au_page_navigate. A redirect or a change of URL the page makes itself
 * is reported with url_changed. */
#ifndef AURA_BROWSER_PAGE_H
#define AURA_BROWSER_PAGE_H

#include "../dom/control.h" /* AuControlFile, AuPopupItem */
#include "../page/perf.h" /* AuPerf: counters only */
#include "../paint/font.h"
#include "../platform/platform.h" /* AuEvent: a plain struct, no OS calls */
#include "prefs.h"

typedef struct AuPage AuPage;

/* The security indicator at the left of the address field. */
typedef enum {
    AU_SECURITY_NONE,       /* nothing to say: about:, file:, empty */
    AU_SECURITY_SECURE,     /* HTTPS with a valid certificate */
    AU_SECURITY_NOT_SECURE, /* plain HTTP */
    AU_SECURITY_CERT_ERROR, /* HTTPS with a certificate problem (or one the user continued past) */
    AU_SECURITY_MIXED       /* HTTPS, but some of the page's resources were insecure or blocked as such */
} AuSecurity;

/* ---- Fetchers ----
 * A fetcher supplies the bytes of URLs of one scheme (a network stack
 * registers one for http and one for https). start() only begins a fetch
 * and returns a nonzero id (0: it cannot be fetched); the response head,
 * the bytes and the end arrive through the sink, from poll() or any later
 * call into the fetcher, never from inside start(). After done (or
 * cancel, or a download hand-off) the fetcher makes no more calls for
 * that id. poll() is called from every au_page_idle of a page with
 * fetches outstanding, so a fetcher shared by several pages is polled
 * more than once per event loop turn. */

/* What a request is for. */
typedef enum {
    AU_FETCH_DOCUMENT,   /* the page itself (a navigation) */
    AU_FETCH_STYLESHEET,
    AU_FETCH_IMAGE,
    AU_FETCH_FONT,
    AU_FETCH_SCRIPT,
    AU_FETCH_OTHER
} AuFetchKind;

/* The referrer policy (Referrer-Policy names, in src/net's order). */
typedef enum {
    AU_FETCH_RP_DEFAULT, /* strict-origin-when-cross-origin */
    AU_FETCH_RP_NO_REFERRER,
    AU_FETCH_RP_NO_REFERRER_WHEN_DOWNGRADE,
    AU_FETCH_RP_SAME_ORIGIN,
    AU_FETCH_RP_ORIGIN,
    AU_FETCH_RP_STRICT_ORIGIN,
    AU_FETCH_RP_ORIGIN_WHEN_CROSS_ORIGIN,
    AU_FETCH_RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN,
    AU_FETCH_RP_UNSAFE_URL
} AuFetchReferrerPolicy;

typedef enum {
    AU_FETCH_CACHE_NORMAL,     /* use the cache as HTTP says */
    AU_FETCH_CACHE_REVALIDATE, /* reload: revalidate the document */
    AU_FETCH_CACHE_BYPASS,     /* Shift- or Option-reload: fetch everything again */
    AU_FETCH_CACHE_HISTORY     /* Back or Forward: use cached copies as they are, stale or not */
} AuFetchCacheMode;

typedef struct AuFetchRequest {
    const char *url;
    AuFetchKind kind;
    /* The document making the request (referrer, mixed content, Sec-Fetch-*),
     * NULL for a navigation the user started (typed, bookmark, history). */
    const char *document_url;
    /* The top-level document (site for cookies, third-party decisions);
     * NULL when this request is the top-level navigation. */
    const char *top_level_url;
    AuFetchReferrerPolicy referrer_policy;
    AuFetchCacheMode cache;
    bool user_activated;      /* a click or the address field started it */
    bool allow_http_fallback; /* HTTPS-first: the user accepted plain http for this navigation */
    const char *method;       /* NULL: GET */
    const void *body;         /* copied; NULL for none */
    size_t body_len;
    const char *body_type;    /* the body's Content-Type */
} AuFetchRequest;

/* Why a fetch failed (src/net's errors, plus what the embedder adds). */
typedef enum {
    AU_FETCH_OK = 0,
    AU_FETCH_ERR_BAD_URL,
    AU_FETCH_ERR_SCHEME,
    AU_FETCH_ERR_DNS,
    AU_FETCH_ERR_CONNECT,
    AU_FETCH_ERR_TIMEOUT,
    AU_FETCH_ERR_RESET,
    AU_FETCH_ERR_PROTOCOL,
    AU_FETCH_ERR_TOO_LARGE,
    AU_FETCH_ERR_REDIRECTS,
    AU_FETCH_ERR_DECODE,
    AU_FETCH_ERR_TLS,
    AU_FETCH_ERR_CERT,
    AU_FETCH_ERR_CLOCK,
    AU_FETCH_ERR_PROXY,
    AU_FETCH_ERR_HTTPS_FALLBACK, /* HTTPS-first: https failed; the user may confirm http */
    AU_FETCH_ERR_BLOCKED,
    AU_FETCH_ERR_CANCELLED,
    AU_FETCH_ERR_NO_NETWORK      /* no TCP/IP: AuFetchInfo.message says why */
} AuFetchError;

/* Certificate details for the error page and the certificate viewer. */
#define AU_FETCH_CERT_CHAIN 8
typedef struct AuFetchCertEntry {
    char subject[192], issuer[192];
    int64_t not_before, not_after; /* seconds since 1970 UTC; 0 when unknown */
    uint8_t sha256[32];
    char san[256];                 /* DNS names, comma-separated */
} AuFetchCertEntry;

typedef struct AuFetchCert {
    char host[256];      /* the name checked */
    int count;           /* certificates the server sent (at most AU_FETCH_CERT_CHAIN kept) */
    AuFetchCertEntry chain[AU_FETCH_CERT_CHAIN];
    char reason[160];    /* English text of the problem; "" when valid */
    bool error;          /* the chain did not validate */
    bool clock_wrong;    /* the system date is before the root store's date */
    bool hsts;           /* an HSTS host: no exception may be made */
    bool overridden;     /* the user's session exception let it through */
    int64_t checked_at;  /* the time used for validation */
    char protocol[16];   /* "TLS 1.2" */
    char cipher[64];
} AuFetchCert;

/* The response head, before any data. Strings are valid during the call. */
typedef struct AuFetchResponse {
    int status;               /* 0 for data: URLs */
    const char *url;          /* the final URL after redirects */
    const char *content_type; /* computed (after sniffing); "" when unknown */
    const char *charset;      /* the Content-Type charset; "" when none */
    const char *encoding;     /* documents: the encoding the bytes are in ("UTF-8" after conversion) */
    int64_t content_length;   /* -1 when unknown */
    bool attachment;          /* Content-Disposition: attachment */
    const char *filename;     /* from Content-Disposition; "" when none */
    const char *csp;          /* Content-Security-Policy; "" when none */
    const char *referrer_policy; /* Referrer-Policy; "" when none */
    AuSecurity security;
    const AuFetchCert *cert;  /* https: the certificate details, or NULL */
    bool from_cache;
    bool no_store;            /* Cache-Control: no-store (a document with it is never kept for Back) */
} AuFetchResponse;

/* The end of a fetch. Strings and cert are valid during the call. */
typedef struct AuFetchInfo {
    const char *url;          /* the final URL after redirects, or NULL when unchanged */
    const char *content_type; /* "text/html", "text/css", ...; NULL when unknown */
    AuFetchError error;
    int status;               /* the HTTP status, 0 when none */
    const AuFetchCert *cert;  /* certificate details after a certificate error, or NULL */
    bool mixed;               /* a subresource blocked (or failed) as mixed content */
    const char *message;      /* more about the error ("" when nothing) */
    /* AU_FETCH_ERR_HTTPS_FALLBACK: why https failed. _CONNECT, _TIMEOUT
     * or _RESET: the site has no HTTPS; _TLS (message says why), _CERT
     * or _CLOCK (cert has the details): it has HTTPS, and that failed.
     * AU_FETCH_OK otherwise. */
    AuFetchError cause;
    /* How the request went, for the request log (netlog.h); NULL when
     * the fetcher does not say. Valid during the call. */
    const struct AuFetchNet *net;
} AuFetchInfo;

typedef struct AuFetchSink {
    void *ctx;
    /* The response head (before data). Optional. */
    void (*response)(void *ctx, uint32_t id, const AuFetchResponse *r);
    void (*data)(void *ctx, uint32_t id, const void *bytes, size_t len);
    /* ok is info->error == AU_FETCH_OK. info is NULL or describes the end. */
    void (*done)(void *ctx, uint32_t id, bool ok, const AuFetchInfo *info);
    /* A document response that should be saved rather than shown
     * (attachment, or a type Aura does not display). Return true to take
     * it: handoff then names the transfer for the download manager (see
     * AuDownload.handoff) and no more sink calls follow for id. Return
     * false to receive it as usual. Optional (false). */
    bool (*download)(void *ctx, uint32_t id, const AuFetchResponse *r, uint32_t handoff);
} AuFetchSink;

/* What a fetch is doing, for the status bar. */
typedef enum {
    AU_FETCH_PHASE_QUEUED,
    AU_FETCH_PHASE_ENTROPY,    /* https waits for the random source */
    AU_FETCH_PHASE_RESOLVING,
    AU_FETCH_PHASE_CONNECTING,
    AU_FETCH_PHASE_SECURING,
    AU_FETCH_PHASE_SENDING,
    AU_FETCH_PHASE_WAITING,
    AU_FETCH_PHASE_RECEIVING,
    AU_FETCH_PHASE_DONE
} AuFetchPhase;

/* Where a response came from. */
typedef enum {
    AU_FETCH_FROM_NETWORK,
    AU_FETCH_FROM_MEMORY,  /* the memory cache */
    AU_FETCH_FROM_DISK,    /* the disk cache */
    AU_FETCH_FROM_HISTORY, /* a Back or Forward load: the stored copy as it was */
    AU_FETCH_FROM_304      /* the stored copy, confirmed by the server */
} AuFetchSource;

/* How a finished request went (AuFetchInfo.net), for the developer
 * tools' request log. Strings are never NULL and are valid during the
 * done call. */
typedef struct AuFetchNet {
    /* Milliseconds from the request's start to the start of each phase;
     * -1 for a phase that did not happen. */
    int32_t at[AU_FETCH_PHASE_DONE + 1];
    AuFetchSource source;
    bool reused;              /* sent on a connection already open */
    uint32_t connection;      /* the connection's number; 0 when none was used */
    const char *remote;       /* "93.184.215.14:443"; "" when unknown */
    const char *protocol;     /* "TLS 1.2"; "" without TLS */
    const char *cipher;
    int64_t transfer_bytes;   /* body bytes as sent; 0 from a cache */
    int redirects;
    const char *request_head;  /* the request line and headers; "" when not recorded */
    const char *response_head; /* "name: value" lines; "" when not recorded */
    const char *blocked_reason; /* what refused the request; "" when nothing did */
} AuFetchNet;

typedef struct AuFetchStatus {
    AuFetchPhase phase;
    int64_t received, total;   /* body bytes; total -1 when unknown */
    unsigned entropy_bits, entropy_needed; /* the random source's progress (0, 0 when not known) */
} AuFetchStatus;

typedef struct AuPageFetcher {
    void *ctx;
    uint32_t (*start)(void *ctx, const AuFetchRequest *rq, const AuFetchSink *sink);
    void (*cancel)(void *ctx, uint32_t id);
    void (*poll)(void *ctx);
    /* Optional: what fetch id is doing. False when the id is unknown. */
    bool (*status)(void *ctx, uint32_t id, AuFetchStatus *out);
    /* Optional: the user chose to continue past a certificate error on
     * host, for the certificate with this fingerprint, for this session.
     * False when refused (an HSTS host). */
    bool (*allow_cert)(void *ctx, const char *host, const uint8_t sha256[32]);
} AuPageFetcher;

typedef struct AuPageEnv {
    AuFontSet *fonts;
    const AuPrefs *prefs; /* read when needed; may change between calls */
    void *ctx;
    /* Milliseconds from an arbitrary start (wrapping), for timers. */
    uint32_t (*time_ms)(void *ctx);
    /* Reads the file a file: URL names, into memory from au_alloc.
     * Returns false when it cannot. */
    bool (*read_file_url)(void *ctx, const char *url, char **data, size_t *len);
    /* The fetcher for a URL scheme (lower case, without ':'), or NULL.
     * Optional. file: and about: are built in. */
    const AuPageFetcher *(*fetcher)(void *ctx, const char *scheme);
    /* The selection highlight colour (paint ARGB); 0 for a default. */
    uint32_t highlight;
} AuPageEnv;

typedef struct AuPageCallbacks {
    void *ctx;
    void (*title_changed)(void *ctx, AuPage *p, const char *title);
    void (*url_changed)(void *ctx, AuPage *p, const char *url);
    void (*load_started)(void *ctx, AuPage *p);
    /* Progress changed (au_page_progress); for the status bar. */
    void (*load_progress)(void *ctx, AuPage *p);
    void (*load_finished)(void *ctx, AuPage *p, bool ok);
    /* The user activated a link: navigate this tab, or open a new tab
     * when new_tab is set (Command-click, target=_blank). */
    void (*open_url)(void *ctx, AuPage *p, const char *url, bool new_tab);
    /* Text for the status bar (a hovered link's URL); "" clears it. */
    void (*status_text)(void *ctx, AuPage *p, const char *text);
    /* Part of the document needs repainting (document coordinates). */
    void (*invalidate)(void *ctx, AuPage *p, AuIRect r);
    /* au_page_content_size changed (scroll bar range). */
    void (*content_size_changed)(void *ctx, AuPage *p);
    /* The page wants the viewport scrolled to (x, y): a fragment, or the
     * position a history entry or a reload restores. The browser clamps
     * it and reports the result with au_page_set_scroll. */
    void (*scroll_to)(void *ctx, AuPage *p, int x, int y);
    /* The document being loaded is a file to save, not a page to show
     * (attachment, or a type Aura does not display): r describes it and
     * handoff names its transfer (AuDownload.handoff). Return true when the
     * browser took it; the page then goes back to the document it showed
     * before (url_changed reports the old URL). Optional: without it, or
     * when it returns false, the page shows that it cannot display the
     * file. */
    bool (*download)(void *ctx, AuPage *p, const AuFetchResponse *r, uint32_t handoff);
    /* Forms (all optional). A form was submitted with POST: load url with
     * the body (of Content-Type type) as a new entry with
     * au_page_navigate_post, or in a new tab. Without it the page loads
     * it itself. GET submissions arrive through open_url. */
    void (*submit_post)(void *ctx, AuPage *p, const char *url, const void *body, size_t len, const char *type,
                        bool new_tab);
    /* A select's pop-up menu at document rectangle at: returns the item
     * chosen, -1 for none, or -2 when there is no menu (a click then
     * steps through the options, as without the callback). */
    int (*popup)(void *ctx, AuPage *p, const AuPopupItem *items, int n, int selected, AuIRect at);
    /* Files for an input type=file (see AuDocViewCallbacks.choose_file). */
    bool (*choose_file)(void *ctx, AuPage *p, bool multiple, AuControlFile **files, uint32_t *n);
    /* Yes or no (sending a form from a secure page over plain http).
     * Without it the answer is yes. */
    bool (*confirm)(void *ctx, AuPage *p, const char *message);
} AuPageCallbacks;

/* Creates a page showing nothing (about:blank). env and cb are copied. */
AuPage *au_page_new(const AuPageEnv *env, const AuPageCallbacks *cb);
void au_page_free(AuPage *p);

/* Starts loading url (already resolved by the browser). A URL that
 * differs from the current one only by a fragment, and has one, scrolls
 * to the fragment in the loaded document instead of loading it again. */
void au_page_navigate(AuPage *p, const char *url);
/* Loads url for a back/forward entry, scrolling to (scroll_x, scroll_y)
 * once the document is tall enough (at the latest when it has loaded).
 * When only the fragment differs from the current URL the document is
 * kept and the position applied at once. */
void au_page_navigate_entry(AuPage *p, const char *url, int scroll_x, int scroll_y);
void au_page_stop(AuPage *p);
/* Loads the current URL again, keeping the scroll position. The document
 * is revalidated with the server; with bypass_cache (Shift- or
 * Option-reload) the document and every resource are fetched again. */
void au_page_reload(AuPage *p);
void au_page_reload_bypass(AuPage *p);
/* Shows url (a view-source: URL) as the source text data, without
 * fetching anything: View Source of a network page uses the bytes kept
 * for Save Page As. */
void au_page_navigate_source(AuPage *p, const char *url, const char *data, size_t len);
/* Shows generated HTML (an internal page such as about:certificate) as
 * url, without fetching anything. */
void au_page_load_internal(AuPage *p, const char *url, const char *html, size_t len);

/* ---- The document cache ----
 * A page keeps the documents it leaves, so that Back and Forward show
 * them again at once: the document view with its tree, layout, images,
 * script runtime and form contents. The browser names each back/forward
 * entry with a nonzero id: au_page_set_entry before a navigation says
 * which entry the load is for (a load without it belongs to no entry and
 * its document is never kept; a reload stays with the entry).
 * au_page_navigate_entry for an entry whose document is kept shows that
 * document without loading: load_started, url_changed, title_changed,
 * content_size_changed, scroll_to and load_finished are still called,
 * and AuPageStats.restored is set.
 *
 * A document is kept when the tab leaves it for another entry, unless it
 * is still loading or has fetches outstanding, failed, is an internal
 * page, was the result of a POST, came with Cache-Control: no-store, or
 * has an unload or beforeunload listener. A kept document does nothing:
 * its timers, animation frames and image animations wait, and it gets
 * pagehide and pageshow with persisted true. A reload or a fresh load of
 * its entry discards it.
 *
 * Limits: at most prefs.document_cache_pages documents per page, the
 * oldest dropped first, and none larger than prefs.document_cache_kb.
 * The browser keeps the total of all pages under that figure with
 * au_page_kept_bytes, au_page_kept_oldest and au_page_evict_oldest. */
void au_page_set_entry(AuPage *p, uint32_t entry);
/* Drops the kept documents whose entry is not among the n ids (the
 * browser's back/forward list lost entries). */
void au_page_retain_entries(AuPage *p, const uint32_t *ids, size_t n);
size_t au_page_kept_count(const AuPage *p);
size_t au_page_kept_bytes(const AuPage *p); /* estimated */
/* When the oldest kept document was hidden (the page's clock); false
 * when the page keeps none. */
bool au_page_kept_oldest(const AuPage *p, uint32_t *stamp);
bool au_page_evict_oldest(AuPage *p);
/* Drops every kept document; returns how many. */
size_t au_page_drop_kept(AuPage *p);

const char *au_page_url(const AuPage *p);   /* never NULL */
const char *au_page_title(const AuPage *p); /* "" when the page has none */
bool au_page_loading(const AuPage *p);
float au_page_progress(const AuPage *p);    /* 0..1 while loading */
AuSecurity au_page_security(const AuPage *p);
/* The main document's certificate (https pages and certificate error
 * pages), or NULL. */
const AuFetchCert *au_page_cert(const AuPage *p);
/* Appends the certificate viewer's HTML for the page's certificate (its
 * chain, validity, fingerprints, protocol and cipher) to out; false when
 * the page has none. */
bool au_page_certificate_html(const AuPage *p, AuBuf *out);
/* What the load is doing, for the status bar: "Looking up example.com",
 * "Waiting for example.com", "Loading 3 of 12 resources"...; "" when
 * not loading. */
void au_page_status_text(const AuPage *p, char *out, size_t cap);
/* The main document's bytes as they arrived (for Save Page As): false
 * while it is loading, after a failed or stopped load, and for pages
 * that have none (about:scene). The bytes stay valid until the next
 * load or au_page_free. */
bool au_page_source(const AuPage *p, const char **data, size_t *len);
/* The page's script console: its last AU_PAGE_CONSOLE_LINES lines across
 * loads, oldest first (i from 0 to count - 1). level: 0 log, 1 info,
 * 2 warn, 3 error, 4 debug. A line stays valid until the next console
 * line or au_page_free. JavaScript runs when the javascript preference is
 * on (the default), from the next load. */
#define AU_PAGE_CONSOLE_LINES 300
size_t au_page_console_count(const AuPage *p);
const char *au_page_console_line(const AuPage *p, size_t i, int *level);

/* The viewport size in CSS pixels; layout wraps to its width. */
void au_page_set_viewport(AuPage *p, int width, int height);
void au_page_content_size(const AuPage *p, int *width, int *height);
/* The browser's scroll position for this page (after it changed). */
void au_page_set_scroll(AuPage *p, int x, int y);
/* Font preferences or other settings in env.prefs changed: restyle. */
void au_page_prefs_changed(AuPage *p);
/* Text zoom: font sizes are multiplied by zoom (clamped to the range
 * below); other lengths are not. It applies to this and later documents
 * of the page until changed. */
#define AU_PAGE_ZOOM_MIN 0.5f
#define AU_PAGE_ZOOM_MAX 3.0f
void au_page_set_zoom(AuPage *p, float zoom);
float au_page_zoom(const AuPage *p);

/* Paints the document into the canvas rectangle view (canvas
 * coordinates), with the document point (scroll_x, scroll_y) at view's
 * top-left. Paints every pixel of view (and nothing outside it). */
void au_page_paint(AuPage *p, AuCanvas *c, AuRect view, int scroll_x, int scroll_y);
/* Scrolling by moving pixels: the browser scrolled the page from (ox, oy)
 * to (nx, ny) and moves the pixels it painted by the difference. Writes
 * the areas (page pixels, at the new position) that are then wrong and
 * must be painted again besides the exposed strips: where viewport-fixed
 * and sticky boxes were moved to and where they are now, at most max (32
 * at most). Returns how many, or -1 when the pixels cannot be moved
 * (layout out of date, a fixed canvas background, a page that is not a
 * document): the browser then paints the whole page. */
#define AU_PAGE_SCROLL_DAMAGE 32
int au_page_scroll_damage(AuPage *p, int ox, int oy, int nx, int ny, AuIRect *out, int max);

/* Input in document coordinates. Return true when the page used it. */
bool au_page_mouse(AuPage *p, const AuEvent *ev);
bool au_page_key(AuPage *p, const AuEvent *ev);
/* ---- Forms ----
 * Clicks and keys reach form controls through au_page_mouse and
 * au_page_key. au_page_editing is true while a text field has the focus
 * (keys then belong to the page, and the Edit menu acts on the field).
 * au_page_edit runs an AU_EDITCMD_* command (src/page/page.h numbers:
 * undo, cut, copy, paste, clear, select all) on that field: Cut and Copy
 * append the selected text (UTF-8) to out, Paste inserts text. */
enum { AU_PAGE_EDIT_UNDO, AU_PAGE_EDIT_CUT, AU_PAGE_EDIT_COPY, AU_PAGE_EDIT_PASTE, AU_PAGE_EDIT_CLEAR,
       AU_PAGE_EDIT_SELECT_ALL };
bool au_page_editing(const AuPage *p);
bool au_page_can_edit(const AuPage *p, int cmd);
bool au_page_edit(AuPage *p, int cmd, const char *text, size_t len, AuBuf *out);
/* Document point (x, y) is over a text field (for the I-beam cursor). */
bool au_page_text_field_at(AuPage *p, int x, int y);
/* The document rectangle of the first element with a box whose name
 * attribute is name (for scripted input in tests). */
bool au_page_control_rect(AuPage *p, const char *name, AuIRect *r);
/* Takes the focus away from the page's focused element (the address
 * field took the keyboard). */
void au_page_blur(AuPage *p);
/* Loads url with a POST body (a form submission). */
void au_page_navigate_post(AuPage *p, const char *url, const void *body, size_t len, const char *type);
/* The current document is the result of a POST (a reload sends it again). */
bool au_page_is_post(const AuPage *p);
/* What is at document point (x, y), for a contextual menu: appends the
 * absolute URL of the link under it to link and of the image under it to
 * image (nothing when there is none; either may be NULL). */
void au_page_hit_info(AuPage *p, int x, int y, AuBuf *link, AuBuf *image);
/* ---- Text selection and find ----
 * The user selects text with the mouse through au_page_mouse: dragging
 * selects characters, a double click a word, a triple click a paragraph
 * (dragging after them extends by words or paragraphs), Shift-click
 * extends the selection. A drag that selects does not activate the link
 * it started on. A new document clears the selection. */
bool au_page_has_selection(const AuPage *p);
/* Appends the selected text (UTF-8, line breaks between blocks) to out;
 * false when nothing is selected. */
bool au_page_selection_text(AuPage *p, AuBuf *out);
void au_page_select_all(AuPage *p);
void au_page_clear_selection(AuPage *p);
/* Finds text in the page, forward from the selection's end or backward
 * from its start (the whole page without one), wrapping around; selects
 * the match and scrolls it into view. Case-insensitive unless
 * case_sensitive. False when there is no match (the selection stays). */
bool au_page_find(AuPage *p, const char *text, bool forward, bool case_sensitive);

/* Scrolls an inner scrolling box under document point (x, y) by (dx,
 * dy). Returns false when none can move: the browser then scrolls the
 * viewport. */
bool au_page_scroll(AuPage *p, int x, int y, int dx, int dy);

/* Called from the event loop's idle time; runs timers and loading. Each
 * call does a bounded slice of work (a few tens of milliseconds) so the
 * interface stays responsive while a large page loads. */
void au_page_idle(AuPage *p);
/* Whether the page wants au_page_idle called soon (loading, animating);
 * the shell can sleep longer when no page does. */
bool au_page_busy(const AuPage *p);

/* ---- Developer tools ----
 * The request log (netlog.h): one record per request since the last
 * navigation. The time counters (src/page/perf.h): script, style, layout
 * and paint, to which the shell adds its screen copies. */
struct AuNetLog;
const struct AuNetLog *au_page_netlog(const AuPage *p);
AuPerf *au_page_perf(AuPage *p);
/* Bytes the page's script runtime has allocated, and its limit (0, 0
 * when it has none). */
void au_page_script_memory(const AuPage *p, size_t *used, size_t *limit);
/* The console: lines ever added (a view compares it to see new lines or
 * a Clear), a line added by a tool (levels 5, the expression typed, and
 * 6, its result, besides the script levels), and Clear. */
#define AU_CONSOLE_INPUT 5
#define AU_CONSOLE_RESULT 6
uint32_t au_page_console_total(const AuPage *p);
void au_page_console_add(AuPage *p, int level, const char *text);
void au_page_console_clear(AuPage *p);
/* Evaluates src as script in the page; the result as a string, or the
 * error, goes in out. False when it threw or the page runs no script. */
bool au_page_eval(AuPage *p, const char *src, AuBuf *out);

/* ---- Measurements (Debug menu) ---- */
typedef struct AuPageStats {
    uint32_t first_paint_ms; /* from navigate to the first paint with content; 0 before it */
    uint32_t load_ms;        /* from navigate to the end of loading; 0 while loading */
    uint32_t layout_ms;      /* the last style and layout pass */
    uint32_t layout_total_ms; /* every style and layout pass of this load */
    uint32_t layouts;        /* passes in this load */
    uint32_t paint_ms;       /* the last au_page_paint */
    uint32_t bytes;          /* document bytes received */
    uint32_t resources;      /* style sheets and images requested */
    uint32_t boxes;          /* layout boxes */
    uint32_t image_decodes, image_evictions; /* the image cache's, since the load */
    uint32_t image_kb, image_budget_kb;      /* decoded pixels held, and the budget */
    uint32_t restored;       /* times this document was shown again from the document cache; load_ms and
                              * first_paint_ms then time the last showing */
    uint32_t hide_ms;        /* of load_ms after a restore: hiding (or freeing) the document that was left */
    bool document_cached;    /* the main document's bytes came from the HTTP cache without a request */
} AuPageStats;
void au_page_stats(const AuPage *p, AuPageStats *s);

#endif
