/* Document view tests: URL resolution, the asynchronous loader, style
 * sheet order, <base> and <title>, fragments, hover and link activation,
 * incremental feeding, resize. Uses the Ahem-like test font. */
#include "../layout/ahem.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);                                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static AuFontSet *fonts;

/* ---- URL resolution ---- */

static void url_case(const char *base, const char *rel, const char *want)
{
    char out[512];
    bool ok = au_url_resolve(base, rel, out, sizeof out);
    CHECK(ok && strcmp(out, want) == 0, "resolve(%s, %s) = %s, want %s", base, rel, ok ? out : "(fail)", want);
}

static void test_urls(void)
{
    const char *b = "http://a/b/c/d;p?q";
    /* RFC 3986 5.4.1 normal examples */
    url_case(b, "g:h", "g:h");
    url_case(b, "g", "http://a/b/c/g");
    url_case(b, "./g", "http://a/b/c/g");
    url_case(b, "g/", "http://a/b/c/g/");
    url_case(b, "/g", "http://a/g");
    url_case(b, "//g", "http://g");
    url_case(b, "?y", "http://a/b/c/d;p?y");
    url_case(b, "g?y", "http://a/b/c/g?y");
    url_case(b, "#s", "http://a/b/c/d;p?q#s");
    url_case(b, "g#s", "http://a/b/c/g#s");
    url_case(b, "g?y#s", "http://a/b/c/g?y#s");
    url_case(b, ";x", "http://a/b/c/;x");
    url_case(b, "g;x", "http://a/b/c/g;x");
    url_case(b, "", "http://a/b/c/d;p?q");
    url_case(b, ".", "http://a/b/c/");
    url_case(b, "./", "http://a/b/c/");
    url_case(b, "..", "http://a/b/");
    url_case(b, "../", "http://a/b/");
    url_case(b, "../g", "http://a/b/g");
    url_case(b, "../..", "http://a/");
    url_case(b, "../../", "http://a/");
    url_case(b, "../../g", "http://a/g");
    /* abnormal examples */
    url_case(b, "../../../g", "http://a/g");
    url_case(b, "/./g", "http://a/g");
    url_case(b, "/../g", "http://a/g");
    url_case(b, "g.", "http://a/b/c/g.");
    url_case(b, "g..", "http://a/b/c/g..");
    url_case(b, "./../g", "http://a/b/g");
    url_case(b, "g/./h", "http://a/b/c/g/h");
    url_case(b, "g/../h", "http://a/b/c/h");
    /* forms pages use */
    url_case("https://example.com", "page", "https://example.com/page");
    url_case("https://example.com/dir/x.html#frag", "#other", "https://example.com/dir/x.html#other");
    url_case("https://example.com/dir/", "  spaced.html ", "https://example.com/dir/spaced.html");
    url_case("file:///Users/me/site/index.html", "css/a.css", "file:///Users/me/site/css/a.css");
    url_case("file:///Users/me/site/index.html", "/top.css", "file:///top.css");
    url_case("http://h/a", "HTTP://Other/x", "HTTP://Other/x");
    url_case("http://h/a", "mailto:x@y", "mailto:x@y");
    url_case("http://h/a/b", "//cdn.example/x.js", "http://cdn.example/x.js");
    char small[8];
    CHECK(!au_url_resolve("http://example.com/", "long/path", small, sizeof small), "overflow not reported");
}

/* ---- A fake asynchronous loader ---- */

typedef struct FakeReq {
    uint32_t id;
    char url[256];
    AuResourceKind kind;
} FakeReq;

typedef struct Fake {
    FakeReq req[32];
    int n;
    uint32_t next;
    int in_request; /* data must never arrive from inside request() */
    char opened[256];
    bool opened_new_tab;
    char title[128];
    char status[256];
    int finished;
} Fake;

static uint32_t fake_request(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    (void)dv;
    Fake *f = ctx;
    if (f->n == 32)
        return 0;
    FakeReq *r = &f->req[f->n++];
    r->id = ++f->next;
    snprintf(r->url, sizeof r->url, "%s", url);
    r->kind = kind;
    return r->id;
}

