/* The models of the Bookmarks, History, Downloads and Cookies windows.
 * Portable C99.
 *
 * A port shows each list in its own list widget and calls these
 * functions; it holds no list logic of its own. A model (AuListModel)
 * holds:
 *
 * - the rows (AuListItem): a stable id, a type (day heading, folder or
 *   entry), a depth for trees, whether a folder is open, an icon, the
 *   text of each column and a one-line summary for a list that shows
 *   only the first column and a detail;
 * - the selection, as a row index (one row, or none);
 * - the actions of the window (AuListAction): which buttons it has,
 *   whether each is enabled for the selection, and what each does.
 *
 * Rows hold only what is visible: the contents of a closed folder are
 * not listed. History is grouped by day under heading rows (depth 0,
 * entries at depth 1). Bookmark folders nest; a row's depth is its depth
 * below the root. Cookies are listed by site (a folder per site, its
 * cookies and storage areas at depth 1 when the site is open).
 * Downloads are a flat list, oldest first, in the order of
 * AuDownloads.items.
 *
 * To keep a window current, call au_list_stale from the event loop and,
 * when it returns true, au_list_refresh and redraw. Every refresh adds 1
 * to AuListModel.generation. au_list_run and the other actions refresh
 * the model themselves.
 *
 * Actions that need a native dialog (a name to type, details to show, a
 * choice of site rule) call the port through AuListHost and wait for
 * the answer. */
#ifndef AURA_BROWSER_LISTS_H
#define AURA_BROWSER_LISTS_H

#include "bookmarks.h"
#include "downloads.h"
#include "history.h"

#include "../net/cookie_view.h"

struct AuBrowser;
struct AuFavicons;

typedef enum { AU_LIST_BOOKMARKS, AU_LIST_HISTORY, AU_LIST_DOWNLOADS, AU_LIST_COOKIES, AU_LIST_KINDS } AuListKind;

typedef enum {
    AU_LIST_ITEM_GROUP,  /* a heading (a day in History); never selected for an action */
    AU_LIST_ITEM_FOLDER, /* a bookmark folder or a cookie site; can be opened and closed */
    AU_LIST_ITEM_ENTRY   /* a bookmark, a visited page, a download, a cookie or a storage area */
} AuListItemType;

typedef enum {
    AU_LIST_ICON_NONE,
    AU_LIST_ICON_FOLDER,
    AU_LIST_ICON_PAGE,    /* a page; icon_host names its site icon when the store has one */
    AU_LIST_ICON_FILE,    /* a download */
    AU_LIST_ICON_COOKIE,
    AU_LIST_ICON_STORAGE  /* a site's web storage area */
} AuListIcon;

#define AU_LIST_MAX_COLS 5

/* Ids: bookmark ids, AuHistoryEntry.id, download ids. A day heading's
 * id is AU_LIST_ID_GROUP plus the day number. A cookie row's id is a
 * hash of what it names (the site; the cookie's domain, path and name;
 * the storage area) with its type in the top two bits, so it stays the
 * same across refreshes; two rows can share one only by a hash
 * collision. 0 is never an id. */
#define AU_LIST_ID_GROUP 0x80000000u

typedef struct AuListItem {
    uint32_t id;
    AuListItemType type;
    int depth;
    bool expanded;  /* folders */
    AuListIcon icon;
    char *icon_host; /* AU_LIST_ICON_PAGE: the host whose site icon to show, or NULL */
    char *cols[AU_LIST_MAX_COLS]; /* au_list_columns many; "" when empty, never NULL */
    char *summary;  /* the other columns on one line, for a list that shows only cols[0] and a detail */
    int progress;   /* downloads: 0 to 1000 when the size is known, else -1 */
    int state;      /* downloads: the AuDownloadState; -1 for the other lists */
} AuListItem;

typedef enum {
    AU_LIST_OPEN,         /* opens the page in the front window, or opens or closes a folder */
    AU_LIST_OPEN_NEW_TAB, /* opens the page in a new tab */
    AU_LIST_NEW_FOLDER,   /* asks for a name; adds a folder in the selected folder or beside the selection */
    AU_LIST_RENAME,       /* asks for a name */
    AU_LIST_DELETE,       /* deletes or removes the selected row (and a folder's contents) */
    AU_LIST_CLEAR,        /* History: clears it; Downloads: clears completed; Cookies: removes all */
    AU_LIST_STOP,         /* stops a download */
    AU_LIST_RESUME,       /* resumes or retries a download */
    AU_LIST_REVEAL,       /* shows a finished download's file */
    AU_LIST_DETAILS,      /* shows every field of a cookie, storage area or site */
    AU_LIST_SITE_RULE,    /* asks for the site's cookie rule (allow, session only, block) */
    AU_LIST_ACTIONS
} AuListAction;

/* A button of a window, in the order the port shows them. width is the
 * width in pixels of the Mac OS 9 window's button; ports that size
 * buttons to their text ignore it. */
