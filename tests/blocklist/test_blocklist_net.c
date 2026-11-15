/* Content blocking over the in-process fake transport (tests/net):
 *   - the loader refuses a listed host before DNS: no lookup, no
 *     connection, the request ends as blocked with its reason; redirects
 *     to listed hosts are not followed; first-party requests pass;
 *   - in a browser window: listed subresources and script requests fail
 *     and are counted, a navigation to a listed site shows the
 *     blocked-site page, Visit Anyway opens the site for the session, Go
 *     Back returns, and the per-site switch turns blocking off;
 *   - the update manager: nothing starts while a page loads, downloads are
 *     https only, conditional, at most once per interval, and a failed,
 *     empty or implausibly small download keeps the old list. */
#include "../layout/ahem.h"
#include "fakenet.h"
#include "tls_fixtures.h"

#include "browser/browser.h"
#include "browser/context.h"
#include "net/blocklist.h"
#include "net/blocklist_store.h"
#include "net/loader.h"
#include "net/roots.h"

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

#define ROOTS_DATE 1700000000LL

static AuFontSet *fonts;

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuRootStore *roots;
    AuNetLoader *l;
    AuBlocklist *bl;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }

/* Attaches a list compiled from text under id. */
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

static void blocking_changed(void *ctx)
{
    Env *e = ctx;
    const AuPrefs *p = &e->b->prefs;
    au_blocklist_set_enabled(e->bl, p->block_trackers);
    au_blocklist_set_user(e->bl, p->block_allow, p->block_deny, p->block_sites_off);
}

static void env_init(Env *e, bool browser)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->roots = au_roots_new(ROOTS_DATE);
    au_roots_add_der(e->roots, tls_fixtures[TLS_FIX_CA].cert.data, tls_fixtures[TLS_FIX_CA].cert.data_len);
    au_net_set_roots(e->net, e->roots);
    e->l = au_net_loader_new(e->net);
    e->bl = au_blocklist_new();
    au_net_loader_set_blocklist(e->l, e->bl);
    if (!browser)
        return;
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    e->b = au_browser_new(fonts, &h);
    e->b->blocking_changed = blocking_changed;
    e->b->blocking_ctx = e;
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    au_browser_set_fetcher(e->b, "https", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
}

static void env_free(Env *e)
{
    if (e->b)
        au_browser_free(e->b);
    if (e->pf)
        au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_blocklist_free(e->bl);
    au_net_free(e->net);
    au_roots_free(e->roots);
    fake_free(&e->f);
}

static FakeResponse *respond(FakeServer *s, const char *type, const char *body)
{
    static char bufs[32][1024];
    static int next;
    char *b = bufs[next++ % 32];
    snprintf(b, 1024, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n%s", type,
             (unsigned)strlen(body), body);
    return fake_add(s, b);
}

/* ---- The loader ---- */

typedef struct Got {
    bool done;
    AuNetError err;
    char reason[96], url[256];
    AuBuf body;
    int status;
} Got;

static void g_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    (void)id;
    au_buf_append(&((Got *)ctx)->body, bytes, len);
}

static void g_done(void *ctx, uint32_t id, AuNetError err, const AuLoadInfo *info)
{
    Got *g = ctx;
    (void)id;
    g->done = true;
    g->err = err;
    g->status = info->status;
    snprintf(g->reason, sizeof g->reason, "%s", info->blocked_reason ? info->blocked_reason : "");
    snprintf(g->url, sizeof g->url, "%s", info->url ? info->url : "");
}

static void load(Env *e, Got *g, const char *url, AuRequestDest dest, const char *top)
{
    memset(g, 0, sizeof *g);
    au_buf_init(&g->body);
    AuLoadRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.dest = dest;
    rq.document_url = top;
    rq.top_level_url = top;
    rq.allow_http_fallback = true;
    AuLoadCallbacks cb = {g, NULL, g_data, g_done, NULL};
    au_net_loader_start(e->l, &rq, &cb);
    for (int i = 0; i < 400 && !g->done; i++) {
        au_net_loader_poll(e->l);
        e->f.now += 10;
    }
}

