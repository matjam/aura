/* The Bookmarks, History, Downloads and Cookies windows.
 *
 * Each is a document window showing a list model (src/browser/lists.c):
 * the rows, the buttons, what each button does and when it is enabled
 * come from the model. This file holds the window: the list painted by
 * the engine (src/browser/listview.c, or the downloads painter in
 * downloads.c) into an offscreen GWorld, a native scroll bar, a bar of
 * push buttons at the bottom, the selection and scrolling, and the
 * dialogs the model asks for (a name, a cookie's details, the site rule
 * pop-up). There is at most one window of each kind; Show brings it
 * forward. The list refreshes when the model reports that its data
 * changed. */
#include "mac.h"

#include "browser/lists.h"

#include <stdio.h>
#include <string.h>

enum { kBarH = 40, kMaxButtons = 6 };
enum { kRuleMenuID = 182 }; /* clear of the folder menus (200 to 255) */

typedef struct ListWin {
    MacWin base;
    GWorldPtr gw;
    AuSurface *surf;
    ControlHandle vscroll;
    ControlHandle buttons[kMaxButtons];
    AuListAction actions[kMaxButtons];
    int nbuttons;
    AuListModel model;
    AuListView lv; /* the painted rows (not for downloads) */
    int dl_scroll; /* downloads: the scroll offset */
    uint32_t last_paint_ms;
    uint32_t last_click_ticks;
    int last_click_row;
} ListWin;

static ListWin *g_list[8]; /* by WinKind */

static AuListKind list_kind(WinKind kind)
{
    switch (kind) {
    case WIN_BOOKMARKS: return AU_LIST_BOOKMARKS;
    case WIN_HISTORY: return AU_LIST_HISTORY;
    case WIN_COOKIES: return AU_LIST_COOKIES;
    default: return AU_LIST_DOWNLOADS;
    }
}

static bool is_downloads(const ListWin *lw)
{
    return lw->base.kind == WIN_DOWNLOADS;
}

static void list_area(ListWin *lw, Rect *r)
{
    GetWindowPortBounds(lw->base.win, r);
    r->right -= kScrollBarWidth - 1;
    r->bottom -= kBarH;
}

static int list_w(ListWin *lw)
{
    Rect r;
    list_area(lw, &r);
    return r.right - r.left;
}

static int list_h(ListWin *lw)
{
    Rect r;
    list_area(lw, &r);
    return r.bottom - r.top;
}

static int content_h(ListWin *lw)
{
    if (is_downloads(lw))
        return g_browser->downloads.n * AU_DL_ROW_H;
    return au_listview_content_height(&lw->lv);
}

static int *scroll_of(ListWin *lw)
{
    return is_downloads(lw) ? &lw->dl_scroll : &lw->lv.scroll;
}

static void make_gworld(ListWin *lw)
{
    Rect r;
    GetWindowPortBounds(lw->base.win, &r);
    OffsetRect(&r, (short)-r.left, (short)-r.top);
    if (lw->surf) {
        au_surface_free(lw->surf);
        lw->surf = NULL;
    }
    if (lw->gw) {
        DisposeGWorld(lw->gw);
        lw->gw = NULL;
    }
    QDErr err = NewGWorld(&lw->gw, 32, &r, NULL, NULL, useTempMem);
    if (err != noErr || !lw->gw)
        err = NewGWorld(&lw->gw, 32, &r, NULL, NULL, 0);
    if (err != noErr || !lw->gw)
        mac_out_of_memory((size_t)r.right * (size_t)r.bottom * 4);
    PixMapHandle pm = GetGWorldPixMap(lw->gw);
    if (!LockPixels(pm))
        mac_out_of_memory((size_t)r.right * (size_t)r.bottom * 4);
    lw->surf = au_surface_wrap(GetPixBaseAddr(pm), r.right, r.bottom, (**pm).rowBytes & 0x3FFF);
    /* A new GWorld holds whatever was in memory; parts the list does not
     * paint (under the scroll bar) must not show that. */
    au_surface_clear(lw->surf, AU_RGB(255, 255, 255));
}

/* Paints the list (and the button bar when bar is set) into the GWorld
 * and copies it to the window. */
