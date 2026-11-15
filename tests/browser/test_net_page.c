/* The browser's fetcher path end to end, with no network: the browser
 * model and page_doc.c over src/net's loader and page fetcher, on the
 * in-process fake transport (tests/net/fakenet.c). Content types,
 * incremental parsing with a first paint before the load ends, redirects
 * updating the address and history once, the download hand-off and real
 * downloads through fake files, security states and the certificate
 * viewer, an exception for a bad certificate, every error page, the
 * entropy wait, status text, and Stop. */
#include "../layout/ahem.h"
#include "fakenet.h"
#include "tls_fixtures.h"

#include "browser/browser.h"
#include "net/cookies.h"
#include "net/hsts.h"
#include "net/loader.h"
#include "net/roots.h"
#include "net/transfer.h"

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

/* ---- A browser on the fake network ---- */

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuCookieJar *jar;
    AuHsts *hsts;
    AuRootStore *roots;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
    AuNetTransferSource *src;
    bool declined;
    int added;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }
static bool t_confirm(void *ctx, const char *url, const char *name, const char *mime)
{
    (void)url, (void)name, (void)mime;
    return !((Env *)ctx)->declined;
}
static void t_added(void *ctx, AuDownload *d)
{
    (void)d;
    ((Env *)ctx)->added++;
}

/* ---- Files for downloads, in memory ---- */

typedef struct MemFile {
    char name[128];
    AuBuf data;
    bool complete, exists;
} MemFile;

static MemFile mem_files[8];

static MemFile *mem_find(const char *name, bool make)
{
    for (int i = 0; i < 8; i++)
        if (mem_files[i].exists && !strcmp(mem_files[i].name, name))
            return &mem_files[i];
    if (!make)
        return NULL;
    for (int i = 0; i < 8; i++)
        if (!mem_files[i].exists) {
            MemFile *m = &mem_files[i];
            memset(m, 0, sizeof *m);
            snprintf(m->name, sizeof m->name, "%s", name);
            au_buf_init(&m->data);
            m->exists = true;
            return m;
        }
    return NULL;
}

static void *mf_open(void *ctx, AuDownload *d, bool restart, int64_t *existing, char **error)
{
    (void)ctx, (void)error;
    MemFile *m = mem_find(d->filename, true);
    if (restart)
        au_buf_clear(&m->data);
    *existing = (int64_t)m->data.len;
    return m;
}
static bool mf_write(void *ctx, void *file, const void *b, size_t n, char **error)
{
    (void)ctx, (void)error;
    au_buf_append(&((MemFile *)file)->data, b, n);
    return true;
}
static bool mf_close(void *ctx, void *file, AuDownload *d, bool complete, char **error)
{
    (void)ctx, (void)d, (void)error;
    if (complete)
        ((MemFile *)file)->complete = true;
    return true;
}
static void mf_remove(void *ctx, AuDownload *d)
{
    (void)ctx;
    MemFile *m = mem_find(d->filename, false);
    if (m) {
        au_buf_free(&m->data);
        m->exists = false;
    }
}

static void env_init(Env *e)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->jar = au_cookie_jar_new();
    e->hsts = au_hsts_new();
    e->roots = au_roots_new(ROOTS_DATE);
    au_roots_add_der(e->roots, tls_fixtures[TLS_FIX_CA].cert.data, tls_fixtures[TLS_FIX_CA].cert.data_len);
    au_net_set_roots(e->net, e->roots);
    au_net_set_cookie_jar(e->net, e->jar);
    au_net_set_hsts(e->net, e->hsts);
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    h.confirm_download = t_confirm;
    h.download_added = t_added;
    e->b = au_browser_new(fonts, &h);
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    au_browser_set_fetcher(e->b, "https", &fe);
    AuDownloadFiles files = {e, mf_open, mf_write, NULL, mf_close, mf_remove};
    e->src = au_net_transfer_source_new(e->l, e->pf, &files);
    AuTransferSource ts;
    au_net_transfer_source_interface(e->src, &ts);
    au_downloads_free(&e->b->downloads);
    au_downloads_init(&e->b->downloads, &ts);
    e->w = au_browser_window_new(e->b, 800, 600);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_transfer_source_free(e->src);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    au_roots_free(e->roots);
    au_hsts_free(e->hsts);
    au_cookie_jar_free(e->jar);
    fake_free(&e->f);
    for (int i = 0; i < 8; i++)
        if (mem_files[i].exists) {
            au_buf_free(&mem_files[i].data);
            mem_files[i].exists = false;
        }
}

