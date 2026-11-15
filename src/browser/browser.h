/* The browser model and controller: windows, tabs, each tab's
 * back/forward list, and the glue between the chrome (chrome.h), the
 * pages (page.h) and the global state (preferences, history,
 * bookmarks). Portable C99: the platform shell creates windows, feeds
 * events in, and paints and shows what this asks for through
 * AuBrowserHost. The host tool and the tests drive it the same way.
 *
 * To use it: create an AuBrowser with au_browser_new, load the saved
 * files into its prefs, history and bookmarks, create windows with
 * au_browser_window_new and give each a tab with au_bw_new_tab. Then
 * pass input to au_bw_mouse and au_bw_key, call au_bw_idle from the event
 * loop, and paint damaged areas with au_bw_paint when host->invalidate
 * reports them.
 *
 * Saving: prefs_gen, history.generation, bookmarks.generation and
 * session_gen change whenever the matching data does; the shell saves a
 * file a short while after its counter moves. */
#ifndef AURA_BROWSER_BROWSER_H
#define AURA_BROWSER_BROWSER_H

#include "bookmarks.h"
#include "chrome.h"
#include "downloads.h"
#include "history.h"
#include "page.h"
#include "prefs.h"

/* ---- Back/forward list ---- */

typedef struct AuNavEntry {
    char *url, *title;
    int scroll_y; /* where the page was scrolled when left */
} AuNavEntry;

typedef struct AuNavList {
    AuNavEntry *e;
    int n, cap;
    int index; /* the current entry; -1 when empty */
} AuNavList;

#define AU_NAV_MAX 50 /* entries kept per tab; the oldest go first */

void au_nav_init(AuNavList *l);
void au_nav_free(AuNavList *l);
/* Adds url after the current entry, dropping the forward entries, and
 * makes it current. */
void au_nav_push(AuNavList *l, const char *url, const char *title);
AuNavEntry *au_nav_current(AuNavList *l);
bool au_nav_can_go(const AuNavList *l, int delta);
/* Moves by delta entries (negative is back). Returns the new current
 * entry, or NULL (and does not move) when out of range. */
AuNavEntry *au_nav_go(AuNavList *l, int delta);

/* ---- Browser ---- */

typedef struct AuBrowser AuBrowser;
typedef struct AuBrowserWindow AuBrowserWindow;
typedef struct AuTab AuTab;

/* What au_bw_* changed, for AuBrowserHost.changed. */
enum {
    AU_CHANGED_FIELD = 1,  /* the address field's text (au_bw_field_text) */
    AU_CHANGED_TITLE = 2,  /* the window title (au_bw_window_title) */
    AU_CHANGED_LAYOUT = 4, /* chrome rectangles moved: place native controls again */
    AU_CHANGED_SCROLL = 8, /* scroll position or range of the active tab */
    AU_CHANGED_MENUS = 16, /* tab list, history or bookmarks: rebuild menus */
    AU_CHANGED_FOCUS_FIELD = 32, /* put the keyboard focus in the address field */
    AU_CHANGED_FOCUS_PAGE = 64   /* take it out of the field: keys go to the page */
};

typedef struct AuBrowserHost {
    void *ctx;
    uint32_t (*time_ms)(void *ctx);
    AuTime (*now)(void *ctx);
    bool (*read_file_url)(void *ctx, const char *url, char **data, size_t *len);
    /* A rectangle of the window surface needs painting (au_bw_paint) and
     * showing. */
    void (*invalidate)(void *ctx, AuBrowserWindow *w, AuIRect r);
    void (*changed)(void *ctx, AuBrowserWindow *w, unsigned what);
    /* The window's last tab closed: the shell closes the window and calls
     * au_browser_window_free. */
    void (*close_window)(void *ctx, AuBrowserWindow *w);
    /* Optional. An address Aura does not open itself (mailto:, ftp:,
     * news:...; see au_browser_opens_url) was navigated to: the shell
     * hands it to a helper application and returns true, or returns
     * false and the tab shows that Aura cannot open it. */
    bool (*open_external)(void *ctx, const char *url);
    /* Optional. Asked before a download is added (Save Link As, or a page
     * that is a file): the shell may warn (for example before an
     * application) and return false to drop it. name is the sanitized
     * file name. */
    bool (*confirm_download)(void *ctx, const char *url, const char *name, const char *mime);
    /* Optional. A download was added (the shell shows the Downloads
     * window). */
    void (*download_added)(void *ctx, AuDownload *d);
    /* Optional, for forms. A select's pop-up menu of n items over window
     * rectangle at, item selected checked: returns the item chosen or -1.
     * Without it a click steps through the options. */
    int (*popup)(void *ctx, AuBrowserWindow *w, const AuPopupItem *items, int n, int selected, AuIRect at);
    /* Asks for files for an input type=file: true with *files (n of them,
     * au_alloc'd with their names, types and bytes, taken over). */
    bool (*choose_file)(void *ctx, bool multiple, AuControlFile **files, uint32_t *n);
    /* A yes or no question (resending a form, sending one from a secure
     * page over plain http). Without it the answer is yes. */
    bool (*confirm)(void *ctx, AuBrowserWindow *w, const char *message);
} AuBrowserHost;

