/* Tests for the command table (src/browser/commands.c): titles, menus
 * and shortcuts, enable and check rules in representative states, what
 * running commands does to the model, the commands the port's
 * AuCommandHost handles, and the dynamic menu listings. Exits non-zero
 * on the first failure. */
#include "browser/commands.h"
#include "browser/context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define CHECK_STR(a, b) \
    do { \
        const char *a_ = (a), *b_ = (b); \
        if (strcmp(a_, b_) != 0) { \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); \
            exit(1); \
        } \
    } while (0)

/* ---- A browser on the stub page ---- */

static uint32_t fake_ms;
static uint32_t t_ms(void *ctx) { (void)ctx; return fake_ms; }
static AuTime t_now(void *ctx) { (void)ctx; return 1763100000; }
static void t_inval(void *ctx, AuBrowserWindow *w, AuIRect r) { (void)ctx, (void)w, (void)r; }
static void t_changed(void *ctx, AuBrowserWindow *w, unsigned what) { (void)ctx, (void)w, (void)what; }
static void t_close(void *ctx, AuBrowserWindow *w) { (void)ctx, (void)w; }

static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    if (strcmp(url, "file:///doc.html") != 0)
        return false;
    static const char html[] = "<!doctype html><title>Doc</title><p>Some words to find here.";
    *data = au_strndup(html, sizeof html - 1);
    *len = sizeof html - 1;
    return true;
}

static AuFontSet *fonts;

static AuBrowser *make_browser(void)
{
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.time_ms = t_ms;
    h.now = t_now;
    h.read_file_url = t_read;
    h.invalidate = t_inval;
    h.changed = t_changed;
    h.close_window = t_close;
    return au_browser_new(fonts, &h);
}

static void settle(AuBrowserWindow *w)
{
    for (int i = 0; i < 100 && au_browser_busy(w->browser); i++) {
        fake_ms += 100;
        au_bw_idle(w);
    }
}

/* ---- A recording AuCommandHost ---- */

typedef struct Rec {
    AuBrowser *browser;
    AuBrowserWindow *made[4]; /* windows new_window made */
    int nmade;
    char new_url[128];
    int shown[AU_WINDOW_PAINT_BENCHMARK + 1];
    int focus, raised, choose, saved, closed, find, find_tool, tool_again, beeps, quits, selects;
    int edits[AU_EDIT_PASTE_AND_GO + 1];
    char copied[128];
    void *selected;
    const char *file_url; /* what choose_file_url returns */
    bool no_source_window; /* show_window(PAGE_SOURCE) returns false */
} Rec;

static AuBrowserWindow *h_new_window(void *ctx, const char *url)
{
    Rec *r = ctx;
    snprintf(r->new_url, sizeof r->new_url, "%s", url ? url : "(home)");
    if (r->nmade >= 4)
        return NULL;
    AuBrowserWindow *w = au_browser_window_new(r->browser, 640, 480);
    au_bw_new_tab(w, url ? url : "about:home", true, -1);
    r->made[r->nmade++] = w;
    return w;
}
static void h_raise(void *ctx, AuBrowserWindow *w) { (void)w; ((Rec *)ctx)->raised++; }
static void h_focus(void *ctx, AuBrowserWindow *w) { (void)w; ((Rec *)ctx)->focus++; }
static bool h_show(void *ctx, AuWindowKind k)
{
    Rec *r = ctx;
    if (k == AU_WINDOW_PAGE_SOURCE && r->no_source_window)
        return false;
    r->shown[k]++;
    return true;
}
static char *h_choose(void *ctx)
{
    Rec *r = ctx;
    r->choose++;
    return r->file_url ? au_strndup(r->file_url, strlen(r->file_url)) : NULL;
}
static void h_save(void *ctx, AuBrowserWindow *w) { (void)w; ((Rec *)ctx)->saved++; }
static void h_close(void *ctx) { ((Rec *)ctx)->closed++; }
static void h_find(void *ctx, AuBrowserWindow *w)
{
    if (w)
        ((Rec *)ctx)->find++;
    else
        ((Rec *)ctx)->find_tool++;
}
static void h_again(void *ctx, bool fwd) { (void)fwd; ((Rec *)ctx)->tool_again++; }
static void h_edit(void *ctx, AuEditAction a) { ((Rec *)ctx)->edits[a]++; }
static void h_copy(void *ctx, const char *s, size_t n)
{
    Rec *r = ctx;
    snprintf(r->copied, sizeof r->copied, "%.*s", (int)n, s);
}
static void h_select(void *ctx, void *w)
{
    ((Rec *)ctx)->selects++;
    ((Rec *)ctx)->selected = w;
}
static void h_beep(void *ctx) { ((Rec *)ctx)->beeps++; }
static void h_quit(void *ctx) { ((Rec *)ctx)->quits++; }

