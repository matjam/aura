/* Document view tests: URL resolution, the asynchronous loader, style
 * sheet order, <base> and <title>, fragments, hover and link activation,
 * incremental feeding, resize. Uses the Ahem-like test font. */
#include "../layout/ahem.h"
#include "page/page.h"
#include "page/script.h" /* au_dv_dom_changed */
#include "page/webfont.h"
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
    size_t opened_len; /* the whole URL's length (opened keeps its start) */
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
    f->opened_len = strlen(url);
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

    /* A sheet that arrives after the first layout restyles the document
     * without building the whole box tree: a change of size lays out
     * again, and a change of display rebuilds only that subtree. */
    dv = new_view(&f);
    html = "<!DOCTYPE html><link rel=stylesheet href=late.css><link rel=stylesheet href=later.css>"
           "<body style='margin:0'><div id=t style='width:7px'></div>"
           "<div id=u><p>one<p id=v>two</div><p>three";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    AuDocViewStats s0, s1;
    au_docview_stats(dv, &s0);
    fake_deliver(&f, dv, "http://example.com/late.css", "#t{height:9px}", true);
    CHECK(rect_of(dv, "t").h == 9, "late sheet applied: h=%g", rect_of(dv, "t").h);
    au_docview_stats(dv, &s1);
    CHECK(s1.tree_builds == s0.tree_builds, "late sheet: %u tree builds", s1.tree_builds - s0.tree_builds);
    s0 = s1;
    fake_deliver(&f, dv, "http://example.com/later.css", "#v{display:none}", true);
    CHECK(rect_of(dv, "v").h <= 0 && rect_of(dv, "u").h == 16, "display: none from a late sheet: %g %g",
          rect_of(dv, "v").h, rect_of(dv, "u").h);
    au_docview_stats(dv, &s1);
    CHECK(s1.tree_builds == s0.tree_builds && s1.subtree_builds > s0.subtree_builds,
          "late display change: %u tree builds, %u subtree rebuilds", s1.tree_builds - s0.tree_builds,
          s1.subtree_builds - s0.subtree_builds);
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

/* Documents that load the same sheet URL with the same bytes share one
 * parsed sheet; a CSSOM change or disabling it in one document does not
 * reach the other, and the sheet is freed once, with its last owner (the
 * asan build checks the frees). */
void au_dv_sheet_set_text(AuDocView *dv, AuElement *el, const char *text, size_t len);
void au_dv_sheet_disable(AuDocView *dv, AuElement *el, bool disabled);

static void load_shared(Fake *f, AuDocView *dv, const char *css, const char *media)
{
    char html[512];
    snprintf(html, sizeof html,
             "<!DOCTYPE html><html><head><link id=l rel=stylesheet href=shared.css%s%s%s>"
             "<style>#u{width:20px;height:5px}</style>"
             "</head><body style='margin:0'><div id=t></div><div id=u></div></body>",
             media ? " media='" : "", media ? media : "", media ? "'" : "");
    au_docview_load(dv, html, strlen(html), "http://example.com/page.html");
    au_docview_set_viewport(dv, 800, 600);
    fake_deliver(f, dv, "http://example.com/shared.css", css, true);
}

static void test_shared_sheets(void)
{
    const char *css = "#t{height:10px;width:100px}#t:hover{height:77px}@media (max-width:500px){#t{width:33px}}";
    size_t base = au_css_shared_sheet_count();
    Fake f1, f2, f3;
    AuDocView *a = new_view(&f1);
    load_shared(&f1, a, css, NULL);
    AuDocView *b = new_view(&f2);
    load_shared(&f2, b, css, NULL);
    /* one sheet for the link, one for the <style> (same text and URL) */
    CHECK(au_css_shared_sheet_count() == base + 2, "%zu shared sheets, want %zu", au_css_shared_sheet_count(),
          base + 2);
    AuRect ra = rect_of(a, "t"), rb = rect_of(b, "t");
    CHECK(ra.w == 100 && rb.w == 100 && ra.h == 10 && rb.h == 10, "both styled: %gx%g, %gx%g", ra.w, ra.h, rb.w,
          rb.h);
    /* the same URL with other bytes, or with other media, is a sheet of its own */
    AuDocView *c = new_view(&f3);
    load_shared(&f3, c, "#t{height:12px;width:100px}", "screen");
    CHECK(au_css_shared_sheet_count() == base + 3, "other bytes: %zu shared sheets", au_css_shared_sheet_count());
    CHECK(rect_of(c, "t").h == 12, "the third document has its own sheet");
    au_docview_free(c);
    CHECK(au_css_shared_sheet_count() == base + 2, "its sheet freed with it");
    /* script changes the rules in a: b keeps the shared sheet */
    const char *changed = "#t{height:40px;width:50px}";
    au_dv_sheet_set_text(a, by_id(a, "l"), changed, strlen(changed));
    ra = rect_of(a, "t");
    rb = rect_of(b, "t");
    CHECK(ra.w == 50 && ra.h == 40, "a sees its change: %gx%g", ra.w, ra.h);
    CHECK(rb.w == 100 && rb.h == 10, "b does not: %gx%g", rb.w, rb.h);
    AuBuf text, url;
    au_buf_init(&text);
    au_buf_init(&url);
    CHECK(au_dv_sheet_text(b, by_id(b, "l"), &text, &url) == 2 && text.len == strlen(css) &&
              !memcmp(text.data, css, text.len),
          "b's CSSOM reads the text as it arrived");
    au_buf_free(&text);
    au_buf_free(&url);
    /* b disables the sheet: a's change stays and nothing else moves */
    au_dv_sheet_disable(b, by_id(b, "l"), true);
    rb = rect_of(b, "t");
    ra = rect_of(a, "t");
    CHECK(rb.h == 0 && ra.h == 40, "disabled in b only: b %g, a %g", rb.h, ra.h);
    au_dv_sheet_disable(b, by_id(b, "l"), false);
    CHECK(rect_of(b, "t").h == 10, "enabled again in b");
    /* the rules parsed late (hover, @media) work in the document that is
     * left after the other goes */
    au_docview_free(a);
    CHECK(au_css_shared_sheet_count() == base + 2, "b still holds both sheets");
    mouse(b, AU_EVENT_MOUSE_MOVE, 5, 5, 0);
    CHECK(rect_of(b, "t").h == 77, "hover in b after a is freed");
    au_docview_set_viewport(b, 400, 600);
    CHECK(rect_of(b, "t").w == 33, "@media in b after a is freed");
    CHECK(rect_of(b, "u").w == 20, "the shared <style> sheet in b");
    au_docview_free(b);
    CHECK(au_css_shared_sheet_count() == base, "%zu shared sheets after both are freed, want %zu",
          au_css_shared_sheet_count(), base);
}

