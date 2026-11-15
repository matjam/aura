/* Canvas elements through the document view, with a loader that serves a
 * small table of pages over made-up http origins: painting into the
 * element's box, one repaint of the canvas alone per script task, no
 * layout for drawing, the memory estimate, the origin-clean rule for
 * images of other origins, fallback content with script off, a hidden
 * document. */
#include "host.h"
#include "image/pngenc.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                           \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            failures++;                                            \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                          \
            fputc('\n', stderr);                                   \
        }                                                          \
    } while (0)

static AuFontSet *fonts;

typedef struct Page {
    const char *url;
    const char *body;
    size_t len; /* 0: strlen(body) */
    const char *type;
} Page;

typedef struct Req {
    uint32_t id;
    char url[512];
    bool done, cancelled;
} Req;

typedef struct World {
    const Page *pages;
    Req req[200];
    int n;
    uint32_t next;
    AuDocView *dv;
    AuBuf console;
    AuIRect inval[64];
    int ninval;
    double clock;
} World;

static uint32_t w_request(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    (void)dv;
    (void)kind;
    World *w = ctx;
    if (w->n == 200)
        return 0;
    Req *r = &w->req[w->n++];
    memset(r, 0, sizeof *r);
    r->id = ++w->next;
    snprintf(r->url, sizeof r->url, "%s", url);
    return r->id;
}

static void w_cancel(void *ctx, uint32_t id)
{
    World *w = ctx;
    for (int i = 0; i < w->n; i++)
        if (w->req[i].id == id)
            w->req[i].cancelled = true;
}

static void w_console(void *ctx, AuDocView *dv, int level, const char *text, size_t len)
{
    (void)dv;
    (void)level;
    World *w = ctx;
    au_buf_append(&w->console, text, len);
    au_buf_putc(&w->console, '\n');
}

static void w_invalidate(void *ctx, AuDocView *dv, AuIRect r)
{
    (void)dv;
    World *w = ctx;
    if (w->ninval < 64)
        w->inval[w->ninval] = r;
    w->ninval++;
}

static double w_now(void *ctx, AuDocView *dv)
{
    (void)dv;
    World *w = ctx;
    return w->clock += 1;
}

static const Page *find_page(const World *w, const char *url)
{
    for (const Page *p = w->pages; p && p->url; p++)
        if (!strcmp(p->url, url))
            return p;
    return NULL;
}

static void pump(World *w)
{
    for (int round = 0; round < 200; round++) {
        bool any = false;
        for (int i = 0; i < w->n; i++) {
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
            const char *hdrs[2] = {"content-type", p->type ? p->type : "text/html"};
            au_docview_resource_head(w->dv, r->id, 200, "", NULL, hdrs, 1);
            au_docview_resource_data(w->dv, r->id, p->body, p->len ? p->len : strlen(p->body));
            au_docview_resource_done(w->dv, r->id, true);
        }
        if (au_docview_run_scripts(w->dv, 50))
            any = true;
        au_docview_update(w->dv);
        for (int i = 0; i < w->n; i++)
            if (!w->req[i].done && !w->req[i].cancelled)
                any = true;
        if (!any && au_docview_scripts_due(w->dv) > 1000)
            break;
        w->clock += 20;
    }
}

