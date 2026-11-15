/* The 2D canvas context in C (src/paint/canvas2d.h): pixel conversion in
 * both byte orders, every composite operator against the formulas of the
 * Compositing and Blending specification worked out here in floating
 * point, coverage of rectangles and paths, state, clipping, limits and
 * the memory budget, the PNG encoder, and text against the font list's
 * own measurement. */
#include "host.h"
#include "image/image.h"
#include "image/pngenc.h"
#include "paint/canvas2d.h"

#include <math.h>
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

static uint32_t px(AuCanvas2D *c, int x, int y)
{
    const AuSurface *s = au_c2d_surface(c);
    return s ? s->pixels[(size_t)y * s->stride + x] : 0;
}

static bool near_px(uint32_t a, uint32_t b, int tol)
{
    for (int sh = 0; sh < 32; sh += 8)
        if (abs((int)((a >> sh) & 255) - (int)((b >> sh) & 255)) > tol)
            return false;
    return true;
}

/* ---- Byte order ---- */

static void test_byte_order(void)
{
    bool native = au_c2d_native_big_endian();
    const AuPixel src[4] = {0xFFFF0000u, 0x80402010u, 0x00000000u, 0xFF123456u};
    const uint8_t want[4][4] = {{255, 0, 0, 255}, {128, 64, 32, 128}, {0, 0, 0, 0}, {0x12, 0x34, 0x56, 255}};
    for (int order = 0; order < 2; order++) {
        bool be = order == 1;
        uint8_t out[16];
        au_c2d_row_to_rgba(src, out, 4, false, be);
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) {
                /* The word built for the other byte order has its bytes
                 * reversed in this machine's memory. */
                uint8_t got = be == native ? out[4 * i + k] : out[4 * i + 3 - k];
                CHECK(got == want[i][k], "to_rgba order %d pixel %d channel %d: %u, want %u", order, i, k, got,
                      want[i][k]);
            }
        /* And back: bytes laid out as that order's machine would hold them. */
        uint8_t in[16];
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++)
                in[be == native ? 4 * i + k : 4 * i + 3 - k] = want[i][k];
        AuPixel back[4];
        au_c2d_row_from_rgba(in, back, 4, be);
        for (int i = 0; i < 4; i++)
            CHECK(near_px(back[i], src[i], 1), "from_rgba order %d pixel %d: %08x, want %08x", order, i,
                  (unsigned)back[i], (unsigned)src[i]);
    }
    /* On this machine the bytes are R, G, B, A in memory. */
    uint8_t out[4];
    au_c2d_row_to_rgba(&src[3], out, 1, false, native);
    CHECK(out[0] == 0x12 && out[1] == 0x34 && out[2] == 0x56 && out[3] == 255, "native bytes are RGBA");
    /* An opaque read ignores alpha and returns the stored colour. */
    au_c2d_row_to_rgba(&src[1], out, 1, true, native);
    CHECK(out[0] == 0x40 && out[1] == 0x20 && out[2] == 0x10 && out[3] == 255, "opaque read");
    /* Every premultiplied value survives the round trip: unpremultiply,
     * then premultiply again. */
    int bad = 0;
    for (uint32_t a = 1; a < 256; a++)
        for (uint32_t v = 0; v <= a; v++) {
            AuPixel p = (a << 24) | (v << 16) | (v << 8) | v, q;
            uint8_t b[4];
            au_c2d_row_to_rgba(&p, b, 1, false, native);
            au_c2d_row_from_rgba(b, &q, 1, native);
            if (q != p)
                bad++;
        }
    CHECK(bad == 0, "%d premultiplied values do not round-trip", bad);
}

/* ---- Compositing ---- */

static double lum(const double c[3]) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }

