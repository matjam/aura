/* Matrices, path transforms, SVG arcs, and stroking with caps, joins and
 * dashes.
 *
 * The stroker flattens each subpath to a polyline at the style's
 * tolerance, cuts it into dashes, and emits one convex piece per segment,
 * join and cap. Every piece is wound the same way (positive area in y-down
 * coordinates, like au_path_ellipse), so a nonzero fill of the output is
 * their union. This is simpler than offsetting curves and exact for the
 * rasterizer's purposes; where pieces overlap at an antialiased edge the
 * coverage saturates instead of adding twice. */
#include "paint_internal.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f

/* ---- Matrices ---- */

void au_matrix_multiply(AuMatrix *m, const AuMatrix *n)
{
    AuMatrix r = {m->a * n->a + m->c * n->b,        m->b * n->a + m->d * n->b,
                  m->a * n->c + m->c * n->d,        m->b * n->c + m->d * n->d,
                  m->a * n->e + m->c * n->f + m->e, m->b * n->e + m->d * n->f + m->f};
    *m = r;
}

bool au_matrix_invert(const AuMatrix *m, AuMatrix *out)
{
    double det = (double)m->a * m->d - (double)m->b * m->c;
    if (!(fabs(det) > 1e-12) || !isfinite(det))
        return false;
    double id = 1.0 / det;
    AuMatrix r;
    r.a = (float)(m->d * id);
    r.b = (float)(-m->b * id);
    r.c = (float)(-m->c * id);
    r.d = (float)(m->a * id);
    r.e = (float)(((double)m->c * m->f - (double)m->d * m->e) * id);
    r.f = (float)(((double)m->b * m->e - (double)m->a * m->f) * id);
    *out = r;
    return true;
}

void au_path_transform(AuPath *p, const AuMatrix *m)
{
    for (size_t i = 0; i + 1 < p->npts; i += 2) {
        float x = p->pts[i], y = p->pts[i + 1];
        p->pts[i] = m->a * x + m->c * y + m->e;
        p->pts[i + 1] = m->b * x + m->d * y + m->f;
    }
    float x = p->cx, y = p->cy;
    p->cx = m->a * x + m->c * y + m->e;
    p->cy = m->b * x + m->d * y + m->f;
    x = p->sx, y = p->sy;
    p->sx = m->a * x + m->c * y + m->e;
    p->sy = m->b * x + m->d * y + m->f;
}

bool au_path_bounds(const AuPath *p, float b[4])
{
    if (p->npts < 2)
        return false;
    b[0] = b[2] = p->pts[0];
    b[1] = b[3] = p->pts[1];
    for (size_t i = 2; i + 1 < p->npts; i += 2) {
        float x = p->pts[i], y = p->pts[i + 1];
        if (x < b[0])
            b[0] = x;
        if (x > b[2])
            b[2] = x;
        if (y < b[1])
            b[1] = y;
        if (y > b[3])
            b[3] = y;
    }
    return true;
}

/* ---- Arcs (SVG 1.1 F.6.5 and F.6.6) ---- */

static float vec_angle(float ux, float uy, float vx, float vy)
{
    float a = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
    return a;
}

