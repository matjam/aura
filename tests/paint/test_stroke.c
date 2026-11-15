/* Tests for stroke.c and the SVG additions to gradients and masks: arcs
 * against analytic points, stroke areas with each cap and join, dashes,
 * matrices, reflected, transformed and focal gradients, and luminance
 * masks. */
#include "test_paint.h"

#include <math.h>
#include <string.h>

static double coverage(const AuPath *p, int w, int h)
{
    uint8_t *m = au_calloc((size_t)w * h);
    AuIRect area = {0, 0, w, h};
    au_raster_mask(p, AU_FILL_NONZERO, area, m);
    double s = 0;
    for (int i = 0; i < w * h; i++)
        s += m[i];
    au_free(m);
    return s / 255.0;
}

static double stroke_area(const AuPath *in, const AuStrokeStyle *st, int w, int h)
{
    AuPath out;
    au_path_init(&out);
    au_path_stroke_style(in, st, &out);
    double a = coverage(&out, w, h);
    au_path_free(&out);
    return a;
}

/* A point on the cubic segment of an arc path, by op index and t. */
static void cubic_at(const AuPath *p, size_t seg, float t, float *x, float *y)
{
    const float *pt = p->pts;
    float cx = 0, cy = 0;
    size_t k = 0;
    for (size_t i = 0; i < p->nops; i++) {
        if (p->ops[i] == AU_PATH_MOVE || p->ops[i] == AU_PATH_LINE) {
            cx = pt[0], cy = pt[1];
            pt += 2;
        } else if (p->ops[i] == AU_PATH_CUBIC) {
            if (k++ == seg) {
                float mt = 1 - t;
                *x = mt * mt * mt * cx + 3 * mt * mt * t * pt[0] + 3 * mt * t * t * pt[2] + t * t * t * pt[4];
                *y = mt * mt * mt * cy + 3 * mt * mt * t * pt[1] + 3 * mt * t * t * pt[3] + t * t * t * pt[5];
                return;
            }
            cx = pt[4], cy = pt[5];
            pt += 6;
        }
    }
    CHECK(!"segment not found");
}

static void test_arcs(void)
{
    /* Half circle of radius 10 from (0,0) to (20,0), sweep 1 (clockwise in
     * y-down), centre (10, 0): every point is 10 from the centre. */
    AuPath p;
    au_path_init(&p);
    au_path_move_to(&p, 0, 0);
    au_path_arc_svg(&p, 10, 10, 0, false, true, 20, 0);
    size_t ncubic = 0;
    for (size_t i = 0; i < p.nops; i++)
        ncubic += p.ops[i] == AU_PATH_CUBIC;
    CHECK(ncubic == 2);
    for (size_t s = 0; s < ncubic; s++)
        for (int k = 0; k <= 8; k++) {
            float x, y;
            cubic_at(&p, s, k / 8.0f, &x, &y);
            CHECK_NEAR(hypotf(x - 10, y), 10, 0.003);
            CHECK(y <= 0.001f); /* sweep 1 from left to right goes over the top in y-down */
        }
    CHECK_NEAR(p.cx, 20, 1e-5);
    CHECK_NEAR(p.cy, 0, 1e-5);

    /* Radii too small: scaled up to reach (a half ellipse). */
    au_path_clear(&p);
    au_path_move_to(&p, 0, 0);
    au_path_arc_svg(&p, 1, 1, 0, false, false, 20, 0);
    float x, y;
    cubic_at(&p, 0, 1.0f, &x, &y);
    CHECK_NEAR(hypotf(x - 10, y), 10, 0.01);
    cubic_at(&p, 0, 0.5f, &x, &y);
    CHECK(y > 0); /* sweep 0: below */

    /* Large arc of a rotated ellipse: endpoints exact, points on the
     * ellipse (rx 20, ry 10, rotated 30 degrees). */
    au_path_clear(&p);
    au_path_move_to(&p, 10, 30);
    au_path_arc_svg(&p, 20, 10, 30, true, true, 40, 20);
    CHECK_NEAR(p.cx, 40, 1e-4);
    CHECK_NEAR(p.cy, 20, 1e-4);
    /* Find the centre from the first two points is complex; check each
     * sampled point against the ellipse equation with the centre solved
     * the way F.6.5 does, recomputed here in double. */
    {
        double phi = 30 * M_PI / 180, cs = cos(phi), sn = sin(phi);
        double dx = (10 - 40) / 2.0, dy = (30 - 20) / 2.0;
        double x1p = cs * dx + sn * dy, y1p = -sn * dx + cs * dy;
        double rx = 20, ry = 10;
        double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (lam > 1)
            rx *= sqrt(lam), ry *= sqrt(lam);
        double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
        double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
        double co = num > 0 ? sqrt(num / den) : 0; /* large == sweep: negative */
        co = -co;
        double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
        double ccx = cs * cxp - sn * cyp + 25, ccy = sn * cxp + cs * cyp + 25;
        size_t n = 0;
        for (size_t i = 0; i < p.nops; i++)
            n += p.ops[i] == AU_PATH_CUBIC;
        CHECK(n >= 2 && n <= 4);
        for (size_t s = 0; s < n; s++)
            for (int k = 0; k <= 4; k++) {
                cubic_at(&p, s, k / 4.0f, &x, &y);
                double ux = x - ccx, uy = y - ccy;
                double ex = cs * ux + sn * uy, ey = -sn * ux + cs * uy;
                CHECK_NEAR(ex * ex / (rx * rx) + ey * ey / (ry * ry), 1, 0.003);
            }
    }
    /* Zero radius: a straight line. Same endpoint: nothing. */
    au_path_clear(&p);
    au_path_move_to(&p, 0, 0);
    au_path_arc_svg(&p, 0, 5, 0, false, true, 10, 10);
    CHECK(p.nops == 2 && p.ops[1] == AU_PATH_LINE);
    au_path_arc_svg(&p, 5, 5, 0, false, true, 10, 10);
    CHECK(p.nops == 2);
    /* Non-finite input does not hang or produce NaN points. */
    au_path_arc_svg(&p, NAN, 5, 0, false, true, 20, 20);
    au_path_arc_svg(&p, 1e30f, 1e30f, 1e30f, true, true, 0, 0);
    for (size_t i = 0; i < p.npts; i++)
        CHECK(isfinite(p.pts[i]));
    au_path_free(&p);
}

