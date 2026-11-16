/* Nested documents (src/page/frames.c, src/js/frames.c) through the
 * document view, with a loader that serves a small table of pages. Each
 * test loads a top-level page, pumps the loader and the script loop until
 * nothing is left, and looks at pixels, requests and what script saw.
 * Uses the Ahem-like test font. */
#include "../layout/ahem.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);                                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static AuFontSet *fonts;

/* ---- The site ---- */

typedef struct Page {
    const char *url;
    const char *body;
    const char *type;   /* NULL: text/html */
    const char *hname;  /* one more response header, or NULL */
    const char *hvalue;
    int status;         /* 0: 200 */
} Page;

typedef struct Req {
    uint32_t id;
    char url[512];
    char doc[512];
    AuResourceKind kind;
    bool post;
    char body[256];
    bool done, cancelled;
} Req;

typedef struct World {
    const Page *pages;
    Req req[400];
    int n;
    uint32_t next;
    AuDocView *dv;
    char opened[512];
    bool opened_new_tab;
    int opens;
    char submitted[512];
    AuBuf console;
    int finished;
    double clock;
    bool hold; /* pump delivers nothing */
} World;

static uint32_t w_request(void *ctx, AuDocView *dv, const AuResourceRequest *rq)
{
    World *w = ctx;
    CHECK(dv == w->dv, "a request names a nested view");
    if (w->n == 400)
        return 0;
    Req *r = &w->req[w->n++];
    memset(r, 0, sizeof *r);
    r->id = ++w->next;
    snprintf(r->url, sizeof r->url, "%s", rq->url);
    snprintf(r->doc, sizeof r->doc, "%s", rq->document_url ? rq->document_url : "");
    r->kind = rq->kind;
    r->post = rq->method && !strcmp(rq->method, "POST");
    if (rq->body)
        snprintf(r->body, sizeof r->body, "%.*s", (int)rq->body_len, (const char *)rq->body);
    return r->id;
}

static uint32_t w_request_plain(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    AuResourceRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.kind = kind;
    return w_request(ctx, dv, &rq);
}

static void w_cancel(void *ctx, uint32_t id)
{
    World *w = ctx;
    for (int i = 0; i < w->n; i++)
        if (w->req[i].id == id)
            w->req[i].cancelled = true;
}

static void w_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    (void)dv;
    World *w = ctx;
    snprintf(w->opened, sizeof w->opened, "%s", url);
    w->opened_new_tab = new_tab;
    w->opens++;
}

static void w_submit(void *ctx, AuDocView *dv, const AuFormSubmission *s)
{
    (void)dv;
    World *w = ctx;
    snprintf(w->submitted, sizeof w->submitted, "%s %s", s->method, s->url);
}

static void w_console(void *ctx, AuDocView *dv, int level, const char *text, size_t len)
{
    (void)dv;
    (void)level;
    World *w = ctx;
    au_buf_append(&w->console, text, len);
    au_buf_putc(&w->console, '\n');
}

static void w_finished(void *ctx, AuDocView *dv)
{
    (void)dv;
    ((World *)ctx)->finished++;
}

static double w_now(void *ctx, AuDocView *dv)
{
    (void)dv;
    World *w = ctx;
    return w->clock += 1;
}

static const Page *find_page(const World *w, const char *url)
{
    size_t n = strcspn(url, "#");
    for (const Page *p = w->pages; p && p->url; p++)
        if (strlen(p->url) == n && !strncmp(p->url, url, n))
            return p;
    return NULL;
}

static Req *find_req(World *w, const char *url)
{
    for (int i = 0; i < w->n; i++)
        if (!strcmp(w->req[i].url, url))
            return &w->req[i];
    return NULL;
}

static int count_reqs(World *w, AuResourceKind kind)
{
    int n = 0;
    for (int i = 0; i < w->n; i++)
        n += w->req[i].kind == kind;
    return n;
}

/* Delivers every outstanding request and runs script until nothing moves. */
static void pump(World *w)
{
    for (int round = 0; round < 200; round++) {
        bool any = false;
        for (int i = 0; i < w->n && !w->hold; i++) {
            Req *r = &w->req[i];
            if (r->done || r->cancelled)
                continue;
            r->done = true;
            any = true;
            const Page *p = find_page(w, r->url);
            if (!p) {
                au_docview_resource_done(w->dv, r->id, false);
                continue;
            }
            const char *hdrs[4] = {"content-type", p->type ? p->type : "text/html", p->hname, p->hvalue};
            au_docview_resource_head(w->dv, r->id, p->status ? p->status : 200, "", NULL, hdrs, p->hname ? 2 : 1);
            /* in two parts, as a network would */
            size_t len = strlen(p->body), half = len / 2;
            if (half)
                au_docview_resource_data(w->dv, r->id, p->body, half);
            if (len - half)
                au_docview_resource_data(w->dv, r->id, p->body + half, len - half);
            au_docview_resource_done(w->dv, r->id, true);
        }
        if (au_docview_run_scripts(w->dv, 50))
            any = true;
        au_docview_update(w->dv);
        /* work done at the end of a turn (a frame navigation) may have asked for more */
        for (int i = 0; i < w->n && !w->hold; i++)
            if (!w->req[i].done && !w->req[i].cancelled)
                any = true;
        if (!any && au_docview_scripts_due(w->dv) > 1000)
            break;
        w->clock += 20;
    }
}

static void world_open(World *w, const Page *pages, const char *url, int vw, int vh)
{
    memset(w, 0, sizeof *w);
    w->pages = pages;
    au_buf_init(&w->console);
    AuLoader l = {w, w_request_plain, w_cancel, NULL};
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = w;
    cb.open_url = w_open;
    cb.submit = w_submit;
    cb.console = w_console;
    cb.load_finished = w_finished;
    cb.now_ms = w_now;
    w->dv = au_docview_new(fonts, &l, &cb);
    au_docview_set_request_ex(w->dv, w_request);
    au_docview_set_scripting(w->dv, true);
    au_docview_set_viewport(w->dv, vw, vh);
    const Page *p = find_page(w, url);
    CHECK(p != NULL, "no page %s", url);
    au_docview_load(w->dv, p ? p->body : "", p ? strlen(p->body) : 0, url);
    pump(w);
}

static void world_close(World *w)
{
    au_docview_free(w->dv);
    au_buf_free(&w->console);
}

static const char *js(World *w, const char *src)
{
    static AuBuf out;
    au_buf_free(&out);
    au_buf_init(&out);
    if (!au_docview_eval(w->dv, src, &out))
        au_buf_puts(&out, " (threw)");
    pump(w);
    return au_buf_cstr(&out);
}

