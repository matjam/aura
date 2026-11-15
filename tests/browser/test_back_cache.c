/* The document cache for Back and Forward (src/browser/page.h, "The
 * document cache"), over the fake transport: Back shows the kept
 * document with no request and no new parse, with its scroll position
 * and form contents; timers stand still while a document is hidden and
 * run after; pagehide and pageshow carry persisted; the limits evict;
 * and the excluded cases (unload listener, POST result, no-store, a load
 * under way, a fetch under way, a reload) load again. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
#include "net/cache.h"
#include "net/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

static AuFontSet *fonts;

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }

static void env_init(Env *e)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    e->b = au_browser_new(fonts, &h);
    e->b->prefs.https_first = false;
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    au_browser_set_fetcher(e->b, "https", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    fake_free(&e->f);
}

static void step(Env *e)
{
    e->f.now += 10;
    au_bw_idle(e->w);
}

static void settle(Env *e, int rounds)
{
    for (int i = 0; i < rounds && au_browser_busy(e->b); i++)
        step(e);
}

static void run_for(Env *e, int ms)
{
    for (int i = 0; i < ms / 10; i++)
        step(e);
}

static void paint(AuBrowserWindow *w)
{
    AuSurface *s = au_surface_new(w->width, w->height);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, w->width, w->height});
    au_canvas_free(c);
    au_surface_free(s);
}

static AuTab *tab(Env *e) { return au_bw_active_tab(e->w); }
static AuPage *page(Env *e) { return tab(e)->page; }

static uint32_t restored(Env *e)
{
    AuPageStats st;
    au_page_stats(page(e), &st);
    return st.restored;
}

static bool page_text_has(Env *e, const char *needle)
{
    au_bw_select_all(e->w);
    AuBuf b;
    au_buf_init(&b);
    au_bw_selection_text(e->w, &b);
    bool has = strstr(au_buf_cstr(&b), needle) != NULL;
    au_buf_free(&b);
    au_page_clear_selection(page(e));
    return has;
}

/* A 200 response with the given extra header lines ("" for none). */
static FakeResponse *respond(FakeServer *s, const char *type, const char *extra, const char *body)
{
    size_t cap = strlen(body) + strlen(extra) + 256;
    char *b = malloc(cap); /* lives as long as the test; the fake keeps the pointer */
    snprintf(b, cap, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%sContent-Length: %u\r\n\r\n%s", type, extra,
             (unsigned)strlen(body), body);
    return fake_add(s, b);
}

static FakeResponse *respond_html(FakeServer *s, const char *body)
{
    return respond(s, "text/html", "", body);
}

static void go(Env *e, const char *url)
{
    au_bw_navigate(e->w, url);
    settle(e, 500);
    paint(e->w);
}

static void bw_key(Env *e, int key, uint32_t ch)
{
    AuEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = AU_EVENT_KEY_DOWN;
    ev.key = key;
    ev.ch = ch;
    au_bw_key(e->w, &ev);
}

static bool console_has(Env *e, const char *needle)
{
    for (size_t i = 0; i < au_page_console_count(page(e)); i++)
        if (strstr(au_page_console_line(page(e), i, NULL), needle))
            return true;
    return false;
}

/* ---- Back shows the kept document ---- */

