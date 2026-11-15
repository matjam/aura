/* aura-host: the headless development tool.
 *
 *   aura-host scene [-o out.png] [-w width] [-h height] [-s scroll]
 *                   [--font file.ttf]... [--no-system-fonts]
 *       Draws the paint test scene and writes it as a PNG.
 *   aura-host fonts [--font file.ttf]...
 *       Lists the registered font faces and the generic families.
 *   aura-host font-index DIR [-o FILE]
 *       Writes the index of a Fonts folder (DIR/fonts.idx).
 *   aura-host bench [--font file.ttf]...
 *       Times the rasterizer, compositing and text.
 *   aura-host chrome [-o out.png] [-w W] [-h H] [--tabs top|left] [--tab URL]...
 *       Renders a browser window (chrome and stub page) as a PNG. See
 *       usage() for the options. */
#include "host.h"
#include "fontdir.h"
#include "png.h"
#include "browser/browser.h"
#include "browser/present.h"
#include "browser/scene.h"
#include "browser/url.h"
#include "render.h"
#include "net_host.h"
#include "net/cache.h"
#include "net/loader.h"
#include "net/net_prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: aura-host scene [-o out.png] [-w W] [-h H] [-s scroll] [font options]\n"
            "       aura-host fonts [font options]\n"
            "       aura-host bench [font options]\n"
            "       aura-host font-index DIR [-o FILE]   (writes DIR/fonts.idx)\n"
            "       font options: --font F, --no-system-fonts (or --bundled-only), --no-bundled-fonts,\n"
            "                     --fonts-dir D (default assets/fonts or $AURA_FONTS)\n"
            "       aura-host chrome [-o out.png] [-w W] [-h H] [--tabs top|left] [--column W]\n"
            "                        [--tab URL]... [--more-tabs N] [--active N] [--field TEXT] [--focus]\n"
            "                        [--loading] [--hover back|forward|reload|home|bookmark|newtab|splitter]\n"
            "                        [--scroll Y] [--mouse X Y]\n"
            "                        [--net] [--scroll-bench STEPS [--no-shift]] [--do ACTION]...\n"
            "                        (--do: click:X,Y dclick:X,Y tclick:X,Y clickname:NAME type:TEXT\n"
            "                        key:[cmd-][shift-]NAME\n"
            "                        edit:copy|cut|paste:TEXT|undo|clear|selectall wait:MS shot:FILE field\n"
            "                        go:URL back forward cache (the HTTP cache's counts with --net)\n"
            "                        js:CODE (evaluate in the page) save:FILE (the page source) console)\n"
            "                        (--tab takes a URL, a path, or file:relative/path; --net loads http and\n"
            "                        https over the network, else they show a stand-in; --scroll-bench times\n"
            "                        32-pixel steps and page downs through the Mac shell's paint path,\n"
            "                        repainting the page for each, then moving pixels; --no-shift only\n"
            "                        the first)\n"
            "       aura-host render page.html -w W [-h H] [-s scrollY] -o out.png\n"
            "       aura-host dump page.html -w W\n"
            "       aura-host image FILE [-o out.png] [--scale N] [--target WxH] [--chunk N] [--frames]\n"
            "                        [--repeat N] [--max-bytes N] [--favicon SIZE]\n"
            "       aura-host js [--html PAGE.html] [--quiet] [--mem MB] [--kill MS] FILE.js...\n"
            "       aura-host page [--quiet] [--expect-errors N] [--click ID]... PAGE.html\n");
    return 2;
}

static int cmd_scene(AuFontSet *fonts, const char *out, int w, int h, int scroll)
{
    AuSurface *s = au_surface_new(w, h);
    au_host_set_window_surface(s);
    AuCanvas *c = au_canvas_new(s);
    double t0 = now_ms();
    au_scene_draw(c, fonts, w, h, scroll);
    double t1 = now_ms();
    au_platform_present(0, 0, w, h);
    if (!au_png_write(out, s)) {
        fprintf(stderr, "aura-host: cannot write %s\n", out);
        return 1;
    }
    printf("%s: %dx%d, drawn in %.2f ms (cold glyph cache)\n", out, w, h, t1 - t0);
    au_canvas_free(c);
    au_surface_free(s);
    return 0;
}

/* aura-host font-index DIR [-o FILE]: writes the Fonts folder index
 * (default DIR/fonts.idx). */
static int cmd_font_index(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    const char *dir = argv[2];
    char def[2048];
    snprintf(def, sizeof def, "%s/fonts.idx", dir);
    const char *out = def;
    if (argc >= 5 && !strcmp(argv[3], "-o"))
        out = argv[4];
    AuBuf b;
    au_buf_init(&b);
    if (!au_host_font_index(dir, &b)) {
        fprintf(stderr, "aura-host: no usable fonts in %s\n", dir);
        au_buf_free(&b);
        return 1;
    }
    FILE *f = !strcmp(out, "-") ? stdout : fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "aura-host: cannot write %s\n", out);
        au_buf_free(&b);
        return 1;
    }
    fwrite(b.data, 1, b.len, f);
    if (f != stdout)
        fclose(f);
    au_buf_free(&b);
    return 0;
}

