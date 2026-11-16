/* The browser chrome: layout, painting, hit testing. See chrome.h. */
#include "chrome.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- Rectangles ---- */

static AuIRect irect(int x0, int y0, int x1, int y1)
{
    AuIRect r = {x0, y0, x1, y1};
    return r;
}

AuIRect au_irect_union(AuIRect a, AuIRect b)
{
    if (au_irect_empty(a))
        return b;
    if (au_irect_empty(b))
        return a;
    return irect(a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
                 a.y1 > b.y1 ? a.y1 : b.y1);
}

AuIRect au_irect_intersect(AuIRect a, AuIRect b)
{
    AuIRect r = irect(a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
                      a.y1 < b.y1 ? a.y1 : b.y1);
    if (au_irect_empty(r))
        return irect(0, 0, 0, 0);
    return r;
}

static AuRect frect(AuIRect r)
{
    AuRect f = {(float)r.x0, (float)r.y0, (float)(r.x1 - r.x0), (float)(r.y1 - r.y0)};
    return f;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- Layout ---- */

void au_chrome_layout(AuChromeLayout *l, int W, int H, AuTabPosition pos, int column_width, int ntabs,
                      int tab_scroll)
{
    memset(l, 0, sizeof *l);
    if (W < 1)
        W = 1;
    if (H < 1)
        H = 1;
    l->width = W;
    l->height = H;
    l->position = pos;
    l->ntabs = ntabs;
    int n = ntabs > 0 ? ntabs : 1;
    int sb = AU_CHROME_SCROLL_W - 1; /* the scroll bar overlaps the window frame by 1 */
    int bottom = H - AU_CHROME_STATUS_H;
    int top;

    if (pos == AU_TABS_TOP) {
        l->tabstrip = irect(0, 0, W, AU_CHROME_TAB_H);
        int newtab_w = 26;
        int avail = W - 8 - newtab_w;
        if (avail < 0)
            avail = 0;
        if (n * AU_CHROME_TAB_MIN_W <= avail) {
            l->tab_size = clampi(avail / n, AU_CHROME_TAB_MIN_W, AU_CHROME_TAB_MAX_W);
            l->tabs_area = irect(4, 0, 4 + avail, AU_CHROME_TAB_H);
            int x = 4 + ntabs * l->tab_size + 3;
            l->new_tab = irect(x, 4, x + 22, AU_CHROME_TAB_H - 3);
        } else {
            l->overflow = true;
            l->tab_size = AU_CHROME_TAB_MIN_W;
            int right = W - 4 - newtab_w;
            l->scroll_fwd = irect(right - 17, 4, right - 1, AU_CHROME_TAB_H - 3);
            l->scroll_back = irect(right - 34, 4, right - 18, AU_CHROME_TAB_H - 3);
            l->tabs_area = irect(4, 0, right - 36, AU_CHROME_TAB_H);
            l->new_tab = irect(W - 4 - 22, 4, W - 4, AU_CHROME_TAB_H - 3);
            int aw = l->tabs_area.x1 - l->tabs_area.x0;
            l->tab_scroll_max = ntabs * l->tab_size - (aw > 0 ? aw : 0);
            if (l->tab_scroll_max < 0)
                l->tab_scroll_max = 0;
        }
        top = AU_CHROME_TAB_H;
        l->toolbar = irect(0, top, W, top + AU_CHROME_TOOLBAR_H);
        l->page = irect(0, top + AU_CHROME_TOOLBAR_H, W - sb, bottom);
    } else {
        l->toolbar = irect(0, 0, W, AU_CHROME_TOOLBAR_H);
        top = AU_CHROME_TOOLBAR_H;
        int maxw = W - AU_CHROME_SPLITTER_W - sb - AU_CHROME_PAGE_MIN_W;
        if (maxw > AU_TAB_COLUMN_MAX)
            maxw = AU_TAB_COLUMN_MAX;
        if (maxw < AU_TAB_COLUMN_MIN)
            maxw = AU_TAB_COLUMN_MIN;
        int cw = clampi(column_width, AU_TAB_COLUMN_MIN, maxw);
        l->tabstrip = irect(0, top, cw, bottom);
        l->new_tab = irect(4, top + 4, cw - 4, top + 4 + 20);
        l->tabs_area = irect(0, top + 28, cw, bottom);
        l->tab_size = AU_CHROME_ROW_H;
        int ah = l->tabs_area.y1 - l->tabs_area.y0;
        if (ntabs * AU_CHROME_ROW_H > ah) {
            l->overflow = true;
            l->tabs_area.y1 -= 16;
            l->scroll_back = irect(0, l->tabs_area.y1, cw / 2, bottom);
            l->scroll_fwd = irect(cw / 2, l->tabs_area.y1, cw, bottom);
            ah = l->tabs_area.y1 - l->tabs_area.y0;
            l->tab_scroll_max = ntabs * AU_CHROME_ROW_H - (ah > 0 ? ah : 0);
            if (l->tab_scroll_max < 0)
                l->tab_scroll_max = 0;
        }
        l->splitter = irect(cw, top, cw + AU_CHROME_SPLITTER_W, bottom);
        l->page = irect(cw + AU_CHROME_SPLITTER_W, top, W - sb, bottom);
    }
    if (l->page.y1 < l->page.y0)
        l->page.y1 = l->page.y0;
    if (l->page.x1 < l->page.x0)
        l->page.x1 = l->page.x0;
    l->tab_scroll = clampi(tab_scroll, 0, l->tab_scroll_max);

    l->vscroll = irect(W - sb, l->page.y0 - 1, W + 1, H - AU_CHROME_STATUS_H + 1);
    l->status = irect(0, bottom, W - sb, H);
    l->grow = irect(W - sb, bottom, W, H);

    /* Toolbar: back, forward | reload, home | address field | bookmark,
     * throbber. */
    int ty = l->toolbar.y0 + 4, tb = ty + 24;
    l->back = irect(6, ty, 32, tb);
    l->forward = irect(32, ty, 58, tb);
    l->reload = irect(64, ty, 90, tb);
    l->home = irect(90, ty, 116, tb);
    l->throbber = irect(W - 30, ty, W - 6, tb);
    l->bookmark = irect(l->throbber.x0 - 32, ty, l->throbber.x0 - 6, tb);
    int ax0 = 124, ax1 = l->bookmark.x0 - 6;
    if (ax1 < ax0 + 40)
        ax1 = ax0 + 40;
    l->address = irect(ax0, l->toolbar.y0 + 5, ax1, l->toolbar.y0 + 27);
    l->security = irect(ax0 + 4, l->address.y0 + 3, ax0 + 20, l->address.y1 - 3);
    l->address_text = irect(ax0 + 23, l->address.y0 + 3, ax1 - 4, l->address.y1 - 3);
}

void au_chrome_layout_hscroll(AuChromeLayout *l)
{
    if (!au_irect_empty(l->hscroll))
        return;
    int sb = AU_CHROME_SCROLL_W - 1;
    int bottom = l->page.y1;
    if (bottom - l->page.y0 < AU_CHROME_SCROLL_W + 8)
        return; /* no room */
    int top = bottom - sb;
    /* Like the vertical bar, it overlaps its neighbours' frame lines by
     * one pixel: the status bar's top line and the splitter or window
     * edge on the left. */
    l->hscroll = irect(l->page.x0 - 1, top, l->page.x1 + 1, bottom + 1);
    l->corner = irect(l->page.x1 + 1, top + 1, l->width, bottom); /* inside both bars' frame lines */
    l->page.y1 = top;
    l->vscroll.y1 = top + 1;
}

AuIRect au_chrome_tab_rect(const AuChromeLayout *l, int i)
{
    if (l->position == AU_TABS_TOP) {
        int x = l->tabs_area.x0 + i * l->tab_size - l->tab_scroll;
        return irect(x, 3, x + l->tab_size, AU_CHROME_TAB_H);
    }
    int y = l->tabs_area.y0 + i * l->tab_size - l->tab_scroll;
    return irect(l->tabs_area.x0, y, l->tabs_area.x1, y + l->tab_size);
}

AuIRect au_chrome_tab_close_rect(const AuChromeLayout *l, int i)
{
    AuIRect t = au_chrome_tab_rect(l, i);
    if (l->position == AU_TABS_TOP)
        return irect(t.x1 - 17, 8, t.x1 - 5, 20);
    return irect(t.x1 - 18, t.y0 + 5, t.x1 - 6, t.y0 + 17);
}

int au_chrome_scroll_to_tab(const AuChromeLayout *l, int i)
{
    int start = i * l->tab_size;
    int extent = l->position == AU_TABS_TOP ? l->tabs_area.x1 - l->tabs_area.x0 : l->tabs_area.y1 - l->tabs_area.y0;
    int s = l->tab_scroll;
    if (start < s)
        s = start;
    else if (start + l->tab_size > s + extent)
        s = start + l->tab_size - extent;
    return clampi(s, 0, l->tab_scroll_max);
}

AuHit au_chrome_hit(const AuChromeLayout *l, int x, int y)
{
    AuHit h = {AU_HIT_NONE, -1};
    if (x < 0 || y < 0 || x >= l->width || y >= l->height)
        return h;
    if (l->overflow && au_irect_contains(l->scroll_back, x, y))
        h.part = AU_HIT_TAB_SCROLL_BACK;
    else if (l->overflow && au_irect_contains(l->scroll_fwd, x, y))
        h.part = AU_HIT_TAB_SCROLL_FWD;
    else if (au_irect_contains(l->new_tab, x, y))
        h.part = AU_HIT_NEW_TAB;
    else if (au_irect_contains(l->tabs_area, x, y)) {
        h.part = AU_HIT_TAB_AREA;
        int off = l->position == AU_TABS_TOP ? x - l->tabs_area.x0 : y - l->tabs_area.y0;
        int i = (off + l->tab_scroll) / l->tab_size;
        if (i >= 0 && i < l->ntabs) {
            AuIRect t = au_chrome_tab_rect(l, i);
            if (au_irect_contains(t, x, y)) {
                h.index = i;
                h.part = au_irect_contains(au_chrome_tab_close_rect(l, i), x, y) ? AU_HIT_TAB_CLOSE : AU_HIT_TAB;
            }
        }
    } else if (au_irect_contains(l->tabstrip, x, y))
        h.part = AU_HIT_TAB_AREA;
    else if (au_irect_contains(l->splitter, x, y))
        h.part = AU_HIT_SPLITTER;
    else if (au_irect_contains(l->toolbar, x, y)) {
        h.part = AU_HIT_TOOLBAR;
        if (au_irect_contains(l->back, x, y))
            h.part = AU_HIT_BACK;
        else if (au_irect_contains(l->forward, x, y))
            h.part = AU_HIT_FORWARD;
        else if (au_irect_contains(l->reload, x, y))
            h.part = AU_HIT_RELOAD;
        else if (au_irect_contains(l->home, x, y))
            h.part = AU_HIT_HOME;
        else if (au_irect_contains(l->security, x, y))
            h.part = AU_HIT_SECURITY;
        else if (au_irect_contains(l->address, x, y))
            h.part = AU_HIT_ADDRESS;
        else if (au_irect_contains(l->bookmark, x, y))
            h.part = AU_HIT_BOOKMARK;
        else if (au_irect_contains(l->throbber, x, y))
            h.part = AU_HIT_THROBBER;
    } else if (au_irect_contains(l->grow, x, y))
        h.part = AU_HIT_GROW;
    else if (au_irect_contains(l->corner, x, y))
        h.part = AU_HIT_CORNER;
    else if (au_irect_contains(l->hscroll, x, y))
        h.part = AU_HIT_HSCROLL;
    else if (au_irect_contains(l->vscroll, x, y))
        h.part = AU_HIT_VSCROLL;
    else if (au_irect_contains(l->page, x, y))
        h.part = AU_HIT_PAGE;
    else if (au_irect_contains(l->status, x, y))
        h.part = AU_HIT_STATUS;
    return h;
}

int au_chrome_drop_index(const AuChromeLayout *l, int x, int y)
{
    int off = l->position == AU_TABS_TOP ? x - l->tabs_area.x0 : y - l->tabs_area.y0;
    int i = (off + l->tab_scroll) / (l->tab_size > 0 ? l->tab_size : 1);
    if (off + l->tab_scroll < 0)
        i = 0;
    return clampi(i, 0, l->ntabs > 0 ? l->ntabs - 1 : 0);
}

/* ---- Painting helpers ---- */

/* Platinum greys. */
#define C_FACE AU_RGB(221, 221, 221)
#define C_FACE_DARK AU_RGB(204, 204, 204)
#define C_LIGHT AU_RGB(255, 255, 255)
#define C_SHADOW AU_RGB(153, 153, 153)
#define C_FRAME AU_RGB(102, 102, 102)
#define C_DARK AU_RGB(68, 68, 68)
#define C_INK AU_RGB(0, 0, 0)
#define C_ICON AU_RGB(34, 34, 34)
#define C_ICON_OFF AU_RGB(160, 160, 160)
#define C_TAB_OFF AU_RGB(200, 200, 200)
#define C_FOCUS AU_RGB(128, 128, 204)

static void fill_box(AuCanvas *c, int x0, int y0, int x1, int y1, AuColor col)
{
    if (x1 > x0 && y1 > y0)
        au_fill_rect(c, (AuRect){(float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0)}, col);
}

static void hline(AuCanvas *c, int x0, int x1, int y, AuColor col) { fill_box(c, x0, y, x1, y + 1, col); }
static void vline(AuCanvas *c, int x, int y0, int y1, AuColor col) { fill_box(c, x, y0, x + 1, y1, col); }

/* A raised (or pressed) Platinum bevel with a frame. */
static void bevel(AuCanvas *c, AuIRect r, bool pressed, bool hover)
{
    AuColor face = pressed ? AU_RGB(150, 150, 150) : hover ? AU_RGB(232, 232, 232) : C_FACE;
    AuColor hi = pressed ? AU_RGB(120, 120, 120) : C_LIGHT;
    AuColor lo = pressed ? AU_RGB(180, 180, 180) : C_SHADOW;
    fill_box(c, r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, face);
    hline(c, r.x0 + 1, r.x1 - 1, r.y0 + 1, hi);
    vline(c, r.x0 + 1, r.y0 + 1, r.y1 - 1, hi);
    hline(c, r.x0 + 1, r.x1 - 1, r.y1 - 2, lo);
    vline(c, r.x1 - 2, r.y0 + 1, r.y1 - 1, lo);
    /* Frame with the corner pixels left out (a slight rounding). */
    hline(c, r.x0 + 1, r.x1 - 1, r.y0, C_FRAME);
    hline(c, r.x0 + 1, r.x1 - 1, r.y1 - 1, C_FRAME);
    vline(c, r.x0, r.y0 + 1, r.y1 - 1, C_FRAME);
    vline(c, r.x1 - 1, r.y0 + 1, r.y1 - 1, C_FRAME);
}

/* A sunken edit-text frame. */
static void sunken(AuCanvas *c, AuIRect r)
{
    hline(c, r.x0, r.x1, r.y0, C_SHADOW);
    vline(c, r.x0, r.y0, r.y1, C_SHADOW);
    hline(c, r.x0, r.x1, r.y1 - 1, C_LIGHT);
    vline(c, r.x1 - 1, r.y0, r.y1, C_LIGHT);
    hline(c, r.x0 + 1, r.x1 - 1, r.y0 + 1, C_DARK);
    vline(c, r.x0 + 1, r.y0 + 1, r.y1 - 1, C_DARK);
    hline(c, r.x0 + 1, r.x1 - 1, r.y1 - 2, C_FACE);
    vline(c, r.x1 - 2, r.y0 + 1, r.y1 - 1, C_FACE);
}

static void stroke_path(AuCanvas *c, AuPath *p, float w, AuColor col)
{
    AuPath s;
    au_path_init(&s);
    au_path_stroke(p, w, &s);
    au_fill_path(c, &s, AU_FILL_NONZERO, col);
    au_path_free(&s);
}

static void line(AuCanvas *c, float x0, float y0, float x1, float y1, float w, AuColor col)
{
    AuPath p;
    au_path_init(&p);
    au_path_move_to(&p, x0, y0);
    au_path_line_to(&p, x1, y1);
    stroke_path(c, &p, w, col);
    au_path_free(&p);
}

static void poly(AuCanvas *c, const float *pts, int n, float cx, float cy, AuColor col)
{
    AuPath p;
    au_path_init(&p);
    au_path_move_to(&p, cx + pts[0], cy + pts[1]);
    for (int i = 1; i < n; i++)
        au_path_line_to(&p, cx + pts[2 * i], cy + pts[2 * i + 1]);
    au_path_close(&p);
    au_fill_path(c, &p, AU_FILL_NONZERO, col);
    au_path_free(&p);
}

static void arc_path(AuPath *p, float cx, float cy, float r, float a0, float a1, int steps)
{
    for (int i = 0; i <= steps; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)steps;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if (i == 0)
            au_path_move_to(p, x, y);
        else
            au_path_line_to(p, x, y);
    }
}

