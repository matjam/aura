/* Tests for native chrome mode (au_browser_window_new_native): the
 * window's surface and coordinates cover only the page, clicks in page
 * coordinates reach links, every action a port's own widgets call works,
 * the accessors report what the painted chrome shows, and each
 * AU_CHANGED_* notice fires when its state changes, in both chrome
 * modes. Uses the Ahem-like test font, so every glyph is a 1em square. */
#include "../layout/ahem.h"
#include "browser/browser.h"

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

static AuFontSet *fonts;
static uint32_t fake_ms = 1000;

/* ---- Files: file:///mem/NAME ---- */

static const struct {
    const char *name, *text;
} mem_files[] = {
    {"links.html",
     "<!DOCTYPE html><title>Links</title><body style='margin:0'>"
     "<p style='margin:0'><a id=a href='other.html'>xxxx</a></p>"
     "<p style='margin:0'><a href='#sec'>yyyy</a></p>"
     "<div style='height:1000px'></div><h2 id=sec style='margin:0;font-size:16px'>s</h2>"
     "<div style='height:2000px'></div>"},
    {"other.html", "<!DOCTYPE html><title>Other</title><body style='margin:0'><div style='height:3000px'>o</div>"},
    {"untitled.html", "<!DOCTYPE html><body style='margin:0'>no title"},
    {"wide.html",
     "<!DOCTYPE html><title>Wide</title><body style='margin:0'>"
     "<div style='white-space:nowrap;font-size:10px;line-height:1;height:2000px'>"
     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
     "</div>"},
};

static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    if (strncmp(url, "file:///mem/", 12) != 0)
        return false;
    for (size_t i = 0; i < sizeof mem_files / sizeof mem_files[0]; i++) {
        if (!strcmp(url + 12, mem_files[i].name)) {
            size_t n = strlen(mem_files[i].text);
            *data = au_alloc(n + 1);
            memcpy(*data, mem_files[i].text, n);
            *len = n;
            return true;
        }
    }
    return false;
}

/* ---- The recording host ---- */

static unsigned seen;  /* every AU_CHANGED_* bit since the last clear */
static int closes;
static AuIRect inval_union; /* what the model invalidated since the last clear */
static int inval_count;

static uint32_t t_ms(void *ctx) { (void)ctx; return fake_ms; }
static AuTime t_now(void *ctx) { (void)ctx; return 0; }
static void t_changed(void *ctx, AuBrowserWindow *w, unsigned what) { (void)ctx, (void)w; seen |= what; }
static void t_close(void *ctx, AuBrowserWindow *w) { (void)ctx, (void)w; closes++; }
static void t_inval(void *ctx, AuBrowserWindow *w, AuIRect r)
{
    (void)ctx, (void)w;
    inval_union = inval_count ? au_irect_union(inval_union, r) : r;
    inval_count++;
}

static void clear(void)
{
    seen = 0;
    inval_count = 0;
    memset(&inval_union, 0, sizeof inval_union);
}

#define FIRED(bits) ((seen & (unsigned)(bits)) == (unsigned)(bits))

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

static AuBrowserWindow *make_window(AuBrowser *b, bool native, int width, int height)
{
    return native ? au_browser_window_new_native(b, width, height) : au_browser_window_new(b, width, height);
}

static void settle(AuBrowserWindow *w)
{
    for (int i = 0; i < 400 && au_browser_busy(w->browser); i++) {
        fake_ms += 50;
        au_bw_idle(w);
    }
    CHECK(!au_browser_busy(w->browser));
}

static void mouse(AuBrowserWindow *w, AuEventType type, int x, int y, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.x = x;
    e.y = y;
    e.mods = mods;
    au_bw_mouse(w, &e);
}

static void status(AuBrowserWindow *w, char *out)
{
    au_bw_status_text(w, out, 300);
}

/* ---- The page fills the surface ---- */

