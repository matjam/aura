/* Script that parks (src/browser/page_doc.c): with AuPageEnv.script_coro
 * a long task leaves its coroutine every 100 ms of script, and the window
 * scrolls and paints meanwhile. Covers scrolling and painting while
 * parked, the script seeing the scroll afterwards, mouse moves dropped,
 * a click waiting for the script, Stop and navigation ending a parked
 * script, a dialog running on the main thread, and network deliveries
 * held until the slice ends. Runs on the POSIX coroutines, as the host
 * tool does. */
#include "../layout/ahem.h"
#include "browser/browser.h"
#include "platform/posix/posix.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define CHECK_STR(a, b) \
    do { \
        const char *a_ = (a), *b_ = (b); \
        if (strcmp(a_, b_) != 0) { \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); \
            exit(1); \
        } \
    } while (0)

static AuFontSet *fonts;

/* Every read of the clock moves it 1 ms, so a script that waits for
 * performance.now() to move on gets there. */
static uint32_t clock_ms = 1000;
static uint32_t t_ms(void *ctx)
{
    (void)ctx;
    return clock_ms++;
}
static AuTime t_now(void *ctx)
{
    (void)ctx;
    return 0;
}

/* A loop of about ms of script time (on the clock above). */
#define BUSY(ms) "var t0=performance.now();while(performance.now()-t0<" #ms "){for(var i=0;i<200;i++);}"

static const struct {
    const char *name, *text;
} files[] = {
    {"scroll.html",
     "<!DOCTYPE html><body style='margin:0'><p id=p>before</p><div style='height:5000px'></div><script>"
     "setTimeout(function(){document.getElementById('p').textContent='during';" BUSY(2000)
     "document.title='done '+scrollY;},10);</script>"},
    {"click.html",
     "<!DOCTYPE html><body style='margin:0'><div id=d style='height:5000px'>x</div><script>"
     "var clicks=0;document.getElementById('d').onmousedown=function(){clicks++;"
     "document.title='click after '+(window.done?'end':'middle');};"
     "setTimeout(function(){" BUSY(1500) "window.done=1;},10);</script>"},
    {"stop.html",
     "<!DOCTYPE html><body><script>setTimeout(function(){" BUSY(100000) "document.title='ran to the end';},10);"
     "</script>"},
    {"other.html", "<!DOCTYPE html><title>Other</title><body>o"},
    {"locked.html",
     "<!DOCTYPE html><html style='overflow:hidden'><body style='margin:0'><div style='height:5000px'></div>"
     "<script>window.go=function(){scrollTo(0,500);document.title='at '+scrollY;};</script>"},
    {"grow.html",
     "<!DOCTYPE html><body style='margin:0'><p>short</p><script>"
     "setTimeout(function(){var d=document.createElement('div');d.style.height='5000px';"
     "document.body.appendChild(d);" BUSY(4000) "document.title='grown';},10);</script>"},
    /* positioned boxes are in the root's stacking lists; a new display
     * rebuilds their subtrees when getComputedStyle restyles */
    {"restyle.html",
     "<!DOCTYPE html><body style='margin:0'><div style='height:5000px'>"
     "<div id=a style='position:relative;z-index:2'>a<span>x</span></div>"
     "<div id=b style='position:absolute;top:50px;z-index:-1'>b</div>"
     "<div id=c style='position:relative;opacity:.5'>c</div></div><script>"
     "setTimeout(function(){['a','b','c'].forEach(function(id){var e=document.getElementById(id);"
     "e.style.display='flex';e.style.zIndex='3';});"
     "document.title=getComputedStyle(document.getElementById('a')).display;" BUSY(1500)
     "document.title+=' done';},10);</script>"},
};

static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    if (strncmp(url, "file:///mem/", 12))
        return false;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (!strcmp(url + 12, files[i].name)) {
            size_t n = strlen(files[i].text);
            *data = au_alloc(n + 1);
            memcpy(*data, files[i].text, n + 1);
            *len = n;
            return true;
        }
    }
    return false;
}

static pthread_t main_thread;
static int confirms;
static bool t_confirm(void *ctx, AuBrowserWindow *w, const char *message)
{
    (void)ctx;
    (void)w;
    CHECK(pthread_equal(pthread_self(), main_thread)); /* the shell's dialog runs on its own thread */
    CHECK(strstr(message, "insecure") != NULL);
    confirms++;
    return false; /* not sent */
}

/* ---- A fetcher whose answers come from polls ---- */

typedef struct NetReq {
    const AuFetchSink *sink;
    char url[256];
    int step;
} NetReq;

static NetReq reqs[8];
static uint32_t nreqs;

static const char *net_body(const char *url)
{
    if (strstr(url, "/data.txt"))
        return "payload";
    if (strstr(url, "/form"))
        /* an https page that submits to http: the browser asks first */
        return "<!DOCTYPE html><body><form id=f action='http://example.net/sent'><input name=a value=1></form>"
               "<script>setTimeout(function(){" BUSY(500) "document.getElementById('f').submit();" BUSY(500)
               "document.title='asked';},10);</script>";
    return "<!DOCTYPE html><body style='margin:0'><div style='height:5000px'></div><script>"
           "setTimeout(function(){fetch('/data.txt').then(function(r){return r.text();}).then(function(t){"
           "document.title=(window.done?'after ':'during ')+t;}).catch(function(e){document.title='err '+e;});"
           BUSY(1500) "window.done=1;},10);</script>";
}