static void clip_color(double c[3])
{
    double l = lum(c), n = fmin(c[0], fmin(c[1], c[2])), x = fmax(c[0], fmax(c[1], c[2]));
    for (int i = 0; i < 3; i++) {
        if (n < 0)
            c[i] = l + (c[i] - l) * l / (l - n);
        if (x > 1)
            c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}

static void set_lum(double c[3], double l)
{
    double d = l - lum(c);
    for (int i = 0; i < 3; i++)
        c[i] += d;
    clip_color(c);
}

static double sat(const double c[3]) { return fmax(c[0], fmax(c[1], c[2])) - fmin(c[0], fmin(c[1], c[2])); }

static void set_sat(double c[3], double s)
{
    int mx = 0, mn = 0;
    for (int i = 1; i < 3; i++) {
        if (c[i] > c[mx])
            mx = i;
        if (c[i] < c[mn])
            mn = i;
    }
    if (mx == mn) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    int mid = 3 - mx - mn;
    c[mid] = (c[mid] - c[mn]) * s / (c[mx] - c[mn]);
    c[mx] = s;
    c[mn] = 0;
}

static double sep(const char *op, double cb, double cs)
{
    if (!strcmp(op, "multiply"))
        return cb * cs;
    if (!strcmp(op, "screen"))
        return cb + cs - cb * cs;
    if (!strcmp(op, "overlay"))
        return cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    if (!strcmp(op, "darken"))
        return fmin(cb, cs);
    if (!strcmp(op, "lighten"))
        return fmax(cb, cs);
    if (!strcmp(op, "color-dodge"))
        return cb == 0 ? 0 : cs >= 1 ? 1 : fmin(1, cb / (1 - cs));
    if (!strcmp(op, "color-burn"))
        return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - fmin(1, (1 - cb) / cs);
    if (!strcmp(op, "hard-light"))
        return cs <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    if (!strcmp(op, "soft-light")) {
        if (cs <= 0.5)
            return cb - (1 - 2 * cs) * cb * (1 - cb);
        double d = cb <= 0.25 ? ((16 * cb - 12) * cb + 4) * cb : sqrt(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    if (!strcmp(op, "difference"))
        return fabs(cb - cs);
    return cb + cs - 2 * cb * cs; /* exclusion */
}

/* The expected premultiplied result of source s over backdrop d. */
static uint32_t expect_op(const char *op, uint32_t s, uint32_t d)
{
    double sa = (s >> 24) / 255.0, da = (d >> 24) / 255.0;
    double S[3], D[3], R[3], ra;
    for (int i = 0; i < 3; i++) {
        S[i] = ((s >> (16 - 8 * i)) & 255) / 255.0;
        D[i] = ((d >> (16 - 8 * i)) & 255) / 255.0;
    }
    static const struct {
        const char *name;
        int fa, fb; /* 0: 0, 1: 1, 2: other alpha, 3: 1 - other alpha */
    } pd[] = {{"source-over", 1, 3}, {"source-in", 2, 0},        {"source-out", 3, 0},      {"source-atop", 2, 3},
              {"destination-over", 3, 1}, {"destination-in", 0, 2}, {"destination-out", 0, 3}, {"destination-atop", 3, 2},
              {"lighter", 1, 1},     {"copy", 1, 0},             {"xor", 3, 3}};
    for (size_t k = 0; k < sizeof pd / sizeof pd[0]; k++)
        if (!strcmp(op, pd[k].name)) {
            double fa = pd[k].fa == 0 ? 0 : pd[k].fa == 1 ? 1 : pd[k].fa == 2 ? da : 1 - da;
            double fb = pd[k].fb == 0 ? 0 : pd[k].fb == 1 ? 1 : pd[k].fb == 2 ? sa : 1 - sa;
            ra = fmin(1, sa * fa + da * fb);
            for (int i = 0; i < 3; i++)
                R[i] = fmin(1, S[i] * fa + D[i] * fb);
            goto done;
        }
    {
        /* Blend modes: on the colours without alpha, then source-over. */
        double cs[3], cb[3], bl[3];
        for (int i = 0; i < 3; i++) {
            cs[i] = sa > 0 ? S[i] / sa : 0;
            cb[i] = da > 0 ? D[i] / da : 0;
        }
        if (!strcmp(op, "hue")) {
            memcpy(bl, cs, sizeof bl);
            set_sat(bl, sat(cb));
            set_lum(bl, lum(cb));
        } else if (!strcmp(op, "saturation")) {
            memcpy(bl, cb, sizeof bl);
            set_sat(bl, sat(cs));
            set_lum(bl, lum(cb));
        } else if (!strcmp(op, "color")) {
            memcpy(bl, cs, sizeof bl);
            set_lum(bl, lum(cb));
        } else if (!strcmp(op, "luminosity")) {
            memcpy(bl, cb, sizeof bl);
            set_lum(bl, lum(cs));
        } else {
            for (int i = 0; i < 3; i++)
                bl[i] = sep(op, cb[i], cs[i]);
        }
        ra = sa + da - sa * da;
        for (int i = 0; i < 3; i++)
            R[i] = (1 - da) * S[i] + (1 - sa) * D[i] + sa * da * bl[i];
    }
done:;
    uint32_t out = (uint32_t)(ra * 255 + 0.5) << 24;
    for (int i = 0; i < 3; i++)
        out |= (uint32_t)(fmin(1, fmax(0, R[i])) * 255 + 0.5) << (16 - 8 * i);
    return out;
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

static void test_composite(AuCanvasSet *set)
{
    static const char *const ops[] = {
        "source-over", "source-in", "source-out", "source-atop", "destination-over", "destination-in",
        "destination-out", "destination-atop", "lighter", "copy", "xor", "multiply", "screen", "overlay", "darken",
        "lighten", "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
        "saturation", "color", "luminosity"};
    AuCanvas2D *c = au_c2d_new(set, NULL, 2, 1, true);
    for (size_t o = 0; o < sizeof ops / sizeof ops[0]; o++) {
        CHECK(au_c2d_set_composite(c, ops[o]), "operator %s is known", ops[o]);
        int worst = 0;
        for (int trial = 0; trial < 300; trial++) {
            /* Colours with alpha from a short list, to hit 0 and 255. */
            static const uint32_t alphas[] = {0, 255, 255, 128, 64, 200, 1, 254};
            AuColor sc = (alphas[rnd() % 8] << 24) | (rnd() & 0xFFFFFF);
            AuColor dc = (alphas[rnd() % 8] << 24) | (rnd() & 0xFFFFFF);
            /* Pixel 0: the backdrop then the source with the operator;
             * pixel 1: the source alone, to read its premultiplied value. */
            au_c2d_set_composite(c, "copy");
            au_c2d_set_color(c, AU_C2D_FILL, dc);
            au_c2d_fill_rect(c, 0, 0, 1, 1);
            au_c2d_set_color(c, AU_C2D_FILL, sc);
            au_c2d_fill_rect(c, 1, 0, 1, 1);
            uint32_t d = px(c, 0, 0), s = px(c, 1, 0);
            au_c2d_save(c);
            /* keep the source pixel out of the operators that touch
             * everything */
            AuCanvasPath clip;
            au_cpath_init(&clip);
            au_cpath_rect(&clip, NULL, 0, 0, 1, 1);
            au_c2d_clip(c, &clip, AU_FILL_NONZERO);
            au_cpath_free(&clip);
            au_c2d_set_composite(c, ops[o]);
            au_c2d_fill_rect(c, 0, 0, 1, 1);
            au_c2d_restore(c);
            uint32_t got = px(c, 0, 0), want = expect_op(ops[o], s, d);
            int dmax = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                int e = abs((int)((got >> sh) & 255) - (int)((want >> sh) & 255));
                if (e > dmax)
                    dmax = e;
            }
            if (dmax > worst)
                worst = dmax;
            if (dmax > 2)
                CHECK(false, "%s: source %08x over %08x gives %08x, expected %08x", ops[o], (unsigned)s, (unsigned)d,
                      (unsigned)got, (unsigned)want);
            else
                checks++;
        }
        (void)worst;
    }
    CHECK(!au_c2d_set_composite(c, "bogus"), "an unknown operator is refused");
    CHECK(!strcmp(au_c2d_get_composite(c), "copy"), "and leaves the value");
    au_c2d_free(c);
}

/* An operator that changes uncovered pixels does so over the whole clip. */
static void test_unbounded(AuCanvasSet *set)
{
    AuCanvas2D *c = au_c2d_new(set, NULL, 20, 20, true);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFFFF0000u);
    au_c2d_fill_rect(c, 0, 0, 20, 20);
    au_c2d_set_composite(c, "source-in");
    au_c2d_set_color(c, AU_C2D_FILL, 0xFF0000FFu);
    au_c2d_fill_rect(c, 5, 5, 10, 10);
    CHECK(px(c, 10, 10) == 0xFF0000FFu, "source-in inside: %08x", (unsigned)px(c, 10, 10));
    CHECK(px(c, 1, 1) == 0, "source-in clears outside the shape: %08x", (unsigned)px(c, 1, 1));
    au_c2d_reset(c);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFFFF0000u);
    au_c2d_fill_rect(c, 0, 0, 20, 20);
    au_c2d_save(c);
    AuCanvasPath clip;
    au_cpath_init(&clip);
    au_cpath_rect(&clip, NULL, 0, 0, 10, 20);
    au_c2d_clip(c, &clip, AU_FILL_NONZERO);
    au_cpath_free(&clip);
    au_c2d_set_composite(c, "copy");
    au_c2d_set_color(c, AU_C2D_FILL, 0xFF00FF00u);
    au_c2d_fill_rect(c, 2, 2, 4, 4);
    au_c2d_restore(c);
    CHECK(px(c, 3, 3) == 0xFF00FF00u, "copy inside");
    CHECK(px(c, 8, 8) == 0, "copy clears the rest of the clip");
    CHECK(px(c, 15, 8) == 0xFFFF0000u, "and leaves pixels outside the clip");
    au_c2d_free(c);
}

/* ---- Geometry ---- */

static void test_rects(AuCanvasSet *set)
{
    AuCanvas2D *c = au_c2d_new(set, NULL, 40, 40, true);
    CHECK(au_c2d_surface(c) == NULL, "no bitmap before the first draw");
    au_c2d_clear_rect(c, 0, 0, 40, 40);
    CHECK(au_c2d_surface(c) == NULL, "clearing a fresh canvas allocates nothing");
    au_c2d_set_color(c, AU_C2D_FILL, 0xFF000000u);
    au_c2d_fill_rect(c, 10.5, 10, 4, 4.25);
    CHECK(au_c2d_surface(c) != NULL, "the first draw allocates");
    CHECK((px(c, 10, 11) >> 24) >= 126 && (px(c, 10, 11) >> 24) <= 129, "half a column: %u", (unsigned)(px(c, 10, 11) >> 24));
    CHECK(px(c, 12, 11) == 0xFF000000u, "a whole pixel");
    CHECK((px(c, 14, 11) >> 24) >= 126 && (px(c, 14, 11) >> 24) <= 129, "the right half");
    CHECK((px(c, 12, 14) >> 24) >= 62 && (px(c, 12, 14) >> 24) <= 66, "a quarter row: %u", (unsigned)(px(c, 12, 14) >> 24));
    CHECK(px(c, 9, 11) == 0 && px(c, 15, 11) == 0 && px(c, 12, 15) == 0, "nothing outside");
    /* a negative size is the same rectangle */
    au_c2d_reset(c);
    au_c2d_fill_rect(c, 20, 20, -10, -10);
    CHECK(px(c, 15, 15) == 0xFF000000u && px(c, 21, 21) == 0, "negative width and height");
    /* transforms: scale and translate stay on the rectangle path */
    au_c2d_reset(c);
    au_c2d_translate(c, 10, 5);
    au_c2d_scale(c, 2, 3);
    au_c2d_fill_rect(c, 1, 1, 2, 2);
    CHECK(px(c, 12, 8) == 0xFF000000u && px(c, 15, 13) == 0xFF000000u, "scaled rectangle covers");
    CHECK(px(c, 11, 8) == 0 && px(c, 16, 8) == 0 && px(c, 12, 14) == 0, "and ends where it should");
    /* a rotation goes through the rasterizer */
    au_c2d_reset(c);
    au_c2d_translate(c, 20, 20);
    au_c2d_rotate(c, 3.14159265358979 / 4);
    au_c2d_fill_rect(c, -8, -8, 16, 16);
    CHECK(px(c, 20, 10) == 0xFF000000u, "the rotated square's top corner");
    CHECK(px(c, 12, 12) == 0, "its old corner is outside");
    double m[6];
    au_c2d_get_transform(c, m);
    CHECK(fabs(m[0] - 0.7071) < 1e-3 && fabs(m[1] - 0.7071) < 1e-3 && m[4] == 20 && m[5] == 20, "getTransform");
    /* clearRect erases */
    au_c2d_reset(c);
    au_c2d_fill_rect(c, 0, 0, 40, 40);
    au_c2d_clear_rect(c, 10, 10, 10, 10);
    CHECK(px(c, 15, 15) == 0 && px(c, 5, 5) == 0xFF000000u, "clearRect");
    /* strokeRect */
    au_c2d_reset(c);
    au_c2d_set_color(c, AU_C2D_STROKE, 0xFF00FF00u);
    au_c2d_set_line_width(c, 2);
    au_c2d_stroke_rect(c, 10, 10, 20, 20);
    CHECK(px(c, 10, 20) == 0xFF00FF00u && px(c, 20, 30) == 0xFF00FF00u && px(c, 20, 20) == 0, "strokeRect");
    au_c2d_free(c);
}

static void test_paths(AuCanvasSet *set)
{
    AuCanvas2D *c = au_c2d_new(set, NULL, 60, 60, true);
    AuCanvasPath *p = au_c2d_path(c);
    const AuMatrix *m = au_c2d_matrix(c);
    /* Two nested squares wound the same way: nonzero fills the hole,
     * evenodd leaves it. */
    au_cpath_rect(p, m, 10, 10, 40, 40);
    au_cpath_rect(p, m, 20, 20, 20, 20);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFF0000FFu);
    au_c2d_fill(c, NULL, AU_FILL_NONZERO);
    CHECK(px(c, 30, 30) == 0xFF0000FFu && px(c, 12, 12) == 0xFF0000FFu, "nonzero");
    CHECK(au_c2d_point_in_path(c, NULL, 30, 30, AU_FILL_NONZERO), "isPointInPath nonzero");
    CHECK(!au_c2d_point_in_path(c, NULL, 30, 30, AU_FILL_EVENODD), "isPointInPath evenodd");
    CHECK(!au_c2d_point_in_path(c, NULL, 5, 5, AU_FILL_NONZERO), "outside");
    CHECK(!au_c2d_point_in_path(c, NULL, NAN, 5, AU_FILL_NONZERO), "a point that is not a number is outside");
    au_c2d_reset(c);
    au_cpath_rect(p, m, 10, 10, 40, 40);
    au_cpath_rect(p, m, 20, 20, 20, 20);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFF0000FFu);
    au_c2d_fill(c, NULL, AU_FILL_EVENODD);
    CHECK(px(c, 30, 30) == 0 && px(c, 12, 12) == 0xFF0000FFu, "evenodd");
    /* arc: a full circle of radius 20 */
    au_c2d_reset(c);
    CHECK(au_cpath_arc(p, m, 30, 30, 20, 0, 6.283185307179586, false), "arc");
    au_c2d_fill(c, NULL, AU_FILL_NONZERO);
    CHECK(px(c, 30, 30) == 0xFF000000u && px(c, 30, 12) == 0xFF000000u && px(c, 47, 30) == 0xFF000000u, "circle");
    CHECK(px(c, 45, 45) == 0 && px(c, 14, 14) == 0, "outside the circle");
    CHECK(!au_cpath_arc(p, m, 30, 30, -1, 0, 1, false), "a negative radius is an error");
    /* stroke: a 6 pixel wide horizontal line, butt caps */
    au_c2d_reset(c);
    au_cpath_move_to(p, m, 10, 30);
    au_cpath_line_to(p, m, 50, 30);
    au_c2d_set_line_width(c, 6);
    au_c2d_stroke(c, NULL);
    CHECK(px(c, 30, 28) == 0xFF000000u && px(c, 30, 32) == 0xFF000000u && px(c, 30, 26) == 0 && px(c, 30, 33) == 0,
          "stroke width");
    CHECK(px(c, 8, 30) == 0 && px(c, 51, 30) == 0, "butt caps end at the points");
    CHECK(au_c2d_point_in_stroke(c, NULL, 30, 31) && !au_c2d_point_in_stroke(c, NULL, 30, 36), "isPointInStroke");
    au_c2d_set_line_cap(c, "square");
    au_c2d_stroke(c, NULL);
    CHECK(px(c, 8, 30) == 0xFF000000u && px(c, 52, 30) == 0xFF000000u && px(c, 6, 30) == 0, "square caps");
    /* dashes */
    au_c2d_reset(c);
    au_cpath_move_to(p, m, 0, 30);
    au_cpath_line_to(p, m, 60, 30);
    au_c2d_set_line_width(c, 4);
    double dash[1] = {10};
    au_c2d_set_line_dash(c, dash, 1);
    const float *d;
    CHECK(au_c2d_get_line_dash(c, &d) == 2 && d[0] == 10 && d[1] == 10, "an odd dash list is doubled");
    au_c2d_stroke(c, NULL);
    CHECK(px(c, 5, 30) == 0xFF000000u && px(c, 15, 30) == 0 && px(c, 25, 30) == 0xFF000000u, "dashes");
    double bad[2] = {5, -1};
    au_c2d_set_line_dash(c, bad, 2);
    CHECK(au_c2d_get_line_dash(c, &d) == 2 && d[0] == 10, "a negative dash leaves the list");
    /* a non-uniform scale widens the pen across, not along */
    au_c2d_reset(c);
    au_c2d_scale(c, 1, 4);
    au_cpath_move_to(p, au_c2d_matrix(c), 10, 7.5);
    au_cpath_line_to(p, au_c2d_matrix(c), 50, 7.5);
    au_c2d_set_line_width(c, 2);
    au_c2d_stroke(c, NULL);
    CHECK(px(c, 30, 27) == 0xFF000000u && px(c, 30, 33) == 0xFF000000u && px(c, 30, 25) == 0 && px(c, 30, 35) == 0,
          "the stroke is scaled with the transform");
    /* the path keeps at most AU_C2D_MAX_PATH_OPS segments */
    au_c2d_reset(c);
    for (int i = 0; i < AU_C2D_MAX_PATH_OPS + 1000; i++)
        au_cpath_line_to(p, m, i % 60, (i * 7) % 60);
    CHECK(p->p.nops == AU_C2D_MAX_PATH_OPS, "path cap: %zu", p->p.nops);
    au_c2d_fill(c, NULL, AU_FILL_NONZERO); /* must not run away */
    au_c2d_free(c);
}

