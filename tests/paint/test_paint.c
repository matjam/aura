/* Tests for src/paint: rasterizer coverage, blending, clipping, groups,
 * borders, images and a golden image. Font tests are in test_font.c.
 *
 * To regenerate the golden image after an intended rendering change, run
 * the test with AURA_UPDATE_GOLDEN=1 and check the new image by eye. */
#include "test_paint.h"

#include "png.h"

#include <math.h>
#include <string.h>

/* ---- Coverage ---- */

typedef struct {
    int w, h;
    uint8_t *m;
} Mask;

static Mask raster(const AuPath *p, AuFillRule rule, int w, int h)
{
    Mask m = {w, h, au_calloc((size_t)w * h)};
    AuIRect area = {0, 0, w, h};
    au_raster_mask(p, rule, area, m.m);
    return m;
}

static double mask_sum(const Mask *m)
{
    double s = 0;
    for (int i = 0; i < m->w * m->h; i++)
        s += m->m[i];
    return s / 255.0;
}

static int at(const Mask *m, int x, int y)
{
    return m->m[y * m->w + x];
}

static void test_coverage(void)
{
    AuPath p;
    au_path_init(&p);

    /* A pixel-aligned square covers exactly its pixels. */
    au_path_rect(&p, (AuRect){2, 3, 5, 4});
    Mask m = raster(&p, AU_FILL_NONZERO, 10, 10);
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++)
            CHECK(at(&m, x, y) == ((x >= 2 && x < 7 && y >= 3 && y < 7) ? 255 : 0));
    au_free(m.m);

    /* Half-pixel edges give half coverage; corners a quarter. */
    au_path_clear(&p);
    au_path_rect(&p, (AuRect){1.5f, 1.5f, 3, 3});
    m = raster(&p, AU_FILL_NONZERO, 6, 6);
    CHECK_NEAR(at(&m, 1, 2), 128, 1);
    CHECK_NEAR(at(&m, 4, 2), 128, 1);
    CHECK_NEAR(at(&m, 2, 1), 128, 1);
    CHECK_NEAR(at(&m, 1, 1), 64, 1);
    CHECK(at(&m, 2, 2) == 255 && at(&m, 3, 3) == 255);
    CHECK_NEAR(mask_sum(&m), 9.0, 0.02);
    au_free(m.m);

    /* A triangle's coverage adds up to its area. */
    au_path_clear(&p);
    au_path_move_to(&p, 1.3f, 1.1f);
    au_path_line_to(&p, 30.7f, 4.2f);
    au_path_line_to(&p, 12.4f, 27.9f);
    au_path_close(&p);
    m = raster(&p, AU_FILL_NONZERO, 32, 32);
    double area = fabs((30.7 - 1.3) * (27.9 - 1.1) - (12.4 - 1.3) * (4.2 - 1.1)) / 2;
    CHECK_NEAR(mask_sum(&m), area, area * 0.005);
    au_free(m.m);

    /* A circle: area pi r^2, centre full, outside empty. */
    au_path_clear(&p);
    au_path_ellipse(&p, 20, 20, 15, 15);
    m = raster(&p, AU_FILL_NONZERO, 40, 40);
    CHECK_NEAR(mask_sum(&m), 3.14159265 * 225, 3.14159265 * 225 * 0.01);
    CHECK(at(&m, 20, 20) == 255 && at(&m, 2, 2) == 0 && at(&m, 20, 4) == 0);
    au_free(m.m);

    /* Two squares wound the same way: nonzero fills the inner one,
     * even-odd leaves a hole. */
    au_path_clear(&p);
    au_path_rect(&p, (AuRect){2, 2, 20, 20});
    au_path_rect(&p, (AuRect){8, 8, 8, 8});
    m = raster(&p, AU_FILL_NONZERO, 24, 24);
    CHECK(at(&m, 12, 12) == 255 && at(&m, 4, 4) == 255);
    au_free(m.m);
    m = raster(&p, AU_FILL_EVENODD, 24, 24);
    CHECK(at(&m, 12, 12) == 0 && at(&m, 4, 4) == 255);
    CHECK_NEAR(mask_sum(&m), 400 - 64, 0.5);
    au_free(m.m);

    /* A reversed inner square is a hole under nonzero too. */
    au_path_clear(&p);
    au_path_rounded_rect(&p, (AuRect){2, 2, 20, 20}, NULL, false);
    au_path_rounded_rect(&p, (AuRect){8, 8, 8, 8}, NULL, true);
    m = raster(&p, AU_FILL_NONZERO, 24, 24);
    CHECK(at(&m, 12, 12) == 0 && at(&m, 4, 4) == 255);
    au_free(m.m);

    /* Clipping on the left keeps the winding of edges outside the clip. */
    au_path_clear(&p);
    au_path_move_to(&p, -50, 5);
    au_path_line_to(&p, 30, 2);
    au_path_line_to(&p, 25, 28);
    au_path_line_to(&p, -40, 20);
    au_path_close(&p);
    Mask full = raster(&p, AU_FILL_NONZERO, 32, 32);
    uint8_t *clipped = au_calloc(22 * 32);
    au_raster_mask(&p, AU_FILL_NONZERO, (AuIRect){10, 0, 32, 32}, clipped);
    for (int y = 0; y < 32; y++)
        for (int x = 10; x < 32; x++)
            CHECK_NEAR(clipped[y * 22 + (x - 10)], at(&full, x, y), 1);
    au_free(clipped);
    au_free(full.m);

    /* Curves: a quadratic-sided lens has the area of two parabolic
     * segments, 2 * (2/3) * base * height. */
    au_path_clear(&p);
    au_path_move_to(&p, 4, 16);
    au_path_quad_to(&p, 16, 4, 28, 16);
    au_path_quad_to(&p, 16, 28, 4, 16);
    au_path_close(&p);
    m = raster(&p, AU_FILL_NONZERO, 32, 32);
    /* Flattening into n chords leaves a parabolic segment's area short by
     * 1/n^2; here n = 6 (0.08 px from the curve at most), so 2.8%. */
    CHECK_NEAR(mask_sum(&m), 2 * (2.0 / 3.0) * 24 * 6 * (1 - 1.0 / 36), 0.5);
    au_free(m.m);

    /* Degenerate input: empty paths, NaN, huge values. */
    au_path_clear(&p);
    m = raster(&p, AU_FILL_NONZERO, 8, 8);
    CHECK(mask_sum(&m) == 0);
    au_free(m.m);
    au_path_move_to(&p, NAN, 0);
    au_path_line_to(&p, 5, 5);
    au_path_line_to(&p, 1e30f, -1e30f);
    au_path_close(&p);
    m = raster(&p, AU_FILL_NONZERO, 8, 8);
    au_free(m.m);
    au_path_free(&p);
}