static int cmd_fonts(AuFontSet *fonts)
{
    for (size_t i = 0; i < au_fontset_count(fonts); i++) {
        int weight;
        bool italic;
        const char *fam = au_fontset_entry(fonts, i, &weight, &italic);
        printf("%-28s %3d %s%s\n", fam, weight, italic ? "italic" : "normal",
               (au_fontset_entry_flags(fonts, i) & AU_FONT_BUNDLED) ? "  (bundled)" : "");
    }
    static const char *gen[] = {"serif", "sans-serif", "monospace", "system-ui"};
    for (int g = 0; g < 4; g++) {
        AuFontFace *f = au_fontset_match(fonts, gen[g], 400, false);
        printf("%-12s -> %s\n", gen[g], f ? f->full_name : "(none)");
    }
    return 0;
}

static const char paragraph[] =
    "Aura is a web browser engine for Mac OS 9. It parses HTML and CSS, lays out boxes and paints "
    "them with its own rasterizer and TrueType renderer, so it runs on a 300 MHz G3. ";

static int cmd_bench(AuFontSet *fonts)
{
    AuSurface *s = au_surface_new(640, 720);
    AuCanvas *c = au_canvas_new(s);
    AuFontFace *sans = au_fontset_match(fonts, "sans-serif", 400, false);
    double t0, t;
    int n;

    /* Scene, warm glyph cache. */
    au_scene_draw(c, fonts, 640, 720, 0);
    n = 50;
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_scene_draw(c, fonts, 640, 720, 0);
    t = (now_ms() - t0) / n;
    printf("scene 640x720, warm cache      %8.3f ms\n", t);

    n = 20;
    t0 = now_ms();
    for (int i = 0; i < n; i++) {
        au_glyph_cache_clear();
        au_scene_draw(c, fonts, 640, 720, 0);
    }
    t = (now_ms() - t0) / n;
    printf("scene 640x720, cold cache      %8.3f ms\n", t);

    /* Rectangles. */
    n = 200;
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_rect(c, (AuRect){0, 0, 640, 480}, AU_RGB(10, 20, 30));
    t = (now_ms() - t0) / n;
    printf("fill 640x480 opaque            %8.3f ms  (%.0f Mpx/s)\n", t, 640 * 480 / t / 1000);
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_rect(c, (AuRect){0.5f, 0.5f, 640, 480}, AU_RGBA(10, 20, 30, 128));
    t = (now_ms() - t0) / n;
    printf("fill 640x480 alpha, fractional %8.3f ms  (%.0f Mpx/s)\n", t, 640 * 480 / t / 1000);

    /* Paths. */
    AuPath p;
    au_path_init(&p);
    au_path_ellipse(&p, 320, 300, 200, 200);
    n = 200;
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGBA(200, 30, 30, 200));
    t = (now_ms() - t0) / n;
    printf("fill circle r=200              %8.3f ms  (%.0f Mpx/s)\n", t, 3.14159 * 200 * 200 / t / 1000);
    au_path_clear(&p);
    AuRadii r = {{20, 20, 20, 20}, {20, 20, 20, 20}};
    AuBorder b = {{4, 4, 4, 4}, {AU_RGB(200, 0, 0), AU_RGB(0, 200, 0), AU_RGB(0, 0, 200), AU_RGB(100, 100, 0)}};
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_draw_border(c, (AuRect){20, 20, 300, 200}, &b, &r);
    t = (now_ms() - t0) / n;
    printf("border 300x200 r=20, 4 colours %8.3f ms\n", t);
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_draw_box_shadow(c, (AuRect){40, 40, 300, 200}, &r, 4, 4, 16, 0, AU_RGBA(0, 0, 0, 100), false);
    t = (now_ms() - t0) / n;
    printf("box-shadow 300x200 blur 16     %8.3f ms\n", t);
    AuGradient g = {0};
    au_gradient_css_line(&g, (AuRect){0, 0, 640, 480}, 45);
    au_gradient_add_stop(&g, 0, AU_RGB(255, 0, 0));
    au_gradient_add_stop(&g, 1, AU_RGB(0, 0, 255));
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_rect_gradient(c, (AuRect){0, 0, 640, 480}, &g);
    t = (now_ms() - t0) / n;
    printf("gradient 640x480               %8.3f ms\n", t);

    /* Text. */
    if (sans) {
        size_t len = strlen(paragraph);
        size_t glyphs = 0;
        for (size_t i = 0; i < len; i++)
            glyphs += (paragraph[i] & 0xC0) != 0x80;
        n = 200;
        au_draw_text(c, sans, 13, 0, 100, paragraph, len, AU_RGB(0, 0, 0));
        t0 = now_ms();
        for (int i = 0; i < n; i++)
            au_draw_text(c, sans, 13, 0, 100 + (i % 40) * 16, paragraph, len, AU_RGB(0, 0, 0));
        t = (now_ms() - t0) / n;
        printf("text 13px, cached              %8.3f ms per %zu glyphs (%.2f us/glyph)\n", t, glyphs,
               t * 1000 / glyphs);
        n = 50;
        t0 = now_ms();
        for (int i = 0; i < n; i++) {
            au_glyph_cache_clear();
            au_draw_text(c, sans, 13, 0, 100, paragraph, len, AU_RGB(0, 0, 0));
        }
        t = (now_ms() - t0) / n;
        AuGlyphCacheStats st;
        au_glyph_cache_stats(&st);
        printf("text 13px, uncached            %8.3f ms per %zu glyphs (%zu misses total)\n", t, glyphs, st.misses);
        t0 = now_ms();
        for (int i = 0; i < n; i++)
            (void)au_text_width(sans, 13, paragraph, len);
        t = (now_ms() - t0) / n;
        printf("measure 13px                   %8.3f ms per %zu glyphs\n", t, glyphs);
    }
    au_path_free(&p);
    au_canvas_free(c);
    au_surface_free(s);
    return 0;
}