static void test_loader(void)
{
    Env e;
    env_init(&e, false);
    use_list(e.bl, "ads", "# Title: Test Ads\nads.test\ntrack.site.test\nlisted.test\n");
    FakeServer *ads = fake_server(&e.f, "cdn.ads.test", 80);
    respond(ads, "text/javascript", "alert(1)");
    FakeServer *site = fake_server(&e.f, "www.site.test", 80);
    FakeServer *track = fake_server(&e.f, "track.site.test", 80);
    FakeServer *hop = fake_server(&e.f, "hop.test", 80);
    FakeServer *listed = fake_server(&e.f, "listed.test", 80);
    const char *page = "http://www.site.test/";
    Got g;

    /* A listed third-party host: no DNS lookup, no connection. */
    uint32_t lookups = e.f.resolve_next;
    load(&e, &g, "http://cdn.ads.test/a.js", AU_DEST_SCRIPT, page);
    CHECK(g.done && g.err == AU_NET_ERR_BLOCKED, "blocked (%s)", au_net_error_name(g.err));
    CHECK(!strcmp(g.reason, "blocked: Test Ads"), "reason: %s", g.reason);
    CHECK(!strcmp(g.url, "http://cdn.ads.test/a.js") && g.status == 0, "the request's URL, no status");
    CHECK(e.f.resolve_next == lookups && ads->connections == 0 && ads->nreq == 0, "no lookup and no connection");
    au_buf_free(&g.body);
    /* Every kind of subresource and script request. */
    static const AuRequestDest dests[] = {AU_DEST_STYLE, AU_DEST_IMAGE, AU_DEST_FONT, AU_DEST_EMPTY, AU_DEST_IFRAME};
    for (size_t i = 0; i < sizeof dests / sizeof dests[0]; i++) {
        load(&e, &g, "http://cdn.ads.test/x", dests[i], page);
        CHECK(g.err == AU_NET_ERR_BLOCKED && g.reason[0], "dest %d blocked", (int)dests[i]);
        au_buf_free(&g.body);
    }
    CHECK(e.f.resolve_next == lookups && ads->connections == 0, "still nothing sent");

    /* An unlisted host loads, and a failure of another kind has no reason. */
    respond(site, "text/css", "p{}");
    load(&e, &g, "http://www.site.test/s.css", AU_DEST_STYLE, page);
    CHECK(g.err == AU_NET_OK && g.body.len == 3 && !g.reason[0], "unlisted loads");
    au_buf_free(&g.body);
    load(&e, &g, "http://nowhere.test/s.css", AU_DEST_STYLE, page);
    CHECK(g.err != AU_NET_OK && g.err != AU_NET_ERR_BLOCKED && !g.reason[0], "a network error is not a block");
    au_buf_free(&g.body);

    /* First party: the page's own listed subdomain is fetched. */
    respond(track, "image/gif", "GIF89a");
    load(&e, &g, "http://track.site.test/p.gif", AU_DEST_IMAGE, page);
    CHECK(g.err == AU_NET_OK && track->connections == 1, "first-party request made");
    au_buf_free(&g.body);
    load(&e, &g, "http://track.site.test/p.gif", AU_DEST_IMAGE, "http://elsewhere.test/");
    CHECK(g.err == AU_NET_ERR_BLOCKED && track->connections == 1, "the same host from another site is blocked");
    au_buf_free(&g.body);

    /* A redirect to a listed host is not followed. */
    fake_add(hop, "HTTP/1.1 302 Found\r\nLocation: http://cdn.ads.test/landing\r\nContent-Length: 0\r\n\r\n");
    load(&e, &g, "http://hop.test/go", AU_DEST_IMAGE, page);
    CHECK(g.err == AU_NET_ERR_BLOCKED && !strcmp(g.reason, "blocked: Test Ads"), "redirect blocked (%s)",
          au_net_error_name(g.err));
    CHECK(!strcmp(g.url, "http://cdn.ads.test/landing"), "names the target: %s", g.url);
    CHECK(hop->nreq == 1 && ads->connections == 0, "the listed host was never contacted");
    au_buf_free(&g.body);

    /* A navigation to a listed site, and one that redirects to it. */
    load(&e, &g, "http://listed.test/", AU_DEST_DOCUMENT, NULL);
    CHECK(g.err == AU_NET_ERR_BLOCKED && !strcmp(g.reason, "blocked: Test Ads") && listed->connections == 0,
          "navigation blocked");
    au_buf_free(&g.body);
    fake_add(hop, "HTTP/1.1 301 Moved\r\nLocation: http://listed.test/in\r\nContent-Length: 0\r\n\r\n");
    load(&e, &g, "http://hop.test/nav", AU_DEST_DOCUMENT, NULL);
    CHECK(g.err == AU_NET_ERR_BLOCKED && !strcmp(g.url, "http://listed.test/in") && listed->connections == 0,
          "navigation redirect blocked: %s", g.url);
    au_buf_free(&g.body);
    /* After Visit Anyway the navigation goes out. */
    au_blocklist_allow_session(e.bl, "listed.test");
    respond(listed, "text/html", "<title>Listed</title>");
    load(&e, &g, "http://listed.test/", AU_DEST_DOCUMENT, NULL);
    CHECK(g.err == AU_NET_OK && listed->connections == 1, "allowed for the session");
    au_buf_free(&g.body);

    /* Blocking off: everything is fetched. */
    au_blocklist_set_enabled(e.bl, false);
    load(&e, &g, "http://cdn.ads.test/a.js", AU_DEST_SCRIPT, page);
    CHECK(g.err == AU_NET_OK && ads->connections == 1, "off: fetched");
    au_buf_free(&g.body);
    AuBlockStats st;
    au_blocklist_stats(e.bl, &st);
    CHECK(st.blocked == 8 && st.navigations == 2, "%u blocked, %u navigations", (unsigned)st.blocked,
          (unsigned)st.navigations);
    env_free(&e);
}