#define JS_IS(w, src, want)                                                                                            \
    do {                                                                                                               \
        const char *got_ = js(w, src);                                                                                 \
        CHECK(!strcmp(got_, want), "%s = \"%s\", want \"%s\"", src, got_, want);                                       \
    } while (0)

static AuSurface *shot(World *w, int vw, int vh, int sx, int sy)
{
    AuSurface *s = au_surface_new(vw, vh);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(w->dv, c, (AuRect){0, 0, (float)vw, (float)vh}, sx, sy);
    au_canvas_free(c);
    return s;
}

static void mouse(World *w, AuEventType t, int x, int y)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.x = x;
    e.y = y;
    e.clicks = 1;
    au_docview_mouse(w->dv, &e);
}

static void click(World *w, int x, int y)
{
    mouse(w, AU_EVENT_MOUSE_MOVE, x, y);
    mouse(w, AU_EVENT_MOUSE_DOWN, x, y);
    mouse(w, AU_EVENT_MOUSE_UP, x, y);
    pump(w);
}

static bool key(World *w, uint32_t ch, int k)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.ch = ch;
    e.key = k;
    bool used = au_docview_key(w->dv, &e);
    pump(w);
    return used;
}

#define RED AU_RGB(255, 0, 0)
#define BLUE AU_RGB(0, 0, 255)
#define WHITE AU_RGB(255, 255, 255)
#define GREEN AU_RGB(0, 128, 0)

/* ---- Layout and paint ---- */

static void test_paint(void)
{
    static const Page site[] = {
        {"http://a.test/", "<!DOCTYPE html><body style='margin:0'>"
                           "<iframe src='child.html' style='border:0;margin:20px 0 0 30px;width:100px;height:80px;"
                           "display:block'></iframe>"
                           "<iframe srcdoc=\"<body style='margin:0;background:blue'>\" style='border:5px solid green;"
                           "width:40px;height:30px;display:block'></iframe>"},
        {"http://a.test/child.html", "<!DOCTYPE html><body style='margin:0;background:white'>"
                                     "<div style='width:50px;height:40px;background:red'></div>"},
        {0}};
    World w;
    world_open(&w, site, "http://a.test/", 300, 200);
    Req *r = find_req(&w, "http://a.test/child.html");
    CHECK(r && r->kind == AU_RES_FRAME, "the frame's document is requested as a frame");
    CHECK(r && !strcmp(r->doc, "http://a.test/"), "the frame request names its parent document: %s", r ? r->doc : "");
    CHECK(w.finished == 1, "the page finished once: %d", w.finished);
    AuSurface *s = shot(&w, 300, 200, 0, 0);
    CHECK(au_surface_get(s, 35, 25) == RED, "the nested document paints in the frame's box: %08x",
          au_surface_get(s, 35, 25));
    CHECK(au_surface_get(s, 85, 25) == WHITE && au_surface_get(s, 35, 65) == WHITE, "beside its content");
    CHECK(au_surface_get(s, 25, 25) == WHITE && au_surface_get(s, 135, 25) == WHITE, "nothing outside the box");
    /* the srcdoc frame: 5px border, then the blue canvas */
    CHECK(au_surface_get(s, 2, 102) == GREEN, "frame border %08x", au_surface_get(s, 2, 102));
    CHECK(au_surface_get(s, 10, 110) == BLUE, "srcdoc document %08x", au_surface_get(s, 10, 110));
    CHECK(au_surface_get(s, 44, 134) == BLUE && au_surface_get(s, 47, 137) == GREEN, "srcdoc box extent");
    /* A reference: the child as a page of the frame's size paints the
     * same pixels as the frame shows. */
    World ref;
    world_open(&ref, site, "http://a.test/child.html", 100, 80);
    AuSurface *rs = shot(&ref, 100, 80, 0, 0);
    int diff = 0;
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 100; x++)
            diff += au_surface_get(rs, x, y) != au_surface_get(s, 30 + x, 20 + y);
    CHECK(diff == 0, "the frame differs from the reference render in %d pixels", diff);
    au_surface_free(rs);
    world_close(&ref);
    /* The page scrolled: the frame moves with it. */
    au_surface_free(s);
    s = shot(&w, 300, 200, 0, 10);
    CHECK(au_surface_get(s, 35, 15) == RED && au_surface_get(s, 35, 9) == WHITE, "the frame moves with the page");
    au_surface_free(s);
    CHECK(au_docview_frame_count(w.dv) == 2, "frame count %u", au_docview_frame_count(w.dv));
    world_close(&w);
}

/* ---- Scrolling, clicks and keys ---- */