static void world_open(World *w, const Page *pages, const char *url, bool scripting)
{
    memset(w, 0, sizeof *w);
    w->pages = pages;
    au_buf_init(&w->console);
    AuLoader l = {w, w_request, w_cancel, NULL};
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = w;
    cb.console = w_console;
    cb.invalidate = w_invalidate;
    cb.now_ms = w_now;
    w->dv = au_docview_new(fonts, &l, &cb);
    au_docview_set_scripting(w->dv, scripting);
    au_docview_set_viewport(w->dv, 400, 300);
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

#define JS_IS(w, src, want)                                                      \
    do {                                                                         \
        const char *got_ = js(w, src);                                           \
        CHECK(!strcmp(got_, want), "%s = \"%s\", want \"%s\"", src, got_, want); \
    } while (0)

static AuSurface *shot(World *w)
{
    AuSurface *s = au_surface_new(400, 300);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(w->dv, c, (AuRect){0, 0, 400, 300}, 0, 0);
    au_canvas_free(c);
    return s;
}

#define GREEN AU_RGB(0, 128, 0)
#define WHITE AU_RGB(255, 255, 255)

/* ---- Painting, repaints, layout, memory ---- */

static const Page paint_site[] = {
    {"http://site.test/paint.html",
     "<!DOCTYPE html><body style='margin:0;background:#fff'>"
     "<div style='height:20px'></div>"
     "<canvas id=c width=100 height=50 style='display:block;margin-left:30px'>fallback</canvas>"
     "<canvas id=big width=10 height=10 style='display:block;width:100px;height:100px;margin-left:200px;image-rendering:pixelated'></canvas>"
     "<p id=after>after</p>"
     "<script>var ctx = document.getElementById('c').getContext('2d');"
     "ctx.fillStyle = 'green'; ctx.fillRect(10, 10, 20, 20);"
     "var b = document.getElementById('big').getContext('2d'); b.fillStyle = 'green'; b.fillRect(0, 0, 5, 10);"
     "</script></body>"},
    {NULL}};

static void test_paint(void)
{
    World w;
    world_open(&w, paint_site, "http://site.test/paint.html", true);
    AuSurface *s = shot(&w);
    /* the canvas box starts at (30, 20); the green square at (10, 10) in it */
    CHECK(au_surface_get(s, 45, 35) == GREEN, "the drawn square shows in the page: %08x", (unsigned)au_surface_get(s, 45, 35));
    CHECK(au_surface_get(s, 35, 25) == WHITE, "the rest of the canvas is transparent over the page");
    CHECK(au_surface_get(s, 62, 35) == WHITE, "right of the square");
    /* a 10 x 10 bitmap shown at 100 x 100: the left half green */
    CHECK(au_surface_get(s, 210, 80) == GREEN && au_surface_get(s, 248, 160) == GREEN, "scaled by CSS");
    CHECK(au_surface_get(s, 252, 80) == WHITE, "scaled, the right half (pixelated: a hard edge)");
    au_surface_free(s);
    JS_IS(&w, "document.getElementById('after').getBoundingClientRect().top", "186");

    /* Many draws in one script task: one repaint, of the canvas only. */
    AuDocViewStats st0, st1;
    au_docview_stats(w.dv, &st0);
    w.ninval = 0;
    js(&w, "for (var i = 0; i < 200; i++) ctx.fillRect(40 + i % 20, 5, 3, 3); ctx.clearRect(0, 40, 5, 5); 1");
    au_docview_stats(w.dv, &st1);
    CHECK(w.ninval == 1, "one invalidation for 200 draws: %d", w.ninval);
    if (w.ninval >= 1) {
        AuIRect r = w.inval[0];
        CHECK(r.x0 >= 28 && r.y0 >= 18 && r.x1 <= 132 && r.y1 <= 72, "inside the canvas box: %d,%d %d,%d", r.x0, r.y0,
              r.x1, r.y1);
        CHECK(r.x0 <= 30 && r.x1 >= 93 && r.y0 <= 25 && r.y1 >= 65, "and covering what was drawn: %d,%d %d,%d", r.x0,
              r.y0, r.x1, r.y1);
    }
    CHECK(st1.tree_builds == st0.tree_builds && st1.layouts == st0.layouts && st1.style_passes == st0.style_passes,
          "drawing costs no style pass, tree build or layout (%u %u %u)", st1.style_passes - st0.style_passes,
          st1.tree_builds - st0.tree_builds, st1.layouts - st0.layouts);
    s = shot(&w);
    CHECK(au_surface_get(s, 30 + 45, 20 + 6) == GREEN, "the new drawing is painted");
    au_surface_free(s);
    /* a draw that changes nothing visible on the page still repaints only the canvas */
    w.ninval = 0;
    js(&w, "ctx.getImageData(0, 0, 10, 10); ctx.measureText('x'); ctx.save(); ctx.restore(); 1");
    CHECK(w.ninval == 0, "reading pixels invalidates nothing: %d", w.ninval);
    /* drawing on the scaled canvas: the rectangle is scaled too */
    w.ninval = 0;
    js(&w, "b.fillRect(8, 8, 1, 1); 1");
    CHECK(w.ninval == 1, "the scaled canvas: %d", w.ninval);
    if (w.ninval == 1)
        CHECK(w.inval[0].x0 >= 276 && w.inval[0].x0 <= 280 && w.inval[0].x1 >= 290 && w.inval[0].x1 <= 294 &&
                  w.inval[0].y0 >= 146 && w.inval[0].y1 <= 164,
              "one bitmap pixel is ten page pixels: %d,%d %d,%d", w.inval[0].x0, w.inval[0].y0, w.inval[0].x1,
              w.inval[0].y1);

    /* Clearing with width = width: no layout. A new size: layout. */
    au_docview_stats(w.dv, &st0);
    js(&w, "ctx.canvas.width = ctx.canvas.width; 1");
    au_docview_stats(w.dv, &st1);
    CHECK(st1.tree_builds == st0.tree_builds && st1.layouts == st0.layouts, "width = width builds no tree");
    s = shot(&w);
    CHECK(au_surface_get(s, 45, 35) == WHITE, "and the canvas is clear on the page");
    au_surface_free(s);
    js(&w, "ctx.canvas.height = 80; 1");
    au_docview_stats(w.dv, &st0);
    CHECK(st0.layouts > st1.layouts, "a new height lays out again");
    JS_IS(&w, "document.getElementById('after').getBoundingClientRect().top", "216");

    /* The memory estimate counts the bitmaps: lazily. */
    size_t m0 = au_docview_memory_estimate(w.dv);
    js(&w, "var m = document.createElement('canvas'); m.width = 500; m.height = 400; var mc = m.getContext('2d'); 1");
    size_t m1 = au_docview_memory_estimate(w.dv);
    js(&w, "mc.fillRect(0, 0, 1, 1); 1");
    size_t m2 = au_docview_memory_estimate(w.dv);
    CHECK(m1 < m0 + 200000, "a canvas without drawing holds no bitmap: %zu", m1 - m0);
    CHECK(m2 >= m1 + 500 * 400 * 4 && m2 < m1 + 500 * 400 * 4 + 200000, "the first draw allocates it: %zu", m2 - m1);
    /* A hidden document keeps its canvases, and they count. */
    js(&w, "ctx.fillStyle = 'green'; ctx.fillRect(10, 10, 20, 20); 1");
    au_docview_set_hidden(w.dv, true);
    CHECK(au_docview_memory_estimate(w.dv) >= m1 + 500 * 400 * 4, "a hidden document still counts its canvases");
    au_docview_set_hidden(w.dv, false);
    pump(&w);
    s = shot(&w);
    CHECK(au_surface_get(s, 45, 35) == GREEN, "and shows them again");
    au_surface_free(s);
    world_close(&w);

    /* With script off the fallback content is laid out. */
    world_open(&w, paint_site, "http://site.test/paint.html", false);
    AuElement *c = au_docview_find_fragment(w.dv, "c");
    AuRect r;
    CHECK(c && au_layout_element_rect(au_docview_layout(w.dv), c, &r) && r.h > 5 && r.h < 40,
          "script off: the canvas is as tall as its fallback text");
    AuBuf dump;
    au_buf_init(&dump);
    au_layout_dump(au_docview_layout(w.dv), &dump);
    CHECK(strstr(au_buf_cstr(&dump), "fallback") != NULL, "and the text has a box");
    au_buf_free(&dump);
    world_close(&w);
}

/* ---- The origin-clean flag ---- */

static AuBuf png;

static void png_row(void *ctx, int y, uint8_t *rgba)
{
    (void)ctx;
    (void)y;
    static const uint8_t row[16] = {255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    memcpy(rgba, row, 16);
}

static const char taint_html[] =
    "<!DOCTYPE html><body>"
    "<img id=same src='/img.png'><img id=cross src='http://other.test/img.png'>"
    "<script>"
    "function mk() { var c = document.createElement('canvas'); c.width = c.height = 8; return c.getContext('2d'); }"
    "function load(url) { return new Promise(function (ok, bad) { var i = new Image(); i.onload = function () { ok(i); };"
    "  i.onerror = function () { bad(new Error('load ' + url)); }; i.src = url; }); }"
    /* what reading a context back gives: 'ok', or the exception's name */
    "function read(c) { try { c.getImageData(0, 0, 1, 1); return 'ok'; } catch (e) { return e.name; } }"
    "function url(c) { try { return c.canvas.toDataURL().slice(0, 14); } catch (e) { return e.name; } }"
    "function blob(c) { try { c.canvas.toBlob(function () {}); return 'ok'; } catch (e) { return e.name; } }"
    "var out = {}, imgs = {};"
    "Promise.all([load('/img.png'), load('http://other.test/img.png'), load('http://site.test:8080/img.png')])"
    ".then(function (l) { imgs.same = l[0]; imgs.cross = l[1]; imgs.port = l[2];"
    "  var d = mk(); d.drawImage(imgs.same, 0, 0); imgs.data = d.canvas.toDataURL(); return load(imgs.data); })"
    ".then(function (i) { imgs.dataImg = i; out.ready = 1; }, function (e) { out.error = String(e); });"
    "</script></body>";

static void test_taint(void)
{
    au_buf_init(&png);
    au_png_encode_rgba(4, 4, png_row, NULL, &png);
    Page site[] = {{"http://site.test/taint.html", taint_html, 0, NULL},
                   {"http://site.test/img.png", png.data, png.len, "image/png"},
                   {"http://other.test/img.png", png.data, png.len, "image/png"},
                   {"http://site.test:8080/img.png", png.data, png.len, "image/png"},
                   {NULL}};
    World w;
    world_open(&w, site, "http://site.test/taint.html", true);
    JS_IS(&w, "out.ready + '|' + out.error", "1|undefined");
    /* same origin, and a data: URL: readable */
    JS_IS(&w, "var a = mk(); a.drawImage(imgs.same, 0, 0); read(a) + ' ' + url(a) + ' ' + blob(a)", "ok data:image/png ok");
    JS_IS(&w, "a.getImageData(0, 0, 1, 1).data[0]", "255");
    JS_IS(&w, "var d = mk(); d.drawImage(imgs.dataImg, 0, 0); read(d)", "ok");
    /* another host, and another port: tainted */
    JS_IS(&w, "var x = mk(); x.fillRect(0, 0, 1, 1); read(x)", "ok");
    JS_IS(&w, "x.drawImage(imgs.cross, 0, 0); read(x) + ' ' + url(x) + ' ' + blob(x)",
          "SecurityError SecurityError SecurityError");
    JS_IS(&w, "var p = mk(); p.drawImage(imgs.port, 0, 0); read(p)", "SecurityError");
    /* the image did draw */
    JS_IS(&w, "var probe = mk(); probe.drawImage(x.canvas, 0, 0); read(probe)", "SecurityError");
    /* a tainted canvas taints what it is drawn to; a clean one does not */
    JS_IS(&w, "var y = mk(); y.drawImage(a.canvas, 0, 0); read(y)", "ok");
    JS_IS(&w, "y.drawImage(x.canvas, 0, 0); read(y)", "SecurityError");
    /* drawing nothing of it still taints (the standard checks the source, not the pixels) */
    JS_IS(&w, "var z = mk(); z.drawImage(imgs.cross, 100, 100); read(z)", "SecurityError");
    /* patterns and bitmaps carry the flag */
    JS_IS(&w, "var q = mk(); q.fillStyle = q.createPattern(imgs.cross, 'repeat'); q.fillRect(0, 0, 8, 8); read(q)", "SecurityError");
    JS_IS(&w, "var q2 = mk(); q2.fillStyle = q2.createPattern(imgs.same, 'repeat'); q2.fillRect(0, 0, 8, 8); read(q2)", "ok");
    JS_IS(&w, "var q3 = mk(); q3.fillStyle = q3.createPattern(x.canvas, 'repeat'); q3.fillRect(0, 0, 8, 8); read(q3)", "SecurityError");
    js(&w, "var bres = ''; createImageBitmap(imgs.cross).then(function (b) { var c = mk(); c.drawImage(b, 0, 0); bres = read(c); }); 1");
    JS_IS(&w, "bres", "SecurityError");
    js(&w, "bres = ''; createImageBitmap(imgs.same).then(function (b) { var c = mk(); c.drawImage(b, 0, 0); bres = read(c); }); 1");
    JS_IS(&w, "bres", "ok");
    /* images of the document */
    JS_IS(&w, "var e = mk(); e.drawImage(document.getElementById('same'), 0, 0); read(e)", "ok");
    JS_IS(&w, "e.getImageData(0, 0, 1, 1).data[0] + ',' + e.getImageData(3, 0, 1, 1).data[2]", "255,255");
    JS_IS(&w, "var f = mk(); f.drawImage(document.getElementById('cross'), 0, 0); read(f)", "SecurityError");
    /* a tainted canvas still draws and measures; resetting its bitmap makes it clean */
    JS_IS(&w, "x.fillRect(0, 0, 2, 2); x.measureText('a').width >= 0", "true");
    JS_IS(&w, "x.canvas.width = 8; read(x)", "ok");
    JS_IS(&w, "f.reset(); read(f)", "SecurityError");
    world_close(&w);
    au_buf_free(&png);
}

int main(void)
{
    fonts = au_fontset_new();
    au_host_use_system_fonts(false);
    au_platform_load_fonts(fonts);
    test_paint();
    test_taint();
    au_fontset_free(fonts);
    printf("canvas page: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
