/* Site icons (src/browser/favicon.h) over the fake transport: the icon a
 * page names or /favicon.ico is fetched after the load, decoded to
 * 16 x 16 and shown for the tab; a cross-host icon goes without cookies;
 * blocked hosts and mixed content are never asked; a host without an
 * icon is not asked twice; the store survives a round trip through its
 * file format. */
#include "fakenet.h"

#include "browser/browser.h"
#include "browser/favicon.h"
#include "image/image.h"
#include "net/blocklist.h"
#include "net/cookies.h"
#include "net/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static AuFontSet *fonts;
static char *ico;
static size_t ico_len;

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuCookieJar *jar;
    AuBlocklist *bl;
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
    e->jar = au_cookie_jar_new();
    au_net_set_cookie_jar(e->net, e->jar);
    /* Third-party cookies allowed, so that only omit_credentials keeps
     * them from an icon on another host. */
    au_net_config(e->net)->block_third_party_cookies = false;
    au_cookie_jar_set_policy(e->jar, au_net_config(e->net)->cookie_policy, false);
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
    e->b->site_icons = true;
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
    au_cookie_jar_free(e->jar);
    fake_free(&e->f);
}

/* Runs until the page and the icon fetches are done. */
static void settle(Env *e)
{
    for (int i = 0; i < 400 && (au_browser_busy(e->b) || au_favicons_pending(e->b->favicons) || i < 5); i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
    }
}

static char *kept[64];
static int nkept;

static FakeResponse *respond_bytes(FakeServer *s, const char *status, const char *type, const char *extra,
                                   const char *body, size_t len)
{
    char *b = malloc(len + strlen(extra) + 256);
    if (nkept < 64)
        kept[nkept++] = b;
    int n = snprintf(b, 256 + strlen(extra), "HTTP/1.1 %s\r\nContent-Type: %s\r\n%sContent-Length: %u\r\n\r\n", status,
                     type, extra, (unsigned)len);
    memcpy(b + n, body, len);
    FakeResponse *r = fake_add(s, b);
    r->len = (size_t)n + len;
    return r;
}

static void respond_html(FakeServer *s, const char *extra, const char *html)
{
    respond_bytes(s, "200 OK", "text/html", extra, html, strlen(html));
}

static void respond_icon(FakeServer *s)
{
    respond_bytes(s, "200 OK", "image/x-icon", "", ico, ico_len);
}

static const AuSurface *icon_of(Env *e, const char *url)
{
    return au_favicons_for_url(e->b->favicons, url);
}

static bool request_has(FakeServer *s, int i, const char *needle)
{
    return i < s->nreq && s->requests[i] && strstr(s->requests[i], needle) != NULL;
}

static void go(Env *e, const char *url)
{
    if (e->w->ntabs == 0)
        au_bw_new_tab(e->w, url, true, -1);
    else
        au_bw_navigate(e->w, url);
    settle(e);
}

static void paint(AuBrowserWindow *w, AuSurface *s)
{
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, w->width, w->height});
    au_canvas_free(c);
}

