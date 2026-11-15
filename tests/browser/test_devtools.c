/* The developer tools' models, with no network: the request log filled
 * through the loader and page fetcher on the fake transport (sources,
 * connections, timings, headers, blocked requests, the ring), the time
 * counters and the Performance report, the console entries and the
 * input line, and the Page Source view (lines, colouring, wrapping,
 * selection, find). */
#include "../layout/ahem.h"
#include "fakenet.h"
#include "tls_fixtures.h"

#include "browser/browser.h"
#include "browser/devtools.h"
#include "browser/netlog.h"
#include "browser/sourceview.h"
#include "browser/textview.h"
#include "net/cache.h"
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
    AuHttpCache *cache;
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
    e->roots = au_roots_new(ROOTS_DATE);
    au_roots_add_der(e->roots, tls_fixtures[TLS_FIX_CA].cert.data, tls_fixtures[TLS_FIX_CA].cert.data_len);
    au_net_set_roots(e->net, e->roots);
    e->cache = au_cache_new(1 << 20);
    au_net_set_cache(e->net, e->cache);
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    e->b = au_browser_new(fonts, &h);
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
    au_roots_free(e->roots);
    fake_free(&e->f);
}

static void settle(Env *e, int rounds)
{
    for (int i = 0; i < rounds && au_browser_busy(e->b); i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
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

static void respond(FakeServer *s, const char *type, const char *extra, const char *body)
{
    static char bufs[32][1024];
    static int next;
    char *b = bufs[next++ % 32];
    snprintf(b, 1024, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%sContent-Length: %u\r\n\r\n%s", type, extra,
             (unsigned)strlen(body), body);
    fake_respond(s, b);
}

static const AuNetRecord *rec_for(const AuNetLog *l, const char *url_end)
{
    size_t n = strlen(url_end);
    for (uint32_t i = 0; i < l->count; i++) {
        const AuNetRecord *r = au_netlog_at(l, i);
        size_t m = strlen(r->url);
        if (m >= n && !strcmp(r->url + m - n, url_end))
            return r;
    }
    return NULL;
}

/* ---- The request log ---- */

static void test_netlog_ring(void)
{
    AuNetLog l;
    au_netlog_init(&l);
    CHECK(au_netlog_at(&l, 0) == NULL && l.count == 0, "an empty log");
    uint32_t seqs[350];
    for (int i = 0; i < 350; i++) {
        char url[64];
        snprintf(url, sizeof url, "http://ring.test/%d", i);
        seqs[i] = au_netlog_begin(&l, url, NULL, AU_FETCH_IMAGE, (uint32_t)i);
        au_netlog_data(&l, seqs[i], 10);
    }
    CHECK(l.requests == 350 && l.count == AU_NETLOG_MAX && l.first_seq == 51, "ring: %u kept of %u, first %u",
          (unsigned)l.count, (unsigned)l.requests, (unsigned)l.first_seq);
    CHECK(!strcmp(au_netlog_at(&l, 0)->url, "http://ring.test/50"), "oldest kept: %s", au_netlog_at(&l, 0)->url);
    CHECK(!strcmp(au_netlog_at(&l, 299)->url, "http://ring.test/349"), "newest");
    CHECK(l.decoded_bytes == 3500, "totals count dropped records too");
    /* Ending a record that left the ring changes only the totals. */
    au_netlog_end(&l, seqs[0], AU_NETREC_DONE, NULL, 400);
    au_netlog_end(&l, seqs[349], AU_NETREC_FAILED, NULL, 400);
    CHECK(l.finished == 2 && l.failed == 1 && au_netlog_at(&l, 299)->state == AU_NETREC_FAILED &&
              au_netlog_at(&l, 0)->state == AU_NETREC_PENDING,
          "end by seq");
    AuBuf b;
    au_buf_init(&b);
    au_netlog_dump(&l, "", false, &b);
    CHECK(strstr(au_buf_cstr(&b), "50 earlier requests") != NULL, "the dump says what was dropped");
    au_buf_free(&b);
    uint32_t gen = l.generation;
    au_netlog_clear(&l);
    CHECK(l.count == 0 && l.requests == 0 && l.generation != gen, "clear");
    CHECK(au_netlog_begin(&l, "http://ring.test/again", "POST", AU_FETCH_DOCUMENT, 0) == 1, "seq restarts");
    CHECK(!strcmp(au_netlog_at(&l, 0)->method, "POST"), "method kept");
    au_netlog_free(&l);

    AuBuf s;
    au_buf_init(&s);
    au_put_size(&s, 512);
    au_buf_putc(&s, '|');
    au_put_size(&s, 12595);
    au_buf_putc(&s, '|');
    au_put_size(&s, 1304013);
    au_buf_putc(&s, '|');
    au_put_ms(&s, 412);
    au_buf_putc(&s, '|');
    au_put_ms(&s, 3410);
    CHECK(!strcmp(au_buf_cstr(&s), "512 B|12.2 KB|1.24 MB|412 ms|3.41 s"), "sizes and times: %s", au_buf_cstr(&s));
    au_buf_free(&s);
}

static void test_netlog_page(void)
{
    Env e;
    env_init(&e);
    au_net_set_trace_headers(e.net, true);
    FakeServer *s = fake_server(&e.f, "log.test", 80);
    respond(s, "text/html", "Cache-Control: no-cache\r\nETag: \"p1\"\r\n",
            "<!DOCTYPE html><title>Log</title><link rel=stylesheet href=/s.css><img src=/missing.png><p>logged");
    respond(s, "text/css", "Cache-Control: max-age=300\r\nX-Sheet: yes\r\n", "p { color: green; }");
    fake_respond(s, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\ngone");
    au_bw_new_tab(e.w, "http://log.test/page", true, -1);
    settle(&e, 500);
    paint(e.w);
    const AuNetLog *l = au_page_netlog(page(&e));
    CHECK(l->requests == 3 && l->count == 3 && l->finished == 3, "three requests logged (%u, %u finished)",
          (unsigned)l->requests, (unsigned)l->finished);
    const AuNetRecord *doc = au_netlog_at(l, 0), *css = rec_for(l, "/s.css"), *img = rec_for(l, "/missing.png");
    CHECK(doc && css && img, "records by URL");
    if (doc && css && img) {
        CHECK(doc->seq == 1 && doc->kind == AU_FETCH_DOCUMENT && doc->status == 200 && !strcmp(doc->method, "GET") &&
                  !strcmp(doc->type, "text/html") && doc->state == AU_NETREC_DONE,
              "the document's record: %d %s %s", doc->status, doc->method, doc->type);
        /* The loader adds a byte order mark to the document it hands on. */
        CHECK(doc->transfer > 80 && doc->decoded >= doc->transfer && doc->source == AU_FETCH_FROM_NETWORK,
              "sizes: %u transferred, %u decoded", (unsigned)doc->transfer, (unsigned)doc->decoded);
        CHECK(doc->have_net && doc->connection != 0 && !doc->reused, "a new connection (#%u)",
              (unsigned)doc->connection);
        CHECK(doc->remote[0] != 0 && strstr(doc->remote, ":80") != NULL, "remote address: %s", doc->remote);
        CHECK(doc->protocol[0] == 0, "no TLS on http");
        CHECK(doc->at[AU_FETCH_PHASE_RESOLVING] >= 0 && doc->at[AU_FETCH_PHASE_CONNECTING] >= 0 &&
                  doc->at[AU_FETCH_PHASE_DONE] >= doc->at[AU_FETCH_PHASE_CONNECTING] && doc->total_ms >= 0 &&
                  doc->first_ms >= 0 && doc->first_ms <= doc->total_ms,
              "timings: dns %d, connect %d, done %d, total %d", (int)doc->at[AU_FETCH_PHASE_RESOLVING],
              (int)doc->at[AU_FETCH_PHASE_CONNECTING], (int)doc->at[AU_FETCH_PHASE_DONE], (int)doc->total_ms);
        CHECK(doc->at[AU_FETCH_PHASE_SECURING] < 0, "no TLS phase on http");
        CHECK(doc->request_head && strstr(doc->request_head, "GET /page HTTP/1.1") &&
                  strstr(doc->request_head, "Host: log.test"),
              "request head recorded");
        CHECK(css->response_head && strstr(css->response_head, "X-Sheet: yes"), "response headers recorded: %s",
              css->response_head ? css->response_head : "(none)");
        CHECK(css->kind == AU_FETCH_STYLESHEET && img->kind == AU_FETCH_IMAGE && img->status == 404,
              "kinds and the 404 (%d)", img->status);
        /* Up to six connections open at once: the two resources may use
         * the document's or new ones, and at least one request reused. */
        CHECK(css->connection != 0 && img->connection != 0, "connections named");
        CHECK((int32_t)css->start_ms >= doc->first_ms, "resources start after the document's head");
    }
    CHECK(l->dcl_ms > 0 && l->load_ms >= l->dcl_ms, "DOMContentLoaded %u, load %u", (unsigned)l->dcl_ms,
          (unsigned)l->load_ms);
    AuBuf b, conn;
    au_buf_init(&b);
    au_buf_init(&conn);
    int total, busy;
    au_net_connections_text(e.net, &conn, &total, &busy);
    CHECK(total >= 1 && busy == 0 && strstr(au_buf_cstr(&conn), "log.test:80 x") != NULL, "connections: %s",
          au_buf_cstr(&conn));
    au_netlog_dump(l, au_buf_cstr(&conn), true, &b);
    const char *text = au_buf_cstr(&b);
    CHECK(strstr(text, "Status Meth") && strstr(text, "http://log.test/page") && strstr(text, "404") &&
              strstr(text, "3 requests") && strstr(text, "DOMContentLoaded") && strstr(text, "connections: log.test") &&
              strstr(text, "Request headers:") && strstr(text, "Timings: queued"),
          "the dump:\n%s", text);
    au_buf_free(&b);
    au_buf_free(&conn);

    /* The same page again: the document is revalidated (304), the style
     * sheet comes from the memory cache, and the log starts over. */
    fake_respond(s, "HTTP/1.1 304 Not Modified\r\nETag: \"p1\"\r\n\r\n");
    fake_respond(s, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\ngone");
    au_bw_navigate(e.w, "http://log.test/page");
    settle(&e, 500);
    l = au_page_netlog(page(&e));
    doc = au_netlog_at(l, 0);
    css = rec_for(l, "/s.css");
    CHECK(l->requests == 3 && doc && doc->seq == 1, "a navigation starts the log again (%u)", (unsigned)l->requests);
    CHECK(doc && doc->source == AU_FETCH_FROM_304 && doc->status == 200, "revalidated document: source %d",
          doc ? doc->source : -1);
    CHECK(css && css->source == AU_FETCH_FROM_MEMORY && css->connection == 0 && css->transfer == 0 &&
              css->decoded == 19,
          "style sheet from the memory cache: source %d", css ? css->source : -1);
    CHECK(l->from_cache == 2, "cache hits counted (%u)", (unsigned)l->from_cache);
    b.len = 0;
    au_buf_init(&b);
    if (css)
        au_netlog_row(css, &b);
    CHECK(strstr(au_buf_cstr(&b), "memory") != NULL, "row: %s", au_buf_cstr(&b));
    au_buf_free(&b);

    /* Back to a kept document shows that document's own log again. */
    respond(s, "text/html", "", "<!DOCTYPE html><title>Two</title><p>second");
    au_bw_navigate(e.w, "http://log.test/two");
    settle(&e, 500);
    l = au_page_netlog(page(&e));
    uint32_t gen_two = l->generation;
    CHECK(l->requests == 1 && rec_for(l, "/two"), "the next page has its own log");
    au_bw_back(e.w);
    settle(&e, 500);
    l = au_page_netlog(page(&e));
    doc = au_netlog_at(l, 0);
    CHECK(l->requests == 3 && doc && doc->source == AU_FETCH_FROM_304 && rec_for(l, "/s.css") &&
              l->generation != gen_two,
          "Back to a kept document: its log returns (%u requests)", (unsigned)l->requests);
    au_bw_forward(e.w);
    settle(&e, 500);
    l = au_page_netlog(page(&e));
    CHECK(l->requests == 1 && rec_for(l, "/two"), "Forward: the other document's log");
    /* Without the document cache, Back loads through the HTTP cache's
     * history mode: source history. */
    e.b->prefs.document_cache_pages = 0;
    au_page_drop_kept(page(&e));
    au_bw_back(e.w);
    settle(&e, 500);
    l = au_page_netlog(page(&e));
    doc = au_netlog_at(l, 0);
    CHECK(doc && doc->source == AU_FETCH_FROM_HISTORY, "Back without a kept document: source %d",
          doc ? doc->source : -1);

    /* Without header tracing nothing is copied. */
    au_net_set_trace_headers(e.net, false);
    respond(s, "text/html", "", "<!DOCTYPE html><title>Three</title><p>third");
    au_bw_navigate(e.w, "http://log.test/three");
    settle(&e, 500);
    doc = au_netlog_at(au_page_netlog(page(&e)), 0);
    CHECK(doc && !doc->request_head && !doc->response_head && doc->have_net, "no header copies when tracing is off");
    CHECK(doc && doc->reused, "the connection was kept from the earlier request");
    b.len = 0;
    au_buf_init(&b);
    if (doc)
        au_netlog_detail(doc, &b);
    CHECK(strstr(au_buf_cstr(&b), "Headers: not recorded") != NULL, "detail says so");
    au_buf_free(&b);

    /* A file and a failed host. */
    FakeServer *dead = fake_server(&e.f, "dead.test", 80);
    dead->dns_fail = true;
    au_bw_navigate(e.w, "http://dead.test/");
    settle(&e, 500);
    doc = au_netlog_at(au_page_netlog(page(&e)), 0);
    CHECK(doc && doc->state == AU_NETREC_FAILED && doc->error == AU_FETCH_ERR_DNS, "failed host: state %d error %d",
          doc ? doc->state : -1, doc ? doc->error : -1);
    b.len = 0;
    au_buf_init(&b);
    if (doc) {
        au_netlog_row(doc, &b);
        au_buf_putc(&b, '\n');
        au_netlog_detail(doc, &b);
    }
    CHECK(strstr(au_buf_cstr(&b), "fail") && strstr(au_buf_cstr(&b), "Failed: host not found"), "failure text: %s",
          au_buf_cstr(&b));
    au_buf_free(&b);
    env_free(&e);
}

static void test_netlog_tls_and_blocked(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "tls.test", 443);
    s->tls = true;
    s->chain = &tls_fixtures[TLS_FIX_VALID].cert;
    s->chain_len = 1;
    s->eckey = &tls_fixtures[TLS_FIX_VALID].key;
    respond(s, "text/html", "",
            "<!DOCTYPE html><title>Secure</title><link rel=stylesheet href=\"http://tls.test/plain.css\"><p>secure");
    au_bw_new_tab(e.w, "https://tls.test/", true, -1);
    settle(&e, 2000);
    const AuNetLog *l = au_page_netlog(page(&e));
    const AuNetRecord *doc = au_netlog_at(l, 0), *css = rec_for(l, "/plain.css");
    CHECK(doc && doc->status == 200, "https page loaded (%d, state %d, error %d)", doc ? doc->status : -1,
          doc ? doc->state : -1, doc ? doc->error : -1);
    if (doc) {
        CHECK(!strcmp(doc->protocol, "TLS 1.2") && doc->cipher[0], "TLS version and cipher: %s, %s", doc->protocol,
              doc->cipher);
        CHECK(doc->at[AU_FETCH_PHASE_SECURING] >= 0 && strstr(doc->remote, ":443"), "TLS phase stamped, remote %s",
              doc->remote);
    }
    CHECK(css && css->state == AU_NETREC_BLOCKED && !strcmp(css->blocked_reason, "mixed content"),
          "mixed content is logged as blocked: state %d, reason %s", css ? css->state : -1,
          css ? css->blocked_reason : "");
    CHECK(l->blocked == 1, "blocked counted");
    AuBuf b;
    au_buf_init(&b);
    if (css) {
        au_netlog_row(css, &b);
        au_buf_putc(&b, '\n');
        au_netlog_detail(css, &b);
    }
    CHECK(strstr(au_buf_cstr(&b), "block") && strstr(au_buf_cstr(&b), "Blocked: mixed content"), "blocked text: %s",
          au_buf_cstr(&b));
    au_buf_free(&b);
    /* Another agent's content blocker fills the same field. */
    AuNetLog own;
    au_netlog_init(&own);
    uint32_t q = au_netlog_begin(&own, "http://ads.test/a.js", NULL, AU_FETCH_SCRIPT, 0);
    AuFetchInfo fi;
    AuFetchNet fn;
    memset(&fi, 0, sizeof fi);
    memset(&fn, 0, sizeof fn);
    fn.remote = fn.protocol = fn.cipher = fn.request_head = fn.response_head = "";
    fn.blocked_reason = "content blocker: ||ads.test^";
    for (int i = 0; i <= AU_FETCH_PHASE_DONE; i++)
        fn.at[i] = -1;
    fi.error = AU_FETCH_ERR_BLOCKED;
    fi.net = &fn;
    au_netlog_end(&own, q, AU_NETREC_FAILED, &fi, 1);
    CHECK(au_netlog_at(&own, 0)->state == AU_NETREC_BLOCKED &&
              !strcmp(au_netlog_at(&own, 0)->blocked_reason, "content blocker: ||ads.test^"),
          "a blocker's reason is kept");
    au_netlog_free(&own);
    env_free(&e);
}

/* ---- Time counters and the report ---- */

static void test_perf(void)
{
    AuPerf p;
    au_perf_reset(&p);
    au_perf_add(&p, AU_PERF_LAYOUT, 412, NULL, 1000);
    au_perf_add(&p, AU_PERF_SCRIPT, 180, "https://site.test/app.js", 1200);
    au_perf_add(&p, AU_PERF_PAINT, 3, NULL, 1210);
    au_perf_add(&p, AU_PERF_STYLE, 60, NULL, 1300);
    au_perf_pixels(&p, 480000);
    au_perf_pixels(&p, 999);
    au_perf_pixels(&p, 2);
    CHECK(p.ms[AU_PERF_LAYOUT] == 412 && p.count[AU_PERF_PAINT] == 1 && p.kpixels == 481, "totals (%u kpixels)",
          (unsigned)p.kpixels);
    AuPerfTask slow[8];
    int n = au_perf_slowest(&p, slow, 8);
    CHECK(n == 3 && slow[0].ms == 412 && slow[1].ms == 180 && slow[2].ms == 60 &&
              !strcmp(slow[1].label, "https://site.test/app.js"),
          "slowest first, tasks under %d ms left out (%d)", AU_PERF_SLOW_MS, n);
    n = au_perf_slowest(&p, slow, 2);
    CHECK(n == 2 && slow[1].ms == 180, "a short list keeps the slowest");
    au_perf_sample(&p, 2000);
    au_perf_add(&p, AU_PERF_PAINT, 7, NULL, 2500);
    au_perf_add(&p, AU_PERF_PAINT, 5, NULL, 2600);
    au_perf_sample(&p, 3000);
    CHECK(p.interval_ms == 1000 && p.last_ms[AU_PERF_PAINT] == 12 && p.last_count[AU_PERF_PAINT] == 2 &&
              p.last_ms[AU_PERF_LAYOUT] == 0 && p.ms[AU_PERF_PAINT] == 15,
          "the last interval");
    for (int i = 0; i < 40; i++)
        au_perf_add(&p, AU_PERF_SCRIPT, 50 + (uint32_t)i, NULL, 4000);
    n = au_perf_slowest(&p, slow, 8);
    CHECK(n == 8 && slow[0].ms == 89 && slow[7].ms == 82, "the ring keeps the last %d slow tasks", AU_PERF_TASKS);

    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "perf.test", 80);
    respond(s, "text/html", "",
            "<!DOCTYPE html><title>Perf</title><p id=p>one</p><script>document.getElementById('p').textContent = "
            "'two'; console.log('ran');</script>");
    au_bw_new_tab(e.w, "http://perf.test/", true, -1);
    settle(&e, 500);
    paint(e.w);
    AuPerf *pf = au_page_perf(page(&e));
    CHECK(pf->count[AU_PERF_SCRIPT] >= 1 && pf->count[AU_PERF_STYLE] >= 1 && pf->count[AU_PERF_LAYOUT] >= 1 &&
              pf->count[AU_PERF_PAINT] >= 1 && pf->kpixels > 100,
          "the page counts its passes: script %u, style %u, layout %u, paint %u, %u kpixels",
          (unsigned)pf->count[AU_PERF_SCRIPT], (unsigned)pf->count[AU_PERF_STYLE],
          (unsigned)pf->count[AU_PERF_LAYOUT], (unsigned)pf->count[AU_PERF_PAINT], (unsigned)pf->kpixels);
    size_t used, limit;
    au_page_script_memory(page(&e), &used, &limit);
    CHECK(used > 0 && limit >= used, "script heap: %lu of %lu", (unsigned long)used, (unsigned long)limit);
    AuDevHostInfo h;
    AuBuf conn, out;
    memset(&h, 0, sizeof h);
    au_buf_init(&conn);
    au_buf_init(&out);
    h.machine = "test machine";
    h.have_memory = true;
    h.heap_free_kb = 12345;
    au_net_dev_info(e.net, &h, &conn);
    CHECK(h.have_net && h.connections >= 1 && h.cache_stored >= 1, "network info: %d connections, %lu stored",
          h.connections, h.cache_stored);
    au_dev_perf_report(page(&e), &h, e.f.now, &out);
    e.f.now += 1000;
    paint(e.w);
    au_buf_clear(&out);
    au_dev_perf_report(page(&e), &h, e.f.now, &out);
    const char *t = au_buf_cstr(&out);
    CHECK(strstr(t, "Page: http://perf.test/") && strstr(t, "Machine: test machine") && strstr(t, "(last 1.00 s") &&
              strstr(t, "  script ") && strstr(t, "  screen copy ") && strstr(t, "12345 KB free") &&
              strstr(t, "script heap: ") && strstr(t, "glyph cache: ") && strstr(t, "HTTP cache: ") &&
              strstr(t, "Connections: ") && strstr(t, "perf.test:80") && strstr(t, "Slowest recent tasks") &&
              strstr(t, "1 requests"),
          "the report:\n%s", t);
    AuTextList list;
    au_textlist_init(&list);
    au_dev_report_lines(t, &list);
    bool heading = false;
    for (uint32_t i = 0; i < list.n; i++)
        if (!strcmp(list.text[i], "Memory"))
            heading = list.style[i] == AU_TS_HEADING;
    CHECK(heading, "section titles become headings");
    au_textlist_free(&list);
    au_buf_free(&conn);
    au_buf_free(&out);

    /* A new document starts the counters again. */
    respond(s, "text/html", "", "<!DOCTYPE html><title>Next</title><p>next");
    au_bw_navigate(e.w, "http://perf.test/next");
    settle(&e, 500);
    CHECK(au_page_perf(page(&e))->count[AU_PERF_SCRIPT] == 0, "counters cleared by a navigation");
    env_free(&e);
}

/* ---- The console ---- */

static void test_console(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "con.test", 80);
    respond(s, "text/html", "",
            "<!DOCTYPE html><title>Con</title><p id=p>text</p><script src=/app.js></script>"
            "<script>console.warn('careful'); Promise.reject(new Error('lost'));</script>");
    respond(s, "text/javascript", "", "console.log('hello', 42);\nfunction boom() {\n  throw new Error('bad');\n}\nboom();\n");
    au_bw_new_tab(e.w, "http://con.test/", true, -1);
    settle(&e, 500);
    AuPage *p = page(&e);
    size_t n = au_page_console_count(p);
    AuConsoleEntry en;
    bool log = false, warn = false, uncaught = false, rejection = false;
    for (size_t i = 0; i < n; i++) {
        CHECK(au_dev_console_entry(p, i, &en), "entry %d", (int)i);
        if (en.level == 0 && !strcmp(en.text, "hello 42"))
            log = true;
        if (en.level == 2 && !strcmp(en.text, "careful"))
            warn = true;
        if (en.level == 3 && strstr(en.text, "Uncaught Error: bad")) {
            uncaught = true;
            CHECK(!strcmp(en.source, "http://con.test/app.js") && en.line == 3, "error source: %s line %d", en.source,
                  en.line);
        }
        if (en.level == 3 && strstr(en.text, "in promise") && strstr(en.text, "lost"))
            rejection = true;
    }
    CHECK(log && warn && uncaught && rejection, "log %d, warn %d, uncaught %d, rejection %d of %d lines", log, warn,
          uncaught, rejection, (int)n);
    uint32_t total = au_page_console_total(p);
    CHECK(au_dev_console_eval(p, "1 + 2"), "an expression evaluates");
    CHECK(au_page_console_total(p) == total + 2, "input and result are added");
    n = au_page_console_count(p);
    CHECK(au_dev_console_entry(p, n - 2, &en) && en.level == AU_CONSOLE_INPUT && !strcmp(en.text, "1 + 2"),
          "the input line");
    CHECK(au_dev_console_entry(p, n - 1, &en) && en.level == AU_CONSOLE_RESULT && !strcmp(en.text, "3"),
          "the result: %s", en.text);
    CHECK(au_dev_console_eval(p, "document.getElementById('p').textContent"), "the page's DOM is reachable");
    n = au_page_console_count(p);
    CHECK(au_dev_console_entry(p, n - 1, &en) && !strcmp(en.text, "text"), "DOM result: %s", en.text);
    CHECK(!au_dev_console_eval(p, "nothing.here"), "an error is reported as one");
    n = au_page_console_count(p);
    CHECK(au_dev_console_entry(p, n - 1, &en) && en.level == 3 && strstr(en.text, "nothing"), "error text: %s",
          en.text);
    AuBuf b;
    au_buf_init(&b);
    au_dev_console_text(p, &b);
    const char *t = au_buf_cstr(&b);
    CHECK(strstr(t, "log   hello 42") && strstr(t, "warn  careful") && strstr(t, ">     1 + 2") &&
              strstr(t, "<     3") && strstr(t, "(http://con.test/app.js:3)"),
          "console text:\n%s", t);
    au_buf_free(&b);
    AuTextList list;
    au_textlist_init(&list);
    au_dev_console_lines(p, &list);
    bool styled = false;
    for (uint32_t i = 0; i < list.n; i++)
        if (strstr(list.text[i], "Uncaught Error: bad"))
            styled = list.style[i] == AU_TS_ERROR;
    CHECK(styled && list.n > n, "lines for the view: errors styled, stacks on their own lines (%u)",
          (unsigned)list.n);
    au_textlist_free(&list);
    au_page_console_clear(p);
    CHECK(au_page_console_count(p) == 0 && au_page_console_total(p) != total + 2, "Clear");

    /* A page without any script still runs the input line. */
    respond(s, "text/html", "", "<!DOCTYPE html><title>Plain</title><p>no script");
    au_bw_navigate(e.w, "http://con.test/plain");
    settle(&e, 500);
    p = page(&e);
    CHECK(au_dev_console_eval(p, "document.title"), "input on a page without script");
    n = au_page_console_count(p);
    CHECK(n && au_dev_console_entry(p, n - 1, &en) && !strcmp(en.text, "Plain"), "its result: %s", n ? en.text : "");
    /* With JavaScript off the console says so. */
    e.b->prefs.javascript = false;
    respond(s, "text/html", "", "<!DOCTYPE html><title>Off</title><p>off");
    au_bw_navigate(e.w, "http://con.test/off");
    settle(&e, 500);
    p = page(&e);
    CHECK(!au_dev_console_eval(p, "1"), "no evaluation with JavaScript off");
    n = au_page_console_count(p);
    CHECK(n && au_dev_console_entry(p, n - 1, &en) && strstr(en.text, "turned off"), "it says why");
    env_free(&e);
}

/* ---- Page source and the text view ---- */

static const char *runs_text(const AuSourceView *sv, uint32_t line, AuBuf *b)
{
    size_t len;
    const char *text = au_sourceview_line(sv, line, &len);
    AuSourceRun r[64];
    int n = au_sourceview_runs(sv, line, 0, len, r, 64);
    au_buf_clear(b);
    for (int i = 0; i < n; i++) {
        size_t end = i + 1 < n ? r[i + 1].start : len;
        au_buf_printf(b, "%s[%s:", i ? " " : "", au_source_style_name(r[i].style));
        au_buf_append(b, text + r[i].start, end - r[i].start);
        au_buf_putc(b, ']');
    }
    return au_buf_cstr(b);
}

static void test_source(void)
{
    const char *doc = "<!DOCTYPE html>\r\n"
                      "<!-- a <b>comment</b>\n"
                      "   still -->\n"
                      "<a href=\"x.html\" class=big id=n1>text &amp; more</a>\n"
                      "<script>if (a < b) { x = \"<p>\"; }</script><p>\n"
                      "<style>p > b { }</style>\tend\n"
                      "<input disabled value='it''s'>";
    AuSourceView sv;
    au_sourceview_init(&sv, doc, strlen(doc));
    CHECK(sv.nlines == 7 && sv.nnumbers == 7, "lines: %u segments, %u numbered", (unsigned)sv.nlines,
          (unsigned)sv.nnumbers);
    size_t len;
    const char *l0 = au_sourceview_line(&sv, 0, &len);
    CHECK(len == 15 && !memcmp(l0, "<!DOCTYPE html>", 15), "CR LF is not part of the line (%u)", (unsigned)len);
    AuBuf b;
    au_buf_init(&b);
    CHECK(!strcmp(runs_text(&sv, 0, &b), "[doctype:<!DOCTYPE html>]"), "doctype: %s", au_buf_cstr(&b));
    CHECK(!strcmp(runs_text(&sv, 1, &b), "[comment:<!-- a <b>comment</b>]"), "comment: %s", au_buf_cstr(&b));
    CHECK(!strcmp(runs_text(&sv, 2, &b), "[comment:   still -->]"), "a comment goes on over lines: %s",
          au_buf_cstr(&b));
    CHECK(!strcmp(runs_text(&sv, 3, &b), "[tag:<a ] [attr:href] [tag:=] [string:\"x.html\"] [tag: ] [attr:class] "
                                         "[tag:=] [string:big] [tag: ] [attr:id] [tag:=] [string:n1] [tag:>] "
                                         "[text:text &amp; more] [tag:</a>]"),
          "tag, attributes, values: %s", au_buf_cstr(&b));
    CHECK(!strcmp(runs_text(&sv, 4, &b),
                  "[tag:<script>] [text:if (a < b) { x = \"<p>\"; }] [tag:</script>] [tag:<p>]") ||
              !strcmp(runs_text(&sv, 4, &b), "[tag:<script>] [text:if (a < b) { x = \"<p>\"; }] [tag:</script><p>]"),
          "script text is not markup: %s", au_buf_cstr(&b));
    CHECK(strstr(runs_text(&sv, 5, &b), "[text:p > b { }]") != NULL, "style text: %s", au_buf_cstr(&b));
    CHECK(!strcmp(runs_text(&sv, 6, &b),
                  "[tag:<input ] [attr:disabled] [tag: ] [attr:value] [tag:=] [string:'it'] [attr:'s'] [tag:>]"),
          "attribute without a value, quoted value: %s", au_buf_cstr(&b));
    CHECK(sv.segs[5].cells == 24 + 4 + 3, "a tab moves to the next multiple of 4 cells (%u)",
          (unsigned)sv.segs[5].cells);

    /* The text view over it. */
    AuTextSource src;
    AuTextView tv;
    au_sourceview_source(&sv, &src);
    au_textview_init(&tv);
    tv.wrap = tv.numbers = true;
    au_textview_set_source(&tv, &src);
    au_textview_layout(&tv, fonts, 400, 200);
    CHECK(tv.nlines == 7 && tv.total_rows >= 7 && tv.cols > 10 && tv.row_h > 6, "layout: %u rows, %d columns",
          (unsigned)tv.total_rows, tv.cols);
    au_textview_select_all(&tv);
    au_buf_clear(&b);
    CHECK(au_textview_selection_text(&tv, &b), "select all");
    /* The copy is the document with its line breaks as LF. */
    AuBuf want;
    au_buf_init(&want);
    for (const char *q = doc; *q; q++)
        if (*q != '\r')
            au_buf_putc(&want, *q);
    CHECK(!strcmp(au_buf_cstr(&b), au_buf_cstr(&want)), "copy gives the source back:\n%s", au_buf_cstr(&b));
    au_buf_free(&want);
    au_textview_clear_selection(&tv);
    CHECK(au_textview_find(&tv, "STILL", true) && tv.anchor.line == 2 && tv.anchor.off == 3 && tv.focus.off == 8,
          "find ignores case: line %u, %u..%u", (unsigned)tv.anchor.line, (unsigned)tv.anchor.off,
          (unsigned)tv.focus.off);
    CHECK(au_textview_find(&tv, "p>", true) && tv.anchor.line == 4, "find again goes on from the selection (%u)",
          (unsigned)tv.anchor.line);
    CHECK(au_textview_find(&tv, "p>", true) && tv.anchor.line == 4 && tv.anchor.off > 30, "the next match (%u)",
          (unsigned)tv.anchor.off);
    CHECK(au_textview_find(&tv, "doctype", true) && tv.anchor.line == 0, "find wraps around");
    CHECK(au_textview_find(&tv, "input", false) && tv.anchor.line == 6, "find backward wraps to the end");
    CHECK(!au_textview_find(&tv, "absent text", true) && tv.anchor.line == 6, "no match keeps the selection");
    /* A double click selects a word; a triple click the line. */
    int y3 = (int)tv.first_row[3] * tv.row_h + 2, x = tv.gutter + (int)(tv.cell_w * 4.5f);
    CHECK(au_textview_mouse_down(&tv, x, y3, 2, false), "double click");
    au_textview_mouse_up(&tv);
    au_buf_clear(&b);
    au_textview_selection_text(&tv, &b);
    CHECK(!strcmp(au_buf_cstr(&b), "href"), "word: %s", au_buf_cstr(&b));
    au_textview_mouse_down(&tv, x, y3, 3, false);
    au_textview_mouse_up(&tv);
    au_buf_clear(&b);
    au_textview_selection_text(&tv, &b);
    CHECK(!strncmp(au_buf_cstr(&b), "<a href=", 8) && strstr(au_buf_cstr(&b), "</a>"), "line: %s", au_buf_cstr(&b));
    /* A drag selects characters across lines. */
    au_textview_mouse_down(&tv, tv.gutter + (int)(tv.cell_w * 2), 2, 1, false);
    au_textview_mouse_drag(&tv, tv.gutter + (int)(tv.cell_w * 4), tv.row_h + 2);
    au_textview_mouse_up(&tv);
    au_buf_clear(&b);
    au_textview_selection_text(&tv, &b);
    CHECK(!strcmp(au_buf_cstr(&b), "DOCTYPE html>\n<!--"), "drag: %s", au_buf_cstr(&b));
    AuSurface *surf = au_surface_new(400, 200);
    AuCanvas *c = au_canvas_new(surf);
    au_textview_paint(&tv, c, 0, 0);
    au_canvas_free(c);
    au_surface_free(surf);
    au_textview_free(&tv);
    au_sourceview_free(&sv);

    /* A large document with a very long line: segments, wrapping, and
     * painting touch only what is shown. */
    AuBuf big;
    au_buf_init(&big);
    for (int i = 0; i < 9000; i++)
        au_buf_printf(&big, "<p class=\"row\">line %d \xE2\x82\xAC</p>\n", i);
    size_t before_long = big.len;
    for (int i = 0; i < 20000; i++)
        au_buf_puts(&big, "<i>\xC3\xA9</i>");
    au_buf_putc(&big, '\n');
    au_buf_puts(&big, "<p>last</p>");
    au_sourceview_init(&sv, big.data, big.len);
    CHECK(sv.nnumbers == 9002, "numbered lines: %u", (unsigned)sv.nnumbers);
    CHECK(sv.nlines > 9002 + 50 && sv.nlines < 9002 + 120, "the long line is cut into segments: %u",
          (unsigned)sv.nlines);
    bool bounded = true, utf8 = true;
    for (uint32_t i = 0; i < sv.nlines; i++) {
        size_t n;
        const char *t = au_sourceview_line(&sv, i, &n);
        if (n > AU_SOURCE_SEG_MAX + 4)
            bounded = false;
        if (n && ((unsigned char)t[0] & 0xC0) == 0x80)
            utf8 = false;
    }
    CHECK(bounded && utf8, "segments are bounded and start on a character");
    /* Colouring is right deep inside the long line (the state is kept
     * per segment). */
    uint32_t mid = 9001 + 40;
    CHECK(sv.segs[mid].number == 0 && sv.segs[mid].start > before_long, "a continuation segment");
    const char *rt = runs_text(&sv, mid, &b);
    CHECK(strstr(rt, "[tag:<i>]") || strstr(rt, "[tag:</i>]") || strstr(rt, "[tag:</i><i>]"),
          "colouring inside the long line: %.80s", rt);
    au_sourceview_source(&sv, &src);
    au_textview_init(&tv);
    tv.wrap = tv.numbers = true;
    au_textview_set_source(&tv, &src);
    au_textview_layout(&tv, fonts, 600, 400);
    CHECK(tv.total_rows > sv.nlines, "long segments wrap: %u rows for %u segments", (unsigned)tv.total_rows,
          (unsigned)sv.nlines);
    au_textview_scroll_to(&tv, 0xFFFFFFFFu);
    CHECK(tv.top == au_textview_max_top(&tv) && au_textview_at_end(&tv), "scroll clamps to the end");
    CHECK(au_textview_find(&tv, "<p>last", false) && tv.anchor.line == sv.nlines - 1, "find in the last line");
    au_textview_select_all(&tv);
    au_buf_clear(&b);
    au_textview_selection_text(&tv, &b);
    CHECK(b.len == big.len && !memcmp(b.data, big.data, big.len), "copy of everything equals the source (%lu, %lu)",
          (unsigned long)b.len, (unsigned long)big.len);
    surf = au_surface_new(600, 400);
    c = au_canvas_new(surf);
    au_textview_paint(&tv, c, 0, 0);
    au_textview_scroll_to(&tv, tv.total_rows / 2);
    au_textview_paint(&tv, c, 0, 0);
    au_canvas_free(c);
    au_surface_free(surf);
    /* Narrow and tiny sizes do not break the layout. */
    au_textview_layout(&tv, fonts, 1, 1);
    CHECK(tv.cols >= 4 && tv.visible_rows == 1 && tv.total_rows > 0, "a tiny view");
    au_textview_free(&tv);
    au_sourceview_free(&sv);
    au_buf_free(&big);

    /* Empty and odd documents. */
    au_sourceview_init(&sv, "", 0);
    CHECK(sv.nlines == 1 && sv.nnumbers == 1, "an empty document is one empty line");
    au_sourceview_free(&sv);
    au_sourceview_init(&sv, "a\n", 2);
    CHECK(sv.nlines == 1, "a final line break starts no line (%u)", (unsigned)sv.nlines);
    au_sourceview_free(&sv);
    const char odd[] = "<\xFF\xFE <a b='unterminated\n<!-- never closed <p";
    au_sourceview_init(&sv, odd, sizeof odd - 1);
    for (uint32_t i = 0; i < sv.nlines; i++)
        runs_text(&sv, i, &b);
    CHECK(sv.nlines == 2, "malformed markup still indexes");
    au_sourceview_free(&sv);
    au_buf_free(&b);

    /* A list view: a click selects a row, keys move it. */
    AuTextList list;
    au_textlist_init(&list);
    au_textlist_add_text(&list, AU_TS_PLAIN, "row one\nrow two\r\nrow three");
    CHECK(list.n == 3 && !strcmp(list.text[1], "row two"), "lines split at line breaks");
    au_textlist_source(&list, &src);
    au_textview_init(&tv);
    tv.select_rows = true;
    au_textview_set_source(&tv, &src);
    au_textview_layout(&tv, fonts, 300, 100);
    CHECK(au_textview_mouse_down(&tv, 20, tv.row_h + 1, 1, false) && tv.selected == 1, "click selects row %d",
          (int)tv.selected);
    CHECK(au_textview_key(&tv, AU_KEY_DOWN) && tv.selected == 2 && au_textview_key(&tv, AU_KEY_DOWN) &&
              tv.selected == 2,
          "arrow keys move the row");
    AuBuf row;
    au_buf_init(&row);
    CHECK(au_textview_selection_text(&tv, &row) && !strcmp(au_buf_cstr(&row), "row three"), "the row's text");
    CHECK(au_textview_find(&tv, "ONE", true) && tv.selected == 0, "find selects a row");
    au_textview_mouse_down(&tv, 20, 90, 1, false);
    CHECK(tv.selected == -1, "a click below the rows clears the selection");
    au_buf_free(&row);
    au_textview_free(&tv);
    au_textlist_free(&list);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_netlog_ring();
    test_netlog_page();
    test_netlog_tls_and_blocked();
    test_perf();
    test_console();
    test_source();
    au_fontset_free(fonts);
    printf("devtools: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
