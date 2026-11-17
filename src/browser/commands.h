/* Commands and menus: every command Aura's menus offer, with its title,
 * shortcut, menu and position, whether it is enabled or checked in a
 * given state, and what it does. Portable C99; each port builds its
 * menus natively from this table.
 *
 * To build the menu bar, walk au_menu_bar() and, for each menu,
 * au_menu_info(id)->items (AU_CMD_SEPARATOR is a divider line); add the
 * entries of the menu's dynamic list (au_menu_list_history,
 * au_menu_list_bookmarks or au_menu_list_windows) after them. Before a
 * menu is shown or a shortcut is looked up, fill an AuCommandState and
 * apply au_command_enabled and au_command_checked to each item.
 *
 * To run a command, call au_command_run. Commands that need a native
 * window or dialog (Preferences, About, Open File, Save Page As, the
 * list and developer windows, Find) come back to the port through its
 * AuCommandHost. */
#ifndef AURA_BROWSER_COMMANDS_H
#define AURA_BROWSER_COMMANDS_H

#include "browser.h"

typedef enum AuCommand {
    AU_CMD_NONE,
    AU_CMD_SEPARATOR,

    /* Contextual menus (context.h builds them). */
    /* a link */
    AU_CMD_OPEN_LINK,
    AU_CMD_OPEN_LINK_TAB,
    AU_CMD_OPEN_LINK_WINDOW, /* AuCommandHost.new_window */
    AU_CMD_COPY_LINK,        /* AuCommandHost.copy_text: AuContextMenu.link */
    AU_CMD_BOOKMARK_LINK,
    /* an image */
    AU_CMD_OPEN_IMAGE_TAB,
    AU_CMD_COPY_IMAGE, /* AuCommandHost.copy_text: AuContextMenu.image */
    /* the page */
    AU_CMD_BACK,
    AU_CMD_FORWARD,
    AU_CMD_RELOAD,
    AU_CMD_VIEW_SOURCE,
    AU_CMD_BOOKMARK_PAGE,
    AU_CMD_COPY, /* AuCommandHost.edit: copy the selection */
    /* the address field (AuCommandHost.edit) */
    AU_CMD_CUT,
    AU_CMD_FIELD_COPY,
    AU_CMD_PASTE,
    AU_CMD_PASTE_GO,
    AU_CMD_SELECT_ALL,
    /* a tab */
    AU_CMD_CLOSE_TAB,
    AU_CMD_CLOSE_OTHER_TABS,
    AU_CMD_RELOAD_TAB,
    AU_CMD_DUPLICATE_TAB,
    /* downloads */
    AU_CMD_SAVE_LINK,
    AU_CMD_SAVE_IMAGE,
    /* the page: "Turn Off (On) Blocking for This Site" */
    AU_CMD_TOGGLE_BLOCKING,
    /* a tab: "Unload Tab" (au_bw_unload_tab) */
    AU_CMD_UNLOAD_TAB,

    /* The menu bar. */
    AU_CMD_ABOUT,
    AU_CMD_FILE_NEW_WINDOW,
    AU_CMD_FILE_NEW_TAB,
    AU_CMD_FILE_OPEN_LOCATION,
    AU_CMD_FILE_OPEN_FILE,
    AU_CMD_FILE_CLOSE_TAB,
    AU_CMD_FILE_CLOSE_WINDOW,
    AU_CMD_FILE_REOPEN_TAB,
    AU_CMD_FILE_SAVE_AS,
    AU_CMD_FILE_QUIT,
    AU_CMD_EDIT_UNDO,
    AU_CMD_EDIT_CUT,
    AU_CMD_EDIT_COPY,
    AU_CMD_EDIT_PASTE,
    AU_CMD_EDIT_CLEAR,
    AU_CMD_EDIT_SELECT_ALL,
    AU_CMD_EDIT_FIND,
    AU_CMD_EDIT_FIND_AGAIN,
    AU_CMD_EDIT_FIND_PREVIOUS,
    AU_CMD_EDIT_PREFERENCES,
    AU_CMD_VIEW_RELOAD,
    AU_CMD_VIEW_STOP,
    AU_CMD_VIEW_PAGE_SOURCE,
    AU_CMD_VIEW_ZOOM_IN,
    AU_CMD_VIEW_ZOOM_OUT,
    AU_CMD_VIEW_ACTUAL_SIZE,
    AU_CMD_VIEW_TABS_TOP,
    AU_CMD_VIEW_TABS_LEFT,
    AU_CMD_GO_BACK,
    AU_CMD_GO_FORWARD,
    AU_CMD_GO_HOME,
    AU_CMD_GO_HISTORY,
    AU_CMD_BOOKMARKS_ADD,
    AU_CMD_BOOKMARKS_SHOW,
    AU_CMD_DEV_PAGE_SOURCE,
    AU_CMD_DEV_CONSOLE,
    AU_CMD_DEV_NETWORK,
    AU_CMD_DEV_PERFORMANCE,
    AU_CMD_DEV_TIMING_TEST,
    AU_CMD_DEV_SYSTEM_STATUS,
    AU_CMD_DEV_TESTS, /* the Tests submenu */
    AU_CMD_DEV_REPORT,
    AU_CMD_TESTS_SCENE,
    AU_CMD_TESTS_FONTS,
    AU_CMD_TESTS_BENCHMARK,
    AU_CMD_WINDOW_DOWNLOADS,
    AU_CMD_WINDOW_COOKIES,
    AU_CMD_HELP_REPORT,

    AU_CMD_COUNT
} AuCommand;