static void test_input(void)
{
    static const Page site[] = {
        {"http://a.test/", "<!DOCTYPE html><body style='margin:0'>"
                           "<iframe name=f src='child.html' style='border:0;width:200px;height:100px;display:block'>"
                           "</iframe><a id=t href='third.html' target=f style='display:block;width:50px;height:20px'>"
                           "x</a>"},
        {"http://a.test/child.html",
         "<!DOCTYPE html><body style='margin:0'><a href='next.html' style='display:block;width:100px;height:20px;"
         "background:red'></a><input id=i style='display:block;width:100px;height:20px;border:0;padding:0;margin:0'>"
         "<div style='height:500px'></div><div id=low style='height:20px;background:blue'></div>"
         "<script>var clicks=0;document.addEventListener('click',function(){clicks++})</script>"},
        {"http://a.test/next.html", "<!DOCTYPE html><body style='margin:0;background:blue'>"
                                    "<a id=up href='top.html' target=_top style='display:block;height:30px'>t</a>"},
        {"http://a.test/third.html", "<!DOCTYPE html><body style='margin:0;background:green'>"},
        {0}};
    World w;
    world_open(&w, site, "http://a.test/", 300, 300);
    /* The wheel over the frame scrolls the frame, not the page. */
    AuSurface *s = shot(&w, 300, 300, 0, 0);
    CHECK(au_surface_get(s, 50, 10) == RED, "before scrolling");
    au_surface_free(s);
    CHECK(au_docview_scroll_at(w.dv, 50, 50, 0, 30), "the frame takes the wheel");
    s = shot(&w, 300, 300, 0, 0);
    CHECK(au_surface_get(s, 50, 10) != RED, "the frame scrolled: %08x", au_surface_get(s, 50, 10));
    au_surface_free(s);
    JS_IS(&w, "frames[0].scrollY", "30");
    CHECK(au_docview_scroll_at(w.dv, 50, 50, 0, 100000), "to the end");
    s = shot(&w, 300, 300, 0, 0);
    CHECK(au_surface_get(s, 50, 95) == BLUE, "the end of the nested document: %08x", au_surface_get(s, 50, 95));
    au_surface_free(s);
    CHECK(!au_docview_scroll_at(w.dv, 50, 50, 0, 30), "at its end the frame passes the wheel on");
    CHECK(au_docview_scroll_at(w.dv, 50, 50, 0, -100000), "back to the top");
    CHECK(!au_docview_scroll_at(w.dv, 50, 250, 0, 30), "outside the frame the page scrolls");
    /* A click on the nested input focuses it; keys go there. */
    click(&w, 50, 30);
    JS_IS(&w, "frames[0].clicks", "1");
    JS_IS(&w, "frames[0].document.activeElement.id", "i");
    CHECK(au_docview_focus_view(w.dv) != w.dv, "the nested view has the keyboard");
    CHECK(au_docview_editing(au_docview_focus_view(w.dv)), "a text field is being edited");
    key(&w, 'h', AU_KEY_NONE);
    key(&w, 'i', AU_KEY_NONE);
    JS_IS(&w, "frames[0].document.getElementById('i').value", "hi");
    /* A key the nested document does not use scrolls the frame. */
    click(&w, 150, 60);
    CHECK(key(&w, 0, AU_KEY_PAGE_DOWN), "page down scrolls the frame");
    JS_IS(&w, "frames[0].scrollY > 0", "true");
    key(&w, 0, AU_KEY_HOME);
    /* A click outside takes the keyboard back. */
    click(&w, 250, 250);
    CHECK(au_docview_focus_view(w.dv) == w.dv, "the page has the keyboard again");
    /* A link in the frame navigates the frame. */
    click(&w, 50, 10);
    Req *r = find_req(&w, "http://a.test/next.html");
    CHECK(r && r->kind == AU_RES_FRAME && w.opens == 0, "a link in a frame loads in the frame");
    CHECK(r && !strcmp(r->doc, "http://a.test/"), "requested for the parent: %s", r ? r->doc : "");
    JS_IS(&w, "frames[0].location.href", "http://a.test/next.html");
    s = shot(&w, 300, 300, 0, 0);
    CHECK(au_surface_get(s, 150, 50) == BLUE, "the new document shows: %08x", au_surface_get(s, 150, 50));
    au_surface_free(s);
    /* A link of the page with target naming the frame. */
    click(&w, 10, 110);
    CHECK(find_req(&w, "http://a.test/third.html") && w.opens == 0, "target names a frame");
    JS_IS(&w, "frames.f.location.pathname", "/third.html");
    /* target=_top from inside a frame navigates the tab. */
    js(&w, "document.querySelector('iframe').src = 'next.html'");
    click(&w, 50, 10);
    CHECK(w.opens == 1 && !strcmp(w.opened, "http://a.test/top.html") && !w.opened_new_tab,
          "_top goes to the tab: %d %s", w.opens, w.opened);
    world_close(&w);
}

/* ---- Script: windows and documents ---- */

static void test_script(void)
{
    static const Page site[] = {
        {"http://a.test/",
         "<!DOCTYPE html><body><script>var loads=[],got=[],order=[];function hello(x){return 'hello '+x}"
         "addEventListener('message',function(e){got.push(e.data+'|'+e.origin+'|'+(e.source===frames[0]?0:e.source==="
         "frames[1]?1:'?'))});addEventListener('load',function(){order.push('page')})</script>"
         "<iframe id=same name=same src='same.html' onload=\"loads.push('same');order.push('same')\"></iframe>"
         "<iframe id=cross src='http://b.test/cross.html' onload=\"loads.push('cross')\"></iframe>"
         "<iframe id=blank></iframe>"},
        {"http://a.test/same.html",
         "<!DOCTYPE html><body><p id=p>inner</p><script>var secret=42;var fromParent=parent.hello('child');"
         "var seen=[];addEventListener('message',function(e){seen.push(e.data+'|'+e.origin+'|'+(e.source===parent))})"
         "</script>"},
        {"http://b.test/cross.html",
         "<!DOCTYPE html><body><script>var err='';try{parent.document}catch(e){err=e.name}"
         "var err2='';try{parent.hello}catch(e){err2=e.name}"
         "var seen=[];addEventListener('message',function(e){seen.push(e.data.n+'|'+e.origin);"
         "if(e.data.n==='sib'){var sb='';try{sb=typeof parent.frames['same'].postMessage+(parent.same===parent.frames[0])}"
         "catch(x){sb=x.name}parent.frames.same.postMessage('from sibling '+sb,'*')}"
         "e.source.postMessage('reply:'+e.data.n+':'+err+':'+err2+':'+(top===parent)+':'+parent.length,e.origin)});"
         "parent.postMessage('ready','http://a.test');parent.postMessage('wrong','http://c.test')</script>"},
        {0}};
    World w;
    world_open(&w, site, "http://a.test/", 400, 600);
    JS_IS(&w, "loads.sort().join()", "cross,same");
    JS_IS(&w, "order.join()", "same,page");
    JS_IS(&w, "window.length", "3");
    JS_IS(&w, "frames[0] === document.getElementById('same').contentWindow", "true");
    JS_IS(&w, "window.same === frames[0] && frames.same.secret", "42");
    JS_IS(&w, "frames[0].fromParent", "hello child");
    JS_IS(&w, "document.getElementById('same').contentDocument.getElementById('p').textContent", "inner");
    JS_IS(&w, "frames[0].frameElement === document.getElementById('same')", "true");
    JS_IS(&w, "frames[0].parent === window && frames[0].top === window && window.parent === window", "true");
    JS_IS(&w, "window.frameElement", "null");
    JS_IS(&w, "frames[0].document.defaultView === frames[0]", "true");
    /* a same-origin document's objects are its own realm's */
    JS_IS(&w, "frames[0].document.body instanceof frames[0].HTMLBodyElement", "true");
    JS_IS(&w, "frames[0].document.body instanceof HTMLBodyElement", "false");
    JS_IS(&w, "frames[0].Array !== Array", "true");
    /* a blank frame has a document of the parent's origin */
    JS_IS(&w, "frames[2].document.URL + ' ' + frames[2].origin", "about:blank http://a.test");
    JS_IS(&w, "frames[2].document.body.appendChild(frames[2].document.createElement('b')).localName", "b");
    /* cross-origin: only the allowed members */
    JS_IS(&w, "document.getElementById('cross').contentDocument", "null");
    JS_IS(&w, "var n=''; try { frames[1].document } catch (e) { n = e.name } n", "SecurityError");
    JS_IS(&w, "n=''; try { frames[1].seen } catch (e) { n = e.name } n", "SecurityError");
    JS_IS(&w, "n=''; try { frames[1].foo = 1 } catch (e) { n = e.name } n", "SecurityError");
    JS_IS(&w, "n=''; try { frames[1].location.href } catch (e) { n = e.name } n", "SecurityError");
    JS_IS(&w, "n=''; try { Object.getPrototypeOf(frames[1]) === null || x } catch (e) { n = e.name } n", "");
    JS_IS(&w, "typeof frames[1].postMessage + frames[1].closed + frames[1].length + (frames[1].parent === window)",
          "functionfalse0true");
    JS_IS(&w, "frames[1] === document.getElementById('cross').contentWindow", "true");
    /* the cross-origin child saw SecurityError both ways and posted; the
     * message for another origin was dropped */
    JS_IS(&w, "got.join()", "ready|http://b.test|1");
    js(&w, "frames[1].postMessage({n: 7}, 'http://b.test'); frames[1].postMessage({n: 8}, 'http://a.test')");
    JS_IS(&w, "got[1]", "reply:7:SecurityError:SecurityError:true:3|http://b.test|1");
    JS_IS(&w, "got.length", "2");
    /* same-origin postMessage: the source is the caller's window */
    js(&w, "frames[0].postMessage('ping', '*')");
    JS_IS(&w, "frames[0].seen.join()", "ping|http://a.test|true");
    /* a port sent to another origin arrives as a port of that realm */
    js(&w, "var ch = new MessageChannel(), back = []; ch.port1.onmessage = function (e) { back.push(e.data) };"
           "frames[0].eval(\"addEventListener('message', function (e) { if (e.ports.length) {"
           " var p = e.ports[0]; p.postMessage('via port ' + (p instanceof MessagePort)); } })\");"
           "frames[0].postMessage('port', '*', [ch.port2])");
    JS_IS(&w, "back.join()", "via port true");
    /* a frame looked up by name through a cross-origin parent: the
     * cross-origin child asks its parent for its sibling */
    js(&w, "frames[1].postMessage({n: 'sib'}, 'http://b.test')");
    JS_IS(&w, "got[got.length - 1]", "reply:sib:SecurityError:SecurityError:true:3|http://b.test|1");
    JS_IS(&w, "frames[0].seen[frames[0].seen.length - 1]", "from sibling functiontrue|http://b.test|false");
    /* setting a cross-origin window's location navigates it */
    js(&w, "frames[1].location = 'http://a.test/same.html'");
    JS_IS(&w, "frames[1].secret", "42");
    CHECK(strstr(au_buf_cstr(&w.console), "Uncaught") == NULL, "script errors: %s", au_buf_cstr(&w.console));
    world_close(&w);
}