static FakeReq *fake_find(Fake *f, const char *url)
{
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->req[i].url, url))
            return &f->req[i];
    return NULL;
}

static void fake_deliver(Fake *f, AuDocView *dv, const char *url, const char *data, bool ok)
{
    FakeReq *r = fake_find(f, url);
    CHECK(r != NULL, "no request for %s", url);
    if (!r)
        return;
    if (data)
        au_docview_resource_data(dv, r->id, data, strlen(data));
    au_docview_resource_done(dv, r->id, ok);
}

static void cb_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    (void)dv;
    Fake *f = ctx;
    snprintf(f->opened, sizeof f->opened, "%s", url);
    f->opened_new_tab = new_tab;
}

static void cb_title(void *ctx, AuDocView *dv, const char *t)
{
    (void)dv;
    snprintf(((Fake *)ctx)->title, 128, "%s", t);
}

static void cb_status(void *ctx, AuDocView *dv, const char *t)
{
    (void)dv;
    snprintf(((Fake *)ctx)->status, 256, "%s", t);
}

static void cb_finished(void *ctx, AuDocView *dv)
{
    (void)dv;
    ((Fake *)ctx)->finished++;
}

static AuDocView *new_view(Fake *f)
{
    memset(f, 0, sizeof *f);
    static AuLoader li;
    li.ctx = f;
    li.request = fake_request;
    static AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = f;
    cb.open_url = cb_open;
    cb.title_changed = cb_title;
    cb.status_text = cb_status;
    cb.load_finished = cb_finished;
    return au_docview_new(fonts, &li, &cb);
}

static AuElement *by_id(AuDocView *dv, const char *id)
{
    AuDocument *doc = au_docview_document(dv);
    AuAtom a = au_atom_lookup(id, strlen(id));
    for (AuNode *n = au_node_next(&doc->node, &doc->node); n; n = au_node_next(n, &doc->node))
        if (a && au_is_element(n) && au_as_element(n)->id == a)
            return au_as_element(n);
    return NULL;
}

static AuRect rect_of(AuDocView *dv, const char *id)
{
    AuRect r = {-1, -1, -1, -1};
    au_docview_update(dv);
    AuElement *el = by_id(dv, id);
    if (el)
        au_layout_element_rect(au_docview_layout(dv), el, &r);
    return r;
}

/* ---- Style sheets ---- */

static void test_sheets(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><html><head>"
                       "<link rel=stylesheet href=a.css>"
                       "<style>#t{height:10px}</style>"
                       "<link rel=stylesheet href=b.css>"
                       "<link rel='alternate stylesheet' href=alt.css>"
                       "</head><body style='margin:0'><div id=t></div></body>";
    au_docview_load(dv, html, strlen(html), "http://example.com/dir/page.html");
    au_docview_set_viewport(dv, 800, 600);
    CHECK(f.n == 2, "%d requests, want 2", f.n);
    CHECK(fake_find(&f, "http://example.com/dir/a.css") != NULL, "a.css not requested");
    CHECK(fake_find(&f, "http://example.com/dir/b.css") != NULL, "b.css not requested");
    CHECK(fake_find(&f, "http://example.com/dir/alt.css") == NULL, "alternate sheet requested");
    CHECK(au_docview_loading(dv), "not loading while sheets pending");
    CHECK(rect_of(dv, "t").h == 10, "inline sheet not applied before others arrive");
    /* b arrives first, then a: b must still win (document order). */
    fake_deliver(&f, dv, "http://example.com/dir/b.css", "#t{height:30px} @import 'c.css';", true);
    CHECK(rect_of(dv, "t").h == 30, "b.css not applied: h=%g", rect_of(dv, "t").h);
    fake_deliver(&f, dv, "http://example.com/dir/a.css", "#t{height:20px;width:100px}", true);
    AuRect r = rect_of(dv, "t");
    CHECK(r.h == 30 && r.w == 100, "order: got %gx%g, want 100x30", r.w, r.h);
    /* @import inside b.css is requested once b.css has arrived; misplaced
     * @import (after a rule) is ignored. */
    CHECK(fake_find(&f, "http://example.com/dir/c.css") == NULL, "misplaced @import requested");
    CHECK(f.finished == 1, "load_finished called %d times", f.finished);
    CHECK(!au_docview_loading(dv), "still loading");
    au_docview_free(dv);

    dv = new_view(&f);
    html = "<!DOCTYPE html><style>@import url(\"i.css\"); #t{height:5px}</style><body style='margin:0'><div id=t></div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    CHECK(fake_find(&f, "http://example.com/i.css") != NULL, "@import in <style> not requested");
    fake_deliver(&f, dv, "http://example.com/i.css", "#t{height:50px;width:10px}", true);
    r = rect_of(dv, "t");
    CHECK(r.h == 5 && r.w == 10, "@import cascade order: %gx%g", r.w, r.h);
    au_docview_free(dv);

    /* A failed sheet does not block the page. */
    dv = new_view(&f);
    html = "<!DOCTYPE html><link rel=stylesheet href=missing.css><body style='margin:0'><div id=t style='height:7px'></div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    fake_deliver(&f, dv, "http://example.com/missing.css", NULL, false);
    CHECK(rect_of(dv, "t").h == 7 && !au_docview_loading(dv), "failed sheet");
    au_docview_free(dv);
}