/* ---- chrome: a browser window rendered to PNG ---- */

static uint32_t host_ms(void *ctx)
{
    (void)ctx;
    return au_platform_time_ms();
}

static AuTime host_now(void *ctx)
{
    (void)ctx;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (AuTime)t + (AuTime)lt.tm_gmtoff;
}

static bool host_read_url(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    char *path = au_url_to_path(url, '/');
    if (!path)
        return false;
    bool ok = au_platform_read_file(path, data, len);
    au_free(path);
    return ok;
}

typedef struct ChromeOpts {
    int w, h;
    AuTabPosition pos;
    int column;
    const char *tabs[64];
    int ntabs, extra_tabs, active;
    const char *field;
    bool focus, loading, menus, net;
    const char *hover;
    int scroll, mouse_x, mouse_y;
    int scroll_bench; /* steps of the scroll benchmark; 0 for none */
    bool no_shift;    /* scroll by repainting the page, not by moving pixels */
    const char *actions[64]; /* --do: scripted input, in order */
    int nactions;
    AuHttpCache *cache; /* --net: an HTTP cache in memory, as the Mac application has */
} ChromeOpts;

/* ---- The scroll benchmark: the Mac shell's flush on the host ----
 * The window lives in a surface (the GWorld) and a second surface stands
 * for the screen. Each step scrolls, then flushes as browserwin.c does:
 * the pending shift moves the surface's pixels and the screen's, the
 * damaged rectangles are painted, and they are copied to the screen. */

static AuPresent bench_pres;

static void bench_inval(void *ctx, AuBrowserWindow *w, AuIRect r)
{
    (void)ctx, (void)w;
    au_present_invalidate(&bench_pres, r);
}

static void bench_scroll(void *ctx, AuBrowserWindow *w, AuIRect area, int dx, int dy)
{
    (void)ctx, (void)w;
    au_present_scroll(&bench_pres, area, dx, dy);
}

static void copy_rect(AuSurface *dst, const AuSurface *src, AuIRect r)
{
    for (int y = r.y0; y < r.y1; y++)
        memcpy(dst->pixels + (size_t)y * (size_t)dst->stride + (size_t)r.x0,
               src->pixels + (size_t)y * (size_t)src->stride + (size_t)r.x0, (size_t)(r.x1 - r.x0) * sizeof(AuPixel));
}

typedef struct BenchTimes {
    double model, paint, blit; /* scrolling in the model (hover, layout), painting, copying */
    long painted, copied; /* pixels */
} BenchTimes;

static void bench_flush(AuBrowserWindow *w, AuSurface *s, AuSurface *screen, BenchTimes *bt)
{
    double t0 = now_ms();
    AuPresentRound r;
    au_present_begin(&bench_pres, s, &r);
    AuCanvas *c = au_canvas_new(s);
    for (int i = 0; i < r.n; i++) {
        au_bw_paint(w, c, r.rects[i]);
        bt->painted += (long)(r.rects[i].x1 - r.rects[i].x0) * (r.rects[i].y1 - r.rects[i].y0);
    }
    au_canvas_free(c);
    double t1 = now_ms();
    if (r.shifted)
        au_present_shift_pixels(screen, r.area, r.dx, r.dy);
    for (int i = 0; i < r.n; i++) {
        copy_rect(screen, s, r.rects[i]);
        bt->copied += (long)(r.rects[i].x1 - r.rects[i].x0) * (r.rects[i].y1 - r.rects[i].y0);
    }
    double t2 = now_ms();
    bt->paint += t1 - t0;
    bt->blit += t2 - t1;
}