/* ---- Sandbox ---- */

static void test_sandbox(void)
{
    static const Page site[] = {
        {"http://a.test/",
         "<!DOCTYPE html><body><script>var ran=[];addEventListener('message',function(e){ran.push(e.data+'@'+e.origin)})"
         "</script>"
         "<iframe id=s0 sandbox src='kid.html?0'></iframe>"
         "<iframe id=s1 sandbox='allow-scripts' src='kid.html?1'></iframe>"
         "<iframe id=s2 sandbox='allow-scripts allow-same-origin allow-forms' src='kid.html?2'></iframe>"},
        {"http://a.test/kid.html?0", "<!DOCTYPE html><body><form action='sent.html'><input name=q value=1></form>"
                                     "<script>parent.postMessage('zero','*')</script>"},
        {"http://a.test/kid.html?1",
         "<!DOCTYPE html><body><form id=f action='sent.html'><input name=q value=1></form>"
         "<script>var e1='';try{parent.document}catch(e){e1=e.name}var e2='';try{top.location='http://x.test/'}"
         "catch(e){e2=e.name}parent.postMessage('one:'+e1+':'+e2+':'+origin,'*');document.getElementById('f').submit()"
         "</script>"},
        {"http://a.test/kid.html?2",
         "<!DOCTYPE html><body><form id=f action='sent.html'><input name=q value=1></form>"
         "<script>parent.postMessage('two:'+(parent.document===top.document)+':'+origin,'*');"
         "document.getElementById('f').submit()</script>"},
        {"http://a.test/sent.html?q=1", "<!DOCTYPE html><body>sent<script>parent.postMessage('sent','*')</script>"},
        {0}};
    World w;
    world_open(&w, site, "http://a.test/", 400, 600);
    JS_IS(&w, "ran.sort().join()", "one:SecurityError:SecurityError:null@null,sent@http://a.test,two:true:http://a.test@http://a.test");
    /* without allow-same-origin the parent cannot reach in either */
    JS_IS(&w, "document.getElementById('s1').contentDocument", "null");
    JS_IS(&w, "document.getElementById('s2').contentDocument !== null", "true");
    CHECK(w.opens == 0, "a sandboxed frame navigated the tab: %s", w.opened);
    int sent = 0;
    for (int i = 0; i < w.n; i++)
        sent += !strcmp(w.req[i].url, "http://a.test/sent.html?q=1");
    CHECK(sent == 1, "only the frame with allow-forms submitted: %d", sent);
    world_close(&w);
}

/* ---- Framing rules, limits ---- */

