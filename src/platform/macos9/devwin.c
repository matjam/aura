/* The developer windows: Page Source, Console, Network, Performance,
 * System Status and Report a Problem.
 *
 * What each window shows, when it refreshes, its parts and their
 * rectangles, its buttons and toggles, the filter field, the Console's
 * input line, the Network selection, Find, Copy All and Save are the
 * model in src/browser/devwin_model.c. This file holds only what is
 * about the Toolbox:
 *   - the window, at most one of each kind, and where it was left (the
 *     develop.windows preference);
 *   - the offscreen GWorld the engine paints into: the bands, the filter
 *     field, the Network column titles, the text views, the input line
 *     and the button bar, copied to the window around the controls;
 *   - the native scroll bars, push buttons, check boxes and radio
 *     buttons, placed on the model's rectangles;
 *   - turning clicks and keys into the model's calls, counting clicks,
 *     and pacing a drag past the edge of the text;
 *   - the AuDevWinHost callbacks: the front page, the machine's facts,
 *     the network stack's text, the Scrap, the Save dialog, the Find
 *     dialog (DLOG 201), the watch cursor and the window title.
 * The window's background colour is the grey of its bands, so that the
 * check boxes, radio buttons and push buttons, which erase to it, sit on
 * the bands without a white box around them. */
#include "mac.h"

#include "browser/chrome.h"
#include "browser/devwin_model.h"

#include <stdio.h>
#include <string.h>

enum { kKinds = WIN_REPORT - WIN_SOURCE + 1 };

typedef struct DevWin {
    MacWin base;
    AuDevWin m;
    GWorldPtr gw;
    AuSurface *surf;
    ControlHandle vscroll, dscroll; /* the text's; the Network detail pane's */
    ControlHandle buttons[AU_DEVWIN_MAX_BUTTONS];
    int nbuttons;
    ControlHandle toggles[AU_DEVWIN_MAX_TOGGLES]; /* the tool band's check boxes or radio buttons */
    int ntoggles;
    /* Clicks close in time and place count up to three. */
    uint32_t click_ticks;
    int clicks;
    Point click_at;
} DevWin;

static DevWin *g_dev[kKinds]; /* by kind - WIN_SOURCE */

/* The last timing test (shown by Performance while its page is the
 * current one) and paint benchmark (shown by System Status). */
static AuTimingTest g_timing;
static const void *g_timing_page;
static AuPaintBench g_bench;

static AuDevWinKind model_kind(WinKind kind)
{
    return (AuDevWinKind)(kind - WIN_SOURCE);
}

static DevWin *dev_of(WinKind kind)
{
    return kind >= WIN_SOURCE && kind <= WIN_REPORT ? g_dev[kind - WIN_SOURCE] : NULL;
}

/* The page the windows show: the active tab of the frontmost browser
 * window. */
static AuPage *current_page(void)
{
    BrowserWin *b = browserwin_front();
    AuTab *t = b ? au_bw_active_tab(browserwin_model(b)) : NULL;
    return t ? t->page : NULL;
}

static void make_gworld(DevWin *dw)
{
    Rect r;
    GetWindowPortBounds(dw->base.win, &r);
    OffsetRect(&r, (short)-r.left, (short)-r.top);
    if (dw->surf) {
        au_surface_free(dw->surf);
        dw->surf = NULL;
    }
    if (dw->gw) {
        DisposeGWorld(dw->gw);
        dw->gw = NULL;
    }
    QDErr err = NewGWorld(&dw->gw, 32, &r, NULL, NULL, useTempMem);
    if (err != noErr || !dw->gw)
        err = NewGWorld(&dw->gw, 32, &r, NULL, NULL, 0);
    if (err != noErr || !dw->gw)
        mac_out_of_memory((size_t)r.right * (size_t)r.bottom * 4);
    PixMapHandle pm = GetGWorldPixMap(dw->gw);
    if (!LockPixels(pm))
        mac_out_of_memory((size_t)r.right * (size_t)r.bottom * 4);
    dw->surf = au_surface_wrap(GetPixBaseAddr(pm), r.right, r.bottom, (**pm).rowBytes & 0x3FFF);
    au_surface_clear(dw->surf, AU_RGB(255, 255, 255));
}

