/* Path building for the 2D canvas context: the path methods of the HTML
 * standard ("Building paths") on an AuPath. Coordinates arrive as doubles
 * in the caller's space and are stored mapped through a matrix; a call
 * with a value that is not finite adds nothing. */
#include "canvas2d_int.h"

#include <math.h>
#include <string.h>

#define C2D_PI 3.14159265358979323846

static bool fin(double v)
{
    return isfinite(v);
}

bool c2d_map(const AuMatrix *m, double x, double y, float *ox, float *oy)
{
    double tx = x, ty = y;
    if (m) {
        tx = (double)m->a * x + (double)m->c * y + m->e;
        ty = (double)m->b * x + (double)m->d * y + m->f;
    }
    if (tx != tx || ty != ty)
        return false;
    if (tx > C2D_COORD_MAX)
        tx = C2D_COORD_MAX;
    if (tx < -C2D_COORD_MAX)
        tx = -C2D_COORD_MAX;
    if (ty > C2D_COORD_MAX)
        ty = C2D_COORD_MAX;
    if (ty < -C2D_COORD_MAX)
        ty = -C2D_COORD_MAX;
    *ox = (float)tx;
    *oy = (float)ty;
    return true;
}

void au_cpath_init(AuCanvasPath *p)
{
    au_path_init(&p->p);
}

void au_cpath_free(AuCanvasPath *p)
{
    au_path_free(&p->p);
}

void au_cpath_clear(AuCanvasPath *p)
{
    au_path_clear(&p->p);
}

static bool room(const AuCanvasPath *p)
{
    return p->p.nops < AU_C2D_MAX_PATH_OPS;
}

void au_cpath_move_to(AuCanvasPath *p, const AuMatrix *m, double x, double y)
{
    float tx, ty;
    if (!fin(x) || !fin(y) || !room(p) || !c2d_map(m, x, y, &tx, &ty))
        return;
    au_path_move_to(&p->p, tx, ty);
}

void au_cpath_line_to(AuCanvasPath *p, const AuMatrix *m, double x, double y)
{
    float tx, ty;
    if (!fin(x) || !fin(y) || !room(p) || !c2d_map(m, x, y, &tx, &ty))
        return;
    if (p->p.nops == 0)
        au_path_move_to(&p->p, tx, ty);
    else
        au_path_line_to(&p->p, tx, ty);
}

void au_cpath_quad_to(AuCanvasPath *p, const AuMatrix *m, double cx, double cy, double x, double y)
{
    float a, b, tx, ty;
    if (!fin(cx) || !fin(cy) || !fin(x) || !fin(y) || !room(p))
        return;
    if (!c2d_map(m, cx, cy, &a, &b) || !c2d_map(m, x, y, &tx, &ty))
        return;
    if (p->p.nops == 0)
        au_path_move_to(&p->p, a, b);
    au_path_quad_to(&p->p, a, b, tx, ty);
}

void au_cpath_cubic_to(AuCanvasPath *p, const AuMatrix *m, double c1x, double c1y, double c2x, double c2y,
                       double x, double y)
{
    float a, b, c, d, tx, ty;
    if (!fin(c1x) || !fin(c1y) || !fin(c2x) || !fin(c2y) || !fin(x) || !fin(y) || !room(p))
        return;
    if (!c2d_map(m, c1x, c1y, &a, &b) || !c2d_map(m, c2x, c2y, &c, &d) || !c2d_map(m, x, y, &tx, &ty))
        return;
    if (p->p.nops == 0)
        au_path_move_to(&p->p, a, b);
    au_path_cubic_to(&p->p, a, b, c, d, tx, ty);
}

void au_cpath_close(AuCanvasPath *p)
{
    if (p->p.nops && room(p))
        au_path_close(&p->p);
}

/* A point of the ellipse: angle t before the rotation. */
static void ell_point(double x, double y, double rx, double ry, double cr, double sr, double ct, double st,
                      double *ox, double *oy)
{
    double px = rx * ct, py = ry * st;
    *ox = x + px * cr - py * sr;
    *oy = y + px * sr + py * cr;
}