static void test_blocking(void)
{
    static const Page site[] = {
        {"http://a.test/",
         "<!DOCTYPE html><body><script>var loads=0</script>"
         "<iframe id=deny src='http://b.test/deny' onload='loads++'></iframe>"
         "<iframe id=so src='http://b.test/sameorigin' onload='loads++'></iframe>"
         "<iframe id=so2 src='so-ok' onload='loads++'></iframe>"
         "<iframe id=none src='http://b.test/csp-none' onload='loads++'></iframe>"
         "<iframe id=self src='http://b.test/csp-self' onload='loads++'></iframe>"
         "<iframe id=host src='http://b.test/csp-host' onload='loads++'></iframe>"
         "<iframe id=other src='http://b.test/csp-other' onload='loads++'></iframe>"
         "<iframe id=me src='http://a.test/#again' onload='loads++'></iframe>"},
        {"http://b.test/deny", "<body>DENY", NULL, "x-frame-options", "DENY"},
        {"http://b.test/sameorigin", "<body>SO", NULL, "x-frame-options", "SAMEORIGIN"},
        {"http://a.test/so-ok", "<body>SO-OK", NULL, "x-frame-options", "sameorigin"},
        {"http://b.test/csp-none", "<body>NONE", NULL, "content-security-policy", "default-src 'self'; frame-ancestors 'none'"},
        {"http://b.test/csp-self", "<body>SELF", NULL, "content-security-policy", "frame-ancestors 'self'"},
        {"http://b.test/csp-host", "<body>HOST", NULL, "content-security-policy", "frame-ancestors http://a.test https://*.c.test"},
        {"http://b.test/csp-other", "<body>OTHER", NULL, "content-security-policy", "frame-ancestors http://*.c.test"},
        {0}};
    World w;
    world_open(&w, site, "http://a.test/", 400, 600);
    JS_IS(&w, "loads", "8");
    JS_IS(&w, "document.getElementById('so2').contentDocument.body.textContent", "SO-OK");
    /* a blocked frame is an empty document of no use to the page */
    JS_IS(&w, "['deny','so','none','self','host','other'].map(function (i) { var f = document.getElementById(i);"
              "try { return f.contentDocument ? f.contentDocument.body.textContent.length : 'x' } catch (e) { return 'E' } }).join()",
          "0,0,0,0,x,0");
    CHECK(find_req(&w, "http://a.test/#again") == NULL, "a frame of its own ancestor's URL was requested");
    int n = 0;
    for (const char *p = au_buf_cstr(&w.console); (p = strstr(p, "does not allow framing")); p++)
        n++;
    CHECK(n == 5, "framing refusals reported: %d\n%s", n, au_buf_cstr(&w.console));
    world_close(&w);

    /* Depth: each level embeds the next. */
    static const Page deep[] = {
        {"http://a.test/", "<iframe src='n1'></iframe>"},  {"http://a.test/n1", "<iframe src='n2'></iframe>"},
        {"http://a.test/n2", "<iframe src='n3'></iframe>"}, {"http://a.test/n3", "<iframe src='n4'></iframe>"},
        {"http://a.test/n4", "<iframe src='n5'></iframe>"}, {"http://a.test/n5", "<iframe src='n6'></iframe>"},
        {"http://a.test/n6", "<iframe src='n7'></iframe>"}, {"http://a.test/n7", "<iframe src='n8'></iframe>"},
        {"http://a.test/n8", "<iframe src='n9'></iframe>"}, {"http://a.test/n9", "<iframe src='n10'></iframe>"},
        {"http://a.test/n10", "<iframe src='n11'></iframe>"}, {"http://a.test/n11", "deepest"},
        {0}};
    world_open(&w, deep, "http://a.test/", 400, 600);
    CHECK(count_reqs(&w, AU_RES_FRAME) == 8, "nesting stops at 8 levels: %d requests", count_reqs(&w, AU_RES_FRAME));
    CHECK(w.finished == 1, "the deep page finished");
    world_close(&w);

    /* Count: a page of 200 frames gets 64 documents. */
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "<!DOCTYPE html><body>");
    for (int i = 0; i < 200; i++)
        au_buf_printf(&b, "<iframe src='k%d'></iframe>", i);
    Page many[202];
    static char urls[200][32];
    memset(many, 0, sizeof many);
    many[0].url = "http://a.test/";
    many[0].body = au_buf_cstr(&b);
    for (int i = 0; i < 200; i++) {
        snprintf(urls[i], sizeof urls[i], "http://a.test/k%d", i);
        many[i + 1].url = urls[i];
        many[i + 1].body = "<body>k";
    }
    world_open(&w, many, "http://a.test/", 400, 600);
    CHECK(count_reqs(&w, AU_RES_FRAME) == 64, "frames per page: %d requests", count_reqs(&w, AU_RES_FRAME));
    CHECK(au_docview_frame_count(w.dv) == 64, "frame count %u", au_docview_frame_count(w.dv));
    JS_IS(&w, "window.length", "64");
    world_close(&w);
    au_buf_free(&b);
}

/* ---- Lazy loading, objects ---- */

static void test_lazy_object(void)
{
    static const Page site[] = {
        {"http://a.test/",
         "<!DOCTYPE html><body style='margin:0'><script>var ev=[]</script>"
         "<object id=o1 data='doc.html' style='display:block;width:100px;height:50px' onload=\"ev.push('o1 load')\">"
         "<p id=f1>fallback</p></object>"
         "<object id=o2 data='missing.html' onerror=\"ev.push('o2 error')\"><p id=f2 style='margin:0;height:17px'>"
         "fallback two</p></object>"
         "<div style='height:3000px'></div>"
         "<iframe id=lazy loading=lazy src='late.html' style='display:block'></iframe>"},
        {"http://a.test/doc.html", "<!DOCTYPE html><body style='margin:0;background:red'>"},
        {"http://a.test/missing.html", "<body>not found", NULL, NULL, NULL, 404},
        {"http://a.test/late.html", "<!DOCTYPE html><body>late"},
        {0}};
    World w;
    world_open(&w, site, "http://a.test/", 300, 200);
    JS_IS(&w, "ev.sort().join()", "o1 load,o2 error");
    AuSurface *s = shot(&w, 300, 200, 0, 0);
    CHECK(au_surface_get(s, 50, 25) == RED, "an object shows an HTML document: %08x", au_surface_get(s, 50, 25));
    au_surface_free(s);
    JS_IS(&w, "document.getElementById('o1').contentDocument.URL", "http://a.test/doc.html");
    JS_IS(&w, "document.getElementById('f1').offsetHeight", "0");
    JS_IS(&w, "document.getElementById('f2').offsetHeight", "17");
    JS_IS(&w, "document.getElementById('o2').contentDocument", "null");
    CHECK(find_req(&w, "http://a.test/late.html") == NULL, "a lazy frame far below loaded at once");
    CHECK(w.finished == 1, "a lazy frame does not hold the load event");
    s = shot(&w, 300, 200, 0, 2900);
    au_surface_free(s);
    pump(&w);
    CHECK(find_req(&w, "http://a.test/late.html") != NULL, "the lazy frame loads when it is shown");
    JS_IS(&w, "document.getElementById('lazy').contentDocument.body.textContent", "late");
    world_close(&w);
}

/* ---- Removal and navigation while script runs ---- */