/* ---- Where the window was left ---- */

static void save_place(DevWin *dw)
{
    SetPort((GrafPtr)dw->base.win);
    Rect r;
    GetWindowPortBounds(dw->base.win, &r);
    Point tl = {r.top, r.left};
    LocalToGlobal(&tl);
    int place[4] = {tl.h, tl.v, r.right - r.left, r.bottom - r.top};
    AuBuf out;
    au_buf_init(&out);
    au_dev_window_set(g_browser->prefs.dev_windows, au_devwin_kind(dw->m.kind)->name, place, &out);
    if (strcmp(au_buf_cstr(&out), g_browser->prefs.dev_windows ? g_browser->prefs.dev_windows : "") != 0) {
        au_prefs_set_str(&g_browser->prefs.dev_windows, au_buf_cstr(&out));
        g_browser->prefs_gen++; /* the shell saves the preferences file */
    }
    au_buf_free(&out);
}

/* The window's content rectangle on screen: where it was left when that
 * is still reachable, else a default that fits the screen. */
static void initial_place(WinKind kind, Rect *wr)
{
    Rect screen = qd.screenBits.bounds;
    int idx = kind - WIN_SOURCE, place[4];
    const AuDevWinKindInfo *info = au_devwin_kind(model_kind(kind));
    if (au_dev_window_get(g_browser->prefs.dev_windows, info->name, place)) {
        /* Usable when some of the title bar is on the screen, below the
         * menu bar. */
        if (place[0] + place[2] > screen.left + 60 && place[0] < screen.right - 60 && place[1] >= screen.top + 38 &&
            place[1] < screen.bottom - 40) {
            SetRect(wr, (short)place[0], (short)place[1], (short)(place[0] + place[2]), (short)(place[1] + place[3]));
            return;
        }
    }
    int w = screen.right - screen.left - 24, h = info->default_h;
    if (w > 720)
        w = 720;
    int x = screen.right - w - 10 - idx * 2, y = 44 + idx * 20;
    if (x < 6)
        x = 6;
    if (y + h > screen.bottom - 30) /* clear of the Control Strip */
        h = screen.bottom - 30 - y;
    if (h < 160)
        h = 160;
    SetRect(wr, (short)x, (short)y, (short)(x + w), (short)(y + h));
}

/* ---- Scroll bars ----
 * A control's range is 16 bits; a long source has more rows, so the
 * value is scaled when the range does not fit. */
static void set_scroll(ControlHandle c, const AuTextView *tv)
{
    uint32_t max = au_textview_max_top(tv);
    uint32_t cmax = max > 32000 ? 32000 : max;
    uint32_t val = max > 32000 ? (uint32_t)((double)tv->top * 32000.0 / (double)max) : tv->top;
    SetControlMaximum(c, (short)cmax);
    SetControlValue(c, (short)val);
}

static uint32_t top_of_control(ControlHandle c, const AuTextView *tv)
{
    uint32_t max = au_textview_max_top(tv);
    uint32_t val = (uint32_t)GetControlValue(c);
    return max > 32000 ? (uint32_t)((double)val * (double)max / 32000.0) : val;
}

static void sync_scroll(DevWin *dw)
{
    SetPort((GrafPtr)dw->base.win);
    set_scroll(dw->vscroll, &dw->m.tv);
    if (dw->dscroll)
        set_scroll(dw->dscroll, &dw->m.dv);
}

/* The check boxes and radio buttons show the model's state. */
static void sync_toggles(DevWin *dw)
{
    for (int i = 0; i < dw->ntoggles; i++)
        if ((GetControlValue(dw->toggles[i]) != 0) != dw->m.toggle_on[i])
            SetControlValue(dw->toggles[i], dw->m.toggle_on[i] ? 1 : 0);
}

/* ---- Painting ---- */

static void band_back(void)
{
    RGBColor grey = {0xDDDD, 0xDDDD, 0xDDDD};
    RGBBackColor(&grey);
}

/* Draws text in the monospace face so that its end stays in view when it
 * is wider than room; returns the x after it. */
