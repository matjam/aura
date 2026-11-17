/* The developer windows: Page Source, Console, Network, Performance,
 * System Status and Report a Problem.
 *
 * The model (src/browser/devwin_model.h) holds the text, the parts of the
 * window and their rectangles, the buttons and toggles, the filter field
 * and the Console's input line. This file paints the model as the Mac OS 9
 * window does, with the engine's canvas into a Cairo image surface the
 * size of a GtkDrawingArea (the engine's premultiplied 0xAARRGGBB pixels
 * are CAIRO_FORMAT_ARGB32), and places native widgets over it in a
 * GtkOverlay at the model's rectangles: the push buttons, the tool band's
 * check boxes and radio buttons, and a GtkScrollbar for each text pane.
 * The drawing area keeps the keyboard; the widgets over it do not take
 * focus.
 *
 * Wayland gives a window no position, so only its size is kept in the
 * develop.windows preference. */
#include "winutil_gtk.h"

#include <string.h>

#include "browser/chrome.h"

enum { SCROLL_W = 14 }; /* what each text pane leaves at its right for its scroll bar */

typedef struct DevWin {
    AuDevWin m;
    unsigned serial; /* tells a closed window from a new one at the same address */
    GtkWindow *win;
    GtkWidget *overlay, *area;
    GtkWidget *vscroll, *dscroll;
    GtkAdjustment *vadj, *dadj;
    GtkWidget *buttons[AU_DEVWIN_MAX_BUTTONS];
    GtkWidget *toggles[AU_DEVWIN_MAX_TOGGLES];
    int nbuttons, ntoggles;
    cairo_surface_t *surf;
    bool syncing; /* the widgets are being set from the model */
    guint timer, place_idle;
    /* The mouse. */
    int clicks;
    gint64 click_us;
    double click_x, click_y;
    double down_x, down_y, mouse_x, mouse_y;
    AuDevWinPane drag_pane;
    bool dragging;
    guint drag_timer;
} DevWin;

static DevWin *g_dev[AU_DEVWIN_KINDS];
static unsigned g_serial;
static AuTimingTest g_timing;
static const void *g_timing_page;
static AuPaintBench g_bench;

static AuPage *front_page(void) { return au_gtk_host.front_page ? au_gtk_host.front_page(au_gtk_host.ctx) : NULL; }

static void bell(DevWin *dw) { gtk_widget_error_bell(dw->area); }

/* ---- The widgets from the model ---- */

static void set_adjustment(DevWin *dw, GtkAdjustment *adj, const AuTextView *tv)
{
    if (!adj)
        return;
    double page = tv->visible_rows > 0 ? tv->visible_rows : 1;
    double max = au_textview_max_top(tv);
    gtk_adjustment_configure(adj, tv->top, 0, max + page, 1, page > 1 ? page - 1 : 1, page);
}

static void sync_scroll(DevWin *dw)
{
    dw->syncing = true;
    set_adjustment(dw, dw->vadj, &dw->m.tv);
    set_adjustment(dw, dw->dadj, &dw->m.dv);
    dw->syncing = false;
}

static void sync_toggles(DevWin *dw)
{
    dw->syncing = true;
    for (int i = 0; i < dw->ntoggles; i++)
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dw->toggles[i])) != dw->m.toggle_on[i])
            gtk_check_button_set_active(GTK_CHECK_BUTTON(dw->toggles[i]), dw->m.toggle_on[i]);
    dw->syncing = false;
}

/* After the model changed: the scroll bars, the toggles and the pixels. */
static void update(DevWin *dw)
{
    sync_scroll(dw);
    sync_toggles(dw);
    gtk_widget_queue_draw(dw->area);
}

static void put(GtkWidget *w, int x, int y, int width, int height)
{
    gtk_widget_set_margin_start(w, x > 0 ? x : 0);
    gtk_widget_set_margin_top(w, y > 0 ? y : 0);
    gtk_widget_set_size_request(w, width, height);
}