/* The atoms a page added go when its view is freed (core.h, "Atoms"):
 * class and id names, attribute names, custom properties, names in its
 * sheets and those script made. A sheet shared with another document
 * keeps its names, those parsed late included, while that document
 * lives. */
static uint32_t dynamic_atoms(void)
{
    AuAtomStats st;
    au_atom_stats(&st);
    return st.dynamic;
}

static void load_named(Fake *f, AuDocView *dv, int round)
{
    AuBuf html;
    au_buf_init(&html);
    au_buf_printf(&html,
                  "<!DOCTYPE html><html><head><link rel=stylesheet href=named.css>"
                  "<style>.st-%d{--style-prop-%d:1px}</style></head><body style='margin:0'>",
                  round, round);
    for (int i = 0; i < 50; i++)
        au_buf_printf(&html, "<div id=id-%d-%d class='cls-%d-%d other-%d' data-attr-%d-%d=x>t</div>", round, i,
                      round, i, i, round, i);
    au_buf_puts(&html, "<p id=t>x</p></body></html>");
    au_docview_load(dv, html.data, html.len, "http://example.com/named.html");
    au_docview_set_viewport(dv, 800, 600);
    char css[256];
    snprintf(css, sizeof css, ".sheet-only-%d{color:red}#t{height:10px}#t:hover{--lazy-name-%d:3px;height:20px}",
             round, round);
    fake_deliver(f, dv, "http://example.com/named.css", css, true);
    au_docview_update(dv);
    au_buf_free(&html);
}

static void hover_t(AuDocView *dv)
{
    AuRect r = rect_of(dv, "t");
    mouse(dv, AU_EVENT_MOUSE_MOVE, (int)r.x + 2, (int)r.y + 2, 0);
}