static float draw_tail(AuCanvas *c, DevWin *dw, float x, float base, float room, const char *text, size_t len,
                       AuColor ink)
{
    AuTextView *tv = &dw->m.tv;
    while (len && au_text_width_list(tv->font, tv->size, text, len) > room) {
        size_t adv = 1;
        au_utf8_decode(text, len, &adv);
        text += adv;
        len -= adv;
    }
    return x + au_draw_text_list(c, tv->font, tv->size, x, base, text, len, ink);
}

static void dw_paint(DevWin *dw)
{
    if (!dw->surf)
        return;
    AuDevWin *m = &dw->m;
    AuDevWinAreas a;
    au_devwin_areas(m, &a);
    bool active = dw->base.win == FrontWindow() && g_in_front;
    m->tv.active = m->dv.active = active;
    m->tv.highlight = m->dv.highlight = g_browser->highlight;
    AuCanvas *c = au_canvas_new(dw->surf);
    int W = a.all.right, H = a.all.bottom;
    AuFontFace *small = au_fontset_match(g_fonts, AU_CHROME_SMALL_FAMILIES, 400, false);
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
            x = draw_tail(c, dw, 5 + x, base, (float)(W - kScrollBarWidth - 24), au_buf_cstr(&m->input), m->input.len,
                          AU_RGB(0, 0, 0));
            if (active && !m->filter_focus)
                au_fill_rect(c, (AuRect){x + 1, (float)a.input.top + 3, 1, AU_DEVWIN_INPUT_H - 6}, AU_RGB(0, 0, 0));
        }
    }
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H), (float)W, AU_DEVWIN_BAR_H}, AU_RGB(221, 221, 221));
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H), (float)W, 1}, AU_RGB(102, 102, 102));
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H + 1), (float)W, 1}, AU_RGB(255, 255, 255));
    if (small && m->footer[0]) {
        int x = 10;
        if (dw->nbuttons) {
            Rect br = (**dw->buttons[dw->nbuttons - 1]).contrlRect;
            x = br.right + 10;
        }
        char buf[320];
        au_chrome_ellipsize(small, 10, m->footer, (float)(W - x - kGrowBox - 4), buf, sizeof buf);
        au_draw_text(c, small, 10, (float)x, (float)(H - AU_DEVWIN_BAR_H + 20), buf, strlen(buf), AU_RGB(40, 40, 40));
    }
    au_canvas_free(c);

    SetPort((GrafPtr)dw->base.win);
    Rect src;
    SetRect(&src, 0, 0, (short)W, (short)H);
    RgnHandle mask = NewRgn(), cut = NewRgn();
    if (!mask || !cut)
        mac_out_of_memory(64);
    RectRgn(mask, &src);
    Rect cr = (**dw->vscroll).contrlRect;
    RectRgn(cut, &cr);
    DiffRgn(mask, cut, mask);
    if (dw->dscroll) {
        cr = (**dw->dscroll).contrlRect;
        RectRgn(cut, &cr);
        DiffRgn(mask, cut, mask);
    }
    for (int i = 0; i < dw->nbuttons; i++) {
        cr = (**dw->buttons[i]).contrlRect;
        RectRgn(cut, &cr);
        DiffRgn(mask, cut, mask);
    }
    for (int i = 0; i < dw->ntoggles; i++) {
        cr = (**dw->toggles[i]).contrlRect;
        RectRgn(cut, &cr);
        DiffRgn(mask, cut, mask);
    }
    Rect g;
    SetRect(&g, (short)(W - kGrowBox), (short)(H - kGrowBox), (short)W, (short)H);
    RectRgn(cut, &g);
    DiffRgn(mask, cut, mask);
    ForeColor(blackColor);
    BackColor(whiteColor);
    CopyBits(&((GrafPtr)dw->gw)->portBits, &((GrafPtr)dw->base.win)->portBits, &src, &src, srcCopy | kDitherCopy,
             mask);
    band_back();
    DisposeRgn(mask);
    DisposeRgn(cut);
}

/* After the model changed: the scroll bars, the toggles and the pixels. */
static void update(DevWin *dw)
{
    sync_scroll(dw);
    sync_toggles(dw);
    dw_paint(dw);
}

/* ---- The model's callbacks ---- */

static AuPage *h_page(void *ctx)
{
    (void)ctx;
    return current_page();
}