void au_path_arc_svg(AuPath *p, float rx, float ry, float rot_deg, bool large, bool sweep, float x, float y)
{
    float x1 = p->cx, y1 = p->cy;
    if (x1 == x && y1 == y)
        return; /* F.6.2: identical endpoints draw nothing */
    rx = fabsf(rx);
    ry = fabsf(ry);
    if (!(rx > 0) || !(ry > 0) || !isfinite(rx) || !isfinite(ry)) {
        au_path_line_to(p, x, y);
        return;
    }
    float phi = fmodf(rot_deg, 360.0f) * PI_F / 180.0f;
    float cs = cosf(phi), sn = sinf(phi);
    float dx = (x1 - x) / 2, dy = (y1 - y) / 2;
    float x1p = cs * dx + sn * dy, y1p = -sn * dx + cs * dy;
    /* Scale radii up when they cannot reach (F.6.6). */
    float lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1) {
        float s = sqrtf(lam);
        rx *= s;
        ry *= s;
    }
    float rx2 = rx * rx, ry2 = ry * ry;
    float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float coef = (den > 0 && num > 0) ? sqrtf(num / den) : 0;
    if (large == sweep)
        coef = -coef;
    float cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    float cx = cs * cxp - sn * cyp + (x1 + x) / 2;
    float cy = sn * cxp + cs * cyp + (y1 + y) / 2;
    float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    float th1 = vec_angle(1, 0, ux, uy);
    float dth = vec_angle(ux, uy, vx, vy);
    if (!sweep && dth > 0)
        dth -= 2 * PI_F;
    else if (sweep && dth < 0)
        dth += 2 * PI_F;
    if (!isfinite(dth) || !isfinite(cx) || !isfinite(cy)) {
        au_path_line_to(p, x, y);
        return;
    }
    int nseg = (int)ceilf(fabsf(dth) / (PI_F / 2) - 1e-3f);
    if (nseg < 1)
        nseg = 1;
    if (nseg > 4)
        nseg = 4;
    float d = dth / nseg;
    float k = 4.0f / 3.0f * tanf(d / 4);
    float t = th1;
    for (int i = 0; i < nseg; i++) {
        float c0 = cosf(t), s0 = sinf(t), c1 = cosf(t + d), s1 = sinf(t + d);
        /* On the unit circle: start, two controls, end; then scale, rotate,
         * translate. */
        float ex[3] = {c0 - k * s0, c1 + k * s1, c1};
        float ey[3] = {s0 + k * c0, s1 - k * c1, s1};
        float px[3], py[3];
        for (int j = 0; j < 3; j++) {
            float ax = ex[j] * rx, ay = ey[j] * ry;
            px[j] = cs * ax - sn * ay + cx;
            py[j] = sn * ax + cs * ay + cy;
        }
        if (i == nseg - 1) {
            px[2] = x; /* land exactly on the endpoint */
            py[2] = y;
        }
        au_path_cubic_to(p, px[0], py[0], px[1], py[1], px[2], py[2]);
        t += d;
    }
}

/* ---- Polylines ---- */

typedef struct {
    float *p; /* x, y pairs */
    size_t n, cap;
} Poly;

static void poly_add(Poly *pl, float x, float y)
{
    if (pl->n && pl->p[2 * pl->n - 2] == x && pl->p[2 * pl->n - 1] == y)
        return;
    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 64;
        pl->p = au_realloc(pl->p, pl->cap * 2 * sizeof(float));
    }
    pl->p[2 * pl->n] = x;
    pl->p[2 * pl->n + 1] = y;
    pl->n++;
}

static int curve_steps(float dd, float tol)
{
    if (!(dd > 0))
        return 1;
    float n = ceilf(sqrtf(dd / tol));
    if (!(n >= 1))
        return 1;
    return n > 256 ? 256 : (int)n;
}

/* ---- Pieces ---- */

/* Appends a convex polygon wound with positive area. */
static void add_convex(AuPath *out, const float *xy, int n)
{
    double area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (double)xy[2 * i] * xy[2 * j + 1] - (double)xy[2 * j] * xy[2 * i + 1];
    }
    if (!(fabs(area) > 1e-12))
        return;
    if (area > 0) {
        au_path_move_to(out, xy[0], xy[1]);
        for (int i = 1; i < n; i++)
            au_path_line_to(out, xy[2 * i], xy[2 * i + 1]);
    } else {
        au_path_move_to(out, xy[2 * (n - 1)], xy[2 * (n - 1) + 1]);
        for (int i = n - 2; i >= 0; i--)
            au_path_line_to(out, xy[2 * i], xy[2 * i + 1]);
    }
    au_path_close(out);
}

static void add_circle(AuPath *out, float x, float y, float r)
{
    au_path_ellipse(out, x, y, r, r);
}

typedef struct {
    float hw, tol, miter;
    int cap, join;
} StrokeCtx;

static void add_cap(const StrokeCtx *s, AuPath *out, float x, float y, float dx, float dy)
{
    /* (dx, dy): unit direction pointing out of the line at this end. */
    if (s->cap == AU_CAP_ROUND) {
        add_circle(out, x, y, s->hw);
    } else if (s->cap == AU_CAP_SQUARE) {
        float nx = -dy * s->hw, ny = dx * s->hw;
        float ex = dx * s->hw, ey = dy * s->hw;
        float q[8] = {x + nx, y + ny, x + nx + ex, y + ny + ey, x - nx + ex, y - ny + ey, x - nx, y - ny};
        add_convex(out, q, 4);
    }
}

