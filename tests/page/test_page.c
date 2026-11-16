/* Document view tests: URL resolution, the asynchronous loader, style
 * sheet order, <base> and <title>, fragments, hover and link activation,
 * incremental feeding, resize. Uses the Ahem-like test font. */
#include "../layout/ahem.h"
#include "page/page.h"
#include "image/cache.h"

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
    /* Long URLs resolve whole: a data: URL of 20,000 bytes, and a relative
     * path of 10,000 with dot segments, a query and a fragment. The
     * resolver cut the relative part at 4095 bytes. */
    size_t big = 20000;
    char *rel = malloc(big + 1);
    memcpy(rel, "data:image/png;base64,", 22);
    memset(rel + 22, 'A', big - 22);
    rel[big] = 0;
    char *got = au_url_resolve_dup("http://a/b/c", rel);
    CHECK(got && !strcmp(got, rel), "a long data: URL resolves whole: %zu bytes", got ? strlen(got) : 0);
    au_free(got);
    char *out = malloc(big + 64);
    CHECK(au_url_resolve("http://a/b/c", rel, out, big + 64) && !strcmp(out, rel), "the same into a buffer");
    CHECK(!au_url_resolve("http://a/b/c", rel, out, big), "a buffer one byte short");
    memcpy(rel, "x/../", 5);
    memset(rel + 5, 'p', 9990);
    memcpy(rel + 9995, "?q#f", 5);
    got = au_url_resolve_dup("http://a/b/c", rel);
    CHECK(got && strlen(got) == strlen("http://a/b/") + 9990 + 4 && !strncmp(got, "http://a/b/ppp", 14) &&
              !strcmp(got + strlen(got) - 4, "?q#f"),
          "a long relative path: %.20s... (%zu bytes)", got ? got : "", got ? strlen(got) : 0);
    au_free(got);
    free(out);
    free(rel);
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

/* A sheet's text is packed once the document has loaded: a rule that
 * first matches afterwards, an @media block whose query first matches
 * afterwards and the CSSOM read it back; a sheet linked twice is parsed
 * once. */
int au_dv_sheet_text(AuDocView *dv, AuElement *el, AuBuf *text, AuBuf *url);
static void mouse(AuDocView *dv, AuEventType t, int x, int y, uint32_t mods);

static void test_packed_sheets(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><html><head>"
                       "<link id=one rel=stylesheet href=big.css>"
                       "<link id=two rel=stylesheet href=big.css>"
                       "<style>#t{background:red}</style>"
                       "</head><body style='margin:0'><div id=t></div></body>";
    AuBuf css;
    au_buf_init(&css);
    au_buf_puts(&css, "#t{height:10px;width:100px}\n");
    for (int i = 0; i < 400; i++)
        au_buf_printf(&css, ".c%d:hover > .d%d{margin:%dpx;color:#%06x}\n", i, i * 7, i, i * 4099);
    au_buf_puts(&css, "#t:hover{height:77px}\n@media (max-width: 500px){#t{width:33px}}\n");
    for (int i = 0; i < 200; i++)
        au_buf_printf(&css, ".e%d{padding:%dpx}\n", i, i);
    au_buf_puts(&css, "#t:active{height:55px}\n");
    au_docview_load(dv, html, strlen(html), "http://example.com/page.html");
    au_docview_set_viewport(dv, 800, 600);
    /* one request per link here; both get the same text */
    for (int i = 0; i < f.n; i++) {
        au_docview_resource_data(dv, f.req[i].id, css.data, css.len);
        au_docview_resource_done(dv, f.req[i].id, true);
    }
    AuRect r = rect_of(dv, "t");
    CHECK(r.h == 10 && r.w == 100, "before packing: %gx%g", r.w, r.h);
    CHECK(!au_docview_loading(dv), "still loading");
    int packed = 0;
    while (au_docview_pack_sheets(dv))
        packed++;
    CHECK(packed == 1, "%d sheets packed, want 1 (the two links share one; the style element's is small)", packed);
    CHECK(!au_docview_pack_sheets(dv), "packing twice");
    /* the CSSOM's view of both links is the text as it arrived */
    for (int k = 0; k < 2; k++) {
        AuBuf text, url;
        au_buf_init(&text);
        au_buf_init(&url);
        int st = au_dv_sheet_text(dv, by_id(dv, k ? "two" : "one"), &text, &url);
        CHECK(st == 2 && text.len == css.len && !memcmp(text.data, css.data, css.len), "link %d: text read back (%d)", k,
              st);
        au_buf_free(&text);
        au_buf_free(&url);
    }
    /* a rule that matches for the first time now */
    mouse(dv, AU_EVENT_MOUSE_MOVE, 5, 5, 0);
    r = rect_of(dv, "t");
    CHECK(r.h == 77, "hover after packing: h=%g", r.h);
    mouse(dv, AU_EVENT_MOUSE_DOWN, 5, 5, 0);
    r = rect_of(dv, "t");
    CHECK(r.h == 55, "active after packing: h=%g", r.h);
    mouse(dv, AU_EVENT_MOUSE_UP, 5, 5, 0);
    /* an @media block that applies for the first time now */
    au_docview_set_viewport(dv, 400, 600);
    r = rect_of(dv, "t");
    CHECK(r.w == 33, "@media after packing: w=%g", r.w);
    au_buf_free(&css);
    au_docview_free(dv);
}