static AuBrowserWindow *h_window(void *ctx)
{
    (void)ctx;
    BrowserWin *bw = browserwin_front();
    return bw ? browserwin_model(bw) : NULL;
}

static uint32_t h_now(void *ctx)
{
    (void)ctx;
    return au_platform_time_ms();
}

static void h_host_info(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn)
{
    (void)ctx;
    mac_sysinfo(sys);
    h->have_memory = true;
    h->partition_kb = sys->partition_kb;
    h->heap_free_kb = sys->heap_free_kb;
    h->heap_block_kb = sys->heap_block_kb;
    h->temp_free_kb = sys->temp_free_kb;
    h->temp_used_kb = (uint32_t)(mac_heap_temp_bytes() / 1024);
    AuHeapStats hs;
    au_heap_stats(&hs);
    h->long_kb = (uint32_t)(hs.long_bytes / 1024);
    h->long_temp_kb = (uint32_t)(mac_heap_long_temp_bytes() / 1024);
    h->temp_pins = (uint32_t)mac_temp_pins();
    mac_net_dev_info(h, conn);
}

static void h_connections(void *ctx, AuBuf *out)
{
    (void)ctx;
    mac_net_connections_text(out);
}

/* Continues the System Status report's Fonts section, then the
 * network's. */
static void h_status_sections(void *ctx, AuBuf *out)
{
    (void)ctx;
    au_report_kv(out, "Found at start in", "%lu ms   (%d bundled faces in %lu ms)", (unsigned long)mac_fonts_ms(),
                 mac_fonts_bundled(), (unsigned long)mac_fonts_bundled_ms());
    au_buf_putc(out, '\n');
    mac_net_status_text(out);
}

static const AuTimingTest *h_timing(void *ctx, const AuPage *p)
{
    (void)ctx;
    return g_timing.ran && g_timing_page == (const void *)p ? &g_timing : NULL;
}

static const AuPaintBench *h_bench(void *ctx)
{
    (void)ctx;
    return &g_bench;
}

static bool h_copy(void *ctx, const char *text, size_t len)
{
    (void)ctx;
    return mac_put_scrap(text, len);
}

static void h_save_text(void *ctx, const char *prompt, const char *name, const char *text, size_t len)
{
    (void)ctx;
    mac_save_text(prompt, name, text, len);
}

static void h_open_url(void *ctx, const char *url)
{
    (void)ctx;
    mac_open_url(url, true); /* in Aura, in a new tab */
}

/* DLOG 201, the browser's Find dialog, with its Match Case check box
 * hidden: the text view's find ignores case. */
enum { FIND_OK = 1, FIND_CANCEL, FIND_TEXT, FIND_LABEL, FIND_CASE };

static char *h_ask_text(void *ctx, const char *prompt, const char *initial)
{
    (void)ctx;
    DialogPtr d = GetNewDialog(201, NULL, (WindowPtr)-1);
    if (!d)
        return NULL;
    short type;
    Handle h;
    Rect r;
    Str255 p;
    mac_pstr(initial ? initial : "", p);
    GetDialogItem(d, FIND_TEXT, &type, &h, &r);
    SetDialogItemText(h, p);
    SelectDialogItemText(d, FIND_TEXT, 0, 32767);
    mac_pstr(prompt, p);
    GetDialogItem(d, FIND_LABEL, &type, &h, &r);
    SetDialogItemText(h, p);
    HideDialogItem(d, FIND_CASE);
    SetDialogDefaultItem(d, FIND_OK);
    SetDialogCancelItem(d, FIND_CANCEL);
    ShowWindow((WindowPtr)d);
    short item = 0;
    do {
        ModalDialog(NULL, &item);
    } while (item != FIND_OK && item != FIND_CANCEL);
    char *text = NULL;
    if (item == FIND_OK) {
        GetDialogItem(d, FIND_TEXT, &type, &h, &r);
        GetDialogItemText(h, p);
        text = mac_roman_to_utf8((const char *)p + 1, p[0]);
    }
    DisposeDialog(d);
    return text;
}

static void h_run_timing(void *ctx)
{
    (void)ctx;
    devwin_run_timing();
}

static void h_busy(void *ctx, bool on)
{
    (void)ctx;
    if (on)
        SetCursor(*GetCursor(watchCursor));
    else
        InitCursor();
}