static void test_back_keeps(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    respond_html(s, "<!DOCTYPE html><title>A</title><form><input name=q></form>"
                    "<div style='height:3000px'>page a</div><p>end of a");
    respond_html(s, "<!DOCTYPE html><title>B</title><p>page b");
    au_bw_new_tab(e.w, "http://site.test/a", true, -1);
    settle(&e, 500);
    paint(e.w);
    bw_key(&e, AU_KEY_TAB, 0);
    for (const char *c = "kept text"; *c; c++)
        bw_key(&e, 0, (unsigned char)*c);
    au_bw_scroll_to(e.w, 0, 700);
    paint(e.w);
    AuPageStats before;
    au_page_stats(page(&e), &before);

    go(&e, "http://site.test/b");
    CHECK(page_text_has(&e, "page b") && s->nreq == 2, "b loaded (%d requests)", s->nreq);
    CHECK(au_page_kept_count(page(&e)) == 1 && au_page_kept_bytes(page(&e)) > 0, "a is kept: %u documents, %u bytes",
          (unsigned)au_page_kept_count(page(&e)), (unsigned)au_page_kept_bytes(page(&e)));
    CHECK(tab(&e)->scroll_y == 0, "b starts at the top");

    au_bw_back(e.w);
    CHECK(!au_page_loading(page(&e)) && !tab(&e)->loading, "Back shows the document at once");
    settle(&e, 500);
    paint(e.w);
    AuPageStats after;
    au_page_stats(page(&e), &after);
    CHECK(s->nreq == 2, "Back: no request (%d)", s->nreq);
    CHECK(after.restored == 1 && after.layouts == before.layouts && after.bytes == before.bytes,
          "Back: no new parse or layout (restored %u, layouts %u then %u)", (unsigned)after.restored,
          (unsigned)before.layouts, (unsigned)after.layouts);
    CHECK(page_text_has(&e, "end of a"), "a is shown");
    CHECK(!strcmp(au_page_title(page(&e)), "A") && !strcmp(au_page_url(page(&e)), "http://site.test/a"),
          "title and address: %s %s", au_page_title(page(&e)), au_page_url(page(&e)));
    CHECK(!strcmp(au_bw_field_text(e.w), "http://site.test/a"), "address field %s", au_bw_field_text(e.w));
    CHECK(tab(&e)->scroll_y == 700, "scroll position kept: %d", tab(&e)->scroll_y);
    AuBuf v;
    au_buf_init(&v);
    CHECK(au_bw_page_editing(e.w), "the field still has the focus");
    au_bw_page_edit(e.w, AU_PAGE_EDIT_SELECT_ALL, NULL, 0, NULL);
    au_bw_page_edit(e.w, AU_PAGE_EDIT_COPY, NULL, 0, &v);
    CHECK(!strcmp(au_buf_cstr(&v), "kept text"), "form field contents kept: '%s'", au_buf_cstr(&v));
    au_buf_free(&v);
    CHECK(au_page_kept_count(page(&e)) == 1, "b is kept for Forward");
    const char *src = "";
    size_t len = 0;
    bool have = au_bw_page_source(e.w, &src, &len);
    CHECK(have && len > 30 && !strncmp(src + 3, "<!DOCTYPE html><title>A", 23) /* after the BOM */,
          "the source came back with the document: %d, %u bytes, %.30s", have, (unsigned)len, src);

    au_bw_forward(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 2 && page_text_has(&e, "page b") && restored(&e) == 1, "Forward: no request (%d)", s->nreq);

    /* A reload replaces the document instead of keeping it. */
    respond_html(s, "<!DOCTYPE html><title>B2</title><p>page b again");
    au_bw_reload(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 3 && page_text_has(&e, "page b again") && restored(&e) == 0, "reload fetches (%d)", s->nreq);
    CHECK(au_page_kept_count(page(&e)) == 1, "only a is kept after the reload: %u",
          (unsigned)au_page_kept_count(page(&e)));

    /* A new navigation from a drops the forward entry and its document. */
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 3 && au_page_kept_count(page(&e)) == 1, "back to a, b kept");
    respond_html(s, "<!DOCTYPE html><title>C</title><p>page c");
    go(&e, "http://site.test/c");
    CHECK(au_page_kept_count(page(&e)) == 1 && tab(&e)->nav.n == 2, "b's document went with its entry: %u kept",
          (unsigned)au_page_kept_count(page(&e)));

    /* Low memory: everything kept goes, and Back loads again. */
    CHECK(au_browser_release_documents(e.b) == 1 && au_browser_kept_documents(e.b, NULL) == 0, "release drops all");
    respond_html(s, "<!DOCTYPE html><title>A</title><p>a loaded again");
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 5 && page_text_has(&e, "a loaded again"), "Back after a release loads (%d)", s->nreq);
    env_free(&e);
}

/* ---- Timers and page transitions ---- */