static uint32_t net_start(void *ctx, const AuFetchRequest *rq, const AuFetchSink *sink)
{
    (void)ctx;
    CHECK(nreqs < 8);
    NetReq *r = &reqs[nreqs++];
    r->sink = sink;
    snprintf(r->url, sizeof r->url, "%s", rq->url);
    r->step = 0;
    return nreqs;
}

static void net_cancel(void *ctx, uint32_t id)
{
    (void)ctx;
    if (id >= 1 && id <= nreqs)
        reqs[id - 1].sink = NULL;
}

static void net_poll(void *ctx)
{
    (void)ctx;
    for (uint32_t i = 0; i < nreqs; i++) {
        NetReq *r = &reqs[i];
        if (!r->sink)
            continue;
        const AuFetchSink *s = r->sink;
        const char *body = net_body(r->url);
        bool text = strstr(r->url, ".txt") != NULL;
        if (r->step == 0 && s->response) {
            AuFetchResponse rs;
            memset(&rs, 0, sizeof rs);
            rs.status = 200;
            rs.url = r->url;
            rs.content_type = text ? "text/plain" : "text/html";
            rs.charset = rs.encoding = rs.filename = rs.csp = rs.referrer_policy = rs.status_text = "";
            rs.content_length = (int64_t)strlen(body);
            s->response(s->ctx, i + 1, &rs);
        } else if (r->step == 1) {
            s->data(s->ctx, i + 1, body, strlen(body));
        } else if (r->step == 2) {
            AuFetchInfo info;
            memset(&info, 0, sizeof info);
            info.content_type = text ? "text/plain" : "text/html";
            info.status = 200;
            info.message = "";
            r->sink = NULL;
            s->done(s->ctx, i + 1, true, &info);
        }
        if (r->sink || r->step < 3)
            r->step++;
    }
}

static AuBrowser *make_browser(void)
{
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.time_ms = t_ms;
    h.now = t_now;
    h.read_file_url = t_read;
    h.confirm = t_confirm;
    h.coro = au_posix_coro_ops();
    h.script_coro = au_posix_coro_ops();
    return au_browser_new(fonts, &h);
}

static void paint(AuBrowserWindow *w)
{
    AuSurface *s = au_surface_new(w->width, w->height);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, w->width, w->height});
    au_canvas_free(c);
    au_surface_free(s);
}

/* Idles until the active page's script parks. */
static void until_parked(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    for (int i = 0; i < 2000 && !au_page_script_parked(t->page); i++)
        au_bw_idle(w);
    CHECK(au_page_script_parked(t->page));
}

static void settle(AuBrowserWindow *w)
{
    for (int i = 0; i < 20000 && au_browser_busy(w->browser); i++)
        au_bw_idle(w);
    CHECK(!au_browser_busy(w->browser));
}

static AuEvent key(int k)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.key = k;
    return e;
}

static void test_scroll(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "file:///mem/scroll.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    until_parked(w);
    /* the page scrolls and paints from the last layout */
    AuEvent e = key(AU_KEY_PAGE_DOWN);
    CHECK(au_bw_key(w, &e));
    CHECK(au_page_script_parked(t->page));
    CHECK(t->scroll_y > 0);
    int y = t->scroll_y;
    paint(w);
    /* a moving pointer is dropped */
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_MOUSE_MOVE;
    e.x = w->layout.page.x0 + 20;
    e.y = w->layout.page.y0 + 20;
    au_bw_mouse(w, &e);
    CHECK(au_page_script_parked(t->page));
    settle(w);
    char want[32];
    snprintf(want, sizeof want, "done %d", y);
    CHECK_STR(au_page_title(t->page), want);
    paint(w);
    au_browser_free(b);
}

/* A script that restyles through getComputedStyle and then parks: the
 * boxes it rebuilt are laid out before it parks, so the parked view
 * paints, scrolls and hit-tests boxes that are all alive. Reddit's feed
 * crashed the paint here (a dropped box in a stacking list). */
static void test_restyle_then_park(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "file:///mem/restyle.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    until_parked(w);
    paint(w);
    AuEvent e = key(AU_KEY_PAGE_DOWN);
    CHECK(au_bw_key(w, &e));
    CHECK(au_page_script_parked(t->page));
    paint(w);
    e = key(AU_KEY_PAGE_UP);
    CHECK(au_bw_key(w, &e));
    paint(w);
    settle(w);
    CHECK_STR(au_page_title(t->page), "flex done");
    paint(w);
    au_browser_free(b);
}

/* Once a second the parked script's document is laid out, so the window
 * shows what the script built so far and can scroll into it. */