static void h_title_changed(void *ctx, const char *title)
{
    DevWin *dw = ctx;
    if (!dw->base.win)
        return;
    Str255 ps, cur;
    mac_pstr(title, ps);
    GetWTitle(dw->base.win, cur);
    if (cur[0] != ps[0] || memcmp(cur + 1, ps + 1, ps[0]) != 0) {
        SetWTitle(dw->base.win, ps);
        mac_menus_dirty();
    }
}

static void h_record_headers(void *ctx)
{
    (void)ctx;
    mac_net_trace_headers(true);
}

/* ---- Find ---- */

void devwin_find(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    AuDevWinResult r = au_devwin_ask_find(&dw->m);
    if (r == AU_DEVWIN_FAILED)
        SysBeep(1);
    else if (r == AU_DEVWIN_DONE)
        update(dw);
}

void devwin_find_again(MacWin *mw, bool forward)
{
    DevWin *dw = (DevWin *)mw;
    if (!au_devwin_find_next(&dw->m, forward)) {
        SysBeep(1);
        return;
    }
    update(dw);
}

bool devwin_can_find_again(MacWin *mw)
{
    return au_devwin_can_find_again(&((DevWin *)mw)->m);
}

/* ---- Events ---- */

static void draw_grow(MacWin *mw)
{
    Rect r, g;
    GetWindowPortBounds(mw->win, &r);
    SetRect(&g, (short)(r.right - kGrowBox), (short)(r.bottom - kGrowBox), r.right, r.bottom);
    ClipRect(&g);
    DrawGrowIcon(mw->win);
    ClipRect(&r);
}

static void dw_update(MacWin *mw)
{
    SetPort((GrafPtr)mw->win);
    BeginUpdate(mw->win);
    dw_paint((DevWin *)mw);
    DrawControls(mw->win);
    draw_grow(mw);
    EndUpdate(mw->win);
}

static void dw_activate(MacWin *mw, bool on)
{
    DevWin *dw = (DevWin *)mw;
    SetPort((GrafPtr)mw->win);
    HiliteControl(dw->vscroll, on ? 0 : 255);
    if (dw->dscroll)
        HiliteControl(dw->dscroll, on ? 0 : 255);
    for (int i = 0; i < dw->nbuttons; i++)
        HiliteControl(dw->buttons[i], on ? 0 : 255);
    for (int i = 0; i < dw->ntoggles; i++)
        HiliteControl(dw->toggles[i], on ? 0 : 255);
    dw_paint(dw);
    draw_grow(mw);
}

static DevWin *g_track;
static AuDevWinPane g_track_pane;

static pascal void dw_scroll_action(ControlHandle c, short part)
{
    DevWin *dw = g_track;
    int key;
    switch (part) {
    case inUpButton: key = AU_KEY_UP; break;
    case inDownButton: key = AU_KEY_DOWN; break;
    case inPageUp: key = AU_KEY_PAGE_UP; break;
    case inPageDown: key = AU_KEY_PAGE_DOWN; break;
    default: return;
    }
    au_devwin_scroll(&dw->m, g_track_pane, key);
    set_scroll(c, au_devwin_view(&dw->m, g_track_pane));
    dw_paint(dw);
}

static void scroll_click(DevWin *dw, ControlHandle c, AuDevWinPane pane, short part, Point pt)
{
    g_track = dw;
    g_track_pane = pane;
    if (part == inThumb) {
        TrackControl(c, pt, NULL);
        au_devwin_scroll_to(&dw->m, pane, top_of_control(c, au_devwin_view(&dw->m, pane)));
        dw_paint(dw);
    } else {
        static ControlActionUPP action;
        if (!action)
            action = NewControlActionUPP(dw_scroll_action);
        TrackControl(c, pt, action);
    }
    g_track = NULL;
}

static void repaint(DevWin *dw)
{
    SetPort((GrafPtr)dw->base.win);
    update(dw);
}

/* A push button: the model's action for it. */
static void button(DevWin *dw, int which)
{
    const AuDevWinButton *b;
    int n = au_devwin_buttons(dw->m.kind, &b);
    if (which < 0 || which >= n)
        return;
    AuDevWinResult r = au_devwin_run(&dw->m, b[which].action);
    if (r == AU_DEVWIN_FAILED)
        SysBeep(1);
    repaint(dw);
}