/* ---- Blending, clipping, groups ---- */

static void test_blend(void)
{
    AuSurface *s = au_surface_new(20, 20);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    AuCanvas *c = au_canvas_new(s);

    /* 50% red over white. */
    au_fill_rect(c, (AuRect){0, 0, 10, 10}, AU_RGBA(255, 0, 0, 128));
    AuColor px = au_surface_get(s, 5, 5);
    CHECK(px >> 24 == 255);
    CHECK_NEAR((px >> 16) & 255, 255, 1);
    CHECK_NEAR((px >> 8) & 255, 127, 2);
    CHECK_NEAR(px & 255, 127, 2);

    /* Opaque fill replaces exactly. */
    au_fill_rect(c, (AuRect){10, 0, 10, 10}, AU_RGB(1, 2, 3));
    CHECK(au_surface_get(s, 15, 5) == AU_RGB(1, 2, 3));

    /* Premultiplied storage: a transparent surface with 50% green. */
    AuSurface *t = au_surface_new(4, 4);
    AuCanvas *tc = au_canvas_new(t);
    au_fill_rect(tc, (AuRect){0, 0, 4, 4}, AU_RGBA(0, 200, 0, 128));
    CHECK_NEAR(t->pixels[0] >> 24, 128, 1);
    CHECK_NEAR((t->pixels[0] >> 8) & 255, 100, 2);
    AuColor back = au_surface_get(t, 0, 0);
    CHECK_NEAR((back >> 8) & 255, 200, 3);
    au_canvas_free(tc);
    au_surface_free(t);

    /* Clipping: a fill outside the clip leaves pixels alone, and pop
     * restores the previous clip. */
    au_surface_clear(s, AU_RGB(0, 0, 0));
    au_canvas_clip_push(c, (AuRect){5, 5, 5, 5});
    au_fill_rect(c, (AuRect){0, 0, 20, 20}, AU_RGB(255, 255, 255));
    CHECK(au_surface_get(s, 4, 4) == AU_RGB(0, 0, 0));
    CHECK(au_surface_get(s, 5, 5) == AU_RGB(255, 255, 255));
    CHECK(au_surface_get(s, 9, 9) == AU_RGB(255, 255, 255));
    CHECK(au_surface_get(s, 10, 10) == AU_RGB(0, 0, 0));
    au_canvas_clip_push(c, (AuRect){8, 8, 10, 10}); /* intersects */
    AuIRect cl = au_canvas_clip(c);
    CHECK(cl.x0 == 8 && cl.y0 == 8 && cl.x1 == 10 && cl.y1 == 10);
    au_canvas_clip_pop(c);
    au_canvas_clip_pop(c);
    cl = au_canvas_clip(c);
    CHECK(cl.x0 == 0 && cl.x1 == 20);

    /* Translation moves drawing; clips are pushed in translated space. */
    au_surface_clear(s, AU_RGB(0, 0, 0));
    au_canvas_translate(c, 3, 4);
    au_fill_rect(c, (AuRect){0, 0, 1, 1}, AU_RGB(255, 0, 0));
    CHECK(au_surface_get(s, 3, 4) == AU_RGB(255, 0, 0));
    CHECK(au_surface_get(s, 0, 0) == AU_RGB(0, 0, 0));
    au_canvas_translate(c, -3, -4);

    /* Opacity group: two overlapping opaque squares at 50% look like one
     * shape at 50% (no darker overlap). */
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_canvas_group_begin(c, (AuRect){0, 0, 20, 20});
    au_fill_rect(c, (AuRect){0, 0, 12, 12}, AU_RGB(0, 0, 0));
    au_fill_rect(c, (AuRect){6, 6, 12, 12}, AU_RGB(0, 0, 0));
    au_canvas_group_end(c, 0.5f);
    AuColor a = au_surface_get(s, 3, 3), b = au_surface_get(s, 8, 8), o = au_surface_get(s, 19, 0);
    CHECK(a == b);
    CHECK_NEAR(a & 255, 128, 2);
    CHECK(o == AU_RGB(255, 255, 255));

    /* A gradient runs from its first to its last colour along its line. */
    AuGradient g = {0};
    au_gradient_css_line(&g, (AuRect){0, 0, 20, 20}, 90);
    au_gradient_add_stop(&g, 0, AU_RGB(0, 0, 0));
    au_gradient_add_stop(&g, 1, AU_RGB(255, 255, 255));
    au_fill_rect_gradient(c, (AuRect){0, 0, 20, 20}, &g);
    CHECK((au_surface_get(s, 0, 10) & 255) < 16);
    CHECK((au_surface_get(s, 19, 10) & 255) > 240);
    CHECK_NEAR(au_surface_get(s, 10, 10) & 255, 134, 8);

    au_canvas_free(c);
    au_surface_free(s);
}