static void test_shows_progress(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "file:///mem/grow.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    until_parked(w);
    for (int i = 0; i < 15 && au_page_script_parked(t->page); i++) {
        au_bw_idle(w); /* each resume runs 100 ms of the script */
        paint(w);
    }
    CHECK(au_page_script_parked(t->page));
    AuEvent e = key(AU_KEY_END);
    CHECK(au_bw_key(w, &e));
    CHECK(t->scroll_y > 1000);
    paint(w);
    settle(w);
    CHECK_STR(au_page_title(t->page), "grown");
    au_browser_free(b);
}

/* overflow: hidden on the root: the user does not scroll the page (keys,
 * the scroll bar's range), script does. */
static void test_scroll_locked(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "file:///mem/locked.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    settle(w);
    paint(w);
    AuEvent e = key(AU_KEY_PAGE_DOWN);
    au_bw_key(w, &e);
    CHECK(t->scroll_y == 0);
    int mx, my;
    au_bw_scroll_max(w, &mx, &my);
    CHECK(my == 0);
    au_page_eval(t->page, "go()", NULL);
    settle(w);
    CHECK_STR(au_page_title(t->page), "at 500");
    CHECK(t->scroll_y == 500);
    au_browser_free(b);
}

static void test_click_waits(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "file:///mem/click.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    until_parked(w);
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_MOUSE_DOWN;
    e.clicks = 1;
    e.x = w->layout.page.x0 + 20;
    e.y = w->layout.page.y0 + 20;
    au_bw_mouse(w, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_bw_mouse(w, &e);
    /* the press and release wait in the queue: the script stays parked
     * and the window still scrolls */
    CHECK(au_page_script_parked(t->page));
    CHECK(strcmp(au_page_title(t->page), "click after end") != 0);
    AuEvent k = key(AU_KEY_PAGE_DOWN);
    CHECK(au_bw_key(w, &k));
    CHECK(t->scroll_y > 0);
    /* they reach the page, in order, once the slice ends */
    settle(w);
    CHECK(!au_page_script_parked(t->page));
    CHECK_STR(au_page_title(t->page), "click after end");
    au_browser_free(b);
}

/* Stop drops what was queued with the script. */
static void test_queue_dropped_on_stop(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "file:///mem/click.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    until_parked(w);
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_MOUSE_DOWN;
    e.clicks = 1;
    e.x = w->layout.page.x0 + 20;
    e.y = w->layout.page.y0 + 20;
    au_bw_mouse(w, &e);
    au_page_stop(t->page);
    CHECK(!au_page_script_parked(t->page));
    settle(w);
    CHECK(strncmp(au_page_title(t->page), "click", 5) != 0);
    au_browser_free(b);
}

static void test_stop_and_leave(void)
{
    AuBrowser *b = make_browser();
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "file:///mem/stop.html", true, -1);
    AuTab *t = au_bw_active_tab(w);
    until_parked(w);
    au_bw_stop(w);
    CHECK(!au_page_script_parked(t->page));
    CHECK_STR(au_page_title(t->page), "");
    settle(w);
    /* leaving the page ends its script too */
    au_bw_navigate(w, "file:///mem/stop.html");
    until_parked(w);
    au_bw_navigate(w, "file:///mem/other.html");
    CHECK(!au_page_script_parked(t->page));
    settle(w);
    CHECK_STR(au_page_title(t->page), "Other");
    /* and closing it */
    au_bw_navigate(w, "file:///mem/stop.html");
    until_parked(w);
    au_browser_free(b);
}

static AuBrowser *net_browser(AuPageFetcher *f)
{
    AuBrowser *b = make_browser();
    memset(f, 0, sizeof *f);
    f->start = net_start;
    f->cancel = net_cancel;
    f->poll = net_poll;
    au_browser_set_fetcher(b, "http", f);
    au_browser_set_fetcher(b, "https", f);
    return b;
}

static void test_dialog(void)
{
    AuPageFetcher f;
    AuBrowser *b = net_browser(&f);
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "https://example.net/form", true, -1);
    AuTab *t = au_bw_active_tab(w);
    settle(w);
    CHECK(confirms == 1);
    CHECK_STR(au_page_title(t->page), "asked");
    au_browser_free(b);
}

static void test_held(void)
{
    AuPageFetcher f;
    AuBrowser *b = net_browser(&f);
    AuBrowserWindow *w = au_browser_window_new(b, 400, 300);
    au_bw_new_tab(w, "http://example.net/", true, -1);
    AuTab *t = au_bw_active_tab(w);
    until_parked(w);
    /* the data request is answered while the script is parked; the
     * answer waits for the slice to end */
    for (int i = 0; i < 6; i++)
        net_poll(NULL);
    CHECK(au_page_script_parked(t->page));
    settle(w);
    CHECK_STR(au_page_title(t->page), "after payload");
    au_browser_free(b);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    main_thread = pthread_self();
    test_scroll();
    test_restyle_then_park();
    test_shows_progress();
    test_scroll_locked();
    test_click_waits();
    test_queue_dropped_on_stop();
    test_stop_and_leave();
    test_dialog();
    test_held();
    au_fontset_free(fonts);
    printf("script_park: all tests passed\n");
    return 0;
}