/* ---- Base, title, fragments ---- */

static void test_base_title_fragments(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><title>  The\n  Title </title><base href='http://other.example/x/'>"
                       "<body style='margin:0'><div style='height:1000px'></div>"
                       "<h2 id=sec style='margin:0'>s</h2><a name=old></a><div style='height:3000px'></div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/page.html");
    au_docview_set_viewport(dv, 800, 600);
    CHECK(!strcmp(au_docview_title(dv), "The Title"), "title '%s'", au_docview_title(dv));
    CHECK(!strcmp(f.title, "The Title"), "title callback '%s'", f.title);
    CHECK(!strcmp(au_docview_base_url(dv), "http://other.example/x/"), "base '%s'", au_docview_base_url(dv));
    CHECK(!strcmp(au_docview_url(dv), "http://example.com/page.html"), "url changed");
    CHECK(au_docview_find_fragment(dv, "sec") == by_id(dv, "sec"), "fragment by id");
    CHECK(au_docview_find_fragment(dv, "old") != NULL, "fragment by <a name>");
    CHECK(au_docview_find_fragment(dv, "nope") == NULL, "missing fragment found");
    CHECK(au_docview_fragment_y(dv, "sec") == 1000, "fragment y %d", au_docview_fragment_y(dv, "sec"));
    CHECK(au_docview_fragment_y(dv, "nope") == -1, "missing fragment y");
    au_docview_free(dv);
}

/* ---- Hover and links ---- */

static void mouse(AuDocView *dv, AuEventType t, int x, int y, uint32_t mods)
{
    AuEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = t;
    ev.x = x;
    ev.y = y;
    ev.mods = mods;
    au_docview_mouse(dv, &ev);
}