/* A sheet whose media query does not match is neither fetched nor parsed
 * until the viewport changes so that it matches. */
static void test_media_sheets(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><html><head>"
                       "<link rel=stylesheet media=print href=p.css>"
                       "<link rel=stylesheet media='(min-width: 1000px)' href=wide.css>"
                       "<link rel=stylesheet media='screen and (max-width: 900px)' href=narrow.css>"
                       "<style media='(min-width: 1000px)'>#t{width:77px}</style>"
                       "<style>@import url(i.css) (min-width: 1000px); @import url(j.css) screen;"
                       " #t{min-height:1px}</style>"
                       "</head><body style='margin:0'><div id=t></div></body>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    CHECK(f.n == 2, "%d requests at 800px, want 2", f.n);
    CHECK(fake_find(&f, "http://example.com/narrow.css") != NULL, "narrow.css not requested");
    CHECK(fake_find(&f, "http://example.com/j.css") != NULL, "j.css not requested");
    CHECK(fake_find(&f, "http://example.com/p.css") == NULL, "print sheet requested");
    CHECK(fake_find(&f, "http://example.com/wide.css") == NULL, "wide.css requested at 800px");
    CHECK(fake_find(&f, "http://example.com/i.css") == NULL, "i.css requested at 800px");
    fake_deliver(&f, dv, "http://example.com/narrow.css", "#t{height:30px}", true);
    fake_deliver(&f, dv, "http://example.com/j.css", "#t{margin-top:2px}", true);
    CHECK(!au_docview_loading(dv) && f.finished == 1, "deferred sheets hold the load: finished %d", f.finished);
    AuRect r = rect_of(dv, "t");
    CHECK(r.h == 30 && r.w == 800 && r.y == 2, "at 800px: %gx%g at y %g", r.w, r.h, r.y);

    /* wider: the deferred link and import are fetched, the deferred
     * <style> is parsed, and narrow.css stops applying */
    au_docview_set_viewport(dv, 1200, 700);
    CHECK(f.n == 4, "%d requests at 1200px, want 4", f.n);
    CHECK(fake_find(&f, "http://example.com/wide.css") != NULL, "wide.css not requested at 1200px");
    CHECK(fake_find(&f, "http://example.com/i.css") != NULL, "i.css not requested at 1200px");
    CHECK(fake_find(&f, "http://example.com/p.css") == NULL, "print sheet requested");
    r = rect_of(dv, "t");
    CHECK(r.w == 77 && r.h == 1, "deferred <style> at 1200px: %gx%g", r.w, r.h);
    fake_deliver(&f, dv, "http://example.com/wide.css", "#t{height:40px}", true);
    fake_deliver(&f, dv, "http://example.com/i.css", "#t{margin-left:3px}", true);
    r = rect_of(dv, "t");
    CHECK(r.w == 77 && r.h == 40 && r.x == 3, "at 1200px: %gx%g at x %g", r.w, r.h, r.x);

    /* narrow again: nothing is fetched twice; the sheets stay parsed and
     * their media queries are evaluated by the cascade */
    au_docview_set_viewport(dv, 800, 600);
    CHECK(f.n == 4, "%d requests after narrowing, want 4", f.n);
    r = rect_of(dv, "t");
    CHECK(r.w == 800 && r.h == 30 && r.x == 0, "back at 800px: %gx%g at x %g", r.w, r.h, r.x);
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

/* A w x h 24-bit BMP of one colour (the view decodes images itself). */
static size_t make_bmp(uint8_t *out, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3, n = 54 + stride * (size_t)h;
    memset(out, 0, n);
    out[0] = 'B';
    out[1] = 'M';
    out[10] = 54;
    out[14] = 40;
    out[18] = (uint8_t)w;
    out[19] = (uint8_t)(w >> 8);
    out[22] = (uint8_t)h;
    out[23] = (uint8_t)(h >> 8);
    out[26] = 1;
    out[28] = 24;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = out + 54 + (size_t)y * stride + (size_t)x * 3;
            p[0] = b;
            p[1] = g;
            p[2] = r;
        }
    return n;
}