bool au_cpath_ellipse(AuCanvasPath *p, const AuMatrix *m, double x, double y, double rx, double ry, double rot,
                      double a0, double a1, bool ccw)
{
    if (!fin(x) || !fin(y) || !fin(rx) || !fin(ry) || !fin(rot) || !fin(a0) || !fin(a1))
        return true;
    if (rx < 0 || ry < 0)
        return false;
    const double tau = 2 * C2D_PI;
    /* The standard's rule for the angles: a difference of a whole turn or
     * more in the drawing direction is the whole ellipse; otherwise the
     * arc runs from a0 to a1 in that direction. */
    double sweep;
    if (!ccw) {
        if (a1 - a0 >= tau)
            sweep = tau;
        else {
            sweep = fmod(a1 - a0, tau);
            if (sweep < 0)
                sweep += tau;
        }
    } else {
        if (a0 - a1 >= tau)
            sweep = -tau;
        else {
            sweep = fmod(a1 - a0, tau);
            if (sweep > 0)
                sweep -= tau;
        }
    }
    double start = fmod(a0, tau);
    double cr = cos(rot), sr = sin(rot);
    double sx, sy;
    ell_point(x, y, rx, ry, cr, sr, cos(start), sin(start), &sx, &sy);
    au_cpath_line_to(p, m, sx, sy);
    if (sweep == 0)
        return true;
    int nseg = (int)ceil(fabs(sweep) / (C2D_PI / 2) - 1e-9);
    if (nseg < 1)
        nseg = 1;
    if (nseg > 4)
        nseg = 4;
    double d = sweep / nseg;
    double k = 4.0 / 3.0 * tan(d / 4);
    double t = start;
    for (int i = 0; i < nseg; i++) {
        double t1 = t + d;
        double c0 = cos(t), s0 = sin(t), c1 = cos(t1), s1 = sin(t1);
        /* Control points: along the tangents at each end. */
        double p1x = rx * (c0 - k * s0), p1y = ry * (s0 + k * c0);
        double p2x = rx * (c1 + k * s1), p2y = ry * (s1 - k * c1);
        double ex, ey;
        ell_point(x, y, rx, ry, cr, sr, c1, s1, &ex, &ey);
        au_cpath_cubic_to(p, m, x + p1x * cr - p1y * sr, y + p1x * sr + p1y * cr, x + p2x * cr - p2y * sr,
                          y + p2x * sr + p2y * cr, ex, ey);
        t = t1;
    }
    return true;
}

bool au_cpath_arc(AuCanvasPath *p, const AuMatrix *m, double x, double y, double r, double a0, double a1, bool ccw)
{
    return au_cpath_ellipse(p, m, x, y, r, r, 0, a0, a1, ccw);
}