static void test_timers(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    respond_html(s, "<!DOCTYPE html><title>T</title><p>ticking<script>"
                    "var n = 0;"
                    "setInterval(function () { n++; console.log('tick ' + n); }, 100);"
                    "addEventListener('pagehide', function (ev) { console.log('hide ' + ev.persisted); });"
                    "addEventListener('pageshow', function (ev) { console.log('show ' + ev.persisted); });"
                    "</script>");
    respond_html(s, "<!DOCTYPE html><title>B</title><p>page b");
    au_bw_new_tab(e.w, "http://site.test/t", true, -1);
    settle(&e, 500);
    run_for(&e, 500);
    CHECK(console_has(&e, "tick 2") && console_has(&e, "show false"), "the timer runs while the page is shown");

    go(&e, "http://site.test/b");
    CHECK(au_page_kept_count(page(&e)) == 1, "the scripted page is kept");
    CHECK(console_has(&e, "hide true"), "pagehide with persisted");
    size_t lines = au_page_console_count(page(&e));
    run_for(&e, 2000);
    CHECK(au_page_console_count(page(&e)) == lines, "no timer fires while hidden (%u lines, then %u)",
          (unsigned)lines, (unsigned)au_page_console_count(page(&e)));
    CHECK(!au_browser_busy(e.b), "a hidden document keeps nothing busy");

    au_bw_back(e.w);
    CHECK(console_has(&e, "show true"), "pageshow with persisted");
    lines = au_page_console_count(page(&e));
    run_for(&e, 500);
    CHECK(au_page_console_count(page(&e)) > lines, "the timer fires again after Back");
    CHECK(s->nreq == 2, "no request for Back (%d)", s->nreq);
    env_free(&e);
}

/* ---- Limits ---- */

static void test_limits(void)
{
    Env e;
    env_init(&e);
    e.b->prefs.document_cache_pages = 2;
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    const char *names[4] = {"a", "b", "c", "d"};
    for (int i = 0; i < 4; i++) {
        char body[128], url[64];
        snprintf(body, sizeof body, "<!DOCTYPE html><title>%s</title><p>page %s", names[i], names[i]);
        respond_html(s, body);
        snprintf(url, sizeof url, "http://site.test/%s", names[i]);
        if (i == 0) {
            au_bw_new_tab(e.w, url, true, -1);
            settle(&e, 500);
        } else {
            go(&e, url);
        }
    }
    CHECK(s->nreq == 4 && au_page_kept_count(page(&e)) == 2, "two of three left documents are kept: %u",
          (unsigned)au_page_kept_count(page(&e)));
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 4 && page_text_has(&e, "page c"), "c was kept");
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 4 && page_text_has(&e, "page b"), "b was kept");
    /* a, the oldest, went when c was kept; d went when b was shown. */
    respond_html(s, "<!DOCTYPE html><title>a</title><p>page a again");
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 5 && page_text_has(&e, "page a again"), "the oldest was evicted: %d requests", s->nreq);

    /* The byte budget, over all tabs: the oldest go until the rest fits. */
    size_t bytes;
    size_t n = au_browser_kept_documents(e.b, &bytes);
    CHECK(n == 2 && bytes > 0, "%u documents kept, %u bytes", (unsigned)n, (unsigned)bytes);
    e.b->prefs.document_cache_kb = 1;
    step(&e);
    CHECK(au_browser_kept_documents(e.b, NULL) == 0, "a budget below one document keeps none: %u",
          (unsigned)au_browser_kept_documents(e.b, NULL));
    /* With the cache off nothing is kept. */
    e.b->prefs.document_cache_kb = 24576;
    e.b->prefs.document_cache_pages = 0;
    respond_html(s, "<!DOCTYPE html><title>e</title><p>page e");
    go(&e, "http://site.test/e");
    CHECK(au_page_kept_count(page(&e)) == 0, "cache off: nothing kept");
    env_free(&e);
}

/* ---- Documents that are not kept ---- */

/* Loads first, then second; returns whether first's document was kept. */
static bool kept_after(Env *e, FakeServer *s, const char *first_extra, const char *first_body)
{
    static int serial;
    char url[64];
    respond(s, "text/html", first_extra, first_body);
    snprintf(url, sizeof url, "http://site.test/x%d", serial++);
    go(e, url);
    run_for(e, 100);
    respond_html(s, "<!DOCTYPE html><title>next</title><p>next");
    snprintf(url, sizeof url, "http://site.test/x%d", serial++);
    size_t before = au_page_kept_count(page(e));
    go(e, url);
    bool kept = au_page_kept_count(page(e)) > before;
    au_browser_release_documents(e->b);
    return kept;
}