static void test_atoms_freed(void)
{
    uint32_t base = dynamic_atoms();
    for (int round = 0; round < 3; round++) {
        Fake f;
        AuDocView *dv = new_view(&f);
        load_named(&f, dv, round);
        hover_t(dv); /* parses the hover block late */
        CHECK(rect_of(dv, "t").h == 20, "round %d: the hover rule", round);
        uint32_t held = dynamic_atoms();
        CHECK(held > base + 150, "round %d: %u atoms while the page is up, %u before", round, held, base);
        CHECK(au_atom_peek("cls-0-7", 7) || round, "a class atom exists");
        au_docview_free(dv);
        CHECK(dynamic_atoms() == base, "round %d: %u atoms after the page is freed, want %u", round,
              dynamic_atoms(), base);
    }
    /* a shared sheet's atoms stay with the document that shares it */
    Fake f1, f2;
    AuDocView *a = new_view(&f1);
    load_named(&f1, a, 7);
    AuDocView *b = new_view(&f2);
    load_named(&f2, b, 7);
    hover_t(a); /* a parses the hover block: b holds what it interned */
    au_docview_free(a);
    CHECK(au_atom_peek("sheet-only-7", 12) != 0, "the sheet's class name stays for b");
    CHECK(au_atom_peek("--lazy-name-7", 13) != 0, "the name a parsed late stays for b");
    hover_t(b);
    CHECK(rect_of(b, "t").h == 20, "b's hover rule");
    au_docview_free(b);
    CHECK(dynamic_atoms() == base, "%u atoms after both, want %u", dynamic_atoms(), base);
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

/* A <base> and a link URL longer than any fixed buffer: hover, status and
 * opening keep the whole URL. */
static void test_long_link(void)
{
    enum { SEG = 5000 };
    AuBuf base, html, want;
    au_buf_init(&base);
    au_buf_init(&html);
    au_buf_init(&want);
    au_buf_puts(&base, "http://example.com/");
    for (int i = 0; i < SEG; i++)
        au_buf_putc(&base, 'a' + i % 26);
    au_buf_puts(&base, "/");
    au_buf_puts(&want, au_buf_cstr(&base));
    au_buf_puts(&want, "next.html?q=");
    au_buf_puts(&html, "<!DOCTYPE html><base href='");
    au_buf_puts(&html, au_buf_cstr(&base));
    au_buf_puts(&html, "'><body style='margin:0'><a id=l style='display:block;height:20px' href='next.html?q=");
    for (int i = 0; i < SEG; i++) {
        au_buf_putc(&html, 'z');
        au_buf_putc(&want, 'z');
    }
    au_buf_puts(&html, "'>x</a>");
    Fake f;
    AuDocView *dv = new_view(&f);
    au_docview_load(dv, html.data, html.len, "http://example.com/page.html");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    CHECK(!strcmp(au_docview_base_url(dv), au_buf_cstr(&base)), "long base cut to %zu bytes",
          strlen(au_docview_base_url(dv)));
    mouse(dv, AU_EVENT_MOUSE_MOVE, 5, 5, 0);
    CHECK(!strcmp(au_docview_hover_link(dv), au_buf_cstr(&want)), "long hover link cut to %zu bytes of %zu",
          strlen(au_docview_hover_link(dv)), want.len);
    mouse(dv, AU_EVENT_MOUSE_DOWN, 5, 5, 0);
    mouse(dv, AU_EVENT_MOUSE_UP, 5, 5, 0);
    CHECK(f.opened_len == want.len, "opened %zu bytes, want %zu", f.opened_len, want.len);
    mouse(dv, AU_EVENT_MOUSE_MOVE, 700, 500, 0);
    CHECK(!strcmp(au_docview_hover_link(dv), ""), "hover link not cleared");
    au_docview_free(dv);
    au_buf_free(&base);
    au_buf_free(&html);
    au_buf_free(&want);
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
    AuLoader li = {&f, fake_request, NULL, NULL, NULL, NULL};
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
    AuLoader li = {&f, fake_request, NULL, NULL, NULL, NULL};
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
    AuLoader li = {&f, fake_request, NULL, NULL, NULL, NULL};
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

/* An HTML <img> in an inline SVG foreignObject draws in the object's box
 * through the object's mask, as Discord draws avatars: a 40 x 40 svg, a
 * 32 x 32 foreignObject masked by a circle with a hole cut where the
 * status dot goes, and the dot drawn after it. */
static void test_foreign_object_img(void)
{
    Fake f;
    memset(&f, 0, sizeof f);
    AuLoader li = {&f, fake_request, NULL, NULL, NULL, NULL};
    AuDocView *dv = au_docview_new(fonts, &li, NULL);
    const char *html =
        "<!DOCTYPE html><body style='margin:0'>"
        "<svg width=0 height=0 style='position:absolute'><mask id=m maskContentUnits=objectBoundingBox>"
        "<circle fill=white cx=0.5 cy=0.5 r=0.5 /><circle fill=black cx=0.85 cy=0.85 r=0.2 /></mask></svg>"
        "<svg id=s width=40 height=40 viewBox='0 0 40 40' style='display:block'>"
        "<foreignObject x=0 y=0 width=32 height=32 mask='url(#m)'>"
        "<div><img src='a.png' style='width:32px;height:32px'></div></foreignObject>"
        "<rect x=22 y=22 width=10 height=10 fill=lime /></svg>"
        "<svg id=t width=20 height=20 style='display:block'><foreignObject width=20 height=20>"
        "<div>text only</div></foreignObject></svg>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 200, 200);
    au_docview_update(dv);
    AuSurface *s = au_surface_new(200, 200);
    AuCanvas *c = au_canvas_new(s);
    /* The image is asked for when the svg first paints. */
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    CHECK(fake_find(&f, "http://example.com/a.png") != NULL, "foreignObject img not requested");
    static uint8_t bmp[54 + 120 * 30];
    size_t n = make_bmp(bmp, 40, 30, 0, 0, 255);
    deliver_image(&f, dv, "http://example.com/a.png", bmp, n, true);
    au_docview_update(dv);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    AuRect r = rect_of(dv, "s");
    int x0 = (int)r.x, y0 = (int)r.y;
    AuColor mid = au_surface_get(s, x0 + 16, y0 + 16); /* a luminance mask rounds a little */
    CHECK(((mid >> 16) & 255) < 8 && ((mid >> 8) & 255) < 8 && (mid & 255) > 245, "image in the circle: %08x",
          (unsigned)mid);
    CHECK(au_surface_get(s, x0 + 1, y0 + 1) == AU_RGB(255, 255, 255), "masked corner: %08x",
          au_surface_get(s, x0 + 1, y0 + 1));
    CHECK(au_surface_get(s, x0 + 27, y0 + 27) == AU_RGB(0, 255, 0), "status dot over the hole: %08x",
          au_surface_get(s, x0 + 27, y0 + 27));
    CHECK(au_surface_get(s, x0 + 36, y0 + 16) == AU_RGB(255, 255, 255), "nothing outside the object: %08x",
          au_surface_get(s, x0 + 36, y0 + 16));
    r = rect_of(dv, "t");
    CHECK(r.w == 20 && r.h == 20, "svg with a text-only foreignObject keeps its size: %gx%g", r.w, r.h);
    au_canvas_free(c);
    au_surface_free(s);
    au_docview_free(dv);
}

/* Whether pixel c is within 6 of r, g, b on each channel. */
static bool near_rgb(AuColor c, int r, int g, int b)
{
    int cr = (int)((c >> 16) & 255), cg = (int)((c >> 8) & 255), cb = (int)(c & 255);
    return abs(cr - r) <= 6 && abs(cg - g) <= 6 && abs(cb - b) <= 6;
}

/* HTML in a foreignObject, as Discord's server rail draws its icons: a
 * div with a background and an svg icon or text centred in it, through
 * the object's mask. A restyle of the div paints its new colour, and a
 * removed object paints nothing. */
static void test_foreign_object_html(void)
{
    Fake f;
    memset(&f, 0, sizeof f);
    AuLoader li = {&f, fake_request, NULL, NULL, NULL, NULL};
    AuDocView *dv = au_docview_new(fonts, &li, NULL);
    const char *html =
        "<!DOCTYPE html><body style='margin:0'>"
        "<svg width=0 height=0 style='position:absolute'><mask id=sq maskContentUnits=objectBoundingBox>"
        "<rect fill=white x=0 y=0 width=1 height=1 rx=0.3 ry=0.3 /></mask></svg>"
        "<svg id=s width=48 height=48 viewBox='0 0 48 48' style='display:block'>"
        "<foreignObject id=fo x=0 y=0 width=48 height=48 mask='url(#sq)'>"
        "<div style='display:flex;align-items:center;justify-content:center;width:48px;height:48px;"
        "background:#5865f2;color:#fff'><svg width=24 height=24 viewBox='0 0 12 12'>"
        "<rect width=12 height=12 fill=lime /></svg></div></foreignObject></svg>"
        "<svg id=t width=48 height=48 style='display:block'><foreignObject x=8 y=8 width=32 height=32>"
        "<div id=v style='width:32px;height:32px;background:red'></div></foreignObject></svg>"
        "<svg id=u width=48 height=48 style='display:block'><foreignObject width=48 height=48>"
        "<div style='display:flex;align-items:center;justify-content:center;width:48px;height:48px;"
        "background:black;color:white;font:16px sans-serif'>FG</div></foreignObject></svg>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_check(dv, true); /* each rebuild below equals a whole build */
    au_docview_set_viewport(dv, 200, 200);
    au_docview_update(dv);
    AuSurface *s = au_surface_new(200, 200);
    AuCanvas *c = au_canvas_new(s);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    AuRect r = rect_of(dv, "s");
    int x0 = (int)r.x, y0 = (int)r.y;
    AuColor px = au_surface_get(s, x0 + 4, y0 + 24);
    CHECK(near_rgb(px, 0x58, 0x65, 0xf2), "the div's background in the object: %08x", (unsigned)px);
    px = au_surface_get(s, x0 + 24, y0 + 24);
    CHECK(near_rgb(px, 0, 255, 0), "the svg icon centred in the div: %08x", (unsigned)px);
    px = au_surface_get(s, x0 + 8, y0 + 24);
    CHECK(near_rgb(px, 0x58, 0x65, 0xf2), "the background beside the icon: %08x", (unsigned)px);
    px = au_surface_get(s, x0 + 1, y0 + 1);
    CHECK(px == AU_RGB(255, 255, 255), "masked-out corner: %08x", (unsigned)px);
    r = rect_of(dv, "t");
    x0 = (int)r.x, y0 = (int)r.y;
    CHECK(au_surface_get(s, x0 + 24, y0 + 24) == AU_RGB(255, 0, 0), "the object at x=8 y=8: %08x",
          au_surface_get(s, x0 + 24, y0 + 24));
    CHECK(au_surface_get(s, x0 + 4, y0 + 4) == AU_RGB(255, 255, 255), "nothing outside the object: %08x",
          au_surface_get(s, x0 + 4, y0 + 4));
    r = rect_of(dv, "u");
    int light = 0;
    for (int y = (int)r.y + 8; y < (int)r.y + 40; y++)
        for (int x = (int)r.x + 8; x < (int)r.x + 40; x++)
            if (((au_surface_get(s, x, y) >> 8) & 255) > 128)
                light++;
    /* Ahem: two 16 x 16 squares, centred in the sampled 32 x 32. */
    CHECK(light > 400 && light <= 32 * 16, "the acronym's text in the object: %d light pixels", light);
    CHECK(au_surface_get(s, (int)r.x + 2, (int)r.y + 2) == AU_RGB(0, 0, 0), "the acronym's background: %08x",
          au_surface_get(s, (int)r.x + 2, (int)r.y + 2));

    /* A restyle inside the object. */
    AuElement *v = by_id(dv, "v");
    au_element_set_attr(v, AU_ATOM_style, "width:32px;height:32px;background:blue", 38);
    au_dv_dom_changed(dv, &v->node, AU_ATOM_style, 2);
    au_docview_update(dv);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    r = rect_of(dv, "t");
    CHECK(au_surface_get(s, (int)r.x + 24, (int)r.y + 24) == AU_RGB(0, 0, 255), "the restyled div: %08x",
          au_surface_get(s, (int)r.x + 24, (int)r.y + 24));
    /* A change of what is built (display) inside the object. */
    au_element_set_attr(v, AU_ATOM_style, "display:inline-block;width:32px;height:32px;background:lime", 59);
    au_dv_dom_changed(dv, &v->node, AU_ATOM_style, 2);
    au_docview_update(dv);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    CHECK(au_surface_get(s, (int)r.x + 24, (int)r.y + 24) == AU_RGB(0, 255, 0), "the div made inline-block: %08x",
          au_surface_get(s, (int)r.x + 24, (int)r.y + 24));

    /* The object removed. */
    AuElement *fo = by_id(dv, "fo");
    AuNode *svg = fo->node.parent;
    au_node_ref(&fo->node);
    au_node_remove(&fo->node);
    au_dv_dom_changed(dv, svg, 0, 1);
    au_docview_update(dv);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0);
    r = rect_of(dv, "s");
    CHECK(au_surface_get(s, (int)r.x + 4, (int)r.y + 24) == AU_RGB(255, 255, 255), "the removed object: %08x",
          au_surface_get(s, (int)r.x + 4, (int)r.y + 24));
    AuBuf msg;
    au_buf_init(&msg);
    uint32_t fails = au_docview_check_failures(dv, &msg);
    CHECK(fails == 0, "rebuilds differ from a whole build: %.*s", (int)msg.len, msg.data);
    au_buf_free(&msg);
    au_node_unref(&fo->node);
    au_canvas_free(c);
    au_surface_free(s);
    au_docview_free(dv);
}

/* Counts red and dark pixels in r (canvas coordinates). */
static void count_red_dark(const AuSurface *s, AuRect r, int *red, int *dark)
{
    *red = *dark = 0;
    for (int y = (int)r.y; y < (int)(r.y + r.h); y++)
        for (int x = (int)r.x; x < (int)(r.x + r.w); x++) {
            AuColor c = au_surface_get(s, x, y);
            int cr = (int)((c >> 16) & 255), cg = (int)((c >> 8) & 255), cb = (int)(c & 255);
            if (cr > 200 && cg < 60 && cb < 60)
                (*red)++;
            else if (cr < 100 && cg < 100 && cb < 100)
                (*dark)++;
        }
}

/* background-clip: text with -webkit-text-fill-color: transparent, as
 * Discord draws gradient names: the background shows through the glyphs
 * only, on a block and on an inline box, and the text's own colour does
 * not show. Without the clip, a transparent fill keeps the colour. */
static void test_background_clip_text(void)
{
    Fake f;
    memset(&f, 0, sizeof f);
    AuLoader li = {&f, fake_request, NULL, NULL, NULL, NULL};
    AuDocView *dv = au_docview_new(fonts, &li, NULL);
    const char *html =
        "<!DOCTYPE html><body style='margin:0;font:40px sans-serif;color:black'>"
        "<div id=b style='padding:10px;width:200px;background:linear-gradient(red,red);"
        "-webkit-background-clip:text;background-clip:text;-webkit-text-fill-color:transparent'>MWM</div>"
        "<div><span id=i style='background-image:linear-gradient(90deg,red,red);background-clip:text;"
        "-webkit-text-fill-color:transparent'>MWM</span></div>"
        "<div id=n style='-webkit-text-fill-color:transparent'>MWM</div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 400, 300);
    au_docview_update(dv);
    AuSurface *s = au_surface_new(400, 300);
    AuCanvas *c = au_canvas_new(s);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 400, 300}, 0, 0);
    int red, dark;
    AuRect r = rect_of(dv, "b");
    count_red_dark(s, r, &red, &dark);
    CHECK(red > 100 && dark == 0, "block: %d red, %d dark pixels", red, dark);
    CHECK(au_surface_get(s, (int)r.x + 3, (int)r.y + 3) == AU_RGB(255, 255, 255), "block padding stays clear: %08x",
          (unsigned)au_surface_get(s, (int)r.x + 3, (int)r.y + 3));
    /* Between the glyphs' tops and the box's top inside the content box,
     * and right of the text: clear. */
    CHECK(au_surface_get(s, (int)(r.x + r.w - 15), (int)(r.y + r.h / 2)) == AU_RGB(255, 255, 255),
          "block right of the text stays clear");
    r = rect_of(dv, "i");
    count_red_dark(s, r, &red, &dark);
    /* (The test font's glyphs are solid boxes that fill the inline box,
     * so only the colour is checked here; the block shows the clip.) */
    CHECK(red > 100 && dark == 0, "inline: %d red, %d dark", red, dark);
    r = rect_of(dv, "n");
    count_red_dark(s, r, &red, &dark);
    CHECK(red == 0 && dark > 100, "transparent fill without the clip keeps the colour: %d dark", dark);
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

