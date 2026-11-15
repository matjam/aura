/* Paths: building, shapes, stroking and flattening to edges. */
#include "paint_internal.h"

#include <math.h>
#include <string.h>

/* Control-point distance for a quarter ellipse drawn as one cubic. */
#define KAPPA 0.5522847498f

/* Flattening tolerance in pixels: the furthest a line segment may stray
 * from its curve. A tenth of a pixel keeps the area within a fraction of a
 * percent (a chord always cuts inside a convex curve). */
#define FLAT_TOL 0.1f

void au_path_init(AuPath *p)
{
    memset(p, 0, sizeof *p);
}

void au_path_free(AuPath *p)
{
    au_free(p->ops);
    au_free(p->pts);
    memset(p, 0, sizeof *p);
}

void au_path_clear(AuPath *p)
{
    p->nops = p->npts = 0;
    p->sx = p->sy = p->cx = p->cy = 0;
}

static void push(AuPath *p, uint8_t op, const float *xy, size_t npt)
{
    if (p->nops == p->ops_cap) {
        p->ops_cap = p->ops_cap ? p->ops_cap * 2 : 16;
        p->ops = au_realloc(p->ops, p->ops_cap);
    }
    if (p->npts + npt * 2 > p->pts_cap) {
        size_t cap = p->pts_cap ? p->pts_cap * 2 : 64;
        while (cap < p->npts + npt * 2)
            cap *= 2;
        p->pts = au_realloc(p->pts, cap * sizeof(float));
        p->pts_cap = cap;
    }
    p->ops[p->nops++] = op;
    if (npt) /* xy is NULL for a close */
        memcpy(p->pts + p->npts, xy, npt * 2 * sizeof(float));
    p->npts += npt * 2;
}

void au_path_move_to(AuPath *p, float x, float y)
{
    float xy[2] = {x, y};
    push(p, AU_PATH_MOVE, xy, 1);
    p->sx = p->cx = x;
    p->sy = p->cy = y;
}

void au_path_line_to(AuPath *p, float x, float y)
{
    float xy[2] = {x, y};
    push(p, AU_PATH_LINE, xy, 1);
    p->cx = x;
    p->cy = y;
}

void au_path_quad_to(AuPath *p, float cx, float cy, float x, float y)
{
    float xy[4] = {cx, cy, x, y};
    push(p, AU_PATH_QUAD, xy, 2);
    p->cx = x;
    p->cy = y;
}

void au_path_cubic_to(AuPath *p, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    float xy[6] = {c1x, c1y, c2x, c2y, x, y};
    push(p, AU_PATH_CUBIC, xy, 3);
    p->cx = x;
    p->cy = y;
}

void au_path_close(AuPath *p)
{
    push(p, AU_PATH_CLOSE, NULL, 0);
    p->cx = p->sx;
    p->cy = p->sy;
}

/* ---- Shapes ---- */

bool au_radii_zero(const AuRadii *r)
{
    if (!r)
        return true;
    for (int i = 0; i < 4; i++)
        if (r->rx[i] > 0 && r->ry[i] > 0)
            return false;
    return true;
}

void au_radii_fit(AuRadii *r, AuRect box)
{
    if (!r)
        return;
    for (int i = 0; i < 4; i++) {
        if (r->rx[i] < 0 || r->ry[i] < 0 || !(r->rx[i] > 0 && r->ry[i] > 0))
            r->rx[i] = r->ry[i] = 0;
    }
    float f = 1.0f;
    float sums[4] = {r->rx[AU_CORNER_TL] + r->rx[AU_CORNER_TR], r->ry[AU_CORNER_TR] + r->ry[AU_CORNER_BR],
                     r->rx[AU_CORNER_BR] + r->rx[AU_CORNER_BL], r->ry[AU_CORNER_BL] + r->ry[AU_CORNER_TL]};
    float lens[4] = {box.w, box.h, box.w, box.h};
    for (int i = 0; i < 4; i++)
        if (sums[i] > 0 && lens[i] / sums[i] < f)
            f = lens[i] / sums[i];
    if (f < 0)
        f = 0;
    if (f < 1.0f) {
        for (int i = 0; i < 4; i++) {
            r->rx[i] *= f;
            r->ry[i] *= f;
        }
    }
}

void au_path_rect(AuPath *p, AuRect r)
{
    au_path_move_to(p, r.x, r.y);
    au_path_line_to(p, r.x + r.w, r.y);
    au_path_line_to(p, r.x + r.w, r.y + r.h);
    au_path_line_to(p, r.x, r.y + r.h);
    au_path_close(p);
}

