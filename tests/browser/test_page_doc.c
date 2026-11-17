/* Tests for the page adapter (src/browser/page_doc.c): real documents
 * through AuPage and the browser model. Asynchronous file loading and the
 * order resources are read in, titles, content size, link hover and
 * activation, fragments, back/forward restoring the scroll position,
 * about: pages, plain text and view-source, fetchers, stop, inner
 * scrolling boxes and relayout on resize. Uses the Ahem-like test font,
 * so every glyph is a 1em square. */
#include "../layout/ahem.h"
#include "browser/browser.h"
#include "browser/context.h"
#include "browser/credits.h"
#include "image/cache.h"

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
    {"sel.html",
     "<!DOCTYPE html><body style='margin:0;font-size:10px;line-height:20px'>"
     "<p style='margin:0'>alpha <a href='other.html'>beta</a> gamma</p><p style='margin:0'>delta findme</p>"
     "<div style='height:3000px'></div><p style='margin:0'>far findme</p>"},
    {"ctx.html",
     "<!DOCTYPE html><body style='margin:0;font-size:10px;line-height:30px'>"
     "<p style='margin:0'><a href='other.html'>link</a> <img src='pic.gif' width=40 height=20> "
     "<a href='tiny.html'><img src='dir/p2.gif' width=40 height=20></a></p><p style='margin:0'>text</p>"},
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
    AuBrowserHost h = {NULL, t_ms, t_now, t_read, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
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
    /* The document first, then its style sheet, its image. The sheet's
     * @import has print media, which does not match the screen: it is
     * not read. */
    const char *doc = strstr(read_log, "file:///samples/wiki.html");
    const char *css = strstr(read_log, "file:///samples/wiki.css");
    const char *img = strstr(read_log, "file:///samples/missing-platypus.jpg");
    const char *imp = strstr(read_log, "file:///samples/wiki-print.css");
    CHECK(doc == read_log && css && img && !imp);
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    CHECK(cw >= w->layout.page.x1 - w->layout.page.x0 && ch > 1500);
    AuPageStats st;
    au_page_stats(t->page, &st);
    CHECK(st.load_ms > 0 && st.layouts >= 1 && st.resources == 2 && st.boxes > 50);
    /* The image budget: the 8 MB memory cache, four times over on a
     * 64-bit build. */
    CHECK(st.image_budget_kb == 8192 * AU_IMAGE_BUDGET_SCALE);
    CHECK(sizeof(void *) < 8 || st.image_budget_kb == 32768);
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
    /* The document was kept (the document cache): shown at once. */
    CHECK(!t->loading && au_page_kept_count(t->page) == 1);
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

/* The start page: the search form from the template, most visited sites,
 * bookmarks. */
static void test_home(void)
{
    AuBrowser *b = make_browser();
    au_prefs_set_str(&b->prefs.search_template, "https://search.example/find?lang=en&q=%s&safe=a%20b");
    au_history_visit(&b->history, "https://often.example/page", "Often <visited>", 100);
    au_history_visit(&b->history, "https://often.example/page", "Often <visited>", 200);
    au_history_visit(&b->history, "https://once.example/", "Once", 300);
    au_bookmark_add(&b->bookmarks, NULL, NULL, "Marked", "https://marked.example/x", 0);
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "about:home", true, -1);
    const char *data;
    size_t len;
    CHECK(au_bw_page_source(w, &data, &len));
    AuBuf s;
    au_buf_init(&s);
    au_buf_append(&s, data, len);
    const char *html = au_buf_cstr(&s);
    CHECK(strstr(html, "action='https://search.example/find'") != NULL);
    CHECK(strstr(html, "type=search size=40 placeholder='Search the web' name='q'") != NULL);
    CHECK(strstr(html, "name='lang' value='en'") != NULL);
    CHECK(strstr(html, "name='safe' value='a b'") != NULL);
    CHECK(strstr(html, "Most visited") != NULL);
    CHECK(strstr(html, "Often &lt;visited&gt;") != NULL);
    CHECK(strstr(html, "once.example") == NULL); /* one visit is not enough */
    CHECK(strstr(html, "href='https://marked.example/x'") != NULL);
    CHECK(strstr(html, "data:image/png;base64,iVBORw0KGgo") != NULL);
    au_buf_free(&s);

    /* A template without a query parameter holding %s gets no form. */
    au_prefs_set_str(&b->prefs.search_template, "https://search.example/%s");
    au_bw_navigate(w, "about:home");
    CHECK(au_bw_page_source(w, &data, &len));
    au_buf_init(&s);
    au_buf_append(&s, data, len);
    CHECK(strstr(au_buf_cstr(&s), "<form") == NULL);
    au_buf_free(&s);
    au_browser_free(b);
}