static void test_page_only(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new_native(b, 640, 400);
    CHECK(w->native_chrome);
    au_bw_new_tab(w, "file:///mem/links.html", true, -1);
    settle(w);
    const AuChromeLayout *l = &w->layout;
    CHECK(l->page.x0 == 0 && l->page.y0 == 0 && l->page.x1 == 640 && l->page.y1 == 400);
    CHECK(au_irect_empty(l->tabstrip) && au_irect_empty(l->toolbar) && au_irect_empty(l->status) &&
          au_irect_empty(l->vscroll) && au_irect_empty(l->hscroll) && au_irect_empty(l->grow) &&
          au_irect_empty(l->address) && au_irect_empty(l->back) && au_irect_empty(l->new_tab));
    /* Every point inside is the page. */
    CHECK(au_chrome_hit(l, 0, 0).part == AU_HIT_PAGE);
    CHECK(au_chrome_hit(l, 639, 399).part == AU_HIT_PAGE);
    CHECK(au_chrome_hit(l, 320, 10).part == AU_HIT_PAGE);
    /* The page is painted from the surface's top left: no chrome. The
     * page's background is white; the Platinum chrome is grey. */
    AuSurface *s = au_surface_new(640, 400);
    AuCanvas *c = au_canvas_new(s);
    memset(s->pixels, 0, (size_t)s->stride * 400 * sizeof(AuPixel));
    au_bw_paint(w, c, (AuIRect){0, 0, 640, 400});
    AuPixel white = s->pixels[(size_t)399 * (size_t)s->stride + 639];
    CHECK(s->pixels[s->stride * 2 + 600] == white);
    CHECK(s->pixels[(size_t)390 * (size_t)s->stride + 5] == white);
    au_canvas_free(c);
    au_surface_free(s);
    /* The same window in painted mode starts the page below the chrome. */
    AuBrowserWindow *pw = au_browser_window_new(b, 640, 400);
    CHECK(pw->layout.page.y0 > 0 && !au_irect_empty(pw->layout.toolbar));
    au_browser_window_free(pw);
    /* A resize keeps the page the whole surface and reports the layout. */
    clear();
    au_bw_resize(w, 500, 300);
    CHECK(l->page.x1 == 500 && l->page.y1 == 300 && l->page.x0 == 0 && l->page.y0 == 0);
    CHECK(FIRED(AU_CHANGED_LAYOUT | AU_CHANGED_SCROLL));
    CHECK(inval_count && inval_union.x0 >= 0 && inval_union.y0 >= 0 && inval_union.x1 <= 500 && inval_union.y1 <= 300);
    au_browser_free(b);
}

/* ---- Clicks in page coordinates reach links ---- */

