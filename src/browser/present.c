/* Damage and scroll bookkeeping for shells (see present.h).
 *
 * Damage is a short list of rectangles rather than one union, so that a
 * strip exposed by a scroll at the top of the page and a status bar change
 * at the bottom of the window do not merge into the whole window. When
 * the list is full, the two rectangles whose union adds the least area are
 * merged. */
#include "present.h"

#include "chrome.h"

#include <stdlib.h>
#include <string.h>

void au_present_init(AuPresent *p)
{
    memset(p, 0, sizeof *p);
}

static long area_of(AuIRect r)
{
    return au_irect_empty(r) ? 0 : (long)(r.x1 - r.x0) * (long)(r.y1 - r.y0);
}

static bool inside(AuIRect a, AuIRect b) /* a within b */
{
    return a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1;
}

static void merge_cheapest(AuPresent *p)
{
    int bi = 0, bj = 1;
    long best = -1;
    for (int i = 0; i < p->n; i++)
        for (int j = i + 1; j < p->n; j++) {
            long grow = area_of(au_irect_union(p->rects[i], p->rects[j])) - area_of(p->rects[i]) -
                        area_of(p->rects[j]);
            if (best < 0 || grow < best) {
                best = grow;
                bi = i;
                bj = j;
            }
        }
    p->rects[bi] = au_irect_union(p->rects[bi], p->rects[bj]);
    p->rects[bj] = p->rects[--p->n];
}

void au_present_invalidate(AuPresent *p, AuIRect r)
{
    if (au_irect_empty(r))
        return;
    for (int i = 0; i < p->n; i++)
        if (inside(r, p->rects[i]))
            return;
    for (int i = 0; i < p->n;) {
        if (inside(p->rects[i], r))
            p->rects[i] = p->rects[--p->n];
        else
            i++;
    }
    if (p->n == AU_PRESENT_RECTS)
        merge_cheapest(p);
    p->rects[p->n++] = r;
}

static AuIRect moved(AuIRect r, int dx, int dy)
{
    return (AuIRect){r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy};
}

void au_present_scroll(AuPresent *p, AuIRect area, int dx, int dy)
{
    if (au_irect_empty(area) || (dx == 0 && dy == 0))
        return;
    if (p->shifting && memcmp(&p->shift_area, &area, sizeof area) != 0) {
        /* Two areas in one round: paint both instead. */
        au_present_invalidate(p, p->shift_area);
        au_present_invalidate(p, area);
        p->shifting = false;
        return;
    }
    /* Damage inside the area moves with its pixels. A rectangle partly
     * outside keeps its place too (the part outside does not move). */
    AuIRect old[AU_PRESENT_RECTS];
    int n = p->n;
    memcpy(old, p->rects, sizeof old);
    p->n = 0;
    for (int i = 0; i < n; i++) {
        AuIRect in = au_irect_intersect(old[i], area);
        if (au_irect_empty(in)) {
            au_present_invalidate(p, old[i]);
            continue;
        }
        if (!inside(old[i], area))
            au_present_invalidate(p, old[i]);
        au_present_invalidate(p, au_irect_intersect(moved(in, dx, dy), area));
    }
    int tdx = (p->shifting ? p->shift_dx : 0) + dx, tdy = (p->shifting ? p->shift_dy : 0) + dy;
    if (abs(tdx) >= area.x1 - area.x0 || abs(tdy) >= area.y1 - area.y0) {
        p->shifting = false;
        au_present_invalidate(p, area);
        return;
    }
    p->shifting = tdx != 0 || tdy != 0;
    p->shift_area = area;
    p->shift_dx = tdx;
    p->shift_dy = tdy;
}

bool au_present_pending(const AuPresent *p)
{
    return p->n > 0 || p->shifting;
}

/* Copies n pixels, word by word. memmove is not used: newlib's on Mac OS
 * 9 (built for size) copies a byte at a time, and a page-sized shift
 * took about 120 ms in the guest at 1400 x 900. backward copies from the
 * end, for a destination after an overlapping source. */
static void copy_pixels(AuPixel *d, const AuPixel *s, size_t n, bool backward)
{
    if (!backward) {
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            AuPixel a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];
            AuPixel f = s[i + 4], g = s[i + 5], h = s[i + 6], k = s[i + 7];
            d[i] = a;
            d[i + 1] = b;
            d[i + 2] = c;
            d[i + 3] = e;
            d[i + 4] = f;
            d[i + 5] = g;
            d[i + 6] = h;
            d[i + 7] = k;
        }
        for (; i < n; i++)
            d[i] = s[i];
    } else {
        size_t i = n;
        for (; i >= 8; i -= 8) {
            AuPixel a = s[i - 1], b = s[i - 2], c = s[i - 3], e = s[i - 4];
            AuPixel f = s[i - 5], g = s[i - 6], h = s[i - 7], k = s[i - 8];
            d[i - 1] = a;
            d[i - 2] = b;
            d[i - 3] = c;
            d[i - 4] = e;
            d[i - 5] = f;
            d[i - 6] = g;
            d[i - 7] = h;
            d[i - 8] = k;
        }
        for (; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
}

void au_present_shift_pixels(AuSurface *s, AuIRect area, int dx, int dy)
{
    area = au_irect_intersect(area, (AuIRect){0, 0, s->width, s->height});
    int w = area.x1 - area.x0, h = area.y1 - area.y0;
    if (w <= 0 || h <= 0 || abs(dx) >= w || abs(dy) >= h || (dx == 0 && dy == 0))
        return;
    /* Source columns and rows that land inside the area. */
    int sx0 = dx >= 0 ? area.x0 : area.x0 - dx;
    int cols = w - abs(dx);
    int rows = h - abs(dy);
    size_t n = (size_t)cols;
    /* Rows move to other rows, so only a move within a row (dy == 0)
     * overlaps; moving right then copies from the end. */
    bool backward = dy == 0 && dx > 0;
    if (dy > 0) {
        /* Moving down: copy from the bottom up. */
        for (int i = rows - 1; i >= 0; i--) {
            int sy = area.y0 + i;
            copy_pixels(s->pixels + (size_t)(sy + dy) * (size_t)s->stride + (size_t)(sx0 + dx),
                        s->pixels + (size_t)sy * (size_t)s->stride + (size_t)sx0, n, backward);
        }
    } else {
        int sy0 = area.y0 - dy;
        for (int i = 0; i < rows; i++) {
            int sy = sy0 + i;
            copy_pixels(s->pixels + (size_t)(sy + dy) * (size_t)s->stride + (size_t)(sx0 + dx),
                        s->pixels + (size_t)sy * (size_t)s->stride + (size_t)sx0, n, backward);
        }
    }
}

void au_present_begin(AuPresent *p, AuSurface *s, AuPresentRound *r)
{
    memset(r, 0, sizeof *r);
    if (p->shifting) {
        r->shifted = true;
        r->area = p->shift_area;
        r->dx = p->shift_dx;
        r->dy = p->shift_dy;
        if (s)
            au_present_shift_pixels(s, r->area, r->dx, r->dy);
        p->shifting = false;
        p->shift_dx = p->shift_dy = 0;
    }
    memcpy(r->rects, p->rects, sizeof r->rects);
    r->n = p->n;
    p->n = 0;
}
