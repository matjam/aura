/* The Bookmarks, History and Downloads windows.
 *
 * Each is a document window with a list painted by the engine
 * (src/browser/listview.c, or the downloads painter in downloads.c) into
 * an offscreen GWorld, a native scroll bar, and a bar of push buttons at
 * the bottom. There is at most one window of each kind; Show brings it
 * forward. The lists rebuild when the data's change counter moves. */
#include "mac.h"

#include <stdio.h>
#include <string.h>

enum { kBarH = 40, kMaxButtons = 6 };

typedef struct ListWin {
    MacWin base;
    GWorldPtr gw;
    AuSurface *surf;
    ControlHandle vscroll;
    ControlHandle buttons[kMaxButtons];
    int nbuttons;
    AuListView lv;  /* bookmarks and history */
    int dl_sel;     /* downloads: selected row */
    int dl_scroll;
    uint32_t seen_gen;
    uint32_t last_paint_ms;
    uint32_t last_click_ticks;
    int last_click_row;
} ListWin;

static ListWin *g_list[6]; /* by WinKind */

/* Button numbers per window. */
enum { B_OPEN, B_NEW_FOLDER, B_RENAME, B_DELETE };       /* bookmarks */
enum { H_OPEN, H_DELETE, H_CLEAR };                      /* history */
enum { D_STOP, D_RESUME, D_REMOVE, D_CLEAR, D_REVEAL };  /* downloads */

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
    if (lw->base.kind == WIN_DOWNLOADS)
        return g_browser->downloads.n * AU_DL_ROW_H;
    return au_listview_content_height(&lw->lv);
}