static void lw_paint(ListWin *lw, bool bar)
{
    if (!lw->surf)
        return;
    AuCanvas *c = au_canvas_new(lw->surf);
    int w = list_w(lw), h = list_h(lw);
    bool active = lw->base.win == FrontWindow() && g_in_front;
    if (is_downloads(lw)) {
        au_downloads_paint(&g_browser->downloads, c, g_fonts, w, h, lw->dl_scroll, lw->model.selected, active);
    } else {
        lw->lv.width = w;
        lw->lv.height = h;
        lw->lv.active = active;
        au_listview_paint(&lw->lv, c, g_fonts);
    }
    Rect all;
    GetWindowPortBounds(lw->base.win, &all);
    int W = all.right - all.left, H = all.bottom - all.top;
    if (bar) {
        au_fill_rect(c, (AuRect){0, (float)(H - kBarH), (float)W, kBarH}, AU_RGB(221, 221, 221));
        au_fill_rect(c, (AuRect){0, (float)(H - kBarH), (float)W, 1}, AU_RGB(102, 102, 102));
        au_fill_rect(c, (AuRect){0, (float)(H - kBarH + 1), (float)W, 1}, AU_RGB(255, 255, 255));
    }
    au_canvas_free(c);
    SetPort((GrafPtr)lw->base.win);
    Rect src;
    SetRect(&src, 0, 0, (short)w, (short)(bar ? H : h));
    if (bar)
        src.right = (short)W;
    RgnHandle mask = NewRgn(), cut = NewRgn();
    if (!mask || !cut)
        mac_out_of_memory(64);
    RectRgn(mask, &src);
    Rect sb = (**lw->vscroll).contrlRect;
    RectRgn(cut, &sb);
    DiffRgn(mask, cut, mask);
    for (int i = 0; i < lw->nbuttons; i++) {
        Rect br = (**lw->buttons[i]).contrlRect;
        RectRgn(cut, &br);
        DiffRgn(mask, cut, mask);
    }
    Rect g;
    SetRect(&g, (short)(W - kGrowBox), (short)(H - kGrowBox), (short)W, (short)H);
    RectRgn(cut, &g);
    DiffRgn(mask, cut, mask);
    ForeColor(blackColor);
    BackColor(whiteColor);
    CopyBits(&((GrafPtr)lw->gw)->portBits, &((GrafPtr)lw->base.win)->portBits, &src, &src, srcCopy | kDitherCopy, mask);
    DisposeRgn(mask);
    DisposeRgn(cut);
    lw->last_paint_ms = au_platform_time_ms();
}

static void update_scroll(ListWin *lw)
{
    int max = content_h(lw) - list_h(lw);
    if (max < 0)
        max = 0;
    if (max > 32767)
        max = 32767;
    int *s = scroll_of(lw);
    if (*s > max)
        *s = max;
    if (*s < 0)
        *s = 0;
    SetPort((GrafPtr)lw->base.win);
    SetControlMaximum(lw->vscroll, (short)max);
    SetControlValue(lw->vscroll, (short)*s);
}

static void enable_buttons(ListWin *lw)
{
    bool front = lw->base.win == FrontWindow() && g_in_front;
    for (int i = 0; i < lw->nbuttons; i++)
        HiliteControl(lw->buttons[i], front && au_list_can(&lw->model, lw->actions[i]) ? 0 : 255);
}

/* Brings the painted rows, the scroll bar and the buttons in step with
 * the model. */
static void sync(ListWin *lw)
{
    if (!is_downloads(lw)) {
        au_listview_from_list(&lw->lv, &lw->model);
        lw->lv.height = list_h(lw);
    }
    update_scroll(lw);
    enable_buttons(lw);
}

static void refresh(ListWin *lw)
{
    au_list_refresh(&lw->model, mac_now());
    sync(lw);
}

/* ---- The dialogs the model asks for ---- */

/* A modal dialog asking for a name; returns a UTF-8 string to free, or
 * NULL when cancelled. */
static char *ask_text(void *ctx, const char *prompt, const char *initial)
{
    (void)ctx;
    Str255 p;
    mac_pstr(prompt, p);
    ParamText(p, (ConstStringPtr) "\p", (ConstStringPtr) "\p", (ConstStringPtr) "\p");
    DialogPtr d = GetNewDialog(200, NULL, (WindowPtr)-1);
    if (!d)
        return NULL;
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, 3, &type, &h, &r);
    mac_pstr(initial, p);
    SetDialogItemText(h, p);
    SelectDialogItemText(d, 3, 0, 32767);
    SetDialogDefaultItem(d, 1);
    SetDialogCancelItem(d, 2);
    ShowWindow((WindowPtr)d);
    short item = 0;
    do {
        ModalDialog(NULL, &item);
    } while (item != 1 && item != 2);
    char *out = NULL;
    if (item == 1) {
        GetDialogItem(d, 3, &type, &h, &r);
        GetDialogItemText(h, p);
        out = mac_roman_to_utf8((const char *)p + 1, p[0]);
    }
    DisposeDialog(d);
    return out;
}

