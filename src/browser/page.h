/* The page interface: what the browser chrome needs from whatever shows a
 * web page in a tab. Portable C99.
 *
 * Today page_stub.c implements it (about: pages, the paint test scene,
 * local HTML files shown as plain text, and a placeholder for network
 * URLs that simulates loading). The real engine (fetch, parse, style,
 * layout, paint, script) will implement the same functions and replace
 * page_stub.c; nothing else in src/browser or the platform shells needs
 * to change. Keep this interface narrow.
 *
 * Ownership and threading: one AuPage per tab, created and freed by the
 * browser (src/browser/browser.c). Everything runs on the event loop's
 * thread. The page never calls the OS: it gets fonts, preferences, the
 * clock and local file reading through AuPageEnv, and reports to the
 * browser through AuPageCallbacks. A callback may be called from inside
 * any au_page_* function, including au_page_navigate.
 *
 * Coordinates: document coordinates are CSS pixels from the top-left of
 * the document. The browser owns the scroll position and passes it to
 * au_page_paint; input events arrive in document coordinates.
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

typedef struct AuPageEnv {
    AuFontSet *fonts;
    const AuPrefs *prefs; /* read when needed; may change between calls */
    void *ctx;
    /* Milliseconds from an arbitrary start (wrapping), for timers. */
    uint32_t (*time_ms)(void *ctx);
    /* Reads the file a file: URL names, into memory from au_alloc.
     * Returns false when it cannot. */
    bool (*read_file_url)(void *ctx, const char *url, char **data, size_t *len);
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
} AuPageCallbacks;

/* Creates a page showing nothing (about:blank). env and cb are copied. */
AuPage *au_page_new(const AuPageEnv *env, const AuPageCallbacks *cb);
void au_page_free(AuPage *p);

/* Starts loading url (already resolved by the browser). */
void au_page_navigate(AuPage *p, const char *url);
void au_page_stop(AuPage *p);
void au_page_reload(AuPage *p);

const char *au_page_url(const AuPage *p);   /* never NULL */
const char *au_page_title(const AuPage *p); /* "" when the page has none */
bool au_page_loading(const AuPage *p);
float au_page_progress(const AuPage *p);    /* 0..1 while loading */
AuSecurity au_page_security(const AuPage *p);

/* The viewport size in CSS pixels; layout wraps to its width. */
void au_page_set_viewport(AuPage *p, int width, int height);
void au_page_content_size(const AuPage *p, int *width, int *height);

/* Paints the document into the canvas rectangle view (canvas
 * coordinates), with the document point (scroll_x, scroll_y) at view's
 * top-left. Paints every pixel of view (and nothing outside it). */
void au_page_paint(AuPage *p, AuCanvas *c, AuRect view, int scroll_x, int scroll_y);

/* Input in document coordinates. Return true when the page used it. */
bool au_page_mouse(AuPage *p, const AuEvent *ev);
bool au_page_key(AuPage *p, const AuEvent *ev);

/* Called from the event loop's idle time; runs timers and loading. */
void au_page_idle(AuPage *p);
/* Whether the page wants au_page_idle called soon (loading, animating);
 * the shell can sleep longer when no page does. */
bool au_page_busy(const AuPage *p);

#endif