/* The menus, in no particular order; au_menu_bar gives the bar's order. */
typedef enum AuMenuId {
    AU_MENU_NONE,
    AU_MENU_APPLICATION, /* the Apple menu on Mac OS 9 */
    AU_MENU_FILE,
    AU_MENU_EDIT,
    AU_MENU_VIEW,
    AU_MENU_GO,
    AU_MENU_BOOKMARKS,
    AU_MENU_DEVELOP,
    AU_MENU_TESTS, /* a submenu of Develop (AU_CMD_DEV_TESTS) */
    AU_MENU_WINDOW,
    AU_MENU_HELP, /* on Mac OS 9 its items go after the system's own */
    AU_MENU_CONTEXT, /* contextual menus; context.c chooses the items */
    AU_MENU_COUNT
} AuMenuId;

/* What a menu lists after its fixed items. */
typedef enum AuMenuDynamic {
    AU_MENU_LIST_NONE,
    AU_MENU_LIST_HISTORY,   /* au_menu_list_history */
    AU_MENU_LIST_BOOKMARKS, /* au_menu_list_bookmarks */
    AU_MENU_LIST_WINDOWS    /* au_menu_list_windows */
} AuMenuDynamic;

/* Shortcut modifiers. AU_SHORTCUT_PRIMARY is Command on Mac OS 9 and
 * Ctrl on Linux; AU_SHORTCUT_ALT is Option on Mac OS 9. */
enum { AU_SHORTCUT_PRIMARY = 1, AU_SHORTCUT_SHIFT = 2, AU_SHORTCUT_ALT = 4 };

typedef struct AuShortcut {
    char key;           /* 0: none; else an upper-case letter, digit or punctuation */
    unsigned char mods; /* AU_SHORTCUT_* */
} AuShortcut;

typedef struct AuCommandInfo {
    AuCommand cmd;
    AuMenuId menu;
    const char *title; /* UTF-8; "..." marks a command that asks for more */
    AuShortcut shortcut;
    AuMenuId submenu; /* the submenu this item opens, or AU_MENU_NONE */
} AuCommandInfo;

typedef struct AuMenuInfo {
    AuMenuId id;
    const char *title; /* UTF-8; AU_MENU_APPLICATION's is the application's name */
    const AuCommand *items; /* in order; AU_CMD_SEPARATOR is a divider line */
    int nitems;
    AuMenuDynamic list;
} AuMenuInfo;

/* The table entry for cmd (NULL for AU_CMD_NONE, AU_CMD_SEPARATOR and
 * values out of range). */
const AuCommandInfo *au_command_info(AuCommand cmd);
/* The menu's description (NULL for AU_MENU_NONE and values out of range). */
const AuMenuInfo *au_menu_info(AuMenuId id);
/* The menu bar's menus from left to right: Application, File, Edit,
 * View, Go, Bookmarks, Develop, Window. Help and Tests are not in it. */