/* ---- Icons (centred on cx, cy; about 14 pixels) ---- */

static void icon_arrow(AuCanvas *c, float cx, float cy, bool left, AuColor col)
{
    float s = left ? -1.f : 1.f;
    float tri[] = {s * 6, 0, s * -1, -6, s * -1, 6};
    poly(c, tri, 3, cx, cy, col);
    float x0 = left ? cx + 1 : cx - 6, x1 = left ? cx + 6 : cx - 1;
    au_fill_rect(c, (AuRect){x0, cy - 2, x1 - x0, 4}, col);
}

static void icon_reload(AuCanvas *c, float cx, float cy, AuColor col)
{
    const float pi = 3.14159265f;
    AuPath p;
    au_path_init(&p);
    arc_path(&p, cx, cy, 5.5f, -pi * 0.35f, pi * 1.45f, 18);
    stroke_path(c, &p, 2.0f, col);
    au_path_free(&p);
    /* Arrowhead at the arc's start, pointing along the turn. */
    float ax = cx + 5.5f * cosf(-pi * 0.35f), ay = cy + 5.5f * sinf(-pi * 0.35f);
    float tri[] = {-1.5f, -4.5f, 4.0f, 0.5f, -2.5f, 2.5f};
    poly(c, tri, 3, ax, ay, col);
}