/* ---- The browser ---- */

static void settle(Env *e, int rounds)
{
    for (int i = 0; i < rounds && au_browser_busy(e->b); i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
    }
    for (int i = 0; i < 5; i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
    }
}

static AuPage *page(Env *e) { return au_bw_active_tab(e->w)->page; }

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

static bool click_link(Env *e, const char *href)
{
    AuTab *t = au_bw_active_tab(e->w);
    for (int y = 0; y < 1200; y += 3) {
        for (int x = 0; x < 760; x += 6) {
            AuBuf link;
            au_buf_init(&link);
            au_page_hit_info(t->page, x, y, &link, NULL);
            bool hit = !strcmp(au_buf_cstr(&link), href);
            au_buf_free(&link);
            if (!hit)
                continue;
            AuEvent ev;
            memset(&ev, 0, sizeof ev);
            ev.x = e->w->layout.page.x0 + x - t->scroll_x;
            ev.y = e->w->layout.page.y0 + y - t->scroll_y;
            ev.clicks = 1;
            ev.type = AU_EVENT_MOUSE_DOWN;
            au_bw_mouse(e->w, &ev);
            ev.type = AU_EVENT_MOUSE_UP;
            au_bw_mouse(e->w, &ev);
            return true;
        }
    }
    return false;
}

static uint32_t blocked_on_page(Env *e)
{
    AuPageStats ps;
    au_page_stats(page(e), &ps);
    return ps.blocked;
}

static const char page_html[] =
    "<!DOCTYPE html><title>Site</title><body><p>Story</p>"
    "<script src='http://cdn.ads.test/a.js' onerror=\"document.title += ' script-error'\"></script>"
    "<img src='http://cdn.ads.test/p.gif'>"
    "<link rel=stylesheet href='http://cdn.ads.test/s.css'>"
    "<script>fetch('http://cdn.ads.test/beacon').then(function(){ document.title += ' fetch-ok'; },"
    "function(){ document.title += ' fetch-failed'; });</script>"
    "<img src='http://img.site.test/own.gif'>";