static void add_join(const StrokeCtx *s, AuPath *out, float vx, float vy, float d0x, float d0y, float d1x,
                     float d1y)
{
    float cross = d0x * d1y - d0y * d1x;
    float dot = d0x * d1x + d0y * d1y;
    if (fabsf(cross) < 1e-6f && dot > 0)
        return; /* straight on */
    float sgn = cross > 0 ? -1.0f : 1.0f; /* the outer side */
    float n0x = -d0y * s->hw * sgn, n0y = d0x * s->hw * sgn;
    float n1x = -d1y * s->hw * sgn, n1y = d1x * s->hw * sgn;
    float cos_half = sqrtf((1 + (dot > 1 ? 1 : dot < -1 ? -1 : dot)) / 2);
    int join = s->join;
    /* Where the outer corner deviates from a bevel by less than the
     * tolerance, the bevel is enough. */
    if (join == AU_JOIN_ROUND && s->hw * (1 - cos_half) < s->tol)
        join = AU_JOIN_BEVEL;
    if (join == AU_JOIN_ROUND) {
        add_circle(out, vx, vy, s->hw);
        return;
    }
    if (join == AU_JOIN_MITER && cos_half > 1e-4f && 1.0f / cos_half <= s->miter) {
        float mx = n0x + n1x, my = n0y + n1y;
        float ml = sqrtf(mx * mx + my * my);
        if (ml > 1e-9f) {
            float len = s->hw / cos_half;
            float tx = vx + mx / ml * len, ty = vy + my / ml * len;
            float q[8] = {vx, vy, vx + n0x, vy + n0y, tx, ty, vx + n1x, vy + n1y};
            add_convex(out, q, 4);
            return;
        }
    }
    float q[6] = {vx, vy, vx + n0x, vy + n0y, vx + n1x, vy + n1y};
    add_convex(out, q, 3);
}

/* Strokes one polyline (consecutive points distinct). */
static void stroke_poly(const StrokeCtx *s, const float *p, size_t n, bool closed, AuPath *out)
{
    if (n == 0)
        return;
    if (n == 1) {
        /* A zero-length subpath: round and square caps draw a dot. */
        if (s->cap == AU_CAP_ROUND)
            add_circle(out, p[0], p[1], s->hw);
        else if (s->cap == AU_CAP_SQUARE)
            add_cap(s, out, p[0], p[1], 1, 0), add_cap(s, out, p[0], p[1], -1, 0);
        return;
    }
    if (closed && n < 3)
        closed = false;
    size_t nseg = closed ? n : n - 1;
    for (size_t i = 0; i < nseg; i++) {
        size_t j = (i + 1) % n;
        float x0 = p[2 * i], y0 = p[2 * i + 1], x1 = p[2 * j], y1 = p[2 * j + 1];
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (!(len > 0))
            continue;
        float nx = -dy / len * s->hw, ny = dx / len * s->hw;
        float q[8] = {x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, x0 + nx, y0 + ny};
        add_convex(out, q, 4);
    }
    size_t first = closed ? 0 : 1, last = closed ? n : n - 1;
    for (size_t i = first; i < last; i++) {
        size_t a = (i + n - 1) % n, b = (i + 1) % n;
        float d0x = p[2 * i] - p[2 * a], d0y = p[2 * i + 1] - p[2 * a + 1];
        float d1x = p[2 * b] - p[2 * i], d1y = p[2 * b + 1] - p[2 * i + 1];
        float l0 = sqrtf(d0x * d0x + d0y * d0y), l1 = sqrtf(d1x * d1x + d1y * d1y);
        if (!(l0 > 0) || !(l1 > 0))
            continue;
        add_join(s, out, p[2 * i], p[2 * i + 1], d0x / l0, d0y / l0, d1x / l1, d1y / l1);
    }
    if (!closed) {
        float dx = p[0] - p[2], dy = p[1] - p[3];
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 0)
            add_cap(s, out, p[0], p[1], dx / l, dy / l);
        size_t e = n - 1;
        dx = p[2 * e] - p[2 * e - 2];
        dy = p[2 * e + 1] - p[2 * e - 1];
        l = sqrtf(dx * dx + dy * dy);
        if (l > 0)
            add_cap(s, out, p[2 * e], p[2 * e + 1], dx / l, dy / l);
    }
}