static void test_input(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><style>a:hover{display:inline-block;width:100px} a:active{height:50px}</style>"
                       "<body style='margin:0'><p style='margin:0'><a id=l href='next.html'>xx</a></p>"
                       "<p style='margin:0'><a id=n href='/new' target=_blank>xx</a></p>"
                       "<p style='margin:0'><a id=j href='#frag'>xx</a></p>";
    au_docview_load(dv, html, strlen(html), "http://example.com/dir/page.html");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    CHECK(rect_of(dv, "l").w == 32, "link width before hover %g", rect_of(dv, "l").w);
    mouse(dv, AU_EVENT_MOUSE_MOVE, 5, 5, 0);
    CHECK(rect_of(dv, "l").w == 100, "hover restyle: width %g", rect_of(dv, "l").w);
    CHECK(!strcmp(au_docview_hover_link(dv), "http://example.com/dir/next.html"), "hover link '%s'",
          au_docview_hover_link(dv));
    CHECK(!strcmp(f.status, "http://example.com/dir/next.html"), "status '%s'", f.status);
    mouse(dv, AU_EVENT_MOUSE_DOWN, 5, 5, 0);
    CHECK(rect_of(dv, "l").h == 50, "active restyle: height %g", rect_of(dv, "l").h);
    mouse(dv, AU_EVENT_MOUSE_UP, 5, 5, 0);
    CHECK(!strcmp(f.opened, "http://example.com/dir/next.html") && !f.opened_new_tab, "open '%s'", f.opened);
    CHECK(rect_of(dv, "l").h != 50, "active state not cleared");
    /* move away: hover cleared, status cleared */
    mouse(dv, AU_EVENT_MOUSE_MOVE, 700, 500, 0);
    CHECK(rect_of(dv, "l").w == 32, "hover not cleared");
    CHECK(f.status[0] == 0, "status not cleared '%s'", f.status);
    /* target=_blank and Command-click open in a new tab */
    f.opened[0] = 0;
    mouse(dv, AU_EVENT_MOUSE_DOWN, 5, 21, 0);
    mouse(dv, AU_EVENT_MOUSE_UP, 5, 21, 0);
    CHECK(!strcmp(f.opened, "http://example.com/new") && f.opened_new_tab, "target=_blank '%s'", f.opened);
    f.opened[0] = 0;
    mouse(dv, AU_EVENT_MOUSE_DOWN, 5, 37, AU_MOD_COMMAND);
    mouse(dv, AU_EVENT_MOUSE_UP, 5, 37, AU_MOD_COMMAND);
    CHECK(!strcmp(f.opened, "http://example.com/dir/page.html#frag") && f.opened_new_tab, "cmd-click '%s'",
          f.opened);
    /* press on one link, release elsewhere: nothing opens */
    f.opened[0] = 0;
    mouse(dv, AU_EVENT_MOUSE_DOWN, 5, 5, 0);
    mouse(dv, AU_EVENT_MOUSE_UP, 700, 500, 0);
    CHECK(f.opened[0] == 0, "drag-off opened '%s'", f.opened);
    au_docview_free(dv);
}

/* ---- Incremental feeding and resize ---- */

static void test_incremental(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><title>T</title><style>#a{height:10px}</style>"
                       "<body style='margin:0'><div id=a></div><style>#b{height:20px}</style><div id=b></div>"
                       "<p id=p style='margin:0'>xx xx xx xx xx xx</p>";
    au_docview_begin(dv, "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    size_t n = strlen(html);
    /* feed one byte at a time, updating layout as we go */
    for (size_t i = 0; i < n; i++) {
        au_docview_feed(dv, html + i, 1);
        if (i % 7 == 0)
            au_docview_update(dv);
    }
    CHECK(au_docview_loading(dv), "loading before finish");
    au_docview_finish(dv);
    CHECK(!au_docview_loading(dv), "loading after finish");
    CHECK(rect_of(dv, "a").h == 10 && rect_of(dv, "b").h == 20, "sheets fed in pieces: %g %g", rect_of(dv, "a").h,
          rect_of(dv, "b").h);
    CHECK(rect_of(dv, "b").y == 10, "b at %g", rect_of(dv, "b").y);
    CHECK(!strcmp(au_docview_title(dv), "T"), "title");
    /* resize: text rewraps */
    CHECK(rect_of(dv, "p").h == 16, "one line at 800");
    au_docview_set_viewport(dv, 100, 600);
    CHECK(rect_of(dv, "p").h == 48, "three lines at 100: %g", rect_of(dv, "p").h);
    int w, h;
    au_docview_content_size(dv, &w, &h);
    CHECK(w == 100 && h == 600, "content size %dx%d", w, h);
    au_docview_set_viewport(dv, 800, 600);
    CHECK(rect_of(dv, "p").h == 16, "back to one line");
    /* paint into a small view after scrolling */
    AuSurface *s = au_surface_new(50, 50);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 50, 50}, 0, 0);
    CHECK((au_surface_get(s, 2, 32) & 0xFFFFFF) == 0, "text pixel %06x", (unsigned)(au_surface_get(s, 2, 32) & 0xFFFFFF));
    au_canvas_free(c);
    au_surface_free(s);
    /* reloading clears the old document */
    const char *h2 = "<!DOCTYPE html><body><div id=z></div>";
    au_docview_load(dv, h2, strlen(h2), "http://example.com/2");
    CHECK(by_id(dv, "a") == NULL && by_id(dv, "z") != NULL, "reload");
    CHECK(!strcmp(au_docview_title(dv), ""), "title kept across reload");
    au_docview_free(dv);
}