static void scroll_bench(AuBrowserWindow *w, int W, int H, int steps)
{
    AuSurface *s = au_surface_new(W, H), *screen = au_surface_new(W, H);
    BenchTimes bt;
    memset(&bt, 0, sizeof bt);
    au_present_invalidate(&bench_pres, (AuIRect){0, 0, W, H});
    bench_flush(w, s, screen, &bt);
    printf("first paint: %.2f ms\n", bt.paint);
    AuTab *t = au_bw_active_tab(w);
    int mx, my;
    au_bw_scroll_max(w, &mx, &my);
    int page = (w->layout.page.y1 - w->layout.page.y0) - 40;
    static const char *names[] = {"32-pixel step", "page down"};
    for (int kind = 0; kind < 2; kind++) {
        int d = kind == 0 ? 32 : page;
        au_bw_scroll_to(w, 0, 0);
        bench_flush(w, s, screen, &bt);
        AuGlyphCacheStats g0, g1;
        au_glyph_cache_stats(&g0);
        memset(&bt, 0, sizeof bt);
        int n = 0;
        for (int i = 0; i < steps; i++) {
            int before = t->scroll_y;
            double s0 = now_ms();
            au_bw_scroll_by(w, 0, d);
            bt.model += now_ms() - s0;
            if (t->scroll_y == before)
                break;
            bench_flush(w, s, screen, &bt);
            n++;
        }
        au_glyph_cache_stats(&g1);
        if (!n) {
            printf("%s: the page does not scroll\n", names[kind]);
            continue;
        }
        printf("%s: %.2f ms scroll + %.2f ms paint + %.2f ms copy per step (%d steps; %ld pixels painted, %ld copied per step; "
               "glyph cache %lu hits, %lu misses, %lu evictions, %lu KB)\n",
               names[kind], bt.model / n, bt.paint / n, bt.blit / n, n, bt.painted / n, bt.copied / n,
               (unsigned long)(g1.hits - g0.hits), (unsigned long)(g1.misses - g0.misses),
               (unsigned long)(g1.evictions - g0.evictions), (unsigned long)(g1.bytes / 1024));
    }
    /* Images: page downs to the end and back up, as a reader does. */
    AuPageStats p0, p1;
    au_page_stats(t->page, &p0);
    for (int dir = 1; dir >= -1; dir -= 2)
        for (int i = 0; i < 400; i++) {
            int before = t->scroll_y;
            au_bw_scroll_by(w, 0, dir * page);
            if (t->scroll_y == before)
                break;
            bench_flush(w, s, screen, &bt);
        }
    au_page_stats(t->page, &p1);
    printf("images, down to the end and back: %lu decodes, %lu evictions; %lu KB decoded of a %lu KB budget\n",
           (unsigned long)(p1.image_decodes - p0.image_decodes),
           (unsigned long)(p1.image_evictions - p0.image_evictions), (unsigned long)p1.image_kb,
           (unsigned long)p1.image_budget_kb);
    au_surface_free(s);
    au_surface_free(screen);
}

/* Runs the idle loop for at least min_ms and then until nothing is busy,
 * at most limit_ms in all. */
static void chrome_settle(AuBrowser *b, AuBrowserWindow *w, AuHostSession *ses, bool net, double min_ms,
                          double limit_ms)
{
    double t0 = now_ms();
    for (;;) {
        double t = now_ms() - t0;
        if (t >= limit_ms || (t >= min_ms && !au_browser_busy(b)))
            break;
        if (net) {
            au_host_net_wait(ses->host, 10);
        } else {
            struct timespec ts = {0, 10 * 1000000};
            nanosleep(&ts, NULL);
        }
        au_bw_idle(w);
    }
}

static void chrome_shot(AuBrowserWindow *w, int width, int height, const char *path)
{
    AuSurface *s = au_surface_new(width, height);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, width, height});
    if (!au_png_write(path, s))
        fprintf(stderr, "aura-host: cannot write %s\n", path);
    else
        printf("%s: screenshot\n", path);
    au_canvas_free(c);
    au_surface_free(s);
}

/* What the window invalidated since the record was cleared (--do keys:). */
static struct {
    int n;
    long area;
    AuIRect u;
    AuIRect r[16];
} key_inv;

static void key_inval(void *ctx, AuBrowserWindow *w, AuIRect r)
{
    (void)ctx, (void)w;
    if (r.x1 <= r.x0 || r.y1 <= r.y0)
        return;
    if (key_inv.n < 16)
        key_inv.r[key_inv.n] = r;
    if (!key_inv.n) {
        key_inv.u = r;
    } else {
        if (r.x0 < key_inv.u.x0) key_inv.u.x0 = r.x0;
        if (r.y0 < key_inv.u.y0) key_inv.u.y0 = r.y0;
        if (r.x1 > key_inv.u.x1) key_inv.u.x1 = r.x1;
        if (r.y1 > key_inv.u.y1) key_inv.u.y1 = r.y1;
    }
    key_inv.n++;
    key_inv.area += (long)(r.x1 - r.x0) * (r.y1 - r.y0);
}

static void chrome_key(AuBrowserWindow *w, int key, uint32_t ch, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.key = key;
    e.ch = ch;
    e.mods = mods;
    au_bw_key(w, &e);
}

/* One --do action: click:X,Y (dclick, tclick for double and triple),
 * type:TEXT, key:[cmd-][shift-][opt-][ctrl-]NAME (a key name or one
 * character), edit:undo|cut|copy|paste:TEXT|clear|selectall, wait:MS
 * (at least MS, then until loads finish), shot:FILE.png, field (the
 * address field takes the keyboard). */
