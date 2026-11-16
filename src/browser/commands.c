/* Commands and menus (commands.h). */
#include "commands.h"
#include "context.h"

#include <string.h>

#define KEY(k) {k, AU_SHORTCUT_PRIMARY}
#define SHIFT_KEY(k) {k, AU_SHORTCUT_PRIMARY | AU_SHORTCUT_SHIFT}
#define NOKEY {0, 0}

/* Indexed by AuCommand. */
static const AuCommandInfo commands[AU_CMD_COUNT] = {
    [AU_CMD_OPEN_LINK] = {AU_CMD_OPEN_LINK, AU_MENU_CONTEXT, "Open Link", NOKEY, AU_MENU_NONE},
    [AU_CMD_OPEN_LINK_TAB] = {AU_CMD_OPEN_LINK_TAB, AU_MENU_CONTEXT, "Open Link in New Tab", NOKEY, AU_MENU_NONE},
    [AU_CMD_OPEN_LINK_WINDOW] = {AU_CMD_OPEN_LINK_WINDOW, AU_MENU_CONTEXT, "Open Link in New Window", NOKEY,
                                 AU_MENU_NONE},
    [AU_CMD_COPY_LINK] = {AU_CMD_COPY_LINK, AU_MENU_CONTEXT, "Copy Link Address", NOKEY, AU_MENU_NONE},
    [AU_CMD_BOOKMARK_LINK] = {AU_CMD_BOOKMARK_LINK, AU_MENU_CONTEXT, "Bookmark Link", NOKEY, AU_MENU_NONE},
    [AU_CMD_OPEN_IMAGE_TAB] = {AU_CMD_OPEN_IMAGE_TAB, AU_MENU_CONTEXT, "Open Image in New Tab", NOKEY, AU_MENU_NONE},
    [AU_CMD_COPY_IMAGE] = {AU_CMD_COPY_IMAGE, AU_MENU_CONTEXT, "Copy Image Address", NOKEY, AU_MENU_NONE},
    [AU_CMD_BACK] = {AU_CMD_BACK, AU_MENU_CONTEXT, "Back", NOKEY, AU_MENU_NONE},
    [AU_CMD_FORWARD] = {AU_CMD_FORWARD, AU_MENU_CONTEXT, "Forward", NOKEY, AU_MENU_NONE},
    [AU_CMD_RELOAD] = {AU_CMD_RELOAD, AU_MENU_CONTEXT, "Reload", NOKEY, AU_MENU_NONE},
    [AU_CMD_VIEW_SOURCE] = {AU_CMD_VIEW_SOURCE, AU_MENU_CONTEXT, "View Source", NOKEY, AU_MENU_NONE},
    [AU_CMD_BOOKMARK_PAGE] = {AU_CMD_BOOKMARK_PAGE, AU_MENU_CONTEXT, "Bookmark Page", NOKEY, AU_MENU_NONE},
    [AU_CMD_COPY] = {AU_CMD_COPY, AU_MENU_CONTEXT, "Copy", NOKEY, AU_MENU_NONE},
    [AU_CMD_CUT] = {AU_CMD_CUT, AU_MENU_CONTEXT, "Cut", NOKEY, AU_MENU_NONE},
    [AU_CMD_FIELD_COPY] = {AU_CMD_FIELD_COPY, AU_MENU_CONTEXT, "Copy", NOKEY, AU_MENU_NONE},
    [AU_CMD_PASTE] = {AU_CMD_PASTE, AU_MENU_CONTEXT, "Paste", NOKEY, AU_MENU_NONE},
    [AU_CMD_PASTE_GO] = {AU_CMD_PASTE_GO, AU_MENU_CONTEXT, "Paste and Go", NOKEY, AU_MENU_NONE},
    [AU_CMD_SELECT_ALL] = {AU_CMD_SELECT_ALL, AU_MENU_CONTEXT, "Select All", NOKEY, AU_MENU_NONE},
    [AU_CMD_CLOSE_TAB] = {AU_CMD_CLOSE_TAB, AU_MENU_CONTEXT, "Close Tab", NOKEY, AU_MENU_NONE},
    [AU_CMD_CLOSE_OTHER_TABS] = {AU_CMD_CLOSE_OTHER_TABS, AU_MENU_CONTEXT, "Close Other Tabs", NOKEY, AU_MENU_NONE},
    [AU_CMD_RELOAD_TAB] = {AU_CMD_RELOAD_TAB, AU_MENU_CONTEXT, "Reload", NOKEY, AU_MENU_NONE},
    [AU_CMD_DUPLICATE_TAB] = {AU_CMD_DUPLICATE_TAB, AU_MENU_CONTEXT, "Duplicate", NOKEY, AU_MENU_NONE},
    [AU_CMD_SAVE_LINK] = {AU_CMD_SAVE_LINK, AU_MENU_CONTEXT, "Save Link As", NOKEY, AU_MENU_NONE},
    [AU_CMD_SAVE_IMAGE] = {AU_CMD_SAVE_IMAGE, AU_MENU_CONTEXT, "Save Image As", NOKEY, AU_MENU_NONE},
    [AU_CMD_TOGGLE_BLOCKING] = {AU_CMD_TOGGLE_BLOCKING, AU_MENU_CONTEXT, "Turn Off Blocking for This Site", NOKEY,
                                AU_MENU_NONE},
    [AU_CMD_UNLOAD_TAB] = {AU_CMD_UNLOAD_TAB, AU_MENU_CONTEXT, "Unload Tab", NOKEY, AU_MENU_NONE},

    [AU_CMD_ABOUT] = {AU_CMD_ABOUT, AU_MENU_APPLICATION, "About Aura...", NOKEY, AU_MENU_NONE},

    [AU_CMD_FILE_NEW_WINDOW] = {AU_CMD_FILE_NEW_WINDOW, AU_MENU_FILE, "New Window", KEY('N'), AU_MENU_NONE},
    [AU_CMD_FILE_NEW_TAB] = {AU_CMD_FILE_NEW_TAB, AU_MENU_FILE, "New Tab", KEY('T'), AU_MENU_NONE},
    [AU_CMD_FILE_OPEN_LOCATION] = {AU_CMD_FILE_OPEN_LOCATION, AU_MENU_FILE, "Open Location...", KEY('L'),
                                   AU_MENU_NONE},
    [AU_CMD_FILE_OPEN_FILE] = {AU_CMD_FILE_OPEN_FILE, AU_MENU_FILE, "Open File...", KEY('O'), AU_MENU_NONE},
    [AU_CMD_FILE_CLOSE_TAB] = {AU_CMD_FILE_CLOSE_TAB, AU_MENU_FILE, "Close Tab", KEY('W'), AU_MENU_NONE},
    [AU_CMD_FILE_CLOSE_WINDOW] = {AU_CMD_FILE_CLOSE_WINDOW, AU_MENU_FILE, "Close Window", SHIFT_KEY('W'),
                                  AU_MENU_NONE},
    [AU_CMD_FILE_REOPEN_TAB] = {AU_CMD_FILE_REOPEN_TAB, AU_MENU_FILE, "Reopen Closed Tab", SHIFT_KEY('T'),
                                AU_MENU_NONE},
    [AU_CMD_FILE_SAVE_AS] = {AU_CMD_FILE_SAVE_AS, AU_MENU_FILE, "Save Page As...", KEY('S'), AU_MENU_NONE},
    [AU_CMD_FILE_QUIT] = {AU_CMD_FILE_QUIT, AU_MENU_FILE, "Quit", KEY('Q'), AU_MENU_NONE},

    [AU_CMD_EDIT_UNDO] = {AU_CMD_EDIT_UNDO, AU_MENU_EDIT, "Undo", KEY('Z'), AU_MENU_NONE},
    [AU_CMD_EDIT_CUT] = {AU_CMD_EDIT_CUT, AU_MENU_EDIT, "Cut", KEY('X'), AU_MENU_NONE},
    [AU_CMD_EDIT_COPY] = {AU_CMD_EDIT_COPY, AU_MENU_EDIT, "Copy", KEY('C'), AU_MENU_NONE},
    [AU_CMD_EDIT_PASTE] = {AU_CMD_EDIT_PASTE, AU_MENU_EDIT, "Paste", KEY('V'), AU_MENU_NONE},
    [AU_CMD_EDIT_CLEAR] = {AU_CMD_EDIT_CLEAR, AU_MENU_EDIT, "Clear", NOKEY, AU_MENU_NONE},
    [AU_CMD_EDIT_SELECT_ALL] = {AU_CMD_EDIT_SELECT_ALL, AU_MENU_EDIT, "Select All", KEY('A'), AU_MENU_NONE},
    [AU_CMD_EDIT_FIND] = {AU_CMD_EDIT_FIND, AU_MENU_EDIT, "Find...", KEY('F'), AU_MENU_NONE},
    [AU_CMD_EDIT_FIND_AGAIN] = {AU_CMD_EDIT_FIND_AGAIN, AU_MENU_EDIT, "Find Again", KEY('G'), AU_MENU_NONE},
    [AU_CMD_EDIT_FIND_PREVIOUS] = {AU_CMD_EDIT_FIND_PREVIOUS, AU_MENU_EDIT, "Find Previous", SHIFT_KEY('G'),
                                   AU_MENU_NONE},
    [AU_CMD_EDIT_PREFERENCES] = {AU_CMD_EDIT_PREFERENCES, AU_MENU_EDIT, "Preferences...", NOKEY, AU_MENU_NONE},

    [AU_CMD_VIEW_RELOAD] = {AU_CMD_VIEW_RELOAD, AU_MENU_VIEW, "Reload", KEY('R'), AU_MENU_NONE},
    [AU_CMD_VIEW_STOP] = {AU_CMD_VIEW_STOP, AU_MENU_VIEW, "Stop", KEY('.'), AU_MENU_NONE},
    [AU_CMD_VIEW_PAGE_SOURCE] = {AU_CMD_VIEW_PAGE_SOURCE, AU_MENU_VIEW, "Page Source", KEY('U'), AU_MENU_NONE},
    [AU_CMD_VIEW_ZOOM_IN] = {AU_CMD_VIEW_ZOOM_IN, AU_MENU_VIEW, "Zoom In", KEY('+'), AU_MENU_NONE},
    [AU_CMD_VIEW_ZOOM_OUT] = {AU_CMD_VIEW_ZOOM_OUT, AU_MENU_VIEW, "Zoom Out", KEY('-'), AU_MENU_NONE},
    [AU_CMD_VIEW_ACTUAL_SIZE] = {AU_CMD_VIEW_ACTUAL_SIZE, AU_MENU_VIEW, "Actual Size", KEY('0'), AU_MENU_NONE},
    [AU_CMD_VIEW_TABS_TOP] = {AU_CMD_VIEW_TABS_TOP, AU_MENU_VIEW, "Tabs on Top", NOKEY, AU_MENU_NONE},
    [AU_CMD_VIEW_TABS_LEFT] = {AU_CMD_VIEW_TABS_LEFT, AU_MENU_VIEW, "Tabs on Left", NOKEY, AU_MENU_NONE},

    [AU_CMD_GO_BACK] = {AU_CMD_GO_BACK, AU_MENU_GO, "Back", KEY('['), AU_MENU_NONE},
    [AU_CMD_GO_FORWARD] = {AU_CMD_GO_FORWARD, AU_MENU_GO, "Forward", KEY(']'), AU_MENU_NONE},
    [AU_CMD_GO_HOME] = {AU_CMD_GO_HOME, AU_MENU_GO, "Home", NOKEY, AU_MENU_NONE},
    [AU_CMD_GO_HISTORY] = {AU_CMD_GO_HISTORY, AU_MENU_GO, "Show History", KEY('Y'), AU_MENU_NONE},

    [AU_CMD_BOOKMARKS_ADD] = {AU_CMD_BOOKMARKS_ADD, AU_MENU_BOOKMARKS, "Add Bookmark", KEY('D'), AU_MENU_NONE},
    [AU_CMD_BOOKMARKS_SHOW] = {AU_CMD_BOOKMARKS_SHOW, AU_MENU_BOOKMARKS, "Show Bookmarks", KEY('B'), AU_MENU_NONE},

    [AU_CMD_DEV_PAGE_SOURCE] = {AU_CMD_DEV_PAGE_SOURCE, AU_MENU_DEVELOP, "Page Source", NOKEY, AU_MENU_NONE},
    [AU_CMD_DEV_CONSOLE] = {AU_CMD_DEV_CONSOLE, AU_MENU_DEVELOP, "Console", KEY('K'), AU_MENU_NONE},
    [AU_CMD_DEV_NETWORK] = {AU_CMD_DEV_NETWORK, AU_MENU_DEVELOP, "Network", KEY('E'), AU_MENU_NONE},
    [AU_CMD_DEV_PERFORMANCE] = {AU_CMD_DEV_PERFORMANCE, AU_MENU_DEVELOP, "Performance", NOKEY, AU_MENU_NONE},
    [AU_CMD_DEV_TIMING_TEST] = {AU_CMD_DEV_TIMING_TEST, AU_MENU_DEVELOP, "Run Timing Test", NOKEY, AU_MENU_NONE},
    [AU_CMD_DEV_SYSTEM_STATUS] = {AU_CMD_DEV_SYSTEM_STATUS, AU_MENU_DEVELOP, "System Status", NOKEY, AU_MENU_NONE},
    [AU_CMD_DEV_TESTS] = {AU_CMD_DEV_TESTS, AU_MENU_DEVELOP, "Tests", NOKEY, AU_MENU_TESTS},
    [AU_CMD_DEV_REPORT] = {AU_CMD_DEV_REPORT, AU_MENU_DEVELOP, "Report a Problem...", NOKEY, AU_MENU_NONE},

    [AU_CMD_TESTS_SCENE] = {AU_CMD_TESTS_SCENE, AU_MENU_TESTS, "Test Scene", NOKEY, AU_MENU_NONE},
    [AU_CMD_TESTS_FONTS] = {AU_CMD_TESTS_FONTS, AU_MENU_TESTS, "Font List", NOKEY, AU_MENU_NONE},
    [AU_CMD_TESTS_BENCHMARK] = {AU_CMD_TESTS_BENCHMARK, AU_MENU_TESTS, "Paint Benchmark", NOKEY, AU_MENU_NONE},

    [AU_CMD_WINDOW_DOWNLOADS] = {AU_CMD_WINDOW_DOWNLOADS, AU_MENU_WINDOW, "Downloads", KEY('J'), AU_MENU_NONE},
    [AU_CMD_WINDOW_COOKIES] = {AU_CMD_WINDOW_COOKIES, AU_MENU_WINDOW, "Cookies", NOKEY, AU_MENU_NONE},

    [AU_CMD_HELP_REPORT] = {AU_CMD_HELP_REPORT, AU_MENU_HELP, "Report a Problem...", NOKEY, AU_MENU_NONE},
};