static AuCommandHost make_host(Rec *r)
{
    AuCommandHost h = {r,      h_new_window, h_raise, h_focus,  h_show, h_choose, h_save, h_close,
                       h_find, h_again,      h_edit,  h_copy,   h_select, h_beep, h_quit};
    return h;
}

static void free_made(Rec *r)
{
    for (int i = 0; i < r->nmade; i++)
        au_browser_window_free(r->made[i]);
    r->nmade = 0;
}

/* ---- The table ---- */

static void test_table(void)
{
    /* Every command has a title and a menu; menu bar commands are listed
     * in their menu exactly once, and nowhere else. */
    for (int c = AU_CMD_SEPARATOR + 1; c < AU_CMD_COUNT; c++) {
        const AuCommandInfo *ci = au_command_info((AuCommand)c);
        CHECK(ci && ci->cmd == (AuCommand)c);
        CHECK(ci->title && *ci->title);
        CHECK(ci->menu > AU_MENU_NONE && ci->menu < AU_MENU_COUNT);
        int seen = 0;
        for (int m = AU_MENU_NONE + 1; m < AU_MENU_COUNT; m++) {
            const AuMenuInfo *mi = au_menu_info((AuMenuId)m);
            for (int i = 0; i < mi->nitems; i++)
                if (mi->items[i] == (AuCommand)c) {
                    CHECK(m == (int)ci->menu);
                    seen++;
                }
        }
        CHECK(seen == (ci->menu == AU_MENU_CONTEXT ? 0 : 1));
        if (ci->shortcut.key) {
            CHECK(ci->shortcut.mods & AU_SHORTCUT_PRIMARY);
            CHECK(!(ci->shortcut.key >= 'a' && ci->shortcut.key <= 'z'));
        }
    }
    CHECK(au_command_info(AU_CMD_NONE) == NULL && au_command_info(AU_CMD_SEPARATOR) == NULL);
    CHECK(au_command_info(AU_CMD_COUNT) == NULL);

    /* Shortcuts are unique per key and modifier set. */
    for (int a = AU_CMD_SEPARATOR + 1; a < AU_CMD_COUNT; a++) {
        const AuShortcut *sa = &au_command_info((AuCommand)a)->shortcut;
        if (!sa->key)
            continue;
        for (int b = a + 1; b < AU_CMD_COUNT; b++) {
            const AuShortcut *sb = &au_command_info((AuCommand)b)->shortcut;
            CHECK(!(sb->key == sa->key && sb->mods == sa->mods));
        }
        CHECK(au_command_for_shortcut(sa->key, sa->mods) == (AuCommand)a);
    }
    CHECK(au_command_for_shortcut('n', AU_SHORTCUT_PRIMARY) == AU_CMD_FILE_NEW_WINDOW);
    CHECK(au_command_for_shortcut('T', AU_SHORTCUT_PRIMARY | AU_SHORTCUT_SHIFT) == AU_CMD_FILE_REOPEN_TAB);
    CHECK(au_command_for_shortcut('G', AU_SHORTCUT_PRIMARY | AU_SHORTCUT_SHIFT) == AU_CMD_EDIT_FIND_PREVIOUS);
    CHECK(au_command_for_shortcut('N', AU_SHORTCUT_PRIMARY | AU_SHORTCUT_SHIFT) == AU_CMD_NONE);
    CHECK(au_command_for_shortcut('N', 0) == AU_CMD_NONE);
    CHECK(au_command_for_shortcut(0, AU_SHORTCUT_PRIMARY) == AU_CMD_NONE);

    /* The menu bar and the Mac OS 9 order of items. */
    int n;
    const AuMenuId *bar = au_menu_bar(&n);
    CHECK(n == 8);
    static const char *const titles[] = {"Aura", "File", "Edit", "View", "Go", "Bookmarks", "Develop", "Window"};
    for (int i = 0; i < n; i++)
        CHECK_STR(au_menu_info(bar[i])->title, titles[i]);
    const AuMenuInfo *file = au_menu_info(AU_MENU_FILE);
    CHECK(file->nitems == 12 && file->items[4] == AU_CMD_SEPARATOR && file->items[5] == AU_CMD_FILE_CLOSE_TAB &&
          file->items[11] == AU_CMD_FILE_QUIT);
    const AuMenuInfo *dev = au_menu_info(AU_MENU_DEVELOP);
    CHECK(dev->nitems == 12 && dev->items[9] == AU_CMD_DEV_TESTS);
    CHECK(au_command_info(AU_CMD_DEV_TESTS)->submenu == AU_MENU_TESTS);
    CHECK(au_menu_info(AU_MENU_GO)->list == AU_MENU_LIST_HISTORY);
    CHECK(au_menu_info(AU_MENU_BOOKMARKS)->list == AU_MENU_LIST_BOOKMARKS);
    CHECK(au_menu_info(AU_MENU_WINDOW)->list == AU_MENU_LIST_WINDOWS);
    CHECK_STR(au_command_info(AU_CMD_FILE_OPEN_LOCATION)->title, "Open Location...");
    CHECK(au_command_info(AU_CMD_VIEW_STOP)->shortcut.key == '.');
}

