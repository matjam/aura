/* Script memory of a page made of several documents and a worker, in a
 * browser window over the fake transport.
 *
 * test_shared_script: a page, two same-origin frames and a worker all
 * load one large script (generated here: 3000 functions inside one outer
 * function, about 830 KB). The page must finish under limits that are
 * well below Automatic, and the peaks of the page's runtime and of the
 * worker must stay under the bounds below, so that a change which makes
 * a compile hold more shows up here.
 *
 * test_worker_out_of_memory: a worker that allocates without end stops
 * with a console line that names the worker, its limit and the
 * preference, the Worker object gets an error event, and the page goes
 * on running.
 *
 * test_page_out_of_memory: the same for the page's own runtime: the
 * line names the page and what was being compiled. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
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

/* The bounds, in KB, on a 64-bit host (pointers are half the size on
 * Mac OS 9, where pointer-heavy memory is smaller). Measured when
 * written: the page's runtime peaked at 15,468 KB and the worker at
 * 4,535 KB; the bounds leave 13% and 21% for compiler and library
 * differences. With each context compiling the script for itself (before
 * the compiled-script cache in src/js/runtime.c) the same page peaked at
 * 20,625 KB and the worker at 10,581 KB, which the limits below refuse.
 * With functions compiled at their first call (QuickJS patch 0021, on
 * 2026-11-18) the page peaked at 10,130 KB and the worker at 2,693 KB,
 * against 14,649 KB and 4,250 KB without; the bounds leave 13% and 21%
 * again, so losing the stubs shows here. */
#define PEAK_PAGE_BOUND_KB 11500
#define PEAK_WORKER_BOUND_KB 3300
#define LIMIT_PAGE_KB 20480
#define LIMIT_WORKER_KB 8192

static AuFontSet *fonts;

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
    FakeServer *a;
} Env;

/* The notices the browser showed (AuBrowserHost.notice). */
static int notices;
static char notice_text[1024];

static void t_notice(void *ctx, AuBrowserWindow *w, const char *message, const char *detail)
{
    (void)ctx;
    (void)w;
    notices++;
    snprintf(notice_text, sizeof notice_text, "%s | %s", message, detail ? detail : "");
}

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }

static void env_init(Env *e, int page_kb, int worker_kb)
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
    h.notice = t_notice;
    notices = 0;
    notice_text[0] = 0;
    e->b = au_browser_new(fonts, &h);
    e->b->prefs.https_first = false;
    e->b->prefs.script_memory_kb = page_kb;
    e->b->prefs.worker_memory_kb = worker_kb;
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    au_browser_set_fetcher(e->b, "https", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
    e->a = fake_server(&e->f, "a.test", 80);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    fake_free(&e->f);
}

static AuPage *page(Env *e) { return au_bw_active_tab(e->w)->page; }

static char *responses[64];
static int nresponses;

static void answer(FakeServer *s, const char *type, const char *body)
{
    size_t cap = strlen(body) + 256;
    char *b = malloc(cap);
    if (nresponses < 64)
        responses[nresponses++] = b;
    snprintf(b, cap, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n%s", type,
             (unsigned)strlen(body), body);
    fake_add(s, b);
}

static const char *js(Env *e, const char *src)
{
    static AuBuf out;
    au_buf_free(&out);
    au_buf_init(&out);
    au_page_eval(page(e), src, &out);
    return au_buf_cstr(&out);
}

/* Runs the event loop until window.R is set (or the rounds run out). */
static void wait_result(Env *e, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
        if (i % 10 == 9 && !strcmp(js(e, "typeof R"), "string"))
            break;
    }
}

static const char *console_find(Env *e, const char *needle)
{
    for (size_t i = 0; i < au_page_console_count(page(e)); i++) {
        const char *line = au_page_console_line(page(e), i, NULL);
        if (strstr(line, needle))
            return line;
    }
    return NULL;
}

static int console_errors(Env *e)
{
    int n = 0;
    for (size_t i = 0; i < au_page_console_count(page(e)); i++) {
        int level = 0;
        const char *line = au_page_console_line(page(e), i, &level);
        if (level == 3) {
            printf("  console error: %s\n", line);
            n++;
        }
    }
    return n;
}