/* ---- Images ---- */

static bool fake_decode(void *ctx, const void *data, size_t len, int *w, int *h, AuSurface **px)
{
    (void)ctx;
    /* "WxH" */
    char buf[32];
    if (len >= sizeof buf)
        return false;
    memcpy(buf, data, len);
    buf[len] = 0;
    if (sscanf(buf, "%dx%d", w, h) != 2)
        return false;
    *px = NULL;
    return true;
}

static void test_images(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    AuLoader li = {&f, fake_request, NULL, fake_decode};
    au_docview_free(dv);
    memset(&f, 0, sizeof f);
    dv = au_docview_new(fonts, &li, NULL);
    const char *html = "<!DOCTYPE html><body style='margin:0'><img id=i src='p.png'><img id=j src='q.png' width=20>"
                       "<div id=d style='background:url(bg.png)'>x</div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    CHECK(fake_find(&f, "http://example.com/p.png") != NULL, "img not requested");
    /* Background images are requested when first painted. */
    AuSurface *s = au_surface_new(100, 100);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 100, 100}, 0, 0);
    au_canvas_free(c);
    au_surface_free(s);
    CHECK(fake_find(&f, "http://example.com/bg.png") != NULL, "background image not requested");
    fake_deliver(&f, dv, "http://example.com/p.png", "40x30", true);
    fake_deliver(&f, dv, "http://example.com/q.png", "40x30", true);
    AuRect r = rect_of(dv, "i");
    CHECK(r.w == 40 && r.h == 30, "img size %gx%g", r.w, r.h);
    r = rect_of(dv, "j");
    CHECK(r.w == 20 && r.h == 15, "img width attr keeps ratio: %gx%g", r.w, r.h);
    au_docview_free(dv);
}

/* ---- User style sheet ---- */

static void test_user_css(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    au_docview_set_user_css(dv, "html { font-size: 20px } #b { height: 3px }");
    const char *html = "<!DOCTYPE html><style>#b{height:9px}</style><body style='margin:0'>"
                       "<div id=a style='height:2em'></div><div id=b></div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    CHECK(rect_of(dv, "a").h == 40, "user font size: 2em = %g, want 40", rect_of(dv, "a").h);
    CHECK(rect_of(dv, "b").h == 9, "author rule must beat the user rule: %g", rect_of(dv, "b").h);
    /* Changing it restyles the loaded document; NULL removes it. */
    au_docview_set_user_css(dv, "html { font-size: 10px }");
    CHECK(rect_of(dv, "a").h == 20, "changed user sheet: %g", rect_of(dv, "a").h);
    au_docview_set_user_css(dv, NULL);
    CHECK(rect_of(dv, "a").h == 32, "removed user sheet: %g", rect_of(dv, "a").h);
    au_docview_free(dv);
}

/* ---- @font-face ---- */

static char *read_sample(const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", AURA_PAGE_SAMPLES_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *d = malloc(n > 0 ? (size_t)n : 1);
    *len = fread(d, 1, (size_t)n, fp);
    fclose(fp);
    return d;
}

static void deliver_bytes(Fake *f, AuDocView *dv, const char *url, const char *data, size_t len, bool ok)
{
    FakeReq *r = fake_find(f, url);
    CHECK(r != NULL, "no request for %s", url);
    if (!r)
        return;
    CHECK(r->kind == AU_RES_OTHER, "font fetched as kind %d", (int)r->kind);
    if (data)
        au_docview_resource_data(dv, r->id, data, len);
    au_docview_resource_done(dv, r->id, ok);
}

