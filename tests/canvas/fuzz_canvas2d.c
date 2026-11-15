/* Random call sequences on the 2D canvas context with hostile numbers:
 * NaN, infinities, huge and tiny values, negative sizes, long dash lists,
 * many gradient stops, long paths. Nothing is checked but that no call
 * crashes, loops without end or leaks the set's byte count; run it under
 * the sanitizers.
 *
 *   fuzz_canvas2d ITERATIONS SEED [SECONDS]
 *
 * Stops after ITERATIONS sequences or SECONDS of wall time, and prints
 * how many it ran. */
#include "host.h"
#include "paint/canvas2d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t state;

static uint32_t rnd(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static double num(void)
{
    switch (rnd() % 24) {
    case 0: return NAN;
    case 1: return INFINITY;
    case 2: return -INFINITY;
    case 3: return 1e30;
    case 4: return -1e30;
    case 5: return 1e300;
    case 6: return -1e300;
    case 7: return 1e-30;
    case 8: return 0;
    case 9: return -0.0;
    case 10: return 4294967296.0;
    case 11: return -2147483649.0;
    case 12: return (double)(rnd() % 100000) - 50000;
    case 13: return (double)(rnd() % 1000) / 1000.0;
    case 14: return 1e7;
    case 15: return -1e7;
    default: return (double)(rnd() % 200) / 2.0 - 20; /* mostly on the canvas */
    }
}

static const char *word(void)
{
    static const char *const w[] = {"source-over", "copy", "source-in", "destination-out", "xor", "multiply", "hue",
                                    "lighter", "soft-light", "luminosity", "round", "square", "butt", "miter",
                                    "bevel", "start", "end", "center", "right", "top", "middle", "bottom", "hanging",
                                    "rtl", "ltr", "", "bogus", "low", "high", "none"};
    return w[rnd() % (sizeof w / sizeof w[0])];
}

static AuColor color(void)
{
    static const uint32_t a[] = {0, 255, 255, 255, 128, 1};
    return (a[rnd() % 6] << 24) | (rnd() & 0xFFFFFF);
}

static void path_op(AuCanvasPath *p, const AuMatrix *m)
{
    switch (rnd() % 12) {
    case 0: au_cpath_move_to(p, m, num(), num()); break;
    case 1:
    case 2: au_cpath_line_to(p, m, num(), num()); break;
    case 3: au_cpath_quad_to(p, m, num(), num(), num(), num()); break;
    case 4: au_cpath_cubic_to(p, m, num(), num(), num(), num(), num(), num()); break;
    case 5: au_cpath_arc(p, m, num(), num(), num(), num(), num(), rnd() & 1); break;
    case 6: au_cpath_arc_to(p, m, num(), num(), num(), num(), num()); break;
    case 7: au_cpath_ellipse(p, m, num(), num(), num(), num(), num(), num(), num(), rnd() & 1); break;
    case 8: au_cpath_rect(p, m, num(), num(), num(), num()); break;
    case 9: {
        double r[8];
        for (int i = 0; i < 8; i++)
            r[i] = num();
        au_cpath_round_rect(p, m, num(), num(), num(), num(), r);
        break;
    }
    case 10: au_cpath_close(p); break;
    default:
        if (rnd() % 8 == 0)
            au_cpath_clear(p);
        break;
    }
}

static void sequence(AuFontSet *fonts)
{
    static const size_t budgets[] = {0, 0, 0, 4096, 65536, 1024 * 1024};
    AuCanvasSet *set = au_canvas_set_new(budgets[rnd() % 6]);
    if (rnd() % 3)
        au_canvas_set_fonts(set, fonts);
    static const int dims[] = {0, 1, 2, 7, 16, 33, 64, 100, 300, 5000, -5, 4096};
    AuCanvas2D *c = au_c2d_new(set, NULL, dims[rnd() % 12], dims[rnd() % 12], rnd() % 5 != 0);
    AuCanvas2D *other = au_c2d_new(set, NULL, 1 + (int)(rnd() % 40), 1 + (int)(rnd() % 40), true);
    au_c2d_set_color(other, AU_C2D_FILL, color());
    au_c2d_fill_rect(other, 0, 0, 20, 20);
    AuCanvasBitmap *bm = au_cbitmap_new(set, 1 + (int)(rnd() % 9), 1 + (int)(rnd() % 9));
    if (bm)
        for (int i = 0; i < bm->surf->width * bm->surf->height; i++)
            bm->surf->pixels[i] = rnd() & 1 ? 0xFF336699u : 0x40102030u;
    AuCanvasGradient *grad = NULL;
    AuCanvasPattern *pat = NULL;
    AuCanvasPath p2d;
    au_cpath_init(&p2d);
    int nops = (int)(rnd() % 60);
    for (int i = 0; i < nops; i++) {
        switch (rnd() % 44) {
        case 0: au_c2d_save(c); break;
        case 1: au_c2d_restore(c); break;
        case 2: au_c2d_scale(c, num(), num()); break;
        case 3: au_c2d_rotate(c, num()); break;
        case 4: au_c2d_translate(c, num(), num()); break;
        case 5: au_c2d_transform(c, num(), num(), num(), num(), num(), num()); break;
        case 6: au_c2d_set_transform(c, num(), num(), num(), num(), num(), num()); break;
        case 7: au_c2d_set_alpha(c, num()); break;
        case 8: au_c2d_set_composite(c, word()); break;
        case 9: au_c2d_set_color(c, rnd() & 1, color()); break;
        case 10:
            au_cgradient_unref(grad);
            grad = rnd() % 3 == 0   ? au_cgradient_linear(num(), num(), num(), num())
                   : rnd() % 2 == 0 ? au_cgradient_radial(num(), num(), fabs(num()), num(), num(), fabs(num()))
                                    : au_cgradient_conic(num(), num(), num());
            for (int k = (int)(rnd() % 6) + (rnd() % 20 == 0 ? 400 : 0); k > 0; k--)
                au_cgradient_add_stop(grad, rnd() % 5 ? (double)(rnd() % 101) / 100.0 : num(), color());
            au_c2d_set_gradient(c, rnd() & 1, grad);
            break;
        case 11:
            if (bm) {
                au_cpattern_unref(pat);
                pat = au_cpattern_new(bm, (int)(rnd() % 4));
                if (rnd() & 1)
                    au_cpattern_set_transform(pat, num(), num(), num(), num(), num(), num());
                au_c2d_set_pattern(c, rnd() & 1, pat);
            }
            break;
        case 12: au_c2d_set_line_width(c, num()); break;
        case 13: au_c2d_set_line_cap(c, word()); break;
        case 14: au_c2d_set_line_join(c, word()); break;
        case 15: au_c2d_set_miter_limit(c, num()); break;
        case 16: {
            double d[200];
            size_t n = rnd() % 8 ? rnd() % 6 : rnd() % 200;
            for (size_t k = 0; k < n; k++)
                d[k] = rnd() % 4 ? fabs(num()) : num();
            au_c2d_set_line_dash(c, d, n);
            au_c2d_set_dash_offset(c, num());
            break;
        }
        case 17:
            au_c2d_set_shadow_color(c, color());
            au_c2d_set_shadow_blur(c, num());
            au_c2d_set_shadow_offset(c, rnd() & 1, num());
            break;
        case 18: au_c2d_begin_path(c); break;
        case 19:
        case 20:
        case 21:
        case 22: {
            /* a run of path calls, sometimes a long one */
            int n = rnd() % 50 == 0 ? 3000 : 1 + (int)(rnd() % 8);
            for (int k = 0; k < n; k++)
                path_op(au_c2d_path(c), au_c2d_matrix(c));
            break;
        }
        case 23: path_op(&p2d, NULL); break;
        case 24: au_c2d_fill(c, rnd() % 4 ? NULL : &p2d, rnd() & 1 ? AU_FILL_EVENODD : AU_FILL_NONZERO); break;
        case 25: au_c2d_stroke(c, rnd() % 4 ? NULL : &p2d); break;
        case 26: au_c2d_clip(c, rnd() % 4 ? NULL : &p2d, rnd() & 1 ? AU_FILL_EVENODD : AU_FILL_NONZERO); break;
        case 27: (void)au_c2d_point_in_path(c, rnd() & 1 ? NULL : &p2d, num(), num(), AU_FILL_NONZERO); break;
        case 28: (void)au_c2d_point_in_stroke(c, rnd() & 1 ? NULL : &p2d, num(), num()); break;
        case 29: au_c2d_clear_rect(c, num(), num(), num(), num()); break;
        case 30:
        case 31: au_c2d_fill_rect(c, num(), num(), num(), num()); break;
        case 32: au_c2d_stroke_rect(c, num(), num(), num(), num()); break;
        case 33: {
            static const char *const fam[] = {"sans-serif", "serif", "monospace", "No Such Font, sans-serif", ""};
            au_c2d_set_font(c, fam[rnd() % 5], (int)(rnd() % 1200), rnd() & 1, rnd() % 6 ? (double)(rnd() % 60) : num(),
                            "x");
            au_c2d_set_text_align(c, word());
            au_c2d_set_text_baseline(c, word());
            au_c2d_set_direction(c, word());
            au_c2d_set_spacing(c, rnd() & 1, num());
            break;
        }
        case 34: {
            static const char text[] = "Hello \xc3\xa9\xf0\x9f\x98\x80 wor\tld\n\xff\xfe";
            au_c2d_draw_text(c, text, rnd() % sizeof text, num(), num(), rnd() & 1 ? -1 : num(), rnd() & 1);
            break;
        }
        case 35: {
            AuTextMetrics2D m;
            au_c2d_measure_text(c, "measure me", rnd() % 11, &m);
            break;
        }
        case 36:
        case 37: {
            const AuSurface *src = rnd() % 3 == 0 ? au_c2d_surface(c) : rnd() & 1 ? au_c2d_surface(other) : bm ? bm->surf : NULL;
            if (src)
                au_c2d_draw_image(c, src, rnd() % 6 ? src->width : (int)(rnd() % 5000), rnd() % 6 ? src->height : 3,
                                  rnd() & 1, num(), num(), num(), num(), num(), num(), num(), num());
            break;
        }
        case 38: {
            int w = (int)(rnd() % 40), h = (int)(rnd() % 40);
            uint8_t *buf = malloc((size_t)(w * h * 4) + 1);
            au_c2d_get_image_data(c, (int)(rnd() % 200) - 100, (int)(rnd() % 80) - 20, w, h, buf);
            au_c2d_put_image_data(c, buf, w, h, (int)(rnd() % 80) - 20, (int)(rnd() % 80) - 20,
                                  rnd() % 4 ? 0 : (int)(rnd() % 4000000000u), (int)(rnd() % 60) - 30, (int)(rnd() % 90) - 30,
                                  rnd() % 4 ? h : -(int)(rnd() % 2000000000u));
            free(buf);
            break;
        }
        case 39: au_c2d_resize(c, dims[rnd() % 12], dims[rnd() % 12]); break;
        case 40: au_c2d_reset(c); break;
        case 41: au_c2d_set_smoothing(c, rnd() & 1); break;
        case 42: au_cpath_add(&p2d, au_c2d_path(c), rnd() & 1 ? NULL : au_c2d_matrix(c)); break;
        default: au_cpath_add(&p2d, &p2d, NULL); break;
        }
    }
    au_canvas_set_take_dirty(set, NULL, NULL);
    au_cpath_free(&p2d);
    au_cgradient_unref(grad);
    au_cpattern_unref(pat);
    au_cbitmap_unref(bm);
    if (rnd() & 1) {
        /* the set goes first: the canvases are detached */
        au_canvas_set_free(set);
        au_c2d_fill_rect(c, 0, 0, 5, 5);
        au_c2d_free(c);
        au_c2d_free(other);
    } else {
        au_c2d_free(c);
        au_c2d_free(other);
        if (au_canvas_set_bytes(set) != 0) {
            fprintf(stderr, "fuzz_canvas2d: %zu bytes still counted after every canvas went\n", au_canvas_set_bytes(set));
            exit(1);
        }
        au_canvas_set_free(set);
    }
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 2000;
    state = argc > 2 ? (uint32_t)atol(argv[2]) : 1;
    long seconds = argc > 3 ? atol(argv[3]) : 0;
    if (!state)
        state = 1;
    AuFontSet *fonts = au_fontset_new();
    au_host_use_system_fonts(false);
    au_platform_load_fonts(fonts);
    time_t start = time(NULL);
    long done = 0;
    for (; done < iterations; done++) {
        if (seconds > 0 && (done & 15) == 0 && time(NULL) - start >= seconds)
            break;
        sequence(fonts);
    }
    au_fontset_free(fonts);
    printf("fuzz_canvas2d: %ld sequences\n", done);
    return 0;
}