/* ---- Enable rules ---- */

static void test_enabled(void)
{
    AuBrowser *b = make_browser();
    AuCommandState st;
    memset(&st, 0, sizeof st);
    st.browser = b;

    /* No window at all. */
    CHECK(au_command_enabled(&st, AU_CMD_FILE_NEW_WINDOW));
    CHECK(au_command_enabled(&st, AU_CMD_FILE_QUIT));
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_PREFERENCES));
    CHECK(au_command_enabled(&st, AU_CMD_GO_HOME));
    CHECK(au_command_enabled(&st, AU_CMD_DEV_SYSTEM_STATUS));
    CHECK(!au_command_enabled(&st, AU_CMD_FILE_CLOSE_TAB));
    CHECK(!au_command_enabled(&st, AU_CMD_FILE_CLOSE_WINDOW));
    CHECK(!au_command_enabled(&st, AU_CMD_FILE_REOPEN_TAB));
    CHECK(!au_command_enabled(&st, AU_CMD_FILE_SAVE_AS));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_COPY));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_FIND));
    CHECK(!au_command_enabled(&st, AU_CMD_VIEW_RELOAD));
    CHECK(!au_command_enabled(&st, AU_CMD_GO_BACK));
    CHECK(!au_command_enabled(&st, AU_CMD_BOOKMARKS_ADD));
    CHECK(!au_command_enabled(&st, AU_CMD_DEV_CONSOLE));
    CHECK(!au_command_enabled(&st, AU_CMD_TESTS_BENCHMARK));
    CHECK(!au_command_enabled(&st, AU_CMD_SEPARATOR));
    CHECK(au_command_checked(&st, AU_CMD_VIEW_TABS_TOP) != au_command_checked(&st, AU_CMD_VIEW_TABS_LEFT));

    /* A window with a page in front. */
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///doc.html", true, -1);
    settle(w);
    st.window = w;
    st.front = AU_FRONT_BROWSER;
    CHECK(au_command_enabled(&st, AU_CMD_FILE_CLOSE_TAB));
    CHECK(au_command_enabled(&st, AU_CMD_FILE_CLOSE_WINDOW));
    CHECK(au_command_enabled(&st, AU_CMD_FILE_SAVE_AS));
    CHECK(au_command_enabled(&st, AU_CMD_VIEW_RELOAD));
    CHECK(!au_command_enabled(&st, AU_CMD_VIEW_STOP));
    CHECK(au_command_enabled(&st, AU_CMD_VIEW_PAGE_SOURCE));
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_FIND));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_FIND_AGAIN)); /* nothing found yet */
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_SELECT_ALL));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_COPY)); /* no selection */
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_PASTE));
    CHECK(au_command_enabled(&st, AU_CMD_BOOKMARKS_ADD));
    CHECK(au_command_enabled(&st, AU_CMD_DEV_CONSOLE));
    CHECK(au_command_enabled(&st, AU_CMD_TESTS_BENCHMARK));
    /* No Back history: Back is off, and Forward. */
    CHECK(!au_command_enabled(&st, AU_CMD_GO_BACK));
    CHECK(!au_command_enabled(&st, AU_CMD_GO_FORWARD));
    au_bw_navigate(w, "about:blank");
    settle(w);
    CHECK(au_command_enabled(&st, AU_CMD_GO_BACK));
    CHECK(!au_command_enabled(&st, AU_CMD_GO_FORWARD));
    au_bw_back(w);
    settle(w);
    CHECK(!au_command_enabled(&st, AU_CMD_GO_BACK) && au_command_enabled(&st, AU_CMD_GO_FORWARD));
    /* A page loading can be stopped. */
    au_bw_navigate(w, "http://slow.example/");
    CHECK(au_command_enabled(&st, AU_CMD_VIEW_STOP));
    au_bw_stop(w);
    au_bw_back(w);
    settle(w);

    /* Text selected in the page: Copy; Find Again after a find. */
    CHECK(au_bw_find(w, "words", false));
    CHECK(au_bw_has_selection(w));
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_COPY));
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_FIND_AGAIN) && au_command_enabled(&st, AU_CMD_EDIT_FIND_PREVIOUS));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_CUT)); /* not in a text field */
    /* The same window behind another window: Close Tab and Copy go. */
    st.front = AU_FRONT_OTHER;
    CHECK(!au_command_enabled(&st, AU_CMD_FILE_CLOSE_TAB));
    CHECK(au_command_enabled(&st, AU_CMD_FILE_CLOSE_WINDOW));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_COPY));
    CHECK(au_command_for_key(&st, 'w', AU_SHORTCUT_PRIMARY) == AU_CMD_FILE_CLOSE_WINDOW);
    /* A list with a selected row in front: Copy only. */
    st.other_can_copy = true;
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_COPY));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_CUT) && !au_command_enabled(&st, AU_CMD_EDIT_PASTE) &&
          !au_command_enabled(&st, AU_CMD_EDIT_SELECT_ALL));
    st.other_can_copy = false;
    st.front = AU_FRONT_BROWSER;
    CHECK(au_command_for_key(&st, 'w', AU_SHORTCUT_PRIMARY) == AU_CMD_FILE_CLOSE_TAB);
    /* A text field has the focus: all of the Edit menu. */
    st.text_focus = true;
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_UNDO) && au_command_enabled(&st, AU_CMD_EDIT_CUT) &&
          au_command_enabled(&st, AU_CMD_EDIT_PASTE) && au_command_enabled(&st, AU_CMD_EDIT_CLEAR));
    st.text_focus = false;
    /* A developer window in front finds in its own text. */
    st.front = AU_FRONT_TOOL;
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_FIND));
    CHECK(!au_command_enabled(&st, AU_CMD_EDIT_FIND_AGAIN));
    st.tool_can_find_again = true;
    CHECK(au_command_enabled(&st, AU_CMD_EDIT_FIND_AGAIN));

    /* Tabs on Top and Tabs on Left follow the preference. */
    b->prefs.tab_position = AU_TABS_LEFT;
    CHECK(au_command_checked(&st, AU_CMD_VIEW_TABS_LEFT) && !au_command_checked(&st, AU_CMD_VIEW_TABS_TOP));
    CHECK(!au_command_checked(&st, AU_CMD_FILE_NEW_WINDOW));

    au_browser_window_free(w);
    au_browser_free(b);
}