/* The widgets at the model's rectangles for its size. */
static void place(DevWin *dw)
{
    AuDevWinAreas a;
    au_devwin_areas(&dw->m, &a);
    int W = a.all.right;
    if (dw->vscroll)
        put(dw->vscroll, W - SCROLL_W, a.main.top, SCROLL_W, a.main.bottom - a.main.top);
    if (dw->dscroll) {
        gtk_widget_set_visible(dw->dscroll, a.has_detail);
        put(dw->dscroll, W - SCROLL_W, a.detail.top, SCROLL_W, a.detail.bottom - a.detail.top);
    }
    AuDevRect r;
    for (int i = 0; i < dw->nbuttons; i++) {
        au_devwin_button_rect(&dw->m, i, &r);
        put(dw->buttons[i], r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    for (int i = 0; i < dw->ntoggles; i++) {
        au_devwin_toggle_rect(&dw->m, i, &r);
        put(dw->toggles[i], r.left, r.top, -1, r.bottom - r.top);
    }
}

/* ---- Painting ---- */

/* Text in the monospace face, its end kept in view when it is wider than
 * room; returns the x after it. */
static float draw_tail(AuCanvas *c, DevWin *dw, float x, float base, float room, const char *text, size_t len,
                       AuColor ink)
{
    AuTextView *tv = &dw->m.tv;
    while (len && au_text_width_list(tv->font, tv->size, text, len) > room) {
        size_t adv = 1;
        au_utf8_decode(text, len, &adv);
        if (adv < 1 || adv > len)
            adv = 1;
        text += adv;
        len -= adv;
    }
    return x + au_draw_text_list(c, tv->font, tv->size, x, base, text, len, ink);
}

/* Where the footer starts: after the last button as GTK sized it. */
static int footer_x(DevWin *dw)
{
    if (!dw->nbuttons)
        return 10;
    GtkWidget *b = dw->buttons[dw->nbuttons - 1];
    AuDevRect r;
    au_devwin_button_rect(&dw->m, dw->nbuttons - 1, &r);
    int w = gtk_widget_get_width(b);
    return r.left + (w > 0 ? w : r.right - r.left) + 10;
}

static void paint_model(DevWin *dw, AuCanvas *c)
{
    AuDevWin *m = &dw->m;
    AuDevWinAreas a;
    au_devwin_areas(m, &a);
    bool active = gtk_window_is_active(dw->win);
    m->tv.active = m->dv.active = active;
    m->tv.highlight = m->dv.highlight = au_gtk_host.browser->highlight;
    int W = a.all.right, H = a.all.bottom;
    au_fill_rect(c, (AuRect){0, 0, (float)W, (float)H}, AU_RGB(255, 255, 255));
    AuFontFace *small = au_fontset_match(au_gtk_host.fonts, AU_CHROME_SMALL_FAMILIES, 400, false);
    if (a.has_band) {
        int th = a.band.bottom;
        au_fill_rect(c, (AuRect){0, 0, (float)W, (float)th}, AU_RGB(221, 221, 221));
        au_fill_rect(c, (AuRect){0, (float)(th - 1), (float)W, 1}, AU_RGB(153, 153, 153));
        if (a.has_filter) {
            if (small)
                au_draw_text(c, small, 10, 8, 16, "Filter:", 7, AU_RGB(40, 40, 40));
            AuRect f = {(float)a.filter.left, (float)a.filter.top, (float)(a.filter.right - a.filter.left),
                        (float)(a.filter.bottom - a.filter.top)};
            bool focus = m->filter_focus && active;
            au_fill_rect(c, f, focus ? AU_RGB(0, 0, 0) : AU_RGB(119, 119, 119));
            au_fill_rect(c, (AuRect){f.x + 1, f.y + 1, f.w - 2, f.h - 2}, AU_RGB(255, 255, 255));
            if (m->tv.font) {
                float x = draw_tail(c, dw, f.x + 4, f.y + 12, f.w - 10, au_buf_cstr(&m->filter), m->filter.len,
                                    AU_RGB(0, 0, 0));
                if (focus)
                    au_fill_rect(c, (AuRect){x + 1, f.y + 3, 1, f.h - 6}, AU_RGB(0, 0, 0));
            }
        } else if (small) {
            /* Report a Problem: what the text is, and what to do with it. */
            const AuDevWinKindInfo *info = au_devwin_kind(m->kind);
            char buf[200];
            if (info->band_text[0]) {
                au_chrome_ellipsize(small, 10, info->band_text[0], (float)(W - 16), buf, sizeof buf);
                au_draw_text(c, small, 10, 8, 14, buf, strlen(buf), AU_RGB(0, 0, 0));
            }
            if (info->band_text[1])
                au_draw_text(c, small, 10, 8, 30, info->band_text[1], strlen(info->band_text[1]), AU_RGB(40, 40, 40));
        }
    }
    if (a.has_head) {
        au_fill_rect(c, (AuRect){0, (float)a.head.top, (float)W, AU_DEVWIN_HEAD_H}, AU_RGB(238, 238, 238));
        au_fill_rect(c, (AuRect){0, (float)(a.head.bottom - 1), (float)W, 1}, AU_RGB(153, 153, 153));
        if (m->tv.font)
            au_draw_text_list(c, m->tv.font, m->tv.size, (float)m->tv.gutter, (float)(a.head.top + 11), m->header,
                              strlen(m->header), AU_RGB(0, 0, 0));
    }
    au_textview_paint(&m->tv, c, 0, a.main.top);
    if (a.has_detail) {
        au_fill_rect(c, (AuRect){0, (float)(a.detail.top - 1), (float)W, 1}, AU_RGB(102, 102, 102));
        au_textview_paint(&m->dv, c, 0, a.detail.top);
    }
    if (a.has_input) {
        au_fill_rect(c, (AuRect){0, (float)a.input.top, (float)W, AU_DEVWIN_INPUT_H}, AU_RGB(255, 255, 255));
        au_fill_rect(c, (AuRect){0, (float)a.input.top, (float)W, 1}, AU_RGB(153, 153, 153));
        if (m->tv.font) {
            float base = (float)a.input.top + 13;
            float x = au_draw_text_list(c, m->tv.font, m->tv.size, 5, base, "> ", 2, AU_RGB(0, 51, 170));
            x = draw_tail(c, dw, 5 + x, base, (float)(W - SCROLL_W - 24), au_buf_cstr(&m->input), m->input.len,
                          AU_RGB(0, 0, 0));
            if (active && !m->filter_focus)
                au_fill_rect(c, (AuRect){x + 1, (float)a.input.top + 3, 1, AU_DEVWIN_INPUT_H - 6}, AU_RGB(0, 0, 0));
        }
    }
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H), (float)W, AU_DEVWIN_BAR_H}, AU_RGB(221, 221, 221));
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H), (float)W, 1}, AU_RGB(102, 102, 102));
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H + 1), (float)W, 1}, AU_RGB(255, 255, 255));
    if (small && m->footer[0]) {
        int x = footer_x(dw);
        char buf[320];
        au_chrome_ellipsize(small, 10, m->footer, (float)(W - x - 8), buf, sizeof buf);
        au_draw_text(c, small, 10, (float)x, (float)(H - AU_DEVWIN_BAR_H + 20), buf, strlen(buf), AU_RGB(40, 40, 40));
    }
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    DevWin *dw = data;
    if (w <= 0 || h <= 0)
        return;
    if (w != dw->m.width || h != dw->m.height)
        au_devwin_resize(&dw->m, w, h);
    if (!dw->surf || cairo_image_surface_get_width(dw->surf) != w || cairo_image_surface_get_height(dw->surf) != h) {
        if (dw->surf)
            cairo_surface_destroy(dw->surf);
        dw->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    }
    if (cairo_surface_status(dw->surf) != CAIRO_STATUS_SUCCESS)
        return;
    cairo_surface_flush(dw->surf);
    AuSurface *s = au_surface_wrap(cairo_image_surface_get_data(dw->surf), w, h,
                                   cairo_image_surface_get_stride(dw->surf));
    if (!s)
        return;
    AuCanvas *c = au_canvas_new(s);
    if (c) {
        paint_model(dw, c);
        au_canvas_free(c);
    }
    au_surface_free(s);
    cairo_surface_mark_dirty(dw->surf);
    cairo_set_source_surface(cr, dw->surf, 0, 0);
    cairo_paint(cr);
}