static void icon_stop(AuCanvas *c, float cx, float cy, AuColor col)
{
    line(c, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, 2.2f, col);
    line(c, cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f, 2.2f, col);
}

static void icon_home(AuCanvas *c, float cx, float cy, AuColor col)
{
    float roof[] = {0, -7, 7.5f, 0, 5, 0, 5, 6, 1.5f, 6, 1.5f, 2, -1.5f, 2, -1.5f, 6, -5, 6, -5, 0, -7.5f, 0};
    poly(c, roof, 11, cx, cy, col);
}

static void star_path(AuPath *p, float cx, float cy, float ro, float ri)
{
    const float pi = 3.14159265f;
    for (int i = 0; i < 10; i++) {
        float a = -pi / 2 + (float)i * pi / 5;
        float r = (i & 1) ? ri : ro;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if (i == 0)
            au_path_move_to(p, x, y);
        else
            au_path_line_to(p, x, y);
    }
    au_path_close(p);
}

static void icon_star(AuCanvas *c, float cx, float cy, bool filled, AuColor col)
{
    AuPath p;
    au_path_init(&p);
    star_path(&p, cx, cy + 0.5f, 7.5f, 3.2f);
    if (filled)
        au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(240, 184, 0));
    stroke_path(c, &p, 1.2f, filled ? AU_RGB(120, 80, 0) : col);
    au_path_free(&p);
}