typedef struct AuListButton {
    AuListAction action;
    const char *label;
    int width;
} AuListButton;

/* What a port provides to the actions. ctx is passed back. */
typedef struct AuListHost {
    void *ctx;
    /* Opens url in the front browser window, or in a new tab. */
    void (*open_url)(void *ctx, const char *url, bool new_tab);
    /* Asks for a line of text: shows prompt with initial selected.
     * Returns the text (UTF-8, au_alloc'd, the caller frees it), or NULL
     * when the user cancelled. */
    char *(*ask_text)(void *ctx, const char *prompt, const char *initial);
    /* Shows text: lines of "Label: value" separated by '\n'. */
    void (*show_text)(void *ctx, const char *title, const char *text);
    /* Lets the user choose one of n items (current is checked) for
     * action's button. Returns the index chosen, or -1. */
    int (*choose)(void *ctx, AuListAction action, const char *const *items, int n, int current);
    /* Shows the download's file in the file manager; false when it
     * cannot. */
    bool (*reveal_download)(void *ctx, const AuDownload *d);
    /* The cookie jar's site rules changed and should be saved. */
    void (*cookie_rules_changed)(void *ctx);
} AuListHost;

/* Where the lists read their data. Any pointer may be NULL; that list is
 * then empty. browser, when set, is told after bookmarks or history
 * change (au_browser_bookmarks_changed, au_browser_history_changed). */
typedef struct AuListSources {
    AuBookmarks *bookmarks;
    AuHistory *history; /* its utc_offset gives the local time for days, times and dates */
    AuDownloads *downloads;
    struct AuFavicons *icons; /* site icons for icon_host; Clear History clears them */
    AuCookieJar *jar;
    AuWebStorage *storage; /* listed and removed with the sites' cookies */
    struct AuBrowser *browser;
} AuListSources;

/* The sources of a browser (bookmarks, history, downloads, site icons
 * and the browser) and a cookie jar and storage service. */
void au_list_sources(AuListSources *s, struct AuBrowser *b, AuCookieJar *jar, AuWebStorage *storage);

typedef enum {
    AU_LIST_NOTHING, /* not enabled, or cancelled */
    AU_LIST_DONE,
    AU_LIST_FAILED   /* the action could not be done (Reveal found no file): alert the user */
} AuListResult;

typedef struct AuListModel {
    AuListKind kind;
    AuListSources src;
    AuListHost host;
    AuListItem *items;
    int n, cap;
    int selected;        /* row index, or -1 */
    uint32_t generation; /* +1 on every refresh */
    /* Private. */
    uint32_t seen;       /* the sources' signature at the last refresh */
    uint32_t touched, seen_touched;
    AuCookieView cv;
    bool have_cv;
} AuListModel;

/* The window's title ("Bookmarks"), its columns' headings, and its
 * buttons. Return the number of headings or buttons. */
const char *au_list_title(AuListKind kind);
int au_list_columns(AuListKind kind, const char *const **headings);
int au_list_buttons(AuListKind kind, const AuListButton **buttons);

/* host may be NULL (no dialogs: actions that ask do nothing). The model
 * is filled at once with now (seconds since 1970). */
void au_list_init(AuListModel *m, AuListKind kind, const AuListSources *src, const AuListHost *host, AuTime now);
void au_list_free(AuListModel *m);

/* True when the data changed since the last refresh. */
bool au_list_stale(const AuListModel *m);
/* Marks the model stale: call it when the data changed in a way the
 * model cannot see (the cookie jar cleared and refilled). */
void au_list_touch(AuListModel *m);
/* Rebuilds the rows, keeping the selection on the same row where it can
 * (on the same id; History and Downloads keep the position when the row
 * went; Cookies select the site of a removed cookie). */
void au_list_refresh(AuListModel *m, AuTime now);

/* The row of id, or -1. */
int au_list_find(const AuListModel *m, uint32_t id);
/* Selects row (-1 for none). */
void au_list_select(AuListModel *m, int row);

/* Whether an action is enabled for the current selection. Actions the
 * window does not have are never enabled. */
bool au_list_can(const AuListModel *m, AuListAction a);
/* Runs an action on the current selection when it is enabled. */
AuListResult au_list_run(AuListModel *m, AuListAction a, AuTime now);
/* A double click or Return on row: selects it and opens the page, opens
 * or closes a folder, or shows a cookie's details. */
AuListResult au_list_activate(AuListModel *m, int row, AuTime now);
/* Opens or closes a folder row (its disclosure triangle). */
void au_list_toggle(AuListModel *m, int row, AuTime now);
/* Cookies: sets the selected site's rule without asking. */
bool au_list_set_site_rule(AuListModel *m, AuCookieRule rule, AuTime now);
/* Bookmarks: moves bookmark id into folder (0: the top level) before
 * bookmark before (0: at the end), as a drag does. False when a folder
 * would move into itself or an id is unknown. */
bool au_list_move_bookmark(AuListModel *m, uint32_t id, uint32_t folder, uint32_t before, AuTime now);

#endif
