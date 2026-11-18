/* A new document's first paint waits for its style sheets
 * (src/browser/page_doc.c, render_held): the window shows the page's
 * background, not the document unstyled, until the sheets linked in its
 * head have arrived, and for at most RENDER_HOLD_MS (5 s). A sheet that
 * script adds once the page shows does not blank it again.
 *
 * The page's div has a red inline style, which applies without the
 * sheet; the sheet turns it green. Each check reads one pixel inside
 * the div. */
#include "../layout/ahem.h"
#include "browser/browser.h"

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

static AuFontSet *fonts;
static uint32_t clock_ms = 1000;
static uint32_t t_ms(void *ctx)
{
    (void)ctx;
    return clock_ms;
}
static AuTime t_now(void *ctx)
{
    (void)ctx;
    return 0;
}

static const char *page_body =
    "<!DOCTYPE html><html><head><link rel=stylesheet href='/s.css'></head>"
    "<body style='margin:0'><div id=d style='height:200px;background:#f00'>x</div>"
    "<script>setTimeout(function(){var l=document.createElement('link');l.rel='stylesheet';"
    "l.href='/late.css';document.head.appendChild(l);},3000);</script></body></html>";
static const char *sheet_body = "#d { background: #0f0 !important }";

/* A fetcher that answers the page at once and a sheet only once
 * sheets_go is set. */
typedef struct NetReq {
    const AuFetchSink *sink;
    char url[256];
    int step;
} NetReq;
static NetReq reqs[8];
static uint32_t nreqs;
static bool sheets_go;

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
        bool css = strstr(r->url, ".css") != NULL;
        if (css && !sheets_go)
            continue;
        const AuFetchSink *s = r->sink;
        const char *body = css ? sheet_body : page_body;
        const char *type = css ? "text/css" : "text/html";
        if (r->step == 0 && s->response) {
            AuFetchResponse rs;
            memset(&rs, 0, sizeof rs);
            rs.status = 200;
            rs.url = r->url;
            rs.content_type = type;
            rs.charset = rs.encoding = rs.filename = rs.csp = rs.referrer_policy = rs.status_text = "";
            rs.content_length = (int64_t)strlen(body);
            s->response(s->ctx, i + 1, &rs);
        } else if (r->step == 1) {
            s->data(s->ctx, i + 1, body, strlen(body));
        } else if (r->step == 2) {
            AuFetchInfo info;
            memset(&info, 0, sizeof info);
            info.content_type = type;
            info.status = 200;
            info.message = "";
            r->sink = NULL;
            s->done(s->ctx, i + 1, true, &info);
        }
        r->step++;
    }
}

static AuBrowser *make_browser(AuPageFetcher *f)
{
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.time_ms = t_ms;
    h.now = t_now;
    AuBrowser *b = au_browser_new(fonts, &h);
    memset(f, 0, sizeof *f);
    f->start = net_start;
    f->cancel = net_cancel;
    f->poll = net_poll;
    au_browser_set_fetcher(b, "http", f);
    return b;
}

/* The colour inside the div, from a paint of the whole window. */
static uint32_t div_colour(AuBrowserWindow *w)
{
    AuSurface *s = au_surface_new(w->width, w->height);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, w->width, w->height});
    int x = w->layout.page.x0 + 100, y = w->layout.page.y0 + 100;
    uint32_t px = s->pixels[y * s->stride + x] & 0xFFFFFF;
    au_canvas_free(c);
    au_surface_free(s);
    return px;
}

static void idle(AuBrowserWindow *w, int n, uint32_t step_ms)
{
    for (int i = 0; i < n; i++) {
        clock_ms += step_ms;
        au_bw_idle(w);
    }
}

static void start(AuBrowserWindow **wp, AuBrowser **bp, AuPageFetcher *f)
{
    nreqs = 0;
    sheets_go = false;
    *bp = make_browser(f);
    *wp = au_browser_window_new(*bp, 400, 300);
    au_bw_new_tab(*wp, "http://a.test/", true, -1);
}

/* The sheet arrives after a second: nothing red was ever painted. */
static void test_waits_for_sheet(void)
{
    AuPageFetcher f;
    AuBrowser *b;
    AuBrowserWindow *w;
    start(&w, &b, &f);
    for (int i = 0; i < 20; i++) {
        idle(w, 1, 50);
        CHECK(div_colour(w) != 0xFF0000);
    }
    CHECK(div_colour(w) == 0xFFFFFF);
    sheets_go = true;
    idle(w, 10, 50);
    CHECK(div_colour(w) == 0x00FF00);
    /* the sheet that script adds later does not blank the page */
    sheets_go = false;
    for (int i = 0; i < 80; i++) {
        idle(w, 1, 50);
        CHECK(div_colour(w) == 0x00FF00);
    }
    au_browser_free(b);
}

/* A sheet that never comes holds the page for 5 seconds only. */
static void test_gives_up(void)
{
    AuPageFetcher f;
    AuBrowser *b;
    AuBrowserWindow *w;
    start(&w, &b, &f);
    idle(w, 10, 100);
    CHECK(div_colour(w) == 0xFFFFFF);
    idle(w, 50, 100);
    CHECK(div_colour(w) == 0xFF0000);
    au_browser_free(b);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_waits_for_sheet();
    test_gives_up();
    au_fontset_free(fonts);
    printf("render_hold: all tests passed\n");
    return 0;
}