/* ---- Running ---- */

static void test_run(void)
{
    AuBrowser *b = make_browser();
    Rec r;
    memset(&r, 0, sizeof r);
    r.browser = b;
    AuCommandHost host = make_host(&r);
    AuCommandState st;
    memset(&st, 0, sizeof st);
    st.browser = b;

    /* With no window, New Tab and Home make one. */
    au_command_run(&st, &host, AU_CMD_FILE_NEW_TAB);
    CHECK(r.nmade == 1 && r.focus == 1);
    CHECK_STR(r.new_url, "about:blank");
    au_command_run(&st, &host, AU_CMD_GO_HOME);
    CHECK(r.nmade == 2);
    CHECK_STR(r.new_url, "(home)");
    free_made(&r);
    /* Disabled commands do nothing. */
    au_command_run(&st, &host, AU_CMD_FILE_CLOSE_WINDOW);
    CHECK(r.closed == 0);

    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "about:blank", true, -1);
    settle(w);
    st.window = w;
    st.front = AU_FRONT_BROWSER;

    au_command_run(&st, &host, AU_CMD_FILE_NEW_TAB);
    CHECK(w->ntabs == 2 && w->active == 1 && r.focus == 2);
    au_command_run(&st, &host, AU_CMD_FILE_CLOSE_TAB);
    CHECK(w->ntabs == 1 && r.closed == 0);
    CHECK(au_command_enabled(&st, AU_CMD_FILE_REOPEN_TAB));
    au_command_run(&st, &host, AU_CMD_FILE_REOPEN_TAB);
    CHECK(w->ntabs == 2);
    au_command_run(&st, &host, AU_CMD_FILE_CLOSE_WINDOW);
    CHECK(r.closed == 1);

    au_command_run(&st, &host, AU_CMD_TESTS_FONTS);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "about:fonts");
    CHECK(r.raised == 1);
    settle(w);
    CHECK(au_command_enabled(&st, AU_CMD_GO_BACK));
    au_command_run(&st, &host, AU_CMD_GO_BACK);
    settle(w);
    CHECK(!au_command_enabled(&st, AU_CMD_GO_BACK));
    au_command_run(&st, &host, AU_CMD_GO_FORWARD);
    settle(w);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "about:fonts");

    /* Zoom and back. */
    au_command_run(&st, &host, AU_CMD_VIEW_ZOOM_IN);
    CHECK(au_command_enabled(&st, AU_CMD_VIEW_ACTUAL_SIZE));
    au_command_run(&st, &host, AU_CMD_VIEW_ACTUAL_SIZE);
    CHECK(!au_command_enabled(&st, AU_CMD_VIEW_ACTUAL_SIZE));

    /* Tabs on Left changes the preference. */
    b->prefs.tab_position = AU_TABS_TOP;
    au_command_run(&st, &host, AU_CMD_VIEW_TABS_LEFT);
    CHECK(b->prefs.tab_position == AU_TABS_LEFT);
    CHECK(au_command_checked(&st, AU_CMD_VIEW_TABS_LEFT));
    au_command_run(&st, &host, AU_CMD_VIEW_TABS_TOP);
    CHECK(b->prefs.tab_position == AU_TABS_TOP);

    /* Add Bookmark. */
    CHECK(au_bookmark_find_url(&b->bookmarks, "about:fonts") == NULL);
    au_command_run(&st, &host, AU_CMD_BOOKMARKS_ADD);
    CHECK(au_bookmark_find_url(&b->bookmarks, "about:fonts") != NULL);

    /* Open File opens what the port chose. */
    r.file_url = "file:///doc.html";
    au_command_run(&st, &host, AU_CMD_FILE_OPEN_FILE);
    CHECK(r.choose == 1);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "file:///doc.html");
    settle(w);

    /* Find Again with nothing to find beeps. */
    CHECK(au_bw_find(w, "zebra", false) == false);
    if (au_command_enabled(&st, AU_CMD_EDIT_FIND_AGAIN)) {
        au_command_run(&st, &host, AU_CMD_EDIT_FIND_AGAIN);
        CHECK(r.beeps == 1);
    }

    /* Native windows and dialogs reach the host. */
    struct {
        AuCommand c;
        AuWindowKind k;
    } windows[] = {
        {AU_CMD_ABOUT, AU_WINDOW_ABOUT},
        {AU_CMD_EDIT_PREFERENCES, AU_WINDOW_PREFERENCES},
        {AU_CMD_GO_HISTORY, AU_WINDOW_HISTORY},
        {AU_CMD_BOOKMARKS_SHOW, AU_WINDOW_BOOKMARKS},
        {AU_CMD_WINDOW_DOWNLOADS, AU_WINDOW_DOWNLOADS},
        {AU_CMD_WINDOW_COOKIES, AU_WINDOW_COOKIES},
        {AU_CMD_VIEW_PAGE_SOURCE, AU_WINDOW_PAGE_SOURCE},
        {AU_CMD_DEV_PAGE_SOURCE, AU_WINDOW_PAGE_SOURCE},
        {AU_CMD_DEV_CONSOLE, AU_WINDOW_CONSOLE},
        {AU_CMD_DEV_NETWORK, AU_WINDOW_NETWORK},
        {AU_CMD_DEV_PERFORMANCE, AU_WINDOW_PERFORMANCE},
        {AU_CMD_DEV_TIMING_TEST, AU_WINDOW_TIMING_TEST},
        {AU_CMD_DEV_SYSTEM_STATUS, AU_WINDOW_SYSTEM_STATUS},
        {AU_CMD_DEV_REPORT, AU_WINDOW_REPORT},
        {AU_CMD_HELP_REPORT, AU_WINDOW_REPORT},
        {AU_CMD_TESTS_BENCHMARK, AU_WINDOW_PAINT_BENCHMARK},
    };
    for (size_t i = 0; i < sizeof windows / sizeof windows[0]; i++) {
        int had = r.shown[windows[i].k];
        au_command_run(&st, &host, windows[i].c);
        CHECK(r.shown[windows[i].k] == had + 1);
    }
    au_command_run(&st, &host, AU_CMD_FILE_SAVE_AS);
    CHECK(r.saved == 1);
    au_command_run(&st, &host, AU_CMD_EDIT_FIND);
    CHECK(r.find == 1 && r.find_tool == 0);
    au_command_run(&st, &host, AU_CMD_FILE_QUIT);
    CHECK(r.quits == 1);
    au_command_run(&st, &host, AU_CMD_FILE_OPEN_LOCATION);
    CHECK(r.focus == 3);
    au_command_run(&st, &host, AU_CMD_EDIT_SELECT_ALL);
    CHECK(r.edits[AU_EDIT_SELECT_ALL] == 1);
    st.text_focus = true;
    au_command_run(&st, &host, AU_CMD_EDIT_PASTE);
    CHECK(r.edits[AU_EDIT_PASTE] == 1);
    st.text_focus = false;

    /* A developer window in front: Find and Find Again are its own. */
    st.front = AU_FRONT_TOOL;
    st.tool_can_find_again = true;
    au_command_run(&st, &host, AU_CMD_EDIT_FIND);
    au_command_run(&st, &host, AU_CMD_EDIT_FIND_PREVIOUS);
    CHECK(r.find_tool == 1 && r.tool_again == 1);
    st.front = AU_FRONT_BROWSER;

    /* A port without a Page Source window gets a view-source: tab. */
    r.no_source_window = true;
    int tabs = w->ntabs;
    au_command_run(&st, &host, AU_CMD_VIEW_PAGE_SOURCE);
    CHECK(w->ntabs == tabs + 1);
    CHECK(strncmp(au_page_url(au_bw_active_tab(w)->page), "view-source:", 12) == 0);
    r.no_source_window = false;

    free_made(&r);
    au_browser_window_free(w);
    au_browser_free(b);
}