#define SEP AU_CMD_SEPARATOR

static const AuCommand app_items[] = {AU_CMD_ABOUT};
static const AuCommand file_items[] = {
    AU_CMD_FILE_NEW_WINDOW, AU_CMD_FILE_NEW_TAB,      AU_CMD_FILE_OPEN_LOCATION, AU_CMD_FILE_OPEN_FILE,
    SEP,                    AU_CMD_FILE_CLOSE_TAB,    AU_CMD_FILE_CLOSE_WINDOW,  AU_CMD_FILE_REOPEN_TAB,
    SEP,                    AU_CMD_FILE_SAVE_AS,      SEP,                       AU_CMD_FILE_QUIT};
static const AuCommand edit_items[] = {
    AU_CMD_EDIT_UNDO, SEP, AU_CMD_EDIT_CUT, AU_CMD_EDIT_COPY, AU_CMD_EDIT_PASTE, AU_CMD_EDIT_CLEAR,
    AU_CMD_EDIT_SELECT_ALL, SEP, AU_CMD_EDIT_FIND, AU_CMD_EDIT_FIND_AGAIN, AU_CMD_EDIT_FIND_PREVIOUS, SEP,
    AU_CMD_EDIT_PREFERENCES};
static const AuCommand view_items[] = {
    AU_CMD_VIEW_RELOAD,  AU_CMD_VIEW_STOP,     AU_CMD_VIEW_PAGE_SOURCE, SEP, AU_CMD_VIEW_ZOOM_IN,
    AU_CMD_VIEW_ZOOM_OUT, AU_CMD_VIEW_ACTUAL_SIZE, SEP, AU_CMD_VIEW_TABS_TOP, AU_CMD_VIEW_TABS_LEFT};
