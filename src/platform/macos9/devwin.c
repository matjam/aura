/* The developer windows: Page Source, Console, Network and Performance.
 *
 * Each is a document window whose text is painted by the engine's text
 * view (src/browser/textview.c) into an offscreen GWorld, with a native
 * scroll bar and a bar of push buttons at the bottom, as the list
 * windows are. There is at most one window of each kind. All four show
 * the active tab of the frontmost browser window and follow it: every
 * idle pass looks the tab up again (a window never keeps a page pointer
 * to use later) and rebuilds its text when the page's change counter
 * moved.
 *
 *   Page Source  the main document's bytes with line numbers and colour
 *                (sourceview.c); Find, Copy, Select All
 *   Console      the page's console lines (devtools.c) and an input line:
 *                type an expression, Return evaluates it in the page;
 *                Up and Down recall earlier input
 *   Network      one row per request (netlog.c); a click shows the
 *                request's details and headers in the lower pane; the
 *                bar shows the totals and the open connections. Opening
 *                the window turns header recording on for later loads.
 *   Performance  the report (au_dev_perf_report), rebuilt once a second;
 *                Copy Report puts it on the clipboard */
#include "mac.h"

#include "browser/chrome.h"
#include "browser/devtools.h"
#include "browser/netlog.h"
#include "browser/sourceview.h"
#include "browser/textview.h"

#include <stdio.h>
#include <string.h>

enum { kBarH = 32, kHeadH = 15, kInputH = 19, kMaxButtons = 3, kHistory = 16 };