static void test_caps_joins(void)
{
    AuPath line;
    au_path_init(&line);
    au_path_move_to(&line, 20, 30);
    au_path_line_to(&line, 60, 30);
    AuStrokeStyle st = {0};
    st.width = 10;
    /* Butt: 40 x 10. Square: 50 x 10. Round: 40 x 10 + a circle. */
    CHECK_NEAR(stroke_area(&line, &st, 100, 60), 400, 1.0);
    st.cap = AU_CAP_SQUARE;
    CHECK_NEAR(stroke_area(&line, &st, 100, 60), 500, 1.0);
    st.cap = AU_CAP_ROUND;
    CHECK_NEAR(stroke_area(&line, &st, 100, 60), 400 + M_PI * 25, 2.0);

    /* A right angle: (10,10) -> (50,10) -> (50,50), width 10. The two
     * butt rectangles overlap in a 5x5 square; a miter join adds the
     * outer 5x5 square, a bevel half of it, round a quarter circle. */
    AuPath corner;
    au_path_init(&corner);
    au_path_move_to(&corner, 10, 10);
    au_path_line_to(&corner, 50, 10);
    au_path_line_to(&corner, 50, 50);
    double base = 40 * 10 + 40 * 10 - 25;
    st.cap = AU_CAP_BUTT;
    st.join = AU_JOIN_MITER;
    st.miter_limit = 4;
    CHECK_NEAR(stroke_area(&corner, &st, 80, 80), base + 25, 1.5);
    st.join = AU_JOIN_BEVEL;
    CHECK_NEAR(stroke_area(&corner, &st, 80, 80), base + 12.5, 1.5);
    st.join = AU_JOIN_ROUND;
    CHECK_NEAR(stroke_area(&corner, &st, 80, 80), base + M_PI * 25 / 4, 1.5);
    /* Miter limit: a right angle has ratio sqrt(2); below it, a bevel. */
    st.join = AU_JOIN_MITER;
    st.miter_limit = 1.4f;
    CHECK_NEAR(stroke_area(&corner, &st, 80, 80), base + 12.5, 1.5);

    /* A closed square, width 4, miter: the outer square minus the inner. */
    AuPath sq;
    au_path_init(&sq);
    au_path_rect(&sq, (AuRect){10, 10, 40, 40});
    st.width = 4;
    st.miter_limit = 4;
    CHECK_NEAR(stroke_area(&sq, &st, 60, 60), 44 * 44 - 36 * 36, 2.0);
    /* Zero-length subpaths: a dot for round and square caps only. */
    AuPath dot;
    au_path_init(&dot);
    au_path_move_to(&dot, 30, 30);
    au_path_line_to(&dot, 30, 30);
    st.width = 10;
    st.cap = AU_CAP_BUTT;
    CHECK_NEAR(stroke_area(&dot, &st, 60, 60), 0, 0.01);
    st.cap = AU_CAP_ROUND;
    CHECK_NEAR(stroke_area(&dot, &st, 60, 60), M_PI * 25, 1.5);
    st.cap = AU_CAP_SQUARE;
    CHECK_NEAR(stroke_area(&dot, &st, 60, 60), 100, 1.5);
    /* A lone moveto draws nothing. */
    au_path_clear(&dot);
    au_path_move_to(&dot, 30, 30);
    CHECK_NEAR(stroke_area(&dot, &st, 60, 60), 0, 0.01);
    au_path_free(&dot);
    au_path_free(&sq);
    au_path_free(&corner);
    au_path_free(&line);
}