static const AuCommand go_items[] = {AU_CMD_GO_BACK, AU_CMD_GO_FORWARD, AU_CMD_GO_HOME, SEP, AU_CMD_GO_HISTORY};
static const AuCommand bookmarks_items[] = {AU_CMD_BOOKMARKS_ADD, AU_CMD_BOOKMARKS_SHOW, SEP};
/* Develop, in groups: looking at the page, measuring it, the engine and
 * the machine, test content, and Report a Problem (also in Help). */
static const AuCommand develop_items[] = {
    AU_CMD_DEV_PAGE_SOURCE,   AU_CMD_DEV_CONSOLE, AU_CMD_DEV_NETWORK, SEP, AU_CMD_DEV_PERFORMANCE,
    AU_CMD_DEV_TIMING_TEST,   SEP,                AU_CMD_DEV_SYSTEM_STATUS, SEP, AU_CMD_DEV_TESTS,
    SEP,                      AU_CMD_DEV_REPORT};
static const AuCommand tests_items[] = {AU_CMD_TESTS_SCENE, AU_CMD_TESTS_FONTS, SEP, AU_CMD_TESTS_BENCHMARK};
static const AuCommand window_items[] = {AU_CMD_WINDOW_DOWNLOADS, AU_CMD_WINDOW_COOKIES};
static const AuCommand help_items[] = {AU_CMD_HELP_REPORT};

