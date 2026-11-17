/* aura-host render and dump: an HTML file through the document view.
 *
 *   aura-host render page.html -w 800 [-h 600] [-s scrollY] -o out.png
 *                    [--font F]... [--no-system-fonts | --bundled-only]
 *                    [--no-bundled-fonts] [--fonts-dir D] [--repeat N]
 *       Lays out the page at width W and writes a PNG. The height
 *       defaults to the document height, capped at 16384. Prints timings.
 *   aura-host dump page.html -w 800 [-h 600]
 *       Prints the box tree with geometry.
 *
 * Resources come through a file: loader (render_loader.c). */
#include "host.h"
#include "png.h"
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
    fprintf(stderr, "usage: aura-host render page.html -w W [-h H] [-s scrollY] -o out.png [font options]\n"
                    "       aura-host dump page.html -w W [-h H] [font options]\n"
                    "font options: --font F, --no-system-fonts (or --bundled-only), --no-bundled-fonts,\n"
                    "              --fonts-dir D\n");
    return 2;
}

static void file_url(const char *path, char *out, size_t cap)
{
    char abs[4096];
    if (path[0] == '/')
        snprintf(abs, sizeof abs, "%s", path);
    else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd))
            cwd[0] = 0;
        snprintf(abs, sizeof abs, "%s/%s", cwd, path);
    }
    snprintf(out, cap, "file://%s", abs);
}