/* ALRT 135 with the text's lines: each cut to 100 characters, spread
 * over the four parameters (255 bytes each). */
static void show_text(void *ctx, const char *title, const char *text)
{
    (void)ctx;
    (void)title;
    Str255 part[4];
    int pi = 0;
    for (int k = 0; k < 4; k++)
        part[k][0] = 0;
    const char *s = text;
    while (*s && pi < 4) {
        const char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        char line[128];
        snprintf(line, sizeof line, "%.*s%s", (int)(n > 100 ? 100 : n), s, n > 100 ? "..." : "");
        Str255 pl;
        mac_pstr(line, pl);
        if (part[pi][0] + pl[0] + 1 > 255)
            pi++;
        if (pi < 4) {
            memcpy(part[pi] + 1 + part[pi][0], pl + 1, pl[0]);
            part[pi][0] = (unsigned char)(part[pi][0] + pl[0]);
            part[pi][1 + part[pi][0]] = '\r';
            part[pi][0]++;
        }
        s += n;
        if (*s == '\n')
            s++;
    }
    ParamText(part[0], part[1], part[2], part[3]);
    NoteAlert(135, NULL);
}

/* A pop-up of the items under the action's button. */
static int choose(void *ctx, AuListAction action, const char *const *items, int n, int current)
{
    ListWin *lw = ctx;
    int b = -1;
    for (int i = 0; i < lw->nbuttons; i++)
        if (lw->actions[i] == action)
            b = i;
    if (b < 0)
        return -1;
    MenuHandle m = NewMenu(kRuleMenuID, (ConstStr255Param) "\pRule");
    if (!m)
        return -1;
    for (int i = 0; i < n; i++)
        mac_append_item(m, items[i]);
    if (current >= 0 && current < n)
        CheckItem(m, (short)(current + 1), true);
    InsertMenu(m, -1);
    Rect br = (**lw->buttons[b]).contrlRect;
    Point pt;
    pt.h = br.left;
    pt.v = br.top;
    SetPort((GrafPtr)lw->base.win);
    LocalToGlobal(&pt);
    long choice = PopUpMenuSelect(m, pt.v, pt.h, (short)(current + 1));
    DeleteMenu(kRuleMenuID);
    DisposeMenu(m);
    short item = (short)(choice & 0xFFFF);
    return (choice >> 16) && item >= 1 && item <= n ? item - 1 : -1;
}

static void open_url(void *ctx, const char *url, bool new_tab)
{
    (void)ctx;
    mac_open_url(url, new_tab);
}

static bool reveal_download(void *ctx, const AuDownload *d)
{
    (void)ctx;
    return mac_download_reveal(d);
}

static void cookie_rules_changed(void *ctx)
{
    (void)ctx;
    mac_net_rules_changed();
}

/* ---- Actions ---- */

static void run_action(ListWin *lw, AuListAction a)
{
    if (au_list_run(&lw->model, a, mac_now()) == AU_LIST_FAILED)
        SysBeep(1);
    sync(lw);
    lw_paint(lw, false);
}

static void open_row(ListWin *lw, int row)
{
    au_list_activate(&lw->model, row, mac_now());
    sync(lw);
    lw_paint(lw, false);
}

/* ---- Events ---- */

static void lw_update(MacWin *mw)
{
    ListWin *lw = (ListWin *)mw;
    SetPort((GrafPtr)mw->win);
    BeginUpdate(mw->win);
    lw_paint(lw, true);
    DrawControls(mw->win);
    Rect r, g;
    GetWindowPortBounds(mw->win, &r);
    SetRect(&g, (short)(r.right - kGrowBox), (short)(r.bottom - kGrowBox), r.right, r.bottom);
    ClipRect(&g);
    DrawGrowIcon(mw->win);
    ClipRect(&r);
    EndUpdate(mw->win);
}

static void lw_activate(MacWin *mw, bool on)
{
    ListWin *lw = (ListWin *)mw;
    SetPort((GrafPtr)mw->win);
    HiliteControl(lw->vscroll, on ? 0 : 255);
    enable_buttons(lw);
    lw_paint(lw, false);
    Rect r, g;
    GetWindowPortBounds(mw->win, &r);
    SetRect(&g, (short)(r.right - kGrowBox), (short)(r.bottom - kGrowBox), r.right, r.bottom);
    ClipRect(&g);
    DrawGrowIcon(mw->win);
    ClipRect(&r);
}