/* Cuts a polyline into dashes and strokes each. *phase_i and *phase_left
 * carry the position in the pattern from one subpath to the next (SVG
 * restarts the pattern per subpath, so the caller resets them). */
#define MAX_DASH_PIECES 20000

static void stroke_dashed(const StrokeCtx *s, const float *p, size_t n, bool closed, const float *dash, size_t nd,
                          float offset, AuPath *out, Poly *tmp, size_t *pieces)
{
    float total = 0;
    for (size_t i = 0; i < nd; i++)
        total += dash[i];
    /* The starting position in the pattern. */
    float ph = fmodf(offset, total);
    if (ph < 0)
        ph += total;
    size_t di = 0;
    for (size_t guard = 0; (ph > dash[di] || (ph == dash[di] && dash[di] > 0)) && guard < 4 * nd; guard++) {
        ph -= dash[di];
        di = (di + 1) % nd;
    }
    if (ph > dash[di])
        ph = dash[di];
    float left = dash[di] - ph; /* length remaining in dash di */
    bool on = (di & 1) == 0;
    tmp->n = 0;
    if (on)
        poly_add(tmp, p[0], p[1]);
    size_t nseg = closed ? n : n - 1;
    for (size_t i = 0; i < nseg; i++) {
        size_t j = (i + 1) % n;
        float x0 = p[2 * i], y0 = p[2 * i + 1], x1 = p[2 * j], y1 = p[2 * j + 1];
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        float pos = 0;
        while (len - pos >= left && (left > 0 || len > 0)) {
            pos += left;
            float t = pos / len;
            float x = x0 + dx * t, y = y0 + dy * t;
            if (on) {
                poly_add(tmp, x, y);
                stroke_poly(s, tmp->p, tmp->n, false, out);
                tmp->n = 0;
                if (++*pieces > MAX_DASH_PIECES)
                    return;
            } else {
                poly_add(tmp, x, y);
            }
            on = !on;
            di = (di + 1) % nd;
            left = dash[di];
            if (!on)
                tmp->n = 0;
        }
        left -= len - pos;
        if (on)
            poly_add(tmp, x1, y1);
    }
    if (on && tmp->n)
        stroke_poly(s, tmp->p, tmp->n, false, out);
}