struct AuTab {
    uint32_t id;
    AuBrowserWindow *win;
    AuNavList nav;
    AuPage *page;
    char *status;
    int scroll_x, scroll_y;
    bool loading;
    bool close_pending; /* opened only for a download: closed at the next idle */
};

struct AuBrowserWindow {
    AuBrowser *browser;
    void *platform; /* the shell's window */
    AuTab **tabs;
    int ntabs, cap, active;
    int width, height;
    int x, y; /* the window's position on screen (set by the shell; -1 when not known), for the session */
    int tab_scroll;
    AuChromeLayout layout;
    AuHit hover, pressed;
    int drag_tab; /* tab being dragged, or -1 */
    int drag_x, drag_y;
    int mouse_x, mouse_y; /* the pointer's last position (window), for hover after scrolling */
    bool mouse_in_page;
    bool drag_moved;
    int split_x, split_w; /* splitter drag start */
    char *field;          /* address field text while edited, else NULL */
    bool field_focused;
    bool native_field;    /* the shell edits the field natively */
    bool native_standins; /* paint scroll bar and grow box stand-ins */
    bool active_window;
    int throbber_frame;
    uint32_t throbber_ms;
    AuFontList *label_list, *small_list; /* the chrome's fonts, made on first paint */
    bool painting;      /* inside au_bw_paint */
    bool hscroll_dirty; /* the page width changed during a paint: check the scroll bar at the next idle */
};

#define AU_MAX_FETCHERS 8
#define AU_MAX_CLOSED_TABS 10

struct AuBrowser {
    AuPrefs prefs;
    AuHistory history;
    AuBookmarks bookmarks;
    AuDownloads downloads; /* the shell sets its transfer source (au_sim_source until it does) */
    AuFontSet *fonts;
    AuBrowserHost host;
    AuBrowserWindow **windows;
    int nwindows;
    uint32_t prefs_gen, session_gen;
    uint32_t scroll_gen; /* any tab's scroll position changed (saved with the session, less often) */
    uint32_t next_tab_id;
    uint32_t highlight;  /* selection colour (paint ARGB) for new tabs; 0 for the default */
    char *find_text;     /* the last Find, for Find Again; NULL before one */
    bool find_case;      /* it was case-sensitive */
    AuNavList closed[AU_MAX_CLOSED_TABS]; /* closed tabs' histories, oldest first */
    int nclosed;
    struct {
        char scheme[16];
        AuPageFetcher fetcher;
    } fetchers[AU_MAX_FETCHERS]; /* see au_browser_set_fetcher */
    int nfetchers;
};

AuBrowser *au_browser_new(AuFontSet *fonts, const AuBrowserHost *host);
/* Frees the windows (without calling close_window) and everything else. */
void au_browser_free(AuBrowser *b);
/* Call after changing b->prefs: lays windows out again (tab position,
 * column width), repaints, and bumps prefs_gen. */
void au_browser_prefs_changed(AuBrowser *b);
/* Call after changing b->bookmarks or b->history from outside (the
 * manager windows): updates the bookmark buttons and menus. */
void au_browser_bookmarks_changed(AuBrowser *b);
void au_browser_history_changed(AuBrowser *b);
/* Registers the fetcher pages use for URLs of scheme (lower case, no
 * ':'), replacing an earlier one; NULL removes it. A network stack calls
 * this for "http" and "https" at start-up, before windows open. */
void au_browser_set_fetcher(AuBrowser *b, const char *scheme, const AuPageFetcher *f);
/* Whether Aura opens url itself: about:, file:, view-source:, http:,
 * https:, a scheme with a fetcher, or text without a scheme. */
bool au_browser_opens_url(const AuBrowser *b, const char *url);
/* Whether any page is loading (the shell then idles more often). */
bool au_browser_busy(const AuBrowser *b);

/* A window with no tabs yet; width and height are the content size. */
AuBrowserWindow *au_browser_window_new(AuBrowser *b, int width, int height);
void au_browser_window_free(AuBrowserWindow *w);

/* ---- Tabs ---- */
/* Opens url (NULL: about:blank) in a new tab at index (-1: after the
 * active tab), activating it when activate is set. Returns NULL when
 * the host's open_external took the address instead (the window keeps
 * its tabs; a window's first tab always opens). */