static void icon_page(AuCanvas *c, float x, float y, AuColor col)
{
    /* A sheet with a folded corner, 10 x 12, top-left at (x, y). */
    float pts[] = {0, 0, 7, 0, 10, 3, 10, 12, 0, 12};
    AuPath p;
    au_path_init(&p);
    au_path_move_to(&p, x + 0.5f, y + 0.5f);
    for (int i = 1; i < 5; i++)
        au_path_line_to(&p, x + 0.5f + pts[2 * i], y + 0.5f + pts[2 * i + 1]);
    au_path_close(&p);
    au_fill_path(c, &p, AU_FILL_NONZERO, C_LIGHT);
    stroke_path(c, &p, 1.0f, col);
    au_path_free(&p);
}

/* A site's 16 x 16 icon with its top-left corner at (x, y). */
static void site_icon(AuCanvas *c, const AuSurface *icon, int x, int y)
{
    au_draw_image(c, icon, (AuIRect){0, 0, icon->width, icon->height}, (AuRect){(float)x, (float)y, 16, 16},
                  AU_FILTER_NEAREST, 1.0f);
}

static void icon_lock(AuCanvas *c, float cx, float cy, bool open, AuColor col)
{
    const float pi = 3.14159265f;
    au_fill_rect(c, (AuRect){cx - 5, cy - 1, 10, 7}, col);
    AuPath p;
    au_path_init(&p);
    float sx = open ? cx + 4 : cx;
    arc_path(&p, sx, cy - 3, 3.2f, pi, 2 * pi, 10);
    au_path_line_to(&p, sx + 3.2f, cy - (open ? 3.f : 1.f));
    au_path_move_to(&p, sx - 3.2f, cy - 3);
    au_path_line_to(&p, sx - 3.2f, cy - 1);
    stroke_path(c, &p, 1.6f, col);
    au_path_free(&p);
}

static void icon_security(AuCanvas *c, AuIRect r, AuSecurity s)
{
    float cx = (float)(r.x0 + r.x1) / 2, cy = (float)(r.y0 + r.y1) / 2;
    switch (s) {
    case AU_SECURITY_SECURE: icon_lock(c, cx, cy, false, AU_RGB(0, 120, 40)); break;
    case AU_SECURITY_NOT_SECURE: icon_lock(c, cx - 1, cy, true, AU_RGB(150, 40, 40)); break;
    case AU_SECURITY_MIXED: /* secure document, insecure parts: an amber lock with a bar */
        icon_lock(c, cx, cy - 1, false, AU_RGB(200, 120, 0));
        au_fill_rect(c, (AuRect){cx - 5, cy + 6, 10, 2}, AU_RGB(200, 120, 0));
        break;
    case AU_SECURITY_CERT_ERROR: {
        float tri[] = {0, -7, 7, 6, -7, 6};
        poly(c, tri, 3, cx, cy, AU_RGB(200, 30, 30));
        au_fill_rect(c, (AuRect){cx - 1, cy - 3, 2, 5}, C_LIGHT);
        au_fill_rect(c, (AuRect){cx - 1, cy + 3, 2, 2}, C_LIGHT);
        break;
    }
    default: icon_page(c, cx - 5, cy - 6, AU_RGB(136, 136, 136)); break;
    }
}

static void icon_close(AuCanvas *c, AuIRect r, bool hover, bool pressed)
{
    float cx = (float)(r.x0 + r.x1) / 2, cy = (float)(r.y0 + r.y1) / 2;
    if (hover || pressed) {
        AuPath p;
        au_path_init(&p);
        au_path_ellipse(&p, cx, cy, 6, 6);
        au_fill_path(c, &p, AU_FILL_NONZERO, pressed ? AU_RGB(110, 110, 110) : AU_RGB(160, 160, 160));
        au_path_free(&p);
    }
    AuColor col = hover || pressed ? C_LIGHT : AU_RGB(80, 80, 80);
    line(c, cx - 3, cy - 3, cx + 3, cy + 3, 1.5f, col);
    line(c, cx + 3, cy - 3, cx - 3, cy + 3, 1.5f, col);
}

static void icon_plus(AuCanvas *c, float cx, float cy, AuColor col)
{
    au_fill_rect(c, (AuRect){cx - 5, cy - 1, 10, 2}, col);
    au_fill_rect(c, (AuRect){cx - 1, cy - 5, 2, 10}, col);
}

static void icon_triangle(AuCanvas *c, float cx, float cy, int dir, AuColor col)
{
    /* dir: 0 left, 1 right, 2 up, 3 down */
    static const float t[4][6] = {
        {-3, 0, 2, -4, 2, 4}, {3, 0, -2, -4, -2, 4}, {0, -3, 4, 2, -4, 2}, {0, 3, 4, -2, -4, -2}};
    poly(c, t[dir], 3, cx, cy, col);
}

/* Eight dots on a ring; while loading one dot is dark and the others fade
 * behind it. */
