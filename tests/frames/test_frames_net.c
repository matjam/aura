/* Nested documents in a browser window, over the fake transport: a frame
 * loads through the page's fetcher with the embedding document as
 * referrer, the framing headers of a real response refuse it, a listed
 * third-party frame host is never contacted, an http frame is mixed
 * content in an https page, a click and typed keys reach a frame, and a
 * kept document comes back with its frames. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
#include "net/blocklist.h"
#include "net/loader.h"
#include "net/policy.h"

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
    AuBlocklist *bl;
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
    e->bl = au_blocklist_new();
    au_net_loader_set_blocklist(e->l, e->bl);
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
    au_blocklist_free(e->bl);
    au_net_free(e->net);
    fake_free(&e->f);
}

static void settle(Env *e, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
        if (!au_browser_busy(e->b) && i > 5)
            break;
    }
}

static void paint(AuBrowserWindow *w)
{
    AuSurface *s = au_surface_new(w->width, w->height);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, w->width, w->height});
    au_canvas_free(c);
    au_surface_free(s);
}

static AuPage *page(Env *e) { return au_bw_active_tab(e->w)->page; }

static char *responses[64];
static int nresponses;

static void respond(FakeServer *s, const char *extra, const char *body)
{
    size_t cap = strlen(body) + strlen(extra) + 256;
    char *b = malloc(cap); /* the fake keeps the pointer: freed at the end of main */
    if (nresponses < 64)
        responses[nresponses++] = b;
    snprintf(b, cap, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%sContent-Length: %u\r\n\r\n%s", extra,
             (unsigned)strlen(body), body);
    fake_add(s, b);
}

static void go(Env *e, const char *url)
{
    au_bw_navigate(e->w, url);
    settle(e, 500);
    paint(e->w);
    settle(e, 50);
}

static const char *js(Env *e, const char *src)
{
    static AuBuf out;
    au_buf_free(&out);
    au_buf_init(&out);
    au_page_eval(page(e), src, &out);
    settle(e, 50);
    return au_buf_cstr(&out);
}

static bool console_has(Env *e, const char *needle)
{
    for (size_t i = 0; i < au_page_console_count(page(e)); i++)
        if (strstr(au_page_console_line(page(e), i, NULL), needle))
            return true;
    return false;
}

static void use_list(AuBlocklist *bl, const char *id, const char *text)
{
    AuBlockBuilder *b = au_block_builder_new(NULL);
    au_block_builder_feed(b, text, strlen(text));
    uint8_t *data = NULL;
    size_t len = 0;
    if (au_block_builder_finish(b, 0) == 1 && au_block_builder_take(b, &data, &len))
        au_blocklist_attach(bl, id, data, len, NULL);
    au_block_builder_free(b);
}