#define ITEMS(a) a, (int)(sizeof a / sizeof a[0])

static const AuMenuInfo menus[AU_MENU_COUNT] = {
    [AU_MENU_APPLICATION] = {AU_MENU_APPLICATION, "Aura", ITEMS(app_items), AU_MENU_LIST_NONE},
    [AU_MENU_FILE] = {AU_MENU_FILE, "File", ITEMS(file_items), AU_MENU_LIST_NONE},
    [AU_MENU_EDIT] = {AU_MENU_EDIT, "Edit", ITEMS(edit_items), AU_MENU_LIST_NONE},
    [AU_MENU_VIEW] = {AU_MENU_VIEW, "View", ITEMS(view_items), AU_MENU_LIST_NONE},
    [AU_MENU_GO] = {AU_MENU_GO, "Go", ITEMS(go_items), AU_MENU_LIST_HISTORY},
    [AU_MENU_BOOKMARKS] = {AU_MENU_BOOKMARKS, "Bookmarks", ITEMS(bookmarks_items), AU_MENU_LIST_BOOKMARKS},
    [AU_MENU_DEVELOP] = {AU_MENU_DEVELOP, "Develop", ITEMS(develop_items), AU_MENU_LIST_NONE},
    [AU_MENU_TESTS] = {AU_MENU_TESTS, "Tests", ITEMS(tests_items), AU_MENU_LIST_NONE},
    [AU_MENU_WINDOW] = {AU_MENU_WINDOW, "Window", ITEMS(window_items), AU_MENU_LIST_WINDOWS},
    [AU_MENU_HELP] = {AU_MENU_HELP, "Help", ITEMS(help_items), AU_MENU_LIST_NONE},
    [AU_MENU_CONTEXT] = {AU_MENU_CONTEXT, "", NULL, 0, AU_MENU_LIST_NONE},
};