static void test_excluded(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    respond_html(s, "<!DOCTYPE html><p>start");
    au_bw_new_tab(e.w, "http://site.test/", true, -1);
    settle(&e, 500);

    CHECK(kept_after(&e, s, "", "<!DOCTYPE html><p>plain"), "a plain document is kept");
    CHECK(kept_after(&e, s, "", "<!DOCTYPE html><p>script<script>var x = 1;</script>"), "a scripted one is kept");
    CHECK(!kept_after(&e, s, "", "<!DOCTYPE html><p>u<script>addEventListener('unload', function () {});</script>"),
          "an unload listener: not kept");
    CHECK(!kept_after(&e, s, "", "<!DOCTYPE html><body onbeforeunload='return 1'><p>bu<script>var y = 2;</script>"),
          "a beforeunload handler: not kept");
    CHECK(!kept_after(&e, s, "Cache-Control: no-store\r\n", "<!DOCTYPE html><p>secret"), "no-store: not kept");
    CHECK(kept_after(&e, s, "Cache-Control: no-cache\r\n", "<!DOCTYPE html><p>fresh"), "no-cache is kept");

    /* Back to a no-store document fetches it again. */
    respond(s, "text/html", "Cache-Control: no-store\r\n", "<!DOCTYPE html><p>secret one");
    go(&e, "http://site.test/secret");
    respond_html(s, "<!DOCTYPE html><p>after secret");
    go(&e, "http://site.test/after");
    int n = s->nreq;
    respond(s, "text/html", "Cache-Control: no-store\r\n", "<!DOCTYPE html><p>secret two");
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == n + 1 && page_text_has(&e, "secret two") && restored(&e) == 0, "Back to no-store loads again");

    /* A document still loading when the tab leaves it. */
    AuBuf big;
    au_buf_init(&big);
    au_buf_puts(&big, "<!DOCTYPE html><title>Big</title><body>");
    for (int i = 0; i < 300; i++)
        au_buf_printf(&big, "<p>paragraph %d</p>", i);
    FakeResponse *r = respond_html(s, au_buf_cstr(&big));
    r->chunk = 128;
    r->paced = true;
    au_bw_navigate(e.w, "http://site.test/big");
    for (int i = 0; i < 30; i++) {
        step(&e);
        paint(e.w);
    }
    CHECK(au_page_loading(page(&e)) && page_text_has(&e, "paragraph 1"), "big is part way in");
    size_t kept = au_page_kept_count(page(&e));
    respond_html(s, "<!DOCTYPE html><p>left big");
    go(&e, "http://site.test/leftbig");
    CHECK(au_page_kept_count(page(&e)) == kept, "a document still loading is not kept");
    au_buf_free(&big);
    au_browser_release_documents(e.b);

    /* A document with a fetch under way (an open connection). */
    respond_html(s, "<!DOCTYPE html><p>fetching<script>fetch('/never').then(function (r) { console.log('fetched ' + r.status); }, function (x) { console.log('failed ' + x); });</script>");
    fake_add(s, "")->silent = true;
    go(&e, "http://site.test/fetching");
    run_for(&e, 200);
    CHECK(!au_page_loading(page(&e)) && au_page_busy(page(&e)), "the page has loaded; its fetch has not answered");
    kept = au_page_kept_count(page(&e));
    FakeServer *o = fake_server(&e.f, "other.test", 80);
    respond_html(o, "<!DOCTYPE html><p>other site");
    go(&e, "http://other.test/");
    CHECK(page_text_has(&e, "other site") && au_page_kept_count(page(&e)) == kept,
          "a document with a fetch outstanding is not kept");

    /* A POST result. */
    respond_html(o, "<!DOCTYPE html><form method=post action=/p><input name=a value=1></form>");
    respond_html(o, "<!DOCTYPE html><title>Posted</title>thanks");
    respond_html(o, "<!DOCTYPE html><p>after post");
    go(&e, "http://other.test/form");
    bw_key(&e, AU_KEY_TAB, 0);
    bw_key(&e, AU_KEY_RETURN, 0);
    settle(&e, 500);
    CHECK(au_page_is_post(page(&e)) && page_text_has(&e, "thanks"), "the POST result is shown");
    kept = au_page_kept_count(page(&e));
    go(&e, "http://other.test/afterpost");
    CHECK(au_page_kept_count(page(&e)) == kept, "a POST result is not kept (%u, then %u)", (unsigned)kept,
          (unsigned)au_page_kept_count(page(&e)));
    env_free(&e);
}