static void test_state(AuCanvasSet *set)
{
    AuCanvas2D *c = au_c2d_new(set, NULL, 30, 30, true);
    au_c2d_set_alpha(c, 0.5);
    au_c2d_set_line_width(c, 7);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFF112233u);
    au_c2d_save(c);
    au_c2d_set_alpha(c, 0.25);
    au_c2d_set_line_width(c, 3);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFFFF0000u);
    au_c2d_translate(c, 5, 5);
    AuCanvasPath clip;
    au_cpath_init(&clip);
    au_cpath_rect(&clip, NULL, 0, 0, 10, 10);
    au_c2d_clip(c, &clip, AU_FILL_NONZERO);
    au_c2d_set_alpha(c, 1);
    au_c2d_fill_rect(c, -5, -5, 30, 30);
    CHECK(px(c, 7, 7) == 0xFFFF0000u && px(c, 2, 2) == 0 && px(c, 16, 16) == 0, "the clip is in place");
    au_c2d_restore(c);
    CHECK(au_c2d_get_alpha(c) == 0.5 && au_c2d_get_line_width(c) == 7 && au_c2d_style_color(c, AU_C2D_FILL) == 0xFF112233u,
          "restore brings the values back");
    double m[6];
    au_c2d_get_transform(c, m);
    CHECK(m[4] == 0 && m[5] == 0, "and the transform");
    au_c2d_set_alpha(c, 1);
    au_c2d_fill_rect(c, 20, 20, 5, 5);
    CHECK(px(c, 22, 22) == 0xFF112233u, "and the clip");
    au_c2d_restore(c); /* nothing saved: no effect */
    au_c2d_restore(c);
    CHECK(au_c2d_get_line_width(c) == 7, "restore without save");
    /* values out of range change nothing */
    au_c2d_set_alpha(c, 2);
    au_c2d_set_alpha(c, NAN);
    au_c2d_set_line_width(c, 0);
    au_c2d_set_line_width(c, -1);
    au_c2d_set_line_width(c, INFINITY);
    au_c2d_set_miter_limit(c, 0);
    CHECK(au_c2d_get_alpha(c) == 1 && au_c2d_get_line_width(c) == 7 && au_c2d_get_miter_limit(c) == 10, "invalid values");
    au_c2d_scale(c, NAN, 1);
    au_c2d_translate(c, INFINITY, 0);
    au_c2d_get_transform(c, m);
    CHECK(m[0] == 1 && m[4] == 0, "a transform call with a value that is not finite does nothing");
    /* the stack is bounded */
    for (int i = 0; i < AU_C2D_MAX_STATES + 100; i++)
        au_c2d_save(c);
    for (int i = 0; i < AU_C2D_MAX_STATES + 100; i++)
        au_c2d_restore(c);
    /* a clip by a rounded shape, then another: both hold */
    au_c2d_reset(c);
    au_cpath_clear(&clip);
    au_cpath_arc(&clip, NULL, 15, 15, 10, 0, 6.3, false);
    au_c2d_clip(c, &clip, AU_FILL_NONZERO);
    au_cpath_clear(&clip);
    au_cpath_rect(&clip, NULL, 0, 0, 15, 30);
    au_c2d_clip(c, &clip, AU_FILL_NONZERO);
    au_c2d_fill_rect(c, 0, 0, 30, 30);
    CHECK(px(c, 10, 15) == 0xFF000000u && px(c, 20, 15) == 0 && px(c, 1, 1) == 0, "two clips intersect");
    au_cpath_free(&clip);
    au_c2d_free(c);
}