typedef struct DevWin {
    MacWin base;
    GWorldPtr gw;
    AuSurface *surf;
    ControlHandle vscroll, dscroll; /* the text's; the Network detail pane's */
    ControlHandle buttons[kMaxButtons];
    int nbuttons;
    AuTextView tv, dv; /* the main text; the Network detail pane */
    AuTextList list, dlist;
    AuSourceView sv;
    bool have_sv;
    /* What is shown, to notice changes. page is compared, never used. */
    const void *page;
    const char *src_data;
    size_t src_len;
    uint32_t seen;       /* the console's line count or the log's generation */
    uint32_t last_ms;    /* when the text was last rebuilt */
    uint32_t detail_seq; /* Network: the request in the detail pane; 0 for none */
    char footer[320];
    char header[160];
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

static DevWin *g_dev[4]; /* by kind - WIN_SOURCE */

static DevWin *dev_of(WinKind kind)
{
    return kind >= WIN_SOURCE && kind <= WIN_PERF ? g_dev[kind - WIN_SOURCE] : NULL;
}

/* The page the windows show: the active tab of the frontmost browser
 * window. */
static AuPage *current_page(void)
{
    BrowserWin *b = browserwin_front();
    AuTab *t = b ? au_bw_active_tab(browserwin_model(b)) : NULL;
    return t ? t->page : NULL;
}

/* ---- Geometry ---- */

typedef struct Areas {
    Rect all, head, main, detail, input, bar;
    bool has_head, has_detail, has_input;
} Areas;

static void areas(DevWin *dw, Areas *a)
{
    memset(a, 0, sizeof *a);
    GetWindowPortBounds(dw->base.win, &a->all);
    short W = a->all.right, H = a->all.bottom;
    SetRect(&a->bar, 0, (short)(H - kBarH), W, H);
    short top = 0, bottom = (short)(H - kBarH);
    if (dw->base.kind == WIN_NETWORK) {
        a->has_head = true;
        SetRect(&a->head, 0, 0, W, kHeadH);
        top = kHeadH;
        a->has_detail = true;
        short dh = (short)((bottom - top) * 2 / 5);
        SetRect(&a->detail, 0, (short)(bottom - dh), (short)(W - kScrollBarWidth + 1), bottom);
        bottom = (short)(bottom - dh - 1);
    }
    if (dw->base.kind == WIN_CONSOLE) {
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
    if (a.has_head) {
        au_fill_rect(c, (AuRect){0, 0, (float)W, kHeadH}, AU_RGB(221, 221, 221));
        au_fill_rect(c, (AuRect){0, kHeadH - 1, (float)W, 1}, AU_RGB(153, 153, 153));
        if (dw->tv.font)
            au_draw_text_list(c, dw->tv.font, dw->tv.size, (float)dw->tv.gutter, 11, dw->header,
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
            /* The end of long input stays in view. */
            const char *text = au_buf_cstr(&dw->input);
            size_t len = dw->input.len;
            float room = (float)(W - kScrollBarWidth - 24);
            while (len && au_text_width_list(dw->tv.font, dw->tv.size, text, len) > room) {
                size_t adv = 1;
                au_utf8_decode(text, len, &adv);
                text += adv;
                len -= adv;
            }
            x += au_draw_text_list(c, dw->tv.font, dw->tv.size, 5 + x, base, text, len, AU_RGB(0, 0, 0));
            if (active)
                au_fill_rect(c, (AuRect){5 + x + 1, (float)a.input.top + 3, 1, kInputH - 6}, AU_RGB(0, 0, 0));
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
    Rect g;
    SetRect(&g, (short)(W - kGrowBox), (short)(H - kGrowBox), (short)W, (short)H);
    RectRgn(cut, &g);
    DiffRgn(mask, cut, mask);
    ForeColor(blackColor);
    BackColor(whiteColor);
    CopyBits(&((GrafPtr)dw->gw)->portBits, &((GrafPtr)dw->base.win)->portBits, &src, &src, srcCopy | kDitherCopy,
             mask);
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

static void set_title(DevWin *dw, const char *what, AuPage *p)
{
    char t[200];
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

/* The machine in one line, for the report. */
static void machine_line(char *out, size_t cap)
{
    long sys = 0, cpu = 0, ram = 0, hz = 0;
    Gestalt('sysv', &sys);
    Gestalt('cput', &cpu);
    Gestalt('ram ', &ram);
    Gestalt('pclk', &hz);
    const char *name = cpu == 0x108 ? "PowerPC G3" : cpu == 0x10C ? "PowerPC G4" : cpu == 0x111 ? "PowerPC G4 (7450)"
                       : cpu == 0x104                                                           ? "PowerPC 604"
                       : cpu == 0x109                                                           ? "PowerPC 604e"
                                                                                                : "PowerPC";
    Rect sb = qd.screenBits.bounds;
    GDHandle gd = GetMainDevice();
    int depth = gd ? (**(**gd).gdPMap).pixelSize : 0;
    snprintf(out, cap, "Mac OS %lx.%lx.%lx, %s (type %lx) %ld MHz, %ld MB, screen %dx%d at %d bits", (sys >> 8) & 0xFF,
             (sys >> 4) & 0xF, sys & 0xF, name, cpu, hz / 1000000, ram / (1024 * 1024), sb.right - sb.left,
             sb.bottom - sb.top, depth);
}

static void network_detail(DevWin *dw, AuPage *p)
{
    const AuNetLog *l = p ? au_page_netlog(p) : NULL;
    const AuNetRecord *r = l && dw->tv.selected >= 0 ? au_netlog_at(l, (uint32_t)dw->tv.selected) : NULL;
    uint32_t seq = r ? r->seq : 0;
    uint32_t old_top = seq == dw->detail_seq ? dw->dv.top : 0;
    dw->detail_seq = seq;
    if (r)
        au_dev_network_detail_lines(r, &dw->dlist);
    else
        au_dev_network_blocked_lines(l, &dw->dlist); /* nothing selected: what was blocked */
    dw->dv.top = old_top;
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
        set_title(dw, "Source", ok ? p : NULL);
        if (ok) {
            AuBuf f;
            au_buf_init(&f);
            au_buf_printf(&f, "%lu lines, ", (unsigned long)dw->sv.nnumbers);
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
        if (p)
            au_dev_console_lines(p, &dw->list);
        else
            au_textlist_clear(&dw->list);
        dw->seen = total;
        set_title(dw, "Console", p);
        snprintf(dw->footer, sizeof dw->footer, "%lu lines. Type an expression and press Return to run it in the page.",
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
        if (l)
            au_dev_network_lines(l, &dw->list);
        else
            au_textlist_clear(&dw->list);
        if (page_changed)
            dw->tv.selected = -1;
        dw->seen = gen;
        set_title(dw, "Network", p);
        AuBuf f, conn;
        au_buf_init(&f);
        au_buf_init(&conn);
        mac_net_connections_text(&conn);
        if (l)
            au_netlog_footer(l, au_buf_cstr(&conn), &f);
        snprintf(dw->footer, sizeof dw->footer, "%s", au_buf_cstr(&f));
        au_buf_free(&f);
        au_buf_free(&conn);
        dw->page = p;
        layout(dw); /* drops a selection past the end */
        network_detail(dw, p);
        layout(dw);
        if (end)
            au_textview_scroll_to(&dw->tv, au_textview_max_top(&dw->tv));
        sync_scroll(dw);
        dw->last_ms = now;
        return true;
    }
    case WIN_PERF: {
        if (!force && !page_changed && now - dw->last_ms < 1000)
            return false;
        AuBuf out, conn;
        au_buf_init(&out);
        au_buf_init(&conn);
        if (p) {
            AuDevHostInfo h;
            char machine[200];
            memset(&h, 0, sizeof h);
            machine_line(machine, sizeof machine);
            h.machine = machine;
            h.have_memory = true;
            h.heap_free_kb = (uint32_t)(FreeMem() / 1024);
            h.heap_block_kb = (uint32_t)(MaxBlock() / 1024);
            h.temp_free_kb = (uint32_t)(TempFreeMem() / 1024);
            mac_net_dev_info(&h, &conn);
            au_dev_perf_report(p, &h, now, &out);
        } else {
            au_buf_puts(&out, "No page: open a browser window.");
        }
        au_dev_report_lines(au_buf_cstr(&out), &dw->list);
        au_buf_free(&out);
        au_buf_free(&conn);
        set_title(dw, "Performance", p);
        snprintf(dw->footer, sizeof dw->footer, "Updated every second.");
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
static void copy_all(DevWin *dw)
{
    AuBuf out;
    au_buf_init(&out);
    AuPage *p = current_page();
    if (dw->base.kind == WIN_NETWORK && p) {
        AuBuf conn;
        au_buf_init(&conn);
        mac_net_connections_text(&conn);
        au_netlog_dump(au_page_netlog(p), au_buf_cstr(&conn), true, &out);
        au_buf_free(&conn);
    } else if (dw->base.kind == WIN_CONSOLE && p) {
        au_dev_console_text(p, &out);
    } else {
        au_textview_all_text(&dw->tv, &out);
    }
    if (!out.len || !mac_put_scrap(out.data, out.len))
        SysBeep(1);
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

static void console_paste(DevWin *dw)
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
        if (dw->input.len + strlen(u) < 4000)
            au_buf_puts(&dw->input, u);
        au_free(u);
    }
    DisposeHandle(h);
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

static void button(DevWin *dw, int which)
{
    AuPage *p = current_page();
    switch (dw->base.kind) {
    case WIN_SOURCE:
        if (which == 0)
            devwin_find(&dw->base);
        else
            copy_all(dw);
        return;
    case WIN_CONSOLE:
        if (which == 0 && p) {
            au_page_console_clear(p);
            refresh(dw, true);
            dw_paint(dw);
        } else if (which == 1) {
            copy_all(dw);
        }
        return;
    default: copy_all(dw); return;
    }
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
        else if (TrackControl(c, pt, NULL))
            for (int i = 0; i < dw->nbuttons; i++)
                if (dw->buttons[i] == c)
                    button(dw, i);
        return;
    }
    Areas a;
    areas(dw, &a);
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
    if (dw->base.kind == WIN_CONSOLE) {
        if (ch == 0x0D || ch == 0x03) {
            console_run(dw);
        } else if (ch == 0x08) {
            /* Delete: the last character, whole. */
            size_t n = dw->input.len;
            while (n && ((unsigned char)dw->input.data[n - 1] & 0xC0) == 0x80)
                n--;
            dw->input.len = n ? n - 1 : 0;
        } else if (ch == 0x1B) {
            au_buf_clear(&dw->input);
        } else if (key == AU_KEY_UP || key == AU_KEY_DOWN) {
            console_recall(dw, key == AU_KEY_UP ? -1 : 1);
        } else if (key) {
            au_textview_key(&dw->tv, key);
            sync_scroll(dw);
        } else if (ch >= 0x20 && ch != 0x7F && dw->input.len < 4000) {
            char mr = (char)ch;
            char *u = mac_roman_to_utf8(&mr, 1);
            au_buf_puts(&dw->input, u);
            au_free(u);
        } else {
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
    if (refresh(dw, false)) {
        SetPort((GrafPtr)mw->win);
        dw_paint(dw);
    }
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
}

static void dw_grow(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
    make_gworld(dw);
    place(dw);
    layout(dw);
    Rect r;
    GetWindowPortBounds(mw->win, &r);
    InvalRect(&r);
}

static void dw_close(MacWin *mw)
{
    DevWin *dw = (DevWin *)mw;
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
    if (dw->have_sv)
        au_sourceview_free(&dw->sv);
    au_buf_free(&dw->input);
    for (int i = 0; i < dw->nhistory; i++)
        au_free(dw->history[i]);
    au_free(dw->find);
    au_free(dw);
    mac_menus_dirty();
}

/* Edit menu: Copy the selection (of the pane that has one), Select All
 * in the main text, Paste into the console's input line. */
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
        if (dw->base.kind != WIN_CONSOLE)
            return false;
        console_paste(dw);
        SetPort((GrafPtr)mw->win);
        dw_paint(dw);
        return true;
    case 7:
        if (dw->tv.select_rows)
            return false;
        au_textview_select_all(&dw->tv);
        SetPort((GrafPtr)mw->win);
        dw_paint(dw);
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

void devwin_show(WinKind kind)
{
    if (kind < WIN_SOURCE || kind > WIN_PERF)
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
    dw->base.close = dw_close;
    dw->base.edit = dw_edit;
    au_textview_init(&dw->tv);
    au_textview_init(&dw->dv);
    au_textlist_init(&dw->list);
    au_textlist_init(&dw->dlist);
    au_buf_init(&dw->input);
    AuTextSource src;
    au_textlist_source(&dw->list, &src);
    au_textview_set_source(&dw->tv, &src);
    au_textlist_source(&dw->dlist, &src);
    au_textview_set_source(&dw->dv, &src);
    dw->tv.wrap = kind != WIN_NETWORK;
    dw->tv.numbers = kind == WIN_SOURCE;
    dw->tv.select_rows = kind == WIN_NETWORK;
    dw->dv.wrap = true;

    Rect screen = qd.screenBits.bounds, wr;
    int w = screen.right - screen.left - 40, h = kind == WIN_NETWORK ? 400 : 340;
    if (w > 760)
        w = 760;
    int x = screen.right - w - 16 - (kind - WIN_SOURCE) * 6, y = 64 + (kind - WIN_SOURCE) * 22;
    if (x < 6)
        x = 6;
    if (y + h > screen.bottom - 30) /* clear of the Control Strip */
        h = screen.bottom - 30 - y;
    SetRect(&wr, (short)x, (short)y, (short)(x + w), (short)(y + h));
    const char *title = kind == WIN_SOURCE    ? "Source"
                        : kind == WIN_CONSOLE ? "Console"
                        : kind == WIN_NETWORK ? "Network"
                                              : "Performance";
    Str255 pt;
    mac_pstr(title, pt);
    WindowPtr win = NewCWindow(NULL, &wr, pt, false, kZoomDocProc, (WindowPtr)-1, true, (long)dw);
    if (!win)
        mac_out_of_memory(sizeof(WindowRecord));
    dw->base.win = win;
    SetPort((GrafPtr)win);
    Rect sr = {0, 0, 10, 10};
    dw->vscroll = NewControl(win, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, scrollBarProc, 0);
    if (!dw->vscroll)
        mac_out_of_memory(256);
    if (kind == WIN_NETWORK) {
        dw->dscroll = NewControl(win, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, scrollBarProc, 0);
        if (!dw->dscroll)
            mac_out_of_memory(256);
        AuBuf hd;
        au_buf_init(&hd);
        au_netlog_header(&hd);
        snprintf(dw->header, sizeof dw->header, "%s", au_buf_cstr(&hd));
        au_buf_free(&hd);
        /* From now on the network stack keeps request and response
         * headers for the log. */
        mac_net_trace_headers(true);
        add_button(dw, "Copy All", 80);
    } else if (kind == WIN_SOURCE) {
        add_button(dw, "Find...", 70);
        add_button(dw, "Copy All", 80);
    } else if (kind == WIN_CONSOLE) {
        add_button(dw, "Clear", 64);
        add_button(dw, "Copy All", 80);
    } else {
        add_button(dw, "Copy Report", 104);
    }
    g_dev[kind - WIN_SOURCE] = dw;
    make_gworld(dw);
    place(dw);
    refresh(dw, true);
    ShowWindow(win);
    SelectWindow(win);
    mac_menus_dirty();
}