static int *scroll_of(ListWin *lw)
{
    return lw->base.kind == WIN_DOWNLOADS ? &lw->dl_scroll : &lw->lv.scroll;
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
    if (lw->base.kind == WIN_DOWNLOADS) {
        au_downloads_paint(&g_browser->downloads, c, g_fonts, w, h, lw->dl_scroll, lw->dl_sel, active);
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
    CopyBits(&((GrafPtr)lw->gw)->portBits, &((GrafPtr)lw->base.win)->portBits, &src, &src, srcCopy, mask);
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

static int selected(ListWin *lw)
{
    return lw->base.kind == WIN_DOWNLOADS ? lw->dl_sel : lw->lv.selected;
}

static void enable_buttons(ListWin *lw)
{
    int sel = selected(lw);
    for (int i = 0; i < lw->nbuttons; i++) {
        bool on = true;
        switch (lw->base.kind) {
        case WIN_BOOKMARKS:
            if (i == B_OPEN || i == B_RENAME || i == B_DELETE)
                on = sel >= 0;
            break;
        case WIN_HISTORY:
            if (i == H_OPEN || i == H_DELETE)
                on = sel >= 0 && lw->lv.rows[sel].kind == AU_ROW_LINK;
            if (i == H_CLEAR)
                on = g_browser->history.n > 0;
            break;
        case WIN_DOWNLOADS: {
            AuDownloads *dl = &g_browser->downloads;
            AuDownload *d = sel >= 0 && sel < dl->n ? dl->items[sel] : NULL;
            if (i == D_STOP)
                on = d && (d->state == AU_DL_ACTIVE || d->state == AU_DL_QUEUED);
            else if (i == D_RESUME)
                on = d && (d->state == AU_DL_PAUSED || d->state == AU_DL_FAILED);
            else if (i == D_REMOVE)
                on = d != NULL;
            else if (i == D_REVEAL)
                on = false; /* simulated downloads write no file */
            break;
        }
        default: break;
        }
        if (!(lw->base.win == FrontWindow() && g_in_front))
            on = false;
        HiliteControl(lw->buttons[i], on ? 0 : 255);
    }
}

static void rebuild(ListWin *lw)
{
    switch (lw->base.kind) {
    case WIN_BOOKMARKS:
        au_listview_from_bookmarks(&lw->lv, &g_browser->bookmarks);
        lw->seen_gen = g_browser->bookmarks.generation;
        break;
    case WIN_HISTORY:
        au_listview_from_history(&lw->lv, &g_browser->history, mac_now());
        lw->seen_gen = g_browser->history.generation;
        break;
    case WIN_DOWNLOADS:
        if (lw->dl_sel >= g_browser->downloads.n)
            lw->dl_sel = g_browser->downloads.n - 1;
        lw->seen_gen = g_browser->downloads.generation;
        break;
    default: break;
    }
    lw->lv.height = list_h(lw);
    update_scroll(lw);
    enable_buttons(lw);
}

/* ---- Actions ---- */

static void open_row(ListWin *lw, int row)
{
    if (lw->base.kind == WIN_BOOKMARKS) {
        AuBookmark *b = au_bookmark_find_id(&g_browser->bookmarks, lw->lv.rows[row].id);
        if (!b)
            return;
        if (b->folder) {
            b->expanded = !b->expanded;
            rebuild(lw);
            lw_paint(lw, false);
        } else {
            mac_open_url(b->url, false);
        }
    } else if (lw->base.kind == WIN_HISTORY) {
        const AuListRow *r = &lw->lv.rows[row];
        if (r->kind == AU_ROW_LINK && r->id < g_browser->history.n) {
            char *u = au_strndup(g_browser->history.e[r->id].url, strlen(g_browser->history.e[r->id].url));
            mac_open_url(u, false);
            au_free(u);
        }
    }
}

/* A modal dialog asking for a name; returns a UTF-8 string to free, or
 * NULL when cancelled. */
static char *ask_name(const char *prompt, const char *initial)
{
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

static void bookmark_button(ListWin *lw, int which)
{
    AuBookmarks *bm = &g_browser->bookmarks;
    int sel = lw->lv.selected;
    AuBookmark *b = sel >= 0 ? au_bookmark_find_id(bm, lw->lv.rows[sel].id) : NULL;
    switch (which) {
    case B_OPEN:
        if (sel >= 0)
            open_row(lw, sel);
        break;
    case B_NEW_FOLDER: {
        AuBookmark *parent = b ? (b->folder ? b : b->parent) : NULL;
        char *name = ask_name("Name of the new folder:", "New Folder");
        if (name) {
            if (parent)
                parent->expanded = true;
            AuBookmark *f = au_bookmark_add_folder(bm, parent, NULL, *name ? name : "New Folder", mac_now());
            au_free(name);
            au_listview_from_bookmarks(&lw->lv, bm);
            for (int i = 0; i < lw->lv.n; i++)
                if (lw->lv.rows[i].id == f->id)
                    lw->lv.selected = i;
            au_browser_bookmarks_changed(g_browser);
        }
        break;
    }
    case B_RENAME:
        if (b) {
            char *name = ask_name("New name:", b->title);
            if (name && *name) {
                au_bookmark_rename(bm, b, name);
                au_browser_bookmarks_changed(g_browser);
            }
            au_free(name);
        }
        break;
    case B_DELETE:
        if (b) {
            au_bookmark_delete(bm, b);
            au_browser_bookmarks_changed(g_browser);
        }
        break;
    }
}

static void history_button(ListWin *lw, int which)
{
    int sel = lw->lv.selected;
    switch (which) {
    case H_OPEN:
        if (sel >= 0)
            open_row(lw, sel);
        break;
    case H_DELETE:
        if (sel >= 0 && lw->lv.rows[sel].kind == AU_ROW_LINK) {
            au_history_remove(&g_browser->history, lw->lv.rows[sel].id);
            au_browser_history_changed(g_browser);
        }
        break;
    case H_CLEAR:
        au_history_clear(&g_browser->history);
        lw->lv.selected = -1;
        au_browser_history_changed(g_browser);
        break;
    }
}

static void download_button(ListWin *lw, int which)
{
    AuDownloads *dl = &g_browser->downloads;
    AuDownload *d = lw->dl_sel >= 0 && lw->dl_sel < dl->n ? dl->items[lw->dl_sel] : NULL;
    switch (which) {
    case D_STOP:
        if (d)
            au_download_pause(dl, d);
        break;
    case D_RESUME:
        if (d)
            au_download_resume(dl, d);
        break;
    case D_REMOVE:
        if (d)
            au_download_remove(dl, d);
        break;
    case D_CLEAR: au_downloads_clear_completed(dl); break;
    default: break;
    }
}

static void button(ListWin *lw, int which)
{
    switch (lw->base.kind) {
    case WIN_BOOKMARKS: bookmark_button(lw, which); break;
    case WIN_HISTORY: history_button(lw, which); break;
    case WIN_DOWNLOADS: download_button(lw, which); break;
    default: break;
    }
    rebuild(lw);
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
    if (lw->base.kind == WIN_DOWNLOADS) {
        lw->dl_sel = row;
        int top = row * AU_DL_ROW_H, h = list_h(lw);
        if (row >= 0 && top < lw->dl_scroll)
            lw->dl_scroll = top;
        else if (row >= 0 && top + AU_DL_ROW_H > lw->dl_scroll + h)
            lw->dl_scroll = top + AU_DL_ROW_H - h;
    } else {
        lw->lv.selected = row;
        au_listview_reveal(&lw->lv, row);
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
                    button(lw, i);
        }
        return;
    }
    Rect la;
    list_area(lw, &la);
    if (!PtInRect(pt, &la))
        return;
    int row;
    bool disclosure = false;
    if (lw->base.kind == WIN_DOWNLOADS)
        row = au_downloads_hit(&g_browser->downloads, pt.v, lw->dl_scroll);
    else
        row = au_listview_hit(&lw->lv, pt.h, pt.v, &disclosure);
    if (disclosure) {
        open_row(lw, row); /* toggles the folder */
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
    int n = lw->base.kind == WIN_DOWNLOADS ? g_browser->downloads.n : lw->lv.n;
    int sel = selected(lw);
    switch (ch) {
    case 0x1E: if (n) select_row(lw, sel > 0 ? sel - 1 : 0); return true;
    case 0x1F: if (n) select_row(lw, sel < n - 1 ? sel + 1 : n - 1); return true;
    case 0x0D:
    case 0x03:
        if (sel >= 0)
            open_row(lw, sel);
        return true;
    case 0x08:
    case 0x7F:
        button(lw, lw->base.kind == WIN_BOOKMARKS ? B_DELETE : lw->base.kind == WIN_HISTORY ? H_DELETE : D_REMOVE);
        return true;
    }
    return false;
}

static void lw_idle(MacWin *mw)
{
    ListWin *lw = (ListWin *)mw;
    uint32_t gen = lw->base.kind == WIN_BOOKMARKS  ? g_browser->bookmarks.generation
                   : lw->base.kind == WIN_HISTORY ? g_browser->history.generation
                                                  : g_browser->downloads.generation;
    if (gen == lw->seen_gen)
        return;
    /* Downloads change many times a second; repaint a few times. */
    if (lw->base.kind == WIN_DOWNLOADS && au_platform_time_ms() - lw->last_paint_ms < 250)
        return;
    rebuild(lw);
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
    rebuild(lw);
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
    au_free(lw);
    mac_menus_dirty();
}

static void add_button(ListWin *lw, const char *title, int width)
{
    Rect r;
    SetRect(&r, 0, 0, (short)width, 20);
    Str255 p;
    mac_pstr(title, p);
    ControlHandle c = NewControl(lw->base.win, &r, p, true, 0, 0, 1, pushButProc, 0);
    if (!c)
        mac_out_of_memory(256);
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
    lw->dl_sel = -1;
    lw->last_click_row = -1;
    au_listview_init(&lw->lv);
    Rect screen = qd.screenBits.bounds, wr;
    int w = kind == WIN_DOWNLOADS ? 520 : 460, h = 400;
    int x = screen.right - w - 30, y = 70 + (kind - WIN_BOOKMARKS) * 24;
    if (x < 10)
        x = 10;
    if (y + h > screen.bottom - 10)
        h = screen.bottom - 10 - y;
    SetRect(&wr, (short)x, (short)y, (short)(x + w), (short)(y + h));
    const char *title = kind == WIN_BOOKMARKS ? "Bookmarks" : kind == WIN_HISTORY ? "History" : "Downloads";
    Str255 pt;
    mac_pstr(title, pt);
    WindowPtr win = NewCWindow(NULL, &wr, pt, false, kZoomDocProc, (WindowPtr)-1, true, (long)lw);
    if (!win)
        mac_out_of_memory(sizeof(WindowRecord));
    lw->base.win = win;
    SetPort((GrafPtr)win);
    Rect sr = {0, 0, 10, 10};
    lw->vscroll = NewControl(win, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, scrollBarProc, 0);
    if (!lw->vscroll)
        mac_out_of_memory(256);
    if (kind == WIN_BOOKMARKS) {
        add_button(lw, "Open", 64);
        add_button(lw, "New Folder", 96);
        add_button(lw, "Rename...", 86);
        add_button(lw, "Delete", 70);
    } else if (kind == WIN_HISTORY) {
        add_button(lw, "Open", 64);
        add_button(lw, "Delete", 70);
        add_button(lw, "Clear History", 110);
    } else {
        add_button(lw, "Stop", 58);
        add_button(lw, "Resume", 72);
        add_button(lw, "Remove", 72);
        add_button(lw, "Clear Completed", 124);
        add_button(lw, "Reveal", 66);
    }
    g_list[kind] = lw;
    make_gworld(lw);
    place(lw);
    rebuild(lw);
    ShowWindow(win);
    SelectWindow(win);
    mac_menus_dirty();
}