static void test_webfonts(void)
{
    size_t wlen;
    char *woff2 = read_sample("sample-inter.woff2", &wlen);
    CHECK(woff2 != NULL, "cannot read sample-inter.woff2");
    if (!woff2)
        return;
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html =
        "<!DOCTYPE html><style>"
        "@font-face{font-family:W;src:url(missing.woff2) format('woff2'),url(w.woff2) format('woff2');"
        "font-weight:100 900}"
        "@font-face{font-family:W;src:url(cyr.woff2);unicode-range:U+0400-045F}"
        "@font-face{font-family:Unused;src:url(unused.woff2)}"
        "@font-face{font-family:Bad;src:url(svg.woff2) tech(color-svg),url(bad.woff2),url(cff.otf)}"
        "</style><body style='margin:0;font:20px W'>"
        "<span id=a>iiii</span><br><span id=l style='font-weight:300'>mmmm</span><br>"
        "<span id=h style='font-weight:800'>mmmm</span><br><span id=b style='font-family:Bad'>x</span>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    AuRect before = rect_of(dv, "a");
    CHECK(before.w == 80, "Ahem before the web font arrives: %g", before.w);
    CHECK(fake_find(&f, "http://example.com/missing.woff2") != NULL, "first source not requested");
    CHECK(fake_find(&f, "http://example.com/w.woff2") == NULL, "second source requested before the first failed");
    CHECK(fake_find(&f, "http://example.com/cyr.woff2") == NULL, "face outside the text's unicode-range requested");
    CHECK(fake_find(&f, "http://example.com/unused.woff2") == NULL, "unused family requested");
    CHECK(fake_find(&f, "http://example.com/svg.woff2") == NULL, "unsupported technology requested");
    CHECK(fake_find(&f, "http://example.com/bad.woff2") != NULL, "Bad face not requested");
    /* Fonts are requested when the document is first styled, after parsing
     * may already have reported the load finished (as images are). */
    CHECK(au_docview_loading(dv), "not loading while fonts are pending");
    int finished_before = f.finished;
    deliver_bytes(&f, dv, "http://example.com/missing.woff2", NULL, 0, false);
    CHECK(fake_find(&f, "http://example.com/w.woff2") != NULL, "next source not requested after a failure");
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    AuRect after = rect_of(dv, "a");
    CHECK(after.w > 5 && after.w < 40, "web font laid out: iiii is %g wide", after.w);
    AuRect l = rect_of(dv, "l"), h = rect_of(dv, "h");
    CHECK(h.w > l.w + 4, "variable weights: 800 is %g wide, 300 is %g", h.w, l.w);
    /* Corrupt data and a CFF font fall through to the fallback font. */
    deliver_bytes(&f, dv, "http://example.com/bad.woff2", "wOF2 not really a font", 22, true);
    CHECK(fake_find(&f, "http://example.com/cff.otf") != NULL, "source after corrupt data not requested");
    static const char otto[] = "OTTO\0\0\0\0\0\0\0\0";
    deliver_bytes(&f, dv, "http://example.com/cff.otf", otto, sizeof otto, true);
    CHECK(rect_of(dv, "b").w == 20, "CFF face must fall back: %g", rect_of(dv, "b").w);
    CHECK(!au_docview_loading(dv) && f.finished == finished_before + 1, "load finished %d times after fonts",
          f.finished - finished_before);
    CHECK(rect_of(dv, "a").w == after.w, "web font kept");
    /* A new document drops the faces. */
    const char *plain = "<!DOCTYPE html><body style='margin:0;font:20px W'><span id=a>iiii</span>";
    au_docview_load(dv, plain, strlen(plain), "http://example.com/2");
    CHECK(rect_of(dv, "a").w == 80, "web font outlived its document: %g", rect_of(dv, "a").w);
    au_docview_free(dv);
    /* Freed while a font is still loading. */
    dv = new_view(&f);
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_update(dv);
    au_docview_free(dv);
    free(woff2);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_urls();
    test_sheets();
    test_base_title_fragments();
    test_input();
    test_incremental();
    test_images();
    test_user_css();
    test_webfonts();
    au_fontset_free(fonts);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