bool au_cpath_arc_to(AuCanvasPath *p, const AuMatrix *m, double x1, double y1, double x2, double y2, double r)
{
    if (!fin(x1) || !fin(y1) || !fin(x2) || !fin(y2) || !fin(r))
        return true;
    if (r < 0)
        return false;
    if (p->p.nops == 0) {
        au_cpath_move_to(p, m, x1, y1);
        return true;
    }
    /* The current point, back in the caller's space. */
    double x0 = p->p.cx, y0 = p->p.cy;
    if (m) {
        AuMatrix inv;
        if (!au_matrix_invert(m, &inv)) {
            au_cpath_line_to(p, m, x1, y1);
            return true;
        }
        double tx = (double)inv.a * x0 + (double)inv.c * y0 + inv.e;
        double ty = (double)inv.b * x0 + (double)inv.d * y0 + inv.f;
        x0 = tx;
        y0 = ty;
    }
    double dir = (x2 - x1) * (y0 - y1) + (y2 - y1) * (x1 - x0);
    double a2 = (x0 - x1) * (x0 - x1) + (y0 - y1) * (y0 - y1);
    double b2 = (x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2);
    if (r == 0 || dir == 0 || !(a2 > 0) || !(b2 > 0) || !fin(dir) || !fin(a2) || !fin(b2)) {
        au_cpath_line_to(p, m, x1, y1);
        return true;
    }
    double c2 = (x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2);
    double cosx = (a2 + b2 - c2) / (2 * sqrt(a2 * b2));
    double sinx = sqrt(1 - cosx * cosx);
    if (!(sinx > 1e-12) || !fin(cosx)) {
        au_cpath_line_to(p, m, x1, y1);
        return true;
    }
    double d = r / ((1 - cosx) / sinx);
    double la = sqrt(a2), lb = sqrt(b2);
    double anx = (x1 - x0) / la, any = (y1 - y0) / la;
    double bnx = (x1 - x2) / lb, bny = (y1 - y2) / lb;
    double x3 = x1 - anx * d, y3 = y1 - any * d;
    double x4 = x1 - bnx * d, y4 = y1 - bny * d;
    bool acw = dir < 0;
    double cx = x3 + any * r * (acw ? 1 : -1);
    double cy = y3 - anx * r * (acw ? 1 : -1);
    double ang0 = atan2(y3 - cy, x3 - cx), ang1 = atan2(y4 - cy, x4 - cx);
    au_cpath_line_to(p, m, x3, y3);
    au_cpath_arc(p, m, cx, cy, r, ang0, ang1, acw);
    return true;
}

void au_cpath_rect(AuCanvasPath *p, const AuMatrix *m, double x, double y, double w, double h)
{
    if (!fin(x) || !fin(y) || !fin(w) || !fin(h))
        return;
    au_cpath_move_to(p, m, x, y);
    au_cpath_line_to(p, m, x + w, y);
    au_cpath_line_to(p, m, x + w, y + h);
    au_cpath_line_to(p, m, x, y + h);
    au_cpath_close(p);
}

bool au_cpath_round_rect(AuCanvasPath *p, const AuMatrix *m, double x, double y, double w, double h,
                         const double radii[8])
{
    if (!fin(x) || !fin(y) || !fin(w) || !fin(h))
        return true;
    double r[8];
    for (int i = 0; i < 8; i++) {
        if (!fin(radii[i]))
            return true;
        if (radii[i] < 0)
            return false;
        r[i] = radii[i];
    }
    /* A negative size mirrors the rectangle and its corners. */
    if (w < 0) {
        x += w;
        w = -w;
        double t;
        t = r[0], r[0] = r[2], r[2] = t;
        t = r[1], r[1] = r[3], r[3] = t;
        t = r[4], r[4] = r[6], r[6] = t;
        t = r[5], r[5] = r[7], r[7] = t;
    }
    if (h < 0) {
        y += h;
        h = -h;
        double t;
        t = r[0], r[0] = r[6], r[6] = t;
        t = r[1], r[1] = r[7], r[7] = t;
        t = r[2], r[2] = r[4], r[4] = t;
        t = r[3], r[3] = r[5], r[5] = t;
    }
    /* Corner curves must not overlap: scale all by the least ratio. */
    double top = r[0] + r[2], right = r[3] + r[5], bottom = r[4] + r[6], left = r[1] + r[7];
    double scale = 1;
    if (top > 0 && w / top < scale)
        scale = w / top;
    if (bottom > 0 && w / bottom < scale)
        scale = w / bottom;
    if (left > 0 && h / left < scale)
        scale = h / left;
    if (right > 0 && h / right < scale)
        scale = h / right;
    if (scale < 1)
        for (int i = 0; i < 8; i++)
            r[i] *= scale;
    const double k = 0.5522847498307936; /* cubic approximation of a quarter turn */
    double tlx = r[0], tly = r[1], trx = r[2], try_ = r[3], brx = r[4], bry = r[5], blx = r[6], bly = r[7];
    au_cpath_move_to(p, m, x + tlx, y);
    au_cpath_line_to(p, m, x + w - trx, y);
    au_cpath_cubic_to(p, m, x + w - trx * (1 - k), y, x + w, y + try_ * (1 - k), x + w, y + try_);
    au_cpath_line_to(p, m, x + w, y + h - bry);
    au_cpath_cubic_to(p, m, x + w, y + h - bry * (1 - k), x + w - brx * (1 - k), y + h, x + w - brx, y + h);
    au_cpath_line_to(p, m, x + blx, y + h);
    au_cpath_cubic_to(p, m, x + blx * (1 - k), y + h, x, y + h - bly * (1 - k), x, y + h - bly);
    au_cpath_line_to(p, m, x, y + tly);
    au_cpath_cubic_to(p, m, x, y + tly * (1 - k), x + tlx * (1 - k), y, x + tlx, y);
    au_cpath_close(p);
    au_cpath_move_to(p, m, x, y);
    return true;
}