static void test_browser(void)
{
    Env e;
    env_init(&e, true);
    use_list(e.bl, "ads", "# Title: Test Ads\nads.test\nlisted.test\nlisted2.test\n");
    FakeServer *ads = fake_server(&e.f, "cdn.ads.test", 80);
    FakeServer *site = fake_server(&e.f, "www.site.test", 80);
    FakeServer *own = fake_server(&e.f, "img.site.test", 80);
    FakeServer *listed = fake_server(&e.f, "listed.test", 80);
    FakeServer *listed2 = fake_server(&e.f, "listed2.test", 80);
    respond(site, "text/html", page_html);
    respond(own, "image/gif", "GIF89a");
    au_bw_new_tab(e.w, "http://www.site.test/", true, -1);
    settle(&e, 800);
    CHECK(ads->connections == 0 && ads->nreq == 0, "no connection to the listed host (%d)", ads->connections);
    CHECK(own->nreq == 1, "the page's own image was fetched");
    CHECK(blocked_on_page(&e) == 4, "%u requests counted as blocked", (unsigned)blocked_on_page(&e));
    const char *title = au_page_title(page(&e));
    CHECK(strstr(title, "script-error") != NULL, "the script element's onerror ran: %s", title);
    CHECK(strstr(title, "fetch-failed") != NULL && !strstr(title, "fetch-ok"), "fetch() rejected: %s", title);
    CHECK(!au_bw_active_tab(e.w)->loading, "the load finished");
    /* The count reaches the status bar. */
    AuSurface *s = au_surface_new(800, 600);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(e.w, c, (AuIRect){0, 0, 800, 600});
    au_canvas_free(c);
    au_surface_free(s);

    /* A navigation to a listed site: the blocked-site page. */
    au_bw_navigate(e.w, "http://listed.test/welcome");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Site blocked"), "title: %s", au_page_title(page(&e)));
    CHECK(page_text_has(&e, "Aura blocked this site") && page_text_has(&e, "Test Ads") &&
              page_text_has(&e, "listed.test") && page_text_has(&e, "Visit Anyway"),
          "the page names the site and the list");
    CHECK(listed->connections == 0, "nothing was sent to it");
    CHECK(!strcmp(au_page_url(page(&e)), "http://listed.test/welcome"), "the address stays");
    /* Go Back returns to the page before (its kept document: no request). */
    CHECK(click_link(&e, "aura-action:back"), "Go Back link");
    settle(&e, 800);
    CHECK(!strncmp(au_page_url(page(&e)), "http://www.site.test/", 21), "back at the site: %s", au_page_url(page(&e)));
    CHECK(!strncmp(au_page_title(page(&e)), "Site", 4) && blocked_on_page(&e) == 4, "the page as it was left: %s",
          au_page_title(page(&e)));

    /* Visit Anyway: the site loads, now and for the session. */
    au_bw_navigate(e.w, "http://listed.test/welcome");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Site blocked"), "blocked again");
    respond(listed, "text/html", "<title>Listed site</title><p>hello<img src='http://cdn.ads.test/p.gif'>");
    CHECK(click_link(&e, "aura-action:unblock"), "Visit Anyway link");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Listed site") && listed->nreq == 1, "the site loaded: %s",
          au_page_title(page(&e)));
    CHECK(blocked_on_page(&e) == 1 && ads->connections == 0, "its third-party listed resources stay blocked");
    respond(listed, "text/html", "<title>Listed second</title>");
    au_bw_navigate(e.w, "http://listed.test/other");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Listed second"), "no page in the way the second time");
    au_bw_navigate(e.w, "http://listed2.test/");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Site blocked") && listed2->connections == 0, "other sites stay blocked");

    /* A link on a web page to aura-action: does nothing. */
    respond(site, "text/html", "<title>Evil</title><a href='aura-action:unblock'>x</a>");
    au_bw_navigate(e.w, "http://www.site.test/evil");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Evil"), "evil page loaded");
    if (click_link(&e, "aura-action:unblock"))
        settle(&e, 200);
    CHECK(!strcmp(au_page_title(page(&e)), "Evil") && listed2->connections == 0, "a web page cannot unblock");

    /* The per-site switch: off for this site, its listed resources load. */
    respond(site, "text/html", "<title>Switch</title><img src='http://cdn.ads.test/p.gif'>");
    au_bw_navigate(e.w, "http://www.site.test/switch");
    settle(&e, 800);
    CHECK(blocked_on_page(&e) == 1 && ads->connections == 0, "blocked before the switch");
    bool off = true;
    CHECK(au_bw_site_blocking(e.w, &off) && !off, "blocking applies here and is on");
    AuContextMenu m;
    AuIRect pg = e.w->layout.page;
    CHECK(au_bw_context_menu(e.w, pg.x0 + 400, pg.y0 + 400, 0, &m), "context menu");
    bool found = false;
    for (int i = 0; i < m.n; i++)
        if (m.items[i].cmd == AU_CMD_TOGGLE_BLOCKING)
            found = !strcmp(m.items[i].label, "Turn Off Blocking for This Site");
    CHECK(found, "the menu offers the switch");
    respond(site, "text/html", "<title>Switch</title><img src='http://cdn.ads.test/p.gif'>");
    respond(ads, "image/gif", "GIF89a");
    uint32_t gen = e.b->prefs_gen;
    CHECK(au_bw_context_command(e.w, &m, AU_CMD_TOGGLE_BLOCKING), "the model runs it");
    au_context_menu_free(&m);
    settle(&e, 800);
    CHECK(!strcmp(e.b->prefs.block_sites_off, "site.test") && e.b->prefs_gen != gen, "stored by site: %s",
          e.b->prefs.block_sites_off);
    CHECK(ads->nreq == 1 && blocked_on_page(&e) == 0, "the page reloaded with its listed image");
    CHECK(au_bw_site_blocking(e.w, &off) && off, "reported as off");
    au_bw_toggle_site_blocking(e.w);
    respond(site, "text/html", "<title>Switch</title><img src='http://cdn.ads.test/p.gif'>");
    settle(&e, 800);
    CHECK(!e.b->prefs.block_sites_off[0] && ads->nreq == 1 && blocked_on_page(&e) == 1, "and on again");
    /* No switch on Aura's own pages. */
    au_bw_navigate(e.w, "about:blank");
    settle(&e, 100);
    CHECK(!au_bw_site_blocking(e.w, &off), "no switch for about:blank");
    env_free(&e);
}