/* A face used only by generated content (an icon font on ::before) is
 * fetched, from a sheet in another directory; one whose unicode-range
 * misses the content, or whose pseudo-element has no content, is not. */
static void test_webfonts_generated(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><link rel=stylesheet href=css/s.css><body>"
                       "<i class=icon></i><i class=cnt></i><i class=none></i><i class=far></i>";
    au_docview_load(dv, html, strlen(html), "http://example.com/a/page.html");
    au_docview_set_viewport(dv, 800, 600);
    fake_deliver(&f, dv, "http://example.com/a/css/s.css",
                 "@font-face{font-family:Icons;src:url(../fonts/i.woff2)}"
                 "@font-face{font-family:Cnt;src:url(c.woff2);unicode-range:U+30-39}"
                 "@font-face{font-family:None;src:url(n.woff2)}"
                 "@font-face{font-family:Far;src:url(f.woff2);unicode-range:U+0400-045F}"
                 ".icon::before{font-family:Icons;content:\"\\f197\"}"
                 ".cnt::after{font-family:Cnt;content:counter(x)}"
                 ".none::before{font-family:None}"
                 ".far::before{font-family:Far;content:'abc'}",
                 true);
    au_docview_update(dv);
    CHECK(count_requests(&f, "http://example.com/a/fonts/i.woff2") == 1, "icon font on ::before not requested");
    CHECK(count_requests(&f, "http://example.com/a/css/c.woff2") == 1, "font for a counter not requested");
    CHECK(count_requests(&f, "http://example.com/a/css/n.woff2") == 0, "font of a pseudo-element without content");
    CHECK(count_requests(&f, "http://example.com/a/css/f.woff2") == 0, "face outside the content's unicode-range");
    au_docview_free(dv);
}