static void deliver_image(Fake *f, AuDocView *dv, const char *url, const uint8_t *data, size_t len, bool ok)
{
    FakeReq *r = fake_find(f, url);
    CHECK(r != NULL, "no request for %s", url);
    if (!r)
        return;
    /* In two parts: sizes are known from the header, before the end. */
    size_t half = len / 2;
    au_docview_resource_data(dv, r->id, data, half);
    au_docview_resource_data(dv, r->id, data + half, len - half);
    au_docview_resource_done(dv, r->id, ok);
}

static void test_images(void)
{
    Fake f;
    memset(&f, 0, sizeof f);
    AuLoader li = {&f, fake_request, NULL, NULL};
    AuDocView *dv = au_docview_new(fonts, &li, NULL);
    const char *html = "<!DOCTYPE html><body style='margin:0'><img id=i src='p.png'><img id=j src='q.png' width=20>"
                       "<div id=d style='background:url(bg.png)'>x</div>"
                       "<p><img id=k src='broken.png' alt='Broken picture'><img id=m src='slow.png' alt='Slow'>";
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
    /* Pending: an empty box, no alt text yet. */
    AuRect r = rect_of(dv, "m");
    CHECK(r.w == 0, "pending image with alt text is %g wide", r.w);
    static uint8_t bmp[54 + 120 * 30];
    size_t n = make_bmp(bmp, 40, 30, 255, 0, 0);
    deliver_image(&f, dv, "http://example.com/p.png", bmp, n, true);
    deliver_image(&f, dv, "http://example.com/q.png", bmp, n, true);
    fake_deliver(&f, dv, "http://example.com/broken.png", "not an image at all", true);
    r = rect_of(dv, "i");
    CHECK(r.w == 40 && r.h == 30, "img size %gx%g", r.w, r.h);
    r = rect_of(dv, "j");
    CHECK(r.w == 20 && r.h == 15, "img width attr keeps ratio: %gx%g", r.w, r.h);
    /* Failed: the alt text, inline. */
    r = rect_of(dv, "k");
    CHECK(r.w > 40, "broken image shows its alt text: %g wide", r.w);
    /* The decoded pixels are painted. */
    s = au_surface_new(100, 100);
    c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 100, 100}, 0, 0);
    CHECK(au_surface_get(s, 10, 10) == AU_RGB(255, 0, 0), "image pixel %08x", au_surface_get(s, 10, 10));
    r = rect_of(dv, "j");
    int jx = (int)(r.x + r.w / 2), jy = (int)(r.y + r.h / 2);
    CHECK(au_surface_get(s, jx, jy) == AU_RGB(255, 0, 0), "scaled image pixel %08x", au_surface_get(s, jx, jy));
    au_canvas_free(c);
    au_surface_free(s);
    AuImageCacheStats st;
    au_docview_image_stats(dv, &st);
    CHECK(st.entries == 5 && st.decoded == 2, "image cache: %u entries, %u decoded", st.entries, st.decoded);
    /* q.png is shown at 20 x 15: decoded at half size. */
    CHECK(st.decoded_bytes == (40 * 30 + 20 * 15) * 4, "decoded bytes %zu", st.decoded_bytes);
    au_docview_free(dv);
}