static void test_border_and_image(void)
{
    AuSurface *s = au_surface_new(40, 40);
    AuCanvas *c = au_canvas_new(s);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    AuBorder b = {{4, 4, 4, 4}, {AU_RGB(255, 0, 0), AU_RGB(0, 255, 0), AU_RGB(0, 0, 255), AU_RGB(0, 0, 0)}};
    au_draw_border(c, (AuRect){0, 0, 40, 40}, &b, NULL);
    CHECK(au_surface_get(s, 20, 1) == AU_RGB(255, 0, 0));
    CHECK(au_surface_get(s, 38, 20) == AU_RGB(0, 255, 0));
    CHECK(au_surface_get(s, 20, 38) == AU_RGB(0, 0, 255));
    CHECK(au_surface_get(s, 1, 20) == AU_RGB(0, 0, 0));
    CHECK(au_surface_get(s, 20, 20) == AU_RGB(255, 255, 255));

    /* Rounded: the corner pixel is outside the curve. */
    au_surface_clear(s, AU_RGB(255, 255, 255));
    AuRadii r = {{12, 12, 12, 12}, {12, 12, 12, 12}};
    AuBorder one = {{3, 3, 3, 3}, {AU_RGB(0, 0, 0), AU_RGB(0, 0, 0), AU_RGB(0, 0, 0), AU_RGB(0, 0, 0)}};
    au_draw_border(c, (AuRect){0, 0, 40, 40}, &one, &r);
    CHECK(au_surface_get(s, 0, 0) == AU_RGB(255, 255, 255));
    CHECK(au_surface_get(s, 20, 1) == AU_RGB(0, 0, 0));

    /* Image scaling: a 2x2 image to 4x4 nearest gives 2x2 blocks. */
    AuSurface *img = au_surface_new(2, 2);
    img->pixels[0] = 0xFFFF0000u;
    img->pixels[1] = 0xFF00FF00u;
    img->pixels[2] = 0xFF0000FFu;
    img->pixels[3] = 0xFFFFFFFFu;
    au_surface_clear(s, AU_RGB(0, 0, 0));
    au_draw_image(c, img, (AuIRect){0, 0, 2, 2}, (AuRect){0, 0, 4, 4}, AU_FILTER_NEAREST, 1);
    CHECK(au_surface_get(s, 0, 0) == AU_RGB(255, 0, 0) && au_surface_get(s, 1, 1) == AU_RGB(255, 0, 0));
    CHECK(au_surface_get(s, 2, 0) == AU_RGB(0, 255, 0) && au_surface_get(s, 3, 1) == AU_RGB(0, 255, 0));
    CHECK(au_surface_get(s, 0, 3) == AU_RGB(0, 0, 255) && au_surface_get(s, 3, 3) == AU_RGB(255, 255, 255));
    /* Bilinear at 1:1 reproduces the source. */
    au_draw_image(c, img, (AuIRect){0, 0, 2, 2}, (AuRect){10, 10, 2, 2}, AU_FILTER_BILINEAR, 1);
    CHECK(au_surface_get(s, 10, 10) == AU_RGB(255, 0, 0) && au_surface_get(s, 11, 11) == AU_RGB(255, 255, 255));
    /* Bilinear upscaled: the middle blends neighbours. */
    au_draw_image(c, img, (AuIRect){0, 0, 2, 2}, (AuRect){20, 20, 8, 8}, AU_FILTER_BILINEAR, 1);
    AuColor mid = au_surface_get(s, 23, 21);
    CHECK(((mid >> 16) & 255) > 60 && ((mid >> 8) & 255) > 60);
    au_surface_free(img);

    /* Box shadow: dark just outside the box, not under it. */
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_draw_box_shadow(c, (AuRect){10, 10, 20, 20}, NULL, 0, 0, 6, 0, AU_RGB(0, 0, 0), false);
    CHECK(au_surface_get(s, 20, 20) == AU_RGB(255, 255, 255));
    CHECK((au_surface_get(s, 8, 20) & 255) < 200);
    CHECK((au_surface_get(s, 1, 1) & 255) > 230);
    au_canvas_free(c);
    au_surface_free(s);
}