static void test_default_icon(void)
{
    Env e;
    env_init(&e);
    FakeServer *a = fake_server(&e.f, "a.test", 80);
    respond_html(a, "Set-Cookie: sid=1\r\n", "<!doctype html><title>A</title><p>hello");
    respond_icon(a);
    /* The tab strip before the icon, to compare. */
    AuSurface *before = au_surface_new(800, 600), *after = au_surface_new(800, 600);
    go(&e, "http://a.test/page");
    CHECK(a->nreq == 2, "the page, then the icon: %d requests", a->nreq);
    CHECK(request_has(a, 1, "GET /favicon.ico "), "/favicon.ico was asked for: %.60s", a->nreq > 1 ? a->requests[1] : "");
    CHECK(request_has(a, 1, "Cookie: sid=1"), "the site's own icon request carries its cookie");
    const AuSurface *ic = icon_of(&e, "http://a.test/other/page");
    CHECK(ic && ic->width == 16 && ic->height == 16, "a 16 x 16 icon is stored for the host");
    CHECK(icon_of(&e, "https://a.test/") == ic, "for https too");
    CHECK(!icon_of(&e, "http://b.test/") && !icon_of(&e, "about:home") && !icon_of(&e, "file:///a.test/x"),
          "not for other hosts or schemes");
    paint(e.w, after);
    /* The same window with the store emptied paints the page glyph. */
    au_favicons_clear(e.b->favicons);
    paint(e.w, before);
    AuIRect tr = au_chrome_tab_rect(&e.w->layout, 0);
    int diff = 0;
    for (int y = tr.y0; y < tr.y1; y++)
        for (int x = tr.x0; x < tr.x0 + 22; x++)
            if (before->pixels[y * before->stride + x] != after->pixels[y * after->stride + x])
                diff++;
    CHECK(diff > 20, "the tab shows the icon: %d pixels differ from the page glyph", diff);
    au_surface_free(before);
    au_surface_free(after);
    env_free(&e);
}

static void test_link_and_sizes(void)
{
    Env e;
    env_init(&e);
    /* Two hosts of one site: a cookie for the whole site would go to
     * both, so only omit_credentials keeps it from the icon's host. */
    FakeServer *a = fake_server(&e.f, "www.site.test", 80), *cdn = fake_server(&e.f, "static.site.test", 80);
    respond_html(a, "Set-Cookie: sid=1; Domain=site.test\r\n",
                 "<!doctype html><title>A</title>"
                 "<link rel=\"apple-touch-icon\" href=\"/touch.png\">"
                 "<link rel=\"icon\" sizes=\"64x64\" href=\"/big.ico\">"
                 "<link rel=\"shortcut icon\" sizes=\"16x16 32x32\" href=\"http://static.site.test/small.ico\">"
                 "<link rel=\"mask-icon\" href=\"/mask.svg\">"
                 "<body><link rel=icon href=/body.ico><p>hello");
    respond_icon(cdn);
    go(&e, "http://www.site.test/");
    CHECK(a->nreq == 1, "nothing but the page from the site: %d", a->nreq);
    CHECK(cdn->nreq == 1 && request_has(cdn, 0, "GET /small.ico "), "the 16 pixel icon was chosen: %.50s",
          cdn->nreq > 0 ? cdn->requests[0] : "");
    CHECK(!request_has(cdn, 0, "Cookie:"), "an icon on another host is asked for without cookies");
    CHECK(request_has(cdn, 0, "Referer: http://www.site.test/"), "as a subresource of the page");
    CHECK(icon_of(&e, "http://www.site.test/") != NULL, "stored for the page's host");
    CHECK(!icon_of(&e, "http://static.site.test/"), "not for the icon's host");
    /* The site cookie does reach that host on an ordinary visit, so the
     * check above means something; its 404 is no icon. */
    respond_html(cdn, "", "<!doctype html><title>cdn</title>x");
    respond_bytes(cdn, "404 Not Found", "text/plain", "", "no", 2);
    go(&e, "http://static.site.test/");
    CHECK(request_has(cdn, 1, "Cookie: sid=1"), "the site's cookie goes to that host otherwise");
    CHECK(cdn->nreq == 3 && request_has(cdn, 2, "GET /favicon.ico ") && request_has(cdn, 2, "Cookie: sid=1"),
          "its own icon request carries the cookie");
    CHECK(!icon_of(&e, "http://static.site.test/"), "a 404 is no icon");
    env_free(&e);
}