/* An image that arrives keeps the box tree. The cause of a box tree
 * build on every key in Wikipedia's search field: the suggestion list's
 * thumbnails are background images, and an image whose size became known
 * (any image, shown or not) made layout build the tree again and the
 * view invalidate the page. Now a background image repaints the boxes
 * that show it, an <img> with a fixed size repaints its box, and an
 * <img> that takes its size from the image is laid out again. */
typedef struct InvalLog {
    int n;
    AuIRect u;
} InvalLog;

static void log_invalidate(void *ctx, AuDocView *dv, AuIRect r)
{
    InvalLog *l = ctx;
    (void)dv;
    if (!l->n++) {
        l->u = r;
        return;
    }
    if (r.x0 < l->u.x0) l->u.x0 = r.x0;
    if (r.y0 < l->u.y0) l->u.y0 = r.y0;
    if (r.x1 > l->u.x1) l->u.x1 = r.x1;
    if (r.y1 > l->u.y1) l->u.y1 = r.y1;
}

static void type_char(AuDocView *dv, char ch)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.ch = (unsigned char)ch;
    au_docview_key(dv, &e);
    au_docview_update(dv);
}

static bool inside(AuIRect u, AuRect a, AuRect b)
{
    float x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    float x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return u.x0 >= (int)x0 - 6 && u.y0 >= (int)y0 - 6 && u.x1 <= (int)x1 + 6 && u.y1 <= (int)y1 + 6;
}

static void test_image_arrival_cost(void)
{
    Fake f;
    memset(&f, 0, sizeof f);
    InvalLog log;
    memset(&log, 0, sizeof log);
    AuLoader li = {&f, fake_request, NULL, NULL};
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &log;
    cb.invalidate = log_invalidate;
    AuDocView *dv = au_docview_new(fonts, &li, &cb);
    AuBuf html;
    au_buf_init(&html);
    au_buf_puts(&html, "<!DOCTYPE html><body style='margin:0'><input id=q value=x>"
                       "<div id=t style='width:40px;height:40px;background-image:url(thumb.png)'></div>"
                       "<img id=fixed src=fixed.png width=20 height=20 style='display:block'>"
                       "<img id=free src=free.png style='display:block'>");
    for (int i = 0; i < 50; i++)
        au_buf_puts(&html, "<p>Paragraph <b>bold</b> text</p>");
    au_docview_load(dv, au_buf_cstr(&html), html.len, "http://example.com/");
    au_buf_free(&html);
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    AuSurface *s = au_surface_new(200, 200);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0); /* asks for the background */
    AuRect q = rect_of(dv, "q");
    mouse(dv, AU_EVENT_MOUSE_DOWN, (int)q.x + 5, (int)q.y + 5, 0);
    mouse(dv, AU_EVENT_MOUSE_UP, (int)q.x + 5, (int)q.y + 5, 0);
    type_char(dv, 'a');
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    static uint8_t bmp[54 + 120 * 30];
    size_t n = make_bmp(bmp, 40, 30, 255, 0, 0);
    static const struct {
        const char *url, *id;
        unsigned layouts;
    } cases[] = {{"http://example.com/thumb.png", "t", 0},
                 {"http://example.com/fixed.png", "fixed", 0},
                 {"http://example.com/free.png", "free", 1}};
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        AuDocViewStats s0, s1;
        au_docview_stats(dv, &s0);
        AuRect before = rect_of(dv, cases[k].id);
        log.n = 0;
        deliver_image(&f, dv, cases[k].url, bmp, n, true);
        type_char(dv, 'b');
        au_docview_stats(dv, &s1);
        CHECK(s1.tree_builds == s0.tree_builds, "%s arrived: %u tree builds", cases[k].id,
              s1.tree_builds - s0.tree_builds);
        CHECK(s1.layouts - s0.layouts == cases[k].layouts, "%s arrived: %u layouts", cases[k].id,
              s1.layouts - s0.layouts);
        if (!cases[k].layouts)
            CHECK(log.n >= 1 && inside(log.u, q, before), "%s arrived: invalidated %d,%d-%d,%d", cases[k].id,
                  log.u.x0, log.u.y0, log.u.x1, log.u.y1);
        au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    }
    AuRect r = rect_of(dv, "free");
    CHECK(r.w == 40 && r.h == 30, "image laid out at its size: %gx%g", r.w, r.h);
    r = rect_of(dv, "t");
    CHECK(au_surface_get(s, (int)r.x + 5, (int)r.y + 5) == AU_RGB(255, 0, 0), "background painted: %08x",
          au_surface_get(s, (int)r.x + 5, (int)r.y + 5));
    r = rect_of(dv, "fixed");
    CHECK(au_surface_get(s, (int)r.x + 5, (int)r.y + 5) == AU_RGB(255, 0, 0), "fixed image painted: %08x",
          au_surface_get(s, (int)r.x + 5, (int)r.y + 5));
    au_canvas_free(c);
    au_surface_free(s);
    au_docview_free(dv);
}

