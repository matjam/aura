/* Tests for the page adapter (src/browser/page_doc.c): real documents
 * through AuPage and the browser model. Asynchronous file loading and the
 * order resources are read in, titles, content size, link hover and
 * activation, fragments, back/forward restoring the scroll position,
 * about: pages, plain text and view-source, fetchers, stop, inner
 * scrolling boxes and relayout on resize. Uses the Ahem-like test font,
 * so every glyph is a 1em square. */
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

/* ---- Files ----
 * file:///samples/NAME reads tests/page/samples/NAME; file:///mem/NAME
 * one of the documents below. Every read is logged in order. */

static const struct {
    const char *name, *text;
} mem_files[] = {
    {"links.html",
     "<!DOCTYPE html><title>Links</title><body style='margin:0'>"
     "<p style='margin:0'><a id=a href='other.html'>xxxx</a></p>"
     "<p style='margin:0'><a href='#sec'>yyyy</a> <a href='new.html' target=_blank>zz</a></p>"
     "<div style='height:1000px'></div><h2 id=sec style='margin:0;font-size:16px'>s</h2>"
     "<div style='height:2000px'></div>"},
    {"other.html", "<!DOCTYPE html><title>Other</title><body style='margin:0'><div style='height:3000px'>o</div>"},
    {"res.html",
     "<!DOCTYPE html><title>Res</title><link rel=stylesheet href=one.css><img src=pic.png>"
     "<link rel=stylesheet href=two.css><body style='margin:0'><div id=box>b</div>"},
    {"one.css", "@import 'three.css'; #box { height: 50px }"},
    {"two.css", "#box { width: 70px }"},
    {"three.css", "body { margin: 0 }"},
    /* A PNG header for a 40x30 image. */
    {"pic.png", "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0(\0\0\0\x1e"},
    {"notes.txt", "line <one>\r\nline & two\n"},
    {"big.html", "<!DOCTYPE html><body style='margin:0'><div style='font-size:50em;line-height:1'>x</div>"},
    {"tiny.html",
     "<!DOCTYPE html><body style='margin:0'><div style='white-space:nowrap;font-size:10px;line-height:1'>"
     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
     "</div>"},
    {"scroller.html",
     "<!DOCTYPE html><body style='margin:0'><div style='height:100px;overflow:auto'>"
     "<div style='height:1000px'>inner</div></div><div style='height:3000px'></div>"},
    {"wide.html", "<!DOCTYPE html><body style='margin:0'><p style='margin:0'>xx xx xx xx xx xx xx xx xx xx</p>"},
};

static char read_log[2048];

static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    snprintf(read_log + strlen(read_log), sizeof read_log - strlen(read_log), "%s ", url);
    if (!strncmp(url, "file:///mem/", 12)) {
        for (size_t i = 0; i < sizeof mem_files / sizeof mem_files[0]; i++) {
            if (!strcmp(url + 12, mem_files[i].name)) {
                size_t n = !strcmp(mem_files[i].name, "pic.png") ? 24 : strlen(mem_files[i].text);
                *data = au_alloc(n + 1);
                memcpy(*data, mem_files[i].text, n);
                *len = n;
                return true;
            }
        }
        return false;
    }
    if (!strncmp(url, "file:///samples/", 16)) {
        char path[1024];
        snprintf(path, sizeof path, "%s/tests/page/samples/%s", AURA_SOURCE_DIR, url + 16);
        FILE *f = fopen(path, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        *data = au_alloc((size_t)n + 1);
        *len = fread(*data, 1, (size_t)n, f);
        fclose(f);
        return true;
    }
    return false;
}

static uint32_t t_ms(void *ctx) { (void)ctx; return fake_ms; }
static AuTime t_now(void *ctx) { (void)ctx; return 0; }

static AuBrowser *make_browser(void)
{
    AuBrowserHost h = {NULL, t_ms, t_now, t_read, NULL, NULL, NULL};
    return au_browser_new(fonts, &h);
}

static void settle(AuBrowserWindow *w)
{
    for (int i = 0; i < 400 && au_browser_busy(w->browser); i++) {
        fake_ms += 50;
        au_bw_idle(w);
    }
    CHECK(!au_browser_busy(w->browser));
}