static void throbber(AuCanvas *c, AuIRect r, bool loading, int frame)
{
    const float pi = 3.14159265f;
    float cx = (float)(r.x0 + r.x1) / 2, cy = (float)(r.y0 + r.y1) / 2;
    for (int i = 0; i < 8; i++) {
        float a = (float)i * pi / 4 - pi / 2;
        int age = ((frame - i) % 8 + 8) % 8;
        int g = loading ? 40 + age * 26 : 175;
        if (g > 220)
            g = 220;
        AuPath p;
        au_path_init(&p);
        au_path_ellipse(&p, cx + 7 * cosf(a), cy + 7 * sinf(a), 2.0f, 2.0f);
        au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(g, g, loading ? g + 20 > 255 ? 255 : g + 20 : g));
        au_path_free(&p);
    }
}

/* ---- Text ---- */

AuFontFace *au_chrome_label_face(AuFontSet *fonts)
{
    return au_fontset_match(fonts, AU_CHROME_LABEL_FAMILIES, 400, false);
}


/* A chrome font: a face, and optionally the font list it heads, through
 * which text gets per-character fallback (titles in any script). */
typedef struct ChromeFont {
    AuFontFace *face;
    AuFontList *list;
} ChromeFont;

static float cf_width(ChromeFont f, float size, const char *text, size_t n)
{
    return f.list ? au_text_width_list(f.list, size, text, n) : au_text_width(f.face, size, text, n);
}

static bool cf_has(ChromeFont f, uint32_t cp)
{
    return f.list ? au_fontlist_face_for(f.list, cp, false) != NULL : au_font_glyph(f.face, cp) != 0;
}

static char *ellipsize(ChromeFont f, float size, const char *text, float max_w, char *out, size_t cap);

char *au_chrome_ellipsize(AuFontFace *f, float size, const char *text, float max_w, char *out, size_t cap)
{
    ChromeFont cf = {f, NULL};
    return ellipsize(cf, size, text, max_w, out, cap);
}

char *au_chrome_ellipsize_list(AuFontList *l, float size, const char *text, float max_w, char *out, size_t cap)
{
    ChromeFont cf = {l ? au_fontlist_primary(l) : NULL, l};
    return ellipsize(cf, size, text, max_w, out, cap);
}

static char *ellipsize(ChromeFont f, float size, const char *text, float max_w, char *out, size_t cap)
{
    size_t n = strlen(text);
    if (cap == 0)
        return out;
    if (!f.face || cf_width(f, size, text, n) <= max_w) {
        size_t k = n < cap - 1 ? n : cap - 1;
        /* Do not cut a UTF-8 sequence. */
        while (k > 0 && k < n && ((unsigned char)text[k] & 0xC0) == 0x80)
            k--;
        memcpy(out, text, k);
        out[k] = 0;
        return out;
    }
    const char *ell = cf_has(f, 0x2026) ? "\xE2\x80\xA6" : "...";
    float ew = cf_width(f, size, ell, strlen(ell));
    /* Binary search the longest prefix that fits with the ellipsis. */
    /* Invariant: the prefix of lo bytes fits (or lo is 0), hi does not. */
    size_t lo = 0, hi = n;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        while (mid > lo + 1 && ((unsigned char)text[mid] & 0xC0) == 0x80)
            mid--; /* measure whole characters */
        if (cf_width(f, size, text, mid) + ew <= max_w)
            lo = mid;
        else
            hi = mid;
    }
    while (lo > 0 && lo < n && ((unsigned char)text[lo] & 0xC0) == 0x80)
        lo--;
    while (lo > 0 && text[lo - 1] == ' ')
        lo--;
    size_t el = strlen(ell);
    if (lo + el + 1 > cap)
        lo = cap > el + 1 ? cap - el - 1 : 0;
    memcpy(out, text, lo);
    memcpy(out + lo, ell, el);
    out[lo + el] = 0;
    return out;
}

static void text_in(AuCanvas *c, ChromeFont f, float size, AuIRect r, const char *s, AuColor col, bool centre)
{
    if (!f.face || !*s)
        return;
    char buf[512];
    float maxw = (float)(r.x1 - r.x0);
    ellipsize(f, size, s, maxw, buf, sizeof buf);
    AuFontMetrics m;
    au_font_metrics(f.face, size, &m);
    float th = m.ascent + m.descent;
    float y = (float)r.y0 + ((float)(r.y1 - r.y0) - th) / 2 + m.ascent;
    y = floorf(y + 0.5f);
    float x = (float)r.x0;
    if (centre) {
        float w = cf_width(f, size, buf, strlen(buf));
        x += floorf((maxw - w) / 2);
    }
    if (f.list)
        au_draw_text_list(c, f.list, size, x, y, buf, strlen(buf), col);
    else
        au_draw_text(c, f.face, size, x, y, buf, strlen(buf), col);
}

static ChromeFont label_font(AuFontSet *fonts, const AuChromeState *s)
{
    ChromeFont f = {NULL, s->label_list};
    f.face = f.list ? au_fontlist_primary(f.list) : NULL;
    if (!f.face) {
        f.list = NULL;
        f.face = au_chrome_label_face(fonts);
    }
    return f;
}

static ChromeFont small_font(AuFontSet *fonts, const AuChromeState *s)
{
    ChromeFont f = {NULL, s->small_list};
    f.face = f.list ? au_fontlist_primary(f.list) : NULL;
    if (!f.face) {
        f.list = NULL;
        f.face = au_fontset_match(fonts, AU_CHROME_SMALL_FAMILIES, 400, false);
    }
    return f;
}

/* ---- Parts ---- */

static bool is(AuHit h, AuHitPart part, int index)
{
    return h.part == part && (index < 0 || h.index == index);
}

static void tool_button(AuCanvas *c, const AuChromeState *s, AuIRect r, AuHitPart part, bool enabled)
{
    bool hover = enabled && is(s->hover, part, -1);
    bool pressed = enabled && hover && is(s->pressed, part, -1);
    bevel(c, r, pressed, hover);
}

static const char *tab_label(const AuChromeTab *t)
{
    return t->title && *t->title ? t->title : "Untitled";
}

/* A tab's title: dimmed for an unloaded tab (its document was freed and
 * loads again when the tab is selected). */
static AuColor tab_ink(const AuChromeState *s, int i)
{
    if (s->tabs[i].unloaded)
        return AU_RGB(142, 142, 142);
    return s->active_window ? C_INK : AU_RGB(110, 110, 110);
}