/* ---- Size ---- */

static gboolean place_later(gpointer data)
{
    DevWin *dw = data;
    dw->place_idle = 0;
    place(dw);
    return G_SOURCE_REMOVE;
}

static void area_resized(GtkDrawingArea *area, int w, int h, gpointer data)
{
    DevWin *dw = data;
    if (w <= 0 || h <= 0)
        return;
    au_devwin_resize(&dw->m, w, h);
    /* Margins cannot change during the allocation that reported this. */
    if (!dw->place_idle)
        dw->place_idle = g_idle_add(place_later, dw);
    sync_scroll(dw);
}

/* The window's size into the develop.windows preference. */
static void save_place(DevWin *dw)
{
    AuBrowser *b = au_gtk_host.browser;
    if (!b || dw->m.width <= 0 || dw->m.height <= 0)
        return;
    int place[4] = {0, 0, dw->m.width, dw->m.height};
    AuBuf out;
    au_buf_init(&out);
    au_dev_window_set(b->prefs.dev_windows, au_devwin_kind(dw->m.kind)->name, place, &out);
    if (strcmp(au_buf_cstr(&out), b->prefs.dev_windows ? b->prefs.dev_windows : "") != 0) {
        au_prefs_set_str(&b->prefs.dev_windows, au_buf_cstr(&out));
        b->prefs_gen++; /* the caller saves the preferences file */
    }
    au_buf_free(&out);
}

