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

#include "../paint/font.h"
#include "../platform/platform.h" /* AuEvent: a plain struct, no OS calls */
#include "prefs.h"

typedef struct AuPage AuPage;

/* The security indicator at the left of the address field. */
typedef enum {
    AU_SECURITY_NONE,       /* nothing to say: about:, file:, empty */
    AU_SECURITY_SECURE,     /* HTTPS with a valid certificate */
    AU_SECURITY_NOT_SECURE, /* plain HTTP */
    AU_SECURITY_CERT_ERROR  /* HTTPS with a certificate problem */
} AuSecurity;

/* ---- Fetchers ----
 * A fetcher supplies the bytes of URLs of one scheme (a network stack
 * registers one for http and one for https). start() only begins a fetch
 * and returns a nonzero id (0: it cannot be fetched); the bytes and the
 * end arrive through the sink, from poll() or any later call into the
 * fetcher, never from inside start(). After done (or cancel) the fetcher
 * makes no more calls for that id. poll() is called from every
 * au_page_idle of a page with fetches outstanding, so a fetcher shared by
 * several pages is polled more than once per event loop turn. */
typedef struct AuFetchInfo {
    const char *url;          /* the final URL after redirects, or NULL when unchanged */
    const char *content_type; /* "text/html", "text/css", ...; NULL when unknown */
} AuFetchInfo;

typedef struct AuFetchSink {
    void *ctx;
    void (*data)(void *ctx, uint32_t id, const void *bytes, size_t len);
    /* info is NULL or describes the response (it may come before data). */
    void (*done)(void *ctx, uint32_t id, bool ok, const AuFetchInfo *info);
} AuFetchSink;

typedef struct AuPageFetcher {
    void *ctx;
    uint32_t (*start)(void *ctx, const char *url, const AuFetchSink *sink);
    void (*cancel)(void *ctx, uint32_t id);
    void (*poll)(void *ctx);
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
/* Loads the current URL again, keeping the scroll position. */
void au_page_reload(AuPage *p);

const char *au_page_url(const AuPage *p);   /* never NULL */
const char *au_page_title(const AuPage *p); /* "" when the page has none */
bool au_page_loading(const AuPage *p);
float au_page_progress(const AuPage *p);    /* 0..1 while loading */
AuSecurity au_page_security(const AuPage *p);
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

/* Input in document coordinates. Return true when the page used it. */
bool au_page_mouse(AuPage *p, const AuEvent *ev);
bool au_page_key(AuPage *p, const AuEvent *ev);
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
} AuPageStats;
void au_page_stats(const AuPage *p, AuPageStats *s);

#endif
