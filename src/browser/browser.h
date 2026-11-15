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
    AU_CHANGED_FOCUS_FIELD = 32 /* put the keyboard focus in the address field */
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
} AuBrowserHost;

struct AuTab {
    uint32_t id;
    AuBrowserWindow *win;
    AuNavList nav;
    AuPage *page;
    char *status;
    int scroll_x, scroll_y;
    bool loading;
};

struct AuBrowserWindow {
    AuBrowser *browser;
    void *platform; /* the shell's window */
    AuTab **tabs;
    int ntabs, cap, active;
    int width, height;
    int tab_scroll;
    AuChromeLayout layout;
    AuHit hover, pressed;
    int drag_tab; /* tab being dragged, or -1 */
    int drag_x, drag_y;
    bool drag_moved;
    int split_x, split_w; /* splitter drag start */
    char *field;          /* address field text while edited, else NULL */
    bool field_focused;
    bool native_field;    /* the shell edits the field natively */
    bool native_standins; /* paint scroll bar and grow box stand-ins */
    bool active_window;
    int throbber_frame;
    uint32_t throbber_ms;
};

struct AuBrowser {
    AuPrefs prefs;
    AuHistory history;
    AuBookmarks bookmarks;
    AuFontSet *fonts;
    AuBrowserHost host;
    AuBrowserWindow **windows;
    int nwindows;
    uint32_t prefs_gen, session_gen;
    uint32_t next_tab_id;
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
/* Whether any page is loading (the shell then idles more often). */
bool au_browser_busy(const AuBrowser *b);

/* A window with no tabs yet; width and height are the content size. */
AuBrowserWindow *au_browser_window_new(AuBrowser *b, int width, int height);
void au_browser_window_free(AuBrowserWindow *w);

/* ---- Tabs ---- */
/* Opens url (NULL: about:blank) in a new tab at index (-1: after the
 * active tab), activating it when activate is set. */
AuTab *au_bw_new_tab(AuBrowserWindow *w, const char *url, bool activate, int index);
/* Closes tab i. Closing the last tab asks the host to close the window. */
void au_bw_close_tab(AuBrowserWindow *w, int i);
void au_bw_select_tab(AuBrowserWindow *w, int i);
void au_bw_move_tab(AuBrowserWindow *w, int from, int to);
AuTab *au_bw_active_tab(AuBrowserWindow *w);

/* ---- Navigation (active tab) ---- */
/* Address-field input: a URL, a path or a search (url.h). */
void au_bw_navigate_input(AuBrowserWindow *w, const char *text);
void au_bw_navigate(AuBrowserWindow *w, const char *url);
void au_bw_back(AuBrowserWindow *w);
void au_bw_forward(AuBrowserWindow *w);
void au_bw_go(AuBrowserWindow *w, int delta);
void au_bw_reload(AuBrowserWindow *w);
void au_bw_stop(AuBrowserWindow *w);
void au_bw_home(AuBrowserWindow *w);
/* Adds the current page to the bookmarks, or removes it when it is
 * bookmarked already and toggle is set. Returns the new bookmark or
 * NULL. */
AuBookmark *au_bw_bookmark_page(AuBrowserWindow *w, bool toggle);
bool au_bw_is_bookmarked(AuBrowserWindow *w);

/* ---- Address field ---- */
/* The text the field shows: the edit in progress, else the current URL
 * ("" for about:blank). */
const char *au_bw_field_text(AuBrowserWindow *w);
/* Records an edit (NULL ends editing and shows the URL again). */
void au_bw_set_field_text(AuBrowserWindow *w, const char *text);
void au_bw_set_field_focus(AuBrowserWindow *w, bool focused);
/* Inline completion for typed text from history and bookmarks: the full
 * URL whose match key extends typed, or NULL. */
const char *au_bw_complete(AuBrowserWindow *w, const char *typed);

/* ---- Geometry, painting, input ---- */
void au_bw_resize(AuBrowserWindow *w, int width, int height);
void au_bw_set_active(AuBrowserWindow *w, bool active); /* window activation */
void au_bw_paint(AuBrowserWindow *w, AuCanvas *c, AuIRect clip);
/* Mouse events in window coordinates. Returns the hit the event was
 * routed to, so the shell can take over parts it handles natively. */
AuHit au_bw_mouse(AuBrowserWindow *w, const AuEvent *ev);
/* Keys not taken by the shell (scrolling keys for the page). */
bool au_bw_key(AuBrowserWindow *w, const AuEvent *ev);
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
 *     window <width> <height> <active tab>
 *     tab <current entry>
 *     entry\t<url>\t<title>
 * Windows hold the tab lines that follow, tabs the entry lines. */
typedef struct AuSessionTab {
    AuNavList nav;
} AuSessionTab;
typedef struct AuSessionWindow {
    int width, height, active;
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
 * entry) and activates its active tab. */
void au_bw_restore(AuBrowserWindow *w, const AuSessionWindow *sw);

#endif