/* ---- Limits and memory ---- */

static void test_limits(void)
{
    AuCanvasSet *set = au_canvas_set_new(1024 * 1024);
    AuCanvas2D *big = au_c2d_new(set, NULL, AU_C2D_MAX_DIM + 1, 10, true);
    CHECK(au_c2d_inert(big) && au_c2d_width(big) == AU_C2D_MAX_DIM + 1, "over the limit: inert, the size is kept");
    au_c2d_fill_rect(big, 0, 0, 10, 10);
    CHECK(au_c2d_surface(big) == NULL, "an inert canvas has no bitmap");
    uint8_t rgba[16];
    memset(rgba, 9, sizeof rgba);
    au_c2d_get_image_data(big, 0, 0, 2, 2, rgba);
    CHECK(rgba[0] == 0 && rgba[15] == 0, "and reads as transparent");
    au_c2d_free(big);
    AuCanvas2D *huge = au_c2d_new(set, NULL, 4096, 4096, true);
    CHECK(au_c2d_inert(huge), "4096 x 4096 is over the byte limit");
    au_c2d_free(huge);
    /* the budget: 1 MB holds one 512 x 512 canvas, not two */
    AuCanvas2D *a = au_c2d_new(set, NULL, 512, 512, true), *b = au_c2d_new(set, NULL, 512, 512, true);
    CHECK(au_canvas_set_bytes(set) == 0, "nothing allocated yet");
    au_c2d_fill_rect(a, 0, 0, 1, 1);
    CHECK(au_canvas_set_bytes(set) == 512 * 512 * 4, "the first canvas");
    au_c2d_fill_rect(b, 0, 0, 1, 1);
    CHECK(au_c2d_surface(b) == NULL && !au_c2d_inert(b), "the second does not fit");
    CHECK(au_cbitmap_new(set, 16, 16) == NULL, "nor does a bitmap");
    au_c2d_resize(a, 10, 10);
    CHECK(au_canvas_set_bytes(set) == 0 && au_c2d_surface(a) == NULL, "resizing frees the bitmap");
    au_c2d_fill_rect(b, 0, 0, 1, 1);
    CHECK(au_c2d_surface(b) != NULL, "now the second fits");
    /* dirty marks */
    CHECK(au_canvas_set_dirty(set), "drawing marks the set");
    int n = au_canvas_set_take_dirty(set, NULL, NULL);
    CHECK(n >= 1 && !au_canvas_set_dirty(set), "take_dirty clears: %d", n);
    au_c2d_fill_rect(b, 3, 3, 2, 2);
    au_c2d_fill_rect(b, 30, 3, 2, 2);
    CHECK(au_canvas_set_take_dirty(set, NULL, NULL) == 1, "many draws, one mark");
    /* a set freed before its canvases: they draw nothing and free cleanly */
    AuCanvasBitmap *bm = au_cbitmap_new(set, 1, 1);
    au_c2d_free(a);
    au_canvas_set_free(set);
    au_c2d_fill_rect(b, 0, 0, 5, 5);
    au_c2d_draw_text(b, "x", 1, 0, 0, -1, false);
    au_c2d_resize(b, 20, 20);
    au_c2d_fill_rect(b, 0, 0, 5, 5);
    CHECK(au_c2d_surface(b) == NULL, "a detached canvas allocates nothing");
    au_c2d_free(b);
    au_cbitmap_unref(bm);
}

