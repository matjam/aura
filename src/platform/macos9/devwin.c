/* The developer windows: Page Source, Console, Network, Performance,
 * System Status and Report a Problem.
 *
 * Each is a document window whose text is painted by the engine's text
 * view (src/browser/textview.c) into an offscreen GWorld. There is at
 * most one window of each kind. The first four show the active tab of
 * the frontmost browser window and follow it: every idle pass looks the
 * tab up again (a window never keeps a page pointer to use later) and
 * rebuilds its text when the page's change counter moved.
 *
 * All six are laid out alike, top to bottom:
 *   - a tool band, where the window has a filter or options;
 *   - the text, in the monospace face, with a scroll bar; a selection is
 *     made by dragging and copied with Edit > Copy;
 *   - a bar of push buttons, Copy All first and Find second, with one
 *     line of status text after them.
 * Each window comes back where it was left and at the size it had (the
 * develop.windows preference).
 *
 *   Page Source  the main document's bytes with line numbers and colour
 *                (sourceview.c)
 *   Console      the page's console lines (devtools.c), filtered by level
 *                and text, and an input line: type an expression, Return
 *                evaluates it in the page; Up and Down recall earlier
 *                input; Tab moves between the filter and the input line
 *   Network      one row per request (netlog.c), filtered by text and by
 *                kind (all, problems, blocked), sorted by a click on a
 *                column title; a click on a row shows the request's
 *                details and headers in the lower pane, which shows the
 *                summary and the problem requests otherwise. Opening the
 *                window turns header recording on for later loads.
 *   Performance  the report (au_dev_perf_report), rebuilt once a second;
 *                Run Timing Test adds its table to it
 *   System Status  the machine, the clock, Aura's memory and the network
 *                stack (sysinfo.c), rebuilt every two seconds; the paint
 *                benchmark adds its table to it
 *   Report a Problem  the bug report (bugreport.c) as it will be copied:
 *                Copy Report, Save Report and Open GitHub Issues. Nothing
 *                is sent anywhere.
 * A report that rebuilds itself waits while text in it is selected, so
 * that a selection can be copied. */
#include "mac.h"

#include "browser/bugreport.h"
#include "browser/chrome.h"
#include "browser/devtools.h"
#include "browser/netlog.h"
#include "browser/sourceview.h"
#include "browser/sysinfo.h"
#include "browser/textview.h"

#include <stdio.h>
#include <string.h>

enum {
    kBarH = 32,    /* the button bar */
    kToolH = 24,   /* the tool band */
    kReportH = 40, /* the Report window's band: two lines of text */
    kHeadH = 15,   /* the Network window's column titles */
    kInputH = 19,
    kMaxButtons = 3,
    kMaxToggles = 3,
    kHistory = 16,
    kFieldX = 46, /* the filter field in the tool band */
    kFieldW = 150,
    kKinds = WIN_REPORT - WIN_SOURCE + 1
};

typedef struct DevWin {
    MacWin base;
    GWorldPtr gw;
    AuSurface *surf;
    ControlHandle vscroll, dscroll; /* the text's; the Network detail pane's */
    ControlHandle buttons[kMaxButtons];
    int nbuttons;
    ControlHandle toggles[kMaxToggles]; /* the tool band's check boxes or radio buttons */
    int ntoggles;
    AuTextView tv, dv; /* the main text; the Network detail pane */
    AuTextList list, dlist;
    AuSourceView sv;
    bool have_sv;
    AuNetView nv;
    AuBuf text; /* Performance, System Status, Report: the text shown */
    /* What is shown, to notice changes. page is compared, never used. */
    const void *page;
    const char *src_data;
    size_t src_len;
    uint32_t seen;       /* the console's line count or the log's generation */
    uint32_t last_ms;    /* when the text was last rebuilt */
    uint32_t detail_seq; /* Network: the request in the detail pane; 0 for none */
    char footer[320];
    char header[160];
    /* The filter field (Console and Network). */
    AuBuf filter;
    bool filter_focus;
    /* Console input. */
    AuBuf input;
    char *history[kHistory];
    int nhistory, history_at;
    /* Clicks and find. */
    uint32_t click_ticks;
    int clicks;
    Point click_at;
    char *find;
} DevWin;

static DevWin *g_dev[kKinds]; /* by kind - WIN_SOURCE */

/* The last timing test (shown by Performance while its page is the
 * current one) and paint benchmark (shown by System Status). */
static AuTimingTest g_timing;
static const void *g_timing_page;
static AuPaintBench g_bench;

static const char *const kind_names[kKinds] = {"source", "console", "network", "performance", "status", "report"};
static const char *const kind_titles[kKinds] = {"Page Source", "Console", "Network", "Performance", "System Status",
                                                "Report a Problem"};

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

static bool has_filter(const DevWin *dw)
{
    return dw->base.kind == WIN_CONSOLE || dw->base.kind == WIN_NETWORK;
}

/* ---- Geometry ---- */

typedef struct Areas {
    Rect all, tool, field, head, main, detail, input, bar;
    bool has_tool, has_head, has_detail, has_input;
} Areas;