static const AuMenuId menu_bar[] = {AU_MENU_APPLICATION, AU_MENU_FILE,      AU_MENU_EDIT,    AU_MENU_VIEW,
                                    AU_MENU_GO,          AU_MENU_BOOKMARKS, AU_MENU_DEVELOP, AU_MENU_WINDOW};

const AuCommandInfo *au_command_info(AuCommand cmd)
{
    if ((int)cmd <= (int)AU_CMD_SEPARATOR || (int)cmd >= (int)AU_CMD_COUNT)
        return NULL;
    return &commands[cmd];
}

const AuMenuInfo *au_menu_info(AuMenuId id)
{
    if ((int)id <= (int)AU_MENU_NONE || (int)id >= (int)AU_MENU_COUNT)
        return NULL;
    return &menus[id];
}

const AuMenuId *au_menu_bar(int *n)
{
    *n = (int)(sizeof menu_bar / sizeof menu_bar[0]);
    return menu_bar;
}

AuCommand au_command_for_shortcut(char key, unsigned mods)
{
    if (key >= 'a' && key <= 'z')
        key = (char)(key - 'a' + 'A');
    if (!key)
        return AU_CMD_NONE;
    for (int c = AU_CMD_SEPARATOR + 1; c < AU_CMD_COUNT; c++)
        if (commands[c].shortcut.key == key && commands[c].shortcut.mods == mods)
            return (AuCommand)c;
    return AU_CMD_NONE;
}

AuCommand au_command_for_key(const AuCommandState *st, char key, unsigned mods)
{
    AuCommand c = au_command_for_shortcut(key, mods);
    /* Close Tab's shortcut closes a window without tabs. */
    if (c == AU_CMD_FILE_CLOSE_TAB && !au_command_enabled(st, c))
        c = AU_CMD_FILE_CLOSE_WINDOW;
    return c;
}

/* ---- Enabled, checked, title ---- */

static AuTab *active_tab(const AuCommandState *st)
{
    return st->window ? au_bw_active_tab(st->window) : NULL;
}

bool au_command_enabled(const AuCommandState *st, AuCommand cmd)
{
    AuBrowserWindow *w = st->window;
    AuTab *t = active_tab(st);
    bool browser_front = w && st->front == AU_FRONT_BROWSER;
    switch (cmd) {
    case AU_CMD_FILE_CLOSE_TAB: return browser_front;
    case AU_CMD_FILE_CLOSE_WINDOW: return st->front != AU_FRONT_NONE;
    case AU_CMD_FILE_REOPEN_TAB: return au_browser_can_reopen(st->browser);
    case AU_CMD_FILE_SAVE_AS: return w && au_bw_has_page_source(w);
    /* Undo, Cut, Paste and Clear need a text field; in a page Copy needs
     * a selection and Select All a tab. */
    case AU_CMD_EDIT_UNDO:
    case AU_CMD_EDIT_CUT:
    case AU_CMD_EDIT_PASTE:
    case AU_CMD_EDIT_CLEAR: return st->text_focus;
    case AU_CMD_EDIT_COPY: return st->text_focus || (browser_front && t && au_bw_has_selection(w));
    case AU_CMD_EDIT_SELECT_ALL: return st->text_focus || (browser_front && t);
    case AU_CMD_EDIT_FIND: return t || st->front == AU_FRONT_TOOL;
    case AU_CMD_EDIT_FIND_AGAIN:
    case AU_CMD_EDIT_FIND_PREVIOUS:
        if (st->front == AU_FRONT_TOOL)
            return st->tool_can_find_again;
        return t && st->browser->find_text;
    case AU_CMD_VIEW_RELOAD: return t != NULL;
    case AU_CMD_VIEW_STOP: return t && t->loading;
    case AU_CMD_VIEW_PAGE_SOURCE:
    case AU_CMD_DEV_PAGE_SOURCE: return w && au_bw_can_view_source(w);
    case AU_CMD_VIEW_ZOOM_IN: return w && au_bw_can_zoom(w, 1);
    case AU_CMD_VIEW_ZOOM_OUT: return w && au_bw_can_zoom(w, -1);
    case AU_CMD_VIEW_ACTUAL_SIZE: return w && au_bw_can_zoom(w, 0);
    case AU_CMD_GO_BACK: return t && au_nav_can_go(&t->nav, -1);
    case AU_CMD_GO_FORWARD: return t && au_nav_can_go(&t->nav, 1);
    case AU_CMD_BOOKMARKS_ADD: return t != NULL;
    /* The windows that show a page need one; the benchmark needs a
     * browser window to paint in. */
    case AU_CMD_DEV_CONSOLE:
    case AU_CMD_DEV_NETWORK:
    case AU_CMD_DEV_PERFORMANCE:
    case AU_CMD_DEV_TIMING_TEST: return t != NULL;
    case AU_CMD_TESTS_BENCHMARK: return w != NULL;
    case AU_CMD_NONE:
    case AU_CMD_SEPARATOR:
    case AU_CMD_COUNT: return false;
    default: return true;
    }
}

