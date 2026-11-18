/* Scrolling by moving pixels: after any sequence of scrolls, a window
 * kept up to date through AuPresent (pixels moved, exposed strips and
 * other damage painted) matches a fresh paint of the whole window, pixel
 * for pixel. Fixed and sticky boxes are painted again where they were and
 * where they are; a canvas gradient takes the full repaint. Runs with the bundled fonts only, so renders are the same on
 * every machine. */
#include "browser/browser.h"
#include "browser/present.h"
#include "platform/posix/posix.h"
#include "png.h"
#include "browser/url.h"

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

static uint32_t fake_ms;
static AuPresent pres;
static int shifts; /* scroll_area calls */

static uint32_t t_ms(void *ctx) { (void)ctx; return fake_ms; }
static AuTime t_now(void *ctx) { (void)ctx; return 1763100000; }
static void t_inval(void *ctx, AuBrowserWindow *w, AuIRect r) { (void)ctx, (void)w; au_present_invalidate(&pres, r); }
static void t_changed(void *ctx, AuBrowserWindow *w, unsigned what) { (void)ctx, (void)w, (void)what; }
static void t_close(void *ctx, AuBrowserWindow *w) { (void)ctx, (void)w; }
static void t_scroll(void *ctx, AuBrowserWindow *w, AuIRect area, int dx, int dy)
{
    (void)ctx, (void)w;
    shifts++;
    au_present_scroll(&pres, area, dx, dy);
}

static const char *inline_pages[][2] = {
    {"file:///fixed.html",
     "<!doctype html><style>body{margin:0;font:16px sans-serif}"
     "header{position:fixed;top:0;left:0;right:0;height:40px;background:#c33;color:#fff}"
     "p{margin:0 0 20px;height:60px;background:#eef}</style><header>Fixed header</header>"},
    {"file:///sticky.html",
     "<!doctype html><style>body{margin:0;font:16px serif}"
     "h2{position:sticky;top:0;background:#3c3;margin:0}p{height:300px;margin:0}</style>"},
    {"file:///canvasgrad.html",
     "<!doctype html><style>html{background:linear-gradient(#fff,#00f)}"
     "p{height:200px}</style>"},
    {"file:///plain.html",
     "<!doctype html><style>body{font:14px serif}div.b{border:3px dashed #844;margin:8px;padding:6px;"
     "box-shadow:4px 4px 6px rgba(0,0,0,.5);border-radius:9px}span{outline:2px solid #08f}"
     "pre{background:#eee;overflow:auto;width:200px}table{border-collapse:collapse}td{border:1px solid}"
     "</style>"},
    /* A header moved out of view by a transform (as Wikipedia's sticky
     * header), a fixed button with a positioned badge and a shadow, and
     * a sticky heading inside a scroller (it follows the scroller). */
    {"file:///fixed2.html",
     "<!doctype html><style>body{margin:0;font:15px sans-serif}"
     "#hdr{position:fixed;top:0;left:0;right:0;height:50px;background:#36c;transform:translateY(-100%);"
     "visibility:hidden}"
     "#btn{position:fixed;right:20px;bottom:30px;width:60px;height:30px;background:#fc0;"
     "box-shadow:0 0 8px #000;border-radius:6px}"
     "#btn i{position:absolute;left:-12px;top:-12px;width:16px;height:16px;border-radius:8px;background:red}"
     "#sc{height:120px;overflow:auto;border:1px solid}#sc h3{position:sticky;top:0;background:#cfc;margin:0}"
     "p{margin:6px}</style><div id=hdr>Hidden</div><div id=btn><i></i>Top <b style='display:inline-block;transform:rotate(30deg)'>&gt;</b></div>"
     "<div id=sc><h3>Inner</h3><p>a<p>b<p>c<p>d<p>e<p>f<p>g</div>"},
    /* Nested documents move with the page as boxes: one with a fixed
     * header of its own (fixed to the frame, not the window), one
     * scrolled, and one fixed to the window (painted again). */
    {"file:///frames.html",
     "<!doctype html><style>body{margin:0;font:15px sans-serif}iframe{display:block;margin:10px;width:300px;"
     "height:120px}#pin{position:fixed;right:10px;top:60px;width:120px;height:60px;margin:0}</style>"
     "<iframe srcdoc=\"<style>body{margin:0}h4{position:fixed;top:0;left:0;right:0;margin:0;background:#fc9}"
     "p{margin:30px 0}</style><h4>Fixed in the frame</h4><p>one<p>two<p>three<p>four\"></iframe>"
     "<iframe srcdoc=\"<body style='background:#def'><p>alpha<p>beta<p>gamma<p>delta<p>epsilon\"></iframe>"
     "<iframe id=pin srcdoc=\"<body style='background:#fed;margin:2px'>pinned\"></iframe>"},
    /* Reddit's toasts: a fixed container of no height at the top holding
     * a fixed box at the bottom. Nothing between them paints, so a step
     * must not repaint the view between them. */
    {"file:///toasts.html",
     "<!doctype html><style>body{margin:0;font:15px sans-serif}"
     "#ctl{position:fixed;top:40px;left:0;right:0;height:0}"
     "#ctl i{position:fixed;left:200px;bottom:20px;width:80px;height:24px;background:#c6f}"
     "p{margin:0 0 20px;height:60px;background:#eef}</style><div id=ctl><i></i></div>"},
    /* Transformed boxes are painted through a layer the size of the part
     * a strip can show: strips across rotated, scaled, skewed and
     * translucent boxes must match a whole paint (strips_match). */
    {"file:///transforms.html",
     "<!doctype html><style>body{margin:0;font:15px sans-serif}"
     "div{margin:30px 60px;height:90px;background:linear-gradient(90deg,#f80,#08f);border:3px solid #333;"
     "border-radius:12px;padding:8px}"
     "div:nth-child(3n){transform:rotate(12deg)}div:nth-child(3n+1){transform:scale(1.3,.8) skewX(10deg);"
     "opacity:.7}div:nth-child(3n+2){transform:rotate(-30deg) translateX(40px)}</style>"},
};