void au_cpath_add(AuCanvasPath *p, const AuCanvasPath *src, const AuMatrix *m)
{
    const AuPath *s = &src->p;
    /* src may be p itself: read the counts first; the arrays may move. */
    size_t nops = s->nops;
    size_t pi = 0;
    for (size_t i = 0; i < nops && room(p); i++) {
        uint8_t op = s->ops[i];
        int np = op == AU_PATH_CLOSE ? 0 : op == AU_PATH_QUAD ? 2 : op == AU_PATH_CUBIC ? 3 : 1;
        float q[6] = {0};
        bool ok = true;
        for (int k = 0; k < np; k++)
            ok = c2d_map(m, s->pts[pi + 2 * k], s->pts[pi + 2 * k + 1], &q[2 * k], &q[2 * k + 1]) && ok;
        pi += (size_t)np * 2;
        if (!ok)
            continue;
        switch (op) {
        case AU_PATH_MOVE:
            au_path_move_to(&p->p, q[0], q[1]);
            break;
        case AU_PATH_LINE:
            if (p->p.nops == 0)
                au_path_move_to(&p->p, q[0], q[1]);
            else
                au_path_line_to(&p->p, q[0], q[1]);
            break;
        case AU_PATH_QUAD:
            if (p->p.nops == 0)
                au_path_move_to(&p->p, q[0], q[1]);
            au_path_quad_to(&p->p, q[0], q[1], q[2], q[3]);
            break;
        case AU_PATH_CUBIC:
            if (p->p.nops == 0)
                au_path_move_to(&p->p, q[0], q[1]);
            au_path_cubic_to(&p->p, q[0], q[1], q[2], q[3], q[4], q[5]);
            break;
        default:
            if (p->p.nops)
                au_path_close(&p->p);
            break;
        }
    }
}

void au_cpath_add_raw(AuCanvasPath *p, const AuPath *src)
{
    AuCanvasPath tmp;
    tmp.p = *src;
    au_cpath_add(p, &tmp, NULL);
}

bool c2d_path_irect(const AuPath *p, AuIRect *out)
{
    if (p->nops != 5 || p->ops[0] != AU_PATH_MOVE || p->ops[1] != AU_PATH_LINE || p->ops[2] != AU_PATH_LINE ||
        p->ops[3] != AU_PATH_LINE || p->ops[4] != AU_PATH_CLOSE)
        return false;
    const float *q = p->pts;
    for (int i = 0; i < 8; i++)
        if (q[i] != floorf(q[i]) || fabsf(q[i]) > 100000.0f)
            return false;
    /* Two opposite corners and the other two agreeing with them. */
    bool hfirst = q[1] == q[3] && q[2] == q[4] && q[5] == q[7] && q[6] == q[0];
    bool vfirst = q[0] == q[2] && q[3] == q[5] && q[4] == q[6] && q[7] == q[1];
    if (!hfirst && !vfirst)
        return false;
    float x0 = q[0] < q[4] ? q[0] : q[4], x1 = q[0] < q[4] ? q[4] : q[0];
    float y0 = q[1] < q[5] ? q[1] : q[5], y1 = q[1] < q[5] ? q[5] : q[1];
    out->x0 = (int)x0;
    out->y0 = (int)y0;
    out->x1 = (int)x1;
    out->y1 = (int)y1;
    return true;
}