/* ---- Network placeholder and fetchers ---- */

typedef struct FakeNet {
    const AuFetchSink *sink;
    uint32_t id;
    int in_start, polls, cancels;
    int step;
} FakeNet;

static uint32_t net_start(void *ctx, const AuFetchRequest *rq, const AuFetchSink *sink)
{
    FakeNet *n = ctx;
    (void)rq;
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
        AuFetchInfo info = {"http://example.net/final", "text/html", AU_FETCH_OK, 200, NULL, false, "", AU_FETCH_OK,
                            NULL, NULL};
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
    AuPageFetcher f = {&net, net_start, net_cancel, net_poll, NULL, NULL, NULL, NULL, NULL, NULL};
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
    /* Wider than the page: a horizontal scroll bar, sharing the corner. */
    {
        const AuChromeLayout *l = &w->layout;
        int pw = l->page.x1 - l->page.x0;
        CHECK(pw < 1000);
        CHECK(!au_irect_empty(l->hscroll) && !au_irect_empty(l->corner));
        CHECK(l->hscroll.y1 == l->status.y0 + 1 && l->page.y1 == l->hscroll.y0);
        CHECK(l->vscroll.y1 == l->hscroll.y0 + 1 && l->corner.x0 > l->page.x1);
        CHECK(au_chrome_hit(l, (l->hscroll.x0 + l->hscroll.x1) / 2, (l->hscroll.y0 + l->hscroll.y1) / 2).part ==
              AU_HIT_HSCROLL);
        CHECK(au_chrome_hit(l, (l->corner.x0 + l->corner.x1) / 2, (l->corner.y0 + l->corner.y1) / 2).part ==
              AU_HIT_CORNER);
        int mx, my;
        au_bw_scroll_max(w, &mx, &my);
        CHECK(mx == 1000 - pw);
        AuEvent e;
        memset(&e, 0, sizeof e);
        e.type = AU_EVENT_SCROLL;
        e.x = l->page.x0 + 10;
        e.y = l->page.y0 + 10;
        e.dy = 48;
        e.mods = AU_MOD_SHIFT; /* Shift-wheel scrolls sideways */
        au_bw_mouse(w, &e);
        CHECK(t2->scroll_x == 48 && t2->scroll_y == 0);
        paint(w);
    }
    au_bw_navigate(w, "file:///mem/other.html");
    settle(w);
    CHECK(au_irect_empty(w->layout.hscroll) && t2->scroll_x == 0);
    au_bw_navigate(w, "file:///mem/tiny.html");
    settle(w);
    b->prefs.font_size_min = 20;
    au_browser_prefs_changed(b);
    au_page_content_size(t2->page, &cw, &ch);
    CHECK(cw == 2000);
    b->prefs.font_size_min = 0;
    au_browser_free(b);
}

static void press(AuBrowserWindow *w, AuEventType type, int x, int y, uint32_t mods, int clicks)
{
    AuTab *t = au_bw_active_tab(w);
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.x = w->layout.page.x0 + x - t->scroll_x;
    e.y = w->layout.page.y0 + y - t->scroll_y;
    e.mods = mods;
    e.clicks = clicks;
    au_bw_mouse(w, &e);
}