/* The inline pages' bodies are built from a head and repeated content. */
static void build_inline(int i, AuBuf *out)
{
    au_buf_puts(out, inline_pages[i][1]);
    for (int k = 0; k < 40; k++) {
        switch (i) {
        case 1: au_buf_printf(out, "<section><h2>Section %d</h2><p>Text of section %d.</p></section>", k, k); break;
        case 2: au_buf_printf(out, "<p>Paragraph %d over a gradient.</p>", k); break;
        case 7:
            au_buf_printf(out, "<div>Box %d, transformed, with text across it</div>", k);
            /* Scaled images are drawn without a layer (paint_scaled_image). */
            if (k % 4 == 0 || k % 4 == 2)
                au_buf_printf(out,
                              "<img src='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFUlEQV"
                              "R4nGP4z8DAsACIGf7/usMAABtDBHXYA4fpAAAAAElFTkSuQmCC' style='display:block;width:120px;"
                              "height:60px;margin:20px 80px;transform:%s'>",
                              k % 4 ? "scale(1.5,.8) translate(10px,5px);opacity:.6" : "scale(1.2)");
            break;
        case 3:
            au_buf_printf(out,
                          "<div class=b>Box %d with <span>outlined text</span> and a shadow. "
                          "<pre>wide preformatted line %d ......................................</pre>"
                          "<table><tr><td>a%d<td>b</table></div>",
                          k, k, k);
            break;
        default: au_buf_printf(out, "<p>Paragraph %d under a fixed header.</p>", k); break;
        }
    }
}

static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    for (int i = 0; i < (int)(sizeof inline_pages / sizeof inline_pages[0]); i++) {
        if (!strcmp(url, inline_pages[i][0])) {
            AuBuf b;
            au_buf_init(&b);
            build_inline(i, &b);
            *len = b.len;
            *data = au_strndup(b.data, b.len);
            au_buf_free(&b);
            return true;
        }
    }
    char *path = au_url_to_path(url, '/');
    if (!path)
        return false;
    bool ok = au_platform_read_file(path, data, len);
    au_free(path);
    return ok;
}

static void settle(AuBrowserWindow *w)
{
    for (int i = 0; i < 400 && au_browser_busy(w->browser); i++) {
        fake_ms += 50;
        au_bw_idle(w);
    }
}

/* Paints what is pending into s, as the Mac shell's flush does. */
static long painted; /* pixels painted by flush */

