/* The page benchmark (src/browser/bench.h) over the fake transport: the
 * manifest is fetched, each page loads in rounds, a row is recorded per
 * load, the results are posted to the post URL and shown in the tab; a
 * missing manifest fails cleanly. */
#include "fakenet.h"

#include "browser/bench.h"
#include "browser/browser.h"
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

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuCookieJar *jar;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }

static void env_init(Env *e)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->jar = au_cookie_jar_new();
    au_net_set_cookie_jar(e->net, e->jar);
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    e->b = au_browser_new(fonts, &h);
    e->b->prefs.https_first = false;
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
    au_bw_new_tab(e->w, "about:blank", true, -1);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    au_cookie_jar_free(e->jar);
    fake_free(&e->f);
}

static char *kept[64];
static int nkept;

static void respond(FakeServer *s, const char *type, const char *body)
{
    size_t len = strlen(body);
    char *b = malloc(len + 256);
    if (nkept < 64)
        kept[nkept++] = b;
    snprintf(b, len + 256, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n%s", type,
             (unsigned)len, body);
    fake_add(s, b);
}

/* Runs the window until the run is over or max_ms of fake time pass. */
static void run(Env *e, uint32_t max_ms)
{
    for (uint32_t t = 0; t < max_ms && e->w->bench && !au_bench_ok(e->w->bench); t += 10) {
        e->f.now += 10;
        au_bw_idle(e->w);
    }
}

static int count_lines(const char *s, const char *prefix)
{
    int n = 0;
    size_t pl = strlen(prefix);
    for (const char *p = s; p && *p;) {
        if (!strncmp(p, prefix, pl))
            n++;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return n;
}

static void test_run(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "bench.test", 80);
    respond(s, "text/plain", "# two pages\nrepeat 2\nsettle 200\npost out\n\na.html\r\n/b.html\n");
    for (int r = 0; r < 2; r++) {
        respond(s, "text/html", "<!doctype html><title>A</title><p>first page");
        respond(s, "text/html", "<!doctype html><title>B</title><p>second <b>page</b>");
    }
    respond(s, "text/plain", "saved");
    au_bw_navigate_input(e.w, "about:bench?http://bench.test/dir/list.txt");
    CHECK(e.w->bench != NULL, "about:bench starts a run");
    run(&e, 60000);
    AuBench *b = e.w->bench;
    CHECK(au_bench_ok(b), "the run finished and posted its results");
    const char *res = au_bench_results(b);
    CHECK(count_lines(res, "# aura-bench 1") == 1 && count_lines(res, "# build ") == 1, "the header: %.80s", res);
    CHECK(count_lines(res, "round\turl\t") == 1, "a line of column names");
    CHECK(count_lines(res, "1\thttp://bench.test/dir/a.html\tok\t") == 1, "round 1 of a.html, resolved");
    CHECK(count_lines(res, "1\thttp://bench.test/b.html\tok\t") == 1, "round 1 of /b.html");
    CHECK(count_lines(res, "2\thttp://bench.test/dir/a.html\tok\t") == 1, "round 2 of a.html");
    CHECK(count_lines(res, "2\thttp://bench.test/b.html\tok\t") == 1, "round 2 of b.html");
    CHECK(s->nreq == 6, "the manifest, four loads and the post: %d requests", s->nreq);
    CHECK(s->nreq == 6 && !strncmp(s->requests[5], "POST /dir/out ", 14), "posted to the post URL: %.40s",
          s->nreq == 6 ? s->requests[5] : "");
    CHECK(s->nreq == 6 && strstr(s->requests[5], "\r\n\r\n# aura-bench 1\n"), "the body is the results");
    CHECK(s->nreq == 6 && strstr(s->requests[5], "Content-Type: text/tab-separated-values"),
          "sent as tab-separated values");
    AuTab *t = au_bw_active_tab(e.w);
    CHECK(t && !strcmp(au_page_url(t->page), "about:bench"), "the tab shows the results: %s",
          t ? au_page_url(t->page) : "");
    env_free(&e);
}

static void test_no_manifest(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "bench.test", 80);
    fake_add(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    au_bw_navigate_input(e.w, "about:bench?http://bench.test/missing.txt");
    run(&e, 5000);
    CHECK(e.w->bench && !au_bench_ok(e.w->bench) && !au_bench_idle(e.w->bench), "a missing manifest ends the run");
    CHECK(s->nreq == 1, "nothing else is requested: %d", s->nreq);
    /* A new run replaces the old one; closing the window frees it. */
    env_free(&e);
}

int main(void)
{
    fonts = au_fontset_new();
    test_run();
    test_no_manifest();
    au_fontset_free(fonts);
    for (int i = 0; i < nkept; i++)
        free(kept[i]);
    printf("bench: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