static char *sel_text(AuBrowserWindow *w)
{
    static char buf[256];
    AuBuf b;
    au_buf_init(&b);
    buf[0] = 0;
    if (au_bw_selection_text(w, &b))
        snprintf(buf, sizeof buf, "%s", au_buf_cstr(&b));
    au_buf_free(&b);
    return buf;
}

/* Selecting with the mouse, copying, Select All and Find through the
 * page and the browser model. */
static void test_selection(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///mem/sel.html", true, -1);
    settle(w);
    AuTab *t = au_bw_active_tab(w);
    CHECK(!au_bw_has_selection(w) && !au_bw_find_again(w, true));

    /* Drag from "alpha" into the link: selects, and the link does not
     * open when released over it. */
    press(w, AU_EVENT_MOUSE_DOWN, 4, 10, 0, 1);
    press(w, AU_EVENT_MOUSE_MOVE, 40, 10, 0, 1);
    press(w, AU_EVENT_MOUSE_MOVE, 94, 10, 0, 1);
    press(w, AU_EVENT_MOUSE_UP, 94, 10, 0, 1);
    CHECK(au_bw_has_selection(w));
    CHECK_STR(sel_text(w), "alpha bet");
    CHECK_STR(au_page_url(t->page), "file:///mem/sel.html");
    paint(w);
    /* Shift-click extends to the next paragraph. */
    press(w, AU_EVENT_MOUSE_DOWN, 25, 30, AU_MOD_SHIFT, 1);
    press(w, AU_EVENT_MOUSE_UP, 25, 30, AU_MOD_SHIFT, 1);
    CHECK_STR(sel_text(w), "alpha beta gamma\nde");
    /* A plain click clears it; a double click selects a word, a triple
     * click the paragraph. */
    press(w, AU_EVENT_MOUSE_DOWN, 300, 30, 0, 1);
    press(w, AU_EVENT_MOUSE_UP, 300, 30, 0, 1);
    CHECK(!au_bw_has_selection(w));
    press(w, AU_EVENT_MOUSE_DOWN, 125, 10, 0, 2);
    press(w, AU_EVENT_MOUSE_UP, 125, 10, 0, 2);
    CHECK_STR(sel_text(w), "gamma");
    press(w, AU_EVENT_MOUSE_DOWN, 125, 10, 0, 3);
    press(w, AU_EVENT_MOUSE_MOVE, 30, 30, 0, 3); /* drag on: whole paragraphs */
    press(w, AU_EVENT_MOUSE_UP, 30, 30, 0, 3);
    CHECK_STR(sel_text(w), "alpha beta gamma\ndelta findme");

    au_bw_select_all(w);
    CHECK_STR(sel_text(w), "alpha beta gamma\ndelta findme\nfar findme");

    /* Find: from the selection's end, wrapping; the far match scrolls
     * into view. */
    press(w, AU_EVENT_MOUSE_DOWN, 300, 10, 0, 1);
    press(w, AU_EVENT_MOUSE_UP, 300, 10, 0, 1);
    CHECK(au_bw_find(w, "FINDME", false));
    CHECK_STR(sel_text(w), "findme");
    CHECK(t->scroll_y == 0);
    CHECK(au_bw_find_again(w, true));
    CHECK(t->scroll_y > 2500);
    CHECK(au_bw_find_again(w, true)); /* wraps to the first */
    CHECK(t->scroll_y == 0);
    CHECK(au_bw_find_again(w, false)); /* back: the last again */
    CHECK(t->scroll_y > 2500);
    CHECK(!au_bw_find(w, "FINDME", true)); /* case-sensitive: none */
    CHECK(!au_bw_find(w, "absent", false));
    CHECK_STR(sel_text(w), "findme"); /* a failed find keeps the selection */

    /* A new document clears the selection; a plain click opens a link. */
    au_bw_scroll_to(w, 0, 0);
    click_at(w, 70, 10, 0);
    CHECK_STR(au_page_url(t->page), "file:///mem/other.html");
    CHECK(!au_bw_has_selection(w));
    settle(w);
    au_browser_free(b);
}