/* A background colour fills whole pixels: a 1px rule at a fractional
 * position is one row in its own colour, as in browsers. */
static void test_background_snapped(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><body style='margin:0'><div style='height:10.6px'></div>"
                       "<div style='height:1px;width:20px;background:#ccb581'></div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 100, 100);
    au_docview_update(dv);
    AuSurface *s = au_surface_new(100, 40);
    AuCanvas *c = au_canvas_new(s);
    au_fill_rect(c, (AuRect){0, 0, 100, 40}, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 100, 40}, 0, 0);
    CHECK(au_surface_get(s, 5, 11) == AU_RGB(0xcc, 0xb5, 0x81), "rule row: %08x", au_surface_get(s, 5, 11));
    CHECK(au_surface_get(s, 5, 10) == AU_RGB(255, 255, 255) && au_surface_get(s, 5, 12) == AU_RGB(255, 255, 255),
          "rule spread over rows: %08x %08x", au_surface_get(s, 5, 10), au_surface_get(s, 5, 12));
    au_canvas_free(c);
    au_surface_free(s);
    au_docview_free(dv);
}

/* An SVG background that gives only a viewBox is fitted to its box like
 * contain (CSS Images 3), not drawn at the 300 x 150 an <img> shows it
 * at; one without a ratio fills the box. */
