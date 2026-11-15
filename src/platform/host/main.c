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
#include "browser/scene.h"
#include "browser/url.h"
#include "render.h"

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
            "                        (--tab takes a URL, a path, or file:relative/path)\n"
            "       aura-host render page.html -w W [-h H] [-s scrollY] -o out.png\n"
            "       aura-host dump page.html -w W\n"
            "       aura-host image FILE [-o out.png] [--scale N] [--target WxH] [--chunk N] [--frames]\n"
            "                        [--repeat N] [--max-bytes N] [--favicon SIZE]\n");
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
    bool focus, loading, menus;
    const char *hover;
    int scroll, mouse_x, mouse_y;
} ChromeOpts;

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
    AuBrowserHost host = {NULL, host_ms, host_now, host_read_url, NULL, NULL, NULL};
    AuBrowser *b = au_browser_new(fonts, &host);
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
        /* Let simulated loads finish. */
        double t0 = now_ms();
        while (au_browser_busy(b) && now_ms() - t0 < 5000) {
            struct timespec ts = {0, 20 * 1000000};
            nanosleep(&ts, NULL);
            au_bw_idle(w);
        }
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
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "render") || !strcmp(cmd, "dump"))
        return au_host_render_main(argc, argv);
    if (!strcmp(cmd, "image"))
        return au_host_image_main(argc, argv);
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
            else if (!strcmp(a, "--hover") && i + 1 < argc)
                o.hover = argv[++i];
            else if (!strcmp(a, "--scroll") && i + 1 < argc)
                o.scroll = atoi(argv[++i]);
            else if (!strcmp(a, "--mouse") && i + 2 < argc) {
                o.mouse_x = atoi(argv[++i]);
                o.mouse_y = atoi(argv[++i]);
            }
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