static ListWin *g_track;

static void scroll_to(ListWin *lw, int y)
{
    int max = GetControlMaximum(lw->vscroll);
    if (y > max)
        y = max;
    if (y < 0)
        y = 0;
    *scroll_of(lw) = y;
    SetControlValue(lw->vscroll, (short)y);
    lw_paint(lw, false);
}

static pascal void lw_scroll_action(ControlHandle c, short part)
{
    ListWin *lw = g_track;
    int page = list_h(lw) - 20;
    int *s = scroll_of(lw);
    switch (part) {
    case inUpButton: scroll_to(lw, *s - 18); break;
    case inDownButton: scroll_to(lw, *s + 18); break;
    case inPageUp: scroll_to(lw, *s - page); break;
    case inPageDown: scroll_to(lw, *s + page); break;
    }
    (void)c;
}

static void select_row(ListWin *lw, int row)
{
    au_list_select(&lw->model, row);
    if (is_downloads(lw)) {
        int top = row * AU_DL_ROW_H, h = list_h(lw);
        if (row >= 0 && top < lw->dl_scroll)
            lw->dl_scroll = top;
        else if (row >= 0 && top + AU_DL_ROW_H > lw->dl_scroll + h)
            lw->dl_scroll = top + AU_DL_ROW_H - h;
    } else {
        lw->lv.selected = lw->model.selected;
        au_listview_reveal(&lw->lv, lw->lv.selected);
    }
    update_scroll(lw);
    enable_buttons(lw);
    lw_paint(lw, false);
}

static void lw_click(MacWin *mw, EventRecord *ev)
{
    ListWin *lw = (ListWin *)mw;
    SetPort((GrafPtr)mw->win);
    Point pt = ev->where;
    GlobalToLocal(&pt);
    ControlHandle c;
    short part = FindControl(pt, mw->win, &c);
    if (c && part) {
        if (c == lw->vscroll) {
            g_track = lw;
            if (part == inThumb) {
                TrackControl(c, pt, NULL);
                scroll_to(lw, GetControlValue(c));
            } else {
                static ControlActionUPP action;
                if (!action)
                    action = NewControlActionUPP(lw_scroll_action);
                TrackControl(c, pt, action);
            }
            g_track = NULL;
            return;
        }
        if (TrackControl(c, pt, NULL)) {
            for (int i = 0; i < lw->nbuttons; i++)
                if (lw->buttons[i] == c)
                    run_action(lw, lw->actions[i]);
        }
        return;
    }
    Rect la;
    list_area(lw, &la);
    if (!PtInRect(pt, &la))
        return;
    int row;
    bool disclosure = false;
    if (is_downloads(lw))
        row = au_downloads_hit(&g_browser->downloads, pt.v, lw->dl_scroll);
    else
        row = au_listview_hit(&lw->lv, pt.h, pt.v, &disclosure);
    if (disclosure) {
        au_list_toggle(&lw->model, row, mac_now());
        sync(lw);
        lw_paint(lw, false);
        return;
    }
    bool dbl = row >= 0 && row == lw->last_click_row && (uint32_t)ev->when - lw->last_click_ticks <= (uint32_t)GetDblTime();
    lw->last_click_row = row;
    lw->last_click_ticks = (uint32_t)ev->when;
    select_row(lw, row);
    if (dbl) {
        lw->last_click_row = -1;
        open_row(lw, row);
    }
}

static bool lw_key(MacWin *mw, EventRecord *ev)
{
    ListWin *lw = (ListWin *)mw;
    unsigned char ch = (unsigned char)(ev->message & charCodeMask);
    int n = lw->model.n;
    int sel = lw->model.selected;
    switch (ch) {
    case 0x1E: if (n) select_row(lw, sel > 0 ? sel - 1 : 0); return true;
    case 0x1F: if (n) select_row(lw, sel < n - 1 ? sel + 1 : n - 1); return true;
    case 0x0D:
    case 0x03:
        if (sel >= 0)
            open_row(lw, sel);
        return true;
    case 0x08:
    case 0x7F: run_action(lw, AU_LIST_DELETE); return true;
    }
    return false;
}

static void lw_idle(MacWin *mw)
{
    ListWin *lw = (ListWin *)mw;
    if (!au_list_stale(&lw->model))
        return;
    /* Downloads change many times a second; repaint a few times. */
    if (is_downloads(lw) && au_platform_time_ms() - lw->last_paint_ms < 250)
        return;
    refresh(lw);
    lw_paint(lw, false);
}