static void areas(DevWin *dw, Areas *a)
{
    memset(a, 0, sizeof *a);
    GetWindowPortBounds(dw->base.win, &a->all);
    short W = a->all.right, H = a->all.bottom;
    SetRect(&a->bar, 0, (short)(H - kBarH), W, H);
    short top = 0, bottom = (short)(H - kBarH);
    WinKind kind = dw->base.kind;
    if (has_filter(dw) || kind == WIN_REPORT) {
        short th = kind == WIN_REPORT ? kReportH : kToolH;
        a->has_tool = true;
        SetRect(&a->tool, 0, 0, W, th);
        SetRect(&a->field, kFieldX, 4, kFieldX + kFieldW, 20);
        top = th;
    }
    if (kind == WIN_NETWORK) {
        a->has_head = true;
        SetRect(&a->head, 0, top, W, (short)(top + kHeadH));
        top = (short)(top + kHeadH);
        a->has_detail = true;
        short dh = (short)((bottom - top) * 2 / 5);
        SetRect(&a->detail, 0, (short)(bottom - dh), (short)(W - kScrollBarWidth + 1), bottom);
        bottom = (short)(bottom - dh - 1);
    }
    if (kind == WIN_CONSOLE) {
        a->has_input = true;
        SetRect(&a->input, 0, (short)(bottom - kInputH), W, bottom);
        bottom = (short)(bottom - kInputH);
    }
    SetRect(&a->main, 0, top, (short)(W - kScrollBarWidth + 1), bottom);
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
    au_dev_window_set(g_browser->prefs.dev_windows, kind_names[dw->base.kind - WIN_SOURCE], place, &out);
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
    static const short heights[kKinds] = {360, 300, 380, 400, 400, 400};
    Rect screen = qd.screenBits.bounds;
    int idx = kind - WIN_SOURCE, place[4];
    if (au_dev_window_get(g_browser->prefs.dev_windows, kind_names[idx], place)) {
        /* Usable when some of the title bar is on the screen, below the
         * menu bar. */
        if (place[0] + place[2] > screen.left + 60 && place[0] < screen.right - 60 && place[1] >= screen.top + 38 &&
            place[1] < screen.bottom - 40) {
            SetRect(wr, (short)place[0], (short)place[1], (short)(place[0] + place[2]), (short)(place[1] + place[3]));
            return;
        }
    }
    int w = screen.right - screen.left - 24, h = heights[idx];
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
    set_scroll(dw->vscroll, &dw->tv);
    if (dw->dscroll)
        set_scroll(dw->dscroll, &dw->dv);
}

/* ---- Painting ---- */

/* The window's background colour is the grey of its bands, so that the
 * check boxes, radio buttons and push buttons, which erase to it, sit on
 * the bands without a white box around them. */
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
    while (len && au_text_width_list(dw->tv.font, dw->tv.size, text, len) > room) {
        size_t adv = 1;
        au_utf8_decode(text, len, &adv);
        text += adv;
        len -= adv;
    }
    return x + au_draw_text_list(c, dw->tv.font, dw->tv.size, x, base, text, len, ink);
}