static void chrome_action(AuBrowser *b, AuBrowserWindow *w, AuHostSession *ses, ChromeOpts *o, const char *a)
{
    char buf[64];
    if (!strncmp(a, "clickname:", 10)) {
        /* click the middle of the element named so (scrolled into view) */
        AuTab *t = au_bw_active_tab(w);
        AuIRect r;
        if (!t || !au_page_control_rect(t->page, a + 10, &r)) {
            printf("clickname: no element named %s\n", a + 10);
            return;
        }
        int ph = w->layout.page.y1 - w->layout.page.y0;
        if (r.y0 < t->scroll_y || r.y1 > t->scroll_y + ph)
            au_bw_scroll_to(w, t->scroll_x, r.y0 - ph / 3);
        int x = (r.x0 + r.x1) / 2 - t->scroll_x + w->layout.page.x0;
        int y = (r.y0 + r.y1) / 2 - t->scroll_y + w->layout.page.y0;
        snprintf(buf, sizeof buf, "click:%d,%d", x, y);
        a = buf;
    }
    if (!strncmp(a, "click:", 6) || !strncmp(a, "dclick:", 7) || !strncmp(a, "tclick:", 7)) {
        int clicks = a[0] == 'd' ? 2 : a[0] == 't' ? 3 : 1;
        int x = 0, y = 0;
        sscanf(strchr(a, ':') + 1, "%d,%d", &x, &y);
        for (int k = 1; k <= clicks; k++) {
            AuEvent e;
            memset(&e, 0, sizeof e);
            e.x = x;
            e.y = y;
            e.clicks = k;
            e.type = AU_EVENT_MOUSE_MOVE;
            au_bw_mouse(w, &e);
            e.type = AU_EVENT_MOUSE_DOWN;
            au_bw_mouse(w, &e);
            e.type = AU_EVENT_MOUSE_UP;
            au_bw_mouse(w, &e);
        }
        au_bw_set_field_focus(w, false);
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "type:", 5)) {
        const char *s = a + 5;
        size_t n = strlen(s), i = 0;
        while (i < n) {
            size_t adv = 1;
            uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
            i += adv ? adv : 1;
            if (cp == '\n')
                chrome_key(w, AU_KEY_RETURN, 0, 0);
            else
                chrome_key(w, 0, cp, 0);
        }
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "key:", 4)) {
        const char *s = a + 4;
        uint32_t mods = 0;
        for (;;) {
            if (!strncmp(s, "cmd-", 4))
                mods |= AU_MOD_COMMAND, s += 4;
            else if (!strncmp(s, "shift-", 6))
                mods |= AU_MOD_SHIFT, s += 6;
            else if (!strncmp(s, "opt-", 4))
                mods |= AU_MOD_OPTION, s += 4;
            else if (!strncmp(s, "ctrl-", 5))
                mods |= AU_MOD_CONTROL, s += 5;
            else
                break;
        }
        static const struct {
            const char *name;
            int key;
        } keys[] = {{"left", AU_KEY_LEFT},       {"right", AU_KEY_RIGHT},       {"up", AU_KEY_UP},
                    {"down", AU_KEY_DOWN},       {"pageup", AU_KEY_PAGE_UP},    {"pagedown", AU_KEY_PAGE_DOWN},
                    {"home", AU_KEY_HOME},       {"end", AU_KEY_END},           {"backspace", AU_KEY_BACKSPACE},
                    {"delete", AU_KEY_DELETE},   {"return", AU_KEY_RETURN},     {"tab", AU_KEY_TAB},
                    {"escape", AU_KEY_ESCAPE},   {"space", -1}};
        int key = 0;
        uint32_t ch = 0;
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
            if (!strcmp(s, keys[k].name))
                key = keys[k].key;
        if (key == -1) {
            key = 0;
            ch = ' ';
        } else if (!key) {
            size_t adv = 0;
            ch = au_utf8_decode(s, strlen(s), &adv);
        }
        chrome_key(w, key, ch, mods);
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "edit:", 5)) {
        const char *c = a + 5;
        int cmd = !strcmp(c, "undo")        ? AU_PAGE_EDIT_UNDO
                  : !strcmp(c, "cut")       ? AU_PAGE_EDIT_CUT
                  : !strcmp(c, "copy")      ? AU_PAGE_EDIT_COPY
                  : !strncmp(c, "paste", 5) ? AU_PAGE_EDIT_PASTE
                  : !strcmp(c, "clear")     ? AU_PAGE_EDIT_CLEAR
                                            : AU_PAGE_EDIT_SELECT_ALL;
        const char *text = !strncmp(c, "paste:", 6) ? c + 6 : "";
        AuBuf out;
        au_buf_init(&out);
        bool ok = au_bw_page_edit(w, cmd, text, strlen(text), &out);
        printf("edit %s: %s%s%s\n", c, ok ? "done" : "not done", out.len ? ", text: " : "", au_buf_cstr(&out));
        au_buf_free(&out);
    } else if (!strncmp(a, "wait:", 5)) {
        chrome_settle(b, w, ses, o->net, atof(a + 5), o->net ? 60000 : 10000);
    } else if (!strncmp(a, "shot:", 5)) {
        chrome_settle(b, w, ses, o->net, 0, 200);
        chrome_shot(w, o->w, o->h, a + 5);
    } else if (!strcmp(a, "field")) {
        au_bw_set_field_focus(w, true);
    } else if (!strncmp(a, "go:", 3)) {
        au_bw_navigate(w, a + 3);
        chrome_settle(b, w, ses, o->net, 20, o->net ? 60000 : 10000);
    } else if (!strcmp(a, "back") || !strcmp(a, "forward")) {
        au_bw_go(w, a[0] == 'b' ? -1 : 1);
        chrome_settle(b, w, ses, o->net, 20, o->net ? 60000 : 10000);
    } else if (!strcmp(a, "cache")) {
        AuCacheStats cs;
        memset(&cs, 0, sizeof cs);
        if (o->cache)
            au_cache_stats(o->cache, &cs);
        printf("cache: %lu fresh, %lu history, %lu revalidated (%lu not modified), %lu missed, %lu stored\n",
               cs.fresh, cs.history, cs.stale, cs.not_modified, cs.misses, cs.stored);
    } else if (!strncmp(a, "keys:", 5)) {
        /* Types TEXT one key at a time and prints what each key cost:
         * the time in the key event and the idle pass after it, the
         * document view's update counts, the rectangles invalidated, and
         * the time to paint them. */
        const char *s = a + 5;
        size_t n = strlen(s), i = 0;
        AuTab *t = au_bw_active_tab(w);
        AuSurface *surf = au_surface_new(o->w, o->h);
        AuCanvas *cv = au_canvas_new(surf);
        au_bw_paint(w, cv, (AuIRect){0, 0, o->w, o->h});
        while (t && i < n) {
            size_t adv = 1;
            uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
            i += adv ? adv : 1;
            AuDocViewStats s0, s1;
            memset(&s0, 0, sizeof s0);
            memset(&s1, 0, sizeof s1);
            au_page_update_stats(t->page, &s0);
            memset(&key_inv, 0, sizeof key_inv);
            double t0 = now_ms();
            chrome_key(w, 0, cp, 0);
            double t1 = now_ms();
            au_bw_idle(w);
            double t2 = now_ms();
            au_page_update_stats(t->page, &s1);
            for (int k = 0; k < key_inv.n && k < 16; k++)
                au_bw_paint(w, cv, key_inv.r[k]);
            double t3 = now_ms();
            printf("key %c: event %.2f ms, idle %.2f ms, paint %.2f ms; %u style passes, %u elements styled, "
                   "%u tree builds, %u layouts; %d invalidations, %ld px, union %d,%d %dx%d\n",
                   cp < 127 ? (char)cp : '?', t1 - t0, t2 - t1, t3 - t2, s1.style_passes - s0.style_passes,
                   s1.styled_elements - s0.styled_elements, s1.tree_builds - s0.tree_builds, s1.layouts - s0.layouts,
                   key_inv.n, key_inv.area, key_inv.u.x0, key_inv.u.y0, key_inv.u.x1 - key_inv.u.x0,
                   key_inv.u.y1 - key_inv.u.y0);
        }
        au_canvas_free(cv);
        au_surface_free(surf);
    } else if (!strncmp(a, "js:", 3)) {
        AuTab *t = au_bw_active_tab(w);
        AuBuf out;
        au_buf_init(&out);
        bool ok = t && au_page_eval(t->page, a + 3, &out);
        printf("js %s: %s\n", ok ? "=" : "failed", au_buf_cstr(&out));
        au_buf_free(&out);
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "save:", 5)) {
        AuTab *t = au_bw_active_tab(w);
        const char *data;
        size_t len;
        FILE *f;
        if (t && au_page_source(t->page, &data, &len) && (f = fopen(a + 5, "wb"))) {
            fwrite(data, 1, len, f);
            fclose(f);
            printf("save: %lu bytes\n", (unsigned long)len);
        } else {
            printf("save: no source\n");
        }
    } else if (!strcmp(a, "console")) {
        AuTab *t = au_bw_active_tab(w);
        size_t n = t ? au_page_console_count(t->page) : 0;
        for (size_t i = 0; i < n; i++) {
            int level;
            const char *line = au_page_console_line(t->page, i, &level);
            printf("console %d: %s\n", level, line ? line : "");
        }
    } else {
        fprintf(stderr, "aura-host: unknown --do action %s\n", a);
    }
    AuTab *t = au_bw_active_tab(w);
    if (t)
        printf("after %s: %s%s\n", a, au_page_url(t->page), au_bw_page_editing(w) ? " (editing a field)" : "");
}