static void test_dashes(void)
{
    AuPath line;
    au_path_init(&line);
    au_path_move_to(&line, 0, 20);
    au_path_line_to(&line, 100, 20);
    AuStrokeStyle st = {0};
    st.width = 4;
    float d[] = {10, 10};
    st.dashes = d;
    st.ndashes = 2;
    /* Half of 100 x 4. */
    CHECK_NEAR(stroke_area(&line, &st, 100, 40), 200, 1.0);
    /* Offset 5: dashes at 0..5, 15..25, ... 95..100: still half. */
    st.dash_offset = 5;
    CHECK_NEAR(stroke_area(&line, &st, 100, 40), 200, 1.0);
    /* An odd list repeats: 10 on, 5 off, 10 off... "10 5" twice: 10,5,10,5. */
    float d3[] = {10, 5, 5};
    st.dashes = d3;
    st.ndashes = 3;
    st.dash_offset = 0;
    /* pattern 10 on 5 off 5 on 10 off 5 on 5 off = 40 long, 20 on per 40. */
    CHECK_NEAR(stroke_area(&line, &st, 100, 40), 4 * (20 + 20 + 15), 1.5);
    /* Zero-length dashes with round caps make dots. */
    float dots[] = {0, 20};
    st.dashes = dots;
    st.ndashes = 2;
    st.cap = AU_CAP_ROUND;
    double a = stroke_area(&line, &st, 120, 40);
    /* dots at 0, 20, ... 100, the first half clipped at x 0 (small circles
     * flatten a few percent inside) */
    CHECK_NEAR(a, 5.5 * M_PI * 4, 4.0);
    /* Negative and all-zero lists stroke solid; tiny patterns are capped. */
    float bad[] = {5, -1};
    st.dashes = bad;
    st.cap = AU_CAP_BUTT;
    CHECK_NEAR(stroke_area(&line, &st, 100, 40), 400, 1.0);
    float zero[] = {0, 0};
    st.dashes = zero;
    CHECK_NEAR(stroke_area(&line, &st, 100, 40), 400, 1.0);
    float tiny[] = {1e-6f, 1e-6f};
    st.dashes = tiny;
    AuPath out;
    au_path_init(&out);
    au_path_stroke_style(&line, &st, &out);
    CHECK(out.nops < 300000);
    au_path_free(&out);
    au_path_free(&line);
}

static void test_matrix(void)
{
    AuMatrix m = {2, 0, 0, 3, 10, 20}, n = {0, 1, -1, 0, 0, 0}, inv;
    au_matrix_multiply(&m, &n); /* rotate 90 then scale/translate */
    CHECK(au_matrix_invert(&m, &inv));
    AuMatrix id = m;
    au_matrix_multiply(&id, &inv);
    CHECK_NEAR(id.a, 1, 1e-5);
    CHECK_NEAR(id.b, 0, 1e-5);
    CHECK_NEAR(id.c, 0, 1e-5);
    CHECK_NEAR(id.d, 1, 1e-5);
    CHECK_NEAR(id.e, 0, 1e-4);
    CHECK_NEAR(id.f, 0, 1e-4);
    AuMatrix sing = {1, 2, 2, 4, 0, 0};
    CHECK(!au_matrix_invert(&sing, &inv));
    AuPath p;
    au_path_init(&p);
    au_path_rect(&p, (AuRect){0, 0, 10, 5});
    AuMatrix s = {2, 0, 0, 2, 1, 1};
    au_path_transform(&p, &s);
    float b[4];
    CHECK(au_path_bounds(&p, b));
    CHECK_NEAR(b[0], 1, 1e-6);
    CHECK_NEAR(b[3], 11, 1e-6);
    CHECK_NEAR(coverage(&p, 40, 40), 200, 0.5);
    au_path_free(&p);
}

static AuColor px(AuSurface *s, int x, int y) { return au_surface_get(s, x, y); }