bool au_command_checked(const AuCommandState *st, AuCommand cmd)
{
    switch (cmd) {
    case AU_CMD_VIEW_TABS_TOP: return st->browser->prefs.tab_position == AU_TABS_TOP;
    case AU_CMD_VIEW_TABS_LEFT: return st->browser->prefs.tab_position == AU_TABS_LEFT;
    default: return false;
    }
}

const char *au_command_title(const AuCommandState *st, AuCommand cmd)
{
    const AuCommandInfo *ci = au_command_info(cmd);
    if (!ci)
        return "";
    if (cmd == AU_CMD_TOGGLE_BLOCKING && st && st->window) {
        bool off;
        if (au_bw_site_blocking(st->window, &off) && off)
            return "Turn On Blocking for This Site";
    }
    return ci->title;
}

/* ---- Running ---- */

static bool show(const AuCommandHost *h, AuWindowKind kind)
{
    return h->show_window && h->show_window(h->ctx, kind);
}

static AuBrowserWindow *new_window(const AuCommandHost *h, const char *url)
{
    return h->new_window ? h->new_window(h->ctx, url) : NULL;
}

static void edit(const AuCommandHost *h, AuEditAction a)
{
    if (h->edit)
        h->edit(h->ctx, a);
}

void au_command_open_url(const AuCommandState *st, const AuCommandHost *host, const char *url, bool new_tab)
{
    AuBrowserWindow *w = st->window;
    if (!w) {
        new_window(host, url);
        return;
    }
    if (new_tab)
        au_bw_new_tab(w, url, true, w->ntabs);
    else
        au_bw_navigate(w, url);
    if (host->raise)
        host->raise(host->ctx, w);
}

/* Page Source: the port's window, or a view-source: tab without one. */
static void page_source(const AuCommandState *st, const AuCommandHost *host)
{
    if (!show(host, AU_WINDOW_PAGE_SOURCE) && st->window)
        au_bw_view_source(st->window);
}