static void test_background_svg_ratio_only(void)
{
    static const char ratio_svg[] = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 20 10'>"
                                    "<rect width='20' height='10' fill='red'/></svg>";
    static const char bare_svg[] = "<svg xmlns='http://www.w3.org/2000/svg'>"
                                   "<rect width='100%' height='100%' fill='blue'/></svg>";
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><body style='margin:0'>"
                       "<div style='width:100px;height:20px;background:url(r.svg) no-repeat'></div>"
                       "<div style='width:100px;height:180px;background:url(b.svg) no-repeat'></div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 200, 100);
    au_docview_update(dv);
    AuSurface *s = au_surface_new(200, 220);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 220}, 0, 0); /* asks for the backgrounds */
    deliver_image(&f, dv, "http://example.com/r.svg", (const uint8_t *)ratio_svg, sizeof ratio_svg - 1, true);
    deliver_image(&f, dv, "http://example.com/b.svg", (const uint8_t *)bare_svg, sizeof bare_svg - 1, true);
    au_docview_update(dv);
    au_fill_rect(c, (AuRect){0, 0, 200, 220}, AU_RGB(255, 255, 255));
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 220}, 0, 0);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 220}, 0, 0); /* drawn at the size the first paint asked for */
    CHECK(au_surface_get(s, 30, 10) == AU_RGB(255, 0, 0), "ratio-only SVG inside: %08x", au_surface_get(s, 30, 10));
    CHECK(au_surface_get(s, 60, 10) == AU_RGB(255, 255, 255), "ratio-only SVG past 40px: %08x",
          au_surface_get(s, 60, 10));
    CHECK(au_surface_get(s, 95, 190) == AU_RGB(0, 0, 255), "SVG without a ratio fills the box: %08x",
          au_surface_get(s, 95, 190));
    CHECK(au_surface_get(s, 105, 190) == AU_RGB(255, 255, 255), "SVG without a ratio past the box: %08x",
          au_surface_get(s, 105, 190));
    au_canvas_free(c);
    au_surface_free(s);
    au_docview_free(dv);
}

/* A url() in a style sheet resolves against the sheet's URL, and one in
 * a style attribute against the document's (CSS Values 4). */
static void test_sheet_relative_urls(void)
{
    Fake f;
    AuDocView *dv = new_view(&f);
    const char *html = "<!DOCTYPE html><link rel=stylesheet href=css/s.css>"
                       "<body style='margin:0'><div id=b></div><ul><li id=l>x</li></ul><p id=c></p>"
                       "<div id=i style='width:4px;height:4px;background-image:url(img/inline.png)'></div>";
    au_docview_load(dv, html, strlen(html), "http://example.com/a/page.html");
    au_docview_set_viewport(dv, 800, 600);
    fake_deliver(&f, dv, "http://example.com/a/css/s.css",
                 "#b{width:4px;height:4px;background-image:url(../img/bg.png)}"
                 "#l{list-style-image:url('../img/li.png')}"
                 "#c::before{content:url(\"sub/c.png\")}",
                 true);
    au_docview_update(dv);
    AuSurface *s = au_surface_new(200, 200);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 200}, 0, 0); /* asks for the backgrounds */
    au_canvas_free(c);
    au_surface_free(s);
    static const char *const want[] = {"http://example.com/a/img/bg.png", "http://example.com/a/img/li.png",
                                       "http://example.com/a/css/sub/c.png", "http://example.com/a/img/inline.png"};
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++)
        CHECK(count_requests(&f, want[k]) == 1, "%s requested %d times", want[k], count_requests(&f, want[k]));
    CHECK(count_requests(&f, "http://example.com/img/bg.png") == 0, "background resolved against the document");
    au_docview_free(dv);
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

