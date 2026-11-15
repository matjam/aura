/* Image drawing (src/paint au_draw_image): unscaled copies, the opaque
 * fast path, area-average downscaling, and bounds under clipping and odd
 * sizes. Pass --bench to time drawing a 1024 x 768 image. */
#include "imgtest.h"
#include "paint/paint.h"
#include <time.h>

static AuSurface *pattern(int w, int h, uint32_t seed, bool opaque)
{
    AuSurface *s = au_surface_new(w, h);
    ItRng r = {seed};
    for (int i = 0; i < w * h; i++) {
        uint32_t v = it_rng(&r);
        uint32_t a = opaque ? 255 : v >> 24;
        uint32_t cr = ((v >> 16) & 255) * a / 255, cg = ((v >> 8) & 255) * a / 255, cb = (v & 255) * a / 255;
        s->pixels[i] = a << 24 | cr << 16 | cg << 8 | cb;
    }
    s->opaque = opaque;
    return s;
}

static void fill(AuSurface *s, uint32_t p)
{
    for (int i = 0; i < s->stride * s->height; i++)
        s->pixels[i] = p;
}

static int max_diff(const AuSurface *a, const AuSurface *b)
{
    int m = 0;
    for (int i = 0; i < a->stride * a->height; i++)
        for (int sh = 0; sh < 32; sh += 8) {
            int d = abs((int)((a->pixels[i] >> sh) & 255) - (int)((b->pixels[i] >> sh) & 255));
            if (d > m)
                m = d;
        }
    return m;
}

static void draw(AuSurface *dst, const AuSurface *img, AuRect r, AuFilter f, float op)
{
    AuCanvas *c = au_canvas_new(dst);
    au_draw_image(c, img, (AuIRect){0, 0, img->width, img->height}, r, f, op);
    au_canvas_free(c);
}

static void test_opaque_path(void)
{
    /* The same opaque image drawn with and without the flag. */
    static const AuRect rects[] = {{3, 4, 50, 40}, {0, 0, 37, 29}, {-5, -7, 120, 90}, {10, 10, 13, 9}, {2, 2, 7, 5}};
    for (size_t k = 0; k < sizeof rects / sizeof rects[0]; k++)
        for (int f = 0; f < 2; f++) {
            AuSurface *img = pattern(37, 29, 7 + (uint32_t)k, true);
            AuSurface *a = au_surface_new(80, 60), *b = au_surface_new(80, 60);
            fill(a, 0xFF336699);
            fill(b, 0xFF336699);
            draw(a, img, rects[k], (AuFilter)f, 1);
            img->opaque = false;
            draw(b, img, rects[k], (AuFilter)f, 1);
            int m = max_diff(a, b);
            IT_CHECK(m <= 2, "opaque fast path differs by %d (rect %zu, filter %d)", m, k, f);
            au_surface_free(a);
            au_surface_free(b);
            au_surface_free(img);
        }
    /* Unscaled translucent image: blended, not copied. */
    AuSurface *img = au_surface_new(4, 4);
    fill(img, 0x80800000); /* half-transparent red */
    AuSurface *d = au_surface_new(8, 8);
    fill(d, 0xFF0000FF);
    draw(d, img, (AuRect){2, 2, 4, 4}, AU_FILTER_BILINEAR, 1);
    uint32_t p = d->pixels[3 * 8 + 3];
    IT_CHECK((p >> 24) == 255 && ((p >> 16) & 255) >= 126 && ((p >> 16) & 255) <= 130 && (p & 255) >= 126 &&
                 (p & 255) <= 129,
             "blended unscaled: %08x", p);
    IT_CHECK(d->pixels[0] == 0xFF0000FF && d->pixels[6 * 8 + 6] == 0xFF0000FF, "outside untouched");
    au_surface_free(img);
    au_surface_free(d);
}