static void test_lifetime(void)
{
    static const Page site[] = {
        {"http://a.test/",
         "<!DOCTYPE html><body><script>var log=[];var kept;function drop(id){var f=document.getElementById(id);"
         "f.parentNode.removeChild(f);return 'dropped'}</script>"
         "<iframe id=a src='a.html'></iframe><iframe id=b src='b.html'></iframe><iframe id=c src='slow.html'></iframe>"
         "<iframe id=d src='d.html' onload=\"log.push('d loaded');drop('d')\"></iframe>"},
        /* removes its own frame from inside its script, then keeps running */
        {"http://a.test/a.html",
         "<!DOCTYPE html><body><p id=p>a</p><script>var r=parent.drop('a');var after=document.getElementById('p')"
         ".textContent;parent.log.push('a '+r+' '+after+' '+(window.parent===window.top));"
         "setTimeout(function(){parent.log.push('a timer')},0)</script>"},
        {"http://a.test/b.html",
         "<!DOCTYPE html><body><div id=x>bee</div><script>function f(){return document.getElementById('x')"
         ".textContent}var obj={v:1}</script>"},
        {"http://a.test/d.html", "<!DOCTYPE html><body>d"},
        {"http://a.test/b2.html", "<!DOCTYPE html><body><script>var second=true</script>"},
        {"http://a.test/outer.html", "<!DOCTYPE html><body><iframe id=in src='b.html'></iframe>"
                                     "<iframe src='b2.html'></iframe>"},
        {0}};
    World w;
    world_open(&w, site, "http://a.test/", 400, 600);
    /* slow.html is not in the site: it fails; the page still finishes */
    JS_IS(&w, "log.join()", "a dropped a true,d loaded");
    JS_IS(&w, "window.length", "2");
    CHECK(w.finished == 1, "finished: %d", w.finished);
    /* Keep objects of a frame, then remove it and navigate another. */
    js(&w, "kept = {f: frames[0].f, doc: frames[0].document, el: frames[0].document.getElementById('x'),"
           " obj: frames[0].obj, win: frames[0]}");
    js(&w, "drop('b')");
    JS_IS(&w, "window.length", "1");
    /* the removed document's objects still work */
    JS_IS(&w, "kept.el.textContent + kept.obj.v + kept.f() + kept.doc.getElementById('x').localName", "bee1beediv");
    JS_IS(&w, "kept.el.ownerDocument === kept.doc", "true");
    JS_IS(&w, "kept.win.setTimeout(function () { log.push('late timer') }, 0); typeof kept.win.document", "object");
    JS_IS(&w, "kept.doc.body.appendChild(kept.doc.createElement('i')).localName", "i");
    js(&w, "kept = null");
    /* Remove during a callback from the frame being removed, while its
     * request is still out. */
    w.hold = true;
    js(&w, "var n = document.createElement('iframe'); n.id = 'n'; n.src = 'b.html'; document.body.appendChild(n);"
           "var m = document.createElement('iframe'); m.id = 'm'; m.src = 'b2.html'; document.body.appendChild(m)");
    JS_IS(&w, "window.length", "3");
    js(&w, "drop('n')"); /* while loading */
    w.hold = false;
    pump(&w);
    JS_IS(&w, "window.length", "2");
    JS_IS(&w, "document.getElementById('m').contentWindow.second", "true");
    /* Navigate a frame from its own script and from a held reference. */
    js(&w, "var mw = document.getElementById('m').contentWindow; var mdoc = mw.document;"
           "mw.location.href = 'b.html'");
    JS_IS(&w, "document.getElementById('m').contentWindow.f()", "bee");
    JS_IS(&w, "typeof mdoc.body", "object"); /* the document it left is still an object */
    /* A frame removed and put back loads what its src names again. */
    js(&w, "var mm = document.getElementById('m'); document.body.removeChild(mm); document.body.appendChild(mm)");
    JS_IS(&w, "document.getElementById('m').contentWindow.second", "true");
    /* A frame inside a frame is removed, then the outer one, in one
     * turn: both wait in the same list and are freed together. */
    js(&w, "var o = document.createElement('iframe'); o.id = 'o'; o.src = 'outer.html'; document.body.appendChild(o)");
    JS_IS(&w, "document.getElementById('o').contentWindow.length", "2");
    JS_IS(&w, "var od = o.contentDocument; var inner = od.getElementById('in'); var iw = inner.contentWindow;"
              "od.body.removeChild(inner); drop('o'); typeof iw.f + od.body.childNodes.length", "function1");
    JS_IS(&w, "window.length", "2");
    /* innerHTML replaces every frame at once, from inside a frame's call */
    js(&w, "document.getElementById('m').contentWindow.eval(\"parent.document.body.innerHTML = '<p>gone</p>'; 1\")");
    JS_IS(&w, "window.length + document.body.textContent", "0gone");
    CHECK(au_docview_frame_count(w.dv) == 0, "frames left: %u", au_docview_frame_count(w.dv));
    CHECK(strstr(au_buf_cstr(&w.console), "Internal error") == NULL, "%s", au_buf_cstr(&w.console));
    world_close(&w);

    /* The page goes while frames load and run. */
    world_open(&w, site, "http://a.test/", 400, 600);
    w.hold = true;
    js(&w, "var n = document.createElement('iframe'); n.src = 'b.html'; document.body.appendChild(n);"
           "setInterval(function () { log.push(1) }, 1)");
    world_close(&w);
}

/* ---- A load listener that begins the frame again ----
 * Navigating to about:blank or setting srcdoc needs no request, so the
 * next load comes at once: the loop never ends (as in other browsers),
 * but one turn of the event loop may begin only a few documents of it. */
static void test_reload_loop(void)
{
    static const Page site[] = {{"http://a.test/", "<!DOCTYPE html><body><script>var n=0</script>"}, {0}};
    static const char *const loops[] = {
        "var f = document.createElement('iframe'); f.onload = function () { n++;"
        " f.contentWindow.location.href = 'about:blank' }; document.body.appendChild(f)",
        "var f = document.createElement('iframe'); f.onload = function () { n++;"
        " f.srcdoc = '<p>' + n }; document.body.appendChild(f)",
    };
    for (int k = 0; k < 2; k++) {
        World w;
        world_open(&w, site, "http://a.test/", 400, 300);
        AuBuf out;
        au_buf_init(&out);
        au_docview_eval(w.dv, loops[k], &out);
        au_buf_free(&out);
        int last = 0;
        for (int turn = 0; turn < 6; turn++) {
            au_docview_run_scripts(w.dv, 50);
            au_buf_init(&out);
            au_docview_eval(w.dv, "n", &out);
            int now = atoi(au_buf_cstr(&out));
            au_buf_free(&out);
            /* run_scripts calls au_frames_idle twice */
            CHECK(now - last <= 2 * 4 + 1, "loop %d turn %d: %d loads in one turn", k, turn, now - last);
            CHECK(turn < 2 || now > last, "loop %d turn %d: the loop stopped at %d", k, turn, now);
            CHECK(au_docview_scripts_due(w.dv) == 0, "loop %d: more work is announced", k);
            last = now;
        }
        world_close(&w);
    }
}