static void test_page_clicks(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new_native(b, 640, 400);
    au_bw_new_tab(w, "file:///mem/links.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    settle(w);
    char st[300];
    /* Hover over the first link, at page (8, 8). */
    clear();
    mouse(w, AU_EVENT_MOUSE_MOVE, 8, 8, 0);
    status(w, st);
    CHECK_STR(st, "file:///mem/other.html");
    CHECK(FIRED(AU_CHANGED_STATUS));
    /* Leaving the page clears it. */
    clear();
    au_bw_mouse_leave(w);
    status(w, st);
    CHECK_STR(st, "");
    CHECK(FIRED(AU_CHANGED_STATUS));
    /* A fragment link, at page (8, 24), scrolls the document. */
    clear();
    mouse(w, AU_EVENT_MOUSE_MOVE, 8, 24, 0);
    mouse(w, AU_EVENT_MOUSE_DOWN, 8, 24, 0);
    mouse(w, AU_EVENT_MOUSE_UP, 8, 24, 0);
    CHECK_STR(au_page_url(t->page), "file:///mem/links.html#sec");
    int sx, sy;
    au_bw_scroll_pos(w, &sx, &sy);
    CHECK(sx == 0 && sy == 1032);
    CHECK(FIRED(AU_CHANGED_SCROLL | AU_CHANGED_NAV));
    CHECK(au_bw_can_go(w, -1) && !au_bw_can_go(w, 1));
    /* A link to another document. */
    au_bw_scroll_to(w, 0, 0);
    mouse(w, AU_EVENT_MOUSE_MOVE, 8, 8, 0);
    mouse(w, AU_EVENT_MOUSE_DOWN, 8, 8, 0);
    mouse(w, AU_EVENT_MOUSE_UP, 8, 8, 0);
    settle(w);
    CHECK_STR(au_page_url(t->page), "file:///mem/other.html");
    au_browser_free(b);
}

/* ---- Every action a port calls ---- */

static void test_actions(void)
{
    AuBrowser *b = make_browser();
    au_prefs_set_str(&b->prefs.home_page, "file:///mem/other.html");
    AuBrowserWindow *w = au_browser_window_new_native(b, 640, 400);
    CHECK(au_bw_tab_count(w) == 0 && au_bw_active_index(w) == -1);
    au_bw_new_tab(w, "file:///mem/links.html", true, -1);
    settle(w);
    AuChromeTab ti;
    CHECK(au_bw_tab_info(w, 0, &ti));
    CHECK_STR(ti.title, "Links");
    CHECK_STR(ti.url, "file:///mem/links.html");
    CHECK(!ti.loading && !ti.unloaded && ti.progress == 0);
    CHECK(!au_bw_tab_info(w, 1, &ti) && !au_bw_tab_info(w, -1, &ti));

    /* New Tab button: a blank tab at the end, active, the field focused. */
    clear();
    AuTab *nt = au_bw_open_new_tab(w);
    CHECK(nt && au_bw_tab_count(w) == 2 && au_bw_active_index(w) == 1);
    CHECK(FIRED(AU_CHANGED_TABS | AU_CHANGED_ACTIVE_TAB | AU_CHANGED_FOCUS_FIELD | AU_CHANGED_NAV));
    CHECK(au_bw_tab_info(w, 1, &ti));
    CHECK_STR(ti.title, "New Tab");
    CHECK_STR(ti.url, "");
    /* The same through au_bw_press. */
    clear();
    au_bw_press(w, (AuHit){AU_HIT_NEW_TAB, -1}, 0);
    CHECK(au_bw_tab_count(w) == 3 && au_bw_active_index(w) == 2 && FIRED(AU_CHANGED_TABS));

    /* Select. */
    clear();
    au_bw_select_tab(w, 0);
    CHECK(au_bw_active_index(w) == 0 && FIRED(AU_CHANGED_ACTIVE_TAB | AU_CHANGED_FIELD | AU_CHANGED_TITLE));
    clear();
    au_bw_press(w, (AuHit){AU_HIT_TAB, 1}, 0);
    CHECK(au_bw_active_index(w) == 1 && FIRED(AU_CHANGED_ACTIVE_TAB));
    /* Move (reorder): the active tab keeps being active. */
    clear();
    au_bw_move_tab(w, 1, 0);
    CHECK(au_bw_active_index(w) == 0 && FIRED(AU_CHANGED_TABS));
    CHECK(au_bw_tab_info(w, 1, &ti));
    CHECK_STR(ti.title, "Links");
    /* Close, through the close box's press. */
    clear();
    au_bw_press(w, (AuHit){AU_HIT_TAB_CLOSE, 2}, 0);
    CHECK(au_bw_tab_count(w) == 2 && FIRED(AU_CHANGED_TABS));
    au_bw_close_tab(w, 0);
    CHECK(au_bw_tab_count(w) == 1 && au_bw_active_index(w) == 0);
    /* Duplicate. */
    clear();
    AuTab *dup = au_bw_duplicate_tab(w, 0);
    settle(w);
    CHECK(dup && au_bw_tab_count(w) == 2 && au_bw_active_index(w) == 1 && FIRED(AU_CHANGED_TABS));
    CHECK(au_bw_tab_info(w, 1, &ti));
    CHECK_STR(ti.url, "file:///mem/links.html");
    au_bw_close_tab(w, 1);

    /* Home, Back, Forward. */
    clear();
    au_bw_press(w, (AuHit){AU_HIT_HOME, -1}, 0);
    settle(w);
    AuTab *t = au_bw_active_tab(w);
    CHECK_STR(au_page_url(t->page), "file:///mem/other.html");
    CHECK(FIRED(AU_CHANGED_NAV | AU_CHANGED_FIELD));
    CHECK(au_bw_can_go(w, -1) && !au_bw_can_go(w, 1));
    clear();
    au_bw_press(w, (AuHit){AU_HIT_BACK, -1}, 0);
    settle(w);
    CHECK_STR(au_page_url(t->page), "file:///mem/links.html");
    CHECK(FIRED(AU_CHANGED_NAV) && !au_bw_can_go(w, -1) && au_bw_can_go(w, 1));
    /* The kept document came back: its title and the tab's label. */
    CHECK(FIRED(AU_CHANGED_TITLE | AU_CHANGED_TAB_INFO | AU_CHANGED_FIELD));
    clear();
    au_bw_press(w, (AuHit){AU_HIT_FORWARD, -1}, 0);
    settle(w);
    CHECK_STR(au_page_url(t->page), "file:///mem/other.html");
    CHECK(FIRED(AU_CHANGED_NAV));
    /* Command-click on Back opens the entry in a new background tab. */
    clear();
    au_bw_press(w, (AuHit){AU_HIT_BACK, -1}, AU_MOD_COMMAND);
    settle(w);
    CHECK(au_bw_tab_count(w) == 2 && au_bw_active_index(w) == 0 && FIRED(AU_CHANGED_TABS));
    CHECK_STR(au_page_url(w->tabs[1]->page), "file:///mem/links.html");
    au_bw_close_tab(w, 1);

    /* Reload, and Stop while loading. */
    au_bw_navigate(w, "http://slow.example/");
    CHECK(au_bw_loading(w));
    clear();
    au_bw_reload_or_stop(w, false);
    CHECK(!au_bw_loading(w) && FIRED(AU_CHANGED_NAV | AU_CHANGED_TAB_INFO));
    clear();
    au_bw_reload_or_stop(w, false); /* not loading: reloads */
    CHECK(au_bw_loading(w) && FIRED(AU_CHANGED_NAV));
    settle(w);
    clear();
    au_bw_press(w, (AuHit){AU_HIT_RELOAD, -1}, AU_MOD_SHIFT); /* past the cache */
    CHECK(au_bw_loading(w));
    settle(w);

    /* The bookmark star. */
    CHECK(!au_bw_is_bookmarked(w));
    clear();
    au_bw_press(w, (AuHit){AU_HIT_BOOKMARK, -1}, 0);
    CHECK(au_bw_is_bookmarked(w) && FIRED(AU_CHANGED_BOOKMARK));
    clear();
    au_bw_press(w, (AuHit){AU_HIT_BOOKMARK, -1}, 0);
    CHECK(!au_bw_is_bookmarked(w) && FIRED(AU_CHANGED_BOOKMARK));

    /* The security button: a page without a certificate opens nothing. */
    CHECK(au_bw_security(w) == AU_SECURITY_NOT_SECURE);
    au_bw_press(w, (AuHit){AU_HIT_SECURITY, -1}, 0);
    CHECK(au_bw_tab_count(w) == 1);

    /* Parts without an action do nothing. */
    au_bw_press(w, (AuHit){AU_HIT_STATUS, -1}, 0);
    au_bw_press(w, (AuHit){AU_HIT_TAB, 7}, 0);
    au_bw_press(w, (AuHit){AU_HIT_TAB_CLOSE, -3}, 0);
    CHECK(au_bw_tab_count(w) == 1 && au_bw_active_index(w) == 0);

    /* The address field. */
    clear();
    au_bw_navigate_input(w, "file:///mem/links.html");
    settle(w);
    CHECK_STR(au_bw_field_text(w), "file:///mem/links.html");
    CHECK(FIRED(AU_CHANGED_FIELD | AU_CHANGED_TITLE));
    char title[64];
    au_bw_window_title(w, title, sizeof title);
    CHECK_STR(title, "Links");

    /* Closing the last tab asks the port to close the window. */
    au_bw_close_tab(w, 0);
    CHECK(closes == 1);
    au_browser_free(b);
}

/* ---- Each notice fires when its state changes, in both modes ---- */

static void test_notices(bool native)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = make_window(b, native, 640, 400);
    clear();
    au_bw_new_tab(w, "file:///mem/links.html", true, -1);
    CHECK(FIRED(AU_CHANGED_TABS | AU_CHANGED_ACTIVE_TAB));
    /* Loading: the tab, Reload/Stop, progress, status and security. */
    CHECK(au_bw_loading(w));
    AuChromeTab ti;
    au_bw_tab_info(w, 0, &ti);
    CHECK(ti.loading);
    char text[300];
    au_bw_progress_text(w, text, sizeof text);
    CHECK(!strncmp(text, "Loading, ", 9));
    clear();
    settle(w);
    CHECK(FIRED(AU_CHANGED_TAB_INFO | AU_CHANGED_NAV | AU_CHANGED_PROGRESS | AU_CHANGED_STATUS |
                AU_CHANGED_SECURITY | AU_CHANGED_TITLE));
    CHECK(!au_bw_loading(w) && au_bw_progress(w) == 0);
    au_bw_progress_text(w, text, sizeof text);
    CHECK_STR(text, "");
    au_bw_tab_info(w, 0, &ti);
    CHECK(!ti.loading);
    CHECK_STR(ti.title, "Links");
    CHECK(au_bw_security(w) == AU_SECURITY_NONE);
    CHECK(au_bw_blocked_count(w) == 0);

    /* A load of a page without a title: the window title is the address. */
    clear();
    au_bw_navigate(w, "file:///mem/untitled.html");
    settle(w);
    CHECK(FIRED(AU_CHANGED_TITLE | AU_CHANGED_TAB_INFO | AU_CHANGED_FIELD | AU_CHANGED_NAV));
    char title[128];
    au_bw_window_title(w, title, sizeof title);
    CHECK_STR(title, "file:///mem/untitled.html");

    /* Security changes with the page. */
    clear();
    au_bw_navigate(w, "http://plain.example/");
    CHECK(FIRED(AU_CHANGED_SECURITY));
    CHECK(au_bw_security(w) == AU_SECURITY_NOT_SECURE);
    /* Progress while loading. */
    clear();
    for (int i = 0; i < 400 && au_bw_loading(w) && !FIRED(AU_CHANGED_PROGRESS); i++) {
        fake_ms += 50;
        au_bw_idle(w);
    }
    CHECK(FIRED(AU_CHANGED_PROGRESS));
    settle(w);

    /* A background tab: the list and its label, not the active tab. */
    clear();
    au_bw_new_tab(w, "file:///mem/other.html", false, -1);
    CHECK(FIRED(AU_CHANGED_TABS) && !(seen & AU_CHANGED_ACTIVE_TAB));
    clear();
    settle(w);
    CHECK(FIRED(AU_CHANGED_TAB_INFO | AU_CHANGED_PROGRESS));
    CHECK(!(seen & (AU_CHANGED_NAV | AU_CHANGED_SECURITY | AU_CHANGED_TITLE)));
    au_bw_tab_info(w, 1, &ti);
    CHECK_STR(ti.title, "Other");

    /* Unloading the background tab. */
    clear();
    CHECK(au_bw_unload_tab(w, 1));
    CHECK(FIRED(AU_CHANGED_TAB_INFO));
    au_bw_tab_info(w, 1, &ti);
    CHECK(ti.unloaded);
    CHECK_STR(ti.title, "Other");
    /* Selecting it loads it again. */
    clear();
    au_bw_select_tab(w, 1);
    CHECK(FIRED(AU_CHANGED_ACTIVE_TAB | AU_CHANGED_TAB_INFO | AU_CHANGED_NAV | AU_CHANGED_STATUS |
                AU_CHANGED_SECURITY | AU_CHANGED_PROGRESS | AU_CHANGED_BOOKMARK | AU_CHANGED_TITLE |
                AU_CHANGED_FIELD | AU_CHANGED_SCROLL));
    au_bw_tab_info(w, 1, &ti);
    CHECK(!ti.unloaded);
    settle(w);

    /* Moving and closing tabs. */
    clear();
    au_bw_move_tab(w, 0, 1);
    CHECK(FIRED(AU_CHANGED_TABS));
    clear();
    au_bw_close_tab(w, 1);
    CHECK(FIRED(AU_CHANGED_TABS | AU_CHANGED_ACTIVE_TAB));

    /* Bookmarks changed from outside (the Bookmarks window). */
    clear();
    au_browser_bookmark_url(b, au_page_url(au_bw_active_tab(w)->page), "x");
    CHECK(FIRED(AU_CHANGED_BOOKMARK) && au_bw_is_bookmarked(w));

    /* Scrolling. */
    clear();
    au_bw_scroll_to(w, 0, 100);
    CHECK(FIRED(AU_CHANGED_SCROLL));
    int sx, sy;
    au_bw_scroll_pos(w, &sx, &sy);
    CHECK(sx == 0 && sy == 100);
    au_browser_free(b);
}