const AuMenuId *au_menu_bar(int *n);
/* The command whose shortcut is key with mods (letters in either case),
 * or AU_CMD_NONE. */
AuCommand au_command_for_shortcut(char key, unsigned mods);

/* ---- State ---- */

/* What is in front, as the port sees it. */
typedef enum AuFrontKind {
    AU_FRONT_NONE,    /* no window */
    AU_FRONT_BROWSER, /* AuCommandState.window is the front window */
    AU_FRONT_TOOL,    /* a developer window, which finds in its own text */
    AU_FRONT_OTHER    /* any other window (lists, Preferences, desk accessories) */
} AuFrontKind;

typedef struct AuCommandState {
    AuBrowser *browser;
    /* The frontmost browser window, in front or not; NULL when there is none. */
    AuBrowserWindow *window;
    AuFrontKind front;
    /* The front window has a text field with the keyboard focus (or is a
     * desk accessory, which takes the Edit menu itself). */
    bool text_focus;
    /* AU_FRONT_TOOL: the tool window has something to find again. */
    bool tool_can_find_again;
    /* AU_FRONT_OTHER: the window (a list) has a selection Copy can take.
     * Copy then reaches the port's edit hook; the other Edit commands
     * stay off. */
    bool other_can_copy;
    /* The modifier keys (AU_MOD_*, platform.h) held as the command was
     * chosen: Reload with Shift or Option reloads past the cache. */
    unsigned mods;
} AuCommandState;

/* ---- The port's part ---- */

/* The native windows a command opens. */
typedef enum AuWindowKind {
    AU_WINDOW_ABOUT,
    AU_WINDOW_PREFERENCES,
    AU_WINDOW_HISTORY,
    AU_WINDOW_BOOKMARKS,
    AU_WINDOW_DOWNLOADS,
    AU_WINDOW_COOKIES,
    AU_WINDOW_PAGE_SOURCE,
    AU_WINDOW_CONSOLE,
    AU_WINDOW_NETWORK,
    AU_WINDOW_PERFORMANCE,
    AU_WINDOW_SYSTEM_STATUS,
    AU_WINDOW_REPORT,
    /* Run the timing test on the front browser window, then show the
     * Performance window with its result. */
    AU_WINDOW_TIMING_TEST,
    /* Run the paint benchmark, then show the System Status window with
     * its result. */
    AU_WINDOW_PAINT_BENCHMARK
} AuWindowKind;

/* Editing in the front window's text (the address field, a page field,
 * or a desk accessory). */
typedef enum AuEditAction {
    AU_EDIT_UNDO,
    AU_EDIT_CUT,
    AU_EDIT_COPY,
    AU_EDIT_PASTE,
    AU_EDIT_CLEAR,
    AU_EDIT_SELECT_ALL,
    AU_EDIT_PASTE_AND_GO /* paste into the address field and open it */
} AuEditAction;

typedef struct AuCommandHost {
    void *ctx;
    /* Opens a browser window on url (NULL: the home page) and returns its
     * model, or NULL when it could not. */
    AuBrowserWindow *(*new_window)(void *ctx, const char *url);
    /* Brings w's window to the front. */
    void (*raise)(void *ctx, AuBrowserWindow *w);
    /* Puts the keyboard focus in w's address field with its text selected. */
    void (*focus_address)(void *ctx, AuBrowserWindow *w);
    /* Shows (or brings forward) a native window. Returns false when the
     * port has none of that kind; for AU_WINDOW_PAGE_SOURCE the model
     * then opens a view-source: tab. */
    bool (*show_window)(void *ctx, AuWindowKind kind);
    /* Asks for a file to open; returns its file: URL (au_alloc'd) or NULL. */
    char *(*choose_file_url)(void *ctx);
    /* Asks where to save w's page and saves its source. */
    void (*save_page)(void *ctx, AuBrowserWindow *w);
    /* Closes the front window, whatever its kind. */
    void (*close_front)(void *ctx);
    /* Asks for text to find: in w's active page, or with w NULL in the
     * front developer window. */
    void (*find)(void *ctx, AuBrowserWindow *w);
    /* Finds again in the front developer window. */
    void (*tool_find_again)(void *ctx, bool forward);
    void (*edit)(void *ctx, AuEditAction action);
    /* Puts text on the clipboard. */
    void (*copy_text)(void *ctx, const char *utf8, size_t len);
    /* Brings a window from the Window menu forward (AuMenuEntry.window). */
    void (*select_window)(void *ctx, void *window);
    void (*beep)(void *ctx);
    void (*quit)(void *ctx);
} AuCommandHost;