static void dw_paint(DevWin *dw)
{
    if (!dw->surf)
        return;
    Areas a;
    areas(dw, &a);
    bool active = dw->base.win == FrontWindow() && g_in_front;
    dw->tv.active = dw->dv.active = active;
    dw->tv.highlight = dw->dv.highlight = g_browser->highlight;
    AuCanvas *c = au_canvas_new(dw->surf);
    int W = a.all.right, H = a.all.bottom;
    AuFontFace *small = au_fontset_match(g_fonts, AU_CHROME_SMALL_FAMILIES, 400, false);
    if (a.has_tool) {
        int th = a.tool.bottom;
        au_fill_rect(c, (AuRect){0, 0, (float)W, (float)th}, AU_RGB(221, 221, 221));
        au_fill_rect(c, (AuRect){0, (float)(th - 1), (float)W, 1}, AU_RGB(153, 153, 153));
        if (has_filter(dw)) {
            if (small)
                au_draw_text(c, small, 10, 8, 16, "Filter:", 7, AU_RGB(40, 40, 40));
            AuRect f = {(float)a.field.left, (float)a.field.top, (float)(a.field.right - a.field.left),
                        (float)(a.field.bottom - a.field.top)};
            bool focus = dw->filter_focus && active;
            au_fill_rect(c, f, focus ? AU_RGB(0, 0, 0) : AU_RGB(119, 119, 119));
            au_fill_rect(c, (AuRect){f.x + 1, f.y + 1, f.w - 2, f.h - 2}, AU_RGB(255, 255, 255));
            if (dw->tv.font) {
                float x = draw_tail(c, dw, f.x + 4, f.y + 12, f.w - 10, au_buf_cstr(&dw->filter), dw->filter.len,
                                    AU_RGB(0, 0, 0));
                if (focus)
                    au_fill_rect(c, (AuRect){x + 1, f.y + 3, 1, f.h - 6}, AU_RGB(0, 0, 0));
            }
        } else if (small) {
            /* Report a Problem: what the text is, and what to do with it. */
            static const char l1[] = "This is all that Copy and Save take. It has no cookies, passwords or history. You can "
                                     "edit it after pasting.";
            static const char l2[] = "Paste it into a new issue at github.com/matjam/aura/issues";
            char buf[200];
            au_chrome_ellipsize(small, 10, l1, (float)(W - 16), buf, sizeof buf);
            au_draw_text(c, small, 10, 8, 14, buf, strlen(buf), AU_RGB(0, 0, 0));
            au_draw_text(c, small, 10, 8, 30, l2, sizeof l2 - 1, AU_RGB(40, 40, 40));
        }
    }
    if (a.has_head) {
        au_fill_rect(c, (AuRect){0, (float)a.head.top, (float)W, kHeadH}, AU_RGB(238, 238, 238));
        au_fill_rect(c, (AuRect){0, (float)(a.head.bottom - 1), (float)W, 1}, AU_RGB(153, 153, 153));
        if (dw->tv.font)
            au_draw_text_list(c, dw->tv.font, dw->tv.size, (float)dw->tv.gutter, (float)(a.head.top + 11), dw->header,
                              strlen(dw->header), AU_RGB(0, 0, 0));
    }
    au_textview_paint(&dw->tv, c, 0, a.main.top);
    if (a.has_detail) {
        au_fill_rect(c, (AuRect){0, (float)(a.detail.top - 1), (float)W, 1}, AU_RGB(102, 102, 102));
        au_textview_paint(&dw->dv, c, 0, a.detail.top);
    }
    if (a.has_input) {
        au_fill_rect(c, (AuRect){0, (float)a.input.top, (float)W, kInputH}, AU_RGB(255, 255, 255));
        au_fill_rect(c, (AuRect){0, (float)a.input.top, (float)W, 1}, AU_RGB(153, 153, 153));
        if (dw->tv.font) {
            float base = (float)a.input.top + 13;
            float x = au_draw_text_list(c, dw->tv.font, dw->tv.size, 5, base, "> ", 2, AU_RGB(0, 51, 170));
            x = draw_tail(c, dw, 5 + x, base, (float)(W - kScrollBarWidth - 24), au_buf_cstr(&dw->input), dw->input.len,
                          AU_RGB(0, 0, 0));
            if (active && !dw->filter_focus)
                au_fill_rect(c, (AuRect){x + 1, (float)a.input.top + 3, 1, kInputH - 6}, AU_RGB(0, 0, 0));
        }
    }
    au_fill_rect(c, (AuRect){0, (float)(H - kBarH), (float)W, kBarH}, AU_RGB(221, 221, 221));
    au_fill_rect(c, (AuRect){0, (float)(H - kBarH), (float)W, 1}, AU_RGB(102, 102, 102));
    au_fill_rect(c, (AuRect){0, (float)(H - kBarH + 1), (float)W, 1}, AU_RGB(255, 255, 255));
    if (small && dw->footer[0]) {
        int x = 10;
        if (dw->nbuttons) {
            Rect br = (**dw->buttons[dw->nbuttons - 1]).contrlRect;
            x = br.right + 10;
        }
        char buf[320];
        au_chrome_ellipsize(small, 10, dw->footer, (float)(W - x - kGrowBox - 4), buf, sizeof buf);
        au_draw_text(c, small, 10, (float)x, (float)(H - kBarH + 20), buf, strlen(buf), AU_RGB(40, 40, 40));
    }
    au_canvas_free(c);

    SetPort((GrafPtr)dw->base.win);
    Rect src = a.all;
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

static void layout(DevWin *dw)
{
    Areas a;
    areas(dw, &a);
    au_textview_layout(&dw->tv, g_fonts, a.main.right - a.main.left, a.main.bottom - a.main.top);
    if (a.has_detail)
        au_textview_layout(&dw->dv, g_fonts, a.detail.right - a.detail.left, a.detail.bottom - a.detail.top);
    sync_scroll(dw);
}

/* ---- Content ---- */

static void set_title(DevWin *dw, AuPage *p)
{
    char t[200];
    const char *what = kind_titles[dw->base.kind - WIN_SOURCE];
    const char *url = p ? au_page_url(p) : "";
    snprintf(t, sizeof t, "%s%s%.150s", what, url[0] ? ": " : "", url);
    Str255 ps, cur;
    mac_pstr(t, ps);
    GetWTitle(dw->base.win, cur);
    if (cur[0] != ps[0] || memcmp(cur + 1, ps + 1, ps[0]) != 0) {
        SetWTitle(dw->base.win, ps);
        mac_menus_dirty();
    }
}

/* What the shell knows, for the reports: the machine's facts, and from
 * them the Performance report's memory and machine line. machine holds
 * the text h points to; conn the connections'. */
static void host_info(AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn, char *machine, size_t cap)
{
    mac_sysinfo(sys);
    memset(h, 0, sizeof *h);
    au_sysinfo_line(sys, machine, cap);
    h->machine = machine;
    h->build = au_build_identity();
    h->have_memory = true;
    h->partition_kb = sys->partition_kb;
    h->heap_free_kb = sys->heap_free_kb;
    h->heap_block_kb = sys->heap_block_kb;
    h->temp_free_kb = sys->temp_free_kb;
    mac_net_dev_info(h, conn);
}

/* The view's filter and options, from the tool band. */
static void network_view(DevWin *dw)
{
    snprintf(dw->nv.filter, sizeof dw->nv.filter, "%s", au_buf_cstr(&dw->filter));
    dw->nv.show = AU_NETSHOW_ALL;
    for (int i = 0; i < dw->ntoggles; i++)
        if (GetControlValue(dw->toggles[i]))
            dw->nv.show = i;
    AuBuf hd;
    au_buf_init(&hd);
    au_netlog_header_sorted(dw->nv.sort == AU_NETCOL_SEQ && !dw->nv.desc ? -1 : dw->nv.sort, dw->nv.desc, &hd);
    snprintf(dw->header, sizeof dw->header, "%s", au_buf_cstr(&hd));
    au_buf_free(&hd);
}

/* The record on the selected line, or NULL. */
static const AuNetRecord *network_selected(DevWin *dw, const AuNetLog *l)
{
    if (!l || dw->tv.selected < 0 || (uint32_t)dw->tv.selected >= dw->nv.n)
        return NULL;
    return au_netlog_at(l, dw->nv.rows[dw->tv.selected]);
}

static void network_footer(DevWin *dw, const AuNetLog *l, const AuNetRecord *sel)
{
    AuBuf f;
    au_buf_init(&f);
    if (sel) {
        /* The whole address of the selected request: the row may cut it. */
        au_buf_puts(&f, sel->url);
    } else if (l) {
        /* Short: the lower pane has the whole summary. */
        if (dw->nv.n != l->count)
            au_buf_printf(&f, "Showing %lu of ", (unsigned long)dw->nv.n);
        au_buf_printf(&f, "%lu requests, %lu blocked, %lu failed, ", (unsigned long)l->requests,
                      (unsigned long)l->blocked, (unsigned long)l->failed);
        au_put_size(&f, l->decoded_bytes);
        if (l->requests > l->finished)
            au_buf_printf(&f, ", %lu still loading", (unsigned long)(l->requests - l->finished));
    }
    snprintf(dw->footer, sizeof dw->footer, "%s", au_buf_cstr(&f));
    au_buf_free(&f);
}

static void network_detail(DevWin *dw, AuPage *p)
{
    const AuNetLog *l = p ? au_page_netlog(p) : NULL;
    const AuNetRecord *r = network_selected(dw, l);
    uint32_t seq = r ? r->seq : 0;
    uint32_t old_top = seq == dw->detail_seq ? dw->dv.top : 0;
    dw->detail_seq = seq;
    if (r) {
        au_dev_network_detail_lines(r, &dw->dlist);
    } else {
        /* Nothing selected: the summary and the problem requests. */
        AuBuf conn;
        au_buf_init(&conn);
        mac_net_connections_text(&conn);
        au_dev_network_summary_lines(l, au_buf_cstr(&conn), &dw->dlist);
        au_buf_free(&conn);
    }
    dw->dv.top = old_top;
    network_footer(dw, l, r);
}

/* Shows text (a report) in the main view. */
static void show_report(DevWin *dw, bool markdown)
{
    if (markdown)
        au_dev_markdown_lines(au_buf_cstr(&dw->text), &dw->list);
    else
        au_dev_report_lines(au_buf_cstr(&dw->text), &dw->list);
}

/* Rebuilds the window's text when what it shows changed (or force).
 * Returns true when it did. */
static bool refresh(DevWin *dw, bool force)
{
    AuPage *p = current_page();
    uint32_t now = au_platform_time_ms();
    bool page_changed = (const void *)p != dw->page;
    switch (dw->base.kind) {
    case WIN_SOURCE: {
        const char *d = NULL;
        size_t n = 0;
        bool ok = p && au_page_source(p, &d, &n);
        /* While a page loads its source is not there yet: keep the old
         * text until the new one is. */
        if (!force && !page_changed && (!ok || (d == dw->src_data && n == dw->src_len)))
            return false;
        if (!ok && !force && page_changed && p && au_page_loading(p))
            return false;
        if (dw->have_sv)
            au_sourceview_free(&dw->sv);
        au_sourceview_init(&dw->sv, ok ? d : "", ok ? n : 0);
        dw->have_sv = true;
        dw->src_data = ok ? d : NULL;
        dw->src_len = ok ? n : 0;
        AuTextSource src;
        au_sourceview_source(&dw->sv, &src);
        au_textview_set_source(&dw->tv, &src);
        set_title(dw, ok ? p : NULL);
        if (ok) {
            AuBuf f;
            au_buf_init(&f);
            au_put_count(&f, dw->sv.nnumbers);
            au_buf_puts(&f, " lines, ");
            au_put_size(&f, (uint32_t)n);
            snprintf(dw->footer, sizeof dw->footer, "%s", au_buf_cstr(&f));
            au_buf_free(&f);
        } else {
            snprintf(dw->footer, sizeof dw->footer, "This page has no source to show.");
        }
        break;
    }
    case WIN_CONSOLE: {
        uint32_t total = p ? au_page_console_total(p) : 0;
        if (!force && !page_changed && total == dw->seen)
            return false;
        bool end = au_textview_at_end(&dw->tv);
        uint32_t hidden = 0;
        if (p) {
            AuConsoleFilter f;
            f.show = 0;
            static const unsigned bits[kMaxToggles] = {AU_CONSOLE_SHOW_ERRORS, AU_CONSOLE_SHOW_WARNINGS,
                                                       AU_CONSOLE_SHOW_LOGS};
            for (int i = 0; i < dw->ntoggles; i++)
                if (GetControlValue(dw->toggles[i]))
                    f.show |= bits[i];
            f.text = au_buf_cstr(&dw->filter);
            hidden = au_dev_console_lines(p, &f, &dw->list);
        } else {
            au_textlist_clear(&dw->list);
        }
        dw->seen = total;
        set_title(dw, p);
        if (hidden)
            snprintf(dw->footer, sizeof dw->footer, "%lu entries, %lu hidden by the filter.",
                     (unsigned long)(p ? au_page_console_count(p) : 0), (unsigned long)hidden);
        else
            snprintf(dw->footer, sizeof dw->footer, "%lu entries. Return runs the expression in the page.",
                     (unsigned long)(p ? au_page_console_count(p) : 0));
        dw->page = p;
        layout(dw);
        if (end || page_changed)
            au_textview_scroll_to(&dw->tv, au_textview_max_top(&dw->tv));
        sync_scroll(dw);
        dw->last_ms = now;
        return true;
    }
    case WIN_NETWORK: {
        const AuNetLog *l = p ? au_page_netlog(p) : NULL;
        uint32_t gen = l ? l->generation : 0;
        if (!force && !page_changed && gen == dw->seen)
            return false;
        if (!force && !page_changed && now - dw->last_ms < 250)
            return false; /* a loading page changes the log many times a second */
        bool end = au_textview_at_end(&dw->tv) && dw->tv.selected < 0;
        if (page_changed)
            dw->detail_seq = 0;
        network_view(dw);
        au_netview_build(&dw->nv, l);
        layout(dw); /* the columns the rows may take */
        if (l)
            au_dev_network_lines(l, &dw->nv, dw->tv.cols, &dw->list);
        else
            au_textlist_clear(&dw->list);
        /* The selected request keeps its selection through a new order. */
        dw->tv.selected = -1;
        for (uint32_t i = 0; l && dw->detail_seq && i < dw->nv.n; i++)
            if (au_netlog_at(l, dw->nv.rows[i])->seq == dw->detail_seq)
                dw->tv.selected = (int32_t)i;
        dw->seen = gen;
        set_title(dw, p);
        dw->page = p;
        layout(dw);
        network_detail(dw, p);
        layout(dw);
        if (end)
            au_textview_scroll_to(&dw->tv, au_textview_max_top(&dw->tv));
        sync_scroll(dw);
        dw->last_ms = now;
        return true;
    }
    case WIN_PERF: {
        if (!force && !page_changed && (now - dw->last_ms < 1000 || au_textview_has_selection(&dw->tv)))
            return false;
        AuBuf conn;
        au_buf_init(&conn);
        au_buf_clear(&dw->text);
        if (p) {
            AuSysInfo sys;
            AuDevHostInfo h;
            char machine[256];
            host_info(&sys, &h, &conn, machine, sizeof machine);
            h.timing = g_timing.ran && g_timing_page == (const void *)p ? &g_timing : NULL;
            au_dev_perf_report(p, &h, now, &dw->text);
        } else {
            au_buf_puts(&dw->text, "Performance\n  No page is open. Open a browser window, then choose Develop > Performance.\n");
        }
        show_report(dw, false);
        au_buf_free(&conn);
        set_title(dw, p);
        snprintf(dw->footer, sizeof dw->footer, "Updates every second; waits while text is selected.");
        break;
    }
    case WIN_STATUS: {
        if (!force && (now - dw->last_ms < 2000 || au_textview_has_selection(&dw->tv)))
            return false;
        AuSysInfo sys;
        AuDevHostInfo h;
        AuBuf conn, shell;
        char machine[256];
        au_buf_init(&conn);
        au_buf_init(&shell);
        host_info(&sys, &h, &conn, machine, sizeof machine);
        /* Continues the report's Fonts section, then the network's. */
        au_report_kv(&shell, "Found at start in", "%lu ms   (%d bundled faces in %lu ms)", (unsigned long)mac_fonts_ms(),
                     mac_fonts_bundled(), (unsigned long)mac_fonts_bundled_ms());
        au_buf_putc(&shell, '\n');
        mac_net_status_text(&shell);
        au_buf_clear(&dw->text);
        au_sysinfo_report(&sys, g_browser, &h, au_buf_cstr(&shell), &g_bench, &dw->text);
        show_report(dw, false);
        au_buf_free(&conn);
        au_buf_free(&shell);
        set_title(dw, NULL);
        snprintf(dw->footer, sizeof dw->footer, "Updates every two seconds; waits while text is selected.");
        break;
    }
    case WIN_REPORT: {
        /* A snapshot: built when the window is shown and when its option
         * changes, never while it is read. */
        if (!force)
            return false;
        AuSysInfo sys;
        AuDevHostInfo h;
        AuBugReport br;
        AuBuf conn;
        char machine[256];
        au_buf_init(&conn);
        host_info(&sys, &h, &conn, machine, sizeof machine);
        memset(&br, 0, sizeof br);
        BrowserWin *bw = browserwin_front();
        br.browser = g_browser;
        br.window = bw ? browserwin_model(bw) : NULL;
        br.sys = &sys;
        br.host = &h;
        br.other_tabs = dw->ntoggles && GetControlValue(dw->toggles[0]);
        au_buf_clear(&dw->text);
        au_bugreport_build(&br, &dw->text);
        show_report(dw, true);
        au_buf_free(&conn);
        set_title(dw, NULL);
        AuBuf f;
        au_buf_init(&f);
        au_put_count(&f, (unsigned long)dw->text.len);
        snprintf(dw->footer, sizeof dw->footer, "%s characters. Nothing is sent.", au_buf_cstr(&f));
        au_buf_free(&f);
        dw->tv.top = 0;
        au_textview_clear_selection(&dw->tv);
        break;
    }
    default: return false;
    }
    dw->page = p;
    dw->last_ms = now;
    layout(dw);
    return true;
}

/* The whole text of the window, for Copy All and Copy Report. */
static void all_text(DevWin *dw, AuBuf *out)
{
    AuPage *p = current_page();
    switch (dw->base.kind) {
    case WIN_NETWORK:
        if (p) {
            AuBuf conn;
            au_buf_init(&conn);
            mac_net_connections_text(&conn);
            au_netlog_dump(au_page_netlog(p), au_buf_cstr(&conn), true, out);
            au_buf_free(&conn);
        }
        break;
    case WIN_CONSOLE:
        if (p)
            au_dev_console_text(p, out);
        break;
    case WIN_PERF:
    case WIN_STATUS:
    case WIN_REPORT: au_buf_append(out, dw->text.data ? dw->text.data : "", dw->text.len); break;
    default: au_textview_all_text(&dw->tv, out); break;
    }
}

static void copy_all(DevWin *dw)
{
    AuBuf out;
    au_buf_init(&out);
    all_text(dw, &out);
    if (!out.len || !mac_put_scrap(out.data, out.len))
        SysBeep(1);
    au_buf_free(&out);
}

/* Save Report: the text with CR LF line ends, which editors on every
 * system read. */
static void save_report(DevWin *dw)
{
    AuBuf out;
    au_buf_init(&out);
    for (size_t i = 0; i < dw->text.len; i++) {
        if (dw->text.data[i] == '\n')
            au_buf_putc(&out, '\r');
        au_buf_putc(&out, dw->text.data[i]);
    }
    if (out.len)
        mac_save_text("Save the report as:", "Aura Problem Report.txt", out.data, out.len);
    au_buf_free(&out);
}

/* ---- Find ---- */

enum { FIND_OK = 1, FIND_CANCEL, FIND_TEXT, FIND_LABEL, FIND_CASE };

static void find_next(DevWin *dw, bool forward)
{
    if (!dw->find || !au_textview_find(&dw->tv, dw->find, forward)) {
        SysBeep(1);
        return;
    }
    if (dw->base.kind == WIN_NETWORK)
        network_detail(dw, current_page());
    layout(dw);
    dw_paint(dw);
}

void devwin_find(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    DialogPtr d = GetNewDialog(201, NULL, (WindowPtr)-1);
    if (!d)
        return;
    short type;
    Handle h;
    Rect r;
    Str255 p;
    mac_pstr(dw->find ? dw->find : "", p);
    GetDialogItem(d, FIND_TEXT, &type, &h, &r);
    SetDialogItemText(h, p);
    SelectDialogItemText(d, FIND_TEXT, 0, 32767);
    GetDialogItem(d, FIND_LABEL, &type, &h, &r);
    SetDialogItemText(h, (ConstStr255Param) "\pFind text in this window:");
    /* The text view's find ignores case: the check box is not offered. */
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
    if (text && *text) {
        au_free(dw->find);
        dw->find = text;
        find_next(dw, true);
    } else {
        au_free(text);
    }
}

void devwin_find_again(MacWin *mw, bool forward)
{
    find_next((DevWin *)mw, forward);
}

bool devwin_can_find_again(MacWin *mw)
{
    return ((DevWin *)mw)->find != NULL;
}

/* ---- Console input ---- */

static void console_run(DevWin *dw)
{
    AuPage *p = current_page();
    if (!dw->input.len || !p) {
        if (!p)
            SysBeep(1);
        return;
    }
    char *expr = au_strndup(dw->input.data, dw->input.len);
    if (dw->nhistory == kHistory) {
        au_free(dw->history[0]);
        memmove(dw->history, dw->history + 1, (kHistory - 1) * sizeof dw->history[0]);
        dw->nhistory--;
    }
    dw->history[dw->nhistory++] = au_strndup(expr, strlen(expr));
    dw->history_at = dw->nhistory;
    au_buf_clear(&dw->input);
    SetCursor(*GetCursor(watchCursor));
    au_dev_console_eval(p, expr);
    InitCursor();
    au_free(expr);
    refresh(dw, true);
    au_textview_scroll_to(&dw->tv, au_textview_max_top(&dw->tv));
    sync_scroll(dw);
}

static void console_recall(DevWin *dw, int dir)
{
    int at = dw->history_at + dir;
    if (at < 0 || at > dw->nhistory)
        return;
    dw->history_at = at;
    au_buf_clear(&dw->input);
    if (at < dw->nhistory)
        au_buf_puts(&dw->input, dw->history[at]);
}

/* Paste into the field that has the keyboard: one line. */
static void paste_into(AuBuf *field, size_t cap)
{
    Handle h = NewHandle(0);
    long off = 0;
    if (!h)
        return;
    long n = GetScrap(h, 'TEXT', &off);
    if (n > 0) {
        HLock(h);
        char *u = mac_roman_to_utf8(*h, (size_t)n);
        HUnlock(h);
        for (char *q = u; *q; q++)
            if (*q == '\r' || *q == '\n' || *q == '\t')
                *q = ' ';
        if (field->len + strlen(u) < cap)
            au_buf_puts(field, u);
        au_free(u);
    }
    DisposeHandle(h);
}

/* A key typed into a one-line field. Returns true when the text changed. */
static bool field_key(AuBuf *field, unsigned char ch, size_t cap)
{
    if (ch == 0x08) {
        /* Delete: the last character, whole. */
        size_t n = field->len;
        if (!n)
            return false;
        while (n && ((unsigned char)field->data[n - 1] & 0xC0) == 0x80)
            n--;
        field->len = n ? n - 1 : 0;
        return true;
    }
    if (ch == 0x1B) {
        if (!field->len)
            return false;
        au_buf_clear(field);
        return true;
    }
    if (ch >= 0x20 && ch != 0x7F && field->len < cap) {
        char mr = (char)ch;
        char *u = mac_roman_to_utf8(&mr, 1);
        au_buf_puts(field, u);
        au_free(u);
        return true;
    }
    return false;
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
static AuTextView *g_track_tv;

static pascal void dw_scroll_action(ControlHandle c, short part)
{
    DevWin *dw = g_track;
    AuTextView *tv = g_track_tv;
    switch (part) {
    case inUpButton: au_textview_key(tv, AU_KEY_UP); break;
    case inDownButton: au_textview_key(tv, AU_KEY_DOWN); break;
    case inPageUp: au_textview_key(tv, AU_KEY_PAGE_UP); break;
    case inPageDown: au_textview_key(tv, AU_KEY_PAGE_DOWN); break;
    default: return;
    }
    set_scroll(c, tv);
    dw_paint(dw);
}

/* The scroll bar keys act on rows even in a list: a list's arrow keys
 * move its selection, which the scroll bar must not. */
static void scroll_click(DevWin *dw, ControlHandle c, AuTextView *tv, short part, Point pt)
{
    bool rows = tv->select_rows;
    tv->select_rows = false;
    g_track = dw;
    g_track_tv = tv;
    if (part == inThumb) {
        TrackControl(c, pt, NULL);
        au_textview_scroll_to(tv, top_of_control(c, tv));
        dw_paint(dw);
    } else {
        static ControlActionUPP action;
        if (!action)
            action = NewControlActionUPP(dw_scroll_action);
        TrackControl(c, pt, action);
    }
    g_track = NULL;
    tv->select_rows = rows;
}

static void repaint(DevWin *dw)
{
    SetPort((GrafPtr)dw->base.win);
    dw_paint(dw);
}

/* The buttons: Copy All is the first in every window and Find the second
 * (Report a Problem has its own three). */
static void button(DevWin *dw, int which)
{
    AuPage *p = current_page();
    if (dw->base.kind == WIN_REPORT) {
        if (which == 0)
            copy_all(dw);
        else if (which == 1)
            save_report(dw);
        else
            mac_open_url(AU_BUGREPORT_ISSUES_URL, true); /* in Aura, in a new tab */
        return;
    }
    if (which == 0) {
        copy_all(dw);
        return;
    }
    if (which == 1) {
        devwin_find(&dw->base);
        return;
    }
    switch (dw->base.kind) {
    case WIN_CONSOLE:
        if (p) {
            au_page_console_clear(p);
            refresh(dw, true);
            repaint(dw);
        }
        break;
    case WIN_PERF: devwin_run_timing(); break;
    case WIN_STATUS:
        refresh(dw, true);
        repaint(dw);
        break;
    default: break;
    }
}

/* A check box or radio button of the tool band was clicked. */
static void toggle(DevWin *dw, int which)
{
    if (dw->base.kind == WIN_NETWORK) {
        for (int i = 0; i < dw->ntoggles; i++)
            SetControlValue(dw->toggles[i], i == which);
    } else {
        SetControlValue(dw->toggles[which], !GetControlValue(dw->toggles[which]));
    }
    refresh(dw, true);
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
            scroll_click(dw, c, &dw->tv, part, pt);
        else if (c == dw->dscroll)
            scroll_click(dw, c, &dw->dv, part, pt);
        else if (TrackControl(c, pt, NULL)) {
            for (int i = 0; i < dw->nbuttons; i++)
                if (dw->buttons[i] == c) {
                    button(dw, i);
                    return;
                }
            for (int i = 0; i < dw->ntoggles; i++)
                if (dw->toggles[i] == c) {
                    toggle(dw, i);
                    return;
                }
        }
        return;
    }
    Areas a;
    areas(dw, &a);
    /* The filter field takes the keyboard when clicked; a click in the
     * text gives it back to the console's input line. */
    if (has_filter(dw)) {
        bool in_field = PtInRect(pt, &a.field);
        if (in_field != dw->filter_focus && (in_field || dw->base.kind == WIN_CONSOLE)) {
            dw->filter_focus = in_field;
            dw_paint(dw);
        }
        if (in_field)
            return;
    }
    /* A column title sorts the requests by it. */
    if (a.has_head && PtInRect(pt, &a.head) && dw->tv.cell_w > 0) {
        au_netview_click_column(&dw->nv, au_netlog_column_at((int)((float)(pt.h - dw->tv.gutter) / dw->tv.cell_w)));
        refresh(dw, true);
        dw_paint(dw);
        return;
    }
    AuTextView *tv = NULL;
    Rect *area = NULL;
    if (PtInRect(pt, &a.main)) {
        tv = &dw->tv;
        area = &a.main;
    } else if (a.has_detail && PtInRect(pt, &a.detail)) {
        tv = &dw->dv;
        area = &a.detail;
    }
    if (!tv)
        return;
    /* Clicks close in time and place count up to three. */
    int dh = pt.h - dw->click_at.h, dv = pt.v - dw->click_at.v;
    if ((uint32_t)ev->when - dw->click_ticks <= (uint32_t)GetDblTime() && dh > -4 && dh < 4 && dv > -4 && dv < 4 &&
        dw->clicks < 3)
        dw->clicks++;
    else
        dw->clicks = 1;
    dw->click_ticks = (uint32_t)ev->when;
    dw->click_at = pt;
    /* One selection at a time between the two panes. */
    if (tv == &dw->tv && !dw->dv.select_rows)
        dw->dv.has_sel = false;
    else if (tv == &dw->dv && !dw->tv.select_rows)
        dw->tv.has_sel = false;
    int32_t was = dw->tv.selected;
    au_textview_mouse_down(tv, pt.h - area->left, pt.v - area->top, dw->clicks, (ev->modifiers & shiftKey) != 0);
    if (tv == &dw->tv && dw->base.kind == WIN_NETWORK && dw->tv.selected != was) {
        network_detail(dw, current_page());
        layout(dw);
    }
    dw_paint(dw);
    if (!tv->select_rows) {
        while (StillDown()) {
            Point m;
            GetMouse(&m);
            if (au_textview_mouse_drag(tv, m.h - area->left, m.v - area->top)) {
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
        au_textview_mouse_up(tv);
    }
}

static bool dw_key(MacWin *mw, EventRecord *ev)
{
    DevWin *dw = (DevWin *)mw;
    unsigned char ch = (unsigned char)(ev->message & charCodeMask);
    int key = ch == 0x1E   ? AU_KEY_UP
              : ch == 0x1F ? AU_KEY_DOWN
              : ch == 0x0B ? AU_KEY_PAGE_UP
              : ch == 0x0C ? AU_KEY_PAGE_DOWN
              : ch == 0x01 ? AU_KEY_HOME
              : ch == 0x04 ? AU_KEY_END
                           : 0;
    SetPort((GrafPtr)mw->win);
    /* The filter field: in the Network window it is the only place to
     * type, so it takes every character; in the Console, Tab moves
     * between it and the input line. */
    bool to_filter = has_filter(dw) && (dw->filter_focus || dw->base.kind == WIN_NETWORK);
    if (dw->base.kind == WIN_CONSOLE && ch == 0x09) {
        dw->filter_focus = !dw->filter_focus;
        dw_paint(dw);
        return true;
    }
    if (to_filter && !key && ch != 0x0D && ch != 0x03) {
        if (field_key(&dw->filter, ch, 60)) {
            dw->filter_focus = true;
            refresh(dw, true);
            dw_paint(dw);
            return true;
        }
        return ch == 0x08 || ch == 0x1B;
    }
    if (dw->base.kind == WIN_CONSOLE) {
        if (ch == 0x0D || ch == 0x03) {
            if (dw->filter_focus)
                dw->filter_focus = false; /* Return leaves the filter */
            else
                console_run(dw);
        } else if (key == AU_KEY_UP || key == AU_KEY_DOWN) {
            console_recall(dw, key == AU_KEY_UP ? -1 : 1);
        } else if (key) {
            au_textview_key(&dw->tv, key);
            sync_scroll(dw);
        } else if (!field_key(&dw->input, ch, 4000)) {
            return false;
        }
        dw_paint(dw);
        return true;
    }
    if (!key)
        return false;
    int32_t was = dw->tv.selected;
    au_textview_key(&dw->tv, key);
    if (dw->base.kind == WIN_NETWORK && dw->tv.selected != was) {
        network_detail(dw, current_page());
        layout(dw);
    }
    sync_scroll(dw);
    dw_paint(dw);
    return true;
}

static void dw_idle(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    if (refresh(dw, false))
        repaint(dw);
}

static void place(DevWin *dw)
{
    Areas a;
    areas(dw, &a);
    SetPort((GrafPtr)dw->base.win);
    short W = a.all.right;
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
    int x = 8;
    for (int i = 0; i < dw->nbuttons; i++) {
        Rect br = (**dw->buttons[i]).contrlRect;
        int w = br.right - br.left;
        MoveControl(dw->buttons[i], (short)x, (short)(a.all.bottom - kBarH + 6));
        x += w + 8;
    }
    /* The tool band's options: after the filter field, or (Report a
     * Problem) at the right of its second line. */
    x = has_filter(dw) ? kFieldX + kFieldW + 14 : 0;
    for (int i = 0; i < dw->ntoggles; i++) {
        Rect tr = (**dw->toggles[i]).contrlRect;
        int w = tr.right - tr.left;
        if (dw->base.kind == WIN_REPORT)
            MoveControl(dw->toggles[i], (short)(W - w - 8 > 330 ? W - w - 8 : 330), 20);
        else
            MoveControl(dw->toggles[i], (short)x, 4);
        x += w + 6;
    }
}

static void dw_grow(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    make_gworld(dw);
    place(dw);
    if (dw->base.kind == WIN_NETWORK)
        refresh(dw, true); /* the rows are cut to the new width */
    layout(dw);
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
    au_textview_free(&dw->tv);
    au_textview_free(&dw->dv);
    au_textlist_free(&dw->list);
    au_textlist_free(&dw->dlist);
    au_netview_free(&dw->nv);
    if (dw->have_sv)
        au_sourceview_free(&dw->sv);
    au_buf_free(&dw->input);
    au_buf_free(&dw->filter);
    au_buf_free(&dw->text);
    for (int i = 0; i < dw->nhistory; i++)
        au_free(dw->history[i]);
    au_free(dw->find);
    au_free(dw);
    mac_menus_dirty();
}

/* Edit menu: Copy the selection (of the pane that has one), Select All
 * in the main text, Paste into the field that has the keyboard. */
static bool dw_edit(MacWin *mw, int item)
{
    DevWin *dw = (DevWin *)mw;
    switch (item) {
    case 0: return true;
    case 4: {
        AuBuf t;
        au_buf_init(&t);
        AuTextView *tv = au_textview_has_selection(&dw->dv) && !dw->dv.select_rows ? &dw->dv : &dw->tv;
        if (au_textview_selection_text(tv, &t) && t.len)
            mac_put_scrap(t.data, t.len);
        else
            SysBeep(1);
        au_buf_free(&t);
        return true;
    }
    case 5:
        if (!has_filter(dw))
            return false;
        if (dw->filter_focus || dw->base.kind == WIN_NETWORK) {
            paste_into(&dw->filter, 60);
            refresh(dw, true);
        } else {
            paste_into(&dw->input, 4000);
        }
        repaint(dw);
        return true;
    case 7:
        if (dw->tv.select_rows)
            return false;
        au_textview_select_all(&dw->tv);
        repaint(dw);
        return true;
    default: return false;
    }
}

static void add_button(DevWin *dw, const char *title, int width)
{
    Rect r;
    SetRect(&r, 0, 0, (short)width, 20);
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
    SetRect(&r, 0, 0, (short)width, 16);
    Str255 p;
    mac_pstr(title, p);
    ControlHandle c = NewControl(dw->base.win, &r, p, true, on ? 1 : 0, 0, 1, (short)((radio ? radioButProc : checkBoxProc) | useWFont), 0);
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
        if (refresh(dw, true))
            dw_paint(dw);
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
    au_textview_init(&dw->tv);
    au_textview_init(&dw->dv);
    au_textlist_init(&dw->list);
    au_textlist_init(&dw->dlist);
    au_netview_init(&dw->nv);
    au_buf_init(&dw->input);
    au_buf_init(&dw->filter);
    au_buf_init(&dw->text);
    AuTextSource src;
    au_textlist_source(&dw->list, &src);
    au_textview_set_source(&dw->tv, &src);
    au_textlist_source(&dw->dlist, &src);
    au_textview_set_source(&dw->dv, &src);
    dw->tv.wrap = kind != WIN_NETWORK;
    dw->tv.numbers = kind == WIN_SOURCE;
    dw->tv.select_rows = kind == WIN_NETWORK;
    dw->dv.wrap = true;

    Rect wr;
    initial_place(kind, &wr);
    Str255 pt;
    mac_pstr(kind_titles[kind - WIN_SOURCE], pt);
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
    if (kind == WIN_NETWORK) {
        dw->dscroll = NewControl(win, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, scrollBarProc, 0);
        if (!dw->dscroll)
            mac_out_of_memory(256);
        /* From now on the network stack keeps request and response
         * headers for the log. */
        mac_net_trace_headers(true);
    }
    /* Copy All first, Find second, then the window's own. */
    if (kind == WIN_REPORT) {
        add_button(dw, "Copy Report", 104);
        add_button(dw, "Save Report...", 116);
        add_button(dw, "Open GitHub Issues", 150);
        add_toggle(dw, "Include other tabs' addresses", 190, false, false);
    } else {
        add_button(dw, "Copy All", 80);
        add_button(dw, "Find...", 66);
        if (kind == WIN_CONSOLE) {
            add_button(dw, "Clear", 64);
            add_toggle(dw, "Errors", 62, false, true);
            add_toggle(dw, "Warnings", 76, false, true);
            add_toggle(dw, "Logs", 52, false, true);
        } else if (kind == WIN_NETWORK) {
            add_toggle(dw, "All", 40, true, true);
            add_toggle(dw, "Problems", 76, true, false);
            add_toggle(dw, "Blocked", 68, true, false);
        } else if (kind == WIN_PERF) {
            add_button(dw, "Run Timing Test", 130);
        } else if (kind == WIN_STATUS) {
            add_button(dw, "Refresh", 76);
        }
    }
    g_dev[kind - WIN_SOURCE] = dw;
    make_gworld(dw);
    place(dw);
    refresh(dw, true);
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