static void step(Env *e)
{
    e->f.now += 10;
    au_bw_idle(e->w);
    au_downloads_idle(&e->b->downloads, e->f.now);
}

static void settle(Env *e, int rounds)
{
    for (int i = 0; i < rounds && (au_browser_busy(e->b) || au_downloads_busy(&e->b->downloads)); i++)
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

static AuPage *page(Env *e) { return au_bw_active_tab(e->w)->page; }

static bool page_text_has(Env *e, const char *needle)
{
    au_bw_select_all(e->w);
    AuBuf b;
    au_buf_init(&b);
    au_bw_selection_text(e->w, &b);
    bool has = strstr(au_buf_cstr(&b), needle) != NULL; /* au_buf_cstr: the data is not NUL-terminated */
    au_buf_free(&b);
    au_page_clear_selection(page(e));
    return has;
}

/* Finds the link with this href on the page and clicks it. */
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

/* A 200 text/html response with the right Content-Length. */
static FakeResponse *respond_html(FakeServer *s, const char *body)
{
    static char bufs[16][512];
    static int next;
    char *b = bufs[next++ % 16];
    snprintf(b, 512, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n\r\n%s",
             (unsigned)strlen(body), body);
    return fake_add(s, b);
}

static FakeServer *tls_server(Env *e, const char *host, int fix)
{
    FakeServer *s = fake_server(&e->f, host, 443);
    s->tls = true;
    s->chain = &tls_fixtures[fix].cert;
    s->chain_len = 1;
    s->eckey = &tls_fixtures[fix].key;
    return s;
}

/* ---- Content types, incremental parsing, redirects ---- */

static void test_types_and_redirects(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "site.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 15\r\n\r\n<b>plain</b>!\r\n");
    au_bw_new_tab(e.w, "http://site.test/notes", true, -1);
    settle(&e, 500);
    CHECK(page_text_has(&e, "<b>plain</b>!"), "text/plain shown as text, not HTML");
    CHECK(au_page_security(page(&e)) == AU_SECURITY_NOT_SECURE, "http: not secure");

    /* A large page in small pieces: painted before it has all arrived. */
    AuBuf big;
    au_buf_init(&big);
    au_buf_puts(&big, "<!DOCTYPE html><title>Big</title><body>");
    for (int i = 0; i < 400; i++)
        au_buf_printf(&big, "<p>paragraph %d</p>", i);
    char head[128];
    snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n\r\n",
             (unsigned)big.len);
    AuBuf resp;
    au_buf_init(&resp);
    au_buf_puts(&resp, head);
    au_buf_append(&resp, big.data, big.len);
    FakeResponse *r = fake_add(s, au_buf_cstr(&resp));
    r->chunk = 256;
    r->paced = true;
    au_bw_navigate(e.w, "http://site.test/big");
    bool painted_early = false;
    for (int i = 0; i < 2000 && au_page_loading(page(&e)); i++) {
        step(&e);
        paint(e.w);
        AuPageStats st;
        au_page_stats(page(&e), &st);
        if (st.first_paint_ms && au_page_loading(page(&e)) && !strcmp(au_page_title(page(&e)), "Big"))
            painted_early = true;
        if (i == 3) {
            char status[160];
            au_page_status_text(page(&e), status, sizeof status);
            CHECK(status[0] != 0, "status text while loading");
        }
    }
    CHECK(painted_early, "first paint before the load finished");
    CHECK(page_text_has(&e, "paragraph 399"), "whole page arrived");
    au_buf_free(&big);
    au_buf_free(&resp);

    /* A redirect: the address and the history take the final URL, once. */
    fake_respond(s, "HTTP/1.1 301 Moved\r\nLocation: http://site.test/final\r\nContent-Length: 0\r\n\r\n");
    respond_html(s, "<title>Final</title>arrived");
    size_t hist = e.b->history.n;
    au_bw_navigate(e.w, "http://site.test/start");
    settle(&e, 500);
    AuTab *t = au_bw_active_tab(e.w);
    CHECK(!strcmp(au_page_url(t->page), "http://site.test/final"), "page URL %s", au_page_url(t->page));
    CHECK(!strcmp(au_nav_current(&t->nav)->url, "http://site.test/final"), "history entry follows the redirect");
    CHECK(!strcmp(au_bw_field_text(e.w), "http://site.test/final"), "address field: %s", au_bw_field_text(e.w));
    CHECK(e.b->history.n == hist + 1 && au_history_find(&e.b->history, "http://site.test/start") < 0 &&
              au_history_find(&e.b->history, "http://site.test/final") == 0,
          "one history entry, for the final URL (%zu)", e.b->history.n);
    CHECK(!strcmp(e.b->history.e[0].title, "Final"), "its title");

    /* Fragments: a plain load keeps the one asked for, and so does a
     * redirect whose Location has none. */
    respond_html(s, "<title>Frag</title><p id=sec>here");
    au_bw_navigate(e.w, "http://site.test/frag#sec");
    settle(&e, 500);
    CHECK(!strcmp(au_page_url(page(&e)), "http://site.test/frag#sec"), "fragment kept: %s", au_page_url(page(&e)));
    fake_respond(s, "HTTP/1.1 302 Found\r\nLocation: /moved\r\nContent-Length: 0\r\n\r\n");
    respond_html(s, "<title>Moved</title>moved");
    au_bw_navigate(e.w, "http://site.test/old#top");
    settle(&e, 500);
    CHECK(!strcmp(au_page_url(page(&e)), "http://site.test/moved#top"), "fragment carried over a redirect: %s",
          au_page_url(page(&e)));

    /* An HTTP error with no body. */
    fake_respond(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    au_bw_navigate(e.w, "http://site.test/missing");
    settle(&e, 500);
    CHECK(!strcmp(au_page_title(page(&e)), "404 Not Found"), "404 page: %s", au_page_title(page(&e)));
    /* An HTTP error with a body shows the body. */
    fake_respond(s, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 22\r\n\r\n<title>Own 404</title>");
    au_bw_navigate(e.w, "http://site.test/missing2");
    settle(&e, 500);
    CHECK(!strcmp(au_page_title(page(&e)), "Own 404"), "server's 404 page: %s", au_page_title(page(&e)));

    /* View Source of a network page: the kept bytes, nothing fetched. */
    int before = s->nreq;
    au_bw_view_source(e.w);
    settle(&e, 100);
    CHECK(s->nreq == before && page_text_has(&e, "<title>Own 404</title>"), "view-source from the kept bytes");
    env_free(&e);
}

/* ---- Downloads ---- */

static void test_downloads(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "files.test", 80);
    respond_html(s, "<title>Index</title>");
    au_bw_new_tab(e.w, "http://files.test/", true, -1);
    settle(&e, 500);
    /* A link to an attachment: handed to the downloads, the tab stays. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                    "Content-Disposition: attachment; filename=\"tool.sit\"\r\nContent-Length: 10\r\n\r\n0123456789");
    au_bw_navigate(e.w, "http://files.test/get?id=1");
    settle(&e, 800);
    AuTab *t = au_bw_active_tab(e.w);
    CHECK(e.b->downloads.n == 1 && e.added == 1, "download added: %d", e.b->downloads.n);
    CHECK(!strcmp(au_page_url(t->page), "http://files.test/") && t->nav.n == 1, "tab back on its page: %s",
          au_page_url(t->page));
    CHECK(!strcmp(au_page_title(t->page), "Index"), "title restored");
    CHECK(au_page_security(t->page) == AU_SECURITY_NOT_SECURE, "security indicator restored");
    CHECK(au_history_find(&e.b->history, "http://files.test/get?id=1") < 0, "no history for the file");
    if (e.b->downloads.n == 1) {
        AuDownload *d = e.b->downloads.items[0];
        CHECK(!strcmp(d->filename, "tool.sit"), "name from Content-Disposition: %s", d->filename);
        CHECK(d->state == AU_DL_DONE && d->received == 10, "done: state %d received %lld", d->state,
              (long long)d->received);
        MemFile *m = mem_find("tool.sit", false);
        CHECK(m && m->complete && m->data.len == 10 && !memcmp(m->data.data, "0123456789", 10),
              "file holds the body");
    }
    CHECK(s->nreq == 2, "nothing fetched twice: %d requests", s->nreq);

    /* Save Link As: a fresh transfer; a resumed one asks for the rest. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 6\r\n\r\nUser-a");
    CHECK(au_bw_save_url(e.w, "http://files.test/robots.txt") != NULL, "save link");
    settle(&e, 500);
    MemFile *m = mem_find("robots.txt", false);
    AuDownload *last = e.b->downloads.items[e.b->downloads.n - 1];
    CHECK(m && m->complete && m->data.len == 6, "saved robots.txt: state %d error %s", last->state, last->error);
    /* Resume: a partial file of 4 bytes; the server honours Range. */
    MemFile *part = mem_find("big.bin", true);
    au_buf_puts(&part->data, "abcd");
    fake_respond(s, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 4-7/8\r\nContent-Length: 4\r\n\r\nefgh");
    au_bw_save_url(e.w, "http://files.test/big.bin");
    settle(&e, 500);
    CHECK(strstr(s->requests[s->nreq - 1], "Range: bytes=4-") != NULL, "Range request");
    CHECK(part->complete && part->data.len == 8 && !memcmp(part->data.data, "abcdefgh", 8), "resumed file");
    /* Declined (the shell's warning): nothing added. */
    e.declined = true;
    CHECK(au_bw_save_url(e.w, "http://files.test/app.smi") == NULL, "declined");
    env_free(&e);
}

/* ---- Security states, certificates, the exception ---- */

static void test_security(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = tls_server(&e, "tls.test", TLS_FIX_VALID);
    respond_html(s, "<title>Valid</title>");
    au_bw_new_tab(e.w, "https://tls.test/", true, -1);
    settle(&e, 800);
    CHECK(au_page_security(page(&e)) == AU_SECURITY_SECURE, "valid https: secure (%d)", au_page_security(page(&e)));
    CHECK(au_page_cert(page(&e)) && au_page_cert(page(&e))->count == 1, "certificate kept");
    int ntabs = e.w->ntabs;
    CHECK(au_bw_show_certificate(e.w) && e.w->ntabs == ntabs + 1, "certificate viewer opens");
    CHECK(page_text_has(&e, "Certificate for tls.test") && page_text_has(&e, "TLS 1.2") &&
              page_text_has(&e, "SHA-256"),
          "viewer shows host, protocol and fingerprint");
    au_bw_close_tab(e.w, e.w->active);

    /* Mixed content: an http style sheet in an https page is blocked. */
    respond_html(s, "<link rel=stylesheet href='http://other.test/a.css'>");
    au_bw_navigate(e.w, "https://tls.test/mixed");
    settle(&e, 800);
    CHECK(au_page_security(page(&e)) == AU_SECURITY_MIXED, "mixed: %d", au_page_security(page(&e)));

    /* An expired certificate: the error page, then Continue Anyway. */
    FakeServer *x = tls_server(&e, "expired.test", TLS_FIX_EXPIRED);
    respond_html(x, "<title>Past</title>");
    au_bw_navigate(e.w, "https://expired.test/");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Certificate problem"), "cert error page: %s", au_page_title(page(&e)));
    CHECK(au_page_security(page(&e)) == AU_SECURITY_CERT_ERROR, "cert error state");
    CHECK(page_text_has(&e, "expired") && page_text_has(&e, "SHA-256"), "error page details");
    CHECK(click_link(&e, "aura-action:continue"), "Continue Anyway link");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Past"), "continued: %s", au_page_title(page(&e)));
    CHECK(au_page_security(page(&e)) == AU_SECURITY_CERT_ERROR, "still marked: %d", au_page_security(page(&e)));

    /* An HSTS host offers no way past. */
    FakeServer *h = tls_server(&e, "strict.test", TLS_FIX_WRONGHOST);
    (void)h;
    au_hsts_process(e.hsts, "strict.test", "max-age=1000", e.f.wall);
    au_bw_navigate(e.w, "https://strict.test/");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Certificate problem") && !page_text_has(&e, "Continue Anyway") &&
              page_text_has(&e, "must always be secure"),
          "HSTS: no continue");

    /* A web page cannot use aura-action: links. */
    FakeServer *evil = fake_server(&e.f, "evil.test", 80);
    respond_html(evil, "<a href='aura-action:continue'>xxxxxxxxx</a>");
    au_bw_navigate(e.w, "http://evil.test/");
    settle(&e, 800);
    int nreq = x->nreq;
    click_link(&e, "aura-action:continue");
    settle(&e, 200);
    CHECK(!strcmp(au_page_url(page(&e)), "http://evil.test/") && x->nreq == nreq, "ignored on a web page");

    /* The clock is wrong. */
    e.f.wall = ROOTS_DATE - 86400 * 400;
    au_net_close_idle(e.net);
    tls_server(&e, "untrusted.test", TLS_FIX_UNTRUSTED);
    au_bw_navigate(e.w, "https://untrusted.test/");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Date is wrong") && page_text_has(&e, "Date & Time"),
          "clock page: %s", au_page_title(page(&e)));
    env_free(&e);
}