static void test_not_asked_twice(void)
{
    Env e;
    env_init(&e);
    FakeServer *a = fake_server(&e.f, "a.test", 80), *b = fake_server(&e.f, "b.test", 80);
    respond_html(a, "", "<!doctype html><title>A</title>one");
    respond_bytes(a, "404 Not Found", "text/plain", "", "no", 2);
    respond_html(a, "", "<!doctype html><title>A</title>two");
    go(&e, "http://a.test/1");
    go(&e, "http://a.test/2");
    CHECK(a->nreq == 3, "a host without an icon is asked once: %d requests", a->nreq);
    /* A host with an icon is asked again only when the icon is old. */
    respond_html(b, "", "<!doctype html><title>B</title>one");
    respond_icon(b);
    respond_html(b, "", "<!doctype html><title>B</title>two");
    respond_html(b, "", "<!doctype html><title>B</title>three");
    respond_bytes(b, "500 Oops", "text/plain", "", "no", 2);
    go(&e, "http://b.test/1");
    go(&e, "http://b.test/2");
    CHECK(b->nreq == 3 && icon_of(&e, "http://b.test/"), "a fresh icon is not fetched again: %d requests", b->nreq);
    e.f.wall += AU_FAVICON_FRESH_S + 60;
    go(&e, "http://b.test/3");
    CHECK(b->nreq == 5 && request_has(b, 4, "GET /favicon.ico "), "an old icon is refreshed: %d requests", b->nreq);
    CHECK(icon_of(&e, "http://b.test/") != NULL, "and kept when the refresh fails");
    /* Junk that is not an image. */
    FakeServer *c = fake_server(&e.f, "c.test", 80);
    respond_html(c, "", "<!doctype html><title>C</title>x");
    respond_bytes(c, "200 OK", "image/x-icon", "", "this is not an icon at all", 26);
    go(&e, "http://c.test/");
    CHECK(c->nreq == 2 && !icon_of(&e, "http://c.test/"), "junk bytes give no icon");
    /* Images off: no icon requests. */
    e.b->prefs.load_images = false;
    FakeServer *d = fake_server(&e.f, "d.test", 80);
    respond_html(d, "", "<!doctype html><title>D</title>x");
    go(&e, "http://d.test/");
    CHECK(d->nreq == 1, "with images off the icon is not fetched: %d", d->nreq);
    env_free(&e);
}

static void test_blocked(void)
{
    Env e;
    env_init(&e);
    /* A listed icon host is never connected to. */
    AuBlockBuilder *bb = au_block_builder_new(NULL);
    const char *list = "ads.test\n";
    au_block_builder_feed(bb, list, strlen(list));
    uint8_t *data = NULL;
    size_t len = 0;
    if (au_block_builder_finish(bb, 0) == 1 && au_block_builder_take(bb, &data, &len))
        au_blocklist_attach(e.bl, "t", data, len, NULL);
    au_block_builder_free(bb);
    au_blocklist_set_enabled(e.bl, true);
    FakeServer *a = fake_server(&e.f, "a.test", 80), *ads = fake_server(&e.f, "ads.test", 80);
    respond_html(a, "", "<!doctype html><title>A</title><link rel=icon href=\"http://ads.test/i.ico\">x");
    respond_icon(ads);
    go(&e, "http://a.test/");
    CHECK(ads->connections == 0 && ads->nreq == 0, "a blocked host is not asked for an icon: %d connections",
          ads->connections);
    CHECK(!icon_of(&e, "http://a.test/"), "and there is no icon");
    /* Mixed content: an http icon for an https page is refused before any
     * connection. */
    FakeServer *plain = fake_server(&e.f, "plain.test", 80);
    respond_icon(plain);
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e.pf, &fe);
    au_favicons_request(e.b->favicons, &fe, "https://secure.test/", "http://plain.test/i.ico", e.f.wall);
    settle(&e);
    CHECK(plain->connections == 0, "an http icon of an https page is not fetched: %d connections", plain->connections);
    CHECK(!icon_of(&e, "https://secure.test/"), "no icon for it");
    /* Not http or https: nothing starts. */
    au_favicons_request(e.b->favicons, &fe, "http://x.test/", "data:image/png;base64,AAAA", e.f.wall);
    au_favicons_request(e.b->favicons, &fe, "file:///x", "http://plain.test/i.ico", e.f.wall);
    CHECK(au_favicons_pending(e.b->favicons) == 0, "other schemes are ignored");
    env_free(&e);
}