void au_path_rounded_rect(AuPath *p, AuRect r, const AuRadii *radii, bool reverse)
{
    if (r.w <= 0 || r.h <= 0)
        return;
    if (au_radii_zero(radii)) {
        if (!reverse) {
            au_path_rect(p, r);
        } else {
            au_path_move_to(p, r.x, r.y);
            au_path_line_to(p, r.x, r.y + r.h);
            au_path_line_to(p, r.x + r.w, r.y + r.h);
            au_path_line_to(p, r.x + r.w, r.y);
            au_path_close(p);
        }
        return;
    }
    AuRadii rr = *radii;
    au_radii_fit(&rr, r);
    float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    /* The outline clockwise as 4 corners; each corner is the point where
     * its arc starts, two control points, and where it ends. Corners with a
     * zero radius collapse to a point. */
    float c[4][8];
    const float *rx = rr.rx, *ry = rr.ry;
    /* top-left: from (x0, y0+ry) to (x0+rx, y0) */
    float a = rx[0], b = ry[0];
    float tl[8] = {x0, y0 + b, x0, y0 + b - b * KAPPA, x0 + a - a * KAPPA, y0, x0 + a, y0};
    a = rx[1], b = ry[1];
    float tr[8] = {x1 - a, y0, x1 - a + a * KAPPA, y0, x1, y0 + b - b * KAPPA, x1, y0 + b};
    a = rx[2], b = ry[2];
    float br[8] = {x1, y1 - b, x1, y1 - b + b * KAPPA, x1 - a + a * KAPPA, y1, x1 - a, y1};
    a = rx[3], b = ry[3];
    float bl[8] = {x0 + a, y1, x0 + a - a * KAPPA, y1, x0, y1 - b + b * KAPPA, x0, y1 - b};
    memcpy(c[0], tl, sizeof tl);
    memcpy(c[1], tr, sizeof tr);
    memcpy(c[2], br, sizeof br);
    memcpy(c[3], bl, sizeof bl);
    if (!reverse) {
        au_path_move_to(p, c[0][0], c[0][1]);
        for (int i = 0; i < 4; i++) {
            au_path_line_to(p, c[i][0], c[i][1]);
            if (rx[i] > 0)
                au_path_cubic_to(p, c[i][2], c[i][3], c[i][4], c[i][5], c[i][6], c[i][7]);
        }
        au_path_close(p);
    } else {
        au_path_move_to(p, c[3][6], c[3][7]);
        for (int i = 3; i >= 0; i--) {
            au_path_line_to(p, c[i][6], c[i][7]);
            if (rx[i] > 0)
                au_path_cubic_to(p, c[i][4], c[i][5], c[i][2], c[i][3], c[i][0], c[i][1]);
        }
        au_path_close(p);
    }
}