/* ---- Stop, fragments and several tabs ---- */

static void test_edges(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    respond_html(s, "<!DOCTYPE html><title>A</title><p>page a<div style='height:2000px'></div><p id=low>low");
    au_bw_new_tab(e.w, "http://site.test/a", true, -1);
    settle(&e, 500);
    paint(e.w);

    /* A load that is stopped before it begins leaves the document on
     * screen; Back to its entry shows it without a request. */
    fake_add(s, "")->silent = true;
    au_bw_navigate(e.w, "http://site.test/slow");
    run_for(&e, 100);
    au_bw_stop(e.w);
    int n = s->nreq;
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == n && page_text_has(&e, "page a") && !strcmp(au_page_url(page(&e)), "http://site.test/a"),
          "Back after Stop: the document that never left (%d requests)", s->nreq - n);
    CHECK(restored(&e) == 1 && au_page_kept_count(page(&e)) == 0, "shown in place");

    /* Fragment entries share the document; leaving and returning keeps it. */
    FakeServer *o = fake_server(&e.f, "other.test", 80);
    go(&e, "http://site.test/a#low");
    CHECK(tab(&e)->scroll_y > 500 && s->nreq == n, "fragment scrolls, no request");
    respond_html(o, "<!DOCTYPE html><p>other");
    go(&e, "http://other.test/");
    CHECK(au_page_kept_count(page(&e)) == 1, "a kept under its fragment entry");
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == n && !strcmp(au_page_url(page(&e)), "http://site.test/a#low") && tab(&e)->scroll_y > 500,
          "Back to the fragment entry: %s at %d", au_page_url(page(&e)), tab(&e)->scroll_y);
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == n && !strcmp(au_page_url(page(&e)), "http://site.test/a") && tab(&e)->scroll_y == 0,
          "Back within the document");
    au_bw_go(e.w, 2);
    settle(&e, 500);
    CHECK(page_text_has(&e, "other") && o->nreq == 1, "Forward two entries: other kept (%d)", o->nreq);

    /* A second tab keeps its own documents; closing a tab frees them. */
    respond_html(o, "<!DOCTYPE html><p>tab two first");
    respond_html(o, "<!DOCTYPE html><p>tab two second");
    au_bw_new_tab(e.w, "http://other.test/one", true, -1);
    settle(&e, 500);
    go(&e, "http://other.test/two");
    CHECK(au_browser_kept_documents(e.b, NULL) == 2, "both tabs keep one: %u",
          (unsigned)au_browser_kept_documents(e.b, NULL));
    /* The oldest over all tabs goes first: the first tab's. */
    CHECK(au_browser_evict_document(e.b) && au_page_kept_count(e.w->tabs[0]->page) == 0 &&
              au_page_kept_count(page(&e)) == 1,
          "evicting one document takes the oldest");
    /* A resize while hidden: the document is laid out for the new width. */
    au_bw_resize(e.w, 500, 400);
    au_bw_back(e.w);
    settle(&e, 500);
    paint(e.w);
    CHECK(page_text_has(&e, "tab two first") && o->nreq == 3, "Back in the second tab");
    CHECK(au_browser_kept_documents(e.b, NULL) == 1, "the second tab keeps the document it left");
    au_bw_close_tab(e.w, e.w->active);
    CHECK(au_browser_kept_documents(e.b, NULL) == 0, "closing the tab frees its kept documents");
    /* Reopen Closed Tab loads through the history cache mode as before. */
    respond_html(o, "<!DOCTYPE html><p>tab two reopened");
    CHECK(au_bw_reopen_closed_tab(e.w) != NULL, "reopen");
    settle(&e, 500);
    CHECK(o->nreq == 4 && page_text_has(&e, "tab two reopened"), "the reopened tab loads (%d)", o->nreq);
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_back_keeps();
    test_timers();
    test_limits();
    test_excluded();
    test_edges();
    au_fontset_free(fonts);
    printf("back_cache: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