static void test_pixels(AuCanvasSet *set)
{
    AuCanvas2D *c = au_c2d_new(set, NULL, 8, 8, true);
    uint8_t in[4 * 4 * 4];
    for (int i = 0; i < 16; i++) {
        in[4 * i] = (uint8_t)(i * 16);
        in[4 * i + 1] = 100;
        in[4 * i + 2] = 200;
        in[4 * i + 3] = 255;
    }
    au_c2d_put_image_data(c, in, 4, 4, 2, 2, 0, 0, 4, 4);
    uint8_t out[8 * 8 * 4];
    au_c2d_get_image_data(c, 0, 0, 8, 8, out);
    CHECK(out[4 * (2 * 8 + 2)] == 0 && out[4 * (2 * 8 + 3)] == 16 && out[4 * (2 * 8 + 3) + 1] == 100 &&
              out[4 * (2 * 8 + 3) + 3] == 255,
          "put then get");
    CHECK(out[3] == 0 && out[4 * (6 * 8 + 6) + 3] == 0, "outside the data stays clear");
    /* a dirty rectangle, with a negative size and parts off the data */
    au_c2d_reset(c);
    au_c2d_put_image_data(c, in, 4, 4, 0, 0, 3, 3, -2, -2);
    au_c2d_get_image_data(c, 0, 0, 8, 8, out);
    CHECK(out[4 * (1 * 8 + 1) + 3] == 255 && out[4 * (2 * 8 + 2) + 3] == 255 && out[3] == 0 &&
              out[4 * (3 * 8 + 3) + 3] == 0,
          "dirty rectangle");
    au_c2d_put_image_data(c, in, 4, 4, -100, -100, -50, -50, 1000, 1000);
    au_c2d_put_image_data(c, in, 4, 4, 2147483647, 2147483647, -2147483647 - 1, 0, 2147483647, 2147483647);
    /* reading across the edges */
    memset(out, 7, sizeof out);
    au_c2d_get_image_data(c, -2, -2, 4, 4, out);
    CHECK(out[3] == 0 && out[4 * (3 * 4 + 3) + 3] == 255, "a read that starts outside");
    /* a canvas without alpha is opaque black and stays opaque */
    AuCanvas2D *o = au_c2d_new(set, NULL, 4, 4, false);
    au_c2d_set_color(o, AU_C2D_FILL, 0x80FF0000u);
    au_c2d_fill_rect(o, 0, 0, 2, 2);
    au_c2d_get_image_data(o, 0, 0, 4, 4, out);
    CHECK(out[3] == 255 && out[0] >= 127 && out[0] <= 129, "half red over black: %u %u", out[0], out[3]);
    CHECK(out[4 * 3 + 3] == 255 && out[4 * 3] == 0, "untouched pixels are opaque black");
    au_c2d_clear_rect(o, 0, 0, 4, 4);
    au_c2d_get_image_data(o, 0, 0, 4, 4, out);
    CHECK(out[3] == 255 && out[0] == 0, "clearRect leaves opaque black");
    au_c2d_free(o);
    au_c2d_free(c);
}