void au_command_run(const AuCommandState *st, const AuCommandHost *host, AuCommand cmd)
{
    if (!au_command_enabled(st, cmd))
        return;
    AuBrowserWindow *w = st->window;
    switch (cmd) {
    case AU_CMD_ABOUT: show(host, AU_WINDOW_ABOUT); break;
    case AU_CMD_FILE_NEW_WINDOW: new_window(host, NULL); break;
    case AU_CMD_FILE_NEW_TAB:
        if (w)
            au_bw_new_tab(w, NULL, true, w->ntabs);
        else
            w = new_window(host, "about:blank");
        if (w && host->focus_address)
            host->focus_address(host->ctx, w);
        break;
    case AU_CMD_FILE_OPEN_LOCATION:
        if (!w)
            w = new_window(host, "about:blank");
        if (w && host->raise)
            host->raise(host->ctx, w);
        if (w && host->focus_address)
            host->focus_address(host->ctx, w);
        break;
    case AU_CMD_FILE_OPEN_FILE: {
        char *url = host->choose_file_url ? host->choose_file_url(host->ctx) : NULL;
        if (url) {
            au_command_open_url(st, host, url, false);
            au_free(url);
        }
        break;
    }
    case AU_CMD_FILE_CLOSE_TAB:
        if (w && st->front == AU_FRONT_BROWSER)
            au_bw_close_tab(w, w->active);
        else if (host->close_front)
            host->close_front(host->ctx);
        break;
    case AU_CMD_FILE_CLOSE_WINDOW:
        if (host->close_front)
            host->close_front(host->ctx);
        break;
    case AU_CMD_FILE_REOPEN_TAB:
        if (!w)
            w = new_window(host, "about:blank");
        if (w)
            au_bw_reopen_closed_tab(w);
        break;
    case AU_CMD_FILE_SAVE_AS:
        if (host->save_page)
            host->save_page(host->ctx, w);
        break;
    case AU_CMD_FILE_QUIT:
        if (host->quit)
            host->quit(host->ctx);
        break;
    case AU_CMD_EDIT_UNDO: edit(host, AU_EDIT_UNDO); break;
    case AU_CMD_EDIT_CUT: edit(host, AU_EDIT_CUT); break;
    case AU_CMD_EDIT_COPY: edit(host, AU_EDIT_COPY); break;
    case AU_CMD_EDIT_PASTE: edit(host, AU_EDIT_PASTE); break;
    case AU_CMD_EDIT_CLEAR: edit(host, AU_EDIT_CLEAR); break;
    case AU_CMD_EDIT_SELECT_ALL: edit(host, AU_EDIT_SELECT_ALL); break;
    case AU_CMD_EDIT_FIND:
        /* Find in a developer window searches its text. */
        if (host->find && (st->front == AU_FRONT_TOOL || w))
            host->find(host->ctx, st->front == AU_FRONT_TOOL ? NULL : w);
        break;
    case AU_CMD_EDIT_FIND_AGAIN:
    case AU_CMD_EDIT_FIND_PREVIOUS: {
        bool forward = cmd == AU_CMD_EDIT_FIND_AGAIN;
        if (st->front == AU_FRONT_TOOL) {
            if (host->tool_find_again)
                host->tool_find_again(host->ctx, forward);
        } else if (w && !au_bw_find_again(w, forward) && host->beep) {
            host->beep(host->ctx);
        }
        break;
    }
    case AU_CMD_EDIT_PREFERENCES: show(host, AU_WINDOW_PREFERENCES); break;
    case AU_CMD_VIEW_RELOAD:
        /* Shift or Option: reload from the network, past the cache. */
        if (st->mods & (AU_MOD_SHIFT | AU_MOD_OPTION))
            au_bw_reload_bypass(w);
        else
            au_bw_reload(w);
        break;
    case AU_CMD_VIEW_STOP: au_bw_stop(w); break;
    case AU_CMD_VIEW_PAGE_SOURCE:
    case AU_CMD_DEV_PAGE_SOURCE: page_source(st, host); break;
    case AU_CMD_VIEW_ZOOM_IN: au_bw_zoom(w, 1); break;
    case AU_CMD_VIEW_ZOOM_OUT: au_bw_zoom(w, -1); break;
    case AU_CMD_VIEW_ACTUAL_SIZE: au_bw_zoom(w, 0); break;
    case AU_CMD_VIEW_TABS_TOP:
    case AU_CMD_VIEW_TABS_LEFT:
        st->browser->prefs.tab_position = cmd == AU_CMD_VIEW_TABS_LEFT ? AU_TABS_LEFT : AU_TABS_TOP;
        au_browser_prefs_changed(st->browser);
        break;
    case AU_CMD_GO_BACK: au_bw_back(w); break;
    case AU_CMD_GO_FORWARD: au_bw_forward(w); break;
    case AU_CMD_GO_HOME:
        if (w)
            au_bw_home(w);
        else
            new_window(host, NULL);
        break;
    case AU_CMD_GO_HISTORY: show(host, AU_WINDOW_HISTORY); break;
    case AU_CMD_BOOKMARKS_ADD: au_bw_bookmark_page(w, false); break;
    case AU_CMD_BOOKMARKS_SHOW: show(host, AU_WINDOW_BOOKMARKS); break;
    case AU_CMD_DEV_CONSOLE: show(host, AU_WINDOW_CONSOLE); break;
    case AU_CMD_DEV_NETWORK: show(host, AU_WINDOW_NETWORK); break;
    case AU_CMD_DEV_PERFORMANCE: show(host, AU_WINDOW_PERFORMANCE); break;
    case AU_CMD_DEV_TIMING_TEST: show(host, AU_WINDOW_TIMING_TEST); break;
    case AU_CMD_DEV_SYSTEM_STATUS: show(host, AU_WINDOW_SYSTEM_STATUS); break;
    case AU_CMD_DEV_REPORT:
    case AU_CMD_HELP_REPORT: show(host, AU_WINDOW_REPORT); break;
    case AU_CMD_TESTS_SCENE: au_command_open_url(st, host, "about:scene", false); break;
    case AU_CMD_TESTS_FONTS: au_command_open_url(st, host, "about:fonts", false); break;
    case AU_CMD_TESTS_BENCHMARK: show(host, AU_WINDOW_PAINT_BENCHMARK); break;
    case AU_CMD_WINDOW_DOWNLOADS: show(host, AU_WINDOW_DOWNLOADS); break;
    case AU_CMD_WINDOW_COOKIES: show(host, AU_WINDOW_COOKIES); break;
    default: break; /* AU_CMD_DEV_TESTS opens a submenu; contextual commands run elsewhere */
    }
}

/* ---- Dynamic menus ---- */

static AuMenuEntry *list_add(AuMenuList *l, AuMenuEntryKind kind, const char *title, int parent)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->e = au_realloc(l->e, (size_t)l->cap * sizeof *l->e);
    }
    AuMenuEntry *e = &l->e[l->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->title = au_strndup(title, strlen(title));
    e->parent = parent;
    e->enabled = kind != AU_ENTRY_SEPARATOR;
    return e;
}

void au_menu_list_free(AuMenuList *l)
{
    for (int i = 0; i < l->n; i++) {
        au_free(l->e[i].title);
        au_free(l->e[i].url);
    }
    au_free(l->e);
    memset(l, 0, sizeof *l);
}