/* ---- font-display: block periods ----
 * The view gets a clock the test moves. Ahem draws solid squares, so the
 * text's ink is counted in a painted surface: hidden text has none. */

static double display_clock;

static double clock_now(void *ctx, AuDocView *dv)
{
    (void)ctx;
    (void)dv;
    return display_clock;
}

static AuDocView *new_clocked_view(Fake *f)
{
    memset(f, 0, sizeof *f);
    static AuLoader li;
    li.ctx = f;
    li.request = fake_request;
    static AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = f;
    cb.load_finished = cb_finished;
    cb.now_ms = clock_now;
    return au_docview_new(fonts, &li, &cb);
}

/* Dark pixels inside element id's box, painted at 400 x 100. */
static int ink_of(AuDocView *dv, const char *id)
{
    AuSurface *s = au_surface_new(400, 100);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 400, 100}, 0, 0);
    AuRect r = rect_of(dv, id);
    int ink = 0;
    for (int y = (int)r.y; y < (int)(r.y + r.h) && y < 100; y++)
        for (int x = (int)r.x; x < (int)(r.x + r.w) && x < 400; x++) {
            uint32_t px = au_surface_get(s, x, y);
            ink += ((px >> 16) & 255) < 128 && ((px >> 8) & 255) < 128 && (px & 255) < 128;
        }
    au_canvas_free(c);
    au_surface_free(s);
    return ink;
}

static uint32_t layouts_of(AuDocView *dv)
{
    AuDocViewStats st;
    au_docview_stats(dv, &st);
    return st.layouts;
}

/* Loads a page whose span #a (iiii, 20px Ahem: 80 x 20) uses face W with
 * the given font-display (NULL: none) and family list, at time 0. */
static AuDocView *display_page(Fake *f, const char *display, const char *family)
{
    char html[512];
    snprintf(html, sizeof html,
             "<!DOCTYPE html><style>@font-face{font-family:W;src:url(w.woff2)%s%s}</style>"
             "<body style='margin:0;font:20px %s;color:#000;background:#fff'><span id=a>iiii</span>",
             display ? ";font-display:" : "", display ? display : "", family);
    display_clock = 0;
    AuDocView *dv = new_clocked_view(f);
    au_docview_load(dv, html, (size_t)strlen(html), "http://example.com/");
    au_docview_set_viewport(dv, 400, 100);
    au_docview_update(dv);
    return dv;
}

/* Moves the clock and lets the view notice. */
static void advance(AuDocView *dv, double to)
{
    display_clock = to;
    au_docview_animate(dv, (uint32_t)to);
}