AuTab *au_bw_new_tab(AuBrowserWindow *w, const char *url, bool activate, int index);
/* Closes tab i. Closing the last tab asks the host to close the window. */
void au_bw_close_tab(AuBrowserWindow *w, int i);
void au_bw_select_tab(AuBrowserWindow *w, int i);
void au_bw_move_tab(AuBrowserWindow *w, int from, int to);
AuTab *au_bw_active_tab(AuBrowserWindow *w);
/* Opens a copy of tab i (its back/forward list, scroll position and
 * zoom) to its right and activates it. */
AuTab *au_bw_duplicate_tab(AuBrowserWindow *w, int i);
/* Command-click on Back or Forward: opens the entry delta steps away
 * in a new tab behind the active one, with a copy of its history. */
AuTab *au_bw_go_new_tab(AuBrowserWindow *w, int delta);
/* The back (dir < 0) or forward (dir > 0) entries of the active tab,
 * nearest first, for a history pop-up: up to cap items. The strings
 * belong to the tab and last until it navigates. */
typedef struct AuHistoryItem {
    int delta; /* for au_bw_go */
    const char *title, *url;
} AuHistoryItem;
int au_bw_history_items(AuBrowserWindow *w, int dir, AuHistoryItem *out, int cap);
/* Reopen Closed Tab: the most recently closed tab comes back (its
 * history and scroll position) after the active tab. The last
 * AU_MAX_CLOSED_TABS closed tabs are kept, across windows. */
bool au_browser_can_reopen(const AuBrowser *b);
AuTab *au_bw_reopen_closed_tab(AuBrowserWindow *w);
/* Closes every tab but tab keep, which becomes active. */
void au_bw_close_other_tabs(AuBrowserWindow *w, int keep);
void au_bw_reload_tab(AuBrowserWindow *w, int i);

/* ---- Navigation (active tab) ---- */
/* Address-field input: a URL, a path or a search (url.h). */
void au_bw_navigate_input(AuBrowserWindow *w, const char *text);
void au_bw_navigate(AuBrowserWindow *w, const char *url);
void au_bw_back(AuBrowserWindow *w);
void au_bw_forward(AuBrowserWindow *w);
void au_bw_go(AuBrowserWindow *w, int delta);
void au_bw_reload(AuBrowserWindow *w);
/* Shift- or Option-reload: fetches the page and its resources again,
 * bypassing the cache. */
void au_bw_reload_bypass(AuBrowserWindow *w);
void au_bw_stop(AuBrowserWindow *w);
/* Opens the certificate viewer (a new tab) for the active page; false
 * when the page has no certificate. Clicking the security indicator does
 * this. */
bool au_bw_show_certificate(AuBrowserWindow *w);
/* Downloads url into the download folder (Save Link As, Save Image As);
 * NULL when it is not http or https or the shell declined. */
AuDownload *au_bw_save_url(AuBrowserWindow *w, const char *url);
/* Adds a download (asking host.confirm_download first); handoff is a
 * page's transfer to continue (AuDownload.handoff) or 0. */
AuDownload *au_browser_download(AuBrowser *b, const char *url, const char *name, const char *mime, int64_t total,
                                uint32_t handoff);
void au_bw_home(AuBrowserWindow *w);
/* Adds the current page to the bookmarks, or removes it when it is
 * bookmarked already and toggle is set. Returns the new bookmark or
 * NULL. */
AuBookmark *au_bw_bookmark_page(AuBrowserWindow *w, bool toggle);
bool au_bw_is_bookmarked(AuBrowserWindow *w);
/* Adds url to the bookmarks (title, else the URL, as its name) unless it
 * is there already; returns the bookmark. */
AuBookmark *au_browser_bookmark_url(AuBrowser *b, const char *url, const char *title);
/* View Source: opens view-source: of the active page in a new tab
 * (file, http and https pages only). */
bool au_bw_can_view_source(AuBrowserWindow *w);
void au_bw_view_source(AuBrowserWindow *w);
/* Save Page As: the active page's source (au_page_source) and a file
 * name to suggest (from the URL, else the title, with ".html" when it
 * has no extension; HFS-safe, cap >= 128). */
bool au_bw_page_source(AuBrowserWindow *w, const char **data, size_t *len);
void au_bw_save_name(AuBrowserWindow *w, char *out, size_t cap);

/* ---- Selection and find (active tab) ---- */
bool au_bw_has_selection(AuBrowserWindow *w);
/* Appends the selected text to out; false when none. */
bool au_bw_selection_text(AuBrowserWindow *w, AuBuf *out);
void au_bw_select_all(AuBrowserWindow *w);
/* Finds text (remembered for au_bw_find_again) forward from the
 * selection; false when there is no match. */
bool au_bw_find(AuBrowserWindow *w, const char *text, bool case_sensitive);
/* Repeats the last find forward or backward; false without a match or
 * an earlier find. */