/* ---- Contextual menus ---- */

static void test_context(void)
{
    AuBrowser *b = make_browser();
    Rec r;
    memset(&r, 0, sizeof r);
    r.browser = b;
    AuCommandHost host = make_host(&r);
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "about:blank", true, -1);
    settle(w);
    AuCommandState st;
    memset(&st, 0, sizeof st);
    st.browser = b;
    st.window = w;
    st.front = AU_FRONT_BROWSER;

    AuContextMenu m;
    memset(&m, 0, sizeof m);
    m.kind = AU_CONTEXT_PAGE;
    m.tab = -1;
    m.link = au_strndup("http://example.com/a", 20);
    au_command_run_context(&st, &host, &m, AU_CMD_COPY_LINK);
    CHECK_STR(r.copied, "http://example.com/a");
    au_command_run_context(&st, &host, &m, AU_CMD_OPEN_LINK_WINDOW);
    CHECK(r.nmade == 1);
    CHECK_STR(r.new_url, "http://example.com/a");
    au_command_run_context(&st, &host, &m, AU_CMD_PASTE_GO);
    au_command_run_context(&st, &host, &m, AU_CMD_FIELD_COPY);
    CHECK(r.edits[AU_EDIT_PASTE_AND_GO] == 1 && r.edits[AU_EDIT_COPY] == 1);
    au_command_run_context(&st, &host, &m, AU_CMD_VIEW_SOURCE);
    CHECK(r.shown[AU_WINDOW_PAGE_SOURCE] == 1);
    au_command_run_context(&st, &host, &m, AU_CMD_OPEN_LINK);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "http://example.com/a");
    au_context_menu_free(&m);

    /* Item labels come from the table. */
    CHECK(au_bw_context_menu(w, 400, 400, 0, &m));
    CHECK(m.n > 0);
    for (int i = 0; i < m.n; i++)
        if (m.items[i].cmd != AU_CMD_SEPARATOR && m.items[i].cmd != AU_CMD_TOGGLE_BLOCKING)
            CHECK_STR(m.items[i].label, au_command_info(m.items[i].cmd)->title);
    au_context_menu_free(&m);

    free_made(&r);
    au_browser_window_free(w);
    au_browser_free(b);
}