void au_path_ellipse(AuPath *p, float cx, float cy, float rx, float ry)
{
    float kx = rx * KAPPA, ky = ry * KAPPA;
    au_path_move_to(p, cx + rx, cy);
    au_path_cubic_to(p, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    au_path_cubic_to(p, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    au_path_cubic_to(p, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    au_path_cubic_to(p, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    au_path_close(p);
}

/* ---- Flattening ---- */

typedef struct {
    AuEdge *e;
    size_t n, cap;
    float b[4];
    bool any;
} EdgeList;

static void add_point_bounds(EdgeList *l, float x, float y)
{
    if (!l->any) {
        l->b[0] = l->b[2] = x;
        l->b[1] = l->b[3] = y;
        l->any = true;
        return;
    }
    if (x < l->b[0])
        l->b[0] = x;
    if (x > l->b[2])
        l->b[2] = x;
    if (y < l->b[1])
        l->b[1] = y;
    if (y > l->b[3])
        l->b[3] = y;
}

static void add_edge(EdgeList *l, float x0, float y0, float x1, float y1)
{
    add_point_bounds(l, x1, y1);
    if (y0 == y1)
        return;
    /* NaN and huge values would make the rasterizer loop forever or
     * overflow; drop them. */
    if (!(fabsf(x0) < 1e6f && fabsf(y0) < 1e6f && fabsf(x1) < 1e6f && fabsf(y1) < 1e6f))
        return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->e = au_realloc(l->e, l->cap * sizeof(AuEdge));
    }
    AuEdge *e = &l->e[l->n++];
    e->x0 = x0;
    e->y0 = y0;
    e->x1 = x1;
    e->y1 = y1;
}

static int subdivisions(float dd)
{
    if (!(dd > 0))
        return 1;
    float n = ceilf(sqrtf(dd / FLAT_TOL));
    if (!(n >= 1))
        return 1;
    if (n > 128)
        return 128;
    return (int)n;
}

size_t au_path_flatten(const AuPath *p, float dx, float dy, AuEdge **edges, float bounds[4])
{
    EdgeList l = {0};
    const float *pt = p->pts;
    float sx = 0, sy = 0, cx = 0, cy = 0;
    bool open = false;
    for (size_t i = 0; i < p->nops; i++) {
        switch (p->ops[i]) {
        case AU_PATH_MOVE:
            if (open && (cx != sx || cy != sy))
                add_edge(&l, cx, cy, sx, sy);
            sx = cx = pt[0] + dx;
            sy = cy = pt[1] + dy;
            add_point_bounds(&l, cx, cy);
            open = true;
            pt += 2;
            break;
        case AU_PATH_LINE: {
            float x = pt[0] + dx, y = pt[1] + dy;
            add_edge(&l, cx, cy, x, y);
            cx = x;
            cy = y;
            pt += 2;
            break;
        }
        case AU_PATH_QUAD: {
            float x1 = pt[0] + dx, y1 = pt[1] + dy, x2 = pt[2] + dx, y2 = pt[3] + dy;
            /* Uniform steps: the error is |p0 - 2p1 + p2| / (8 n^2). */
            float ddx = cx - 2 * x1 + x2, ddy = cy - 2 * y1 + y2;
            int n = subdivisions(sqrtf(ddx * ddx + ddy * ddy) / 8.0f);
            float px = cx, py = cy;
            for (int k = 1; k <= n; k++) {
                float t = (float)k / n, mt = 1 - t;
                float x = mt * mt * cx + 2 * mt * t * x1 + t * t * x2;
                float y = mt * mt * cy + 2 * mt * t * y1 + t * t * y2;
                add_edge(&l, px, py, x, y);
                px = x;
                py = y;
            }
            cx = x2;
            cy = y2;
            pt += 4;
            break;
        }
        case AU_PATH_CUBIC: {
            float x1 = pt[0] + dx, y1 = pt[1] + dy, x2 = pt[2] + dx, y2 = pt[3] + dy;
            float x3 = pt[4] + dx, y3 = pt[5] + dy;
            /* The error of n uniform steps is at most 3 * dd / (4 n^2),
             * dd the largest second difference of the control points. */
            float ax = cx - 2 * x1 + x2, ay = cy - 2 * y1 + y2;
            float bx = x1 - 2 * x2 + x3, by = y1 - 2 * y2 + y3;
            float d1 = ax * ax + ay * ay, d2 = bx * bx + by * by;
            float dd = sqrtf(d1 > d2 ? d1 : d2);
            int n = subdivisions(dd * 0.75f);
            float px = cx, py = cy;
            for (int k = 1; k <= n; k++) {
                float t = (float)k / n, mt = 1 - t;
                float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
                float x = a * cx + b * x1 + c * x2 + d * x3;
                float y = a * cy + b * y1 + c * y2 + d * y3;
                add_edge(&l, px, py, x, y);
                px = x;
                py = y;
            }
            cx = x3;
            cy = y3;
            pt += 6;
            break;
        }
        case AU_PATH_CLOSE:
            if (open && (cx != sx || cy != sy))
                add_edge(&l, cx, cy, sx, sy);
            cx = sx;
            cy = sy;
            break;
        }
    }
    if (open && (cx != sx || cy != sy))
        add_edge(&l, cx, cy, sx, sy);
    *edges = l.e;
    memcpy(bounds, l.b, sizeof l.b);
    return l.n;
}

/* ---- Stroking ----
 * Each flattened segment becomes a rectangle, and each joint a round cap,
 * all wound the same way so a nonzero fill unions them. */

static void stroke_polyline(const float *pts, size_t n, bool closed, float hw, AuPath *out)
{
    for (size_t i = 0; i + 1 < n || (closed && i < n && n > 2); i++) {
        size_t j = (i + 1) % n;
        float x0 = pts[2 * i], y0 = pts[2 * i + 1], x1 = pts[2 * j], y1 = pts[2 * j + 1];
        float vx = x1 - x0, vy = y1 - y0;
        float len = sqrtf(vx * vx + vy * vy);
        if (len < 1e-6f)
            continue;
        float nx = -vy / len * hw, ny = vx / len * hw;
        /* Wind each quad clockwise in y-down coordinates. */
        au_path_move_to(out, x0 - nx, y0 - ny);
        au_path_line_to(out, x1 - nx, y1 - ny);
        au_path_line_to(out, x1 + nx, y1 + ny);
        au_path_line_to(out, x0 + nx, y0 + ny);
        au_path_close(out);
        if (!closed && j == n - 1)
            break;
    }
    /* Round joins at interior vertices (all vertices when closed). */
    size_t first = closed ? 0 : 1, last = closed ? n : (n > 0 ? n - 1 : 0);
    for (size_t i = first; i < last; i++)
        au_path_ellipse(out, pts[2 * i], pts[2 * i + 1], hw, hw);
}

void au_path_stroke(const AuPath *in, float width, AuPath *out)
{
    if (!(width > 0))
        return;
    /* Flatten each subpath into a polyline first. */
    float *poly = NULL;
    size_t n = 0, cap = 0;
    const float *pt = in->pts;
    float cx = 0, cy = 0;
    bool closed = false;
#define POLY_ADD(X, Y) \
    do { \
        if (n == cap) { \
            cap = cap ? cap * 2 : 64; \
            poly = au_realloc(poly, cap * 2 * sizeof(float)); \
        } \
        poly[2 * n] = (X); \
        poly[2 * n + 1] = (Y); \
        n++; \
    } while (0)
    for (size_t i = 0; i <= in->nops; i++) {
        uint8_t op = i < in->nops ? in->ops[i] : AU_PATH_MOVE;
        if (op == AU_PATH_MOVE || op == AU_PATH_CLOSE) {
            if (op == AU_PATH_CLOSE)
                closed = true;
            if (n > 1)
                stroke_polyline(poly, n, closed, width / 2, out);
            if (op == AU_PATH_CLOSE && n > 0) {
                cx = poly[0];
                cy = poly[1];
            }
            n = 0;
            closed = false;
            if (i == in->nops)
                break;
            if (op == AU_PATH_MOVE) {
                cx = pt[0];
                cy = pt[1];
                pt += 2;
            }
            POLY_ADD(cx, cy);
            continue;
        }
        if (n == 0)
            POLY_ADD(cx, cy);
        if (op == AU_PATH_LINE) {
            cx = pt[0];
            cy = pt[1];
            pt += 2;
            POLY_ADD(cx, cy);
        } else if (op == AU_PATH_QUAD || op == AU_PATH_CUBIC) {
            /* q: the start point and the control points. */
            float q[8] = {cx, cy, pt[0], pt[1], pt[2], pt[3], 0, 0};
            if (op == AU_PATH_QUAD) {
                cx = pt[2];
                cy = pt[3];
                pt += 4;
            } else {
                q[6] = pt[4];
                q[7] = pt[5];
                cx = pt[4];
                cy = pt[5];
                pt += 6;
            }
            int steps = 16;
            float x0 = q[0], y0 = q[1];
            for (int k = 1; k <= steps; k++) {
                float t = (float)k / steps, mt = 1 - t, x, y;
                if (op == AU_PATH_QUAD) {
                    x = mt * mt * x0 + 2 * mt * t * q[2] + t * t * q[4];
                    y = mt * mt * y0 + 2 * mt * t * q[3] + t * t * q[5];
                } else {
                    float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
                    x = a * x0 + b * q[2] + c * q[4] + d * q[6];
                    y = a * y0 + b * q[3] + c * q[5] + d * q[7];
                }
                POLY_ADD(x, y);
            }
        }
    }
#undef POLY_ADD
    au_free(poly);
}

/* ---- Gradients ---- */

void au_gradient_css_line(AuGradient *g, AuRect box, float angle_deg)
{
    float a = angle_deg * 3.14159265358979f / 180.0f;
    float s = sinf(a), c = cosf(a);
    /* CSS Images 3: the gradient line passes through the centre; its
     * length makes the perpendiculars through opposite corners its ends. */
    float len = fabsf(box.w * s) + fabsf(box.h * c);
    float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
    g->x0 = cx - s * len / 2;
    g->y0 = cy + c * len / 2;
    g->x1 = cx + s * len / 2;
    g->y1 = cy - c * len / 2;
}

void au_gradient_add_stop(AuGradient *g, float pos, AuColor c)
{
    if (g->nstops >= AU_GRADIENT_MAX_STOPS)
        return;
    g->stops[g->nstops].pos = pos;
    g->stops[g->nstops].color = c;
    g->nstops++;
}