static bool has_item(const AuContextMenu *m, AuCommand cmd, bool enabled)
{
    for (int i = 0; i < m->n; i++)
        if (m->items[i].cmd == cmd)
            return m->items[i].enabled == enabled;
    return false;
}

static int item_count(const AuContextMenu *m)
{
    int n = 0;
    for (int i = 0; i < m->n; i++)
        n += m->items[i].cmd != AU_CMD_SEPARATOR;
    return n;
}

/* Contextual menus: what each target offers and the model's commands. */
static void test_context_menu(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///mem/ctx.html", true, -1);
    settle(w);
    AuTab *t = au_bw_active_tab(w);
    AuContextMenu m;
    int px = w->layout.page.x0, py = w->layout.page.y0;

    /* A link. */
    CHECK(au_bw_context_menu(w, px + 15, py + 15, 0, &m));
    CHECK(m.kind == AU_CONTEXT_PAGE && m.link && !m.image);
    CHECK_STR(m.link ? m.link : "", "file:///mem/other.html");
    CHECK(has_item(&m, AU_CMD_OPEN_LINK, true) && has_item(&m, AU_CMD_OPEN_LINK_TAB, true) &&
          has_item(&m, AU_CMD_OPEN_LINK_WINDOW, true) && has_item(&m, AU_CMD_COPY_LINK, true) &&
          has_item(&m, AU_CMD_BOOKMARK_LINK, true));
    CHECK(item_count(&m) == 5 && !has_item(&m, AU_CMD_RELOAD, true));
    CHECK(!au_bw_context_command(w, &m, AU_CMD_COPY_LINK));         /* the shell's */
    CHECK(!au_bw_context_command(w, &m, AU_CMD_OPEN_LINK_WINDOW)); /* the shell's */
    CHECK(au_bw_context_command(w, &m, AU_CMD_BOOKMARK_LINK));
    CHECK(au_bookmark_find_url(&b->bookmarks, "file:///mem/other.html") != NULL);
    CHECK(au_bw_context_command(w, &m, AU_CMD_OPEN_LINK_TAB));
    CHECK(w->ntabs == 2 && au_bw_active_tab(w) == t); /* opened behind */
    CHECK_STR(au_page_url(w->tabs[1]->page), "file:///mem/other.html");
    au_context_menu_free(&m);

    /* An image; an image inside a link offers both. */
    CHECK(au_bw_context_menu(w, px + 70, py + 15, 0, &m));
    CHECK(!m.link && m.image);
    CHECK_STR(m.image ? m.image : "", "file:///mem/pic.gif");
    CHECK(has_item(&m, AU_CMD_OPEN_IMAGE_TAB, true) && has_item(&m, AU_CMD_COPY_IMAGE, true) && item_count(&m) == 2);
    CHECK(m.items[0].cmd != AU_CMD_SEPARATOR);
    au_context_menu_free(&m);
    CHECK(au_bw_context_menu(w, px + 120, py + 15, 0, &m));
    CHECK(m.link && m.image && item_count(&m) == 7);
    CHECK_STR(m.image ? m.image : "", "file:///mem/dir/p2.gif");
    CHECK_STR(m.link ? m.link : "", "file:///mem/tiny.html");
    au_context_menu_free(&m);

    /* The page: Back greyed on the first page, Copy only with a selection. */
    CHECK(au_bw_context_menu(w, px + 400, py + 200, 0, &m));
    CHECK(!m.link && !m.image);
    CHECK(has_item(&m, AU_CMD_BACK, false) && has_item(&m, AU_CMD_FORWARD, false) && has_item(&m, AU_CMD_RELOAD, true) &&
          has_item(&m, AU_CMD_VIEW_SOURCE, true) && has_item(&m, AU_CMD_BOOKMARK_PAGE, true));
    CHECK(!has_item(&m, AU_CMD_COPY, true));
    CHECK(m.items[m.n - 1].cmd != AU_CMD_SEPARATOR);
    au_context_menu_free(&m);
    au_bw_select_all(w);
    CHECK(au_bw_context_menu(w, px + 400, py + 200, 0, &m));
    CHECK(has_item(&m, AU_CMD_COPY, true));
    CHECK(!au_bw_context_command(w, &m, AU_CMD_COPY));
    CHECK(au_bw_context_command(w, &m, AU_CMD_VIEW_SOURCE));
    CHECK(w->ntabs == 3 && w->active == 1);
    CHECK_STR(au_page_url(au_bw_active_tab(w)->page), "view-source:file:///mem/ctx.html");
    au_context_menu_free(&m);
    settle(w);

    /* The address field: by what the shell reports. */
    AuIRect a = w->layout.address_text;
    CHECK(au_bw_context_menu(w, a.x0 + 5, a.y0 + 3, AU_CONTEXT_FIELD_TEXT, &m));
    CHECK(m.kind == AU_CONTEXT_FIELD);
    CHECK(has_item(&m, AU_CMD_CUT, false) && has_item(&m, AU_CMD_FIELD_COPY, false) && has_item(&m, AU_CMD_PASTE, false) &&
          has_item(&m, AU_CMD_PASTE_GO, false) && has_item(&m, AU_CMD_SELECT_ALL, true));
    CHECK(!au_bw_context_command(w, &m, AU_CMD_PASTE_GO));
    au_context_menu_free(&m);
    CHECK(au_bw_context_menu(w, a.x0 + 5, a.y0 + 3, AU_CONTEXT_FIELD_SELECTION | AU_CONTEXT_CLIPBOARD_TEXT, &m));
    CHECK(has_item(&m, AU_CMD_CUT, true) && has_item(&m, AU_CMD_PASTE_GO, true) && has_item(&m, AU_CMD_SELECT_ALL, false));
    au_context_menu_free(&m);

    /* Tabs: duplicate keeps the history; close others. */
    AuIRect r0 = au_chrome_tab_rect(&w->layout, 0);
    CHECK(au_bw_context_menu(w, r0.x0 + 10, (r0.y0 + r0.y1) / 2, 0, &m));
    CHECK(m.kind == AU_CONTEXT_TAB && m.tab == 0 && has_item(&m, AU_CMD_CLOSE_OTHER_TABS, true));
    au_bw_scroll_to(w, 0, 0);
    CHECK(au_bw_context_command(w, &m, AU_CMD_DUPLICATE_TAB));
    CHECK(w->ntabs == 4 && w->active == 1);
    CHECK_STR(au_page_url(w->tabs[1]->page), "file:///mem/ctx.html");
    CHECK(w->tabs[1]->nav.n == w->tabs[0]->nav.n);
    au_context_menu_free(&m);
    settle(w);
    CHECK(au_bw_context_menu(w, r0.x0 + 10, (r0.y0 + r0.y1) / 2, 0, &m));
    CHECK(au_bw_context_command(w, &m, AU_CMD_CLOSE_OTHER_TABS));
    CHECK(w->ntabs == 1 && w->active == 0 && w->tabs[0] == t);
    au_context_menu_free(&m);
    CHECK(au_bw_context_menu(w, r0.x0 + 10, (r0.y0 + r0.y1) / 2, 0, &m));
    CHECK(has_item(&m, AU_CMD_CLOSE_OTHER_TABS, false));
    au_context_menu_free(&m);

    /* The toolbar offers nothing. */
    CHECK(!au_bw_context_menu(w, w->layout.toolbar.x1 - 2, w->layout.toolbar.y0 + 1, 0, &m) && m.n == 0);
    settle(w);
    au_browser_free(b);
}