/* ---- Images, gradients, patterns ---- */

static void test_images(AuCanvasSet *set)
{
    AuCanvasBitmap *bm = au_cbitmap_new(set, 4, 4);
    CHECK(bm != NULL, "a bitmap");
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            bm->surf->pixels[y * 4 + x] = (x < 2) == (y < 2) ? 0xFFFF0000u : 0xFF0000FFu;
    AuCanvas2D *c = au_c2d_new(set, NULL, 40, 40, true);
    au_c2d_draw_image(c, bm->surf, 4, 4, false, 0, 0, 4, 4, 10, 10, 4, 4);
    CHECK(px(c, 10, 10) == 0xFFFF0000u && px(c, 12, 10) == 0xFF0000FFu && px(c, 13, 13) == 0xFFFF0000u &&
              px(c, 14, 10) == 0 && px(c, 9, 10) == 0,
          "unscaled");
    au_c2d_reset(c);
    au_c2d_set_smoothing(c, false);
    au_c2d_draw_image(c, bm->surf, 4, 4, false, 0, 0, 4, 4, 0, 0, 40, 40);
    CHECK(px(c, 5, 5) == 0xFFFF0000u && px(c, 25, 5) == 0xFF0000FFu && px(c, 35, 35) == 0xFFFF0000u, "scaled up, nearest");
    /* a source rectangle: the top-right quarter */
    au_c2d_reset(c);
    au_c2d_draw_image(c, bm->surf, 4, 4, false, 2, 0, 2, 2, 0, 0, 20, 20);
    CHECK(px(c, 3, 3) == 0xFF0000FFu && px(c, 17, 17) == 0xFF0000FFu && px(c, 22, 5) == 0, "source rectangle");
    /* rotated: the general path */
    au_c2d_reset(c);
    au_c2d_set_smoothing(c, false);
    au_c2d_translate(c, 20, 20);
    au_c2d_rotate(c, 3.14159265358979 / 2);
    au_c2d_draw_image(c, bm->surf, 4, 4, false, 0, 0, 4, 4, -10, -10, 20, 20);
    /* a quarter turn clockwise: the image's top-left ends top-right */
    CHECK(px(c, 25, 15) == 0xFFFF0000u && px(c, 15, 15) == 0xFF0000FFu, "rotated: %08x %08x", (unsigned)px(c, 25, 15),
          (unsigned)px(c, 15, 15));
    /* global alpha */
    au_c2d_reset(c);
    au_c2d_set_alpha(c, 0.5);
    au_c2d_draw_image(c, bm->surf, 4, 4, false, 0, 0, 4, 4, 0, 0, 4, 4);
    CHECK(near_px(px(c, 0, 0), 0x80800000u, 1), "half alpha: %08x", (unsigned)px(c, 0, 0));
    /* hostile numbers */
    au_c2d_draw_image(c, bm->surf, 4, 4, false, NAN, 0, 4, 4, 0, 0, 4, 4);
    au_c2d_draw_image(c, bm->surf, 4, 4, false, -1e30, -1e30, 1e30, 1e30, 1e30, 0, 1e30, 1e-30);
    au_c2d_draw_image(c, bm->surf, 4, 4, false, 0, 0, 4, 4, 0, 0, 1e9, 1e9);
    /* the canvas onto itself */
    au_c2d_reset(c);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFF00FF00u);
    au_c2d_fill_rect(c, 0, 0, 10, 10);
    au_c2d_draw_image(c, au_c2d_surface(c), 40, 40, false, 0, 0, 10, 10, 5, 5, 10, 10);
    CHECK(px(c, 12, 12) == 0xFF00FF00u && px(c, 16, 16) == 0, "self copy");
    /* a pattern */
    au_c2d_reset(c);
    AuCanvasPattern *pat = au_cpattern_new(bm, 3);
    au_c2d_set_pattern(c, AU_C2D_FILL, pat);
    au_c2d_fill_rect(c, 0, 0, 40, 40);
    CHECK(px(c, 0, 0) == 0xFFFF0000u && px(c, 2, 0) == 0xFF0000FFu && px(c, 4, 0) == 0xFFFF0000u &&
              px(c, 38, 38) == 0xFFFF0000u,
          "a repeating pattern");
    au_c2d_reset(c);
    AuCanvasPattern *once = au_cpattern_new(bm, 0);
    au_cpattern_set_transform(once, 1, 0, 0, 1, 10, 10);
    au_c2d_set_pattern(c, AU_C2D_FILL, once);
    au_c2d_fill_rect(c, 0, 0, 40, 40);
    CHECK(px(c, 10, 10) == 0xFFFF0000u && px(c, 5, 5) == 0 && px(c, 14, 14) == 0, "no-repeat with a transform");
    au_cpattern_unref(pat);
    au_cpattern_unref(once);
    /* gradients */
    au_c2d_reset(c);
    AuCanvasGradient *g = au_cgradient_linear(0, 0, 40, 0);
    au_cgradient_add_stop(g, 0, 0xFF000000u);
    au_cgradient_add_stop(g, 1, 0xFFFFFFFFu);
    au_cgradient_add_stop(g, 7, 0xFFFF0000u); /* out of range: ignored */
    au_c2d_set_gradient(c, AU_C2D_FILL, g);
    au_c2d_fill_rect(c, 0, 0, 40, 40);
    int mid = (int)(px(c, 20, 5) & 255);
    CHECK(mid >= 125 && mid <= 135 && (px(c, 0, 5) & 255) < 8 && (px(c, 39, 5) & 255) > 247, "linear gradient: %d", mid);
    au_cgradient_unref(g);
    g = au_cgradient_radial(20, 20, 0, 20, 20, 20);
    au_cgradient_add_stop(g, 0, 0xFFFF0000u);
    au_cgradient_add_stop(g, 1, 0xFF0000FFu);
    au_c2d_set_gradient(c, AU_C2D_FILL, g);
    au_c2d_fill_rect(c, 0, 0, 40, 40);
    CHECK(((px(c, 20, 20) >> 16) & 255) > 240 && (px(c, 39, 20) & 255) > 230 && (px(c, 0, 0) & 255) == 255,
          "radial gradient: %08x %08x", (unsigned)px(c, 20, 20), (unsigned)px(c, 39, 20));
    au_cgradient_unref(g);
    g = au_cgradient_conic(0, 20, 20);
    au_cgradient_add_stop(g, 0, 0xFFFF0000u);
    au_cgradient_add_stop(g, 0.5, 0xFFFF0000u);
    au_cgradient_add_stop(g, 0.5, 0xFF0000FFu);
    au_cgradient_add_stop(g, 1, 0xFF0000FFu);
    au_c2d_set_gradient(c, AU_C2D_FILL, g);
    au_c2d_fill_rect(c, 0, 0, 40, 40);
    /* from +x clockwise: the lower half is the first half of the turn */
    CHECK(px(c, 20, 30) == 0xFFFF0000u && px(c, 20, 10) == 0xFF0000FFu, "conic gradient: %08x %08x",
          (unsigned)px(c, 20, 30), (unsigned)px(c, 20, 10));
    au_cgradient_unref(g);
    /* a gradient with no extent paints nothing */
    au_c2d_reset(c);
    g = au_cgradient_linear(5, 5, 5, 5);
    au_cgradient_add_stop(g, 0, 0xFFFF0000u);
    au_c2d_set_gradient(c, AU_C2D_FILL, g);
    au_c2d_fill_rect(c, 0, 0, 40, 40);
    CHECK(px(c, 5, 5) == 0, "a zero-length gradient");
    au_cgradient_unref(g);
    /* shadows: a hard shadow 5 right and down */
    au_c2d_reset(c);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFFFF0000u);
    au_c2d_set_shadow_color(c, 0xFF0000FFu);
    au_c2d_set_shadow_offset(c, 0, 5);
    au_c2d_set_shadow_offset(c, 1, 5);
    au_c2d_fill_rect(c, 10, 10, 10, 10);
    CHECK(px(c, 12, 12) == 0xFFFF0000u && px(c, 22, 22) == 0xFF0000FFu && px(c, 26, 26) == 0 && px(c, 8, 8) == 0,
          "shadow offset");
    au_c2d_reset(c);
    au_c2d_set_shadow_color(c, 0xFF0000FFu);
    au_c2d_set_shadow_blur(c, 6);
    au_c2d_set_color(c, AU_C2D_FILL, 0xFFFF0000u);
    au_c2d_fill_rect(c, 15, 15, 10, 10);
    uint32_t edge = px(c, 13, 20) >> 24, far = px(c, 2, 20) >> 24;
    CHECK(edge > 20 && edge < 250 && far == 0, "a blurred shadow fades: %u %u", (unsigned)edge, (unsigned)far);
    au_c2d_free(c);
    au_cbitmap_unref(bm);
}