/* ---- Golden image ----
 * Shapes only (no fonts, which differ between systems). */

static void draw_golden(AuCanvas *c)
{
    au_surface_clear(au_canvas_target(c), AU_RGB(250, 250, 250));
    AuPath p;
    au_path_init(&p);
    au_fill_rect(c, (AuRect){4.25f, 4.5f, 30.5f, 20.25f}, AU_RGBA(30, 90, 200, 220));
    au_path_move_to(&p, 60, 4);
    au_path_line_to(&p, 72, 40);
    au_path_line_to(&p, 42, 16);
    au_path_line_to(&p, 78, 16);
    au_path_line_to(&p, 48, 40);
    au_path_close(&p);
    au_fill_path(c, &p, AU_FILL_EVENODD, AU_RGB(220, 120, 20));
    au_path_clear(&p);
    au_path_ellipse(&p, 105, 22, 20, 14);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGBA(20, 160, 80, 200));
    AuRadii r = {{14, 4, 20, 0}, {10, 4, 12, 0}};
    AuBorder b = {{4, 6, 3, 2}, {AU_RGB(200, 30, 30), AU_RGB(30, 160, 30), AU_RGB(30, 30, 200), AU_RGB(200, 180, 0)}};
    au_draw_box_shadow(c, (AuRect){8, 50, 60, 40}, &r, 3, 3, 8, 0, AU_RGBA(0, 0, 0, 120), false);
    au_fill_rounded_rect(c, (AuRect){8, 50, 60, 40}, &r, AU_RGB(255, 255, 240));
    au_draw_border(c, (AuRect){8, 50, 60, 40}, &b, &r);
    AuGradient g = {0};
    au_gradient_css_line(&g, (AuRect){80, 50, 70, 40}, 135);
    au_gradient_add_stop(&g, 0, AU_RGB(255, 220, 0));
    au_gradient_add_stop(&g, 1, AU_RGBA(120, 0, 200, 160));
    au_path_clear(&p);
    AuRadii r2 = {{20, 20, 20, 20}, {20, 20, 20, 20}};
    au_path_rounded_rect(&p, (AuRect){80, 50, 70, 40}, &r2, false);
    au_fill_path_gradient(c, &p, AU_FILL_NONZERO, &g);
    au_canvas_clip_push(c, (AuRect){10, 96, 60, 20});
    au_canvas_group_begin(c, (AuRect){0, 90, 160, 30});
    au_path_clear(&p);
    au_path_ellipse(&p, 30, 106, 18, 18);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(200, 0, 0));
    au_path_clear(&p);
    au_path_ellipse(&p, 50, 106, 18, 18);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(0, 0, 200));
    au_canvas_group_end(c, 0.6f);
    au_canvas_clip_pop(c);
    AuSurface *img = au_surface_new(3, 3);
    for (int i = 0; i < 9; i++)
        img->pixels[i] = (i & 1) ? 0xFF2040A0u : 0xFFF0E0C0u;
    au_draw_image(c, img, (AuIRect){0, 0, 3, 3}, (AuRect){90, 96, 18, 18}, AU_FILTER_NEAREST, 1);
    au_draw_image(c, img, (AuIRect){0, 0, 3, 3}, (AuRect){114, 96, 18, 18}, AU_FILTER_BILINEAR, 0.8f);
    au_surface_free(img);
    AuPath s;
    au_path_init(&s);
    au_path_clear(&p);
    au_path_move_to(&s, 136, 98);
    au_path_cubic_to(&s, 160, 98, 136, 118, 158, 116);
    au_path_stroke(&s, 2.5f, &p);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(40, 40, 40));
    au_path_free(&s);
    au_path_free(&p);
}