/* ---- The update manager ---- */

typedef struct MemStore {
    struct {
        char name[40];
        AuBuf data;
        bool used;
    } files[8];
    int writes, reads;
    bool fail_writes;
} MemStore;

static bool ms_read(void *ctx, const char *name, const AuBlockMem *mem, uint8_t **data, size_t *len)
{
    MemStore *s = ctx;
    (void)mem;
    s->reads++;
    for (int i = 0; i < 8; i++)
        if (s->files[i].used && !strcmp(s->files[i].name, name)) {
            *len = s->files[i].data.len;
            *data = au_alloc(*len ? *len : 1);
            memcpy(*data, s->files[i].data.data, *len);
            return true;
        }
    return false;
}

static bool ms_write(void *ctx, const char *name, const void *data, size_t len)
{
    MemStore *s = ctx;
    if (s->fail_writes)
        return false;
    s->writes++;
    int at = -1;
    for (int i = 0; i < 8; i++)
        if (s->files[i].used && !strcmp(s->files[i].name, name))
            at = i;
    for (int i = 0; i < 8 && at < 0; i++)
        if (!s->files[i].used) {
            at = i;
            s->files[i].used = true;
            snprintf(s->files[i].name, sizeof s->files[i].name, "%s", name);
            au_buf_init(&s->files[i].data);
        }
    if (at < 0)
        return false;
    au_buf_clear(&s->files[at].data);
    au_buf_append(&s->files[at].data, data, len);
    return true;
}

static void ms_remove(void *ctx, const char *name)
{
    MemStore *s = ctx;
    for (int i = 0; i < 8; i++)
        if (s->files[i].used && !strcmp(s->files[i].name, name)) {
            au_buf_free(&s->files[i].data);
            s->files[i].used = false;
        }
}

static AuBuf *ms_file(MemStore *s, const char *name)
{
    for (int i = 0; i < 8; i++)
        if (s->files[i].used && !strcmp(s->files[i].name, name))
            return &s->files[i].data;
    return NULL;
}

static void ms_free(MemStore *s)
{
    for (int i = 0; i < 8; i++)
        if (s->files[i].used)
            au_buf_free(&s->files[i].data);
}

/* Runs the manager until it has nothing in progress. */
static void run(Env *e, AuBlockManager *m, bool busy)
{
    for (int i = 0; i < 3000; i++) {
        bool working = au_block_manager_idle(m, busy);
        au_net_pump(e->net);
        e->f.now += 10;
        if (!working && !au_net_active(e->net))
            break;
    }
    au_block_manager_idle(m, busy);
}