/* ---- Dynamic menus ---- */

static void test_lists(void)
{
    AuBrowser *b = make_browser();
    Rec r;
    memset(&r, 0, sizeof r);
    r.browser = b;
    AuCommandHost host = make_host(&r);
    AuCommandState st;
    memset(&st, 0, sizeof st);
    st.browser = b;

    /* History: nothing while empty; else a separator and the most recent
     * entries, at most AU_GO_MENU_ENTRIES. */
    AuMenuList l;
    memset(&l, 0, sizeof l);
    au_history_clear(&b->history);
    au_menu_list_history(b, &l);
    CHECK(l.n == 0);
    char url[64];
    for (int i = 0; i < 20; i++) {
        snprintf(url, sizeof url, "http://h%d.example/", i);
        au_history_visit(&b->history, url, i == 19 ? "Latest" : "", 1763100000 + i);
    }
    au_menu_list_history(b, &l);
    CHECK(l.n == 1 + AU_GO_MENU_ENTRIES);
    CHECK(l.e[0].kind == AU_ENTRY_SEPARATOR);
    for (int i = 1; i < l.n; i++) {
        CHECK(l.e[i].kind == AU_ENTRY_ITEM && l.e[i].enabled && l.e[i].parent == -1);
        CHECK_STR(l.e[i].url, b->history.e[i - 1].url);
    }
    CHECK_STR(l.e[1].title, "Latest");
    CHECK_STR(l.e[2].title, "http://h18.example/");
    /* Running an entry opens it (in a new window here, with none open). */
    au_menu_entry_run(&st, &host, &l.e[2]);
    CHECK(r.nmade == 1);
    CHECK_STR(r.new_url, "http://h18.example/");
    au_menu_list_free(&l);
    CHECK(l.n == 0 && l.e == NULL);

    /* Bookmarks, breadth-first. */
    au_bookmarks_clear(&b->bookmarks);
    AuBookmark *a = au_bookmark_add(&b->bookmarks, NULL, NULL, "A", "http://a.example/", 0);
    AuBookmark *f = au_bookmark_add_folder(&b->bookmarks, NULL, NULL, "Folder", 0);
    AuBookmark *e = au_bookmark_add_folder(&b->bookmarks, NULL, NULL, "Empty", 0);
    AuBookmark *c = au_bookmark_add(&b->bookmarks, NULL, NULL, "", "http://c.example/", 0);
    AuBookmark *inner = au_bookmark_add_folder(&b->bookmarks, f, NULL, "Inner", 0);
    AuBookmark *fb = au_bookmark_add(&b->bookmarks, f, NULL, "In folder", "http://f.example/", 0);
    AuBookmark *deep = au_bookmark_add(&b->bookmarks, inner, NULL, "Deep", "http://d.example/", 0);
    (void)e;
    au_menu_list_bookmarks(b, &l);
    /* Top level: A, Folder, Empty, c; then Folder's: Inner, In folder;
     * then Empty's "(empty)"; then Inner's: Deep. */
    CHECK(l.n == 8);
    CHECK_STR(l.e[0].title, "A");
    CHECK(l.e[0].kind == AU_ENTRY_ITEM && l.e[0].bookmark == a->id && l.e[0].parent == -1);
    CHECK(l.e[1].kind == AU_ENTRY_FOLDER && l.e[1].parent == -1);
    CHECK(l.e[2].kind == AU_ENTRY_FOLDER);
    CHECK_STR(l.e[3].title, "http://c.example/");
    CHECK(l.e[3].bookmark == c->id);
    CHECK(l.e[4].bookmark == inner->id && l.e[4].kind == AU_ENTRY_FOLDER && l.e[4].parent == 1);
    CHECK(l.e[5].bookmark == fb->id && l.e[5].parent == 1);
    CHECK_STR(l.e[6].title, "(empty)");
    CHECK(!l.e[6].enabled && l.e[6].parent == 2);
    CHECK(l.e[7].bookmark == deep->id && l.e[7].parent == 4);
    for (int i = 0; i < l.n; i++)
        CHECK(l.e[i].parent < i);
    /* Running one opens it in the front window. */
    AuBrowserWindow *w = r.made[0];
    st.window = w;
    st.front = AU_FRONT_BROWSER;
    au_menu_entry_run(&st, &host, &l.e[7]);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "http://d.example/");
    /* The "(empty)" item and folders do nothing. */
    au_menu_entry_run(&st, &host, &l.e[6]);
    au_menu_entry_run(&st, &host, &l.e[1]);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "http://d.example/");
    /* A bookmark deleted since the menu was built is not opened. */
    au_bookmark_delete(&b->bookmarks, a);
    au_menu_entry_run(&st, &host, &l.e[0]);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "http://d.example/");
    au_menu_list_free(&l);

    /* Windows: a separator, then each window, the front one checked. */
    int x, y, z;
    AuWindowRef wins[3] = {{"Front", &x}, {"", &y}, {"Back", &z}};
    au_menu_list_windows(wins, 0, NULL, &l);
    CHECK(l.n == 0);
    au_menu_list_windows(wins, 3, &x, &l);
    CHECK(l.n == 4 && l.e[0].kind == AU_ENTRY_SEPARATOR);
    CHECK_STR(l.e[1].title, "Front");
    CHECK_STR(l.e[2].title, " ");
    CHECK(l.e[1].checked && !l.e[2].checked && !l.e[3].checked);
    au_menu_entry_run(&st, &host, &l.e[3]);
    CHECK(r.selects == 1 && r.selected == &z);
    au_menu_list_free(&l);

    free_made(&r);
    au_browser_free(b);
}

int main(void)
{
    fonts = au_fontset_new(); /* empty: the code must cope */
    test_table();
    test_enabled();
    test_run();
    test_context();
    test_lists();
    au_fontset_free(fonts);
    printf("commands: all tests passed\n");
    return 0;
}