static void dw_click(MacWin *mw, EventRecord *ev)
{
    DevWin *dw = (DevWin *)mw;
    SetPort((GrafPtr)mw->win);
    Point pt = ev->where;
    GlobalToLocal(&pt);
    ControlHandle c;
    short part = FindControl(pt, mw->win, &c);
    if (c && part) {
        if (c == dw->vscroll)
            scroll_click(dw, c, AU_DEVWIN_PANE_MAIN, part, pt);
        else if (c == dw->dscroll)
            scroll_click(dw, c, AU_DEVWIN_PANE_DETAIL, part, pt);
        else if (TrackControl(c, pt, NULL)) {
            for (int i = 0; i < dw->nbuttons; i++)
                if (dw->buttons[i] == c) {
                    button(dw, i);
                    return;
                }
            for (int i = 0; i < dw->ntoggles; i++)
                if (dw->toggles[i] == c) {
                    au_devwin_toggle(&dw->m, i);
                    repaint(dw);
                    return;
                }
        }
        return;
    }
    AuDevWinPane at = au_devwin_pane_at(&dw->m, pt.h, pt.v);
    if (at == AU_DEVWIN_PANE_MAIN || at == AU_DEVWIN_PANE_DETAIL) {
        int dh = pt.h - dw->click_at.h, dv = pt.v - dw->click_at.v;
        if ((uint32_t)ev->when - dw->click_ticks <= (uint32_t)GetDblTime() && dh > -4 && dh < 4 && dv > -4 &&
            dv < 4 && dw->clicks < 3)
            dw->clicks++;
        else
            dw->clicks = 1;
        dw->click_ticks = (uint32_t)ev->when;
        dw->click_at = pt;
    }
    AuDevWinPane pane = au_devwin_mouse_down(&dw->m, pt.h, pt.v, dw->clicks, (ev->modifiers & shiftKey) != 0);
    update(dw);
    AuTextView *tv = au_devwin_view(&dw->m, pane);
    if (!tv || tv->select_rows)
        return;
    AuDevWinAreas a;
    au_devwin_areas(&dw->m, &a);
    const AuDevRect *area = pane == AU_DEVWIN_PANE_DETAIL ? &a.detail : &a.main;
    while (StillDown()) {
        Point m;
        GetMouse(&m);
        if (au_devwin_mouse_drag(&dw->m, pane, m.h, m.v)) {
            sync_scroll(dw);
            dw_paint(dw);
            /* Past the edge the text scrolls a row at a time: pace it. */
            if (m.v < area->top || m.v >= area->bottom) {
                unsigned long until = TickCount() + 2;
                while (TickCount() < until && StillDown())
                    ;
            }
        }
    }
    au_devwin_mouse_up(&dw->m, pane);
}

/* A Mac key as the model's: the arrows, page keys, Home and End, Delete,
 * Escape, Tab, Return and Enter, or a typed character as UTF-8. */
static int mac_key(unsigned char ch)
{
    switch (ch) {
    case 0x1E: return AU_KEY_UP;
    case 0x1F: return AU_KEY_DOWN;
    case 0x0B: return AU_KEY_PAGE_UP;
    case 0x0C: return AU_KEY_PAGE_DOWN;
    case 0x01: return AU_KEY_HOME;
    case 0x04: return AU_KEY_END;
    case 0x08: return AU_KEY_BACKSPACE;
    case 0x1B: return AU_KEY_ESCAPE;
    case 0x09: return AU_KEY_TAB;
    case 0x0D:
    case 0x03: return AU_KEY_RETURN;
    default: return AU_KEY_NONE;
    }
}

static bool dw_key(MacWin *mw, EventRecord *ev)
{
    DevWin *dw = (DevWin *)mw;
    unsigned char ch = (unsigned char)(ev->message & charCodeMask);
    int key = mac_key(ch);
    char *text = NULL;
    if (key == AU_KEY_NONE && ch >= 0x20 && ch != 0x7F) {
        char mr = (char)ch;
        text = mac_roman_to_utf8(&mr, 1);
    }
    SetPort((GrafPtr)mw->win);
    AuDevWinResult r = au_devwin_key(&dw->m, key, text);
    au_free(text);
    if (r == AU_DEVWIN_NOTHING)
        return false;
    if (r == AU_DEVWIN_FAILED)
        SysBeep(1);
    update(dw);
    return true;
}