/* The session keeps each tab's scroll position and restores it. */
static void test_session_scroll(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///mem/other.html", true, -1);
    settle(w);
    au_bw_navigate(w, "file:///mem/sel.html");
    settle(w);
    uint32_t g = b->scroll_gen;
    au_bw_scroll_to(w, 0, 1200);
    CHECK(b->scroll_gen != g);
    AuBuf out;
    au_buf_init(&out);
    au_browser_session_serialize(b, &out);
    CHECK(strstr(au_buf_cstr(&out), "sel.html\t\t1200\n") != NULL);
    AuSession s;
    CHECK(au_session_parse(&s, out.data, out.len));
    CHECK(s.windows[0].tabs[0].nav.e[1].scroll_y == 1200);
    AuBrowserWindow *r = au_browser_window_new(b, 800, 600);
    au_bw_restore(r, &s.windows[0]);
    settle(r);
    CHECK(r->tabs[0]->scroll_y == 1200);
    au_session_free(&s);
    /* Files written before scroll positions still load. */
    static const char old[] = "# Aura session 1\nwindow 700 500 0\ntab 0\nentry\tfile:///mem/sel.html\tT\n";
    CHECK(au_session_parse(&s, old, sizeof old - 1));
    CHECK(s.nwindows == 1 && s.windows[0].tabs[0].nav.e[0].scroll_y == 0);
    CHECK_STR(s.windows[0].tabs[0].nav.e[0].title, "T");
    au_session_free(&s);
    au_buf_free(&out);
    au_browser_free(b);
}