/* <object> shows an image or its fallback content; <embed> shows an
 * image; SVG images size from their width and height. */
static void test_object_embed(void)
{
    Fake f;
    memset(&f, 0, sizeof f);
    AuLoader li = {&f, fake_request, NULL, NULL};
    AuDocView *dv = au_docview_new(fonts, &li, NULL);
    const char *html =
        "<!DOCTYPE html><body style='margin:0'>"
        "<p><object id=o1 data='a.png'><span id=f1>fallback one</span></object>"
        "<p><object id=o2 data='movie.swf' type='application/x-shockwave-flash'><span id=f2>no plugin</span></object>"
        "<p><object id=o3 data='missing.png'><span id=f3>missing</span></object>"
        "<p><embed id=e1 src='e.svg'>"
        "<p><object data='data:application/x-unknown,ERROR'><object data='404/' type='text/html'>"
        "<object id=o4 data='a.png'></object></object></object>"
        "<p><img id=s src='s.svg'>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    CHECK(fake_find(&f, "http://example.com/a.png") != NULL, "object image not requested");
    CHECK(fake_find(&f, "http://example.com/movie.swf") == NULL, "plugin data requested");
    /* An object whose resource may be a document asks for it; while it
     * waits it shows neither the document nor its fallback. */
    CHECK(fake_find(&f, "http://example.com/404/") != NULL, "nested document not requested");
    fake_deliver(&f, dv, "http://example.com/404/", "", false);
    au_docview_update(dv);
    AuRect r = rect_of(dv, "f2");
    CHECK(r.w > 20, "object with a plugin type shows its fallback: %g wide", r.w);
    static uint8_t bmp[54 + 120 * 30];
    size_t n = make_bmp(bmp, 40, 30, 0, 0, 255);
    deliver_image(&f, dv, "http://example.com/a.png", bmp, n, true);
    deliver_image(&f, dv, "http://example.com/missing.png", (const uint8_t *)"<html>404</html>", 16, true);
    const char *svg = "<svg xmlns='http://www.w3.org/2000/svg' width='50' height='20'><rect width='50' height='20' fill='lime'/></svg>";
    deliver_image(&f, dv, "http://example.com/e.svg", (const uint8_t *)svg, strlen(svg), true);
    deliver_image(&f, dv, "http://example.com/s.svg", (const uint8_t *)svg, strlen(svg), true);
    au_docview_update(dv);
    r = rect_of(dv, "o1");
    CHECK(r.w == 40 && r.h == 30, "object image %gx%g", r.w, r.h);
    CHECK(by_id(dv, "f1") && !au_layout_element_rect(au_docview_layout(dv), by_id(dv, "f1"), &r),
          "an object showing its image does not lay out its fallback");
    r = rect_of(dv, "f3");
    CHECK(r.w > 20, "object whose image failed shows its fallback: %g wide", r.w);
    r = rect_of(dv, "o4");
    CHECK(r.w == 40 && r.h == 30, "innermost object of a fallback chain shows its image: %gx%g", r.w, r.h);
    r = rect_of(dv, "e1");
    CHECK(r.w == 50 && r.h == 20, "embed of an SVG image %gx%g", r.w, r.h);
    r = rect_of(dv, "s");
    CHECK(r.w == 50 && r.h == 20, "img of an SVG image %gx%g", r.w, r.h);
    AuSurface *s = au_surface_new(800, 600);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 800, 600}, 0, 0);
    int sx = (int)(r.x + r.w / 2), sy = (int)(r.y + r.h / 2);
    CHECK(au_surface_get(s, sx, sy) == AU_RGB(0, 255, 0), "SVG image pixel %08x", au_surface_get(s, sx, sy));
    r = rect_of(dv, "o1");
    CHECK(au_surface_get(s, (int)r.x + 5, (int)r.y + 5) == AU_RGB(0, 0, 255), "object image pixel");
    au_canvas_free(c);
    au_surface_free(s);
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
    CHECK(r->kind == AU_RES_FONT, "font fetched as kind %d", (int)r->kind);
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