static void paint_tab_top(AuCanvas *c, ChromeFont lf, const AuChromeLayout *l, const AuChromeState *s, int i)
{
    AuIRect r = au_chrome_tab_rect(l, i);
    bool active = i == s->active;
    bool hover = is(s->hover, AU_HIT_TAB, i) || is(s->hover, AU_HIT_TAB_CLOSE, i);
    int y0 = active ? r.y0 : r.y0 + 2;
    int x0 = r.x0, x1 = r.x1 - 1; /* a 1-pixel gap between tabs */
    AuColor face = active ? C_FACE : hover ? AU_RGB(214, 214, 214) : C_TAB_OFF;
    int y1 = active ? r.y1 : r.y1 - 1;
    fill_box(c, x0 + 1, y0 + 1, x1 - 1, y1, face);
    /* Frame: top with cut corners, sides. */
    hline(c, x0 + 2, x1 - 2, y0, C_FRAME);
    fill_box(c, x0 + 1, y0 + 1, x0 + 2, y0 + 2, C_FRAME);
    fill_box(c, x1 - 2, y0 + 1, x1 - 1, y0 + 2, C_FRAME);
    vline(c, x0, y0 + 2, y1, C_FRAME);
    vline(c, x1 - 1, y0 + 2, y1, C_FRAME);
    /* Inner highlight and shadow. */
    hline(c, x0 + 2, x1 - 2, y0 + 1, C_LIGHT);
    vline(c, x0 + 1, y0 + 2, y1, C_LIGHT);
    vline(c, x1 - 2, y0 + 2, y1, active ? C_SHADOW : AU_RGB(170, 170, 170));
    if (active)
        hline(c, x0 + 2, x1 - 2, r.y1 - 1, C_FACE); /* open into the toolbar */
    if (s->tabs[i].loading)
        throbber(c, irect(x0 + 2, y0 + 2, x0 + 20, y1), true, s->throbber_frame);
    else if (s->tabs[i].icon)
        site_icon(c, s->tabs[i].icon, x0 + 3, y0 + 2 + (y1 - y0 - 2 - 16) / 2);
    else
        icon_page(c, (float)x0 + 7, (float)y0 + 5, AU_RGB(100, 100, 100));
    AuIRect cr = au_chrome_tab_close_rect(l, i);
    AuIRect tr = irect(x0 + 21, y0 + 1, cr.x0 - 2, y1);
    text_in(c, lf, 12, tr, tab_label(&s->tabs[i]), tab_ink(s, i), false);
    if (active || hover)
        icon_close(c, cr, is(s->hover, AU_HIT_TAB_CLOSE, i),
                   is(s->hover, AU_HIT_TAB_CLOSE, i) && is(s->pressed, AU_HIT_TAB_CLOSE, i));
}

static void paint_tab_left(AuCanvas *c, ChromeFont lf, const AuChromeLayout *l, const AuChromeState *s, int i)
{
    AuIRect r = au_chrome_tab_rect(l, i);
    bool active = i == s->active;
    bool hover = is(s->hover, AU_HIT_TAB, i) || is(s->hover, AU_HIT_TAB_CLOSE, i);
    if (active) {
        AuIRect b = irect(r.x0 + 3, r.y0 + 1, r.x1 - 3, r.y1 - 1);
        bevel(c, b, false, false);
        fill_box(c, b.x0 + 2, b.y0 + 2, b.x1 - 2, b.y1 - 2, AU_RGB(236, 236, 236));
    } else if (hover) {
        fill_box(c, r.x0 + 3, r.y0 + 1, r.x1 - 3, r.y1 - 1, AU_RGB(214, 214, 214));
    }
    if (s->tabs[i].loading)
        throbber(c, irect(r.x0 + 4, r.y0, r.x0 + 24, r.y1), true, s->throbber_frame);
    else if (s->tabs[i].icon)
        site_icon(c, s->tabs[i].icon, r.x0 + 6, r.y0 + (r.y1 - r.y0 - 16) / 2);
    else
        icon_page(c, (float)r.x0 + 9, (float)r.y0 + 5, AU_RGB(100, 100, 100));
    AuIRect cr = au_chrome_tab_close_rect(l, i);
    AuIRect tr = irect(r.x0 + 25, r.y0, (active || hover) ? cr.x0 - 2 : r.x1 - 6, r.y1);
    text_in(c, lf, 12, tr, tab_label(&s->tabs[i]), tab_ink(s, i), false);
    if (active || hover)
        icon_close(c, cr, is(s->hover, AU_HIT_TAB_CLOSE, i),
                   is(s->hover, AU_HIT_TAB_CLOSE, i) && is(s->pressed, AU_HIT_TAB_CLOSE, i));
}

static void paint_small_button(AuCanvas *c, const AuChromeState *s, AuIRect r, AuHitPart part, int dir)
{
    tool_button(c, s, r, part, true);
    float cx = (float)(r.x0 + r.x1) / 2, cy = (float)(r.y0 + r.y1) / 2;
    if (dir < 0)
        icon_plus(c, cx, cy, C_ICON);
    else
        icon_triangle(c, cx, cy, dir, C_ICON);
}