static void test_reduce(void)
{
    /* A one-pixel checkerboard reduced by 2, 3, 4 and 100 is flat grey;
     * bilinear point sampling would keep stripes. */
    AuSurface *img = au_surface_new(400, 300);
    for (int y = 0; y < 300; y++)
        for (int x = 0; x < 400; x++)
            img->pixels[y * 400 + x] = (x + y) & 1 ? 0xFFFFFFFF : 0xFF000000;
    img->opaque = true;
    static const int sizes[][2] = {{200, 150}, {133, 100}, {100, 75}, {4, 3}};
    for (int k = 0; k < 4; k++) {
        AuSurface *d = au_surface_new(sizes[k][0], sizes[k][1]);
        draw(d, img, (AuRect){0, 0, (float)sizes[k][0], (float)sizes[k][1]}, AU_FILTER_BILINEAR, 1);
        int lo = 255, hi = 0;
        for (int i = 0; i < d->width * d->height; i++) {
            int g = d->pixels[i] & 255;
            lo = g < lo ? g : lo;
            hi = g > hi ? g : hi;
            IT_CHECK((d->pixels[i] >> 24) == 255, "opaque stays opaque");
        }
        /* Odd footprints (3 pixels) average to 1/3 or 2/3. */
        IT_CHECK(lo >= (k == 1 ? 70 : 120) && hi <= (k == 1 ? 185 : 135), "reduced to %dx%d: grey %d..%d",
                 sizes[k][0], sizes[k][1], lo, hi);
        au_surface_free(d);
    }
    /* Pixelated keeps nearest sampling: still black and white. */
    AuSurface *d = au_surface_new(200, 150);
    draw(d, img, (AuRect){0, 0, 200, 150}, AU_FILTER_NEAREST, 1);
    int g = d->pixels[0] & 255;
    IT_CHECK(g == 0 || g == 255, "nearest unchanged: %d", g);
    au_surface_free(d);
    au_surface_free(img);

    /* Translucent pixels average premultiplied: half transparent white
     * and transparent average to quarter alpha white. */
    img = au_surface_new(64, 64);
    for (int i = 0; i < 64 * 64; i++)
        img->pixels[i] = i & 1 ? 0x80808080 : 0;
    d = au_surface_new(16, 16);
    draw(d, img, (AuRect){0, 0, 16, 16}, AU_FILTER_BILINEAR, 1);
    uint32_t p = d->pixels[5 * 16 + 5];
    IT_CHECK((p >> 24) == 64 && (p & 255) == 64, "premultiplied average: %08x", p);
    au_surface_free(d);
    au_surface_free(img);
}

static void test_bounds(void)
{
    /* Random sizes, offsets, clips and opacities: run under ASan. */
    ItRng r = {99};
    for (int it = 0; it < 3000; it++) {
        int sw = 1 + (int)(it_rng(&r) % 300), sh = 1 + (int)(it_rng(&r) % 300);
        AuSurface *img = pattern(sw, sh, it_rng(&r), it_rng(&r) & 1);
        AuSurface *d = au_surface_new(1 + (int)(it_rng(&r) % 120), 1 + (int)(it_rng(&r) % 120));
        AuCanvas *c = au_canvas_new(d);
        if (it_rng(&r) & 1)
            au_canvas_clip_push(c, (AuRect){(float)(it_rng(&r) % 50), (float)(it_rng(&r) % 50),
                                            (float)(it_rng(&r) % 80), (float)(it_rng(&r) % 80)});
        AuIRect src = {(int)(it_rng(&r) % (unsigned)sw), (int)(it_rng(&r) % (unsigned)sh), sw, sh};
        if (it_rng(&r) & 1)
            src = (AuIRect){-10, -10, sw + 10, sh + 10};
        AuRect dst = {(float)((int)(it_rng(&r) % 200) - 60) + 0.37f, (float)((int)(it_rng(&r) % 200) - 60),
                      (float)(it_rng(&r) % 400) + 0.5f, (float)(it_rng(&r) % 400) + 0.25f};
        float op = (it_rng(&r) % 3) ? 1.0f : 0.5f;
        au_draw_image(c, img, src, dst, (AuFilter)(it_rng(&r) & 1), op);
        au_canvas_free(c);
        au_surface_free(d);
        au_surface_free(img);
    }
}

static double now_s(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

static void bench(void)
{
    AuSurface *img = pattern(1024, 768, 1, true);
    AuSurface *d = au_surface_new(1024, 768);
    static const struct {
        int w, h;
        const char *name;
    } cases[] = {{1024, 768, "unscaled"}, {800, 600, "bilinear 0.78"}, {512, 384, "box 1/2"}, {300, 225, "box 0.29"}};
    for (int k = 0; k < 4; k++)
        for (int op = 0; op < 2; op++) {
            img->opaque = op == 0;
            double t0 = now_s();
            int n = 0;
            while (now_s() - t0 < 0.3) {
                draw(d, img, (AuRect){0, 0, (float)cases[k].w, (float)cases[k].h}, AU_FILTER_BILINEAR, 1);
                n++;
            }
            double t = (now_s() - t0) / n;
            printf("%-14s %-8s %7.2f ms  %6.1f MP/s (destination)\n", cases[k].name, op ? "blended" : "opaque",
                   t * 1000, cases[k].w * cases[k].h / t / 1e6);
        }
    au_surface_free(d);
    au_surface_free(img);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        bench();
        return 0;
    }
    test_opaque_path();
    test_reduce();
    test_bounds();
    if (it_failures) {
        printf("%d failures\n", it_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