/* ---- A kept document ---- */

static void test_kept(void)
{
    static const Page site[] = {
        {"http://a.test/", "<!DOCTYPE html><body><script>var ev=[]</script><iframe src='kid.html'></iframe>"
                           "<iframe src='slow.html'></iframe>"},
        {"http://a.test/kid.html",
         "<!DOCTYPE html><body><img src='i.png'><script>var ticks=0;setInterval(function(){ticks++},10);"
         "addEventListener('pagehide',function(e){parent.ev.push('hide '+e.persisted)});"
         "addEventListener('pageshow',function(e){parent.ev.push('show '+e.persisted)})</script>"},
        {0}};
    World w;
    memset(&w, 0, sizeof w);
    world_open(&w, site, "http://a.test/", 400, 600);
    CHECK(au_docview_can_persist(w.dv), "a loaded page with frames can be kept");
    size_t with = au_docview_memory_estimate(w.dv);
    static const Page bare[] = {{"http://a.test/", "<!DOCTYPE html><body><script>var ev=[]</script>"}, {0}};
    World b;
    world_open(&b, bare, "http://a.test/", 400, 600);
    size_t without = au_docview_memory_estimate(b.dv);
    world_close(&b);
    CHECK(with > without + 1000, "the estimate counts frames: %zu against %zu", with, without);
    au_docview_set_hidden(w.dv, true);
    /* nothing calls a hidden view: its frames' timers stand still */
    const char *t0 = js(&w, "frames[0].ticks");
    char before[32];
    snprintf(before, sizeof before, "%s", t0);
    au_docview_set_hidden(w.dv, false);
    JS_IS(&w, "ev.join()", "show false,hide true,show true");
    w.clock += 100;
    pump(&w);
    CHECK(strcmp(js(&w, "frames[0].ticks"), before) != 0, "timers of a frame run again after the page is shown");
    /* A frame still loading: not kept. */
    w.hold = true;
    js(&w, "var n = document.createElement('iframe'); n.src = 'kid.html'; document.body.appendChild(n)");
    CHECK(!au_docview_can_persist(w.dv), "a page with a frame still loading is not kept");
    world_close(&w);
}

/* ---- A widget in a cross-origin frame ----
 * The shape of an embedded third-party widget: the frame's script sizes
 * itself by asking its parent, opens a channel, and takes clicks and
 * keys. */
static void test_widget(void)
{
    static const Page site[] = {
        {"http://shop.test/",
         "<!DOCTYPE html><body style='margin:0'><iframe id=w src='http://widget.test/box' style='border:0;"
         "display:block;width:100px;height:20px'></iframe>"
         "<script>var said=[];addEventListener('message',function(e){if(e.origin!=='http://widget.test')return;"
         "var d=e.data;if(d.type==='size'){var f=document.getElementById('w');f.style.width=d.w+'px';"
         "f.style.height=d.h+'px'}if(d.type==='hello'){e.ports[0].onmessage=function(m){said.push(m.data)};"
         "e.ports[0].postMessage('welcome')}})</script>"},
        {"http://widget.test/box",
         "<!DOCTYPE html><body style='margin:0;background:white'>"
         "<div id=box style='width:28px;height:28px;margin:10px;background:red'></div>"
         "<input id=k style='display:block;margin:10px;width:100px;height:20px;border:0;padding:0'>"
         "<script>var ch=new MessageChannel();ch.port1.onmessage=function(e){ch.port1.postMessage('got '+e.data)};"
         "parent.postMessage({type:'size',w:240,h:120},'http://shop.test');"
         "parent.postMessage({type:'hello'},'http://shop.test',[ch.port2]);"
         "document.getElementById('box').addEventListener('click',function(){this.style.background='blue';"
         "ch.port1.postMessage('checked')});"
         "document.getElementById('k').addEventListener('input',function(){ch.port1.postMessage('typed '+this.value)})"
         "</script>"},
        {0}};
    World w;
    world_open(&w, site, "http://shop.test/", 400, 300);
    JS_IS(&w, "document.getElementById('w').offsetWidth + 'x' + document.getElementById('w').offsetHeight", "240x120");
    JS_IS(&w, "said.join()", "got welcome");
    AuSurface *s = shot(&w, 400, 300, 0, 0);
    CHECK(au_surface_get(s, 20, 20) == RED, "the widget paints: %08x", au_surface_get(s, 20, 20));
    au_surface_free(s);
    click(&w, 20, 20);
    s = shot(&w, 400, 300, 0, 0);
    CHECK(au_surface_get(s, 20, 20) == BLUE, "a click reaches the widget: %08x", au_surface_get(s, 20, 20));
    au_surface_free(s);
    click(&w, 50, 60);
    key(&w, 'o', AU_KEY_NONE);
    key(&w, 'k', AU_KEY_NONE);
    JS_IS(&w, "said.join()", "got welcome,checked,typed o,typed ok");
    CHECK(strstr(au_buf_cstr(&w.console), "Uncaught") == NULL, "script errors: %s", au_buf_cstr(&w.console));
    world_close(&w);
}

/* ---- An image-tile challenge in a positioned frame ---- */

/* The shape of reCAPTCHA's 4x4 image challenge (class names and the rules
 * that matter from its public style sheet), in a frame inside an
 * absolutely positioned container with a large z-index that script
 * creates hidden and then moves into view. Each tile's wrapper clips and
 * has a transform, so it is a stacking context, and its picture is the
 * whole photo moved by -100% steps; a click toggles the tile's selected
 * class and shows its check mark, a background image of an absolutely
 * positioned box. Every click went to the last tile, and the check mark
 * (a long data: URL there; a plain URL here) was not painted. The tiles
 * are 40px wrappers in 42px cells. */