static void place(ListWin *lw)
{
    Rect r;
    GetWindowPortBounds(lw->base.win, &r);
    SetPort((GrafPtr)lw->base.win);
    HideControl(lw->vscroll);
    MoveControl(lw->vscroll, (short)(r.right - kScrollBarWidth + 1), -1);
    SizeControl(lw->vscroll, kScrollBarWidth, (short)(r.bottom - kBarH + 2));
    ShowControl(lw->vscroll);
    int x = 10;
    for (int i = 0; i < lw->nbuttons; i++) {
        Rect br = (**lw->buttons[i]).contrlRect;
        int w = br.right - br.left;
        MoveControl(lw->buttons[i], (short)x, (short)(r.bottom - kBarH + 10));
        x += w + 8;
    }
}

static void lw_grow(MacWin *mw)
{
    ListWin *lw = (ListWin *)mw;
    make_gworld(lw);
    place(lw);
    sync(lw);
    Rect r;
    GetWindowPortBounds(mw->win, &r);
    InvalRect(&r);
}

static void lw_close(MacWin *mw)
{
    ListWin *lw = (ListWin *)mw;
    g_list[mw->kind] = NULL;
    DisposeWindow(mw->win);
    if (lw->surf)
        au_surface_free(lw->surf);
    if (lw->gw)
        DisposeGWorld(lw->gw);
    au_listview_free(&lw->lv);
    au_list_free(&lw->model);
    au_free(lw);
    mac_menus_dirty();
}

void listwin_cookies_changed(void)
{
    if (g_list[WIN_COOKIES])
        au_list_touch(&g_list[WIN_COOKIES]->model);
}

static void add_button(ListWin *lw, const AuListButton *b)
{
    if (lw->nbuttons >= kMaxButtons)
        return;
    Rect r;
    SetRect(&r, 0, 0, (short)b->width, 20);
    Str255 p;
    mac_pstr(b->label, p);
    ControlHandle c = NewControl(lw->base.win, &r, p, true, 0, 0, 1, pushButProc, 0);
    if (!c)
        mac_out_of_memory(256);
    lw->actions[lw->nbuttons] = b->action;
    lw->buttons[lw->nbuttons++] = c;
}

void listwin_show(WinKind kind)
{
    if (g_list[kind]) {
        SelectWindow(g_list[kind]->base.win);
        return;
    }
    ListWin *lw = au_calloc(sizeof *lw);
    lw->base.kind = kind;
    lw->base.update = lw_update;
    lw->base.activate = lw_activate;
    lw->base.click = lw_click;
    lw->base.key = lw_key;
    lw->base.idle = lw_idle;
    lw->base.grow = lw_grow;
    lw->base.close = lw_close;
    lw->last_click_row = -1;
    au_listview_init(&lw->lv);
    lw->lv.icons = g_browser->favicons;
    AuListSources src;
    au_list_sources(&src, g_browser, mac_net_cookie_jar(), mac_net_storage());
    AuListHost host = {lw, open_url, ask_text, show_text, choose, reveal_download, cookie_rules_changed};
    au_list_init(&lw->model, list_kind(kind), &src, &host, mac_now());
    Rect screen = qd.screenBits.bounds, wr;
    int w = kind == WIN_DOWNLOADS || kind == WIN_COOKIES ? 520 : 460, h = 400;
    int x = screen.right - w - 30, y = 70 + (kind - WIN_BOOKMARKS) * 24;
    if (x < 10)
        x = 10;
    if (y + h > screen.bottom - 26) /* clear of the Control Strip */
        h = screen.bottom - 26 - y;
    SetRect(&wr, (short)x, (short)y, (short)(x + w), (short)(y + h));
    Str255 pt;
    mac_pstr(au_list_title(lw->model.kind), pt);
    WindowPtr win = NewCWindow(NULL, &wr, pt, false, kZoomDocProc, (WindowPtr)-1, true, (long)lw);
    if (!win)
        mac_out_of_memory(sizeof(WindowRecord));
    lw->base.win = win;
    SetPort((GrafPtr)win);
    Rect sr = {0, 0, 10, 10};
    lw->vscroll = NewControl(win, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, scrollBarProc, 0);
    if (!lw->vscroll)
        mac_out_of_memory(256);
    const AuListButton *buttons;
    int nb = au_list_buttons(lw->model.kind, &buttons);
    for (int i = 0; i < nb; i++)
        add_button(lw, &buttons[i]);
    g_list[kind] = lw;
    make_gworld(lw);
    place(lw);
    sync(lw);
    ShowWindow(win);
    SelectWindow(win);
    mac_menus_dirty();
}