static void flush(AuBrowserWindow *w, AuSurface *s)
{
    for (int round = 0; round < 8 && au_present_pending(&pres); round++) {
        AuPresentRound r;
        au_present_begin(&pres, s, &r);
        if (getenv("SHIFT_TRACE")) {
            fprintf(stderr, "round %d: shift %d (%d,%d)", round, r.shifted, r.dx, r.dy);
            for (int i = 0; i < r.n; i++)
                fprintf(stderr, " [%d,%d %d,%d]", r.rects[i].x0, r.rects[i].y0, r.rects[i].x1, r.rects[i].y1);
            fprintf(stderr, "\n");
        }
        AuCanvas *c = au_canvas_new(s);
        for (int i = 0; i < r.n; i++) {
            au_bw_paint(w, c, r.rects[i]);
            painted += (long)(r.rects[i].x1 - r.rects[i].x0) * (r.rects[i].y1 - r.rects[i].y0);
        }
        au_canvas_free(c);
    }
}

static void compare(AuBrowserWindow *w, AuSurface *s, const char *what)
{
    AuSurface *ref = au_surface_new(s->width, s->height);
    AuCanvas *c = au_canvas_new(ref);
    au_bw_paint(w, c, (AuIRect){0, 0, s->width, s->height});
    au_canvas_free(c);
    flush(w, s); /* anything the reference paint invalidated (images decoded) */
    if (au_present_pending(&pres))
        flush(w, s);
    /* Pixels must match exactly, with one exception: a channel off by one
     * where floating point rounds differently at another canvas position
     * (a rotated SVG gradient does), on a handful of pixels. A fresh paint
     * at the new scroll position differs from one at the old position in
     * the same way. */
    long bad = 0, near = 0;
    int bx = -1, by = -1;
    for (int y = 0; y < s->height; y++)
        for (int x = 0; x < s->width; x++) {
            AuPixel a = s->pixels[(size_t)y * (size_t)s->stride + (size_t)x];
            AuPixel b = ref->pixels[(size_t)y * (size_t)ref->stride + (size_t)x];
            if (a == b)
                continue;
            bool one = true;
            for (int sh = 0; sh < 32; sh += 8) {
                int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
                if (d < -1 || d > 1)
                    one = false;
            }
            if (one && near < 64) {
                near++;
                continue;
            }
            if (!bad) {
                bx = x;
                by = y;
            }
            bad++;
        }
    if (bad) {
        AuTab *t = au_bw_active_tab(w);
        fprintf(stderr, "%s: %ld pixels differ after %s (first at %d,%d; scroll %d,%d)\n", au_page_url(t->page), bad,
                what, bx, by, t->scroll_x, t->scroll_y);
        const char *dump = getenv("SHIFT_DUMP"); /* a folder for got.png and want.png */
        if (dump) {
            char path[1024];
            snprintf(path, sizeof path, "%s/got.png", dump);
            au_png_write(path, s);
            snprintf(path, sizeof path, "%s/want.png", dump);
            au_png_write(path, ref);
        }
        exit(1);
    }
    au_surface_free(ref);
}