static int count_requests(const Fake *f, const char *url)
{
    int n = 0;
    for (int i = 0; i < f->n; i++)
        n += !strcmp(f->req[i].url, url);
    return n;
}

/* A web font arriving makes a new layout: images stay decoded, are not
 * fetched again, and animations keep running. */
static void test_images_and_webfonts(void)
{
    size_t wlen, glen;
    char *woff2 = read_sample("sample-inter.woff2", &wlen);
    char *gif = read_sample("img/spinner.gif", &glen);
    CHECK(woff2 && gif, "cannot read samples");
    if (!woff2 || !gif) {
        free(woff2);
        free(gif);
        return;
    }
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><style>@font-face{font-family:W;src:url(w.woff2)}</style>"
                       "<body style='margin:0;font:20px W'><img id=i src='p.png'><img id=g src='s.gif'>"
                       "<span id=a>iiii</span>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    static uint8_t bmp[54 + 120 * 30];
    size_t n = make_bmp(bmp, 40, 30, 255, 0, 0);
    deliver_image(&f, dv, "http://example.com/p.png", bmp, n, true);
    deliver_image(&f, dv, "http://example.com/s.gif", (const uint8_t *)gif, glen, true);
    AuSurface *s = au_surface_new(200, 100);
    AuCanvas *c = au_canvas_new(s);
    au_docview_animate(dv, 1000);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 100}, 0, 0);
    AuRect ri = rect_of(dv, "i");
    int ix = (int)(ri.x + 20), iy = (int)(ri.y + 15);
    CHECK(au_surface_get(s, ix, iy) == AU_RGB(255, 0, 0), "image before the font: %08x", au_surface_get(s, ix, iy));
    CHECK(au_docview_animating(dv), "animation running before the font");
    AuImageCacheStats st0;
    au_docview_image_stats(dv, &st0);
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    CHECK(rect_of(dv, "a").w < 40, "web font laid out: %g", rect_of(dv, "a").w);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 100}, 0, 0);
    ri = rect_of(dv, "i");
    ix = (int)(ri.x + 20), iy = (int)(ri.y + 15);
    CHECK(au_surface_get(s, ix, iy) == AU_RGB(255, 0, 0), "image after the font: %08x", au_surface_get(s, ix, iy));
    CHECK(rect_of(dv, "i").w == 40 && rect_of(dv, "g").w == 48, "image sizes kept");
    CHECK(count_requests(&f, "http://example.com/p.png") == 1 && count_requests(&f, "http://example.com/s.gif") == 1,
          "images fetched again after the font");
    AuImageCacheStats st1;
    au_docview_image_stats(dv, &st1);
    CHECK(st1.entries == st0.entries && st1.decodes == st0.decodes, "images lost: %u entries, %u decodes (were %u, %u)",
          st1.entries, st1.decodes, st0.entries, st0.decodes);
    uint32_t next = au_docview_animate(dv, 1100);
    CHECK(au_docview_animating(dv) && next != UINT32_MAX, "animation stopped after the font");
    au_canvas_free(c);
    au_surface_free(s);
    au_docview_free(dv);
    free(woff2);
    free(gif);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_urls();
    test_sheets();
    test_media_sheets();
    test_packed_sheets();
    test_base_title_fragments();
    test_input();
    test_incremental();
    test_images();
    test_image_arrival_cost();
    test_object_embed();
    test_user_css();
    test_webfonts();
    test_images_and_webfonts();
    au_fontset_free(fonts);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
