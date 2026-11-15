/* Tests for src/browser: back/forward lists, tabs, history, bookmarks,
 * preferences, the session file, the URL-or-search helper, chrome layout
 * and hit testing. Exits non-zero on the first failure. */
#include "browser/browser.h"
#include "browser/listview.h"
#include "browser/macroman.h"
#include "browser/url.h"

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

/* ---- URL helper ---- */

static void check_input(const char *in, const char *want, bool search)
{
    bool s = !search;
    char *u = au_url_from_input(in, "https://search.example/?q=%s", &s);
    if (!want) {
        CHECK(u == NULL);
        return;
    }
    CHECK(u != NULL);
    if (strcmp(u, want) != 0 || s != search) {
        fprintf(stderr, "input \"%s\": got \"%s\" (search %d), want \"%s\" (search %d)\n", in, u, s, want, search);
        exit(1);
    }
    au_free(u);
}

static void test_url(void)
{
    check_input("", NULL, false);
    check_input("   ", NULL, false);
    check_input("http://example.com/a?b#c", "http://example.com/a?b#c", false);
    check_input("HTTPS://Example.com", "https://Example.com", false);
    check_input("about:blank", "about:blank", false);
    check_input("example.com", "http://example.com/", false);
    check_input("  example.com/path?x=1  ", "http://example.com/path?x=1", false);
    check_input("www.apple.com", "http://www.apple.com/", false);
    check_input("localhost", "http://localhost/", false);
    check_input("localhost:8080/x", "http://localhost:8080/x", false);
    check_input("192.168.1.10:3128", "http://192.168.1.10:3128/", false);
    check_input("[::1]:80", "http://[::1]:80/", false);
    check_input("gopher://floodgap.com", "gopher://floodgap.com", false);
    check_input("x-custom://thing", "x-custom://thing", false);
    check_input("mac os 9", "https://search.example/?q=mac+os+9", true);
    check_input("example", "https://search.example/?q=example", true);
    check_input("c++ & rust?", "https://search.example/?q=c%2B%2B+%26+rust%3F", true);
    check_input("1.5", "https://search.example/?q=1.5", true);
    check_input("example.com is a site", "https://search.example/?q=example.com+is+a+site", true);
    check_input("/Users/me/a b.html", "file:///Users/me/a%20b.html", false);
    check_input("Macintosh HD:Documents:a.html", "file:///Macintosh%20HD/Documents/a.html", false);

    CHECK(au_url_looks_like_host("a.co", 4));
    CHECK(!au_url_looks_like_host("a.c", 3));
    CHECK(!au_url_looks_like_host("-a.com", 6));
    CHECK(!au_url_looks_like_host("a..com", 6));
    CHECK(!au_url_looks_like_host("example.com:http", 16));
    CHECK(!au_url_looks_like_host("999.1.1.1x", 10));

    char *p = au_url_to_path("file:///Macintosh%20HD/Documents/a.html", ':');
    CHECK_STR(p, "Macintosh HD:Documents:a.html");
    au_free(p);
    p = au_url_to_path("file://localhost/tmp/x%41.txt?q", '/');
    CHECK_STR(p, "/tmp/xA.txt");
    au_free(p);
    CHECK(au_url_to_path("file://server/x", '/') == NULL);
    CHECK(au_url_to_path("http://a/b", '/') == NULL);
    char *u = au_url_from_path("Disk:A/B:c", ':');
    CHECK_STR(u, "file:///Disk/A%2FB/c");
    au_free(u);

    size_t n;
    const char *k = au_url_match_key("https://www.example.com/x", &n);
    CHECK_STR(k, "example.com/x");
    char host[64];
    au_url_host("http://user@example.com:80/x", host, sizeof host);
    CHECK_STR(host, "example.com:80");
    au_url_host("about:blank", host, sizeof host);
    CHECK_STR(host, "");
}

/* ---- Back/forward ---- */

static void test_nav(void)
{
    AuNavList l;
    au_nav_init(&l);
    CHECK(au_nav_current(&l) == NULL);
    CHECK(!au_nav_can_go(&l, -1));
    au_nav_push(&l, "a", "A");
    au_nav_push(&l, "b", NULL);
    au_nav_push(&l, "c", "C");
    CHECK(l.n == 3 && l.index == 2);
    CHECK(au_nav_can_go(&l, -2) && !au_nav_can_go(&l, -3) && !au_nav_can_go(&l, 1));
    CHECK_STR(au_nav_go(&l, -1)->url, "b");
    CHECK_STR(au_nav_go(&l, -1)->url, "a");
    CHECK(au_nav_go(&l, -1) == NULL && l.index == 0);
    CHECK_STR(au_nav_go(&l, 2)->url, "c");
    au_nav_go(&l, -2);
    /* Navigating after going back drops the forward entries. */
    au_nav_push(&l, "d", "");
    CHECK(l.n == 2 && l.index == 1);
    CHECK_STR(l.e[0].url, "a");
    CHECK_STR(l.e[1].url, "d");
    CHECK(!au_nav_can_go(&l, 1));
    /* The list is capped; the oldest entries go. */
    char name[16];
    for (int i = 0; i < AU_NAV_MAX + 10; i++) {
        snprintf(name, sizeof name, "u%d", i);
        au_nav_push(&l, name, "");
    }
    CHECK(l.n == AU_NAV_MAX && l.index == AU_NAV_MAX - 1);
    snprintf(name, sizeof name, "u%d", AU_NAV_MAX + 9);
    CHECK_STR(au_nav_current(&l)->url, name);
    au_nav_free(&l);
}

/* ---- Browser controller with the stub page ---- */

static uint32_t fake_ms;
static AuTime fake_now = 1763100000; /* a Friday in November 2025 */
static int closes, changes, invalidations;

static uint32_t t_ms(void *ctx) { (void)ctx; return fake_ms; }
static AuTime t_now(void *ctx) { (void)ctx; return fake_now; }
static void t_inval(void *ctx, AuBrowserWindow *w, AuIRect r) { (void)ctx, (void)w, (void)r; invalidations++; }
static unsigned focus_last; /* the last focus change reported */
static void t_changed(void *ctx, AuBrowserWindow *w, unsigned what)
{
    (void)ctx, (void)w;
    changes++;
    if (what & (AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE))
        focus_last = what & (AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE);
}
static void t_close(void *ctx, AuBrowserWindow *w) { (void)ctx, (void)w; closes++; }

static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    if (strcmp(url, "file:///doc.html") != 0)
        return false;
    static const char html[] = "<!doctype html><title> A  Title </title><h1>Head</h1><p>Some <b>text</b> here.";
    *data = au_strndup(html, sizeof html - 1);
    *len = sizeof html - 1;
    return true;
}

static AuBrowser *make_browser(AuFontSet *fonts)
{
    AuBrowserHost h = {NULL, t_ms, t_now, t_read, t_inval, t_changed, t_close, NULL};
    return au_browser_new(fonts, &h);
}

static void settle(AuBrowserWindow *w)
{
    for (int i = 0; i < 100 && au_browser_busy(w->browser); i++) {
        fake_ms += 100;
        au_bw_idle(w);
    }
}