/* A deterministic library: n functions with loops, objects, strings and
 * closures in their bodies, inside one outer function, as a bundler
 * writes them; it defines BIG in the global it is given. */
static char *make_script(int n)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "(function(g){var T=[],shared={hits:0};\n");
    for (int i = 0; i < n; i++) {
        au_buf_printf(&b,
                      "function f%d(a,b){var s=%d,o={id:%d,name:\"fn%d\",tags:[\"t%d\",\"u%d\"]};"
                      "for(var i=0;i<a;i++){s+=(i*b+%d)%%7;if(s>1000){s-=999}}"
                      "var inner=function(x){shared.hits++;return x+o.id+s};"
                      "if(b>a){o.note=\"greater \"+o.name}else{o.note=o.tags.join(\"-\")}"
                      "return inner(o.note.length)}\nT.push(f%d);\n",
                      i, i % 13, i, i, i % 17, i % 19, i % 5, i);
    }
    /* in a worker the same file answers a message with its check */
    au_buf_puts(&b, "g.BIG={count:T.length,check:function(){var s=0;for(var i=0;i<T.length;i+=97)s+=T[i](3,i);return s}};\n"
                    "if(!g.document)g.onmessage=function(){g.postMessage(g.BIG.check())};\n"
                    "})(self);\n");
    return au_buf_take(&b, NULL);
}

static void test_shared_script(void)
{
    Env e;
    env_init(&e, LIMIT_PAGE_KB, LIMIT_WORKER_KB);
    char *script = make_script(3000);
    CHECK(strlen(script) > 800000, "the script is %u bytes", (unsigned)strlen(script));
    /* The server answers in order whatever is asked, so every request
     * after the page's must be for the script: the frames are srcdoc
     * documents (same origin as the page, no request of their own). */
    answer(e.a, "text/html",
           "<!DOCTYPE html><title>t</title><script src='/big.js'></script>"
           "<iframe srcdoc=\"<script src='/big.js'></script><p>frame\"></iframe>"
           "<iframe srcdoc=\"<script src='/big.js'></script><p>frame\"></iframe>"
           "<script>onload=function(){var w=new Worker('/big.js');"
           "w.onerror=function(ev){R='worker error '+ev.message};"
           "w.onmessage=function(ev){R=[BIG.count,BIG.check(),frames[0].BIG.check(),frames[1].BIG.check(),ev.data].join(' ')};"
           "w.postMessage('go')}</script>");
    for (int i = 0; i < 4; i++)
        answer(e.a, "text/javascript", script);
    au_bw_new_tab(e.w, "http://a.test/", true, -1);
    wait_result(&e, 6000);
    const char *r = js(&e, "typeof R=='string'?R:'no result'");
    /* every document and the worker computed the same value */
    int count = 0;
    long v[4] = {0, 1, 2, 3};
    CHECK(sscanf(r, "%d %ld %ld %ld %ld", &count, &v[0], &v[1], &v[2], &v[3]) == 5, "result: %s", r);
    CHECK(count == 3000 && v[0] > 0 && v[0] == v[1] && v[1] == v[2] && v[2] == v[3], "result: %s", r);
    CHECK(e.a->nreq >= 2 && e.a->nreq <= 5, "requests: %d", e.a->nreq);
    CHECK(console_errors(&e) == 0, "the page logged errors");
    CHECK(!console_find(&e, "out of memory"), "out of memory under the limits");
    size_t page_peak = 0, worker_peak = 0, used = 0, limit = 0;
    au_page_script_peaks(page(&e), &page_peak, &worker_peak);
    au_page_script_memory(page(&e), &used, &limit);
    printf("shared script: %u KB of script; page runtime peak %lu KB (limit %lu KB), worker peak %lu KB, in use %lu KB\n",
           (unsigned)(strlen(script) / 1024), (unsigned long)(page_peak / 1024), (unsigned long)(limit / 1024),
           (unsigned long)(worker_peak / 1024), (unsigned long)(used / 1024));
    CHECK(limit == (size_t)LIMIT_PAGE_KB * 1024, "the preference is the limit: %lu", (unsigned long)limit);
    CHECK(page_peak > 0 && page_peak / 1024 < PEAK_PAGE_BOUND_KB, "page runtime peak %lu KB, bound %d KB",
          (unsigned long)(page_peak / 1024), PEAK_PAGE_BOUND_KB);
    CHECK(worker_peak > 0 && worker_peak / 1024 < PEAK_WORKER_BOUND_KB, "worker peak %lu KB, bound %d KB",
          (unsigned long)(worker_peak / 1024), PEAK_WORKER_BOUND_KB);
    /* the workers' memory is in the page's figure, which the budget of
     * kept documents uses */
    CHECK(used > worker_peak / 2 && used > page_peak / 2, "in use %lu KB", (unsigned long)(used / 1024));
    /* the report names each runtime and document */
    AuBuf rep;
    au_buf_init(&rep);
    au_page_script_summary(page(&e), &rep);
    const char *t = au_buf_cstr(&rep);
    CHECK(strstr(t, "script runtime of http://a.test/: ") && strstr(t, "    document http://a.test/: ") &&
              strstr(t, "    frame about:srcdoc: ") && strstr(t, "  worker http://a.test/big.js: ") &&
              strstr(t, ", peak ") && strstr(t, "limit 20480 KB") && strstr(t, "limit 8192 KB"),
          "summary:\n%s", t);
    au_buf_free(&rep);
    free(script);
    env_free(&e);
}