static void test_load(void)
{
    Env e;
    env_init(&e);
    use_list(e.bl, "test", "ads.test\n");
    au_blocklist_set_enabled(e.bl, true);
    FakeServer *site = fake_server(&e.f, "site.test", 80);
    FakeServer *other = fake_server(&e.f, "other.test", 80);
    FakeServer *deny = fake_server(&e.f, "deny.test", 80);
    FakeServer *ads = fake_server(&e.f, "ads.test", 80);
    respond(site, "", "<!DOCTYPE html><title>Top</title><body style='margin:0'>"
                      "<script>var got=[];addEventListener('message',function(e){got.push(e.data+'@'+e.origin)})</script>"
                      "<iframe id=a src='http://other.test/kid' style='display:block;border:0;width:300px;height:100px'>"
                      "</iframe><iframe id=b src='http://deny.test/x'></iframe>"
                      "<iframe id=c src='http://ads.test/ad'></iframe><p>top text");
    respond(other, "", "<!DOCTYPE html><body style='margin:0'><input id=i style='display:block;width:200px;height:20px;"
                       "border:0;margin:0;padding:0'><p>kid text<script>parent.postMessage('kid ready','*');"
                       "document.getElementById('i').addEventListener('input',function(){parent.postMessage("
                       "'typed '+this.value,'*')})</script>");
    respond(deny, "X-Frame-Options: DENY\r\n", "<!DOCTYPE html><body>secret");
    respond(ads, "", "<!DOCTYPE html><body>ad");
    au_bw_new_tab(e.w, "http://site.test/", true, -1);
    settle(&e, 500);
    paint(e.w);
    settle(&e, 50);
    CHECK(other->nreq == 1 && deny->nreq == 1, "frames requested: %d %d", other->nreq, deny->nreq);
    CHECK(ads->nreq == 0 && ads->connections == 0, "a listed third-party frame host was contacted");
    AuPageStats st;
    au_page_stats(page(&e), &st);
    CHECK(st.blocked == 1, "blocked count %u", (unsigned)st.blocked);
    CHECK(other->nreq == 1 && strstr(other->requests[0], "Referer: http://site.test/"),
          "the frame request names the embedding page: %s", other->nreq ? other->requests[0] : "");
    CHECK(other->nreq == 1 && strstr(other->requests[0], "Accept: text/html"), "requested as a document: %s",
          other->nreq ? other->requests[0] : "");
    CHECK(!strcmp(js(&e, "got.join()"), "kid ready@http://other.test"), "message from the frame: %s",
          js(&e, "got.join()"));
    CHECK(console_has(&e, "does not allow framing"), "X-Frame-Options refusal reported");
    CHECK(!strcmp(js(&e, "window.length + ' ' + document.title"), "3 Top"), "%s", js(&e, "window.length"));
    CHECK(!au_page_loading(page(&e)), "the page finished loading");
    /* A click in the frame's field, then keys. */
    AuEvent ev;
    memset(&ev, 0, sizeof ev);
    AuIRect area = e.w->layout.page;
    ev.x = area.x0 + 20;
    ev.y = area.y0 + 10;
    ev.clicks = 1;
    ev.type = AU_EVENT_MOUSE_DOWN;
    au_bw_mouse(e.w, &ev);
    ev.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(e.w, &ev);
    CHECK(au_bw_page_editing(e.w), "the frame's field is being edited");
    memset(&ev, 0, sizeof ev);
    ev.type = AU_EVENT_KEY_DOWN;
    ev.ch = 'x';
    au_bw_key(e.w, &ev);
    settle(&e, 20);
    CHECK(!strcmp(js(&e, "got[got.length-1]"), "typed x@http://other.test"), "typing reaches the frame: %s",
          js(&e, "got.join()"));
    /* Select All and Copy work on the frame's field. */
    AuBuf v;
    au_buf_init(&v);
    au_bw_page_edit(e.w, AU_PAGE_EDIT_SELECT_ALL, NULL, 0, NULL);
    au_bw_page_edit(e.w, AU_PAGE_EDIT_COPY, NULL, 0, &v);
    CHECK(!strcmp(au_buf_cstr(&v), "x"), "copy from the frame's field: '%s'", au_buf_cstr(&v));
    au_buf_free(&v);

    /* A script POST (XMLHttpRequest) carries its body. */
    respond(site, "", "ok");
    js(&e, "var x = new XMLHttpRequest(); x.open('POST', '/api'); x.setRequestHeader('Content-Type', 'text/plain');"
           "x.onload = function () { got.push('xhr ' + x.status + ' ' + x.responseText) }; x.send('hello=1')");
    settle(&e, 100);
    CHECK(!strcmp(js(&e, "got[got.length-1]"), "xhr 200 ok"), "a script POST completes: %s", js(&e, "got.join()"));
    CHECK(site->nreq == 2 && strstr(site->requests[1], "POST /api") && strstr(site->requests[1], "hello=1") &&
              strstr(site->requests[1], "Content-Type: text/plain"),
          "the POST reached the server with its body: %s", site->nreq > 1 ? site->requests[1] : "");
    /* The document is kept with its frames; Back shows it with no request. */
    respond(site, "", "<!DOCTYPE html><title>Next</title><p>next");
    go(&e, "http://site.test/next");
    CHECK(au_page_kept_count(page(&e)) == 1, "the page with frames is kept: %u", (unsigned)au_page_kept_count(page(&e)));
    int before = other->nreq + site->nreq + deny->nreq;
    au_bw_back(e.w);
    settle(&e, 100);
    paint(e.w);
    au_page_stats(page(&e), &st);
    CHECK(st.restored == 1 && other->nreq + site->nreq + deny->nreq == before, "Back: restored %u, requests %d then %d",
          (unsigned)st.restored, before, other->nreq + site->nreq + deny->nreq);
    CHECK(!strcmp(js(&e, "window.length + ' ' + got.length"), "3 3"), "the kept frames are there: %s",
          js(&e, "window.length + ' ' + got.length"));
    /* its frame still works (the field's text is still selected) */
    memset(&ev, 0, sizeof ev);
    ev.type = AU_EVENT_KEY_DOWN;
    ev.ch = 'y';
    au_bw_key(e.w, &ev);
    settle(&e, 20);
    CHECK(!strcmp(js(&e, "got[got.length-1]"), "typed y@http://other.test"), "the kept frame takes keys: %s",
          js(&e, "got.join()"));
    env_free(&e);
}

/* The loader's mixed content rule for frames: blocked, never upgraded. */
static void test_mixed(void)
{
    AuUrl doc, frame, same;
    au_url_parse(&doc, "https://secure.test/", NULL);
    au_url_parse(&frame, "http://plain.test/frame", NULL);
    au_url_parse(&same, "https://other.test/frame", NULL);
    CHECK(au_mixed_classify(&doc, &frame, AU_DEST_IFRAME) == AU_MIXED_BLOCKABLE, "an http frame in an https page");
    CHECK(au_mixed_classify(&doc, &same, AU_DEST_IFRAME) == AU_MIXED_NONE, "an https frame in an https page");
    au_url_free(&doc);
    au_url_free(&frame);
    au_url_free(&same);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_load();
    test_mixed();
    au_fontset_free(fonts);
    for (int i = 0; i < nresponses; i++)
        free(responses[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
