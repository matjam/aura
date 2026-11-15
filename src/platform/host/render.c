/* aura-host render and dump: an HTML file through the document view.
 *
 *   aura-host render page.html -w 800 [-h 600] [-s scrollY] -o out.png
 *                    [--font F]... [--no-system-fonts] [--repeat N]
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
    fprintf(stderr, "usage: aura-host render page.html -w W [-h H] [-s scrollY] -o out.png [--font F]...\n"
                    "       aura-host dump page.html -w W [-h H]\n");
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
        else if (!strcmp(a, "--font") && i + 1 < argc)
            au_host_add_font_path(argv[++i]);
        else if (!strcmp(a, "--no-system-fonts"))
            au_host_use_system_fonts(false);
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
    char url[4200];
    file_url(page, url, sizeof url);
    t0 = now_ms();
    au_docview_load(dv, data, len, url);
    double t_parse = now_ms() - t0;
    au_docview_set_viewport(dv, w, h ? h : 600);
    /* Deliver resources until nothing is pending (images are requested
     * during layout, so update between rounds). */
    t0 = now_ms();
    for (int round = 0; round < 20; round++) {
        au_docview_update(dv);
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
        if (!au_png_write(out, s)) {
            fprintf(stderr, "aura-host: cannot write %s\n", out);
            rc = 1;
        }
        AuLayoutStats st;
        au_layout_stats(L, &st);
        printf("%s: %dx%d (document %dx%d)\n", out, w, ph, cw, ch);
        printf("  fonts %.1f ms, parse %.2f ms, resources %.2f ms, style+layout %.2f ms, layout only %.2f ms\n",
               t_fonts, t_parse, t_res, t_layout, t_relayout);
        printf("  paint full %.2f ms (cold glyphs), %.2f ms (warm); 600px view %.2f ms\n", t_paint, t_paint2,
               t_paint_view);
        printf("  boxes %u (blocks %u, inlines %u, texts %u), lines %u, fragments %u\n", st.boxes, st.blocks,
               st.inlines, st.texts, st.lines, st.frags);
        printf("  memory: box tree %zu bytes (%.1f per box), fragments %zu bytes; word cache %u hits %u misses\n",
               st.tree_bytes, st.boxes ? (double)st.tree_bytes / st.boxes : 0.0, st.frag_bytes, st.word_hits,
               st.word_misses);
        au_canvas_free(c);
        au_surface_free(s);
    }
    au_docview_free(dv);
    au_host_loader_free(loader);
    au_fontset_free(fonts);
    au_free(data);
    return rc;
}