/* ---- What the model asks of the system ---- */

static AuPage *h_page(void *ctx) { return front_page(); }

static AuBrowserWindow *h_window(void *ctx)
{
    return au_gtk_host.front_window ? au_gtk_host.front_window(au_gtk_host.ctx) : NULL;
}

static uint32_t h_now(void *ctx) { return (uint32_t)(g_get_monotonic_time() / 1000); }

static void h_host_info(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn)
{
    if (au_gtk_host.host_info)
        au_gtk_host.host_info(au_gtk_host.ctx, sys, h, conn);
}

static void h_connections(void *ctx, AuBuf *out)
{
    if (au_gtk_host.connections)
        au_gtk_host.connections(au_gtk_host.ctx, out);
}

static void h_status_sections(void *ctx, AuBuf *out)
{
    if (au_gtk_host.status_sections)
        au_gtk_host.status_sections(au_gtk_host.ctx, out);
    else if (au_gtk_host.aura_app)
        au_app_net_status(au_gtk_host.aura_app, out);
}

static const AuTimingTest *h_timing(void *ctx, const AuPage *p)
{
    return g_timing.ran && g_timing_page == (const void *)p ? &g_timing : NULL;
}

static const AuPaintBench *h_bench(void *ctx) { return &g_bench; }

static bool h_copy(void *ctx, const char *text, size_t len)
{
    DevWin *dw = ctx;
    au_gtk_copy(dw->area, text, len);
    return true;
}

static void saved(GObject *src, GAsyncResult *res, gpointer data)
{
    GBytes *bytes = data;
    GError *err = NULL;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        gsize n = 0;
        const void *p = g_bytes_get_data(bytes, &n);
        if (!g_file_replace_contents(f, p, n, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, &err)) {
            au_gtk_alert(NULL, "The report could not be saved.", err ? err->message : NULL);
            g_clear_error(&err);
        }
        g_object_unref(f);
    }
    g_bytes_unref(bytes);
}

static void h_save_text(void *ctx, const char *prompt, const char *name, const char *text, size_t len)
{
    DevWin *dw = ctx;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, prompt ? prompt : "Save");
    if (name)
        gtk_file_dialog_set_initial_name(d, name);
    gtk_file_dialog_save(d, dw->win, NULL, saved, g_bytes_new(text, len));
    g_object_unref(d);
}

static void h_open_url(void *ctx, const char *url)
{
    if (au_gtk_host.open_url)
        au_gtk_host.open_url(au_gtk_host.ctx, url, true);
}

static char *h_ask_text(void *ctx, const char *prompt, const char *initial)
{
    DevWin *dw = ctx;
    return au_gtk_ask_text(dw->win, "Find", prompt, initial);
}

static void h_run_timing(void *ctx) { au_gtk_devwin_run_timing(); }

static void h_busy(void *ctx, bool on)
{
    DevWin *dw = ctx;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(dw->win), on ? "wait" : NULL);
}

static void h_title_changed(void *ctx, const char *title)
{
    DevWin *dw = ctx;
    if (dw->win) {
        gtk_window_set_title(dw->win, title);
        au_gtk_windows_changed();
    }
}

static void h_record_headers(void *ctx)
{
    if (au_gtk_host.record_headers)
        au_gtk_host.record_headers(au_gtk_host.ctx);
}

/* ---- Events ---- */

static void button_clicked(GtkButton *b, gpointer data)
{
    DevWin *dw = data;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "aura-index"));
    const AuDevWinButton *list;
    int n = au_devwin_buttons(dw->m.kind, &list);
    if (i < 0 || i >= n)
        return;
    unsigned serial = dw->serial;
    AuDevWinKind kind = dw->m.kind;
    AuDevWinResult r = au_devwin_run(&dw->m, list[i].action);
    /* Run Timing shows this window again; nothing closes it, but check. */
    if (g_dev[kind] != dw || dw->serial != serial)
        return;
    if (r == AU_DEVWIN_FAILED)
        bell(dw);
    update(dw);
}

static void toggle_toggled(GtkCheckButton *b, gpointer data)
{
    DevWin *dw = data;
    if (dw->syncing)
        return;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "aura-index"));
    bool on = gtk_check_button_get_active(b);
    const AuDevWinToggle *t;
    int n = au_devwin_toggles(dw->m.kind, &t);
    if (i < 0 || i >= n || on == dw->m.toggle_on[i] || (t[i].radio && !on))
        return;
    au_devwin_toggle(&dw->m, i);
    update(dw);
}