static AuHitPart hit_by_name(const char *s)
{
    static const struct {
        const char *name;
        AuHitPart part;
    } names[] = {{"back", AU_HIT_BACK},       {"forward", AU_HIT_FORWARD}, {"reload", AU_HIT_RELOAD},
                 {"home", AU_HIT_HOME},       {"bookmark", AU_HIT_BOOKMARK}, {"newtab", AU_HIT_NEW_TAB},
                 {"splitter", AU_HIT_SPLITTER}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(names[i].name, s))
            return names[i].part;
    return AU_HIT_NONE;
}

static int cmd_chrome(AuFontSet *fonts, const char *out, ChromeOpts *o)
{
    AuBrowserHost host = {NULL, host_ms, host_now, host_read_url, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    if (o->scroll_bench) {
        au_present_init(&bench_pres);
        host.invalidate = bench_inval;
        host.scroll_area = bench_scroll;
    } else {
        host.invalidate = key_inval;
    }
    AuBrowser *b = au_browser_new(fonts, &host);
    /* --net: http and https through the network stack and page fetcher,
     * as the Mac application does; without it they show the stand-in. */
    AuHostSession ses;
    AuNetLoader *loader = NULL;
    AuNetPageFetcher *pf = NULL;
    memset(&ses, 0, sizeof ses);
    if (o->net) {
        if (!au_host_session_open(&ses, true, false)) {
            fprintf(stderr, "aura-host: cannot set up sockets\n");
            au_browser_free(b);
            return 3;
        }
        au_net_apply_prefs(ses.net, &b->prefs);
        o->cache = au_cache_new(au_net_cache_bytes_from_prefs(&b->prefs));
        au_net_set_cache(ses.net, o->cache);
        loader = au_net_loader_new(ses.net);
        pf = au_net_page_fetcher_new(loader);
        AuPageFetcher f;
        au_net_page_fetcher_interface(pf, &f);
        au_browser_set_fetcher(b, "http", &f);
        au_browser_set_fetcher(b, "https", &f);
    }
    b->prefs.tab_position = o->pos;
    if (o->column)
        b->prefs.tab_column_width = o->column;
    AuBrowserWindow *w = au_browser_window_new(b, o->w, o->h);
    w->native_standins = true;
    au_bookmark_add(&b->bookmarks, NULL, NULL, "Aura", "about:home", 0);
    if (o->ntabs == 0 && o->extra_tabs == 0)
        o->tabs[o->ntabs++] = "about:home";
    for (int i = 0; i < o->ntabs; i++) {
        /* "file:relative/path" and plain paths are made absolute file: URLs. */
        const char *t = o->tabs[i];
        char *url = NULL;
        if (!strncmp(t, "file:", 5) && t[5] != '/')
            t += 5;
        if (!au_url_scheme_len(t, strlen(t))) {
            char path[4096];
            if (t[0] == '/')
                snprintf(path, sizeof path, "%s", t);
            else if (getcwd(path, sizeof path - 2))
                snprintf(path + strlen(path), sizeof path - strlen(path), "/%s", t);
            url = au_url_from_path(path, '/');
        }
        au_bw_new_tab(w, url ? url : o->tabs[i], true, w->ntabs);
        au_free(url);
    }
    static const char *fill[] = {"about:home", "http://example.com/", "about:scene", "about:fonts",
                                 "http://www.macintoshrepository.org/", "about:blank"};
    for (int i = 0; i < o->extra_tabs; i++)
        au_bw_new_tab(w, fill[i % 6], false, w->ntabs);
    if (!o->loading) {
        /* Let loads finish (simulated ones in 5 seconds, network ones in
         * 60). */
        double t0 = now_ms(), limit = o->net ? 60000 : 5000;
        while (au_browser_busy(b) && now_ms() - t0 < limit) {
            if (o->net) {
                au_host_net_wait(ses.host, 10);
            } else {
                struct timespec ts = {0, 20 * 1000000};
                nanosleep(&ts, NULL);
            }
            au_bw_idle(w);
        }
        if (o->net)
            printf("loaded in %.0f ms%s\n", now_ms() - t0, au_browser_busy(b) ? " (still busy at the limit)" : "");
    } else {
        double t0 = now_ms();
        while (now_ms() - t0 < 500) {
            struct timespec ts = {0, 20 * 1000000};
            nanosleep(&ts, NULL);
            au_bw_idle(w);
        }
    }
    if (o->active >= 0 && o->active < w->ntabs)
        au_bw_select_tab(w, o->active);
    if (o->nactions)
        au_bw_set_field_focus(w, false); /* keys go to the page */
    for (int i = 0; i < o->nactions; i++)
        chrome_action(b, w, &ses, o, o->actions[i]);
    if (o->field)
        au_bw_set_field_text(w, o->field);
    au_bw_set_field_focus(w, o->focus);
    if (o->hover) {
        w->hover.part = hit_by_name(o->hover);
        w->hover.index = -1;
    }
    if (o->scroll)
        au_bw_scroll_to(w, 0, o->scroll);
    if (o->mouse_x >= 0) {
        /* Move the pointer there (hover state, a link's status text). */
        AuEvent e;
        memset(&e, 0, sizeof e);
        e.type = AU_EVENT_MOUSE_MOVE;
        e.x = o->mouse_x;
        e.y = o->mouse_y;
        au_bw_mouse(w, &e);
    }
    if (o->scroll_bench) {
        /* Repainting the page for each step (as before pixels were moved),
         * then moving pixels. */
        b->host.scroll_area = NULL;
        printf("%dx%d, scrolling by repainting the page\n", o->w, o->h);
        scroll_bench(w, o->w, o->h, o->scroll_bench);
        if (!o->no_shift) {
            b->host.scroll_area = bench_scroll;
            printf("%dx%d, scrolling by moving pixels\n", o->w, o->h);
            scroll_bench(w, o->w, o->h, o->scroll_bench);
        }
    }
    AuSurface *s = au_surface_new(o->w, o->h);
    AuCanvas *c = au_canvas_new(s);
    double t0 = now_ms();
    au_bw_paint(w, c, (AuIRect){0, 0, o->w, o->h});
    double t1 = now_ms();
    int rc = 0;
    if (!au_png_write(out, s)) {
        fprintf(stderr, "aura-host: cannot write %s\n", out);
        rc = 1;
    } else {
        printf("%s: %dx%d, %d tabs (%s), painted in %.2f ms\n", out, o->w, o->h, w->ntabs,
               o->pos == AU_TABS_LEFT ? "left" : "top", t1 - t0);
    }
    au_canvas_free(c);
    au_surface_free(s);
    au_browser_free(b);
    if (o->net) {
        au_net_page_fetcher_free(pf);
        au_net_loader_free(loader);
        au_host_session_close(&ses);
        au_cache_free(o->cache);
    }
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "fetch"))
        return au_host_fetch_main(argc, argv);
    if ((!strcmp(cmd, "render") || !strcmp(cmd, "dump")) && au_host_net_render_wanted(argc, argv))
        return au_host_net_render_main(argc, argv);
    if (!strcmp(cmd, "render") || !strcmp(cmd, "dump"))
        return au_host_render_main(argc, argv);
    if (!strcmp(cmd, "image"))
        return au_host_image_main(argc, argv);
    if (!strcmp(cmd, "js"))
        return au_host_js_main(argc, argv);
    if (!strcmp(cmd, "page"))
        return au_host_page_main(argc, argv);
    if (!strcmp(cmd, "chrome")) {
        ChromeOpts o;
        memset(&o, 0, sizeof o);
        o.w = 800;
        o.h = 600;
        o.active = -1;
        o.mouse_x = -1;
        const char *out = "chrome.png";
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            int used = au_host_font_option(argc, argv, i);
            if (used)
                i += used - 1;
            else if (!strcmp(a, "-o") && i + 1 < argc)
                out = argv[++i];
            else if (!strcmp(a, "-w") && i + 1 < argc)
                o.w = atoi(argv[++i]);
            else if (!strcmp(a, "-h") && i + 1 < argc)
                o.h = atoi(argv[++i]);
            else if (!strcmp(a, "--tabs") && i + 1 < argc)
                o.pos = !strcmp(argv[++i], "left") ? AU_TABS_LEFT : AU_TABS_TOP;
            else if (!strcmp(a, "--column") && i + 1 < argc)
                o.column = atoi(argv[++i]);
            else if (!strcmp(a, "--tab") && i + 1 < argc && o.ntabs < 64)
                o.tabs[o.ntabs++] = argv[++i];
            else if (!strcmp(a, "--more-tabs") && i + 1 < argc)
                o.extra_tabs = atoi(argv[++i]);
            else if (!strcmp(a, "--active") && i + 1 < argc)
                o.active = atoi(argv[++i]);
            else if (!strcmp(a, "--field") && i + 1 < argc)
                o.field = argv[++i];
            else if (!strcmp(a, "--focus"))
                o.focus = true;
            else if (!strcmp(a, "--loading"))
                o.loading = true;
            else if (!strcmp(a, "--net"))
                o.net = true;
            else if (!strcmp(a, "--hover") && i + 1 < argc)
                o.hover = argv[++i];
            else if (!strcmp(a, "--scroll") && i + 1 < argc)
                o.scroll = atoi(argv[++i]);
            else if (!strcmp(a, "--scroll-bench") && i + 1 < argc)
                o.scroll_bench = atoi(argv[++i]);
            else if (!strcmp(a, "--no-shift"))
                o.no_shift = true;
            else if (!strcmp(a, "--mouse") && i + 2 < argc) {
                o.mouse_x = atoi(argv[++i]);
                o.mouse_y = atoi(argv[++i]);
            }
            else if (!strcmp(a, "--do") && i + 1 < argc && o.nactions < 64)
                o.actions[o.nactions++] = argv[++i];
            else
                return usage();
        }
        if (o.w <= 0 || o.h <= 0 || o.w > 8192 || o.h > 8192)
            return usage();
        AuFontSet *fonts = au_fontset_new();
        au_platform_load_fonts(fonts);
        int rc = cmd_chrome(fonts, out, &o);
        au_fontset_free(fonts);
        return rc;
    }
    const char *out = "scene.png";
    int w = 640, h = AU_SCENE_HEIGHT, scroll = 0;
    if (!strcmp(cmd, "font-index"))
        return cmd_font_index(argc, argv);
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int used = au_host_font_option(argc, argv, i);
        if (used)
            i += used - 1;
        else if (!strcmp(a, "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "-w") && i + 1 < argc)
            w = atoi(argv[++i]);
        else if (!strcmp(a, "-h") && i + 1 < argc)
            h = atoi(argv[++i]);
        else if (!strcmp(a, "-s") && i + 1 < argc)
            scroll = atoi(argv[++i]);
        else
            return usage();
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return usage();
    AuFontSet *fonts = au_fontset_new();
    au_platform_load_fonts(fonts);
    int rc;
    if (!strcmp(cmd, "scene"))
        rc = cmd_scene(fonts, out, w, h, scroll);
    else if (!strcmp(cmd, "fonts"))
        rc = cmd_fonts(fonts);
    else if (!strcmp(cmd, "bench"))
        rc = cmd_bench(fonts);
    else
        rc = usage();
    au_fontset_free(fonts);
    return rc;
}