bool au_bw_find_again(AuBrowserWindow *w, bool forward);

/* ---- Text zoom (active tab) ----
 * dir > 0 zooms in a step, dir < 0 out, 0 back to 100%. The site's zoom
 * is remembered (prefs.zoom_sites) and applied when a tab next opens a
 * page of that site. */
void au_bw_zoom(AuBrowserWindow *w, int dir);
bool au_bw_can_zoom(AuBrowserWindow *w, int dir);
/* The remembered zoom for url's site (1 when none). */
float au_browser_site_zoom(const AuBrowser *b, const char *url);

/* ---- Address field ---- */
/* The text the field shows: the edit in progress, else the current URL
 * ("" for about:blank). */
const char *au_bw_field_text(AuBrowserWindow *w);
/* Records an edit (NULL ends editing and shows the URL again). */
void au_bw_set_field_text(AuBrowserWindow *w, const char *text);
void au_bw_set_field_focus(AuBrowserWindow *w, bool focused);
/* The user is typing in the field: it has the focus and holds an edit.
 * Navigation and tab switches move the focus to the page otherwise (see
 * AU_CHANGED_FOCUS_PAGE). */
bool au_bw_typing(const AuBrowserWindow *w);
/* Inline completion for typed text from history and bookmarks: the full
 * URL whose match key extends typed, or NULL. */
const char *au_bw_complete(AuBrowserWindow *w, const char *typed);

/* ---- Geometry, painting, input ---- */
void au_bw_resize(AuBrowserWindow *w, int width, int height);
/* The shell moved the window (screen position of its content's top
 * left): remembered with the session. */
void au_bw_set_position(AuBrowserWindow *w, int x, int y);
void au_bw_set_active(AuBrowserWindow *w, bool active); /* window activation */
void au_bw_paint(AuBrowserWindow *w, AuCanvas *c, AuIRect clip);
/* Mouse events in window coordinates. Returns the hit the event was
 * routed to, so the shell can take over parts it handles natively. */
AuHit au_bw_mouse(AuBrowserWindow *w, const AuEvent *ev);
/* Keys not taken by the address field: the page's focused element and
 * script first, then scrolling keys. */
bool au_bw_key(AuBrowserWindow *w, const AuEvent *ev);
/* A text field in the active page has the keyboard (the address field
 * does not): keys and the Edit menu go to it. cmd is AU_PAGE_EDIT_*;
 * see au_page_edit. */
bool au_bw_page_editing(AuBrowserWindow *w);
bool au_bw_page_can_edit(AuBrowserWindow *w, int cmd);
bool au_bw_page_edit(AuBrowserWindow *w, int cmd, const char *text, size_t len, AuBuf *out);
void au_bw_scroll_to(AuBrowserWindow *w, int x, int y);
void au_bw_scroll_by(AuBrowserWindow *w, int dx, int dy);
/* The largest scroll offsets for the active tab (0 when it fits). */
void au_bw_scroll_max(AuBrowserWindow *w, int *max_x, int *max_y);
/* Runs page timers and the throbber. */
void au_bw_idle(AuBrowserWindow *w);
/* The window title for the active tab. */
void au_bw_window_title(AuBrowserWindow *w, char *out, size_t cap);
/* The rectangle a hit part occupies (for repainting). */
AuIRect au_bw_hit_rect(AuBrowserWindow *w, AuHit h);

/* ---- Session (open windows and tabs, for "restore last tabs") ----
 * File format:
 *     # Aura session 1
 *     window <width> <height> <active tab> <x> <y>
 *     tab <current entry>
 *     entry\t<url>\t<title>\t<scroll y>
 * Windows hold the tab lines that follow, tabs the entry lines. The
 * window position (-1 -1 when not known) and the scroll position are
 * optional (older files lack them); for the current
 * entry it is where the tab is scrolled now. session_gen changes with
 * the windows, tabs and entries; scroll_gen with the scroll positions,
 * which the shell saves less often. */
typedef struct AuSessionTab {
    AuNavList nav;
} AuSessionTab;
typedef struct AuSessionWindow {
    int width, height, active;
    int x, y; /* the window's position, -1 when not known */
    AuSessionTab *tabs;
    int ntabs;
} AuSessionWindow;
typedef struct AuSession {
    AuSessionWindow *windows;
    int nwindows;
} AuSession;

void au_browser_session_serialize(const AuBrowser *b, AuBuf *out);
/* Returns false for a damaged file (what parsed is kept). */
bool au_session_parse(AuSession *s, const char *data, size_t len);
void au_session_free(AuSession *s);
/* Adds the session window's tabs to w (loading each tab's current
 * entry, scrolled to where it was) and activates its active tab. */
void au_bw_restore(AuBrowserWindow *w, const AuSessionWindow *sw);

#endif