static void scrolled(GtkAdjustment *adj, gpointer data)
{
    DevWin *dw = data;
    if (dw->syncing)
        return;
    double v = gtk_adjustment_get_value(adj);
    au_devwin_scroll_to(&dw->m, adj == dw->dadj ? AU_DEVWIN_PANE_DETAIL : AU_DEVWIN_PANE_MAIN,
                        v > 0 ? (uint32_t)(v + 0.5) : 0);
    gtk_widget_queue_draw(dw->area);
}

static gboolean wheel(GtkEventControllerScroll *sc, double dx, double dy, gpointer data)
{
    DevWin *dw = data;
    AuDevWinPane pane = au_devwin_pane_at(&dw->m, (int)dw->mouse_x, (int)dw->mouse_y);
    if (pane != AU_DEVWIN_PANE_DETAIL)
        pane = AU_DEVWIN_PANE_MAIN;
    AuTextView *tv = au_devwin_view(&dw->m, pane);
    if (!tv || dy == 0)
        return FALSE;
    double rows = dy * 3;
    if (gtk_event_controller_scroll_get_unit(sc) == GDK_SCROLL_UNIT_SURFACE)
        rows = dy / (tv->row_h > 0 ? tv->row_h : 12);
    double top = (double)tv->top + rows;
    if (top < 0)
        top = 0;
    au_devwin_scroll_to(&dw->m, pane, (uint32_t)top);
    update(dw);
    return TRUE;
}

static void motion(GtkEventControllerMotion *mc, double x, double y, gpointer data)
{
    DevWin *dw = data;
    dw->mouse_x = x;
    dw->mouse_y = y;
}

/* Past a text pane's top or bottom the selection scrolls a row a call:
 * this paces the calls while the pointer stays there. */
static gboolean drag_tick(gpointer data)
{
    DevWin *dw = data;
    if (!dw->dragging) {
        dw->drag_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (au_devwin_mouse_drag(&dw->m, dw->drag_pane, (int)dw->mouse_x, (int)dw->mouse_y))
        update(dw);
    return G_SOURCE_CONTINUE;
}

static void drag_begin(GtkGestureDrag *g, double x, double y, gpointer data)
{
    DevWin *dw = data;
    gtk_widget_grab_focus(dw->area);
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    AuDevWinPane at = au_devwin_pane_at(&dw->m, (int)x, (int)y);
    gint64 now = g_get_monotonic_time();
    if (at == AU_DEVWIN_PANE_MAIN || at == AU_DEVWIN_PANE_DETAIL) {
        int dbl_ms = 400;
        g_object_get(gtk_widget_get_settings(dw->area), "gtk-double-click-time", &dbl_ms, NULL);
        double ddx = x - dw->click_x, ddy = y - dw->click_y;
        if (now - dw->click_us <= (gint64)dbl_ms * 1000 && ddx > -4 && ddx < 4 && ddy > -4 && ddy < 4 &&
            dw->clicks < 3)
            dw->clicks++;
        else
            dw->clicks = 1;
        dw->click_us = now;
        dw->click_x = x;
        dw->click_y = y;
    }
    dw->down_x = dw->mouse_x = x;
    dw->down_y = dw->mouse_y = y;
    AuDevWinPane pane =
        au_devwin_mouse_down(&dw->m, (int)x, (int)y, dw->clicks ? dw->clicks : 1, (mods & GDK_SHIFT_MASK) != 0);
    update(dw);
    AuTextView *tv = au_devwin_view(&dw->m, pane);
    dw->dragging = tv && !tv->select_rows;
    dw->drag_pane = pane;
}

static void drag_update(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    DevWin *dw = data;
    if (!dw->dragging)
        return;
    dw->mouse_x = dw->down_x + ox;
    dw->mouse_y = dw->down_y + oy;
    if (au_devwin_mouse_drag(&dw->m, dw->drag_pane, (int)dw->mouse_x, (int)dw->mouse_y))
        update(dw);
    AuDevWinAreas a;
    au_devwin_areas(&dw->m, &a);
    const AuDevRect *r = dw->drag_pane == AU_DEVWIN_PANE_DETAIL ? &a.detail : &a.main;
    bool outside = dw->mouse_y < r->top || dw->mouse_y >= r->bottom;
    if (outside && !dw->drag_timer)
        dw->drag_timer = g_timeout_add(40, drag_tick, dw);
    else if (!outside && dw->drag_timer) {
        g_source_remove(dw->drag_timer);
        dw->drag_timer = 0;
    }
}

static void drag_end(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    DevWin *dw = data;
    if (dw->drag_timer) {
        g_source_remove(dw->drag_timer);
        dw->drag_timer = 0;
    }
    if (dw->dragging)
        au_devwin_mouse_up(&dw->m, dw->drag_pane);
    dw->dragging = false;
}

static int gdk_key(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left: return AU_KEY_LEFT;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right: return AU_KEY_RIGHT;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up: return AU_KEY_UP;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down: return AU_KEY_DOWN;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up: return AU_KEY_PAGE_UP;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down: return AU_KEY_PAGE_DOWN;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home: return AU_KEY_HOME;
    case GDK_KEY_End:
    case GDK_KEY_KP_End: return AU_KEY_END;
    case GDK_KEY_BackSpace: return AU_KEY_BACKSPACE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete: return AU_KEY_DELETE;
    case GDK_KEY_Escape: return AU_KEY_ESCAPE;
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return AU_KEY_TAB;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter: return AU_KEY_RETURN;
    default: return AU_KEY_NONE;
    }
}