/* ---- The PNG encoder ---- */

typedef struct Rows {
    const uint8_t *rgba;
    int w;
} Rows;

static void row_fn(void *ctx, int y, uint8_t *out)
{
    Rows *r = ctx;
    memcpy(out, r->rgba + (size_t)y * (size_t)r->w * 4, (size_t)r->w * 4);
}

static void test_png(void)
{
    static const int sizes[][2] = {{1, 1}, {7, 3}, {64, 64}, {300, 150}, {1024, 40}};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int w = sizes[k][0], h = sizes[k][1];
        uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
                /* flat areas, ramps and noise, opaque (so the decoder's
                 * premultiplied pixels compare exactly) */
                int zone = (x * 4 / w + y * 2 / h) % 3;
                p[0] = zone == 0 ? 200 : zone == 1 ? (uint8_t)(x * 3) : (uint8_t)rnd();
                p[1] = zone == 0 ? 30 : zone == 1 ? (uint8_t)(y * 5) : (uint8_t)rnd();
                p[2] = zone == 0 ? 90 : (uint8_t)(x + y);
                p[3] = 255;
            }
        AuBuf png;
        au_buf_init(&png);
        Rows r = {rgba, w};
        CHECK(au_png_encode_rgba(w, h, row_fn, &r, &png), "encode %dx%d", w, h);
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(png.data, png.len, NULL, &info);
        CHECK(s && s->width == w && s->height == h, "the decoder reads the %dx%d PNG back (%zu bytes)", w, h, png.len);
        int bad = 0;
        if (s)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
                    uint32_t want = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                    if (s->pixels[(size_t)y * s->stride + x] != want)
                        bad++;
                }
        CHECK(bad == 0, "%d pixels differ after the round trip at %dx%d", bad, w, h);
        if (k == 3)
            CHECK(png.len < (size_t)w * (size_t)h * 4, "the PNG is smaller than the pixels: %zu", png.len);
        au_surface_free(s);
        au_buf_free(&png);
        free(rgba);
    }
    AuBuf none;
    au_buf_init(&none);
    CHECK(!au_png_encode_rgba(0, 5, row_fn, NULL, &none) && none.len == 0, "an empty size is refused");
    au_buf_free(&none);
}