static void paint(AuBrowserWindow *w)
{
    AuSurface *s = au_surface_new(w->width, w->height);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, w->width, w->height});
    au_canvas_free(c);
    au_surface_free(s);
}

/* Window coordinates of document point (x, y) in the active tab. */
static void mouse_at(AuBrowserWindow *w, AuEventType type, int x, int y, uint32_t mods)
{
    AuTab *t = au_bw_active_tab(w);
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.x = w->layout.page.x0 + x - t->scroll_x;
    e.y = w->layout.page.y0 + y - t->scroll_y;
    e.mods = mods;
    au_bw_mouse(w, &e);
}

static void click_at(AuBrowserWindow *w, int x, int y, uint32_t mods)
{
    mouse_at(w, AU_EVENT_MOUSE_DOWN, x, y, mods);
    mouse_at(w, AU_EVENT_MOUSE_UP, x, y, mods);
}

/* ---- A real sample: asynchronous loading and resources ---- */

static void test_sample(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    read_log[0] = 0;
    au_bw_new_tab(w, "file:///samples/wiki.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    /* Nothing is read inside navigate. */
    CHECK(read_log[0] == 0);
    CHECK(t->loading && au_page_loading(t->page));
    settle(w);
    CHECK(!t->loading);
    CHECK_STR(au_page_title(t->page), "Platypus - Encyclopedia sample");
    CHECK_STR(au_bw_field_text(w), "file:///samples/wiki.html");
    /* The document first, then its style sheet, its image; the sheet's
     * @import (print media) is read after the sheet itself. */
    const char *doc = strstr(read_log, "file:///samples/wiki.html");
    const char *css = strstr(read_log, "file:///samples/wiki.css");
    const char *img = strstr(read_log, "file:///samples/missing-platypus.jpg");
    const char *imp = strstr(read_log, "file:///samples/wiki-print.css");
    CHECK(doc == read_log && css && img && imp);
    CHECK(css < imp);
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    CHECK(cw >= w->layout.page.x1 - w->layout.page.x0 && ch > 1500);
    AuPageStats st;
    au_page_stats(t->page, &st);
    CHECK(st.load_ms > 0 && st.layouts >= 1 && st.resources == 3 && st.boxes > 50);
    paint(w);
    au_page_stats(t->page, &st);
    CHECK(st.first_paint_ms > 0);
    /* Scrolling keys reach the end of the long page. */
    AuEvent k;
    memset(&k, 0, sizeof k);
    k.type = AU_EVENT_KEY_DOWN;
    k.key = AU_KEY_END;
    au_bw_key(w, &k);
    int mx, my;
    au_bw_scroll_max(w, &mx, &my);
    CHECK(my > 0 && t->scroll_y == my);
    au_browser_free(b);
}

/* Resources load in the order they are requested and style sheets apply
 * in document order; image sizes come from the file header. */
static void test_resources(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    read_log[0] = 0;
    au_bw_new_tab(w, "file:///mem/res.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    float last = -1;
    bool rising = true;
    for (int i = 0; i < 100 && au_browser_busy(b); i++) {
        fake_ms += 50;
        au_bw_idle(w);
        float pr = t->loading ? au_page_progress(t->page) : 1;
        rising &= pr >= last;
        last = pr;
    }
    CHECK(rising && !t->loading);
    /* Sheets as the parser finds them, an @import once its sheet is in,
     * the image when layout first needs its size. */
    CHECK_STR(read_log, "file:///mem/res.html file:///mem/one.css file:///mem/two.css file:///mem/three.css "
                        "file:///mem/pic.png ");
    CHECK_STR(au_page_title(t->page), "Res");
    au_browser_free(b);
}

/* ---- Links, hover, fragments, back and forward ---- */

static int status_has(AuTab *t, const char *s)
{
    return strcmp(t->status, s) == 0;
}

static void test_links(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///mem/links.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    settle(w);
    CHECK_STR(au_page_title(t->page), "Links");
    /* Hover: the link's URL in the status bar; away clears it. */
    mouse_at(w, AU_EVENT_MOUSE_MOVE, 8, 8, 0);
    CHECK(status_has(t, "file:///mem/other.html"));
    mouse_at(w, AU_EVENT_MOUSE_MOVE, 700, 500, 0);
    CHECK(status_has(t, ""));

    /* A fragment link scrolls within the document; no reload. */
    mouse_at(w, AU_EVENT_MOUSE_MOVE, 8, 24, 0);
    CHECK(status_has(t, "file:///mem/links.html#sec"));
    click_at(w, 8, 24, 0);
    CHECK_STR(au_page_url(t->page), "file:///mem/links.html#sec");
    CHECK(status_has(t, "")); /* the pointer now rests on no link */
    CHECK(!t->loading && t->nav.n == 2);
    CHECK(t->scroll_y == 1032);
    /* Back to the top entry: the same document, at its old position. */
    au_bw_back(w);
    CHECK(!t->loading && t->scroll_y == 0);
    au_bw_forward(w);
    CHECK(t->scroll_y == 1032);

    /* Scroll somewhere, follow a link, come back: the position returns
     * once the document has loaded again. */
    au_bw_scroll_to(w, 0, 400);
    au_bw_back(w);
    CHECK(t->scroll_y == 0);
    au_bw_scroll_to(w, 0, 400);
    au_bw_navigate(w, "file:///mem/other.html");
    CHECK(t->loading && t->scroll_y == 0);
    CHECK(au_nav_current(&t->nav)[-1].scroll_y == 400);
    settle(w);
    CHECK_STR(au_page_title(t->page), "Other");
    CHECK(t->scroll_y == 0);
    au_bw_scroll_to(w, 0, 900);
    au_bw_back(w);
    CHECK(t->loading);
    settle(w);
    CHECK_STR(au_page_title(t->page), "Links");
    CHECK(t->scroll_y == 400);
    au_bw_forward(w);
    settle(w);
    CHECK(t->scroll_y == 900);
    au_bw_back(w);
    settle(w);

    /* Reload keeps the position. */
    au_bw_reload(w);
    settle(w);
    CHECK(t->scroll_y == 400);

    /* target=_blank and Command-click open new tabs (in the background). */
    au_bw_scroll_to(w, 0, 0);
    click_at(w, 8 + 5 * 16, 24, 0);
    CHECK(w->ntabs == 2 && w->active == 0);
    CHECK_STR(au_page_url(w->tabs[1]->page), "file:///mem/new.html");
    click_at(w, 8, 8, AU_MOD_COMMAND);
    CHECK(w->ntabs == 3);
    CHECK_STR(au_page_url(w->tabs[1]->page), "file:///mem/other.html");
    settle(w);
    /* A missing file shows an error page and does not hang. */
    CHECK_STR(au_page_title(w->tabs[2]->page), "File not found");

    /* A typed URL with a fragment scrolls once the document has loaded. */
    au_bw_navigate(w, "file:///mem/links.html");
    settle(w);
    au_bw_navigate(w, "file:///mem/other.html");
    settle(w);
    au_bw_navigate(w, "file:///mem/links.html#sec");
    CHECK(t->loading);
    settle(w);
    CHECK(t->scroll_y == 1032);
    au_browser_free(b);
}

/* ---- about: pages, plain text, view-source ---- */

static void test_about(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "about:home", true, -1);
    AuTab *t = au_bw_active_tab(w);
    /* Generated pages load at once. */
    CHECK(!t->loading);
    CHECK_STR(au_page_title(t->page), "Aura");
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    CHECK(ch >= w->layout.page.y1 - w->layout.page.y0 && cw >= w->layout.page.x1 - w->layout.page.x0);
    au_bw_navigate(w, "about:fonts");
    CHECK_STR(au_page_title(t->page), "Fonts");
    au_bw_navigate(w, "about:scene");
    CHECK_STR(au_page_title(t->page), "Test Scene");
    au_page_content_size(t->page, &cw, &ch);
    CHECK(cw == 600 && ch > 600);
    paint(w);
    au_bw_navigate(w, "about:blank");
    CHECK_STR(au_page_title(t->page), "");
    CHECK_STR(au_bw_field_text(w), "");
    au_bw_navigate(w, "about:nothing");
    CHECK_STR(au_page_title(t->page), "Unknown page");
    au_bw_navigate(w, "gopher://example.com/");
    CHECK_STR(au_page_title(t->page), "Cannot open");

    au_bw_navigate(w, "file:///mem/notes.txt");
    settle(w);
    CHECK_STR(au_page_title(t->page), "notes.txt");
    au_bw_navigate(w, "view-source:file:///mem/links.html");
    settle(w);
    CHECK_STR(au_page_title(t->page), "Source of links.html");
    /* The source is text: no links to hover. */
    mouse_at(w, AU_EVENT_MOUSE_MOVE, 8, 8, 0);
    CHECK(status_has(t, ""));
    paint(w);
    au_browser_free(b);
}

/* ---- Network placeholder and fetchers ---- */

typedef struct FakeNet {
    const AuFetchSink *sink;
    uint32_t id;
    int in_start, polls, cancels;
    int step;
} FakeNet;

static uint32_t net_start(void *ctx, const char *url, const AuFetchSink *sink)
{
    FakeNet *n = ctx;
    (void)url;
    n->in_start = 1;
    n->sink = sink;
    n->step = 0;
    n->in_start = 0;
    return ++n->id;
}

static void net_cancel(void *ctx, uint32_t id)
{
    FakeNet *n = ctx;
    (void)id;
    n->cancels++;
    n->sink = NULL;
}

static void net_poll(void *ctx)
{
    FakeNet *n = ctx;
    CHECK(!n->in_start);
    n->polls++;
    if (!n->sink)
        return;
    const AuFetchSink *s = n->sink;
    if (n->step == 0) {
        s->data(s->ctx, n->id, "<!DOCTYPE html><title>Net", 25);
    } else if (n->step == 1) {
        s->data(s->ctx, n->id, " page</title><p>hello", 21);
    } else {
        AuFetchInfo info = {"http://example.net/final", "text/html"};
        n->sink = NULL;
        s->done(s->ctx, n->id, true, &info);
    }
    n->step++;
}

static void test_network(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    /* No fetcher: the placeholder, after a simulated load. */
    au_bw_new_tab(w, "http://example.com/", true, -1);
    AuTab *t = au_bw_active_tab(w);
    CHECK(t->loading && au_page_security(t->page) == AU_SECURITY_NOT_SECURE);
    settle(w);
    CHECK_STR(au_page_title(t->page), "example.com");
    au_bw_navigate(w, "http://slow.example/");
    fake_ms += 300;
    au_bw_idle(w);
    CHECK(t->loading && au_page_progress(t->page) > 0);
    au_bw_stop(w);
    CHECK(!t->loading);

    /* A registered fetcher. */
    FakeNet net;
    memset(&net, 0, sizeof net);
    AuPageFetcher f = {&net, net_start, net_cancel, net_poll};
    au_browser_set_fetcher(b, "http", &f);
    au_bw_navigate(w, "http://example.net/start");
    CHECK(t->loading && net.id == 1 && net.polls == 0);
    settle(w);
    CHECK_STR(au_page_title(t->page), "Net page");
    CHECK_STR(au_page_url(t->page), "http://example.net/final"); /* redirect reported */
    CHECK_STR(au_nav_current(&t->nav)->url, "http://example.net/final");
    /* Navigating away mid-transfer cancels it. */
    au_bw_navigate(w, "http://example.net/again");
    fake_ms += 50;
    au_bw_idle(w);
    au_bw_navigate(w, "about:blank");
    CHECK(net.cancels == 1);
    au_browser_set_fetcher(b, "http", NULL);
    au_bw_navigate(w, "http://example.com/");
    CHECK(net.id == 2);
    au_browser_free(b);
}

/* ---- Stop, inner scrolling, resize ---- */

static void test_stop_scroll_resize(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    /* Stop keeps what arrived (here the first slice of the document). */
    au_bw_new_tab(w, "file:///samples/hn.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    fake_ms += 50;
    au_bw_idle(w);
    au_bw_stop(w);
    CHECK(!t->loading && !au_browser_busy(b));
    CHECK_STR(au_page_title(t->page), "Sample News");
    paint(w);

    /* The wheel scrolls an inner box under the pointer first. */
    au_bw_navigate(w, "file:///mem/scroller.html");
    settle(w);
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_SCROLL;
    e.x = w->layout.page.x0 + 10;
    e.y = w->layout.page.y0 + 10;
    e.dy = 48;
    au_bw_mouse(w, &e);
    CHECK(t->scroll_y == 0);
    e.y = w->layout.page.y0 + 300;
    au_bw_mouse(w, &e);
    CHECK(t->scroll_y == 48);

    /* Narrowing the window wraps the text again. */
    au_bw_navigate(w, "file:///mem/wide.html");
    settle(w);
    au_bw_resize(w, 120, 600);
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    int vw = w->layout.page.x1 - w->layout.page.x0;
    CHECK(cw <= vw + 1);
    au_bw_resize(w, 800, 600);
    paint(w);
    /* Font preferences restyle the page. */
    b->prefs.font_size = 20;
    au_browser_prefs_changed(b);
    paint(w);
    au_browser_free(b);
}

/* Font preferences reach the resolver (default size, minimum size), and
 * text zoom is per tab and remembered per site. */
static void test_zoom(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///mem/big.html", true, -1);
    settle(w);
    AuTab *t = au_bw_active_tab(w);
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    CHECK(ch == 800); /* 50em of a 16px default */
    b->prefs.font_size = 20;
    au_browser_prefs_changed(b);
    au_page_content_size(t->page, &cw, &ch);
    CHECK(ch == 1000);
    b->prefs.font_size = 16;
    au_browser_prefs_changed(b);

    CHECK(!au_bw_can_zoom(w, 0) && au_bw_can_zoom(w, 1) && au_bw_can_zoom(w, -1));
    au_bw_zoom(w, 1);
    CHECK(au_page_zoom(t->page) > 1.09f && au_page_zoom(t->page) < 1.11f);
    au_page_content_size(t->page, &cw, &ch);
    CHECK(ch == 880);
    CHECK(au_bw_can_zoom(w, 0));
    /* The site (file:) keeps its zoom: another page of it, and a new tab. */
    float sz = au_browser_site_zoom(b, "file:///anything.html");
    CHECK(sz > 1.09f && sz < 1.11f);
    au_bw_navigate(w, "file:///mem/links.html");
    CHECK(au_page_zoom(t->page) > 1.09f);
    au_bw_new_tab(w, "about:home", true, -1);
    CHECK(au_page_zoom(au_bw_active_tab(w)->page) == 1.0f);
    au_bw_new_tab(w, "file:///mem/big.html", true, -1);
    settle(w);
    AuTab *t2 = au_bw_active_tab(w);
    au_page_content_size(t2->page, &cw, &ch);
    CHECK(ch == 880);
    /* Actual Size forgets the site. */
    au_bw_zoom(w, 0);
    au_page_content_size(t2->page, &cw, &ch);
    CHECK(ch == 800);
    CHECK(au_browser_site_zoom(b, "file:///x") == 1.0f);
    /* Steps stop at the ends. */
    for (int i = 0; i < 20; i++)
        au_bw_zoom(w, -1);
    CHECK(au_page_zoom(t2->page) == AU_PAGE_ZOOM_MIN && !au_bw_can_zoom(w, -1));
    au_bw_zoom(w, 0);

    /* Minimum font size. */
    au_bw_navigate(w, "file:///mem/tiny.html");
    settle(w);
    au_page_content_size(t2->page, &cw, &ch);
    CHECK(cw == 1000);
    b->prefs.font_size_min = 20;
    au_browser_prefs_changed(b);
    au_page_content_size(t2->page, &cw, &ch);
    CHECK(cw == 2000);
    b->prefs.font_size_min = 0;
    au_browser_free(b);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_sample();
    test_resources();
    test_links();
    test_about();
    test_network();
    test_stop_scroll_resize();
    test_zoom();
    au_fontset_free(fonts);
    printf("page_doc: all tests passed\n");
    return 0;
}