static FakeServer *tls_server(Env *e, const char *host)
{
    FakeServer *s = fake_server(&e->f, host, 443);
    s->tls = true;
    s->chain = &tls_fixtures[TLS_FIX_VALID].cert;
    s->chain_len = 1;
    s->key = &tls_fixtures[TLS_FIX_VALID].key;
    return s;
}

/* A list of n names "hN.PREFIX.test". */
static const char *list_response(const char *prefix, int n, const char *etag)
{
    static char bufs[8][8192];
    static int next;
    char body[6000];
    size_t k = (size_t)snprintf(body, sizeof body, "# Title: Custom %s\n", prefix);
    for (int i = 0; i < n; i++)
        k += (size_t)snprintf(body + k, sizeof body - k, "h%d.%s.test\n", i, prefix);
    char *b = bufs[next++ % 8];
    snprintf(b, 8192, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nETag: %s\r\nLast-Modified: Sun, 15 Nov 2026 "
                      "10:00:00 GMT\r\nContent-Length: %u\r\n\r\n%s",
             etag, (unsigned)k, body);
    return b;
}

static bool is_listed(Env *e, const char *host) { return au_blocklist_listed(e->bl, host, NULL); }

static bool find_info(AuBlockManager *m, const char *url, AuBlockListInfo *out)
{
    for (size_t i = 0; au_block_manager_info(m, i, out); i++)
        if (!strcmp(out->url, url))
            return true;
    return false;
}