/* ---- Text ---- */

static void test_text(AuCanvasSet *set, AuFontSet *fonts)
{
    AuCanvas2D *c = au_c2d_new(set, NULL, 200, 60, true);
    AuTextMetrics2D m;
    /* without fonts: nothing measured, nothing drawn, no crash */
    au_canvas_set_fonts(set, NULL);
    au_c2d_measure_text(c, "Hello", 5, &m);
    CHECK(m.width == 0, "no fonts, no width");
    au_c2d_draw_text(c, "Hello", 5, 10, 30, -1, false);
    CHECK(au_c2d_surface(c) == NULL, "and nothing drawn");
    au_canvas_set_fonts(set, fonts);
    au_c2d_set_font(c, "sans-serif", 400, false, 20, "20px sans-serif");
    CHECK(!strcmp(au_c2d_get_font(c), "20px sans-serif"), "the font reads back");
    au_c2d_measure_text(c, "Hello, canvas", 13, &m);
    AuFontList *l = au_fontlist_parse(fonts, "sans-serif", 400, false);
    float want = au_text_width_list(l, 20, "Hello, canvas", 13);
    au_fontlist_free(l);
    CHECK(want > 50 && fabs(m.width - want) < 0.01, "measureText is the font list's width: %g, %g", m.width, (double)want);
    CHECK(m.ascent > 10 && m.ascent < 20 && m.descent >= 0 && m.descent < 8, "ink ascent %g descent %g", m.ascent, m.descent);
    CHECK(m.font_ascent > m.ascent - 1 && m.font_descent > 0, "font box");
    CHECK(fabs(m.em_ascent + m.em_descent - 20) < 0.01, "the em box is the font size");
    CHECK(m.left <= 2 && m.right > want - 4 && m.right < want + 2, "ink left %g right %g", m.left, m.right);
    /* drawing puts ink inside the measured box and nowhere else */
    au_c2d_draw_text(c, "Hello, canvas", 13, 10, 40, -1, false);
    const AuSurface *s = au_c2d_surface(c);
    CHECK(s != NULL, "text draws");
    int minx = 1000, maxx = -1, miny = 1000, maxy = -1;
    for (int y = 0; s && y < 60; y++)
        for (int x = 0; x < 200; x++)
            if (s->pixels[y * s->stride + x] >> 24 > 40) {
                if (x < minx)
                    minx = x;
                if (x > maxx)
                    maxx = x;
                if (y < miny)
                    miny = y;
                if (y > maxy)
                    maxy = y;
            }
    CHECK(minx >= 10 - m.left - 1 && maxx <= 10 + m.right + 1, "ink x %d..%d within %g..%g", minx, maxx, 10 - m.left,
          10 + m.right);
    CHECK(miny >= 40 - m.ascent - 1 && maxy <= 40 + m.descent + 1, "ink y %d..%d within %g..%g", miny, maxy, 40 - m.ascent,
          40 + m.descent);
    /* alignment moves the ink */
    au_c2d_reset(c);
    au_c2d_set_font(c, "sans-serif", 400, false, 20, "20px sans-serif");
    au_c2d_set_text_align(c, "right");
    au_c2d_draw_text(c, "Hi", 2, 190, 40, -1, false);
    s = au_c2d_surface(c);
    maxx = -1;
    for (int y = 0; y < 60; y++)
        for (int x = 0; x < 200; x++)
            if (s->pixels[y * s->stride + x] >> 24 > 40 && x > maxx)
                maxx = x;
    CHECK(maxx <= 190 && maxx > 180, "right-aligned ink ends at the point: %d", maxx);
    /* a maximum width condenses */
    au_c2d_reset(c);
    au_c2d_set_font(c, "sans-serif", 400, false, 20, "20px sans-serif");
    au_c2d_draw_text(c, "Hello, canvas", 13, 10, 40, 50, false);
    s = au_c2d_surface(c);
    maxx = -1;
    for (int y = 0; y < 60; y++)
        for (int x = 0; x < 200; x++)
            if (s->pixels[y * s->stride + x] >> 24 > 40 && x > maxx)
                maxx = x;
    CHECK(maxx <= 61 && maxx > 50, "maxWidth: ink ends at %d", maxx);
    /* letter spacing adds to the width */
    au_c2d_set_spacing(c, 0, 3);
    AuTextMetrics2D sp;
    au_c2d_measure_text(c, "Hello, canvas", 13, &sp);
    CHECK(fabs(sp.width - (m.width + 39)) < 0.01, "letterSpacing: %g", sp.width);
    au_c2d_set_spacing(c, 0, 0);
    /* stroked text, hostile arguments */
    au_c2d_draw_text(c, "Hi", 2, 10, 40, -1, true);
    au_c2d_draw_text(c, "Hi", 2, NAN, 40, -1, false);
    au_c2d_draw_text(c, "Hi", 2, 1e30, -1e30, 1e-30, true);
    au_c2d_set_font(c, "sans-serif", 400, false, 1e9, "x");
    au_c2d_draw_text(c, "Hi", 2, 10, 40, -1, false);
    au_c2d_measure_text(c, "\xff\xfe\x80", 3, &m);
    au_canvas_set_fonts(set, NULL);
    au_c2d_free(c);
}

int main(void)
{
    AuFontSet *fonts = au_fontset_new();
    au_host_use_system_fonts(false);
    au_platform_load_fonts(fonts);
    AuCanvasSet *set = au_canvas_set_new(0);
    test_byte_order();
    test_composite(set);
    test_unbounded(set);
    test_rects(set);
    test_paths(set);
    test_state(set);
    test_limits();
    test_pixels(set);
    test_images(set);
    test_png();
    test_text(set, fonts);
    CHECK(au_canvas_set_bytes(set) == 0, "every canvas returned its bytes: %zu left", au_canvas_set_bytes(set));
    au_canvas_set_free(set);
    au_fontset_free(fonts);
    printf("canvas2d: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