static void test_store(void)
{
    AuFavicons *f = au_favicons_new();
    AuSurface *s = au_image_decode_favicon(ico, ico_len, 16);
    CHECK(s != NULL, "the test icon decodes");
    if (!s)
        return;
    au_favicons_put(f, "one.test", s, 1000);
    au_favicons_put(f, "two.test", s, 2000);
    au_favicons_put(f, "none.test", NULL, 3000);
    CHECK(au_favicons_count(f) == 2 && au_favicons_dirty(f), "two icons, one miss");
    AuBuf out;
    au_buf_init(&out);
    au_favicons_serialize(f, &out);
    CHECK(!au_favicons_dirty(f), "serializing clears dirty");
    CHECK(out.len == 7 + 2 * (1 + 8 + 8 + 1024), "the file holds the icons only: %lu bytes", (unsigned long)out.len);
    AuFavicons *g = au_favicons_new();
    CHECK(au_favicons_parse(g, out.data, out.len) && au_favicons_count(g) == 2, "parsed back");
    const AuSurface *a = au_favicons_for_url(f, "http://two.test/x"), *b = au_favicons_for_url(g, "https://two.test/");
    CHECK(a && b && !memcmp(a->pixels, b->pixels, 16 * 16 * sizeof(AuPixel)), "with the same pixels");
    CHECK(!au_favicons_for_url(g, "http://none.test/"), "misses are not saved");
    /* Damaged files: every truncation fails cleanly and leaves an empty store. */
    for (size_t n = 0; n < out.len; n += 37) {
        bool ok = au_favicons_parse(g, out.data, n);
        CHECK(!ok && au_favicons_count(g) == 0, "truncated at %lu", (unsigned long)n);
    }
    CHECK(!au_favicons_parse(g, "AUFJ\1\0\0", 7), "another magic");
    /* A file that claims pixels brighter than their alpha is clamped. */
    AuBuf bad;
    au_buf_init(&bad);
    au_buf_append(&bad, out.data, out.len);
    memset(bad.data + 7 + 1 + 8 + 8, 0x7f, 4);
    bad.data[7 + 1 + 8 + 8] = 0x10;
    CHECK(au_favicons_parse(g, bad.data, bad.len), "odd pixels parse");
    const AuSurface *c = au_favicons_for_url(g, "http://one.test/");
    CHECK(c && c->pixels[0] == 0x10101010u, "and are clamped to premultiplied: %08lx", c ? (unsigned long)c->pixels[0] : 0ul);
    au_buf_free(&bad);
    /* The store is bounded: the oldest host goes. */
    char host[32];
    for (int i = 0; i < AU_FAVICON_MAX + 10; i++) {
        snprintf(host, sizeof host, "h%d.test", i);
        au_favicons_put(f, host, s, 5000 + i);
    }
    CHECK(au_favicons_count(f) <= AU_FAVICON_MAX, "at most %d hosts: %lu", AU_FAVICON_MAX,
          (unsigned long)au_favicons_count(f));
    CHECK(!au_favicons_for_url(f, "http://one.test/") && au_favicons_for_url(f, "http://h100.test/"),
          "the oldest were dropped");
    au_favicons_clear(f);
    CHECK(au_favicons_count(f) == 0 && au_favicons_dirty(f), "cleared");
    au_buf_free(&out);
    au_surface_free(s);
    au_favicons_free(f);
    au_favicons_free(g);
}

int main(void)
{
    FILE *fp = fopen(AURA_SOURCE_DIR "/tests/image/vectors/bmp/icons.ico", "rb");
    if (!fp) {
        printf("cannot open the test icon\n");
        return 1;
    }
    ico = malloc(1 << 20);
    ico_len = fread(ico, 1, 1 << 20, fp);
    fclose(fp);
    fonts = au_fontset_new();
    test_store();
    test_default_icon();
    test_link_and_sizes();
    test_not_asked_twice();
    test_blocked();
    au_fontset_free(fonts);
    for (int i = 0; i < nkept; i++)
        free(kept[i]);
    free(ico);
    printf("favicon: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