static void test_golden(void)
{
    AuSurface *s = au_surface_new(160, 120);
    AuCanvas *c = au_canvas_new(s);
    draw_golden(c);
    const char *path = AURA_PAINT_GOLDEN_DIR "/shapes.png";
    const char *update = getenv("AURA_UPDATE_GOLDEN");
    if (update && *update == '1') {
        CHECK(au_png_write(path, s));
        printf("wrote %s\n", path);
    } else {
        FILE *f = fopen(path, "rb");
        CHECK(f);
        static uint8_t buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        AuSurface *g = au_png_read_stored(buf, n);
        CHECK(g && g->width == s->width && g->height == s->height);
        /* Tolerance: up to 3 levels per channel anywhere (float rounding
         * differs between compilers and CPUs); no more than 0.5% of pixels
         * may differ at all. */
        int maxd = 0, diff = 0;
        for (int y = 0; y < s->height; y++)
            for (int x = 0; x < s->width; x++) {
                uint32_t a = s->pixels[y * s->stride + x], b = g->pixels[y * g->stride + x];
                int d = 0;
                for (int k = 0; k < 32; k += 8) {
                    int e = abs((int)((a >> k) & 255) - (int)((b >> k) & 255));
                    if (e > d)
                        d = e;
                }
                if (d)
                    diff++;
                if (d > maxd)
                    maxd = d;
            }
        if (maxd > 3 || diff > s->width * s->height / 200) {
            au_png_write("golden-actual.png", s);
            fprintf(stderr, "golden mismatch: max diff %d, %d pixels differ (wrote golden-actual.png)\n", maxd, diff);
            exit(1);
        }
        au_surface_free(g);
    }
    /* The PNG writer round-trips. */
    AuBuf b;
    au_buf_init(&b);
    au_png_encode(s, &b);
    AuSurface *back = au_png_read_stored((const uint8_t *)b.data, b.len);
    CHECK(back);
    for (int i = 0; i < 160 * 120; i++) {
        uint32_t x = s->pixels[i], y = back->pixels[i];
        if ((x >> 24) == 255)
            CHECK(x == y);
    }
    au_surface_free(back);
    au_buf_free(&b);
    au_canvas_free(c);
    au_surface_free(s);
}

int main(void)
{
    test_coverage();
    test_blend();
    test_border_and_image();
    test_golden();
    test_fonts();
    test_stroke();
    printf("paint: ok\n");
    return 0;
}
