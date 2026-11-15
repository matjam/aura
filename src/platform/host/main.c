/* aura-host: the headless development tool.
 *
 *   aura-host scene [-o out.png] [-w width] [-h height] [-s scroll]
 *                   [--font file.ttf]... [--no-system-fonts]
 *       Draws the paint test scene and writes it as a PNG.
 *   aura-host fonts [--font file.ttf]...
 *       Lists the registered font faces and the generic families.
 *   aura-host bench [--font file.ttf]...
 *       Times the rasterizer, compositing and text. */
#include "host.h"
#include "png.h"
#include "platform/scene.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: aura-host scene [-o out.png] [-w W] [-h H] [-s scroll] [--font F]... [--no-system-fonts]\n"
            "       aura-host fonts [--font F]...\n"
            "       aura-host bench [--font F]...\n"
            "       aura-host render page.html -w W [-h H] [-s scrollY] -o out.png\n"
            "       aura-host dump page.html -w W\n");
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

static int cmd_fonts(AuFontSet *fonts)
{
    for (size_t i = 0; i < au_fontset_count(fonts); i++) {
        int weight;
        bool italic;
        const char *fam = au_fontset_entry(fonts, i, &weight, &italic);
        printf("%-28s %3d %s\n", fam, weight, italic ? "italic" : "normal");
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

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "render") || !strcmp(cmd, "dump"))
        return au_host_render_main(argc, argv);
    const char *out = "scene.png";
    int w = 640, h = AU_SCENE_HEIGHT, scroll = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "-w") && i + 1 < argc)
            w = atoi(argv[++i]);
        else if (!strcmp(a, "-h") && i + 1 < argc)
            h = atoi(argv[++i]);
        else if (!strcmp(a, "-s") && i + 1 < argc)
            scroll = atoi(argv[++i]);
        else if (!strcmp(a, "--font") && i + 1 < argc)
            au_host_add_font_path(argv[++i]);
        else if (!strcmp(a, "--no-system-fonts"))
            au_host_use_system_fonts(false);
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