static void test_worker_out_of_memory(void)
{
    Env e;
    env_init(&e, 0, 4096);
    answer(e.a, "text/html",
           "<!DOCTYPE html><title>t</title><script>var ticks=0,err='';setInterval(function(){ticks++},10);"
           "var w=new Worker('/hog.js');w.onerror=function(ev){err=ev.message||'error'};"
           "w.onmessage=function(ev){err='message '+ev.data}</script><p id=p>page");
    answer(e.a, "text/javascript", "var keep=[];for(;;){keep.push(new Array(1000).join('x')+keep.length)}");
    au_bw_new_tab(e.w, "http://a.test/", true, -1);
    for (int i = 0; i < 3000; i++) {
        e.f.now += 10;
        au_bw_idle(e.w);
        if (i % 10 == 9 && strcmp(js(&e, "err"), "") != 0)
            break;
    }
    const char *line = console_find(&e, "out of memory");
    CHECK(line != NULL, "no out-of-memory line in the console");
    for (size_t i = 0; !line && i < au_page_console_count(page(&e)); i++)
        printf("  console: %s\n", au_page_console_line(page(&e), i, NULL));
    if (line) {
        CHECK(strstr(line, "The worker http://a.test/hog.js reached its script memory limit of 4096 KB"), "%s", line);
        CHECK(strstr(line, "KB in use") && strstr(line, "while running script"), "%s", line);
        CHECK(strstr(line, "Script memory per worker") && strstr(line, "content.worker_memory_kb"), "%s", line);
    }
    /* the Worker object heard of it, and the page still runs */
    const char *err = js(&e, "err");
    CHECK(err[0] && strncmp(err, "message", 7) != 0, "worker error event: '%s'", err);
    int before = atoi(js(&e, "ticks"));
    for (int i = 0; i < 50; i++) {
        e.f.now += 10;
        au_bw_idle(e.w);
    }
    int after = atoi(js(&e, "ticks"));
    CHECK(after > before, "page timers stopped: %d then %d", before, after);
    CHECK(!strcmp(js(&e, "document.getElementById('p').textContent+(1+1)"), "page2"), "the page's script runs");
    size_t page_peak = 0, worker_peak = 0;
    au_page_script_peaks(page(&e), &page_peak, &worker_peak);
    CHECK(page_peak < (size_t)4096 * 1024, "the page's runtime was not charged: %lu KB", (unsigned long)(page_peak / 1024));
    /* one notice, which says why and names the worker setting */
    CHECK(notices == 1, "notices: %d", notices);
    CHECK(strstr(notice_text, "ran out of script memory") && strstr(notice_text, "a worker") &&
              strstr(notice_text, "4 MB") && strstr(notice_text, "\"Script memory per worker\" to Custom with 48 MB"),
          "%s", notice_text);
    env_free(&e);
}