static gboolean key_pressed(GtkEventControllerKey *kc, guint keyval, guint keycode, GdkModifierType state,
                            gpointer data)
{
    DevWin *dw = data;
    if (state & GDK_CONTROL_MASK) {
        switch (gdk_keyval_to_lower(keyval)) {
        case GDK_KEY_c: au_gtk_devwin_edit(dw->win, AU_EDIT_COPY); return TRUE;
        case GDK_KEY_v: au_gtk_devwin_edit(dw->win, AU_EDIT_PASTE); return TRUE;
        case GDK_KEY_a: au_gtk_devwin_edit(dw->win, AU_EDIT_SELECT_ALL); return TRUE;
        case GDK_KEY_f: au_gtk_devwin_find(dw->win); return TRUE;
        case GDK_KEY_g: au_gtk_devwin_find_again(dw->win, !(state & GDK_SHIFT_MASK)); return TRUE;
        default: return FALSE;
        }
    }
    if (state & GDK_ALT_MASK)
        return FALSE;
    int key = gdk_key(keyval);
    char text[8] = "";
    if (key == AU_KEY_NONE) {
        gunichar u = gdk_keyval_to_unicode(keyval);
        if (u < 0x20 || u == 0x7F)
            return FALSE;
        text[g_unichar_to_utf8(u, text)] = 0;
    }
    AuDevWinResult r = au_devwin_key(&dw->m, key, key == AU_KEY_NONE ? text : NULL);
    if (r == AU_DEVWIN_NOTHING)
        return FALSE;
    if (r == AU_DEVWIN_FAILED)
        bell(dw);
    update(dw);
    return TRUE;
}

static gboolean tick(gpointer data)
{
    DevWin *dw = data;
    if (!au_gtk_dialog_running() && au_devwin_refresh(&dw->m, false))
        update(dw);
    return G_SOURCE_CONTINUE;
}

static void active_changed(GObject *o, GParamSpec *ps, gpointer data)
{
    DevWin *dw = data;
    gtk_widget_queue_draw(dw->area);
}

static gboolean close_request(GtkWindow *w, gpointer data)
{
    return au_gtk_dialog_running(); /* the Find dialog is open over it */
}

static void destroyed(GtkWidget *w, gpointer data)
{
    DevWin *dw = data;
    save_place(dw);
    if (dw->timer)
        g_source_remove(dw->timer);
    if (dw->place_idle)
        g_source_remove(dw->place_idle);
    if (dw->drag_timer)
        g_source_remove(dw->drag_timer);
    if (g_dev[dw->m.kind] == dw)
        g_dev[dw->m.kind] = NULL;
    dw->win = NULL;
    au_devwin_free(&dw->m);
    if (dw->surf)
        cairo_surface_destroy(dw->surf);
    g_free(dw);
    au_gtk_windows_changed();
}

/* ---- Showing ---- */

static GtkWidget *overlay_child(DevWin *dw, GtkWidget *w)
{
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    gtk_widget_set_valign(w, GTK_ALIGN_START);
    gtk_widget_set_focusable(w, FALSE);
    gtk_widget_set_focus_on_click(w, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(dw->overlay), w);
    return w;
}

static GtkWidget *scrollbar(DevWin *dw, GtkAdjustment **adj)
{
    *adj = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    g_signal_connect(*adj, "value-changed", G_CALLBACK(scrolled), dw);
    return overlay_child(dw, gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, *adj));
}