/* ---- Enabled, checked, title ---- */

bool au_command_enabled(const AuCommandState *st, AuCommand cmd);
/* The command a shortcut runs in this state: au_command_for_shortcut,
 * except that Close Tab's shortcut is Close Window where Close Tab is
 * disabled (a window without tabs in front). The caller still checks
 * au_command_enabled. */
AuCommand au_command_for_key(const AuCommandState *st, char key, unsigned mods);
bool au_command_checked(const AuCommandState *st, AuCommand cmd);
/* The title to show now: the table's, or for a command whose title
 * follows the state (AU_CMD_TOGGLE_BLOCKING) the current one. */
const char *au_command_title(const AuCommandState *st, AuCommand cmd);

/* ---- Running ---- */

/* Runs a menu bar command. Does nothing for a disabled command. */
void au_command_run(const AuCommandState *st, const AuCommandHost *host, AuCommand cmd);
/* Opens url in the front browser window (in a new tab when new_tab),
 * or in a new window when there is none, and brings it forward. */
void au_command_open_url(const AuCommandState *st, const AuCommandHost *host, const char *url, bool new_tab);

/* ---- Dynamic menus ---- */

typedef enum AuMenuEntryKind {
    AU_ENTRY_ITEM,
    AU_ENTRY_SEPARATOR,
    AU_ENTRY_FOLDER /* opens a submenu holding the entries whose parent it is */
} AuMenuEntryKind;

typedef struct AuMenuEntry {
    AuMenuEntryKind kind;
    char *title;  /* UTF-8, owned */
    int parent;   /* the AU_ENTRY_FOLDER entry it is in, or -1: the menu itself */
    bool enabled;
    bool checked;
    char *url;          /* history: the URL it opens (owned), else NULL */
    uint32_t bookmark;  /* bookmarks: the bookmark's id, else 0 */
    void *window;       /* windows: the port's window, else NULL */
} AuMenuEntry;

typedef struct AuMenuList {
    AuMenuEntry *e;
    int n, cap;
} AuMenuList;

/* The Go menu's history items, at most this many. */
#define AU_GO_MENU_ENTRIES 15

/* The most recent history entries: a separator, then up to
 * AU_GO_MENU_ENTRIES items; nothing when the history is empty. */
void au_menu_list_history(AuBrowser *b, AuMenuList *out);
/* The bookmarks, breadth-first: the top level, then the contents of
 * each folder in the order the folders were listed, so every entry
 * comes after its folder. An empty folder holds one disabled
 * "(empty)" item. */
void au_menu_list_bookmarks(AuBrowser *b, AuMenuList *out);
/* A window of the port, for au_menu_list_windows. */
typedef struct AuWindowRef {
    const char *title; /* UTF-8 */
    void *window;
} AuWindowRef;
/* The Window menu's window items for the port's visible windows, front
 * to back: a separator, then one item each (an empty title becomes a
 * space), the front one checked; nothing when there are none. */
void au_menu_list_windows(const AuWindowRef *wins, int n, void *front, AuMenuList *out);
void au_menu_list_free(AuMenuList *l);
/* Runs a dynamic entry: opens its URL or bookmark in the front window,
 * or brings its window forward. */
void au_menu_entry_run(const AuCommandState *st, const AuCommandHost *host, const AuMenuEntry *e);

/* ---- Contextual menus ---- */

struct AuContextMenu;
/* Runs cmd from contextual menu m (context.h): the model's commands
 * through au_bw_context_command, the rest through the host (clipboard,
 * editing in the address field, a new window, the Page Source window). */
void au_command_run_context(const AuCommandState *st, const AuCommandHost *host, const struct AuContextMenu *m,
                            AuCommand cmd);

#endif