static void test_font_display(void)
{
    size_t wlen;
    char *woff2 = read_sample("sample-inter.woff2", &wlen);
    CHECK(woff2 != NULL, "cannot read sample-inter.woff2");
    if (!woff2)
        return;
    const int full = 80 * 20; /* iiii in Ahem */
    Fake f;

    /* auto: hidden for the block period, then one layout when the face
     * arrives inside it, and nothing more when the period ends. */
    AuDocView *dv = display_page(&f, NULL, "W");
    CHECK(fake_find(&f, "http://example.com/w.woff2") != NULL, "face not requested");
    CHECK(rect_of(dv, "a").w == 80, "laid out with the fallback's metrics: %g", rect_of(dv, "a").w);
    CHECK(ink_of(dv, "a") == 0, "fallback text shown at once: %d", ink_of(dv, "a"));
    advance(dv, 2000);
    CHECK(ink_of(dv, "a") == 0, "fallback text shown inside the block period: %d", ink_of(dv, "a"));
    uint32_t l0 = layouts_of(dv);
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    AuRect r = rect_of(dv, "a");
    int ink = ink_of(dv, "a");
    CHECK(r.w > 5 && r.w < 40 && ink > 0 && ink < full, "web face shown: %g wide, ink %d", r.w, ink);
    CHECK(layouts_of(dv) == l0 + 1, "layouts when the face arrived: %u", layouts_of(dv) - l0);
    advance(dv, 5000);
    ink_of(dv, "a");
    CHECK(layouts_of(dv) == l0 + 1, "the block period's end laid out again: %u", layouts_of(dv) - l0);
    au_docview_free(dv);

    /* block, slow: the fallback shows when the period ends, without a
     * layout, and the face swaps in when it arrives. */
    dv = display_page(&f, "block", "W");
    CHECK(ink_of(dv, "a") == 0, "block: fallback shown at once");
    l0 = layouts_of(dv);
    advance(dv, AU_WEBFONT_BLOCK_MS - 1);
    CHECK(ink_of(dv, "a") == 0, "block: fallback shown before the period ended");
    advance(dv, AU_WEBFONT_BLOCK_MS);
    CHECK(ink_of(dv, "a") == full, "block: fallback not shown after the period: %d", ink_of(dv, "a"));
    CHECK(layouts_of(dv) == l0, "block: the period's end laid out again");
    advance(dv, 60000);
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    CHECK(rect_of(dv, "a").w < 40, "block: late face not swapped in: %g", rect_of(dv, "a").w);
    au_docview_free(dv);

    /* A failed face ends its block period at once. */
    dv = display_page(&f, NULL, "W");
    CHECK(ink_of(dv, "a") == 0, "failure: fallback shown at once");
    advance(dv, 500);
    deliver_bytes(&f, dv, "http://example.com/w.woff2", NULL, 0, false);
    CHECK(ink_of(dv, "a") == full, "failure: fallback hidden after the face failed: %d", ink_of(dv, "a"));
    au_docview_free(dv);

    /* A platform family before the web face shows at once. */
    dv = display_page(&f, NULL, "Ahem, W");
    CHECK(ink_of(dv, "a") == full, "a platform family first is hidden: %d", ink_of(dv, "a"));
    au_docview_free(dv);
    dv = display_page(&f, NULL, "Nonesuch, W, serif");
    CHECK(ink_of(dv, "a") == 0, "an unknown family before the web face shows the fallback");
    au_docview_free(dv);

    /* swap: a short block period, then the face swaps in whenever it arrives. */
    dv = display_page(&f, "swap", "W");
    CHECK(ink_of(dv, "a") == 0, "swap: fallback shown inside the short block period");
    advance(dv, AU_WEBFONT_SHORT_BLOCK_MS);
    CHECK(ink_of(dv, "a") == full, "swap: fallback not shown after the block period");
    advance(dv, 60000);
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    CHECK(rect_of(dv, "a").w < 40, "swap: face not swapped in: %g", rect_of(dv, "a").w);
    au_docview_free(dv);

    /* fallback: swaps inside its swap period, keeps the fallback after it. */
    dv = display_page(&f, "fallback", "W");
    advance(dv, AU_WEBFONT_SHORT_BLOCK_MS);
    CHECK(ink_of(dv, "a") == full, "fallback: fallback not shown after the block period");
    advance(dv, 2000);
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    CHECK(rect_of(dv, "a").w < 40, "fallback: face inside the swap period not used: %g", rect_of(dv, "a").w);
    au_docview_free(dv);
    dv = display_page(&f, "fallback", "W");
    advance(dv, AU_WEBFONT_SHORT_BLOCK_MS + AU_WEBFONT_SWAP_MS);
    l0 = layouts_of(dv);
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    CHECK(rect_of(dv, "a").w == 80 && ink_of(dv, "a") == full, "fallback: face after the swap period used: %g",
          rect_of(dv, "a").w);
    CHECK(layouts_of(dv) == l0, "fallback: a face not used laid out again");
    CHECK(!au_docview_loading(dv), "fallback: still loading");
    au_docview_free(dv);

    /* optional: used only inside its block period. */
    dv = display_page(&f, "optional", "W");
    advance(dv, AU_WEBFONT_SHORT_BLOCK_MS / 2);
    CHECK(ink_of(dv, "a") == 0, "optional: fallback shown inside the block period");
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    CHECK(rect_of(dv, "a").w < 40, "optional: face inside the block period not used: %g", rect_of(dv, "a").w);
    au_docview_free(dv);
    dv = display_page(&f, "optional", "W");
    advance(dv, AU_WEBFONT_SHORT_BLOCK_MS);
    CHECK(ink_of(dv, "a") == full, "optional: fallback not shown after the block period");
    deliver_bytes(&f, dv, "http://example.com/w.woff2", woff2, wlen, true);
    CHECK(rect_of(dv, "a").w == 80, "optional: late face used: %g", rect_of(dv, "a").w);
    au_docview_free(dv);

    /* A family whose name CSS knows as a keyword (fast) is still a family. */
    {
        display_clock = 0;
        dv = new_clocked_view(&f);
        const char *kw = "<!DOCTYPE html><style>\n@font-face{font-family:Fast;src:url(f.woff2)}</style>"
                         "<body style='margin:0;font:20px Fast'><span id=a>iiii</span>";
        au_docview_load(dv, kw, strlen(kw), "http://example.com/");
        au_docview_set_viewport(dv, 400, 100);
        au_docview_update(dv);
        CHECK(fake_find(&f, "http://example.com/f.woff2") != NULL, "face of family Fast not requested");
        CHECK(ink_of(dv, "a") == 0, "family Fast: fallback shown in the block period");
        deliver_bytes(&f, dv, "http://example.com/f.woff2", woff2, wlen, true);
        CHECK(rect_of(dv, "a").w < 40, "family Fast not used: %g", rect_of(dv, "a").w);
        au_docview_free(dv);
    }

    /* An invalid value is auto. */
    dv = display_page(&f, "sometimes", "W");
    advance(dv, AU_WEBFONT_SHORT_BLOCK_MS);
    CHECK(ink_of(dv, "a") == 0, "invalid font-display: not auto");
    au_docview_free(dv);

    /* A page without web fonts paints at once. */
    display_clock = 0;
    dv = new_clocked_view(&f);
    const char *plain = "<!DOCTYPE html><body style='margin:0;font:20px W'><span id=a>iiii</span>";
    au_docview_load(dv, plain, strlen(plain), "http://example.com/");
    au_docview_set_viewport(dv, 400, 100);
    CHECK(ink_of(dv, "a") == full, "no web fonts: text hidden");
    au_docview_free(dv);
    free(woff2);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_urls();
    test_sheets();
    test_media_sheets();
    test_packed_sheets();
    test_shared_sheets();
    test_atoms_freed();
    test_base_title_fragments();
    test_input();
    test_long_link();
    test_incremental();
    test_images();
    test_image_arrival_cost();
    test_object_embed();
    test_foreign_object_img();
    test_foreign_object_html();
    test_background_clip_text();
    test_user_css();
    test_webfonts();
    test_images_and_webfonts();
    test_sheet_relative_urls();
    test_webfonts_generated();
    test_background_snapped();
    test_background_svg_ratio_only();
    test_font_display();
    au_fontset_free(fonts);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