/* ---- Error pages ---- */

static void expect_title(Env *e, const char *url, const char *title)
{
    au_bw_navigate(e->w, url);
    settle(e, 4000);
    CHECK(!strcmp(au_page_title(page(e)), title), "%s: \"%s\", want \"%s\"", url, au_page_title(page(e)), title);
}

static const char *offline_why(void *ctx)
{
    return *(bool *)ctx ? "Open Transport is not installed." : NULL;
}

static void test_errors(void)
{
    Env e;
    env_init(&e);
    au_bw_new_tab(e.w, "about:blank", true, -1);
    FakeServer *dns = fake_server(&e.f, "nxdomain.test", 80);
    dns->dns_fail = true;
    expect_title(&e, "http://nxdomain.test/", "Server not found");
    CHECK(click_link(&e, "aura-action:retry"), "Try Again link");
    FakeServer *ref = fake_server(&e.f, "refused.test", 80);
    ref->refuse = true;
    expect_title(&e, "http://refused.test/", "Cannot connect");
    au_net_config(e.net)->connect_timeout_ms = 200;
    au_net_config(e.net)->idle_timeout_ms = 200;
    FakeServer *slow = fake_server(&e.f, "silent.test", 80);
    fake_add(slow, "")->silent = true;
    expect_title(&e, "http://silent.test/", "Connection timed out");
    FakeServer *loop = fake_server(&e.f, "loop.test", 80);
    au_net_config(e.net)->max_redirects = 3;
    for (int i = 0; i < 6; i++)
        fake_respond(loop, "HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n");
    expect_title(&e, "http://loop.test/", "Too many redirects");
    FakeServer *bad = fake_server(&e.f, "bad.test", 80);
    fake_respond(bad, "garbage that is not HTTP\r\n\r\n");
    expect_title(&e, "http://bad.test/", "Bad response");
    /* TLS failure: a server that speaks plain text on 443. */
    FakeServer *nt = fake_server(&e.f, "nottls.test", 443);
    nt->banner = "hello, this is not TLS\r\n";
    expect_title(&e, "https://nottls.test/", "Secure connection failed");
    /* No TCP/IP at all. */
    bool offline = true;
    au_net_page_fetcher_set_offline(e.pf, offline_why, &offline);
    expect_title(&e, "http://nxdomain.test/", "No network");
    CHECK(page_text_has(&e, "Open Transport is not installed."), "why");
    offline = false;
    /* HTTPS-first: no https, then the user accepts http. */
    e.b->prefs.https_first = true;
    au_net_config(e.net)->https_first = true;
    FakeServer *plain = fake_server(&e.f, "plain.test", 80);
    respond_html(plain, "<title>Plain</title>");
    expect_title(&e, "http://plain.test/", "HTTPS not available");
    CHECK(click_link(&e, "aura-action:http"), "Continue to the HTTP site link");
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(page(&e)), "Plain") && plain->nreq == 1, "http after consent: %s",
          au_page_title(page(&e)));
    env_free(&e);
}