static void dw_idle(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    if (au_devwin_refresh(&dw->m, false))
        repaint(dw);
}

/* The controls on the model's rectangles. */
static void place(DevWin *dw)
{
    AuDevWinAreas a;
    au_devwin_areas(&dw->m, &a);
    SetPort((GrafPtr)dw->base.win);
    short W = (short)a.all.right;
    HideControl(dw->vscroll);
    MoveControl(dw->vscroll, (short)(W - kScrollBarWidth + 1), (short)(a.main.top - 1));
    SizeControl(dw->vscroll, kScrollBarWidth, (short)(a.main.bottom - a.main.top + 2));
    ShowControl(dw->vscroll);
    if (dw->dscroll) {
        HideControl(dw->dscroll);
        MoveControl(dw->dscroll, (short)(W - kScrollBarWidth + 1), (short)(a.detail.top - 1));
        SizeControl(dw->dscroll, kScrollBarWidth, (short)(a.detail.bottom - a.detail.top + 2));
        ShowControl(dw->dscroll);
    }
    AuDevRect r;
    for (int i = 0; i < dw->nbuttons; i++) {
        au_devwin_button_rect(&dw->m, i, &r);
        MoveControl(dw->buttons[i], (short)r.left, (short)r.top);
    }
    for (int i = 0; i < dw->ntoggles; i++) {
        au_devwin_toggle_rect(&dw->m, i, &r);
        MoveControl(dw->toggles[i], (short)r.left, (short)r.top);
    }
}

/* The model's size: the window's content. */
static void resize_model(DevWin *dw)
{
    Rect r;
    GetWindowPortBounds(dw->base.win, &r);
    au_devwin_resize(&dw->m, r.right - r.left, r.bottom - r.top);
}

static void dw_grow(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    make_gworld(dw);
    resize_model(dw);
    place(dw);
    sync_scroll(dw);
    Rect r;
    GetWindowPortBounds(mw->win, &r);
    InvalRect(&r);
    save_place(dw);
}

static void dw_moved(MacWin *mw)
{
    save_place((DevWin *)mw);
}

static void dw_close(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    save_place(dw);
    g_dev[mw->kind - WIN_SOURCE] = NULL;
    DisposeWindow(mw->win);
    if (dw->surf)
        au_surface_free(dw->surf);
    if (dw->gw)
        DisposeGWorld(dw->gw);
    au_devwin_free(&dw->m);
    au_free(dw);
    mac_menus_dirty();
}

/* Paste: the Scrap's text, as UTF-8. */
static void paste(DevWin *dw)
{
    Handle h = NewHandle(0);
    long off = 0;
    char *u = NULL;
    if (h) {
        long n = GetScrap(h, 'TEXT', &off);
        if (n > 0) {
            HLock(h);
            u = mac_roman_to_utf8(*h, (size_t)n);
            HUnlock(h);
        }
        DisposeHandle(h);
    }
    au_devwin_paste(&dw->m, u ? u : "");
    au_free(u);
}

/* Edit menu: Copy the selection (of the pane that has one), Paste into
 * the field that has the keyboard, Select All in the main text. */
static bool dw_edit(MacWin *mw, int item)
{
    DevWin *dw = (DevWin *)mw;
    switch (item) {
    case 0: return true;
    case 4: {
        AuBuf t;
        au_buf_init(&t);
        if (au_devwin_copy_text(&dw->m, &t))
            mac_put_scrap(t.data, t.len);
        else
            SysBeep(1);
        au_buf_free(&t);
        return true;
    }
    case 5:
        if (!au_devwin_kind(dw->m.kind)->filter)
            return false;
        paste(dw);
        repaint(dw);
        return true;
    case 7:
        if (!au_devwin_select_all(&dw->m))
            return false;
        repaint(dw);
        return true;
    default: return false;
    }
}