static void window_click(AuBrowserWindow *w, AuIRect r, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.x = (r.x0 + r.x1) / 2;
    e.y = (r.y0 + r.y1) / 2;
    e.mods = mods;
    e.clicks = 1;
    e.type = AU_EVENT_MOUSE_DOWN;
    au_bw_mouse(w, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
}

/* Command-click on Back, the history pop-up's items, Reopen Closed Tab,
 * and the window position in the session. */
static void test_conveniences(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///mem/other.html", true, -1);
    settle(w);
    au_bw_navigate(w, "file:///mem/sel.html");
    settle(w);
    au_bw_navigate(w, "file:///mem/tiny.html");
    settle(w);
    AuHistoryItem items[8];
    CHECK(au_bw_history_items(w, 1, items, 8) == 0);
    CHECK(au_bw_history_items(w, -1, items, 8) == 2);
    CHECK(items[0].delta == -1 && items[1].delta == -2);
    CHECK_STR(items[0].url, "file:///mem/sel.html");
    CHECK(au_bw_history_items(w, -1, items, 1) == 1);

    /* Command-click Back: the previous page in a new tab behind. */
    AuTab *t = au_bw_active_tab(w);
    window_click(w, w->layout.back, AU_MOD_COMMAND);
    CHECK(w->ntabs == 2 && au_bw_active_tab(w) == t);
    CHECK_STR(au_page_url(t->page), "file:///mem/tiny.html");
    CHECK_STR(au_page_url(w->tabs[1]->page), "file:///mem/sel.html");
    CHECK(au_nav_can_go(&w->tabs[1]->nav, 1) && au_nav_can_go(&w->tabs[1]->nav, -1));
    settle(w);
    /* A plain click goes back in place. */
    window_click(w, w->layout.back, 0);
    CHECK(w->ntabs == 2);
    CHECK_STR(au_page_url(t->page), "file:///mem/sel.html");
    settle(w);

    /* Close the scrolled second tab, then reopen it. */
    au_bw_select_tab(w, 1);
    settle(w);
    au_bw_scroll_to(w, 0, 900);
    CHECK(!au_browser_can_reopen(b));
    au_bw_close_tab(w, 1);
    CHECK(w->ntabs == 1 && au_browser_can_reopen(b));
    AuTab *r = au_bw_reopen_closed_tab(w);
    CHECK(r && w->ntabs == 2 && au_bw_active_tab(w) == r);
    CHECK_STR(au_page_url(r->page), "file:///mem/sel.html");
    CHECK(r->nav.n == 3 && r->nav.index == 1);
    settle(w);
    CHECK(r->scroll_y == 900);
    CHECK(!au_browser_can_reopen(b) && !au_bw_reopen_closed_tab(w));
    /* Only the last AU_MAX_CLOSED_TABS are kept. */
    for (int i = 0; i < AU_MAX_CLOSED_TABS + 3; i++) {
        au_bw_new_tab(w, "about:blank", false, -1);
        au_bw_close_tab(w, w->ntabs - 1);
    }
    CHECK(b->nclosed == AU_MAX_CLOSED_TABS);

    /* The window's position goes into the session. */
    au_bw_set_position(w, 40, 60);
    AuBuf out;
    au_buf_init(&out);
    au_browser_session_serialize(b, &out);
    AuSession s;
    CHECK(au_session_parse(&s, out.data, out.len));
    CHECK(s.windows[0].x == 40 && s.windows[0].y == 60 && s.windows[0].width == 800);
    au_session_free(&s);
    static const char old[] = "window 700 500 0\ntab 0\nentry\tabout:home\t\n";
    CHECK(au_session_parse(&s, old, sizeof old - 1) && s.windows[0].x == -1 && s.windows[0].y == -1);
    au_session_free(&s);
    au_buf_free(&out);
    settle(w);
    au_browser_free(b);
}

static const char *find_mem(const char *name)
{
    for (size_t i = 0; i < sizeof mem_files / sizeof mem_files[0]; i++)
        if (!strcmp(mem_files[i].name, name))
            return mem_files[i].text;
    return NULL;
}

/* Save Page As: the bytes as they arrived, and the suggested name. */
static void test_save_source(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 800, 600);
    au_bw_new_tab(w, "file:///mem/ctx.html", true, -1);
    const char *data;
    size_t len;
    CHECK(!au_bw_page_source(w, &data, &len)); /* still loading */
    settle(w);
    CHECK(au_bw_page_source(w, &data, &len));
    const char *want = find_mem("ctx.html");
    CHECK(want && len == strlen(want) && !memcmp(data, want, len));
    char name[128];
    au_bw_save_name(w, name, sizeof name);
    CHECK_STR(name, "ctx.html");

    /* view-source: saves the source too, under the page's name. */
    au_bw_view_source(w);
    settle(w);
    CHECK(au_bw_page_source(w, &data, &len) && len == strlen(want) && !memcmp(data, want, len));
    au_bw_save_name(w, name, sizeof name);
    CHECK_STR(name, "ctx.html");

    /* Generated pages have their HTML; a missing file has none. */
    au_bw_navigate(w, "about:home");
    settle(w);
    CHECK(au_bw_page_source(w, &data, &len) && len > 0);
    au_bw_save_name(w, name, sizeof name);
    CHECK(strstr(name, ".html") != NULL);
    au_bw_navigate(w, "file:///mem/missing.html");
    settle(w);
    CHECK(!au_bw_page_source(w, &data, &len));
    au_bw_navigate(w, "about:scene");
    settle(w);
    CHECK(!au_bw_page_source(w, &data, &len));

    /* about:licences (the About box's Licences button) lists every credit. */
    au_bw_navigate(w, "about:licences");
    settle(w);
    CHECK_STR(au_page_title(au_bw_active_tab(w)->page), "Licences");
    CHECK(au_bw_page_source(w, &data, &len));
    char *src = au_strndup(data, len);
    for (size_t i = 0; i < au_ncredits; i++) {
        CHECK(au_credits[i].file || au_credits[i].text); /* every credit says where its licence is */
        if (au_credits[i].file)
            CHECK(strstr(src, au_credits[i].file) != NULL);
    }
    CHECK(strstr(src, "SIL Open Font License 1.1") && strstr(src, "Bitstream Vera"));
    CHECK(strstr(src, "Mbed TLS 4.1.1") && strstr(src, "Apache License") && strstr(src, "Public Suffix List") && strstr(src, "Brotli Authors") &&
          strstr(src, "Twemoji") && strstr(src, "https://mozilla.org/MPL/2.0/"));
    au_free(src);
    au_bw_navigate(w, "about:licenses");
    settle(w);
    CHECK_STR(au_page_title(au_bw_active_tab(w)->page), "Licences");
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
    test_home();
    test_network();
    test_stop_scroll_resize();
    test_zoom();
    test_selection();
    test_context_menu();
    test_save_source();
    test_session_scroll();
    test_conveniences();
    au_fontset_free(fonts);
    printf("page_doc: all tests passed\n");
    return 0;
}