void au_path_stroke_style(const AuPath *in, const AuStrokeStyle *st, AuPath *out)
{
    if (!(st->width > 0) || !isfinite(st->width))
        return;
    StrokeCtx s;
    s.hw = st->width / 2;
    s.tol = st->tolerance > 0 ? st->tolerance : 0.1f;
    s.miter = st->miter_limit < 1 ? 1 : st->miter_limit;
    s.cap = st->cap;
    s.join = st->join;
    /* Dashes: an odd list repeats; negative or all-zero lists, and
     * patterns too fine to draw, stroke solid. */
    float dash[2 * AU_STROKE_MAX_DASHES];
    size_t nd = 0;
    if (st->dashes && st->ndashes && st->ndashes <= AU_STROKE_MAX_DASHES) {
        float sum = 0;
        bool bad = false;
        for (size_t r = 0; r < (st->ndashes & 1 ? 2u : 1u); r++)
            for (size_t i = 0; i < st->ndashes; i++) {
                float d = st->dashes[i];
                if (!(d >= 0) || !isfinite(d))
                    bad = true;
                dash[nd++] = d;
                sum += d;
            }
        if (bad || !(sum > s.tol * 0.01f) || !isfinite(sum))
            nd = 0;
    }
    float tol = s.tol;
    Poly pl = {0}, tmp = {0};
    size_t pieces = 0;
    const float *pt = in->pts;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    int drawn = 0; /* drawing ops in the current subpath */
    for (size_t i = 0; i <= in->nops; i++) {
        uint8_t op = i < in->nops ? in->ops[i] : AU_PATH_MOVE;
        if (op == AU_PATH_MOVE || op == AU_PATH_CLOSE) {
            bool closed = op == AU_PATH_CLOSE;
            if ((drawn || closed) && pl.n) {
                /* A closed polyline does not repeat its first point. */
                if (closed && pl.n > 1 && pl.p[2 * pl.n - 2] == pl.p[0] && pl.p[2 * pl.n - 1] == pl.p[1])
                    pl.n--;
                if (nd)
                    stroke_dashed(&s, pl.p, pl.n, closed, dash, nd, st->dash_offset, out, &tmp, &pieces);
                else
                    stroke_poly(&s, pl.p, pl.n, closed, out);
            }
            pl.n = 0;
            drawn = 0;
            if (i == in->nops || pieces > MAX_DASH_PIECES)
                break;
            if (op == AU_PATH_MOVE) {
                cx = sx = pt[0];
                cy = sy = pt[1];
                pt += 2;
            } else {
                /* Drawing after a close starts a new subpath at the
                 * closed one's start. */
                cx = sx;
                cy = sy;
            }
            poly_add(&pl, cx, cy);
            continue;
        }
        drawn++;
        if (pl.n == 0)
            poly_add(&pl, cx, cy);
        if (op == AU_PATH_LINE) {
            cx = pt[0];
            cy = pt[1];
            pt += 2;
            poly_add(&pl, cx, cy);
            continue;
        }
        float q[8] = {cx, cy, pt[0], pt[1], pt[2], pt[3], 0, 0};
        int steps;
        if (op == AU_PATH_QUAD) {
            float ddx = q[0] - 2 * q[2] + q[4], ddy = q[1] - 2 * q[3] + q[5];
            steps = curve_steps(sqrtf(ddx * ddx + ddy * ddy) / 8.0f, tol);
            cx = pt[2];
            cy = pt[3];
            pt += 4;
        } else {
            q[6] = pt[4];
            q[7] = pt[5];
            float ax = q[0] - 2 * q[2] + q[4], ay = q[1] - 2 * q[3] + q[5];
            float bx = q[2] - 2 * q[4] + q[6], by = q[3] - 2 * q[5] + q[7];
            float d1 = ax * ax + ay * ay, d2 = bx * bx + by * by;
            steps = curve_steps(sqrtf(d1 > d2 ? d1 : d2) * 0.75f, tol);
            cx = pt[4];
            cy = pt[5];
            pt += 6;
        }
        for (int k = 1; k <= steps; k++) {
            float t = (float)k / steps, mt = 1 - t, x, y;
            if (op == AU_PATH_QUAD) {
                x = mt * mt * q[0] + 2 * mt * t * q[2] + t * t * q[4];
                y = mt * mt * q[1] + 2 * mt * t * q[3] + t * t * q[5];
            } else {
                float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
                x = a * q[0] + b * q[2] + c * q[4] + d * q[6];
                y = a * q[1] + b * q[3] + c * q[5] + d * q[7];
            }
            poly_add(&pl, x, y);
        }
    }
    au_free(pl.p);
    au_free(tmp.p);
}

/* An upper bound on the polyline points au_path_stroke_style walks for p
 * at flattening tolerance tol (0: 0.1), without stroking: the canvas
 * context refuses strokes that would make too large an outline
 * (canvas2d_int.h declares this). */
size_t au_path_stroke_points(const AuPath *p, float tol);
size_t au_path_stroke_points(const AuPath *p, float tol)
{
    if (!(tol > 0))
        tol = 0.1f;
    const float *pt = p->pts;
    float cx = 0, cy = 0;
    size_t n = 0;
    for (size_t i = 0; i < p->nops; i++) {
        switch (p->ops[i]) {
        case AU_PATH_MOVE:
        case AU_PATH_LINE:
            n++;
            cx = pt[0];
            cy = pt[1];
            pt += 2;
            break;
        case AU_PATH_QUAD: {
            float ddx = cx - 2 * pt[0] + pt[2], ddy = cy - 2 * pt[1] + pt[3];
            n += (size_t)curve_steps(sqrtf(ddx * ddx + ddy * ddy) / 8.0f, tol);
            cx = pt[2];
            cy = pt[3];
            pt += 4;
            break;
        }
        case AU_PATH_CUBIC: {
            float ax = cx - 2 * pt[0] + pt[2], ay = cy - 2 * pt[1] + pt[3];
            float bx = pt[0] - 2 * pt[2] + pt[4], by = pt[1] - 2 * pt[3] + pt[5];
            float d1 = ax * ax + ay * ay, d2 = bx * bx + by * by;
            n += (size_t)curve_steps(sqrtf(d1 > d2 ? d1 : d2) * 0.75f, tol);
            cx = pt[4];
            cy = pt[5];
            pt += 6;
            break;
        }
        default:
            n++;
            break;
        }
    }
    return n;
}