static void test_tabs(AuFontSet *fonts)
{
    AuBrowser *b = make_browser(fonts);
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "about:home", true, -1);
    CHECK(w->ntabs == 1 && w->active == 0);
    CHECK_STR(au_bw_field_text(w), "about:home");
    char title[64];
    au_bw_window_title(w, title, sizeof title);
    CHECK_STR(title, "Aura");

    au_bw_navigate_input(w, "example.com");
    AuTab *t = au_bw_active_tab(w);
    CHECK(t->loading);
    CHECK_STR(au_page_url(t->page), "http://example.com/");
    CHECK(au_page_security(t->page) == AU_SECURITY_NOT_SECURE);
    settle(w);
    CHECK(!t->loading);
    CHECK_STR(au_page_title(t->page), "example.com");
    CHECK_STR(au_nav_current(&t->nav)->title, "example.com");

    au_bw_navigate(w, "file:///doc.html");
    CHECK(t->loading && !*au_page_title(t->page)); /* files arrive from idle */
    settle(w);
    CHECK_STR(au_page_title(t->page), "A Title");
    CHECK(au_page_security(t->page) == AU_SECURITY_NONE);
    au_bw_back(w);
    CHECK_STR(au_page_url(t->page), "http://example.com/");
    au_bw_back(w);
    CHECK_STR(au_page_url(t->page), "about:home");
    CHECK(!au_nav_can_go(&t->nav, -1));
    au_bw_forward(w);
    settle(w);
    au_bw_navigate(w, "about:scene"); /* drops file:///doc.html */
    CHECK(t->nav.n == 3 && !au_nav_can_go(&t->nav, 1));

    /* Stop during a simulated load. */
    au_bw_navigate(w, "http://slow.example/");
    CHECK(t->loading);
    au_bw_stop(w);
    CHECK(!t->loading);

    /* History recorded the visits, most recent first. */
    CHECK(b->history.n >= 4);
    CHECK_STR(b->history.e[0].url, "http://slow.example/");
    long ex = au_history_find(&b->history, "http://example.com/");
    CHECK(ex >= 0 && b->history.e[ex].visits >= 2);
    CHECK_STR(b->history.e[au_history_find(&b->history, "file:///doc.html")].title, "A Title");

    /* More tabs, selection, moving, closing. */
    au_bw_new_tab(w, "about:blank", true, -1);
    au_bw_new_tab(w, "about:fonts", false, -1);
    CHECK(w->ntabs == 3 && w->active == 1);
    CHECK_STR(au_bw_field_text(w), "");
    au_bw_move_tab(w, 2, 0);
    CHECK(w->active == 2);
    CHECK_STR(au_page_url(w->tabs[0]->page), "about:fonts");
    au_bw_select_tab(w, 0);
    au_bw_close_tab(w, 0); /* the active tab: the one to its right becomes active */
    CHECK(w->ntabs == 2 && w->active == 0);
    CHECK(t == w->tabs[0]);
    au_bw_close_tab(w, 1);
    CHECK(w->ntabs == 1 && closes == 0);
    au_bw_close_tab(w, 0);
    CHECK(closes == 1 && w->ntabs == 1); /* the host closes the window */

    /* Bookmark toggling. */
    au_bw_navigate(w, "about:home");
    CHECK(!au_bw_is_bookmarked(w));
    CHECK(au_bw_bookmark_page(w, true) != NULL);
    CHECK(au_bw_is_bookmarked(w));
    CHECK_STR(b->bookmarks.root.first->title, "Aura");
    au_bw_bookmark_page(w, true);
    CHECK(!au_bw_is_bookmarked(w));

    /* Completion from history. */
    const char *c = au_bw_complete(w, "exa");
    CHECK(c && !strcmp(c, "http://example.com/"));
    CHECK(au_bw_complete(w, "zzz") == NULL);

    /* Field edits. */
    au_bw_set_field_text(w, "typed");
    CHECK_STR(au_bw_field_text(w), "typed");
    au_bw_set_field_text(w, NULL);
    CHECK_STR(au_bw_field_text(w), "about:home");

    /* Painting with whatever fonts there are must not crash. */
    AuSurface *s = au_surface_new(800, 600);
    AuCanvas *cv = au_canvas_new(s);
    au_bw_paint(w, cv, (AuIRect){0, 0, 800, 600});
    au_bw_paint(w, cv, (AuIRect){-10, -10, 5, 5});
    au_canvas_free(cv);
    au_surface_free(s);

    /* History off: no new visits. */
    b->prefs.history_enabled = false;
    size_t before = b->history.n;
    au_bw_navigate(w, "file:///doc.html?private");
    CHECK(b->history.n == before && au_history_find(&b->history, "file:///doc.html?private") < 0);
    au_browser_free(b);
}

/* The focus rule: the page has the focus after navigation and tab
 * switches unless the user is typing in the field; a fresh blank tab puts
 * it in the field. */
static char external_url[256];
static bool external_ok;
static bool t_external(void *ctx, const char *url)
{
    (void)ctx;
    snprintf(external_url, sizeof external_url, "%s", url);
    return external_ok;
}

/* Addresses Aura does not open go to the host's helper applications. */
static void test_external(AuFontSet *fonts)
{
    AuBrowserHost h = {NULL, t_ms, t_now, t_read, t_inval, t_changed, t_close, t_external};
    AuBrowser *b = au_browser_new(fonts, &h);
    CHECK(au_browser_opens_url(b, "http://a/") && au_browser_opens_url(b, "HTTPS://a/") &&
          au_browser_opens_url(b, "file:///x") && au_browser_opens_url(b, "about:home") &&
          au_browser_opens_url(b, "view-source:http://a/") && au_browser_opens_url(b, "no scheme"));
    CHECK(!au_browser_opens_url(b, "mailto:a@b.c") && !au_browser_opens_url(b, "ftp://h/f") &&
          !au_browser_opens_url(b, "news:comp.sys.mac"));
    AuPageFetcher f;
    memset(&f, 0, sizeof f);
    au_browser_set_fetcher(b, "gopher", &f);
    CHECK(au_browser_opens_url(b, "gopher://h/"));

    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "about:home", true, -1);
    external_ok = true;
    external_url[0] = 0;
    au_bw_navigate(w, "mailto:someone@example.com");
    CHECK_STR(external_url, "mailto:someone@example.com");
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "about:home"); /* stays */
    CHECK(au_bw_active_tab(w)->nav.n == 1);
    CHECK(au_bw_new_tab(w, "ftp://h/f", false, -1) == NULL && w->ntabs == 1);
    CHECK_STR(external_url, "ftp://h/f");
    /* Typed in the address field: the field shows the page again. */
    au_bw_set_field_focus(w, true);
    au_bw_set_field_text(w, "mailto:x@y.z");
    focus_last = 0;
    au_bw_navigate_input(w, "mailto:x@y.z");
    CHECK_STR(external_url, "mailto:x@y.z");
    CHECK_STR(au_bw_field_text(w), "about:home");
    CHECK(focus_last == AU_CHANGED_FOCUS_PAGE);
    /* Without a helper the tab shows that it cannot open the address. */
    external_ok = false;
    au_bw_navigate(w, "news:comp.sys.mac");
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "news:comp.sys.mac");
    au_browser_free(b);
}