static void add_button(DevWin *dw, const char *title, int width)
{
    Rect r;
    SetRect(&r, 0, 0, (short)width, AU_DEVWIN_BUTTON_H);
    Str255 p;
    mac_pstr(title, p);
    ControlHandle c = NewControl(dw->base.win, &r, p, true, 0, 0, 1, pushButProc, 0);
    if (!c)
        mac_out_of_memory(256);
    dw->buttons[dw->nbuttons++] = c;
}

/* A check box or radio button in the window's font (Geneva 10). */
static void add_toggle(DevWin *dw, const char *title, int width, bool radio, bool on)
{
    Rect r;
    SetRect(&r, 0, 0, (short)width, AU_DEVWIN_TOGGLE_H);
    Str255 p;
    mac_pstr(title, p);
    ControlHandle c = NewControl(dw->base.win, &r, p, true, on ? 1 : 0, 0, 1,
                                 (short)((radio ? radioButProc : checkBoxProc) | useWFont), 0);
    if (!c)
        mac_out_of_memory(256);
    dw->toggles[dw->ntoggles++] = c;
}

void devwin_show(WinKind kind)
{
    if (kind < WIN_SOURCE || kind > WIN_REPORT)
        return;
    DevWin *dw = dev_of(kind);
    if (dw) {
        SelectWindow(dw->base.win);
        SetPort((GrafPtr)dw->base.win);
        if (au_devwin_refresh(&dw->m, true))
            update(dw);
        return;
    }
    dw = au_calloc(sizeof *dw);
    dw->base.kind = kind;
    dw->base.update = dw_update;
    dw->base.activate = dw_activate;
    dw->base.click = dw_click;
    dw->base.key = dw_key;
    dw->base.idle = dw_idle;
    dw->base.grow = dw_grow;
    dw->base.moved = dw_moved;
    dw->base.close = dw_close;
    dw->base.edit = dw_edit;
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
    au_devwin_init(&dw->m, model_kind(kind), g_browser, g_fonts, kScrollBarWidth - 1, &host);

    Rect wr;
    initial_place(kind, &wr);
    Str255 pt;
    mac_pstr(au_devwin_title(&dw->m), pt);
    WindowPtr win = NewCWindow(NULL, &wr, pt, false, kZoomDocProc, (WindowPtr)-1, true, (long)dw);
    if (!win)
        mac_out_of_memory(sizeof(WindowRecord));
    dw->base.win = win;
    SetPort((GrafPtr)win);
    TextFont(applFont); /* Geneva 10 for the tool band's options */
    TextSize(10);
    band_back();
    Rect sr = {0, 0, 10, 10};
    dw->vscroll = NewControl(win, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, scrollBarProc, 0);
    if (!dw->vscroll)
        mac_out_of_memory(256);
    if (au_devwin_kind(dw->m.kind)->detail) {
        dw->dscroll = NewControl(win, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, scrollBarProc, 0);
        if (!dw->dscroll)
            mac_out_of_memory(256);
    }
    const AuDevWinButton *b;
    int nb = au_devwin_buttons(dw->m.kind, &b);
    for (int i = 0; i < nb; i++)
        add_button(dw, b[i].label, b[i].width);
    const AuDevWinToggle *t;
    int nt = au_devwin_toggles(dw->m.kind, &t);
    for (int i = 0; i < nt; i++)
        add_toggle(dw, t[i].label, t[i].width, t[i].radio, dw->m.toggle_on[i]);
    g_dev[kind - WIN_SOURCE] = dw;
    make_gworld(dw);
    resize_model(dw);
    place(dw);
    au_devwin_refresh(&dw->m, true);
    sync_scroll(dw);
    ShowWindow(win);
    SelectWindow(win);
    mac_menus_dirty();
}

void devwin_run_timing(void)
{
    BrowserWin *b = browserwin_front();
    if (!b || !browserwin_timing_test(b, &g_timing)) {
        SysBeep(1);
        return;
    }
    g_timing_page = current_page();
    devwin_show(WIN_PERF);
}

void devwin_run_benchmark(void)
{
    BrowserWin *b = browserwin_front();
    if (!b) {
        SysBeep(1);
        return;
    }
    browserwin_benchmark(b, &g_bench);
    devwin_show(WIN_STATUS);
}