/* expect_shift: 1 the page must move pixels, 0 it must not, -1 either. */
static void run_page(AuFontSet *fonts, const char *url, int w, int h, int expect_shift)
{
    AuBrowserHost host = {.time_ms = t_ms, .now = t_now, .read_file_url = t_read, .invalidate = t_inval,
                          .changed = t_changed, .close_window = t_close, .scroll_area = t_scroll};
    AuBrowser *b = au_browser_new(fonts, &host);
    au_present_init(&pres);
    AuBrowserWindow *bw = au_browser_window_new(b, w, h);
    bw->native_standins = true;
    au_bw_new_tab(bw, url, true, 0);
    settle(bw);
    AuSurface *s = au_surface_new(w, h);
    au_present_invalidate(&pres, (AuIRect){0, 0, w, h});
    flush(bw, s);
    compare(bw, s, "loading");
    shifts = 0;
    AuTab *t = au_bw_active_tab(bw);
    int mx, my;
    au_bw_scroll_max(bw, &mx, &my);
    if (my <= 100) {
        printf("%s at %dx%d: scroll range %d, skipped\n", url, w, h, my);
        CHECK(!strstr(url, "wiki"));
        au_surface_free(s);
        au_browser_free(b);
        return;
    }
    static const struct {
        int dx, dy, flush;
    } steps[] = {{0, 32, 1},  {0, 32, 1},   {0, 16, 0},  {0, 16, 1},  {0, 500, 1}, {0, -40, 1}, {0, 7, 0},
                 {0, -3, 1},  {0, 2000, 1}, {0, -32, 1}, {0, 32, 0},  {0, 32, 0},  {0, 32, 1}, {20, 0, 1},
                 {-10, 5, 1}, {0, -1, 1}};
    char what[64];
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        painted = 0;
        au_bw_scroll_by(bw, steps[i].dx, steps[i].dy);
        if (i == 5) {
            /* Damage between scrolls: the pointer over the page. */
            AuEvent e;
            memset(&e, 0, sizeof e);
            e.type = AU_EVENT_MOUSE_MOVE;
            e.x = bw->layout.page.x0 + 50;
            e.y = bw->layout.page.y0 + 60;
            au_bw_mouse(bw, &e);
        }
        if (steps[i].flush) {
            flush(bw, s);
            if (i == 0 && strstr(url, "toasts")) {
                /* the 32-pixel strip, the box's two places, edges: far
                 * less than the view (it was the view twice over) */
                long view = (long)(bw->layout.page.x1 - bw->layout.page.x0) * (bw->layout.page.y1 - bw->layout.page.y0);
                if (painted > view / 4)
                    fprintf(stderr, "toasts: a 32-pixel step painted %ld of %ld pixels\n", painted, view);
                CHECK(painted <= view / 4);
            }
            snprintf(what, sizeof what, "step %d (by %d,%d)", (int)i, steps[i].dx, steps[i].dy);
            compare(bw, s, what);
        }
    }
    /* Keys: Page Down, End, Home. */
    AuEvent k;
    memset(&k, 0, sizeof k);
    k.type = AU_EVENT_KEY_DOWN;
    k.key = AU_KEY_PAGE_DOWN;
    au_bw_key(bw, &k);
    flush(bw, s);
    compare(bw, s, "Page Down");
    k.key = AU_KEY_END;
    au_bw_key(bw, &k);
    flush(bw, s);
    compare(bw, s, "End");
    k.key = AU_KEY_UP;
    au_bw_key(bw, &k);
    au_bw_key(bw, &k);
    flush(bw, s);
    compare(bw, s, "Up Up");
    /* Hover follows the pointer once the scrolling pauses. */
    CHECK(bw->rehover_pending);
    au_bw_idle(bw);
    CHECK(bw->rehover_pending);
    fake_ms += AU_REHOVER_MS;
    au_bw_idle(bw);
    CHECK(!bw->rehover_pending);
    flush(bw, s);
    compare(bw, s, "hover after scrolling");
    printf("%s at %dx%d: %d moves\n", url, w, h, shifts);
    if (expect_shift > 0)
        CHECK(shifts > 0);
    else if (expect_shift == 0)
        CHECK(shifts == 0);
    (void)t;
    au_surface_free(s);
    au_browser_free(b);
}

/* Painting the view in strips must give the pixels of one whole paint, at
 * the same scroll position (no shift, so no rounding of positions):
 * a transformed box is painted through a layer the size of the part a
 * strip shows, and that must not change what the strip gets. */
static void strips_match(AuFontSet *fonts, const char *url, int w, int h)
{
    AuBrowserHost host = {.time_ms = t_ms, .now = t_now, .read_file_url = t_read, .invalidate = t_inval,
                          .changed = t_changed, .close_window = t_close, .scroll_area = t_scroll};
    AuBrowser *b = au_browser_new(fonts, &host);
    au_present_init(&pres);
    AuBrowserWindow *bw = au_browser_window_new(b, w, h);
    bw->native_standins = true;
    au_bw_new_tab(bw, url, true, 0);
    settle(bw);
    AuSurface *whole = au_surface_new(w, h), *strips = au_surface_new(w, h);
    AuIRect page = {bw->layout.page.x0, bw->layout.page.y0, bw->layout.page.x1, bw->layout.page.y1};
    static const int scrolls[] = {0, 150, 700, 1600};
    static const int bands[] = {16, 7, 33};
    for (size_t si = 0; si < sizeof scrolls / sizeof scrolls[0]; si++) {
        au_bw_scroll_to(bw, 0, scrolls[si]);
        settle(bw);
        AuCanvas *c = au_canvas_new(whole);
        au_bw_paint(bw, c, page);
        au_canvas_free(c);
        for (size_t bi = 0; bi < sizeof bands / sizeof bands[0]; bi++) {
            c = au_canvas_new(strips);
            for (int y = page.y0; y < page.y1; y += bands[bi]) {
                int y1 = y + bands[bi] < page.y1 ? y + bands[bi] : page.y1;
                au_bw_paint(bw, c, (AuIRect){page.x0, y, page.x1, y1});
            }
            au_canvas_free(c);
            long off_by_one = 0, bad = 0;
            for (int y = page.y0; y < page.y1; y++)
                for (int x = page.x0; x < page.x1; x++) {
                    AuPixel p = whole->pixels[(size_t)y * (size_t)whole->stride + (size_t)x];
                    AuPixel q = strips->pixels[(size_t)y * (size_t)strips->stride + (size_t)x];
                    if (p == q)
                        continue;
                    int dmax = 0;
                    for (int sh = 0; sh < 32; sh += 8) {
                        int d = (int)((p >> sh) & 0xFF) - (int)((q >> sh) & 0xFF);
                        if (d < 0)
                            d = -d;
                        dmax = d > dmax ? d : dmax;
                    }
                    if (dmax <= 1)
                        off_by_one++;
                    else
                        bad++;
                }
            if (bad || off_by_one > 64)
                fprintf(stderr, "%s at scroll %d in %d-pixel strips: %ld pixels differ, %ld by one\n", url,
                        scrolls[si], bands[bi], bad, off_by_one);
            CHECK(!bad && off_by_one <= 64);
        }
    }
    printf("%s: strips match a whole paint\n", url);
    au_surface_free(whole);
    au_surface_free(strips);
    au_browser_free(b);
}