void au_menu_list_history(AuBrowser *b, AuMenuList *out)
{
    AuHistory *h = &b->history;
    if (!h->n)
        return;
    list_add(out, AU_ENTRY_SEPARATOR, "-", -1);
    char t[64];
    for (size_t i = 0; i < h->n && i < AU_GO_MENU_ENTRIES; i++) {
        au_menu_title(h->e[i].title, h->e[i].url, t, sizeof t);
        AuMenuEntry *e = list_add(out, AU_ENTRY_ITEM, t, -1);
        e->url = au_strndup(h->e[i].url, strlen(h->e[i].url));
    }
}

void au_menu_list_bookmarks(AuBrowser *b, AuMenuList *out)
{
    /* Breadth-first without recursion: the folder each listed folder
     * entry stands for is kept beside the list, and the scan over the
     * list appends each folder's contents when it reaches the folder. */
    AuBookmark **nodes = NULL;
    int ncap = 0;
    int start = out->n;
    AuBookmark *folder = &b->bookmarks.root;
    int parent = -1;
    int scan = start;
    for (;;) {
        if (!folder->first) {
            if (parent >= 0)
                list_add(out, AU_ENTRY_ITEM, "(empty)", parent)->enabled = false;
        }
        for (AuBookmark *bk = folder->first; bk; bk = bk->next) {
            char t[64];
            au_menu_title(bk->title, bk->url, t, sizeof t);
            AuMenuEntry *e = list_add(out, bk->folder ? AU_ENTRY_FOLDER : AU_ENTRY_ITEM, t, parent);
            e->bookmark = bk->id;
            int k = out->n - 1 - start;
            if (k >= ncap) {
                ncap = ncap ? ncap * 2 : 32;
                while (ncap <= k)
                    ncap *= 2;
                nodes = au_realloc(nodes, (size_t)ncap * sizeof *nodes);
            }
            nodes[k] = bk;
        }
        /* The next folder entry not yet expanded. */
        while (scan < out->n && out->e[scan].kind != AU_ENTRY_FOLDER)
            scan++;
        if (scan >= out->n)
            break;
        folder = nodes[scan - start];
        parent = scan;
        scan++;
    }
    au_free(nodes);
}

void au_menu_list_windows(const AuWindowRef *wins, int n, void *front, AuMenuList *out)
{
    if (n <= 0)
        return;
    list_add(out, AU_ENTRY_SEPARATOR, "-", -1);
    for (int i = 0; i < n; i++) {
        const char *t = wins[i].title && *wins[i].title ? wins[i].title : " ";
        AuMenuEntry *e = list_add(out, AU_ENTRY_ITEM, t, -1);
        e->window = wins[i].window;
        e->checked = wins[i].window == front;
    }
}

void au_menu_entry_run(const AuCommandState *st, const AuCommandHost *host, const AuMenuEntry *e)
{
    if (e->kind != AU_ENTRY_ITEM || !e->enabled)
        return;
    if (e->window) {
        if (host->select_window)
            host->select_window(host->ctx, e->window);
    } else if (e->url) {
        char *u = au_strndup(e->url, strlen(e->url));
        au_command_open_url(st, host, u, false);
        au_free(u);
    } else if (e->bookmark) {
        /* Looked up now: the bookmark may have changed since the menu
         * was built. */
        AuBookmark *bk = au_bookmark_find_id(&st->browser->bookmarks, e->bookmark);
        if (bk && !bk->folder) {
            char *u = au_strndup(bk->url, strlen(bk->url));
            au_command_open_url(st, host, u, false);
            au_free(u);
        }
    }
}

/* ---- Contextual menus ---- */

void au_command_run_context(const AuCommandState *st, const AuCommandHost *host, const AuContextMenu *m,
                            AuCommand cmd)
{
    AuBrowserWindow *w = st->window;
    if (!w)
        return;
    if (cmd == AU_CMD_VIEW_SOURCE) {
        /* The Page Source window, as View > Page Source. */
        page_source(st, host);
        return;
    }
    if (au_bw_context_command(w, m, cmd))
        return;
    switch (cmd) {
    case AU_CMD_OPEN_LINK_WINDOW:
        if (m->link)
            new_window(host, m->link);
        break;
    case AU_CMD_COPY_LINK:
        if (m->link && host->copy_text)
            host->copy_text(host->ctx, m->link, strlen(m->link));
        break;
    case AU_CMD_COPY_IMAGE:
        if (m->image && host->copy_text)
            host->copy_text(host->ctx, m->image, strlen(m->image));
        break;
    case AU_CMD_COPY:
    case AU_CMD_FIELD_COPY: edit(host, AU_EDIT_COPY); break;
    case AU_CMD_CUT: edit(host, AU_EDIT_CUT); break;
    case AU_CMD_PASTE: edit(host, AU_EDIT_PASTE); break;
    case AU_CMD_SELECT_ALL: edit(host, AU_EDIT_SELECT_ALL); break;
    case AU_CMD_PASTE_GO: edit(host, AU_EDIT_PASTE_AND_GO); break;
    default: break;
    }
}