static void paint_tabs(AuCanvas *c, AuFontSet *fonts, const AuChromeLayout *l, const AuChromeState *s)
{
    ChromeFont lf = label_font(fonts, s);
    AuIRect st = l->tabstrip;
    if (l->position == AU_TABS_TOP) {
        /* Strip background: a little darker than the toolbar, with a
         * frame line along the bottom that the active tab opens. */
        fill_box(c, st.x0, st.y0, st.x1, st.y1, AU_RGB(190, 190, 190));
        hline(c, st.x0, st.x1, st.y0, AU_RGB(170, 170, 170));
        hline(c, st.x0, st.x1, st.y1 - 1, C_FRAME);
    } else {
        fill_box(c, st.x0, st.y0, st.x1, st.y1, AU_RGB(210, 210, 210));
        vline(c, st.x1 - 1, st.y0, st.y1, AU_RGB(190, 190, 190));
        /* The new-tab row is a button with a label. */
        bool hover = is(s->hover, AU_HIT_NEW_TAB, -1);
        bevel(c, l->new_tab, hover && is(s->pressed, AU_HIT_NEW_TAB, -1), hover);
        icon_plus(c, (float)l->new_tab.x0 + 12, (float)(l->new_tab.y0 + l->new_tab.y1) / 2, C_ICON);
        text_in(c, lf, 12, irect(l->new_tab.x0 + 22, l->new_tab.y0, l->new_tab.x1 - 4, l->new_tab.y1), "New Tab",
                C_INK, false);
    }
    au_canvas_clip_push(c, frect(l->tabs_area));
    for (int i = 0; i < l->ntabs; i++) {
        AuIRect r = au_chrome_tab_rect(l, i);
        if (r.x1 <= l->tabs_area.x0 || r.x0 >= l->tabs_area.x1 || r.y1 <= l->tabs_area.y0 || r.y0 >= l->tabs_area.y1)
            continue;
        if (i == s->active && l->position == AU_TABS_TOP)
            continue; /* drawn last, over its neighbours */
        if (l->position == AU_TABS_TOP)
            paint_tab_top(c, lf, l, s, i);
        else
            paint_tab_left(c, lf, l, s, i);
    }
    if (l->position == AU_TABS_TOP && s->active >= 0 && s->active < l->ntabs)
        paint_tab_top(c, lf, l, s, s->active);
    au_canvas_clip_pop(c);
    if (l->position == AU_TABS_TOP) {
        paint_small_button(c, s, l->new_tab, AU_HIT_NEW_TAB, -1);
        if (l->overflow) {
            paint_small_button(c, s, l->scroll_back, AU_HIT_TAB_SCROLL_BACK, 0);
            paint_small_button(c, s, l->scroll_fwd, AU_HIT_TAB_SCROLL_FWD, 1);
        }
    } else if (l->overflow) {
        paint_small_button(c, s, l->scroll_back, AU_HIT_TAB_SCROLL_BACK, 2);
        paint_small_button(c, s, l->scroll_fwd, AU_HIT_TAB_SCROLL_FWD, 3);
    }
}

static void paint_toolbar(AuCanvas *c, AuFontSet *fonts, const AuChromeLayout *l, const AuChromeState *s)
{
    AuIRect t = l->toolbar;
    fill_box(c, t.x0, t.y0, t.x1, t.y1, C_FACE);
    if (l->position == AU_TABS_LEFT)
        hline(c, t.x0, t.x1, t.y0, C_LIGHT);
    hline(c, t.x0, t.x1, t.y1 - 1, C_FRAME);
    hline(c, t.x0, t.x1, t.y1 - 2, C_SHADOW);

    struct {
        AuIRect r;
        AuHitPart part;
        bool enabled;
    } b[] = {{l->back, AU_HIT_BACK, s->can_back},
             {l->forward, AU_HIT_FORWARD, s->can_forward},
             {l->reload, AU_HIT_RELOAD, true},
             {l->home, AU_HIT_HOME, true},
             {l->bookmark, AU_HIT_BOOKMARK, true}};
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++)
        tool_button(c, s, b[i].r, b[i].part, b[i].enabled);
#define CX(r) ((float)((r).x0 + (r).x1) / 2)
#define CY(r) ((float)((r).y0 + (r).y1) / 2)
    icon_arrow(c, CX(l->back), CY(l->back), true, s->can_back ? C_ICON : C_ICON_OFF);
    icon_arrow(c, CX(l->forward), CY(l->forward), false, s->can_forward ? C_ICON : C_ICON_OFF);
    if (s->loading)
        icon_stop(c, CX(l->reload), CY(l->reload), AU_RGB(150, 30, 30));
    else
        icon_reload(c, CX(l->reload), CY(l->reload), C_ICON);
    icon_home(c, CX(l->home), CY(l->home), C_ICON);
    icon_star(c, CX(l->bookmark), CY(l->bookmark), s->bookmarked, C_ICON);
    throbber(c, l->throbber, s->loading, s->throbber_frame);

    /* Address field. */
    AuIRect a = l->address;
    fill_box(c, a.x0 + 2, a.y0 + 2, a.x1 - 2, a.y1 - 2, C_LIGHT);
    sunken(c, a);
    if (s->field_focused) {
        AuIRect f = irect(a.x0 - 2, a.y0 - 2, a.x1 + 2, a.y1 + 2);
        AuBorder bd = {{2, 2, 2, 2}, {C_FOCUS, C_FOCUS, C_FOCUS, C_FOCUS}};
        au_draw_border(c, frect(f), &bd, NULL);
    }
    icon_security(c, l->security, s->security);
    vline(c, l->security.x1 + 1, a.y0 + 4, a.y1 - 4, AU_RGB(210, 210, 210));
    if (s->draw_field_text && s->field_text) {
        ChromeFont f = small_font(fonts, s);
        au_canvas_clip_push(c, frect(l->address_text));
        text_in(c, f, 12, l->address_text, s->field_text, C_INK, false);
        au_canvas_clip_pop(c);
    }
#undef CX
#undef CY
}