static void test_focus(AuFontSet *fonts)
{
    AuBrowser *b = make_browser(fonts);
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    focus_last = 0;
    au_bw_new_tab(w, NULL, true, -1);
    CHECK(focus_last == AU_CHANGED_FOCUS_FIELD);
    /* Focused but not edited (the URL selected): navigation moves it. */
    au_bw_set_field_focus(w, true);
    au_bw_navigate(w, "about:fonts");
    CHECK(focus_last == AU_CHANGED_FOCUS_PAGE);

    /* Typing in the field: Back does not take the focus or the edit. */
    au_bw_set_field_focus(w, true);
    au_bw_set_field_text(w, "half-typ");
    CHECK(au_bw_typing(w));
    focus_last = 0;
    au_bw_back(w);
    CHECK(focus_last == 0);
    CHECK_STR(au_bw_field_text(w), "half-typ");

    /* A tab switch ends the edit and gives the page the focus. */
    au_bw_new_tab(w, "about:scene", false, -1);
    au_bw_select_tab(w, 1);
    CHECK(focus_last == AU_CHANGED_FOCUS_PAGE);
    CHECK_STR(au_bw_field_text(w), "about:scene");
    /* So does closing a tab (the defect: the field stayed focused). */
    au_bw_new_tab(w, "about:home", true, -1);
    au_bw_set_field_focus(w, true);
    focus_last = 0;
    au_bw_close_tab(w, w->active);
    CHECK(focus_last == AU_CHANGED_FOCUS_PAGE);
    /* A background tab opened from a link leaves the focus alone. */
    focus_last = 0;
    au_bw_new_tab(w, "about:home", false, -1);
    CHECK(focus_last == 0);
    /* Switching to a fresh blank tab focuses the field. */
    au_bw_new_tab(w, NULL, false, w->ntabs);
    au_bw_select_tab(w, w->ntabs - 1);
    CHECK(focus_last == AU_CHANGED_FOCUS_FIELD);
    au_browser_free(b);
}