int main(void)
{
    AuFontSet *fonts = au_fontset_new();
    AuPosixFontDir *dir = au_posix_add_font_dir(fonts, AURA_FONTS_DIR, AU_FONT_BUNDLED);
    CHECK(dir && au_posix_font_dir_count(dir) > 0);

    /* The damage list on its own. */
    au_present_init(&pres);
    au_present_invalidate(&pres, (AuIRect){0, 0, 10, 10});
    au_present_invalidate(&pres, (AuIRect){2, 2, 5, 5});
    CHECK(pres.n == 1);
    au_present_invalidate(&pres, (AuIRect){0, 500, 100, 520});
    CHECK(pres.n == 2);
    au_present_scroll(&pres, (AuIRect){0, 0, 100, 400}, 0, -4);
    CHECK(pres.shifting && pres.shift_dy == -4);
    CHECK(pres.n == 2 && pres.rects[0].y0 == 0 && pres.rects[0].y1 == 6);
    au_present_scroll(&pres, (AuIRect){0, 0, 100, 400}, 0, -400);
    CHECK(!pres.shifting);
    for (int i = 0; i < 20; i++)
        au_present_invalidate(&pres, (AuIRect){i * 20, i * 30, i * 20 + 5, i * 30 + 5});
    CHECK(pres.n <= AU_PRESENT_RECTS);

    /* Shifting pixels. */
    AuSurface *s = au_surface_new(8, 6);
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 8; x++)
            s->pixels[y * s->stride + x] = (AuPixel)(y * 16 + x);
    au_present_shift_pixels(s, (AuIRect){1, 1, 7, 5}, 1, -2);
    CHECK(s->pixels[1 * s->stride + 2] == 3 * 16 + 1);
    CHECK(s->pixels[2 * s->stride + 6] == 4 * 16 + 5);
    CHECK(s->pixels[0 * s->stride + 2] == 2); /* outside the area */
    CHECK(s->pixels[1 * s->stride + 7] == 16 + 7);
    au_surface_free(s);

    char url[1024];
    static const char *samples[] = {"wiki.html",  "hn.html",       "hn-item.html", "blog.html",   "docs.html",
                                    "forum.html", "images.html",   "reddit.html",  "qa-flex.html", "qa-tables.html",
                                    "svg.html",   "wikitable.html", "grid.html",    "canvas.html"};
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        char *u;
        snprintf(url, sizeof url, "%s/tests/page/samples/%s", AURA_SOURCE_DIR, samples[i]);
        u = au_url_from_path(url, '/');
        int expect = strstr(samples[i], "wiki") || strstr(samples[i], "docs") ? 1 : -1;
        run_page(fonts, u, 640, 480, expect);
        run_page(fonts, u, 1024, 700, expect);
        au_free(u);
    }
    run_page(fonts, "file:///plain.html", 640, 480, 1);
    run_page(fonts, "file:///fixed.html", 640, 480, 1);
    run_page(fonts, "file:///fixed2.html", 640, 480, 1);
    run_page(fonts, "file:///sticky.html", 640, 480, 1);
    run_page(fonts, "file:///frames.html", 640, 480, 1);
    run_page(fonts, "file:///toasts.html", 640, 480, 1);
    strips_match(fonts, "file:///transforms.html", 640, 480);
    run_page(fonts, "file:///canvasgrad.html", 640, 480, 0);
    au_fontset_free(fonts);
    printf("scroll_shift: ok\n");
    return 0;
}
