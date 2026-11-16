/* Unloaded tabs (src/browser/browser.h, "Unloaded tabs"), over the fake
 * transport: a background tab gives up its document and loads it again
 * from the HTTP cache when selected, with its address, title, history
 * list and scroll position; tabs that would lose something are skipped;
 * the active tab never goes; the memory rules unload the least recently
 * shown tab, after the kept documents; and a restored session loads only
 * its active tab. */
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
    AuHttpCache *cache;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
    uint32_t free_kb; /* what the host reports as free memory; 0: no report */
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }
static uint32_t t_free(void *ctx) { return ((Env *)ctx)->free_kb; }

static void env_init(Env *e)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->cache = au_cache_new(4 << 20);
    au_net_set_cache(e->net, e->cache);
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    e->free_kb = 1 << 20;
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    h.memory_free_kb = t_free;
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
    au_cache_free(e->cache);
    fake_free(&e->f);
}

static void step(Env *e)
{
    e->f.now += 10;
    for (int i = 0; i < e->b->nwindows; i++)
        au_bw_idle(e->b->windows[i]);
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

static char *responses[256];
static int nresponses;

static FakeResponse *respond(FakeServer *s, const char *extra, const char *body)
{
    size_t cap = strlen(body) + strlen(extra) + 256;
    char *b = malloc(cap); /* the fake keeps the pointer: freed at the end of main */
    if (nresponses < 256)
        responses[nresponses++] = b;
    snprintf(b, cap, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%sContent-Length: %u\r\n\r\n%s", extra,
             (unsigned)strlen(body), body);
    return fake_add(s, b);
}

/* A cacheable page. */
static void respond_page(FakeServer *s, const char *body)
{
    respond(s, "Cache-Control: max-age=600\r\n", body);
}

static void go(Env *e, const char *url)
{
    au_bw_navigate(e->w, url);
    settle(e, 500);
    paint(e->w);
}

static void new_tab(Env *e, const char *url)
{
    au_bw_new_tab(e->w, url, true, e->w->ntabs);
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

#define TALL "<div style='height:3000px'>tall</div>"

/* ---- Unload and load again ---- */

static void test_unload_reload(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    respond_page(s, "<!DOCTYPE html><title>One</title><p>page one" TALL);
    respond_page(s, "<!DOCTYPE html><title>Two</title><p>page two" TALL "<p>end of two");
    respond_page(s, "<!DOCTYPE html><title>Other</title><p>the other tab");
    new_tab(&e, "http://site.test/one");
    go(&e, "http://site.test/two");
    au_bw_scroll_to(e.w, 0, 700);
    paint(e.w);
    CHECK(tab(&e)->scroll_y == 700 && au_page_kept_count(page(&e)) == 1, "tab 0 is at two, scrolled, one kept");
    size_t loaded = au_page_memory_estimate(page(&e));

    CHECK(!au_bw_can_unload_tab(e.w, 0) && !au_bw_unload_tab(e.w, 0), "the active tab is not unloaded");
    new_tab(&e, "http://site.test/other");
    CHECK(s->nreq == 3 && e.w->active == 1, "three pages loaded");

    CHECK(au_bw_can_unload_tab(e.w, 0), "the background tab can be unloaded");
    CHECK(au_bw_unload_tab(e.w, 0), "and is");
    AuTab *t0 = e.w->tabs[0];
    CHECK(au_page_discarded(t0->page) && !t0->loading, "its page is discarded");
    CHECK(!strcmp(au_page_title(t0->page), "Two") && !strcmp(au_page_url(t0->page), "http://site.test/two"),
          "title and address stay: %s %s", au_page_title(t0->page), au_page_url(t0->page));
    CHECK(au_page_kept_count(t0->page) == 0, "its kept documents went");
    CHECK(au_page_memory_estimate(t0->page) * 2 < loaded,"it holds little now: %u bytes, %u loaded",
          (unsigned)au_page_memory_estimate(t0->page), (unsigned)loaded);
    CHECK(t0->nav.n == 2 && t0->nav.index == 1 && t0->scroll_y == 700, "history list and position stay");
    CHECK(!au_bw_can_unload_tab(e.w, 0), "an unloaded tab is not unloaded twice");
    run_for(&e, 500);
    paint(e.w);
    CHECK(s->nreq == 3 && au_page_discarded(t0->page) && t0->scroll_y == 700,
          "nothing loads while it stays in the background (%d requests)", s->nreq);

    /* The session file names it as before. */
    AuBuf ses;
    au_buf_init(&ses);
    au_browser_session_serialize(e.b, &ses);
    CHECK(strstr(au_buf_cstr(&ses), "entry\thttp://site.test/two\tTwo\t700"), "session: %s", au_buf_cstr(&ses));
    au_buf_free(&ses);

    /* Selecting it loads the entry again from the HTTP cache. */
    au_bw_select_tab(e.w, 0);
    CHECK(!au_page_discarded(page(&e)) && tab(&e)->loading, "selecting it starts the load");
    settle(&e, 500);
    paint(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 3, "the reload came from the cache (%d requests)", s->nreq);
    CHECK(page_text_has(&e, "end of two") && !strcmp(au_page_title(page(&e)), "Two"), "two is shown again");
    CHECK(!strcmp(au_page_url(page(&e)), "http://site.test/two"), "address %s", au_page_url(page(&e)));
    CHECK(tab(&e)->scroll_y == 700, "scroll position restored: %d", tab(&e)->scroll_y);
    CHECK(tab(&e)->nav.n == 2 && tab(&e)->nav.index == 1, "history list: %d entries at %d", tab(&e)->nav.n,
          tab(&e)->nav.index);
    au_bw_back(e.w);
    settle(&e, 500);
    CHECK(s->nreq == 3 && page_text_has(&e, "page one"), "Back goes to one, from the cache (%d)", s->nreq);
    au_bw_forward(e.w);
    settle(&e, 500);
    CHECK(page_text_has(&e, "end of two"), "and Forward to two");

    /* Reload on an unloaded background tab loads it there. */
    CHECK(au_bw_unload_tab(e.w, 1), "the other tab is unloaded");
    au_bw_reload_tab(e.w, 1);
    CHECK(e.w->tabs[1]->loading, "Reload starts a load in an unloaded tab");
    settle(&e, 500);
    CHECK(!au_page_discarded(e.w->tabs[1]->page) && !strcmp(au_page_title(e.w->tabs[1]->page), "Other"),
          "Reload loads an unloaded tab (%d requests)", s->nreq);

    /* Closing an unloaded tab, and closing the tab beside one. */
    CHECK(au_bw_unload_tab(e.w, 1), "unloaded again");
    au_bw_close_tab(e.w, 0);
    CHECK(e.w->ntabs == 1 && e.w->active == 0, "the tab beside it closed");
    settle(&e, 500);
    paint(e.w);
    CHECK(!au_page_discarded(page(&e)) && page_text_has(&e, "the other tab"),
          "the unloaded tab became active and loaded");
    env_free(&e);
}

/* ---- Tabs that are not unloaded ---- */

static void test_ineligible(void)
{
    Env e;
    env_init(&e);
    e.b->prefs.javascript = true;
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    /* 0: a plain page. */
    respond_page(s, "<!DOCTYPE html><title>plain</title><p>plain");
    new_tab(&e, "http://site.test/plain");
    /* 1: a form the user typed in. */
    respond_page(s, "<!DOCTYPE html><title>form</title><form><input name=q></form>");
    new_tab(&e, "http://site.test/form");
    bw_key(&e, AU_KEY_TAB, 0);
    bw_key(&e, 0, 'x');
    /* 2: an unload listener. */
    respond_page(s, "<!DOCTYPE html><title>unload</title><script>addEventListener('beforeunload', function(){})"
                    "</script><p>x");
    new_tab(&e, "http://site.test/unload");
    /* 3: a canvas script drew on. */
    respond_page(s, "<!DOCTYPE html><title>canvas</title><canvas id=c width=20 height=20></canvas><script>"
                    "document.getElementById('c').getContext('2d').fillRect(1,1,5,5)</script>");
    new_tab(&e, "http://site.test/canvas");
    /* 4: a form the user did not touch (script set its value). */
    respond_page(s, "<!DOCTYPE html><title>untouched</title><form><input name=q id=q></form><script>"
                    "document.getElementById('q').value='set by script'</script>");
    new_tab(&e, "http://site.test/untouched");
    /* 5: the answer to a POST. */
    respond_page(s, "<!DOCTYPE html><title>poster</title><form method=post action=/posted><input name=a value=1>"
                    "<input type=submit id=go></form>");
    new_tab(&e, "http://site.test/poster");
    respond(s, "", "<!DOCTYPE html><title>posted</title><p>posted");
    au_page_navigate_post(page(&e), "http://site.test/posted", "a=1", 3, "application/x-www-form-urlencoded");
    settle(&e, 500);
    CHECK(au_page_is_post(page(&e)), "tab 5 shows a POST result");
    /* 6: the active tab. */
    respond_page(s, "<!DOCTYPE html><title>active</title><p>active");
    new_tab(&e, "http://site.test/active");
    CHECK(e.w->ntabs == 7 && e.w->active == 6, "seven tabs");

    CHECK(au_bw_can_unload_tab(e.w, 0), "a plain page can go");
    CHECK(!au_bw_can_unload_tab(e.w, 1), "a form the user typed in stays");
    CHECK(!au_bw_can_unload_tab(e.w, 2), "a page with a beforeunload listener stays");
    CHECK(!au_bw_can_unload_tab(e.w, 3), "a page with a drawn canvas stays");
    CHECK(au_bw_can_unload_tab(e.w, 4), "a form only script filled can go");
    CHECK(!au_bw_can_unload_tab(e.w, 5), "a POST result stays");
    CHECK(!au_bw_can_unload_tab(e.w, 6), "the active tab stays");
    CHECK(!au_bw_can_unload_tab(e.w, 7) && !au_bw_can_unload_tab(e.w, -1), "indexes out of range");

    /* A tab that is loading stays. */
    respond_page(s, "<!DOCTYPE html><title>slow</title><p>slow");
    au_bw_new_tab(e.w, "http://site.test/slow", false, e.w->ntabs);
    CHECK(e.w->tabs[7]->loading && !au_bw_can_unload_tab(e.w, 7), "a loading tab stays");
    settle(&e, 500);
    CHECK(au_bw_can_unload_tab(e.w, 7), "and can go once it has loaded");

    /* The rules take every tab that can go and no other. */
    size_t n = au_browser_release_memory(e.b);
    CHECK(n == 1 && au_browser_kept_documents(e.b, NULL) == 0, "low memory: the kept document first (%u)", (unsigned)n);
    n = au_browser_release_memory(e.b);
    CHECK(n == 3, "low memory unloaded %u tabs", (unsigned)n);
    bool want[8] = {true, false, false, false, true, false, false, true};
    for (int i = 0; i < 8; i++)
        CHECK(au_page_discarded(e.w->tabs[i]->page) == want[i], "tab %d %s", i,
              want[i] ? "was unloaded" : "was left alone");
    CHECK(!au_browser_unload_lru(e.b), "nothing more to unload");
    CHECK(page_text_has(&e, "active"), "the active tab still shows its page");
    /* The typed text is still there. */
    au_bw_select_tab(e.w, 1);
    AuBuf v;
    au_buf_init(&v);
    au_bw_page_edit(e.w, AU_PAGE_EDIT_SELECT_ALL, NULL, 0, NULL);
    au_bw_page_edit(e.w, AU_PAGE_EDIT_COPY, NULL, 0, &v);
    CHECK(!strcmp(au_buf_cstr(&v), "x"), "the typed text stayed: '%s'", au_buf_cstr(&v));
    au_buf_free(&v);
    env_free(&e);
}

/* ---- The memory rules ---- */

static void test_rules(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    const char *names[4] = {"a", "b", "c", "d"};
    for (int i = 0; i < 4; i++) {
        char body[128], url[64];
        snprintf(body, sizeof body, "<!DOCTYPE html><title>%s</title><p>page %s", names[i], names[i]);
        respond_page(s, body);
        snprintf(url, sizeof url, "http://site.test/%s", names[i]);
        new_tab(&e, url);
        run_for(&e, 100);
    }
    /* Shown in the order a, b, c, d; then b again, then d: the least
     * recently shown is a, then c, then b. */
    au_bw_select_tab(e.w, 1);
    run_for(&e, 100);
    au_bw_select_tab(e.w, 3);
    run_for(&e, 100);
    /* Tab d gets a kept document. */
    respond_page(s, "<!DOCTYPE html><title>d2</title><p>page d2");
    go(&e, "http://site.test/d2");
    CHECK(au_browser_kept_documents(e.b, NULL) == 1, "one document is kept");

    /* Plenty of memory: nothing happens. */
    run_for(&e, 3000);
    CHECK(e.b->unloaded_count == 0 && au_browser_kept_documents(e.b, NULL) == 1, "no pressure, no unload");

    /* Under the threshold, with the preference off: nothing. */
    e.b->prefs.unload_tabs = false;
    e.free_kb = 1000;
    run_for(&e, 3000);
    CHECK(e.b->unloaded_count == 0 && au_browser_kept_documents(e.b, NULL) == 1, "preference off: no unload");
    CHECK(au_bw_can_unload_tab(e.w, 0), "the user's command does not ask the preference");

    /* On: the kept document goes first, then one tab a second, oldest first. */
    e.b->prefs.unload_tabs = true;
    run_for(&e, 500);
    CHECK(au_browser_kept_documents(e.b, NULL) == 0 && e.b->unloaded_count == 0, "the kept document went first");
    run_for(&e, 1000);
    CHECK(e.b->unloaded_count == 1 && au_page_discarded(e.w->tabs[0]->page), "then the tab shown longest ago (a)");
    run_for(&e, 1000);
    CHECK(e.b->unloaded_count == 2 && au_page_discarded(e.w->tabs[2]->page), "then c");
    e.free_kb = 1 << 20; /* memory came back */
    run_for(&e, 3000);
    CHECK(e.b->unloaded_count == 2 && !au_page_discarded(e.w->tabs[1]->page), "b stays once memory is back");
    CHECK(!au_page_discarded(e.w->tabs[3]->page), "the active tab was never a candidate");

    /* The softer rule: a tab not shown for the idle time, while memory
     * is under the softer threshold. */
    e.b->prefs.unload_idle_minutes = 1;
    e.free_kb = (uint32_t)e.b->prefs.unload_idle_kb - 1;
    run_for(&e, 5000);
    CHECK(!au_page_discarded(e.w->tabs[1]->page), "b was shown less than a minute ago");
    run_for(&e, 60000);
    CHECK(au_page_discarded(e.w->tabs[1]->page) && e.b->unloaded_count == 3, "after a minute in the background b goes");
    e.b->prefs.unload_idle_minutes = 0;

    /* The allocator's release handler: kept documents, then tabs. */
    au_bw_select_tab(e.w, 0);
    settle(&e, 500);
    au_bw_select_tab(e.w, 1);
    settle(&e, 500);
    au_bw_select_tab(e.w, 3);
    au_bw_back(e.w);
    settle(&e, 500);
    e.free_kb = 1 << 20;
    CHECK(au_browser_kept_documents(e.b, NULL) == 1 && !au_page_discarded(e.w->tabs[0]->page), "loaded again");
    CHECK(au_browser_release_memory(e.b) == 1 && au_browser_kept_documents(e.b, NULL) == 0,
          "low memory: the kept document first");
    CHECK(!au_page_discarded(e.w->tabs[0]->page), "and no tab yet");
    CHECK(au_browser_release_memory(e.b) == 2, "low memory again: the two loaded background tabs");
    CHECK(au_page_discarded(e.w->tabs[0]->page) && au_page_discarded(e.w->tabs[1]->page) &&
              !au_page_discarded(e.w->tabs[3]->page),
          "every tab but the active one is unloaded");
    CHECK(au_browser_release_memory(e.b) == 0, "nothing is left to free");
    CHECK(page_text_has(&e, "page d"), "the active tab shows its page");
    env_free(&e);
}

/* ---- Session restore ---- */

static void test_session(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    const char *names[3] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
        char body[160], url[64];
        snprintf(body, sizeof body, "<!DOCTYPE html><title>Title %s</title><p>page %s" TALL, names[i], names[i]);
        respond_page(s, body);
        snprintf(url, sizeof url, "http://site.test/%s", names[i]);
        new_tab(&e, url);
        au_bw_scroll_to(e.w, 0, 100 * (i + 1));
    }
    au_bw_select_tab(e.w, 1);
    AuBuf out;
    au_buf_init(&out);
    au_browser_session_serialize(e.b, &out);
    AuSession ses;
    CHECK(au_session_parse(&ses, out.data, out.len) && ses.nwindows == 1 && ses.windows[0].ntabs == 3,
          "the session parses");
    au_buf_free(&out);
    au_cache_clear(e.cache);
    int before = s->nreq;

    /* Restored: only the active tab loads. */
    respond_page(s, "<!DOCTYPE html><title>Title b</title><p>page b restored" TALL);
    AuBrowserWindow *r = au_browser_window_new(e.b, 800, 600);
    au_bw_restore(r, &ses.windows[0]);
    au_session_free(&ses);
    for (int i = 0; i < 500 && au_browser_busy(e.b); i++)
        step(&e);
    CHECK(r->ntabs == 3 && r->active == 1, "three tabs, the second active");
    CHECK(s->nreq == before + 1, "one request for three restored tabs (%d)", s->nreq - before);
    CHECK(!au_page_discarded(r->tabs[1]->page) && !strcmp(au_page_title(r->tabs[1]->page), "Title b"),
          "the active tab loaded");
    CHECK(au_page_discarded(r->tabs[0]->page) && au_page_discarded(r->tabs[2]->page), "the others are unloaded");
    CHECK(!strcmp(au_page_title(r->tabs[0]->page), "Title a") &&
              !strcmp(au_page_url(r->tabs[2]->page), "http://site.test/c"),
          "with their titles and addresses: %s, %s", au_page_title(r->tabs[0]->page), au_page_url(r->tabs[2]->page));
    /* Saved again before they load, the session is the same. */
    au_buf_init(&out);
    au_browser_session_serialize(e.b, &out);
    CHECK(strstr(au_buf_cstr(&out), "entry\thttp://site.test/c\tTitle c\t300"), "an unloaded tab keeps its position");
    au_buf_free(&out);

    respond_page(s, "<!DOCTYPE html><title>Title c</title><p>page c restored" TALL);
    au_bw_select_tab(r, 2);
    for (int i = 0; i < 500 && au_browser_busy(e.b); i++)
        step(&e);
    paint(r);
    for (int i = 0; i < 500 && au_browser_busy(e.b); i++)
        step(&e);
    CHECK(s->nreq == before + 2 && !au_page_discarded(r->tabs[2]->page), "selecting one loads it (%d)",
          s->nreq - before);
    CHECK(r->tabs[2]->scroll_y == 300, "at its position: %d", r->tabs[2]->scroll_y);

    /* With the preference off every tab loads at once. */
    e.b->prefs.unload_restored = false;
    au_buf_init(&out);
    au_browser_session_serialize(e.b, &out);
    CHECK(au_session_parse(&ses, out.data, out.len), "parses again");
    au_buf_free(&out);
    AuBrowserWindow *r2 = au_browser_window_new(e.b, 800, 600);
    au_bw_restore(r2, &ses.windows[1]);
    au_session_free(&ses);
    CHECK(r2->ntabs == 3 && r2->tabs[0]->loading && r2->tabs[2]->loading && !au_page_discarded(r2->tabs[0]->page),
          "preference off: every restored tab loads");
    for (int i = 0; i < 500 && au_browser_busy(e.b); i++)
        step(&e);
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_unload_reload();
    test_ineligible();
    test_rules();
    test_session();
    for (int i = 0; i < nresponses; i++)
        free(responses[i]);
    au_fontset_free(fonts);
    printf("unload: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