static void test_page_out_of_memory(void)
{
    Env e;
    env_init(&e, 4096, 0);
    char *script = make_script(4000);
    answer(e.a, "text/html",
           "<!DOCTYPE html><title>t</title><script>var before=1</script><script src='/big.js'></script>"
           "<script>var after=typeof BIG</script><p id=p>page");
    answer(e.a, "text/javascript", script);
    au_bw_new_tab(e.w, "http://a.test/", true, -1);
    for (int i = 0; i < 300; i++) {
        e.f.now += 10;
        au_bw_idle(e.w);
    }
    const char *line = console_find(&e, "out of memory");
    CHECK(line != NULL, "no out-of-memory line in the console");
    for (size_t i = 0; !line && i < au_page_console_count(page(&e)); i++)
        printf("  console: %s\n", au_page_console_line(page(&e), i, NULL));
    if (line) {
        CHECK(strstr(line, "The page http://a.test/ reached its script memory limit of 4096 KB"), "%s", line);
        CHECK(strstr(line, "while compiling http://a.test/big.js"), "%s", line);
        CHECK(strstr(line, "Script memory per page") && strstr(line, "content.script_memory_kb"), "%s", line);
    }
    /* the scripts before and after it ran, and the document is shown */
    CHECK(!strcmp(js(&e, "before+' '+after+' '+document.getElementById('p').textContent"), "1 undefined page"), "%s",
          js(&e, "before+' '+after"));
    /* one notice for the page, however long it goes on; another after a reload */
    CHECK(notices == 1, "notices: %d", notices);
    CHECK(strstr(notice_text, "\"t\" ran out of script memory") && strstr(notice_text, "This page's scripts") &&
              strstr(notice_text, "\"Script memory per page\" to Custom with 48 MB") &&
              strstr(notice_text, "reload the page"),
          "%s", notice_text);
    free(script);
    env_free(&e);
}

/* The notice's text: what it suggests, and the machine's own shortage. */
static void test_oom_text(void)
{
    CHECK(au_script_oom_suggest_kb(4096) == 48 * 1024, "%u", (unsigned)au_script_oom_suggest_kb(4096));
    CHECK(au_script_oom_suggest_kb(96 * 1024) == 192 * 1024, "%u", (unsigned)au_script_oom_suggest_kb(96 * 1024));
    CHECK(au_script_oom_suggest_kb(192 * 1024) == 256 * 1024, "%u", (unsigned)au_script_oom_suggest_kb(192 * 1024));
    CHECK(au_script_oom_suggest_kb(256 * 1024) == 0, "%u", (unsigned)au_script_oom_suggest_kb(256 * 1024));
    AuScriptOom o = {false, false, 256 * 1024};
    AuBuf m, d;
    au_buf_init(&m);
    au_buf_init(&d);
    au_script_oom_text(&o, "", &m, &d);
    CHECK(strstr(au_buf_cstr(&m), "\"This page\"") && strstr(au_buf_cstr(&d), "already the largest setting"), "%s | %s",
          au_buf_cstr(&m), au_buf_cstr(&d));
    au_buf_free(&m);
    au_buf_free(&d);
    au_buf_init(&m);
    au_buf_init(&d);
    o.machine = true;
    au_script_oom_text(&o, "News", &m, &d);
    CHECK(strstr(au_buf_cstr(&d), "more memory than Aura has left") && strstr(au_buf_cstr(&d), "close other tabs") &&
              !strstr(au_buf_cstr(&d), "Custom"),
          "%s", au_buf_cstr(&d));
    au_buf_free(&m);
    au_buf_free(&d);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_shared_script();
    test_worker_out_of_memory();
    test_page_out_of_memory();
    test_oom_text();
    au_fontset_free(fonts);
    for (int i = 0; i < nresponses; i++)
        free(responses[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