int au_host_render_main(int argc, char **argv)
{
    const char *cmd = argv[1];
    const char *page = NULL, *out = "page.png";
    int w = 800, h = 0, scroll = 0, repeat = 1;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int used = au_host_font_option(argc, argv, i);
        if (used) {
            i += used - 1;
            continue;
        }
        if (!strcmp(a, "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "-w") && i + 1 < argc)
            w = atoi(argv[++i]);
        else if (!strcmp(a, "-h") && i + 1 < argc)
            h = atoi(argv[++i]);
        else if (!strcmp(a, "-s") && i + 1 < argc)
            scroll = atoi(argv[++i]);
        else if (!strcmp(a, "--repeat") && i + 1 < argc)
            repeat = atoi(argv[++i]);
        else if (a[0] != '-' && !page)
            page = a;
        else
            return usage();
    }
    if (!page || w <= 0 || w > 8192 || h < 0 || h > 16384 || repeat < 1)
        return usage();
    char *data;
    size_t len;
    if (!au_platform_read_file(page, &data, &len)) {
        fprintf(stderr, "aura-host: cannot read %s\n", page);
        return 1;
    }
    AuFontSet *fonts = au_fontset_new();
    double t0 = now_ms();
    au_platform_load_fonts(fonts);
    double t_fonts = now_ms() - t0;
    AuHostLoader *loader = au_host_loader_new();
    AuLoader li;
    au_host_loader_interface(loader, &li);
    AuDocView *dv = au_docview_new(fonts, &li, NULL);
    au_docview_set_dark(dv, au_host_dark());
    char url[4200];
    file_url(page, url, sizeof url);
    t0 = now_ms();
    au_docview_load(dv, data, len, url);
    double t_parse = now_ms() - t0;
    au_docview_set_viewport(dv, w, h ? h : 600);
    /* Deliver resources until nothing is pending (images are requested
     * during layout, so update between rounds). */
    t0 = now_ms();
    double t_first = 0, t_last_update = 0;
    for (int round = 0; round < 20; round++) {
        double tu = now_ms();
        au_docview_update(dv);
        t_last_update = now_ms() - tu;
        if (round == 0)
            t_first = t_last_update;
        if (!au_host_loader_pump(loader, dv))
            break;
    }
    double t_res = now_ms() - t0;
    /* Timed style + layout from scratch. */
    AuLayout *L = au_docview_layout(dv);
    t0 = now_ms();
    au_layout_tree_changed(L);
    au_docview_update(dv);
    double t_layout = now_ms() - t0;
    double t_relayout = 0;
    for (int i = 0; i < repeat; i++) {
        t0 = now_ms();
        au_layout_tree_changed(L);
        au_layout_update(L, (float)w, (float)(h ? h : 600));
        t_relayout += now_ms() - t0;
    }
    t_relayout /= repeat;
    int cw, ch;
    au_docview_content_size(dv, &cw, &ch);
    int rc = 0;
    if (!strcmp(cmd, "dump")) {
        AuBuf b;
        au_buf_init(&b);
        au_layout_dump(L, &b);
        fwrite(b.data, 1, b.len, stdout);
        au_buf_free(&b);
    } else {
        int ph = h ? h : (ch > 16384 ? 16384 : ch);
        if (h)
            au_docview_set_viewport(dv, w, h);
        AuSurface *s = au_surface_new(w, ph);
        AuCanvas *c = au_canvas_new(s);
        t0 = now_ms();
        au_docview_paint(dv, c, (AuRect){0, 0, (float)w, (float)ph}, 0, scroll);
        double t_paint = now_ms() - t0;
        t0 = now_ms();
        au_docview_paint(dv, c, (AuRect){0, 0, (float)w, (float)ph}, 0, scroll);
        double t_paint2 = now_ms() - t0;
        double t_paint_view = 0;
        if (ph > 600) {
            t0 = now_ms();
            au_docview_paint(dv, c, (AuRect){0, 0, (float)w, 600}, 0, scroll);
            t_paint_view = now_ms() - t0;
            au_docview_paint(dv, c, (AuRect){0, 0, (float)w, (float)ph}, 0, scroll);
        }
        /* Background images are requested when first painted: deliver
         * them and paint again. */
        for (int round = 0; round < 5 && au_host_loader_pump(loader, dv); round++) {
            au_docview_update(dv);
            au_docview_paint(dv, c, (AuRect){0, 0, (float)w, (float)ph}, 0, scroll);
        }
        if (!au_png_write(out, s)) {
            fprintf(stderr, "aura-host: cannot write %s\n", out);
            rc = 1;
        }
        AuLayoutStats st;
        au_layout_stats(L, &st);
        printf("%s: %dx%d (document %dx%d)\n", out, w, ph, cw, ch);
        printf("  fonts %.1f ms, parse %.2f ms, resources (with updates) %.2f ms\n", t_fonts, t_parse, t_res);
        printf("  first style+layout %.2f ms, last style+layout after sheets %.2f ms, box rebuild+layout %.2f ms, "
               "relayout %.2f ms\n",
               t_first, t_last_update, t_layout, t_relayout);
        printf("  paint full %.2f ms (cold glyphs), %.2f ms (warm); 600px view %.2f ms\n", t_paint, t_paint2,
               t_paint_view);
        printf("  boxes %u (blocks %u, inlines %u, texts %u), lines %u, fragments %u\n", st.boxes, st.blocks,
               st.inlines, st.texts, st.lines, st.frags);
        printf("  memory: box tree %zu bytes (%.1f per box), fragments %zu bytes; word cache %u hits %u misses\n",
               st.tree_bytes, st.boxes ? (double)st.tree_bytes / st.boxes : 0.0, st.frag_bytes, st.word_hits,
               st.word_misses);
        AuFontSetStats fst;
        au_fontset_stats(fonts, &fst);
        printf("  fonts: %zu of %zu faces loaded (%zu bytes of font data); fallback cache %zu hits %zu misses\n",
               fst.loaded, au_fontset_count(fonts), fst.loaded_bytes, fst.fallback_hits, fst.fallback_misses);
        if (fst.paged)
            printf("  paged faces: %zu, %zu glyph blocks read\n", fst.paged, fst.paged_reads);
        au_canvas_free(c);
        au_surface_free(s);
    }
    au_docview_free(dv);
    au_host_loader_free(loader);
    au_fontset_free(fonts);
    au_free(data);
    return rc;
}

/* aura-host layout-bench page.html [-w W] [-w2 W2] [-h H] [--repeat N]
 * Times layout and painting of one page at two widths (default 1440 and
 * 656) with a viewport H high (default 900): a layout from scratch, a
 * layout after only the width changed (through the document view, as a
 * window resize does, and in the layout alone), a paint of the viewport,
 * of a 32-pixel strip, of one pixel (what a paint costs whatever its area)
 * and of page downs over content not painted before.
 * Each figure is the best of N runs (default 3). --phase relayout, resize,
 * paint or strip first runs that one step N times, for a profiler. */
/* The bench times the process's CPU time, which other work on the machine
 * disturbs less than the wall clock. */
static double cpu_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double best_of(double *v, int n)
{
    double b = v[0];
    for (int i = 1; i < n; i++)
        if (v[i] < b)
            b = v[i];
    return b;
}

int au_host_layout_bench_main(int argc, char **argv)
{
    const char *page = NULL, *phase = NULL;
    int w = 1440, w2 = 656, h = 900, repeat = 3;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int used = au_host_font_option(argc, argv, i);
        if (used) {
            i += used - 1;
            continue;
        }
        if (!strcmp(a, "-w") && i + 1 < argc)
            w = atoi(argv[++i]);
        else if (!strcmp(a, "-w2") && i + 1 < argc)
            w2 = atoi(argv[++i]);
        else if (!strcmp(a, "-h") && i + 1 < argc)
            h = atoi(argv[++i]);
        else if (!strcmp(a, "--repeat") && i + 1 < argc)
            repeat = atoi(argv[++i]);
        else if (!strcmp(a, "--phase") && i + 1 < argc)
            phase = argv[++i];
        else if (a[0] != '-' && !page)
            page = a;
        else
            page = NULL, i = argc;
    }
    if (!page || w <= 0 || w > 8192 || w2 <= 0 || w2 > 8192 || h <= 0 || h > 16384 || repeat < 1 || repeat > 4096) {
        fprintf(stderr, "usage: aura-host layout-bench page.html [-w W] [-w2 W2] [-h H] [--repeat N]\n"
                        "       [--phase relayout|resize|paint|paint-cold|strip] [font options]\n");
        return 2;
    }
    char *data;
    size_t len;
    if (!au_platform_read_file(page, &data, &len)) {
        fprintf(stderr, "aura-host: cannot read %s\n", page);
        return 1;
    }
    AuFontSet *fonts = au_fontset_new();
    au_platform_load_fonts(fonts);
    AuHostLoader *loader = au_host_loader_new();
    AuLoader li;
    au_host_loader_interface(loader, &li);
    AuDocView *dv = au_docview_new(fonts, &li, NULL);
    au_docview_set_dark(dv, au_host_dark());
    char url[4200];
    file_url(page, url, sizeof url);
    au_docview_load(dv, data, len, url);
    au_docview_set_viewport(dv, w, h);
    for (int round = 0; round < 20; round++) {
        au_docview_update(dv);
        if (!au_host_loader_pump(loader, dv))
            break;
    }
    AuLayout *L = au_docview_layout(dv);
    AuLayoutStats st;
    au_layout_stats(L, &st);
    int cw, ch;
    au_docview_content_size(dv, &cw, &ch);
    printf("%s: %u boxes, %u lines, %u fragments; document %dx%d at width %d\n", page, st.boxes, st.lines, st.frags,
           cw, ch, w);
    static double v[4096];
    int widths[2] = {w, w2};
    if (phase && !strcmp(phase, "relayout")) {
        /* For a profiler: only width changes in the layout. */
        double t0 = cpu_ms();
        for (int i = 0; i < repeat; i++)
            au_layout_update(L, (float)widths[i & 1], (float)h);
        printf("%d relayouts: %.2f ms each\n", repeat, (cpu_ms() - t0) / repeat);
        repeat = 1;
    } else if (phase && !strcmp(phase, "resize")) {
        double t0 = cpu_ms();
        for (int i = 0; i < repeat; i++) {
            au_docview_set_viewport(dv, widths[i & 1], h);
            au_docview_update(dv);
        }
        printf("%d resizes: %.2f ms each\n", repeat, (cpu_ms() - t0) / repeat);
        repeat = 1;
    } else if (phase && (!strcmp(phase, "paint") || !strcmp(phase, "paint-cold") || !strcmp(phase, "strip"))) {
        AuSurface *s = au_surface_new(w, h);
        AuCanvas *c = au_canvas_new(s);
        double t0 = cpu_ms();
        for (int i = 0; i < repeat && phase[0] == 'p'; i++) {
            if (!strcmp(phase, "paint-cold"))
                au_glyph_cache_clear(); /* every glyph is rasterized again */
            au_docview_paint(dv, c, (AuRect){0, 0, (float)w, (float)h}, 0, (i % 20) * (h - 40));
        }
        if (phase[0] == 'p')
            printf("%d paints: %.3f ms each\n", repeat, (cpu_ms() - t0) / repeat);
        t0 = cpu_ms();
        for (int i = 0; i < repeat; i++)
            au_docview_paint(dv, c, (AuRect){0, (float)(h - 32), (float)w, 32}, 0, (i % 80) * 200);
        printf("%d strips: %.3f ms each\n", repeat, (cpu_ms() - t0) / repeat);
        au_canvas_free(c);
        au_surface_free(s);
        repeat = 1;
    }
    /* A layout from scratch: the box tree is built again. */
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < repeat; i++) {
            double t0 = cpu_ms();
            au_layout_tree_changed(L);
            au_layout_update(L, (float)widths[k], (float)h);
            v[i] = cpu_ms() - t0;
        }
        printf("full layout at %d: %.2f ms\n", widths[k], best_of(v, repeat));
    }
    /* A width change in the layout alone, then through the document view
     * (style included), each way. */
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < repeat; i++) {
            au_layout_update(L, (float)widths[!k], (float)h);
            double t0 = cpu_ms();
            au_layout_update(L, (float)widths[k], (float)h);
            v[i] = cpu_ms() - t0;
        }
        printf("relayout %d -> %d (layout only): %.2f ms\n", widths[!k], widths[k], best_of(v, repeat));
    }
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < repeat; i++) {
            au_docview_set_viewport(dv, widths[!k], h);
            au_docview_update(dv);
            double t0 = cpu_ms();
            au_docview_set_viewport(dv, widths[k], h);
            au_docview_update(dv);
            v[i] = cpu_ms() - t0;
        }
        printf("relayout %d -> %d (document view): %.2f ms\n", widths[!k], widths[k], best_of(v, repeat));
    }
    /* A height change alone. */
    for (int i = 0; i < repeat; i++) {
        au_docview_set_viewport(dv, widths[1], h);
        au_docview_update(dv);
        double t0 = cpu_ms();
        au_docview_set_viewport(dv, widths[1], h - 16);
        au_docview_update(dv);
        v[i] = cpu_ms() - t0;
    }
    printf("relayout at %d, height %d -> %d (document view): %.2f ms\n", widths[1], h, h - 16, best_of(v, repeat));
    /* What one change of each kind costs in passes. */
    for (int k = 0; k < 2; k++) {
        AuLayoutStats a, b;
        au_docview_set_viewport(dv, widths[0], h);
        au_docview_update(dv);
        au_layout_stats(L, &a);
        if (k == 0)
            au_docview_set_viewport(dv, widths[1], h);
        else
            au_docview_set_viewport(dv, widths[0], h - 16);
        au_docview_update(dv);
        au_layout_stats(L, &b);
        printf("%s change: %u box tree builds, %u layout passes, %u intrinsic size runs, %u subtrees reused\n",
               k == 0 ? "width" : "height", b.tree_builds - a.tree_builds, b.layout_passes - a.layout_passes,
               b.intrinsic_runs - a.intrinsic_runs, b.layouts_skipped);
    }
    /* Painting, at each width. */
    for (int k = 0; k < 2; k++) {
        int W = widths[k];
        au_docview_set_viewport(dv, W, h);
        au_docview_update(dv);
        au_docview_content_size(dv, &cw, &ch);
        AuSurface *s = au_surface_new(W, h);
        AuCanvas *c = au_canvas_new(s);
        AuGlyphCacheStats g0, g1;
        au_glyph_cache_stats(&g0);
        /* Page downs over content painted for the first time. */
        int pages = 0;
        double t_down = 0;
        for (int y = 0; y + h <= ch && pages < 10; y += h - 40, pages++) {
            double t0 = cpu_ms();
            au_docview_paint(dv, c, (AuRect){0, 0, (float)W, (float)h}, 0, y);
            t_down += cpu_ms() - t0;
        }
        au_glyph_cache_stats(&g1);
        /* The same pages again: the steady state. */
        double t_warm = 0;
        int n = 0;
        for (int r = 0; r < repeat; r++) {
            double t = 0;
            n = 0;
            for (int y = 0; y + h <= ch && n < 10; y += h - 40, n++) {
                double t0 = cpu_ms();
                au_docview_paint(dv, c, (AuRect){0, 0, (float)W, (float)h}, 0, y);
                t += cpu_ms() - t0;
            }
            if (r == 0 || t < t_warm)
                t_warm = t;
        }
        /* 32-pixel strips at the bottom of the view, as a scroll step
         * paints them. */
        double t_strip = 0, t_walk = 0;
        int ns = 0;
        for (int r = 0; r < repeat; r++) {
            double t = 0, t_dot = 0;
            ns = 0;
            for (int y = 0; y + h <= ch && ns < 40; y += (h - 40) / 4, ns++) {
                double t0 = cpu_ms();
                au_docview_paint(dv, c, (AuRect){0, (float)(h - 32), (float)W, 32}, 0, y);
                t += cpu_ms() - t0;
                t0 = cpu_ms();
                au_docview_paint(dv, c, (AuRect){0, (float)(h - 1), 1, 1}, 0, y);
                t_dot += cpu_ms() - t0;
            }
            if (r == 0 || t < t_strip)
                t_strip = t;
            if (r == 0 || t_dot < t_walk)
                t_walk = t_dot;
        }
        if (pages)
            printf("paint at %dx%d: page down, first time %.2f ms (glyph cache %lu misses per page); again %.2f ms; "
                   "32-pixel strip %.3f ms; one pixel (the walk alone) %.3f ms\n",
                   W, h, t_down / pages, (unsigned long)((g1.misses - g0.misses) / (unsigned long)pages),
                   t_warm / (n ? n : 1), t_strip / (ns ? ns : 1), t_walk / (ns ? ns : 1));
        else
            printf("paint at %dx%d: the page does not scroll\n", W, h);
        au_canvas_free(c);
        au_surface_free(s);
    }
    au_docview_free(dv);
    au_host_loader_free(loader);
    au_fontset_free(fonts);
    au_free(data);
    return 0;
}