void au_gtk_devwin_show(AuDevWinKind kind)
{
    if ((int)kind < 0 || kind >= AU_DEVWIN_KINDS || !au_gtk_host.browser || !au_gtk_host.fonts)
        return;
    DevWin *dw = g_dev[kind];
    if (dw) {
        if (au_devwin_refresh(&dw->m, true))
            update(dw);
        gtk_window_present(dw->win);
        return;
    }
    au_gtk_window_css();
    dw = g_new0(DevWin, 1);
    dw->serial = ++g_serial;
    AuDevWinHost host;
    memset(&host, 0, sizeof host);
    host.ctx = dw;
    host.page = h_page;
    host.window = h_window;
    host.now_ms = h_now;
    host.host_info = h_host_info;
    host.connections = h_connections;
    host.status_sections = h_status_sections;
    host.timing = h_timing;
    host.bench = h_bench;
    host.copy = h_copy;
    host.save_text = h_save_text;
    host.open_url = h_open_url;
    host.ask_text = h_ask_text;
    host.run_timing = h_run_timing;
    host.busy = h_busy;
    host.title_changed = h_title_changed;
    host.record_headers = h_record_headers;
    au_devwin_init(&dw->m, kind, au_gtk_host.browser, au_gtk_host.fonts, SCROLL_W, &host);
    const AuDevWinKindInfo *info = au_devwin_kind(kind);

    GtkWidget *w = gtk_window_new();
    dw->win = GTK_WINDOW(w);
    gtk_window_set_title(dw->win, au_devwin_title(&dw->m));
    int saved_place[4], width = 720, height = info->default_h;
    if (au_dev_window_get(au_gtk_host.browser->prefs.dev_windows, info->name, saved_place) && saved_place[2] >= 200 &&
        saved_place[3] >= 160) {
        width = saved_place[2];
        height = saved_place[3];
    }
    if (height < 160)
        height = 160;
    gtk_window_set_default_size(dw->win, width, height);
    if (au_gtk_host.app)
        gtk_window_set_application(dw->win, au_gtk_host.app);
    gtk_widget_add_css_class(w, "aura-dev");

    dw->overlay = gtk_overlay_new();
    dw->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(dw->area, 320, 160);
    gtk_widget_set_hexpand(dw->area, TRUE);
    gtk_widget_set_vexpand(dw->area, TRUE);
    gtk_widget_set_focusable(dw->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(dw->area), draw, dw, NULL);
    g_signal_connect(dw->area, "resize", G_CALLBACK(area_resized), dw);
    gtk_overlay_set_child(GTK_OVERLAY(dw->overlay), dw->area);
    gtk_window_set_child(dw->win, dw->overlay);

    dw->vscroll = scrollbar(dw, &dw->vadj);
    if (info->detail)
        dw->dscroll = scrollbar(dw, &dw->dadj);
    const AuDevWinButton *b;
    dw->nbuttons = au_devwin_buttons(kind, &b);
    for (int i = 0; i < dw->nbuttons; i++) {
        dw->buttons[i] = overlay_child(dw, gtk_button_new_with_label(b[i].label));
        g_object_set_data(G_OBJECT(dw->buttons[i]), "aura-index", GINT_TO_POINTER(i));
        g_signal_connect(dw->buttons[i], "clicked", G_CALLBACK(button_clicked), dw);
    }
    const AuDevWinToggle *t;
    dw->ntoggles = au_devwin_toggles(kind, &t);
    GtkCheckButton *group = NULL;
    for (int i = 0; i < dw->ntoggles; i++) {
        GtkWidget *c = gtk_check_button_new_with_label(t[i].label);
        if (t[i].radio) {
            if (group)
                gtk_check_button_set_group(GTK_CHECK_BUTTON(c), group);
            else
                group = GTK_CHECK_BUTTON(c);
        }
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c), dw->m.toggle_on[i]);
        g_object_set_data(G_OBJECT(c), "aura-index", GINT_TO_POINTER(i));
        g_signal_connect(c, "toggled", G_CALLBACK(toggle_toggled), dw);
        dw->toggles[i] = overlay_child(dw, c);
    }

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), dw);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), dw);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), dw);
    gtk_widget_add_controller(dw->area, GTK_EVENT_CONTROLLER(drag));
    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(motion), dw);
    gtk_widget_add_controller(dw->area, mc);
    GtkEventController *sc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(sc, "scroll", G_CALLBACK(wheel), dw);
    gtk_widget_add_controller(dw->area, sc);
    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(key_pressed), dw);
    gtk_widget_add_controller(dw->area, kc);

    g_signal_connect(w, "notify::is-active", G_CALLBACK(active_changed), dw);
    g_signal_connect(w, "close-request", G_CALLBACK(close_request), dw);
    g_signal_connect(w, "destroy", G_CALLBACK(destroyed), dw);

    g_dev[kind] = dw;
    au_devwin_resize(&dw->m, width, height);
    place(dw);
    au_devwin_refresh(&dw->m, true);
    sync_scroll(dw);
    dw->timer = g_timeout_add(250, tick, dw);
    gtk_window_present(dw->win);
    gtk_widget_grab_focus(dw->area);
    au_gtk_windows_changed();
}