/* ---- The entropy wait, Stop, two tabs ---- */

static unsigned entropy_bits(void *ctx)
{
    return ((Env *)ctx)->f.entropy_ready ? 128 : 40;
}

static void test_entropy_and_stop(void)
{
    Env e;
    env_init(&e);
    e.f.entropy_ready = false;
    au_net_page_fetcher_set_entropy(e.pf, entropy_bits, &e, 128);
    FakeServer *s = tls_server(&e, "tls.test", TLS_FIX_VALID);
    respond_html(s, "<title>Ready</title>");
    FakeServer *p = fake_server(&e.f, "plain.test", 80);
    respond_html(p, "<title>Quick</title>");
    au_bw_new_tab(e.w, "https://tls.test/", true, -1);
    AuTab *secure = au_bw_active_tab(e.w);
    au_bw_new_tab(e.w, "http://plain.test/", true, -1); /* a second tab at once */
    AuTab *plain = au_bw_active_tab(e.w);
    for (int i = 0; i < 100; i++)
        step(&e);
    CHECK(!strcmp(au_page_title(plain->page), "Quick"), "plain http is not delayed: %s", au_page_title(plain->page));
    CHECK(!strcmp(au_page_title(secure->page), "Preparing a secure connection"), "entropy page: %s",
          au_page_title(secure->page));
    char status[160];
    au_page_status_text(secure->page, status, sizeof status);
    CHECK(strstr(status, "randomness") != NULL, "status: %s", status);
    au_bw_select_tab(e.w, 0);
    CHECK(page_text_has(&e, "40 of 128 bits"), "progress shown");
    e.f.entropy_ready = true;
    settle(&e, 800);
    CHECK(!strcmp(au_page_title(secure->page), "Ready"), "then the page: %s", au_page_title(secure->page));

    /* Stop during a slow load cancels the request. */
    fake_add(p, "")->silent = true;
    au_bw_navigate(e.w, "http://plain.test/slow");
    for (int i = 0; i < 20; i++)
        step(&e);
    CHECK(au_page_loading(page(&e)), "loading");
    au_bw_stop(e.w);
    CHECK(!au_page_loading(page(&e)) && au_net_loader_active(e.l) == 0, "stopped, nothing outstanding");
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_types_and_redirects();
    test_downloads();
    test_security();
    test_errors();
    test_entropy_and_stop();
    au_fontset_free(fonts);
    printf("net_page: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