static void test_mouse(AuFontSet *fonts)
{
    AuBrowser *b = make_browser(fonts);
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "about:home", true, -1);
    au_bw_navigate(w, "about:scene");
    AuEvent e;
    memset(&e, 0, sizeof e);
    /* Click Back. */
    AuIRect r = w->layout.back;
    e.x = (r.x0 + r.x1) / 2;
    e.y = (r.y0 + r.y1) / 2;
    e.type = AU_EVENT_MOUSE_DOWN;
    CHECK(au_bw_mouse(w, &e).part == AU_HIT_BACK);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "about:home");
    /* A press that leaves the button before release does nothing. */
    r = w->layout.forward;
    e.x = (r.x0 + r.x1) / 2;
    e.type = AU_EVENT_MOUSE_DOWN;
    au_bw_mouse(w, &e);
    e.x = 400;
    e.y = 300;
    e.type = AU_EVENT_MOUSE_MOVE;
    au_bw_mouse(w, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "about:home");
    /* New tab button. */
    r = w->layout.new_tab;
    e.x = (r.x0 + r.x1) / 2;
    e.y = (r.y0 + r.y1) / 2;
    e.type = AU_EVENT_MOUSE_DOWN;
    au_bw_mouse(w, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
    CHECK(w->ntabs == 2 && w->active == 1);
    /* Drag tab 1 onto tab 0's place. */
    AuIRect t1 = au_chrome_tab_rect(&w->layout, 1), t0 = au_chrome_tab_rect(&w->layout, 0);
    e.x = t1.x0 + 20;
    e.y = 12;
    e.type = AU_EVENT_MOUSE_DOWN;
    au_bw_mouse(w, &e);
    e.type = AU_EVENT_MOUSE_MOVE;
    e.x = t0.x0 + 10;
    au_bw_mouse(w, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
    CHECK_STR(au_page_url(w->tabs[1]->page), "about:home");
    CHECK(w->active == 0);
    /* Close box. */
    AuIRect cb = au_chrome_tab_close_rect(&w->layout, 1);
    e.x = (cb.x0 + cb.x1) / 2;
    e.y = (cb.y0 + cb.y1) / 2;
    e.type = AU_EVENT_MOUSE_DOWN;
    CHECK(au_bw_mouse(w, &e).part == AU_HIT_TAB_CLOSE);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
    CHECK(w->ntabs == 1);
    /* Splitter drag in left mode changes the column width preference. */
    b->prefs.tab_position = AU_TABS_LEFT;
    au_browser_prefs_changed(b);
    int w0 = b->prefs.tab_column_width;
    AuIRect sp = w->layout.splitter;
    e.x = sp.x0 + 2;
    e.y = (sp.y0 + sp.y1) / 2;
    e.type = AU_EVENT_MOUSE_DOWN;
    CHECK(au_bw_mouse(w, &e).part == AU_HIT_SPLITTER);
    e.type = AU_EVENT_MOUSE_MOVE;
    e.x += 40;
    au_bw_mouse(w, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
    CHECK(b->prefs.tab_column_width == w0 + 40);
    CHECK(w->layout.splitter.x0 == w0 + 40);
    /* Scrolling keys. */
    au_bw_navigate(w, "about:scene");
    AuEvent k;
    memset(&k, 0, sizeof k);
    k.type = AU_EVENT_KEY_DOWN;
    k.key = AU_KEY_PAGE_DOWN;
    CHECK(au_bw_key(w, &k));
    CHECK(au_bw_active_tab(w)->scroll_y > 0);
    k.key = AU_KEY_END;
    au_bw_key(w, &k);
    int mx, my;
    au_bw_scroll_max(w, &mx, &my);
    CHECK(au_bw_active_tab(w)->scroll_y == my);
    au_browser_free(b);
}

/* ---- History ---- */

static void test_history(void)
{
    AuHistory h;
    au_history_init(&h, 5);
    AuTime t0 = au_time_of_date(2026, 11, 14) + 10 * 3600; /* Saturday 10:00 */
    for (int i = 0; i < 8; i++) {
        char u[32];
        snprintf(u, sizeof u, "http://s%d.example/", i);
        au_history_visit(&h, u, "T", t0 + i);
    }
    CHECK(h.n == 5); /* capped, oldest dropped */
    CHECK_STR(h.e[0].url, "http://s7.example/");
    CHECK_STR(h.e[4].url, "http://s3.example/");
    au_history_visit(&h, "http://s4.example/", "Four", t0 + 100);
    CHECK(h.n == 5);
    CHECK_STR(h.e[0].url, "http://s4.example/");
    CHECK(h.e[0].visits == 2);
    CHECK_STR(h.e[0].title, "Four");
    au_history_visit(&h, "http://s4.example/", NULL, t0 + 101);
    CHECK_STR(h.e[0].title, "Four");

    /* Day grouping. */
    AuHistory g;
    au_history_init(&g, 100);
    AuTime day = 86400;
    au_history_visit(&g, "a", "", t0 - 3 * day);
    au_history_visit(&g, "b", "", t0 - day);
    au_history_visit(&g, "c", "", t0 - day + 60);
    au_history_visit(&g, "d", "", t0);
    AuHistoryGroup groups[8];
    size_t ng = au_history_groups(&g, groups, 8);
    CHECK(ng == 3);
    CHECK(groups[0].count == 1 && groups[1].count == 2 && groups[2].count == 1);
    CHECK(groups[0].day == au_day_of(t0));
    char label[64];
    au_day_label(groups[0].day, au_day_of(t0), label, sizeof label);
    CHECK_STR(label, "Today");
    au_day_label(groups[1].day, au_day_of(t0), label, sizeof label);
    CHECK_STR(label, "Yesterday");
    au_day_label(groups[2].day, au_day_of(t0), label, sizeof label);
    CHECK_STR(label, "Wednesday");
    au_day_label(au_day_of(t0) - 30, au_day_of(t0), label, sizeof label);
    CHECK_STR(label, "Thursday, 15 October 2026");
    int y, m, d, wd;
    au_date_of_day(au_day_of(t0), &y, &m, &d, &wd);
    CHECK(y == 2026 && m == 11 && d == 14 && wd == 6);
    au_date_of_day(0, &y, &m, &d, &wd);
    CHECK(y == 1970 && m == 1 && d == 1 && wd == 4);
    CHECK(au_day_of(-1) == -1);

    /* The history window list groups by day. */
    AuListView lv;
    au_listview_init(&lv);
    au_listview_from_history(&lv, &g, t0);
    CHECK(lv.n == 3 + 4);
    CHECK(lv.rows[0].kind == AU_ROW_HEADER && !strcmp(lv.rows[0].text, "Today"));
    CHECK(lv.rows[1].kind == AU_ROW_LINK && lv.rows[1].id == 0);
    au_listview_free(&lv);

    /* Expiry. */
    au_history_expire(&g, t0, 2);
    CHECK(g.n == 3 && au_history_find(&g, "a") < 0);

    /* Round trip, with tabs in titles. */
    au_history_set_title(&g, "c", "has\ttab");
    AuBuf out;
    au_buf_init(&out);
    au_history_serialize(&g, &out);
    AuHistory r;
    au_history_init(&r, 100);
    CHECK(au_history_parse(&r, out.data, out.len));
    CHECK(r.n == g.n);
    for (size_t i = 0; i < r.n; i++) {
        CHECK_STR(r.e[i].url, g.e[i].url);
        CHECK_STR(r.e[i].title, g.e[i].title);
        CHECK(r.e[i].last_visit == g.e[i].last_visit && r.e[i].visits == g.e[i].visits);
    }
    CHECK_STR(r.e[au_history_find(&r, "c")].title, "has tab");
    au_history_free(&r);

    /* Damaged files: no crash, reported, good lines kept. */
    au_history_init(&r, 100);
    static const char bad[] = "# Aura history 1\nnot a line\n12\tx\turl-without-title\nzz\n\x01\x02\x03\n";
    CHECK(!au_history_parse(&r, bad, sizeof bad - 1));
    au_history_free(&r);
    au_history_init(&r, 100);
    static const char binary[] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR";
    CHECK(!au_history_parse(&r, binary, sizeof binary - 1));
    CHECK(r.n == 0);
    au_history_free(&r);
    au_history_init(&r, 100);
    CHECK(au_history_parse(&r, "", 0));
    au_history_free(&r);

    au_buf_free(&out);
    au_history_free(&g);
    au_history_free(&h);
}

/* ---- Bookmarks ---- */

static void test_bookmarks(void)
{
    AuBookmarks bm;
    au_bookmarks_init(&bm);
    AuBookmark *a = au_bookmark_add(&bm, NULL, NULL, "Apple & <Mac>", "http://apple.com/?a=1&b=\"2\"", 1000);
    AuBookmark *f = au_bookmark_add_folder(&bm, NULL, NULL, "Retro \"Stuff\"", 2000);
    AuBookmark *f2 = au_bookmark_add_folder(&bm, f, NULL, "Inner", 3000);
    au_bookmark_add(&bm, f2, NULL, "Deep", "http://deep.example/", 4000);
    AuBookmark *m = au_bookmark_add(&bm, f, NULL, "Macintosh Garden", "http://macintoshgarden.org/", 5000);
    au_bookmark_add_folder(&bm, NULL, NULL, "Empty", 6000);
    AuBookmark *u = au_bookmark_add(&bm, NULL, a, "Unicode \xC3\xA9t\xC3\xA9 \xE2\x80\x94", "about:blank", 7000);
    CHECK(bm.root.first == u && u->next == a);
    CHECK(au_bookmarks_count(&bm) == 7);
    CHECK(au_bookmark_depth(f2) == 1 && au_bookmark_depth(a) == 0);
    CHECK(au_bookmark_find_url(&bm, "http://deep.example/") != NULL);
    CHECK(au_bookmark_find_id(&bm, m->id) == m);

    /* Edits. */
    au_bookmark_rename(&bm, m, "Garden");
    CHECK_STR(m->title, "Garden");
    CHECK(!au_bookmark_move(&bm, f, f2, NULL)); /* into its own child */
    CHECK(au_bookmark_move(&bm, m, f2, NULL));
    CHECK(m->parent == f2 && f2->last == m);
    CHECK(au_bookmark_move(&bm, m, f, f2));
    CHECK(f->first == m && m->next == f2);

    /* Preorder walk. */
    const char *order[] = {"Unicode \xC3\xA9t\xC3\xA9 \xE2\x80\x94", "Apple & <Mac>", "Retro \"Stuff\"", "Garden",
                           "Inner", "Deep", "Empty"};
    int i = 0;
    for (AuBookmark *b = au_bookmark_next(&bm.root, &bm.root, false); b; b = au_bookmark_next(b, &bm.root, false))
        CHECK_STR(b->title, order[i++]);
    CHECK(i == 7);

    /* Manager list: collapsed folders hide their contents. */
    AuListView lv;
    au_listview_init(&lv);
    au_listview_from_bookmarks(&lv, &bm);
    CHECK(lv.n == 4); /* Unicode, Apple, Retro (collapsed), Empty */
    f->expanded = true;
    lv.selected = 2;
    au_listview_from_bookmarks(&lv, &bm);
    CHECK(lv.n == 6 && lv.selected == 2 && lv.rows[2].id == f->id);
    CHECK(lv.rows[4].kind == AU_ROW_FOLDER && lv.rows[4].indent == 1);
    bool disc;
    CHECK(au_listview_hit(&lv, 6 + 16 + 2, 2 + 4 * AU_LIST_ROW_H + 5, &disc) == 4 && disc);
    au_listview_free(&lv);

    /* Netscape round trip. */
    AuBuf out;
    au_buf_init(&out);
    au_bookmarks_serialize(&bm, &out);
    CHECK(strstr(out.data, "<!DOCTYPE NETSCAPE-Bookmark-file-1>") != NULL);
    CHECK(strstr(out.data, "Apple &amp; &lt;Mac&gt;") != NULL);
    CHECK(strstr(out.data, "HREF=\"http://apple.com/?a=1&amp;b=&quot;2&quot;\"") != NULL);
    AuBookmarks r;
    au_bookmarks_init(&r);
    CHECK(au_bookmarks_parse(&r, out.data, out.len));
    AuBookmark *x = au_bookmark_next(&r.root, &r.root, false);
    AuBookmark *y = au_bookmark_next(&bm.root, &bm.root, false);
    int n = 0;
    while (x && y) {
        CHECK(x->folder == y->folder);
        CHECK_STR(x->title, y->title);
        CHECK_STR(x->url, y->url);
        CHECK(x->added == y->added);
        CHECK(au_bookmark_depth(x) == au_bookmark_depth(y));
        x = au_bookmark_next(x, &r.root, false);
        y = au_bookmark_next(y, &bm.root, false);
        n++;
    }
    CHECK(!x && !y && n == 7);

    /* A file written by another browser. */
    static const char other[] =
        "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; "
        "charset=UTF-8\">\n<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks Menu</H1>\n<DL><p>\n"
        "    <DT><H3 ADD_DATE=\"1\" LAST_MODIFIED=\"2\">Toolbar</H3>\n    <DD>Description\n"
        "    <DL><p>\n        <DT><A HREF=\"https://a.example/\" ADD_DATE=\"5\" ICON=\"data:x\">A</A>\n"
        "        <DD>about A\n    </DL><p>\n    <HR>\n"
        "    <DT><A HREF=\"https://b.example/\">B &amp; C</A>\n</DL>\n";
    CHECK(au_bookmarks_parse(&r, other, sizeof other - 1));
    CHECK_STR(r.root.title, "Bookmarks Menu");
    CHECK(r.root.first && r.root.first->folder);
    CHECK_STR(r.root.first->title, "Toolbar");
    CHECK(r.root.first->first && !strcmp(r.root.first->first->url, "https://a.example/"));
    CHECK(r.root.first->first->added == 5);
    CHECK(r.root.last && !strcmp(r.root.last->title, "B & C"));

    /* Not a bookmark file: reported, tree left empty, no crash. */
    static const char junk[] = "\x00\xff\xfe garbage <<<< >>>> \x01";
    CHECK(!au_bookmarks_parse(&r, junk, sizeof junk - 1));
    CHECK(r.root.first == NULL);
    CHECK(!au_bookmarks_parse(&r, "", 0));

    /* Completion. */
    CHECK(au_bookmarks_complete(&bm, "deep") != NULL);
    CHECK(au_bookmarks_complete(&bm, "nothing") == NULL);

    /* Deleting a folder deletes its contents. */
    au_bookmark_delete(&bm, f);
    CHECK(au_bookmarks_count(&bm) == 3);
    CHECK(au_bookmark_find_url(&bm, "http://deep.example/") == NULL);
    au_buf_free(&out);
    au_bookmarks_free(&r);
    au_bookmarks_free(&bm);
}

/* ---- Preferences ---- */

static void test_prefs(void)
{
    AuPrefs p;
    au_prefs_init(&p);
    CHECK_STR(p.home_page, "about:home");
    CHECK(p.tab_position == AU_TABS_TOP && p.javascript && p.load_images && p.history_enabled);
    CHECK(!p.send_dnt && p.send_gpc && p.https_first && p.block_third_party_cookies && p.block_trackers &&
          p.block_popups && p.warn_app_downloads);
    CHECK(!p.clear_history_on_quit && !p.clear_cookies_on_quit && !p.clear_cache_on_quit);

    /* A file with some keys, an unknown key, comments, odd spacing and
     * an out-of-range number. */
    static const char file[] = "# comment\n\nhome_page=http://home.example/\r\n"
                               "  tabs.position = left\n"
                               "future.setting=keep me\n"
                               "proxy.http.host=proxy.local\nproxy.http.port=99999\n"
                               "privacy.do_not_track=true\nprivacy.global_privacy_control=false\n"
                               "privacy.clear_cache_on_quit=1\n"
                               "privacy.cookies=same-site\n"
                               "network.user_agent=Line\\nBreak \\\\ ok\n";
    CHECK(au_prefs_parse(&p, file, sizeof file - 1));
    CHECK_STR(p.home_page, "http://home.example/");
    CHECK(p.tab_position == AU_TABS_LEFT);
    CHECK_STR(p.http_proxy_host, "proxy.local");
    CHECK(p.http_proxy_port == 65535);
    CHECK(p.send_dnt && !p.send_gpc && p.clear_cache_on_quit);
    CHECK(p.cookies == AU_COOKIES_SAME_SITE);
    CHECK_STR(p.user_agent, "Line\nBreak \\ ok");
    CHECK(p.font_size == 16); /* missing: default */
    CHECK(p.nextra == 1);

    /* Change more, then round trip. */
    p.tab_column_width = 222;
    p.block_popups = false;
    p.history_days = 7;
    au_prefs_set_str(&p.download_folder, "Macintosh HD:Downloads");
    au_prefs_set_str(&p.search_template, "http://frogfind.com/?q=%s");
    AuBuf out;
    au_buf_init(&out);
    au_prefs_serialize(&p, &out);
    CHECK(strstr(out.data, "future.setting=keep me\n") != NULL);
    AuPrefs q;
    au_prefs_init(&q);
    CHECK(au_prefs_parse(&q, out.data, out.len));
    AuBuf a, b;
    au_buf_init(&a);
    au_buf_init(&b);
    au_prefs_serialize(&q, &a);
    CHECK(a.len == out.len && !memcmp(a.data, out.data, a.len));
    CHECK(q.tab_column_width == 222 && !q.block_popups && q.history_days == 7);
    CHECK_STR(q.download_folder, "Macintosh HD:Downloads");
    CHECK(q.nextra == 1 && !strcmp(q.extra[0].value, "keep me"));

    /* Copy. */
    AuPrefs c;
    au_prefs_copy(&c, &q);
    au_prefs_serialize(&c, &b);
    CHECK(b.len == out.len && !memcmp(b.data, out.data, b.len));
    au_prefs_free(&c);

    /* Typed access by key. */
    au_buf_clear(&a);
    CHECK(au_prefs_get(&q, "privacy.https_first", &a));
    CHECK_STR(au_buf_cstr(&a), "true");
    CHECK(au_prefs_set(&q, "privacy.https_first", "false") && !q.https_first);
    CHECK(!au_prefs_set(&q, "future.setting", "changed"));
    CHECK_STR(q.extra[0].value, "changed");

    /* Damaged: reported, readable lines still applied, nothing crashes. */
    AuPrefs d;
    au_prefs_init(&d);
    static const char bad[] = "home_page=http://x/\n\x00\x01\x02garbage\nno equals sign\n=novalue\nfonts.size=abc\n";
    CHECK(!au_prefs_parse(&d, bad, sizeof bad - 1));
    CHECK_STR(d.home_page, "http://x/");
    CHECK(d.font_size == 16);
    au_prefs_free(&d);

    au_buf_free(&a);
    au_buf_free(&b);
    au_buf_free(&out);
    au_prefs_free(&q);
    au_prefs_free(&p);
}

/* ---- Session ---- */

static void test_session(AuFontSet *fonts)
{
    AuBrowser *b = make_browser(fonts);
    AuBrowserWindow *w = au_browser_window_new(b, 700, 500);
    au_bw_new_tab(w, "about:home", true, -1);
    au_bw_navigate(w, "about:scene");
    au_bw_back(w);
    au_bw_new_tab(w, "file:///doc.html", true, -1);
    settle(w);
    AuBrowserWindow *w2 = au_browser_window_new(b, 640, 480);
    au_bw_new_tab(w2, "about:fonts", true, -1);
    AuBuf out;
    au_buf_init(&out);
    au_browser_session_serialize(b, &out);
    AuSession s;
    CHECK(au_session_parse(&s, out.data, out.len));
    CHECK(s.nwindows == 2);
    CHECK(s.windows[0].width == 700 && s.windows[0].ntabs == 2 && s.windows[0].active == 1);
    CHECK(s.windows[0].tabs[0].nav.n == 2 && s.windows[0].tabs[0].nav.index == 0);
    CHECK_STR(s.windows[0].tabs[1].nav.e[0].title, "A Title");

    AuBrowserWindow *r = au_browser_window_new(b, 700, 500);
    au_bw_restore(r, &s.windows[0]);
    CHECK(r->ntabs == 2 && r->active == 1);
    CHECK_STR(au_page_url(r->tabs[0]->page), "about:home");
    CHECK(au_nav_can_go(&r->tabs[0]->nav, 1));
    au_session_free(&s);

    static const char bad_session[] = "window x\ntab\nentry\t\tz\ngarbage\n";
    CHECK(!au_session_parse(&s, bad_session, sizeof bad_session - 1));
    au_session_free(&s);
    au_buf_free(&out);
    au_browser_free(b);
}

/* ---- Chrome layout and hit testing ---- */

static void check_inside(AuIRect inner, AuIRect outer)
{
    CHECK(inner.x0 >= outer.x0 && inner.y0 >= outer.y0 && inner.x1 <= outer.x1 && inner.y1 <= outer.y1);
}

static bool disjoint(AuIRect a, AuIRect b)
{
    return au_irect_empty(au_irect_intersect(a, b));
}

static AuHitPart hit_centre(const AuChromeLayout *l, AuIRect r)
{
    return au_chrome_hit(l, (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2).part;
}

static void test_layout(void)
{
    static const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {420, 300}, {1600, 1200}};
    for (int pos = 0; pos < 2; pos++) {
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            int W = sizes[s][0], H = sizes[s][1];
            AuChromeLayout l;
            au_chrome_layout(&l, W, H, (AuTabPosition)pos, 170, 3, 0);
            AuIRect win = {0, 0, W, H};
            /* Regions are inside the window and do not overlap. */
            check_inside(l.toolbar, win);
            check_inside(l.page, win);
            check_inside(l.status, win);
            check_inside(l.tabstrip, win);
            CHECK(disjoint(l.toolbar, l.page) && disjoint(l.page, l.status) && disjoint(l.tabstrip, l.page) &&
                  disjoint(l.tabstrip, l.toolbar));
            CHECK(l.status.y1 == H && l.status.y0 == H - AU_CHROME_STATUS_H);
            CHECK(l.page.x1 == W - (AU_CHROME_SCROLL_W - 1));
            CHECK(l.vscroll.x0 == l.page.x1 && l.vscroll.x1 == W + 1);
            if (pos == AU_TABS_TOP) {
                CHECK(l.tabstrip.y0 == 0 && l.tabstrip.y1 == AU_CHROME_TAB_H);
                CHECK(l.toolbar.y0 == AU_CHROME_TAB_H && l.page.y0 == AU_CHROME_TAB_H + AU_CHROME_TOOLBAR_H);
                CHECK(l.page.x0 == 0);
                CHECK(au_irect_empty(l.splitter));
            } else {
                CHECK(l.toolbar.y0 == 0 && l.toolbar.x1 == W);
                CHECK(l.tabstrip.x0 == 0 && l.tabstrip.x1 == 170 && l.tabstrip.y0 == AU_CHROME_TOOLBAR_H);
                CHECK(l.splitter.x0 == 170 && l.page.x0 == 170 + AU_CHROME_SPLITTER_W);
                CHECK(hit_centre(&l, l.splitter) == AU_HIT_SPLITTER);
            }
            /* Toolbar parts in order, inside the toolbar, hit where drawn. */
            AuIRect parts[] = {l.back, l.forward, l.reload, l.home, l.address, l.bookmark, l.throbber};
            AuHitPart want[] = {AU_HIT_BACK,    AU_HIT_FORWARD,  AU_HIT_RELOAD,  AU_HIT_HOME,
                                AU_HIT_ADDRESS, AU_HIT_BOOKMARK, AU_HIT_THROBBER};
            for (int i = 0; i < 7; i++) {
                check_inside(parts[i], l.toolbar);
                if (i)
                    CHECK(parts[i].x0 >= parts[i - 1].x1);
                if (i != 4)
                    CHECK(hit_centre(&l, parts[i]) == want[i]);
            }
            CHECK(au_chrome_hit(&l, l.address_text.x0 + 2, (l.address.y0 + l.address.y1) / 2).part == AU_HIT_ADDRESS);
            CHECK(hit_centre(&l, l.security) == AU_HIT_SECURITY);
            check_inside(l.address_text, l.address);
            check_inside(l.security, l.address);
            CHECK(l.address_text.x0 >= l.security.x1);
            /* Tabs. */
            for (int i = 0; i < 3; i++) {
                AuIRect t = au_chrome_tab_rect(&l, i);
                check_inside(t, l.tabstrip);
                AuHit h = au_chrome_hit(&l, (t.x0 + t.x0 + 30) / 2, (t.y0 + t.y1) / 2);
                CHECK(h.part == AU_HIT_TAB && h.index == i);
                AuIRect c = au_chrome_tab_close_rect(&l, i);
                check_inside(c, t);
                h = au_chrome_hit(&l, (c.x0 + c.x1) / 2, (c.y0 + c.y1) / 2);
                CHECK(h.part == AU_HIT_TAB_CLOSE && h.index == i);
            }
            CHECK(hit_centre(&l, l.new_tab) == AU_HIT_NEW_TAB);
            CHECK(hit_centre(&l, l.page) == AU_HIT_PAGE);
            CHECK(hit_centre(&l, l.status) == AU_HIT_STATUS);
            CHECK(hit_centre(&l, l.grow) == AU_HIT_GROW);
            CHECK(au_chrome_hit(&l, -1, 5).part == AU_HIT_NONE);
            CHECK(au_chrome_hit(&l, W, H).part == AU_HIT_NONE);
            CHECK(!l.overflow);
        }
    }
    /* Top tabs: widths shrink with count and stop at the minimum. */
    AuChromeLayout l;
    au_chrome_layout(&l, 800, 600, AU_TABS_TOP, 0, 1, 0);
    CHECK(l.tab_size == AU_CHROME_TAB_MAX_W);
    au_chrome_layout(&l, 800, 600, AU_TABS_TOP, 0, 6, 0);
    CHECK(l.tab_size < AU_CHROME_TAB_MAX_W && l.tab_size > AU_CHROME_TAB_MIN_W && !l.overflow);
    CHECK(l.new_tab.x0 >= au_chrome_tab_rect(&l, 5).x1);

    /* Overflow: scroll buttons appear, tabs scroll. */
    au_chrome_layout(&l, 640, 480, AU_TABS_TOP, 0, 20, 0);
    CHECK(l.overflow && l.tab_size == AU_CHROME_TAB_MIN_W);
    CHECK(l.tab_scroll_max == 20 * AU_CHROME_TAB_MIN_W - (l.tabs_area.x1 - l.tabs_area.x0));
    CHECK(hit_centre(&l, l.scroll_back) == AU_HIT_TAB_SCROLL_BACK);
    CHECK(hit_centre(&l, l.scroll_fwd) == AU_HIT_TAB_SCROLL_FWD);
    CHECK(disjoint(l.scroll_fwd, l.tabs_area) && disjoint(l.new_tab, l.scroll_fwd));
    int s = au_chrome_scroll_to_tab(&l, 19);
    CHECK(s == l.tab_scroll_max);
    au_chrome_layout(&l, 640, 480, AU_TABS_TOP, 0, 20, s);
    AuIRect last = au_chrome_tab_rect(&l, 19);
    CHECK(last.x1 == l.tabs_area.x1);
    AuHit h = au_chrome_hit(&l, last.x0 + 10, 12);
    CHECK(h.part == AU_HIT_TAB && h.index == 19);
    au_chrome_layout(&l, 640, 480, AU_TABS_TOP, 0, 20, 100000);
    CHECK(l.tab_scroll == l.tab_scroll_max);
    au_chrome_layout(&l, 640, 480, AU_TABS_TOP, 0, 20, -5);
    CHECK(l.tab_scroll == 0);
    CHECK(au_chrome_drop_index(&l, l.tabs_area.x0 + 3 * AU_CHROME_TAB_MIN_W + 5, 10) == 3);
    CHECK(au_chrome_drop_index(&l, -50, 10) == 0);
    CHECK(au_chrome_drop_index(&l, 5000, 10) == 19);

    /* Left column overflow. */
    au_chrome_layout(&l, 640, 480, AU_TABS_LEFT, 150, 40, 0);
    CHECK(l.overflow);
    CHECK(hit_centre(&l, l.scroll_back) == AU_HIT_TAB_SCROLL_BACK);
    CHECK(l.scroll_fwd.y1 == l.tabstrip.y1);
    s = au_chrome_scroll_to_tab(&l, 39);
    au_chrome_layout(&l, 640, 480, AU_TABS_LEFT, 150, 40, s);
    last = au_chrome_tab_rect(&l, 39);
    CHECK(last.y1 == l.tabs_area.y1);
    h = au_chrome_hit(&l, 20, last.y0 + 5);
    CHECK(h.part == AU_HIT_TAB && h.index == 39);
    /* A point in the scrolled-off area above the first visible tab does
     * not hit a hidden tab through the new-tab row. */
    CHECK(hit_centre(&l, l.new_tab) == AU_HIT_NEW_TAB);

    /* Column width is clamped to the window. */
    au_chrome_layout(&l, 420, 300, AU_TABS_LEFT, 400, 2, 0);
    CHECK(l.page.x1 - l.page.x0 >= AU_CHROME_PAGE_MIN_W);
    au_chrome_layout(&l, 800, 600, AU_TABS_LEFT, 10, 2, 0);
    CHECK(l.tabstrip.x1 == AU_TAB_COLUMN_MIN);

    /* Tiny windows do not produce inverted rectangles. */
    au_chrome_layout(&l, 1, 1, AU_TABS_TOP, 0, 3, 0);
    CHECK(l.page.x1 >= l.page.x0 && l.page.y1 >= l.page.y0);
}

static void test_ellipsize(AuFontSet *fonts)
{
    AuFontFace *f = au_chrome_label_face(fonts);
    char buf[64];
    au_chrome_ellipsize(f, 12, "short", 1000, buf, sizeof buf);
    CHECK_STR(buf, "short");
    if (!f)
        return;
    au_chrome_ellipsize(f, 12, "A rather long tab title that will not fit", 80, buf, sizeof buf);
    CHECK(au_text_width(f, 12, buf, strlen(buf)) <= 80);
    CHECK(strlen(buf) > 3);
    au_chrome_ellipsize(f, 12, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 20, buf, sizeof buf);
    size_t adv;
    for (size_t i = 0; buf[i]; i += adv)
        CHECK(au_utf8_decode(buf + i, strlen(buf + i), &adv) != 0xFFFD);
}

/* ---- Downloads ---- */

static bool taken(void *ctx, const char *name)
{
    const char *const *list = ctx;
    for (; *list; list++)
        if (!strcmp(*list, name))
            return true;
    return false;
}

static void test_downloads(void)
{
    char out[128];
    au_hfs_sanitize("report:final.pdf", NULL, NULL, out, sizeof out);
    CHECK_STR(out, "report-final.pdf");
    au_hfs_sanitize("A very long file name that will not fit in HFS.sit", NULL, NULL, out, sizeof out);
    CHECK_STR(out, "A very long file name that.sit"); /* cut at 27, trailing space trimmed */
    au_hfs_sanitize("  .profile  ", NULL, NULL, out, sizeof out);
    CHECK_STR(out, "_profile");
    au_hfs_sanitize("", NULL, NULL, out, sizeof out);
    CHECK_STR(out, "download");
    au_hfs_sanitize("tab\there\x01.txt", NULL, NULL, out, sizeof out);
    CHECK_STR(out, "tabhere.txt");
    au_hfs_sanitize("archive.tar.gzipped-long", NULL, NULL, out, sizeof out);
    CHECK_STR(out, "archive.tar.gzipped-long");
    /* Non-ASCII counts as one character each. */
    au_hfs_sanitize("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                    "\xC3\xA9\xC3\xA9\xC3\xA9.hqx",
                    NULL, NULL, out, sizeof out);
    size_t nchars = 0;
    for (const char *p = out; *p; p++)
        nchars += ((unsigned char)*p & 0xC0) != 0x80;
    CHECK(nchars == 31 && strstr(out, ".hqx"));
    const char *existing[] = {"file.bin", "file 2.bin", NULL};
    au_hfs_sanitize("file.bin", taken, existing, out, sizeof out);
    CHECK_STR(out, "file 3.bin");
    const char *existing2[] = {"Aaaaaaaaaaaaaaaaaaaaaaaaaaa.sit", NULL};
    au_hfs_sanitize("Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sit", taken, existing2, out, sizeof out);
    CHECK_STR(out, "Aaaaaaaaaaaaaaaaaaaaaaaaa 2.sit");
    CHECK(strlen(out) == 31);

    au_download_name_from_url("http://x.example/files/Stuffit%20Expander.sit?x=1", out, sizeof out);
    CHECK_STR(out, "Stuffit Expander.sit");
    au_download_name_from_url("http://x.example/", out, sizeof out);
    CHECK_STR(out, "x.example");

    char sz[32];
    au_format_size(512, sz, sizeof sz);
    CHECK_STR(sz, "512 bytes");
    au_format_size(1536 * 1024, sz, sizeof sz);
    CHECK_STR(sz, "1.5 MB");

    /* Model with the simulated source and a fake clock. */
    AuDownloads dl;
    au_downloads_init(&dl, au_sim_source());
    dl.max_active = 2;
    AuDownload *a = au_downloads_add(&dl, "http://x.example/a.sit", "a.sit", "application/x-stuffit", 100000, 5);
    AuDownload *b = au_downloads_add(&dl, "http://x.example/b:c.bin", "b:c.bin", "", -1, 6);
    AuDownload *c = au_downloads_add(&dl, "http://x.example/fail.hqx", "fail.hqx", "", 200000, 7);
    AuDownload *r = au_downloads_add(&dl, "http://refuse.example/r", "r", "", -1, 8);
    CHECK_STR(b->filename, "b-c.bin");
    CHECK(a->state == AU_DL_QUEUED && au_downloads_busy(&dl));
    uint32_t t = 1000;
    au_downloads_idle(&dl, t);
    CHECK(a->state == AU_DL_ACTIVE && b->state == AU_DL_ACTIVE && c->state == AU_DL_QUEUED);
    t += 600;
    au_downloads_idle(&dl, t);
    CHECK(a->received > 0 && a->received < a->total && a->rate > 0);
    CHECK(au_download_eta(a) > 0);
    au_download_pause(&dl, b);
    CHECK(b->state == AU_DL_PAUSED);
    au_downloads_idle(&dl, t);
    CHECK(c->state == AU_DL_ACTIVE); /* the slot b freed */
    for (int i = 0; i < 400 && (a->state == AU_DL_ACTIVE || c->state == AU_DL_ACTIVE); i++) {
        t += 250;
        au_downloads_idle(&dl, t);
    }
    CHECK(a->state == AU_DL_DONE && a->received == 100000);
    CHECK(c->state == AU_DL_FAILED && c->received == 100000 && strlen(c->error) > 0);
    au_downloads_idle(&dl, t);
    CHECK(r->state == AU_DL_FAILED && !strcmp(r->error, "Connection refused"));
    au_download_resume(&dl, b);
    CHECK(b->state == AU_DL_QUEUED);
    int64_t bfore = b->received;
    au_downloads_idle(&dl, t);
    t += 1000;
    au_downloads_idle(&dl, t);
    CHECK(b->state == AU_DL_ACTIVE && b->received > bfore);

    /* Persistence: the active item comes back paused and interrupted. */
    AuBuf out_file;
    au_buf_init(&out_file);
    au_downloads_serialize(&dl, &out_file);
    AuDownloads back;
    au_downloads_init(&back, au_sim_source());
    CHECK(au_downloads_parse(&back, out_file.data, out_file.len));
    CHECK(back.n == 4);
    AuDownload *bb = au_downloads_find(&back, b->id);
    CHECK(bb && bb->state == AU_DL_PAUSED && bb->interrupted && bb->received == b->received);
    CHECK(au_downloads_find(&back, a->id)->state == AU_DL_DONE);
    CHECK_STR(au_downloads_find(&back, c->id)->error, c->error);
    CHECK(au_downloads_find(&back, a->id)->started == 5);
    AuDownload *nw = au_downloads_add(&back, "http://new/", "n", "", -1, 9);
    CHECK(nw->id > r->id);

    au_download_cancel(&dl, b);
    CHECK(b->state == AU_DL_CANCELLED);
    au_downloads_clear_completed(&dl);
    CHECK(dl.n == 0);
    a = au_downloads_add(&dl, "http://x/", "x", "", -1, 1);
    au_download_remove(&dl, a);
    CHECK(dl.n == 0);

    /* Damaged file. */
    static const char bad[] = "# Aura downloads 1\n1\tbogus\t0\t0\t0\tu\tf\tm\te\nnot\ta\tline\n\x01\x02";
    CHECK(!au_downloads_parse(&back, bad, sizeof bad - 1));
    CHECK(back.n == 0);
    static const char nohead[] = "1\tdone\t5\t5\t0\thttp://a/\ta\t\t\n";
    CHECK(!au_downloads_parse(&back, nohead, sizeof nohead - 1));
    CHECK(back.n == 1);

    /* Painting the list must not crash without fonts. */
    AuSurface *s = au_surface_new(300, 200);
    AuCanvas *cv = au_canvas_new(s);
    au_downloads_paint(&back, cv, NULL, 300, 200, 0, 0, true);
    au_canvas_free(cv);
    au_surface_free(s);
    CHECK(au_downloads_hit(&back, 5, 0) == 0 && au_downloads_hit(&back, AU_DL_ROW_H + 1, 0) == -1);

    au_buf_free(&out_file);
    au_downloads_free(&back);
    au_downloads_free(&dl);
}

/* ---- Mac Roman ---- */

static void check_roman(const char *utf8, const char *want, unsigned flags)
{
    AuBuf b;
    au_buf_init(&b);
    au_macroman_from_utf8(utf8, strlen(utf8), &b, '?', flags);
    au_buf_cstr(&b);
    if (strcmp(b.data, want) != 0) {
        fprintf(stderr, "roman \"%s\": got \"", utf8);
        for (size_t i = 0; i < b.len; i++)
            fprintf(stderr, (unsigned char)b.data[i] < 0x80 ? "%c" : "\\x%02X", (unsigned char)b.data[i]);
        fprintf(stderr, "\"\n");
        exit(1);
    }
    au_buf_free(&b);
}

static void test_macroman(void)
{
    /* Characters Mac Roman has map to it, including curly quotes, dashes
     * and the ellipsis. */
    check_roman("caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x94 \xE2\x80\xA6", "caf\x8E \xD2quoted\xD3 \xD1 \xC9", 0);
    check_roman("it\xE2\x80\x99s \xE2\x82\xAC" "5 \xE2\x80\xA2", "it\xD5s \xDB" "5 \xA5", 0);
    /* Transliterated: other dashes, primes, spaces, arrows, fractions,
     * letters with other diacritics, fullwidth forms. */
    check_roman("a\xE2\x80\x90" "b\xE2\x80\x95" "c", "a-b\xD1" "c", 0);
    check_roman("5\xE2\x80\xB2" "3\xE2\x80\xB3", "5'3\"", 0);
    check_roman("a\xE2\x80\x83" "b\xE3\x80\x80" "c", "a b c", 0);
    check_roman("x \xE2\x86\x92 y", "x -> y", 0);
    check_roman("\xC2\xBD", " 1/2", 0);
    check_roman("\xC5\x81\xC3\xB3" "d\xC5\xBA \xC4\x8D" "e\xC5\xA1ka", "L\x97" "dz ceska", 0);
    check_roman("\xEF\xBC\xA1\xEF\xBC\x91", "A1", 0);
    check_roman("\xC4\xB2", "IJ", 0);
    /* Invisible characters vanish; decomposed accents keep their base. */
    check_roman("a\xE2\x80\x8B" "b\xC2\xAD" "c\xEF\xBB\xBF" "e\xCC\x81", "abce", 0);
    /* The rest: one replacement each, never garbage. */
    check_roman("\xE6\x97\xA5\xE6\x9C\xAC", "??", 0);
    check_roman("\xF0\x9F\x98\x80!", "?!", 0);
    check_roman("bad\xFF\xFE" "end", "bad??end", 0);
    check_roman("\xCE\xB1\xCE\xB2", "??", 0);
    /* Line breaks for the clipboard. */
    check_roman("a\nb\r\nc", "a\rb\rc", AU_MR_CR_LINES);
    check_roman("a\nb", "a\nb", 0);

    AuBuf b;
    au_buf_init(&b);
    au_macroman_to_utf8("\x8E\xD2\rx", 4, &b, AU_MR_CR_LINES);
    au_buf_cstr(&b);
    CHECK_STR(b.data, "\xC3\xA9\xE2\x80\x9C\nx");
    au_buf_clear(&b);
    au_utf16be_from_utf8("A\xC3\xA9\n\xF0\x9F\x98\x80", 8, &b, AU_MR_CR_LINES);
    CHECK(b.len == 10);
    CHECK(!memcmp(b.data, "\0A\0\xE9\0\r\xD8\x3D\xDE\x00", 10));
    au_buf_free(&b);
    CHECK(au_macroman_byte(0x2014) == 0xD1 && au_macroman_byte(0x4E00) == -1);
}

int main(void)
{
    AuFontSet *fonts = au_fontset_new(); /* empty: the code must cope */
    test_url();
    test_nav();
    test_tabs(fonts);
    test_focus(fonts);
    test_external(fonts);
    test_mouse(fonts);
    test_history();
    test_bookmarks();
    test_prefs();
    test_session(fonts);
    test_layout();
    test_ellipsize(fonts);
    test_downloads();
    test_macroman();
    au_fontset_free(fonts);
    printf("browser: all tests passed\n");
    return 0;
}