static void paint_status(AuCanvas *c, AuFontSet *fonts, const AuChromeLayout *l, const AuChromeState *s)
{
    AuIRect r = l->status;
    fill_box(c, r.x0, r.y0, r.x1, r.y1, C_FACE);
    hline(c, r.x0, r.x1, r.y0, C_FRAME);
    hline(c, r.x0, r.x1, r.y0 + 1, C_LIGHT);
    AuIRect tr = irect(r.x0 + 6, r.y0 + 1, r.x1 - 6, r.y1);
    if (s->loading) {
        int pw = 100;
        if (r.x1 - r.x0 > pw + 80) {
            AuIRect p = irect(r.x1 - pw - 6, r.y0 + 4, r.x1 - 6, r.y1 - 3);
            fill_box(c, p.x0, p.y0, p.x1, p.y1, C_FRAME);
            fill_box(c, p.x0 + 1, p.y0 + 1, p.x1 - 1, p.y1 - 1, AU_RGB(238, 238, 238));
            float pr = s->progress < 0 ? 0 : s->progress > 1 ? 1 : s->progress;
            int w = (int)((float)(p.x1 - p.x0 - 2) * pr);
            fill_box(c, p.x0 + 1, p.y0 + 1, p.x0 + 1 + w, p.y1 - 1, AU_RGB(102, 102, 204));
            hline(c, p.x0 + 1, p.x0 + 1 + w, p.y0 + 1, AU_RGB(153, 153, 238));
            tr.x1 = p.x0 - 8;
        }
    }
    ChromeFont f = small_font(fonts, s);
    /* Blocked requests: a small shield and "N blocked" at the right (left
     * of the progress bar while loading). */
    if (s->blocked > 0 && tr.x1 - tr.x0 > 200) {
        char text[32];
        snprintf(text, sizeof text, "%d blocked", s->blocked);
        int w = (int)(cf_width(f, 10, text, strlen(text)) + 0.99f);
        AuIRect br = irect(tr.x1 - w, r.y0 + 1, tr.x1, r.y1);
        text_in(c, f, 10, br, text, C_INK, false);
        float sx = (float)(br.x0 - 9), sy = (float)(r.y0 + r.y1) / 2 + 0.5f;
        const float shield[] = {-4, -4.5f, 4, -4.5f, 4, 0.5f, 0, 5, -4, 0.5f};
        poly(c, shield, 5, sx, sy, AU_RGB(70, 110, 70));
        tr.x1 = br.x0 - 20;
    }
    /* Progress text in the middle; the status text keeps to its left. */
    if (s->progress_text && *s->progress_text && r.x1 - r.x0 > 320) {
        int mid = (r.x0 + r.x1) / 2;
        AuIRect pr = irect(mid - 70, r.y0 + 1, mid + 70, r.y1);
        if (pr.x1 > tr.x1)
            pr.x1 = tr.x1;
        text_in(c, f, 10, pr, s->progress_text, C_INK, true);
        if (tr.x1 > pr.x0 - 8)
            tr.x1 = pr.x0 - 8;
    }
    if (s->status_text && *s->status_text)
        text_in(c, f, 10, tr, s->status_text, C_INK, false);
}

static void paint_standins(AuCanvas *c, const AuChromeLayout *l)
{
    /* A plain Platinum scroll bar track and an empty grow box, for host
     * renders; the Mac shell has native controls here. */
    AuIRect v = l->vscroll;
    fill_box(c, v.x0, v.y0, v.x1, v.y1, C_FRAME);
    fill_box(c, v.x0 + 1, v.y0 + 1, v.x1 - 1, v.y1 - 1, AU_RGB(204, 204, 204));
    AuIRect up = irect(v.x0, v.y0, v.x1, v.y0 + 16), dn = irect(v.x0, v.y1 - 16, v.x1, v.y1);
    bevel(c, up, false, false);
    bevel(c, dn, false, false);
    icon_triangle(c, (float)(up.x0 + up.x1) / 2, (float)(up.y0 + up.y1) / 2, 2, C_ICON);
    icon_triangle(c, (float)(dn.x0 + dn.x1) / 2, (float)(dn.y0 + dn.y1) / 2, 3, C_ICON);
    if (!au_irect_empty(l->hscroll)) {
        AuIRect hs = l->hscroll;
        fill_box(c, hs.x0, hs.y0, hs.x1, hs.y1, C_FRAME);
        fill_box(c, hs.x0 + 1, hs.y0 + 1, hs.x1 - 1, hs.y1 - 1, AU_RGB(204, 204, 204));
        AuIRect lf = irect(hs.x0, hs.y0, hs.x0 + 16, hs.y1), rt = irect(hs.x1 - 16, hs.y0, hs.x1, hs.y1);
        bevel(c, lf, false, false);
        bevel(c, rt, false, false);
        icon_triangle(c, (float)(lf.x0 + lf.x1) / 2, (float)(lf.y0 + lf.y1) / 2, 0, C_ICON);
        icon_triangle(c, (float)(rt.x0 + rt.x1) / 2, (float)(rt.y0 + rt.y1) / 2, 1, C_ICON);
    }
    AuIRect g = l->grow;
    fill_box(c, g.x0, g.y0, g.x1, g.y1, C_FACE);
    hline(c, g.x0, g.x1, g.y0, C_FRAME);
    vline(c, g.x0, g.y0, g.y1, C_FRAME);
    for (int i = 0; i < 3; i++) {
        int k = 4 + i * 3;
        line(c, (float)g.x1 - (float)k, (float)g.y1 - 2.5f, (float)g.x1 - 2.5f, (float)g.y1 - (float)k, 1.0f,
             C_SHADOW);
    }
}

static bool touches(AuIRect a, AuIRect clip)
{
    return !au_irect_empty(au_irect_intersect(a, clip));
}

void au_chrome_paint(AuCanvas *c, AuFontSet *fonts, const AuChromeLayout *l, const AuChromeState *s, AuIRect clip)
{
    au_canvas_clip_push(c, frect(clip));
    if (touches(l->tabstrip, clip))
        paint_tabs(c, fonts, l, s);
    if (touches(l->toolbar, clip))
        paint_toolbar(c, fonts, l, s);
    if (!au_irect_empty(l->splitter) && touches(l->splitter, clip)) {
        AuIRect sp = l->splitter;
        bool hot = is(s->hover, AU_HIT_SPLITTER, -1) || is(s->pressed, AU_HIT_SPLITTER, -1);
        fill_box(c, sp.x0, sp.y0, sp.x1, sp.y1, hot ? AU_RGB(200, 200, 200) : C_FACE);
        vline(c, sp.x0, sp.y0, sp.y1, C_LIGHT);
        vline(c, sp.x1 - 1, sp.y0, sp.y1, C_FRAME);
        /* A grip: three short dashes in the middle. */
        int my = (sp.y0 + sp.y1) / 2;
        for (int i = -1; i <= 1; i++)
            hline(c, sp.x0 + 1, sp.x1 - 1, my + i * 3, C_SHADOW);
    }
    if (touches(l->status, clip))
        paint_status(c, fonts, l, s);
    if (!au_irect_empty(l->corner) && touches(l->corner, clip))
        fill_box(c, l->corner.x0, l->corner.y0, l->corner.x1, l->corner.y1, C_FACE);
    if (s->draw_native_standins &&
        (touches(l->vscroll, clip) || touches(l->grow, clip) || touches(l->hscroll, clip)))
        paint_standins(c, l);
    au_canvas_clip_pop(c);
}