/* ---- Scroll bars: the port's, from au_bw_scroll_max and _to ---- */

static void test_scrolling(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new_native(b, 640, 400);
    au_bw_new_tab(w, "file:///mem/wide.html", true, -1);
    clear();
    settle(w);
    CHECK(FIRED(AU_CHANGED_SCROLL)); /* the content size arrived */
    /* No scroll bar is laid out: the page keeps the whole surface. */
    CHECK(au_irect_empty(w->layout.hscroll) && au_irect_empty(w->layout.vscroll));
    CHECK(w->layout.page.x1 == 640 && w->layout.page.y1 == 400);
    int mx, my;
    au_bw_scroll_max(w, &mx, &my);
    CHECK(mx == 1000 - 640 && my == 2000 - 400);
    /* Vertical and horizontal positions, clamped to the range. */
    clear();
    au_bw_scroll_to(w, 200, 300);
    int sx, sy;
    au_bw_scroll_pos(w, &sx, &sy);
    CHECK(sx == 200 && sy == 300 && FIRED(AU_CHANGED_SCROLL));
    au_bw_scroll_to(w, 5000, 5000);
    au_bw_scroll_pos(w, &sx, &sy);
    CHECK(sx == mx && sy == my);
    au_bw_scroll_to(w, 0, 0);
    /* The wheel, and Shift turning it horizontal. */
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_SCROLL;
    e.x = 100;
    e.y = 100;
    e.dy = 40;
    au_bw_mouse(w, &e);
    au_bw_scroll_pos(w, &sx, &sy);
    CHECK(sx == 0 && sy == 40);
    e.mods = AU_MOD_SHIFT;
    au_bw_mouse(w, &e);
    au_bw_scroll_pos(w, &sx, &sy);
    CHECK(sx == 40 && sy == 40);
    /* Arrow keys scroll both ways too. */
    AuEvent k;
    memset(&k, 0, sizeof k);
    k.type = AU_EVENT_KEY_DOWN;
    k.key = AU_KEY_RIGHT;
    CHECK(au_bw_key(w, &k));
    au_bw_scroll_pos(w, &sx, &sy);
    CHECK(sx == 56);
    /* A narrower window: a larger range, reported. */
    clear();
    au_bw_resize(w, 400, 400);
    CHECK(FIRED(AU_CHANGED_SCROLL | AU_CHANGED_LAYOUT));
    au_bw_scroll_max(w, &mx, &my);
    CHECK(mx == 1000 - 400 && my == 2000 - 400);
    CHECK(au_irect_empty(w->layout.hscroll));
    au_browser_free(b);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_page_only();
    test_page_clicks();
    test_actions();
    test_notices(true);
    test_notices(false);
    test_scrolling();
    au_fontset_free(fonts);
    printf("native_chrome: all tests passed\n");
    return 0;
}