static void test_image_tiles(void)
{
    static const Page site[] = {
        {"http://site.test/",
         "<!DOCTYPE html><body style='margin:0;background:#eee'>"
         "<script>var picked='';addEventListener('message',function(e){if(e.origin==='http://challenge.test')"
         "picked=e.data});var outer=document.createElement('div');"
         "outer.style.cssText='visibility:hidden;position:absolute;width:100%;top:-10000px;left:0;right:0;"
         "transition:visibility 0s linear 0.3s,opacity 0.3s linear;opacity:0';"
         "var box=document.createElement('div');"
         "box.style.cssText='border:1px solid #ccc;position:absolute;z-index:2000000000;background:#fff';"
         "var f=document.createElement('iframe');f.setAttribute('frameborder','0');f.setAttribute('scrolling','no');"
         "f.src='http://challenge.test/bframe';f.style.cssText='width:200px;height:200px;display:block';"
         "box.appendChild(f);outer.appendChild(box);document.body.appendChild(outer);"
         "setTimeout(function(){outer.style.transition='visibility 0s linear 0s,opacity 0.3s linear';"
         "outer.style.visibility='visible';outer.style.opacity='1';outer.style.top='10px';"
         "box.style.left='20px';box.style.top='0px'},10)</script>"},
        {"http://challenge.test/bframe",
         "<!DOCTYPE html><style>body{margin:0;background:#fff}"
         ".rc-imageselect-table-44{border-collapse:separate;border-spacing:0;transition:all 1s ease;margin:-1px}"
         ".rc-imageselect-table-44 td{padding:1px}"
         ".rc-image-tile-target{position:relative}"
         ".rc-imageselect-tileselected{position:relative}"
         ".rc-image-tile-wrapper{overflow:hidden;position:relative;transform:scale(1);transition:.1s ease;"
         "width:40px;height:40px}"
         ".rc-imageselect-tileselected .rc-image-tile-wrapper{transform:scale(.8)}"
         ".rc-image-tile-wrapper img{position:relative}"
         ".rc-image-tile-44{width:400%;height:400%}"
         ".rc-imageselect-checkbox{background:url(check.svg);display:none;position:absolute}"
         ".rc-imageselect-tileselected .rc-imageselect-checkbox{display:block;background-repeat:no-repeat;"
         "bottom:0;left:0;right:0;top:0}</style>"
         "<table class=rc-imageselect-table-44><tbody id=tb></tbody></table>"
         "<script>var log=[];var tb=document.getElementById('tb');"
         "for(var r=0;r<4;r++){var tr=document.createElement('tr');for(var c=0;c<4;c++){"
         "var td=document.createElement('td');td.setAttribute('role','button');td.id=String(r*4+c);td.tabIndex=0;"
         "td.className='rc-imageselect-tile';"
         "td.innerHTML='<div class=\"rc-image-tile-target\"><div class=\"rc-image-tile-wrapper\">'+"
         "'<img class=\"rc-image-tile-44\" src=\"photo.svg\" alt=\"\" style=\"top:'+(r*-100)+'%;left:'+(c*-100)+'%\">'+"
         "'</div><div class=\"rc-imageselect-checkbox\"></div></div>';"
         "td.addEventListener('click',function(){var on=!this.classList.contains('rc-imageselect-tileselected');"
         "this.classList.toggle('rc-imageselect-tileselected');"
         "this.querySelector('.rc-imageselect-checkbox').style.display=on?'':'none';log.push(this.id);"
         "parent.postMessage(log.join(),'http://site.test')});"
         "tr.appendChild(td)}tb.appendChild(tr)}</script>"},
        /* tile 0 red, tile 5 blue, tile 15 green, the rest grey */
        {"http://challenge.test/photo.svg",
         "<svg xmlns='http://www.w3.org/2000/svg' width='160' height='160' viewBox='0 0 4 4'>"
         "<rect width='4' height='4' fill='#808080'/><rect width='1' height='1' fill='#ff0000'/>"
         "<rect x='1' y='1' width='1' height='1' fill='#0000ff'/><rect x='3' y='3' width='1' height='1' "
         "fill='#008000'/></svg>",
         "image/svg+xml"},
        {"http://challenge.test/check.svg",
         "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' height='10' "
         "fill='#ff00ff'/></svg>",
         "image/svg+xml"},
        {0}};
    World w;
    world_open(&w, site, "http://site.test/", 300, 300);
    /* The frame's content starts at (21, 11); tile (r, c) has its centre
     * at (41 + 42c, 31 + 42r). */
    AuSurface *s = shot(&w, 300, 300, 0, 0);
    pump(&w);
    au_surface_free(s);
    s = shot(&w, 300, 300, 0, 0);
    CHECK(au_surface_get(s, 41, 31) == RED, "tile 0 shows its part of the photo: %08x", au_surface_get(s, 41, 31));
    CHECK(au_surface_get(s, 83, 73) == BLUE, "tile 5: %08x", au_surface_get(s, 83, 73));
    CHECK(au_surface_get(s, 167, 157) == GREEN, "tile 15: %08x", au_surface_get(s, 167, 157));
    au_surface_free(s);
    click(&w, 41, 31);
    click(&w, 83, 73);
    click(&w, 167, 157);
    JS_IS(&w, "picked", "0,5,15");
    /* The check marks: asked for by the first paint that shows them. */
    s = shot(&w, 300, 300, 0, 0);
    pump(&w);
    au_surface_free(s);
    s = shot(&w, 300, 300, 0, 0);
    uint32_t magenta = AU_RGB(255, 0, 255);
    CHECK(au_surface_get(s, 23, 13) == magenta, "tile 0's check mark: %08x", au_surface_get(s, 23, 13));
    CHECK(au_surface_get(s, 65, 55) == magenta, "tile 5's check mark: %08x", au_surface_get(s, 65, 55));
    CHECK(au_surface_get(s, 65, 13) != magenta, "an unselected tile has none: %08x", au_surface_get(s, 65, 13));
    /* a selected tile's picture is scaled to 32px about its centre */
    uint32_t mid = au_surface_get(s, 41, 31);
    CHECK(((mid >> 16) & 0xff) > 0xf0 && ((mid >> 8) & 0xff) < 0x10 && (mid & 0xff) < 0x10 &&
              au_surface_get(s, 21 + 2, 11 + 38) == WHITE,
          "tile 0 is drawn smaller: %08x %08x", mid, au_surface_get(s, 23, 49));
    au_surface_free(s);
    /* A click in a selected tile's margin, outside its scaled picture,
     * still reaches its cell; the next tile's picture does not take it. */
    click(&w, 21 + 38, 11 + 20);
    click(&w, 83, 73);
    JS_IS(&w, "picked", "0,5,15,0,5");
    CHECK(strstr(au_buf_cstr(&w.console), "Uncaught") == NULL, "script errors: %s", au_buf_cstr(&w.console));
    world_close(&w);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_paint();
    test_input();
    test_script();
    test_sandbox();
    test_blocking();
    test_lazy_object();
    test_lifetime();
    test_reload_loop();
    test_kept();
    test_widget();
    test_image_tiles();
    au_fontset_free(fonts);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