static void test_svg_gradients(void)
{
    AuSurface *s = au_surface_new(100, 20);
    AuCanvas *c = au_canvas_new(s);
    /* Reflect: 0..25 black to white, 25..50 back to black. */
    AuGradient g;
    memset(&g, 0, sizeof g);
    g.x0 = 0, g.y0 = 0, g.x1 = 25, g.y1 = 0;
    g.spread = AU_SPREAD_REFLECT;
    au_gradient_add_stop(&g, 0, AU_RGB(0, 0, 0));
    au_gradient_add_stop(&g, 1, AU_RGB(255, 255, 255));
    au_fill_rect_gradient(c, (AuRect){0, 0, 100, 20}, &g);
    CHECK(((px(s, 1, 5) >> 16) & 255) < 20);
    CHECK(((px(s, 24, 5) >> 16) & 255) > 235);
    CHECK(((px(s, 26, 5) >> 16) & 255) > 235);
    CHECK(((px(s, 49, 5) >> 16) & 255) < 20);
    CHECK_NEAR((px(s, 37, 5) >> 16) & 255, 128, 16);
    /* Pad (default): past the end stays white. */
    g.spread = AU_SPREAD_PAD;
    au_fill_rect_gradient(c, (AuRect){0, 0, 100, 20}, &g);
    CHECK(((px(s, 80, 5) >> 16) & 255) > 250);
    /* A matrix: the same gradient scaled by 2 in x reaches white at 50. */
    g.has_matrix = true;
    g.matrix = (AuMatrix){2, 0, 0, 1, 0, 0};
    au_fill_rect_gradient(c, (AuRect){0, 0, 100, 20}, &g);
    CHECK_NEAR((px(s, 25, 5) >> 16) & 255, 128, 12);
    CHECK(((px(s, 51, 5) >> 16) & 255) > 245);
    /* Radial with focal point at the left edge of the circle: the colour
     * at the focal point is the first stop, at the circle the last. */
    memset(&g, 0, sizeof g);
    g.radial = true;
    g.x0 = 50, g.y0 = 10, g.rx = g.ry = 40;
    g.focal = true;
    g.fx = 20, g.fy = 10;
    au_gradient_add_stop(&g, 0, AU_RGB(255, 0, 0));
    au_gradient_add_stop(&g, 1, AU_RGB(0, 0, 255));
    au_fill_rect_gradient(c, (AuRect){0, 0, 100, 20}, &g);
    AuColor at_f = px(s, 20, 10), right = px(s, 88, 10), mid = px(s, 50, 10);
    CHECK(((at_f >> 16) & 255) > 220);
    CHECK((right & 255) > 220);
    /* Halfway between focus and right edge (t = 0.5 at x 55). */
    CHECK_NEAR((px(s, 55, 10) >> 16) & 255, 128, 16);
    CHECK(((mid >> 16) & 255) > (mid & 255)); /* the centre is nearer the focus side */
    /* Focal point outside the circle is moved inside: no NaN colours. */
    g.fx = -500;
    au_fill_rect_gradient(c, (AuRect){0, 0, 100, 20}, &g);
    CHECK((px(s, 50, 10) >> 24) == 255);
    au_canvas_free(c);
    au_surface_free(s);
}

static void test_mask_end(void)
{
    AuSurface *s = au_surface_new(40, 20);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    AuCanvas *c = au_canvas_new(s);
    /* Content: red everywhere. Mask: left half white, right half 50% grey
     * (luminance 0.5) with full alpha. */
    au_canvas_group_begin(c, (AuRect){0, 0, 40, 20});
    au_fill_rect(c, (AuRect){0, 0, 40, 20}, AU_RGB(255, 0, 0));
    au_canvas_group_begin(c, (AuRect){0, 0, 40, 20});
    au_fill_rect(c, (AuRect){0, 0, 20, 20}, AU_RGB(255, 255, 255));
    au_fill_rect(c, (AuRect){20, 0, 20, 20}, AU_RGB(128, 128, 128));
    au_canvas_mask_end(c, true);
    au_canvas_group_end(c, 1.0f);
    CHECK(px(s, 5, 5) == AU_RGB(255, 0, 0));
    AuColor h = px(s, 30, 5);
    CHECK_NEAR((h >> 8) & 255, 127, 4); /* white blended with half red: green channel half */
    CHECK(((h >> 16) & 255) == 255);
    /* Alpha mask: a circle path gives a disc. */
    au_surface_clear(s, AU_RGB(255, 255, 255));
    au_canvas_group_begin(c, (AuRect){0, 0, 40, 20});
    au_fill_rect(c, (AuRect){0, 0, 40, 20}, AU_RGB(0, 0, 255));
    au_canvas_group_begin(c, (AuRect){0, 0, 40, 20});
    AuPath p;
    au_path_init(&p);
    au_path_ellipse(&p, 10, 10, 8, 8);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGBA(0, 0, 0, 255));
    au_canvas_mask_end(c, false);
    au_canvas_group_end(c, 1.0f);
    CHECK(px(s, 10, 10) == AU_RGB(0, 0, 255));
    CHECK(px(s, 30, 10) == AU_RGB(255, 255, 255));
    au_path_free(&p);
    au_canvas_free(c);
    au_surface_free(s);
}

void test_stroke(void)
{
    test_arcs();
    test_caps_joins();
    test_dashes();
    test_matrix();
    test_svg_gradients();
    test_mask_end();
}