void au_gtk_devwin_run_timing(void)
{
    AuPage *p = front_page();
    if (!p || !au_gtk_host.timing_test || !au_gtk_host.timing_test(au_gtk_host.ctx, &g_timing)) {
        GtkWindow *w = au_gtk_front_parent();
        if (w)
            gtk_widget_error_bell(GTK_WIDGET(w));
        return;
    }
    g_timing_page = p;
    au_gtk_devwin_show(AU_DEVWIN_PERF);
}

void au_gtk_devwin_run_benchmark(void)
{
    if (!au_gtk_host.benchmark || !au_gtk_host.benchmark(au_gtk_host.ctx, &g_bench)) {
        GtkWindow *w = au_gtk_front_parent();
        if (w)
            gtk_widget_error_bell(GTK_WIDGET(w));
        return;
    }
    au_gtk_devwin_show(AU_DEVWIN_STATUS);
}

static DevWin *dev_of(GtkWindow *w)
{
    if (!w)
        return NULL;
    for (int i = 0; i < AU_DEVWIN_KINDS; i++)
        if (g_dev[i] && g_dev[i]->win == w)
            return g_dev[i];
    return NULL;
}

bool au_gtk_devwin_owns(GtkWindow *w) { return dev_of(w) != NULL; }

/* Paste arrives later; the window may have closed by then. */
typedef struct PasteReq {
    AuDevWinKind kind;
    unsigned serial;
} PasteReq;

static void pasted(GObject *src, GAsyncResult *res, gpointer data)
{
    PasteReq *req = data;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    DevWin *dw = g_dev[req->kind];
    if (dw && dw->serial == req->serial) {
        if (text && au_devwin_paste(&dw->m, text))
            update(dw);
        else
            bell(dw);
    }
    g_free(text);
    g_free(req);
}

bool au_gtk_devwin_edit(GtkWindow *w, AuEditAction action)
{
    DevWin *dw = dev_of(w);
    if (!dw)
        return false;
    switch (action) {
    case AU_EDIT_COPY: {
        AuBuf b;
        au_buf_init(&b);
        bool ok = au_devwin_copy_text(&dw->m, &b);
        if (ok)
            au_gtk_copy(dw->area, au_buf_cstr(&b), b.len);
        au_buf_free(&b);
        return ok;
    }
    case AU_EDIT_PASTE: {
        if (!au_devwin_kind(dw->m.kind)->filter)
            return false;
        PasteReq *req = g_new(PasteReq, 1);
        req->kind = dw->m.kind;
        req->serial = dw->serial;
        gdk_clipboard_read_text_async(gtk_widget_get_clipboard(dw->area), NULL, pasted, req);
        return true;
    }
    case AU_EDIT_SELECT_ALL:
        if (!au_devwin_select_all(&dw->m))
            return false;
        update(dw);
        return true;
    default: return false;
    }
}

void au_gtk_devwin_find(GtkWindow *w)
{
    DevWin *dw = dev_of(w);
    if (!dw)
        return;
    unsigned serial = dw->serial;
    AuDevWinKind kind = dw->m.kind;
    AuDevWinResult r = au_devwin_ask_find(&dw->m);
    if (g_dev[kind] != dw || dw->serial != serial)
        return;
    if (r == AU_DEVWIN_FAILED)
        bell(dw);
    update(dw);
}

void au_gtk_devwin_find_again(GtkWindow *w, bool forward)
{
    DevWin *dw = dev_of(w);
    if (!dw)
        return;
    if (!au_devwin_find_next(&dw->m, forward))
        bell(dw);
    update(dw);
}

bool au_gtk_devwin_can_find_again(GtkWindow *w)
{
    DevWin *dw = dev_of(w);
    return dw && au_devwin_can_find_again(&dw->m);
}

void au_gtk_devwins_close(void)
{
    for (int i = 0; i < AU_DEVWIN_KINDS; i++)
        if (g_dev[i] && g_dev[i]->win)
            gtk_window_destroy(g_dev[i]->win);
}