static void test_updates(void)
{
    Env e;
    env_init(&e, false);
    MemStore store;
    memset(&store, 0, sizeof store);
    AuBlockStore st = {&store, ms_read, ms_write, ms_remove};
    FakeServer *srv = tls_server(&e, "tls.test");
    const char *url = "https://tls.test/list.txt";
    AuPrefs prefs;
    au_prefs_init(&prefs);
    au_prefs_set_str(&prefs.block_lists, ""); /* no catalogue list: the test stays off the network */
    au_prefs_set_str(&prefs.block_custom, "https://tls.test/list.txt http://plain.test/list.txt ftp://x/y");
    AuBlockManager *m = au_block_manager_new(e.net, e.bl, &st, NULL);
    au_block_manager_apply_prefs(m, &prefs);
    size_t ncat;
    au_block_catalog(&ncat);
    CHECK(au_block_manager_count(m) == ncat + 1, "only the https address became a list (%zu)",
          au_block_manager_count(m));
    AuBlockListInfo li;
    CHECK(find_info(m, url, &li) && li.enabled && !li.loaded && li.state == AU_BLOCK_WAITING, "waiting for a download");

    /* Nothing starts while a page loads. */
    fake_add(srv, list_response("one", 40, "\"v1\""));
    run(&e, m, true);
    CHECK(srv->connections == 0 && au_net_active(e.net) == 0, "no download while busy");
    /* At idle: the list is fetched, compiled, stored and in use. */
    run(&e, m, false);
    CHECK(srv->nreq == 1, "one request (%d)", srv->nreq);
    CHECK(srv->nreq == 1 && !strstr(srv->requests[0], "If-None-Match") && !strstr(srv->requests[0], "Cookie:"),
          "unconditional, without cookies");
    CHECK(is_listed(&e, "h7.one.test") && is_listed(&e, "www.h39.one.test") && !is_listed(&e, "h40.one.test"),
          "the list is in use");
    CHECK(find_info(m, url, &li) && li.loaded && li.entries == 40 && li.state == AU_BLOCK_IDLE && !li.error[0] &&
              li.updated_at == e.f.wall && li.checked_at == e.f.wall,
          "info: %u entries, error \"%s\"", (unsigned)li.entries, li.error);
    CHECK(!strcmp(li.title, "Custom one"), "the list's own title: %s", li.title);
    AuBuf *file = ms_file(&store, li.id);
    CHECK(file && au_block_file_check((const uint8_t *)file->data, file->len, NULL), "the compiled file was stored");
    CHECK(ms_file(&store, "Index") != NULL, "and the index");
    AuBlockCompileStats cs;
    au_block_manager_compile_stats(m, &cs);
    CHECK(cs.entries == 40 && cs.file_bytes == AU_BLOCK_HEADER + 40 * 6, "compile stats");

    /* Not asked again within the interval. */
    e.f.wall += 3600;
    run(&e, m, false);
    CHECK(srv->nreq == 1, "no second request the same day");
    /* A day later: a conditional request; 304 keeps the list. */
    e.f.wall += 86400;
    fake_add(srv, "HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\n\r\n");
    run(&e, m, false);
    CHECK(srv->nreq == 2 && strstr(srv->requests[1], "If-None-Match: \"v1\"") &&
              strstr(srv->requests[1], "If-Modified-Since: Sun, 15 Nov 2026 10:00:00 GMT"),
          "conditional request");
    CHECK(find_info(m, url, &li) && li.checked_at == e.f.wall && li.updated_at == e.f.wall - 86400 - 3600 &&
              is_listed(&e, "h7.one.test"),
          "304: checked now, list unchanged");

    /* Failures keep the old list: a server error... */
    e.f.wall += 86400;
    fake_add(srv, "HTTP/1.1 500 Oops\r\nContent-Length: 0\r\n\r\n");
    run(&e, m, false);
    CHECK(srv->nreq == 3 && is_listed(&e, "h7.one.test"), "500: the old list stays");
    CHECK(find_info(m, url, &li) && !strcmp(li.error, "HTTP 500") && li.loaded, "error shown: %s", li.error);
    run(&e, m, false);
    CHECK(srv->nreq == 3, "not retried at once");
    /* ...a page that is not a list... */
    e.f.wall += 7 * 3600;
    fake_add(srv, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 46\r\n\r\n"
                  "<html><body><h1>Service unavailable</h1></html>");
    run(&e, m, false);
    CHECK(srv->nreq == 4 && is_listed(&e, "h7.one.test"), "an error page: the old list stays");
    CHECK(find_info(m, url, &li) && strstr(li.error, "no domains"), "error: %s", li.error);
    /* ...a list that shrank to almost nothing... */
    e.f.wall += 7 * 3600;
    fake_add(srv, list_response("tiny", 3, "\"v2\""));
    run(&e, m, false);
    CHECK(srv->nreq == 5 && is_listed(&e, "h7.one.test") && !is_listed(&e, "h1.tiny.test"),
          "implausibly small: the old list stays");
    CHECK(find_info(m, url, &li) && strstr(li.error, "implausibly small") && li.entries == 40, "error: %s", li.error);
    /* ...a connection that breaks half way... */
    e.f.wall += 7 * 3600;
    FakeResponse *cut = fake_add(srv, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5000\r\n\r\n"
                                      "h1.cut.test\nh2.cut.test\n");
    cut->close_after = true;
    run(&e, m, false);
    CHECK(srv->nreq == 6 && is_listed(&e, "h7.one.test") && !is_listed(&e, "h1.cut.test"),
          "a broken download: the old list stays");
    CHECK(find_info(m, url, &li) && li.error[0] && li.loaded, "error: %s", li.error);
    /* ...and a redirect off https. */
    e.f.wall += 7 * 3600;
    fake_add(srv, "HTTP/1.1 302 Found\r\nLocation: http://plain.test/list.txt\r\nContent-Length: 0\r\n\r\n");
    FakeServer *plain = fake_server(&e.f, "plain.test", 80);
    respond(plain, "text/plain", "h1.plain.test\n");
    run(&e, m, false);
    CHECK(srv->nreq == 7 && plain->connections == 0 && is_listed(&e, "h7.one.test"), "no download over plain http");
    AuBuf *kept = ms_file(&store, li.id);
    CHECK(kept && kept->len == AU_BLOCK_HEADER + 40 * 6, "the stored file is still the first");

    /* A good update replaces the list. Update Now does not wait. */
    fake_add(srv, list_response("two", 50, "\"v3\""));
    au_block_manager_update_now(m);
    run(&e, m, false);
    CHECK(srv->nreq == 8 && strstr(srv->requests[7], "If-None-Match: \"v1\""), "Update Now asks at once");
    CHECK(is_listed(&e, "h7.two.test") && !is_listed(&e, "h7.one.test"), "the new list replaced the old");
    CHECK(find_info(m, url, &li) && li.entries == 50 && !li.error[0] && li.updated_at == e.f.wall, "info updated");

    /* A store that cannot be written: the list is used anyway. */
    store.fail_writes = true;
    fake_add(srv, list_response("three", 60, "\"v4\""));
    au_block_manager_update_now(m);
    run(&e, m, false);
    CHECK(is_listed(&e, "h7.three.test") && find_info(m, url, &li) && strstr(li.error, "could not be saved"),
          "unsaved but in use: %s", li.error);
    store.fail_writes = false;
    au_block_manager_free(m);

    /* The next launch: the stored list (version two) is attached without
     * a request, with its validators. */
    au_blocklist_detach(e.bl, li.id);
    CHECK(!is_listed(&e, "h7.three.test"), "detached");
    m = au_block_manager_new(e.net, e.bl, &st, NULL);
    au_block_manager_apply_prefs(m, &prefs);
    CHECK(is_listed(&e, "h7.two.test") && srv->nreq == 9, "loaded from the store at start, no request");
    run(&e, m, false);
    CHECK(srv->nreq == 9, "and none at idle within the interval");
    CHECK(find_info(m, url, &li) && li.loaded && li.entries == 50 && li.updated_at > 0, "times from the index");
    /* Blocking off detaches the lists; on attaches them again. */
    prefs.block_trackers = false;
    au_block_manager_apply_prefs(m, &prefs);
    AuBlockStats bs;
    au_blocklist_stats(e.bl, &bs);
    CHECK(bs.lists == 0 && !au_blocklist_enabled(e.bl), "off: no list in memory");
    prefs.block_trackers = true;
    au_block_manager_apply_prefs(m, &prefs);
    CHECK(is_listed(&e, "h7.two.test"), "on again from the store");
    /* With updates off the publisher is not asked, however long. */
    prefs.block_update = AU_BLOCK_UPDATE_NEVER;
    au_block_manager_apply_prefs(m, &prefs);
    e.f.wall += 40 * 86400;
    run(&e, m, false);
    CHECK(srv->nreq == 9, "never: no request");
    prefs.block_update = AU_BLOCK_UPDATE_WEEKLY;
    au_block_manager_apply_prefs(m, &prefs);
    fake_add(srv, "HTTP/1.1 304 Not Modified\r\n\r\n");
    run(&e, m, false);
    CHECK(srv->nreq == 10 && strstr(srv->requests[9], "If-None-Match: \"v3\""), "weekly: asked after 40 days");
    au_block_manager_free(m);

    /* A damaged stored file is refused at start and fetched again. */
    AuBuf *f2 = ms_file(&store, li.id);
    f2->data[AU_BLOCK_HEADER + 3] ^= 0x10;
    au_blocklist_detach(e.bl, li.id);
    m = au_block_manager_new(e.net, e.bl, &st, NULL);
    au_block_manager_apply_prefs(m, &prefs);
    CHECK(!is_listed(&e, "h7.two.test") && find_info(m, url, &li) && !li.loaded, "damaged file not used");
    fake_add(srv, list_response("four", 20, "\"v5\""));
    run(&e, m, false);
    CHECK(srv->nreq == 11 && !strstr(srv->requests[10], "If-None-Match") && is_listed(&e, "h7.four.test"),
          "fetched again, unconditionally");
    /* Removing the address removes the list and its file. */
    char id[32];
    snprintf(id, sizeof id, "%s", li.id);
    au_prefs_set_str(&prefs.block_custom, "");
    au_block_manager_apply_prefs(m, &prefs);
    CHECK(!is_listed(&e, "h7.four.test") && !ms_file(&store, id) && au_block_manager_count(m) == ncat, "removed");
    au_block_manager_free(m);

    /* A catalogue list whose publisher cannot be reached: blocking stays
     * on with nothing listed, and says why. */
    au_prefs_set_str(&prefs.block_lists, "light");
    m = au_block_manager_new(e.net, e.bl, &st, NULL);
    au_block_manager_apply_prefs(m, &prefs);
    run(&e, m, false);
    au_block_manager_info(m, 0, &li);
    CHECK(!strcmp(li.id, "light") && li.enabled && !li.loaded && li.error[0], "light not loaded: %s", li.error);
    CHECK(au_blocklist_enabled(e.bl) && !is_listed(&e, "h7.four.test"), "nothing blocked, nothing crashed");
    au_block_manager_free(m);
    au_prefs_free(&prefs);
    ms_free(&store);
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_loader();
    test_browser();
    test_updates();
    au_fontset_free(fonts);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
