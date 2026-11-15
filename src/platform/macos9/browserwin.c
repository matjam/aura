/* Browser windows.
 *
 * The chrome and the page are painted by the engine (src/browser) into an
 * offscreen 32-bit GWorld the size of the content area; its pixmap is the
 * engine's surface (big-endian xRGB is the engine's pixel layout, see
 * src/paint/paint.h), and CopyBits puts damaged rectangles on screen at
 * whatever depth the screen has. Native parts sit on top: the vertical
 * scroll bar, the grow box, and a TextEdit record placed over the
 * address field's text area, which edits the URL (selection, clipboard,
 * arrow keys, drag selection). The engine leaves the field's text to
 * TextEdit (AuBrowserWindow.native_field).
 *
 * Redraws are cheap: the model reports damaged rectangles through
 * AuBrowserHost.invalidate into a short list per window (AuPresent), and
 * flush() paints only those rectangles into the GWorld and copies only
 * them to the screen, clipped so the scroll bar and grow box are never
 * covered. A scroll moves the page's pixels instead of painting them
 * (AuBrowserHost.scroll_area): flush moves them in the GWorld (memmove)
 * and on screen (ScrollRect), and paints only the exposed strip and the
 * areas the model names (fixed and sticky boxes). Scroll events that
 * arrive before a flush add up to one move. The caret blink is TEIdle in
 * the field and repaints nothing else. */
#include "mac.h"

#include "browser/present.h"
#include "browser/scene.h"
#include "clock_mac.h"

#include <stdio.h>
#include <string.h>

struct BrowserWin {
    MacWin base;
    AuBrowserWindow *bw;
    GWorldPtr gw;
    AuSurface *surf;
    ControlHandle vscroll, hscroll;
    TEHandle te;
    bool field_focus;
    bool closing;
    AuPresent pres; /* damage and the pending scroll move */
    /* Debug > Page Timings: time in flush, split into painting into the
     * GWorld and copying to the screen (moves included). */
    uint32_t paint_us, blit_us;
    Point last_mouse;
    char *undo; /* field text before the last edit (Mac Roman) */
    size_t undo_len;
    /* Multiple clicks: the last mouse-down's time, place and count. */
    UInt32 click_when;
    Point click_at;
    int clicks;
    BrowserWin *next;
};

static BrowserWin *g_wins;
static BrowserWin *g_tracking; /* window whose scroll bar is being tracked */

/* ---- Helpers ---- */

static Rect qd_rect(AuIRect r)
{
    Rect q;
    SetRect(&q, (short)r.x0, (short)r.y0, (short)r.x1, (short)r.y1);
    return q;
}

static void content_size(BrowserWin *b, int *w, int *h)
{
    Rect r;
    GetWindowPortBounds(b->base.win, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

AuBrowserWindow *browserwin_model(BrowserWin *b)
{
    return b->bw;
}

BrowserWin *browserwin_front(void)
{
    for (WindowPtr w = FrontWindow(); w; w = mac_next_window(w)) {
        MacWin *m = mac_win_of(w);
        if (m && m->kind == WIN_BROWSER && ((BrowserWin *)m)->closing == false)
            return (BrowserWin *)m;
    }
    return NULL;
}

/* ---- GWorld ---- */

static void make_gworld(BrowserWin *b)
{
    int w, h;
    content_size(b, &w, &h);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (b->surf) {
        au_surface_free(b->surf);
        b->surf = NULL;
    }
    if (b->gw) {
        DisposeGWorld(b->gw);
        b->gw = NULL;
    }
    Rect r;
    SetRect(&r, 0, 0, (short)w, (short)h);
    /* Temporary memory first, so the window size does not eat into the
     * application heap; the application heap if that fails. */
    QDErr err = NewGWorld(&b->gw, 32, &r, NULL, NULL, useTempMem);
    if (err != noErr || !b->gw)
        err = NewGWorld(&b->gw, 32, &r, NULL, NULL, 0);
    if (err != noErr || !b->gw)
        mac_out_of_memory((size_t)w * (size_t)h * 4);
    PixMapHandle pm = GetGWorldPixMap(b->gw);
    if (!LockPixels(pm))
        mac_out_of_memory((size_t)w * (size_t)h * 4);
    Ptr base = GetPixBaseAddr(pm);
    int rowbytes = (**pm).rowBytes & 0x3FFF;
    b->surf = au_surface_wrap(base, w, h, rowbytes);
    au_surface_clear(b->surf, AU_RGB(255, 255, 255)); /* not left as random memory */
}

/* ---- Showing ---- */

/* The CopyBits mode: dithering only when the screen has 256 colours or
 * fewer. On a deeper screen the 32-bit GWorld copies straight across
 * (QuickDraw's fast path: same depth, no colour table, no scaling). */
static short copy_mode(BrowserWin *b)
{
    CGrafPtr port = (CGrafPtr)b->base.win;
    if ((port->portVersion & 0xC000) == 0xC000 && port->portPixMap && (**port->portPixMap).pixelSize > 8)
        return srcCopy;
    return srcCopy | kDitherCopy;
}

static bool touches(AuIRect a, AuIRect b)
{
    return !au_irect_empty(au_irect_intersect(a, b));
}

/* Copies r of the GWorld to the window, leaving out the scroll bar and
 * grow box, and redraws the address field's text when r covers it. */
static void blit(BrowserWin *b, AuIRect r)
{
    if (!b->gw || au_irect_empty(r))
        return;
    GrafPtr saved;
    GetPort(&saved);
    SetPort((GrafPtr)b->base.win);
    const AuChromeLayout *l = &b->bw->layout;
    Rect q = qd_rect(r);
    RgnHandle mask = NULL;
    /* A mask only when r reaches the native parts (most copies are page
     * strips, which do not). */
    if (touches(r, l->vscroll) || touches(r, l->grow) || touches(r, l->hscroll)) {
        RgnHandle cut = NewRgn();
        mask = NewRgn();
        if (!mask || !cut)
            mac_out_of_memory(64);
        RectRgn(mask, &q);
        Rect sb = qd_rect(l->vscroll);
        RectRgn(cut, &sb);
        DiffRgn(mask, cut, mask);
        Rect gr = qd_rect(l->grow);
        RectRgn(cut, &gr);
        DiffRgn(mask, cut, mask);
        if (!au_irect_empty(l->hscroll)) {
            Rect hr = qd_rect(l->hscroll);
            RectRgn(cut, &hr);
            DiffRgn(mask, cut, mask);
        }
        DisposeRgn(cut);
    }
    ForeColor(blackColor);
    BackColor(whiteColor);
    CopyBits(&((GrafPtr)b->gw)->portBits, &((GrafPtr)b->base.win)->portBits, &q, &q, copy_mode(b), mask);
    if (mask)
        DisposeRgn(mask);
    if (b->te && !au_irect_empty(au_irect_intersect(r, l->address_text))) {
        Rect view = qd_rect(l->address_text);
        ClipRect(&view);
        TEUpdate(&view, b->te);
        Rect all;
        GetWindowPortBounds(b->base.win, &all);
        ClipRect(&all);
    }
    SetPort(saved);
}

/* Moves the window's pixels in area by (dx, dy) on screen, as the GWorld's
 * were, and copies from the GWorld what could not be moved: the strip
 * uncovered, and parts of the area that were covered by other windows. */
static void scroll_screen(BrowserWin *b, AuIRect area, int dx, int dy)
{
    GrafPtr saved;
    GetPort(&saved);
    SetPort((GrafPtr)b->base.win);
    RgnHandle upd = NewRgn();
    if (!upd) {
        SetPort(saved);
        blit(b, area);
        return;
    }
    Rect q = qd_rect(area);
    ForeColor(blackColor);
    BackColor(whiteColor);
    ScrollRect(&q, (short)dx, (short)dy, upd);
    if (!EmptyRgn(upd)) {
        Rect box = (**upd).rgnBBox;
        CopyBits(&((GrafPtr)b->gw)->portBits, &((GrafPtr)b->base.win)->portBits, &box, &box, copy_mode(b), upd);
    }
    DisposeRgn(upd);
    SetPort(saved);
}

/* Takes the pending damage and move: moves the GWorld's pixels and paints
 * the damaged rectangles into it. */
static void paint_pending(BrowserWin *b, AuPresentRound *r)
{
    au_present_begin(&b->pres, b->surf, r);
    if (!b->surf || r->n == 0)
        return;
    AuCanvas *c = au_canvas_new(b->surf);
    for (int i = 0; i < r->n; i++)
        au_bw_paint(b->bw, c, r->rects[i]);
    au_canvas_free(c);
}

static void flush(BrowserWin *b)
{
    if (!au_present_pending(&b->pres) || !b->surf)
        return;
    uint32_t t0 = au_mac_clock_us();
    AuPresentRound r;
    paint_pending(b, &r);
    uint32_t t1 = au_mac_clock_us();
    if (r.shifted)
        scroll_screen(b, r.area, r.dx, r.dy);
    for (int i = 0; i < r.n; i++)
        blit(b, r.rects[i]);
    uint32_t t2 = au_mac_clock_us();
    b->paint_us += t1 - t0;
    b->blit_us += t2 - t1;
}

static void invalidate_all(BrowserWin *b)
{
    int w, h;
    content_size(b, &w, &h);
    au_present_invalidate(&b->pres, (AuIRect){0, 0, w, h});
}

static void draw_grow_box(BrowserWin *b)
{
    /* Clip so DrawGrowIcon draws only the box, not the scroll bar lines
     * across the content. */
    Rect r, g;
    GetWindowPortBounds(b->base.win, &r);
    SetRect(&g, (short)(r.right - kGrowBox), (short)(r.bottom - kGrowBox), r.right, r.bottom);
    ClipRect(&g);
    DrawGrowIcon(b->base.win);
    ClipRect(&r);
}

/* ---- Address field (TextEdit) ---- */

static char *field_text(BrowserWin *b)
{
    CharsHandle h = TEGetText(b->te);
    long n = (**b->te).teLength;
    SignedByte st = HGetState((Handle)h);
    HLock((Handle)h);
    char *s = mac_roman_to_utf8((const char *)*h, (size_t)n);
    HSetState((Handle)h, st);
    return s;
}

static void field_scroll_home(BrowserWin *b)
{
    TERec *t = *b->te;
    short dh = t->viewRect.left - t->destRect.left;
    if (dh)
        TEScroll(dh, 0, b->te);
}

/* Scrolls the field sideways so the selection end is visible. */
static void field_show_caret(BrowserWin *b)
{
    /* The caret's x from the text width (TEGetPoint crashed with an
     * access fault when called from PowerPC code in the 9.2.2 guest). */
    SignedByte state = HGetState((Handle)b->te);
    HLock((Handle)b->te);
    TERec *t = *b->te;
    CharsHandle text = TEGetText(b->te);
    SignedByte tstate = HGetState((Handle)text);
    HLock((Handle)text);
    SetPort((GrafPtr)b->base.win);
    short h = (short)(t->destRect.left + TextWidth((Ptr)*text, 0, t->selEnd));
    HSetState((Handle)text, tstate);
    short left = t->viewRect.left, right = t->viewRect.right;
    short dh = 0;
    if (h > right - 3)
        dh = (short)(right - 12 - h);
    else if (h < left + 1)
        dh = (short)(left + 12 - h);
    if (t->destRect.left + dh > left)
        dh = (short)(left - t->destRect.left);
    HSetState((Handle)b->te, state);
    if (dh)
        TEScroll(dh, 0, b->te);
}

static void field_set(BrowserWin *b, const char *utf8, bool select_all)
{
    size_t n;
    char *r = mac_utf8_to_roman(utf8, &n);
    SetPort((GrafPtr)b->base.win);
    TESetText(r, (long)n, b->te);
    au_free(r);
    field_scroll_home(b);
    TESetSelect(select_all ? 0 : (short)n, (short)n, b->te);
    Rect view = (**b->te).viewRect;
    ClipRect(&view);
    BackColor(whiteColor);
    EraseRect(&view);
    TEUpdate(&view, b->te);
    Rect all;
    GetWindowPortBounds(b->base.win, &all);
    ClipRect(&all);
}

static void place_field(BrowserWin *b)
{
    const AuChromeLayout *l = &b->bw->layout;
    Rect view = qd_rect(l->address_text);
    TERec *t = *b->te;
    short lh = t->lineHeight;
    short top = (short)(view.top + (view.bottom - view.top - lh) / 2);
    t->viewRect = view;
    SetRect(&t->destRect, view.left, top, (short)(view.left + 4000), (short)(top + lh));
    TECalText(b->te);
}

static void field_sync_model(BrowserWin *b)
{
    char *s = field_text(b);
    au_bw_set_field_text(b->bw, s);
    au_free(s);
}

static void undo_snapshot(BrowserWin *b)
{
    CharsHandle h = TEGetText(b->te);
    long n = (**b->te).teLength;
    au_free(b->undo);
    b->undo = au_alloc((size_t)n + 1);
    /* au_alloc may move memory: copy after it, with nothing in between. */
    memcpy(b->undo, *h, (size_t)n);
    b->undo_len = (size_t)n;
}

static void focus_field(BrowserWin *b, bool on)
{
    if (b->field_focus == on)
        return;
    b->field_focus = on;
    SetPort((GrafPtr)b->base.win);
    if (on) {
        if (b->base.win == FrontWindow() && g_in_front)
            TEActivate(b->te);
        TESetSelect(0, 32767, b->te);
    } else {
        TEDeactivate(b->te);
    }
    au_bw_set_field_focus(b->bw, on);
}

void browserwin_focus_field(BrowserWin *b)
{
    /* Open Location (Command-L) selects the whole address again when the
     * field already has the focus, ready to type over. */
    if (b->field_focus) {
        SetPort((GrafPtr)b->base.win);
        field_scroll_home(b);
        TESetSelect(0, 32767, b->te);
    }
    focus_field(b, true);
    flush(b);
}

static bool has_scheme(const char *s)
{
    return au_url_scheme_len(s, strlen(s)) != 0;
}

/* Inline completion: after typing at the end of the field, the best
 * history or bookmark match is filled in and the added part selected. */
static void autocomplete(BrowserWin *b)
{
    TERec *t = *b->te;
    if (t->selStart != t->selEnd || t->selEnd != t->teLength)
        return;
    char *typed = field_text(b);
    size_t tn = strlen(typed);
    const char *url = tn ? au_bw_complete(b->bw, typed) : NULL;
    if (url) {
        const char *key = url;
        size_t kn = strlen(url);
        if (!has_scheme(typed)) {
            size_t k = au_url_scheme_len(url, kn);
            key = url + (k ? k + 1 : 0);
            if (!strncmp(key, "//", 2))
                key += 2;
            if (strncmp(typed, "www.", 4) != 0 && !strncmp(key, "www.", 4))
                key += 4;
            kn = strlen(key);
        }
        if (kn > tn && au_ieq(key, tn, typed, tn)) {
            AuBuf full;
            au_buf_init(&full);
            au_buf_puts(&full, typed);
            au_buf_puts(&full, key + tn);
            size_t rn, fn;
            char *rt = mac_utf8_to_roman(typed, &rn);
            char *rf = mac_utf8_to_roman(au_buf_cstr(&full), &fn);
            TESetText(rf, (long)fn, b->te);
            TESetSelect((short)rn, (short)fn, b->te);
            Rect view = (**b->te).viewRect;
            BackColor(whiteColor);
            EraseRect(&view);
            TEUpdate(&view, b->te);
            au_free(rt);
            au_free(rf);
            au_buf_free(&full);
        }
    }
    au_free(typed);
}

static void field_commit(BrowserWin *b)
{
    char *s = field_text(b);
    focus_field(b, false);
    au_bw_navigate_input(b->bw, s);
    au_free(s);
    field_set(b, au_bw_field_text(b->bw), false);
}

static void field_revert(BrowserWin *b)
{
    au_bw_set_field_text(b->bw, NULL);
    field_set(b, au_bw_field_text(b->bw), true);
}

/* ---- Scroll bar ---- */

/* kControlScrollBarLiveProc: the Appearance Manager's scroll bar, which
 * calls the action procedure while its thumb is dragged. */
enum { kLiveScrollBarProc = 386 };

static bool live_scroll_bars(void)
{
    static int known = -1;
    if (known < 0) {
        long resp = 0;
        known = Gestalt('appr', &resp) == noErr && (resp & 1) ? 1 : 0; /* gestaltAppearanceExists */
    }
    return known == 1;
}

static void place_controls(BrowserWin *b)
{
    const AuChromeLayout *l = &b->bw->layout;
    SetPort((GrafPtr)b->base.win);
    HideControl(b->vscroll);
    MoveControl(b->vscroll, (short)l->vscroll.x0, (short)l->vscroll.y0);
    SizeControl(b->vscroll, (short)(l->vscroll.x1 - l->vscroll.x0), (short)(l->vscroll.y1 - l->vscroll.y0));
    ShowControl(b->vscroll);
    HideControl(b->hscroll);
    if (!au_irect_empty(l->hscroll)) {
        MoveControl(b->hscroll, (short)l->hscroll.x0, (short)l->hscroll.y0);
        SizeControl(b->hscroll, (short)(l->hscroll.x1 - l->hscroll.x0), (short)(l->hscroll.y1 - l->hscroll.y0));
        ShowControl(b->hscroll);
    }
    place_field(b);
}

static void update_scroll(BrowserWin *b)
{
    int mx, my;
    au_bw_scroll_max(b->bw, &mx, &my);
    if (my > 32767)
        my = 32767; /* Control Manager values are 16-bit */
    AuTab *t = au_bw_active_tab(b->bw);
    int y = t ? t->scroll_y : 0;
    if (y > 32767)
        y = 32767;
    SetPort((GrafPtr)b->base.win);
    if (GetControlMaximum(b->vscroll) != my)
        SetControlMaximum(b->vscroll, (short)my);
    if (GetControlValue(b->vscroll) != y)
        SetControlValue(b->vscroll, (short)y);
    if (mx > 32767)
        mx = 32767;
    int x = t ? t->scroll_x : 0;
    if (x > 32767)
        x = 32767;
    if (GetControlMaximum(b->hscroll) != mx)
        SetControlMaximum(b->hscroll, (short)mx);
    if (GetControlValue(b->hscroll) != x)
        SetControlValue(b->hscroll, (short)x);
}

static pascal void scroll_action(ControlHandle c, short part)
{
    BrowserWin *b = g_tracking;
    if (!b)
        return;
    const AuChromeLayout *l = &b->bw->layout;
    bool horiz = c == b->hscroll;
    int page = horiz ? (l->page.x1 - l->page.x0) - 40 : (l->page.y1 - l->page.y0) - 40;
    if (page < 20)
        page = 20;
    int d = 0;
    switch (part) {
    case inUpButton: d = -16; break;
    case inDownButton: d = 16; break;
    case inPageUp: d = -page; break;
    case inPageDown: d = page; break;
    case inThumb: {
        /* The live scroll bar (Appearance) calls this while the thumb is
         * dragged: the page follows. */
        AuTab *t = au_bw_active_tab(b->bw);
        if (horiz)
            au_bw_scroll_to(b->bw, GetControlValue(c), t ? t->scroll_y : 0);
        else
            au_bw_scroll_to(b->bw, t ? t->scroll_x : 0, GetControlValue(c));
        flush(b);
        return;
    }
    }
    if (horiz)
        au_bw_scroll_by(b->bw, d, 0);
    else
        au_bw_scroll_by(b->bw, 0, d);
    flush(b);
}

/* ---- Host callbacks ---- */

static void host_invalidate(void *ctx, AuBrowserWindow *w, AuIRect r)
{
    BrowserWin *b = w->platform;
    (void)ctx;
    if (b)
        au_present_invalidate(&b->pres, r);
}

static void host_scroll_area(void *ctx, AuBrowserWindow *w, AuIRect area, int dx, int dy)
{
    BrowserWin *b = w->platform;
    (void)ctx;
    if (b)
        au_present_scroll(&b->pres, area, dx, dy);
}

static void host_changed(void *ctx, AuBrowserWindow *w, unsigned what)
{
    BrowserWin *b = w->platform;
    (void)ctx;
    if (!b)
        return;
    if (what & AU_CHANGED_LAYOUT)
        place_controls(b);
    if (what & AU_CHANGED_FIELD) {
        if (!(b->field_focus && w->field))
            field_set(b, au_bw_field_text(w), b->field_focus);
    }
    if (what & AU_CHANGED_TITLE) {
        char title[256];
        au_bw_window_title(w, title, sizeof title);
        Str255 p;
        mac_pstr(title, p);
        SetWTitle(b->base.win, p);
        mac_menus_dirty();
    }
    if (what & AU_CHANGED_SCROLL)
        update_scroll(b);
    if (what & AU_CHANGED_MENUS)
        mac_menus_dirty();
    if ((what & AU_CHANGED_FOCUS_PAGE) && b->field_focus) {
        focus_field(b, false);
        field_set(b, au_bw_field_text(w), false);
    }
    if (what & AU_CHANGED_FOCUS_FIELD)
        focus_field(b, true);
}

static void host_close(void *ctx, AuBrowserWindow *w)
{
    BrowserWin *b = w->platform;
    (void)ctx;
    /* Closing now would free the model under the caller; the event loop
     * closes it after this event (browserwin_flush_all). */
    if (b)
        b->closing = true;
}

/* mailto:, ftp: and other addresses go to their Internet Config helper. */
static bool host_external(void *ctx, const char *url)
{
    (void)ctx;
    return mac_ic_launch_url(url);
}

/* "Warn before downloading applications": ALRT 134, Download or Cancel. */
static bool host_confirm_download(void *ctx, const char *url, const char *name, const char *mime)
{
    (void)ctx, (void)url;
    if (!g_browser->prefs.warn_app_downloads || !mac_download_is_app(name, mime))
        return true;
    Str255 p;
    mac_pstr(name, p);
    ParamText(p, (ConstStringPtr) "\p", (ConstStringPtr) "\p", (ConstStringPtr) "\p");
    InitCursor();
    return CautionAlert(134, NULL) == 1;
}

static void host_download_added(void *ctx, AuDownload *d)
{
    (void)ctx, (void)d;
    listwin_show(WIN_DOWNLOADS);
}

static uint32_t host_ms(void *ctx)
{
    (void)ctx;
    return au_platform_time_ms();
}

static AuTime host_now(void *ctx)
{
    (void)ctx;
    return mac_now();
}

static bool host_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    return mac_read_file_url(url, data, len);
}

void browserwin_host(AuBrowserHost *h)
{
    memset(h, 0, sizeof *h);
    h->time_ms = host_ms;
    h->now = host_now;
    h->read_file_url = host_read;
    h->invalidate = host_invalidate;
    h->changed = host_changed;
    h->close_window = host_close;
    h->open_external = host_external;
    h->confirm_download = host_confirm_download;
    h->download_added = host_download_added;
    h->scroll_area = host_scroll_area;
}

/* ---- Events ---- */

static uint32_t mods_of(AuEventModifiers m)
{
    uint32_t r = 0;
    if (m & shiftKey)
        r |= AU_MOD_SHIFT;
    if (m & cmdKey)
        r |= AU_MOD_COMMAND;
    if (m & optionKey)
        r |= AU_MOD_OPTION;
    if (m & ControlKey)
        r |= AU_MOD_CONTROL;
    return r;
}

static void bw_update(MacWin *mw)
{
    BrowserWin *b = (BrowserWin *)mw;
    WindowPtr w = mw->win;
    SetPort((GrafPtr)w);
    BeginUpdate(w);
    AuPresentRound r;
    paint_pending(b, &r); /* the whole window is copied below */
    int cw, ch;
    content_size(b, &cw, &ch);
    blit(b, (AuIRect){0, 0, cw, ch});
    DrawControls(w);
    draw_grow_box(b);
    EndUpdate(w);
}

static void bw_activate(MacWin *mw, bool on)
{
    BrowserWin *b = (BrowserWin *)mw;
    SetPort((GrafPtr)mw->win);
    HiliteControl(b->vscroll, on ? 0 : 255);
    HiliteControl(b->hscroll, on ? 0 : 255);
    if (b->field_focus) {
        if (on)
            TEActivate(b->te);
        else
            TEDeactivate(b->te);
    }
    au_bw_set_active(b->bw, on);
    draw_grow_box(b);
    flush(b);
}

static bool bw_edit(MacWin *mw, int item);
static void context_menu(BrowserWin *b, Point pt, Point global);

enum { kHistoryMenuID = 181, kHistoryItems = 15 };

/* The pop-up under Back (dir -1) or Forward (dir 1): the entries that
 * way, nearest first; choosing one goes there. */
static void history_popup(BrowserWin *b, int dir)
{
    AuHistoryItem items[kHistoryItems];
    int n = au_bw_history_items(b->bw, dir, items, kHistoryItems);
    if (n == 0)
        return;
    MenuHandle m = NewMenu(kHistoryMenuID, (ConstStringPtr) "\p");
    if (!m)
        return;
    for (int i = 0; i < n; i++)
        mac_append_item(m, items[i].title);
    InsertMenu(m, -1);
    AuIRect r = dir < 0 ? b->bw->layout.back : b->bw->layout.forward;
    Point at = {(short)r.y1, (short)r.x0};
    LocalToGlobal(&at);
    long choice = PopUpMenuSelect(m, at.v, at.h, 0);
    DeleteMenu(kHistoryMenuID);
    DisposeMenu(m);
    short item = (short)(choice & 0xFFFF);
    if ((short)(choice >> 16) == kHistoryMenuID && item >= 1 && item <= n)
        au_bw_go(b->bw, items[item - 1].delta);
    flush(b);
}

static void track_mouse(BrowserWin *b, EventRecord *ev, Point pt)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_MOUSE_DOWN;
    e.x = pt.h;
    e.y = pt.v;
    e.mods = mods_of(ev->modifiers);
    /* A click within the double-click time of the last one, near it,
     * counts up: 2 selects a word, 3 a paragraph. */
    int dx = pt.h - b->click_at.h, dy = pt.v - b->click_at.v;
    if (b->clicks && ev->when - b->click_when <= (UInt32)GetDblTime() && dx * dx + dy * dy <= 16)
        b->clicks = b->clicks < 3 ? b->clicks + 1 : 1;
    else
        b->clicks = 1;
    b->click_when = ev->when;
    b->click_at = pt;
    e.clicks = b->clicks;
    e.time_ms = au_platform_time_ms();
    AuHit hit = au_bw_mouse(b->bw, &e);
    browserwin_flush_all();
    Point last = pt;
    /* Holding the button still on the page for most of a second opens
     * the contextual menu, as Control-click does; on Back or Forward,
     * after half a second, the pop-up of the entries that way. */
    bool history = hit.part == AU_HIT_BACK || hit.part == AU_HIT_FORWARD;
    bool may_hold = (hit.part == AU_HIT_PAGE || history) && b->clicks == 1 && !(ev->modifiers & (shiftKey | cmdKey));
    while (StillDown() && !b->closing) {
        Point p;
        GetMouse(&p);
        if (may_hold && (p.h - pt.h) * (p.h - pt.h) + (p.v - pt.v) * (p.v - pt.v) > 9)
            may_hold = false;
        if (may_hold && TickCount() - ev->when >= (history ? 30u : 45u)) {
            /* End the press where nothing is, so no link opens and the
             * button does not act. */
            e.type = AU_EVENT_MOUSE_UP;
            e.x = e.y = -30000;
            au_bw_mouse(b->bw, &e);
            b->clicks = 0;
            if (history) {
                history_popup(b, hit.part == AU_HIT_BACK ? -1 : 1);
            } else {
                Point global = pt;
                LocalToGlobal(&global);
                context_menu(b, pt, global);
            }
            return;
        }
        if (p.h != last.h || p.v != last.v) {
            last = p;
            e.type = AU_EVENT_MOUSE_MOVE;
            e.x = p.h;
            e.y = p.v;
            au_bw_mouse(b->bw, &e);
            flush(b);
        }
    }
    if (b->closing)
        return;
    e.type = AU_EVENT_MOUSE_UP;
    e.x = last.h;
    e.y = last.v;
    au_bw_mouse(b->bw, &e);
    b->last_mouse = last;
}

static bool clipboard_has_text(void)
{
    long off = 0;
    return GetScrap(NULL, 'TEXT', &off) > 0;
}

/* Paste and Go: the clipboard's text, line breaks as spaces, as typed
 * address-field input. */
static void paste_and_go(BrowserWin *b)
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
        for (char *p = u; *p; p++)
            if (*p == '\r' || *p == '\n' || *p == '\t')
                *p = ' ';
        char *s = u;
        while (*s == ' ')
            s++;
        size_t len = strlen(s);
        while (len && s[len - 1] == ' ')
            s[--len] = 0;
        if (*s) {
            field_revert(b);
            focus_field(b, false);
            au_bw_navigate_input(b->bw, s);
        }
        au_free(u);
    }
    DisposeHandle(h);
}

/* The contextual menu for local point pt. */
static void context_menu(BrowserWin *b, Point pt, Point global)
{
    AuHit h = au_chrome_hit(&b->bw->layout, pt.h, pt.v);
    bool field = h.part == AU_HIT_ADDRESS || h.part == AU_HIT_SECURITY;
    if (field && !b->field_focus)
        focus_field(b, true);
    unsigned flags = 0;
    if (b->field_focus) {
        TEPtr te = *b->te;
        if (te->selStart != te->selEnd)
            flags |= AU_CONTEXT_FIELD_SELECTION;
        if (te->teLength > 0)
            flags |= AU_CONTEXT_FIELD_TEXT;
    }
    if (clipboard_has_text())
        flags |= AU_CONTEXT_CLIPBOARD_TEXT;
    AuContextMenu m;
    if (!au_bw_context_menu(b->bw, pt.h, pt.v, flags, &m))
        return;
    flush(b);
    int i = mac_context_select(&m, global);
    AuCommand cmd = i >= 0 ? m.items[i].cmd : AU_CMD_NONE;
    SetPort((GrafPtr)b->base.win);
    if (!au_bw_context_command(b->bw, &m, cmd)) {
        switch (cmd) {
        case AU_CMD_OPEN_LINK_WINDOW:
            if (m.link)
                browserwin_new(m.link, NULL);
            break;
        case AU_CMD_COPY_LINK:
            if (m.link)
                mac_put_scrap(m.link, strlen(m.link));
            break;
        case AU_CMD_COPY_IMAGE:
            if (m.image)
                mac_put_scrap(m.image, strlen(m.image));
            break;
        case AU_CMD_COPY: bw_edit(&b->base, 4); break;
        case AU_CMD_CUT: bw_edit(&b->base, 3); break;
        case AU_CMD_FIELD_COPY: bw_edit(&b->base, 4); break;
        case AU_CMD_PASTE: bw_edit(&b->base, 5); break;
        case AU_CMD_SELECT_ALL: bw_edit(&b->base, 7); break;
        case AU_CMD_PASTE_GO: paste_and_go(b); break;
        default: break;
        }
    }
    au_context_menu_free(&m);
    if (!b->closing)
        flush(b);
}

static void bw_click(MacWin *mw, EventRecord *ev)
{
    BrowserWin *b = (BrowserWin *)mw;
    SetPort((GrafPtr)mw->win);
    Point pt = ev->where;
    GlobalToLocal(&pt);
    if (mac_context_click(ev)) {
        context_menu(b, pt, ev->where);
        return;
    }
    AuHit h = au_chrome_hit(&b->bw->layout, pt.h, pt.v);
    /* The padlock opens the certificate viewer (a page with a
     * certificate); elsewhere it is part of the field. */
    if (h.part == AU_HIT_SECURITY && au_bw_show_certificate(b->bw)) {
        while (StillDown())
            ;
        flush(b);
        return;
    }
    if (h.part == AU_HIT_ADDRESS || h.part == AU_HIT_SECURITY) {
        if (!b->field_focus) {
            /* The first click selects the whole URL, as browsers do. */
            focus_field(b, true);
            while (StillDown())
                ;
        } else {
            TEClick(pt, (ev->modifiers & shiftKey) != 0, b->te);
            field_show_caret(b);
        }
        flush(b);
        return;
    }
    if (h.part == AU_HIT_VSCROLL || h.part == AU_HIT_HSCROLL) {
        ControlHandle c;
        short part = FindControl(pt, mw->win, &c);
        if ((c == b->vscroll || c == b->hscroll) && part) {
            g_tracking = b;
            static ControlActionUPP action;
            if (!action)
                action = NewControlActionUPP(scroll_action);
            if (part == inThumb) {
                /* The live scroll bar tracks the thumb through the action
                 * procedure; the classic one drags an outline and takes
                 * none. */
                TrackControl(c, pt, live_scroll_bars() ? action : NULL);
                AuTab *t = au_bw_active_tab(b->bw);
                if (c == b->hscroll)
                    au_bw_scroll_to(b->bw, GetControlValue(c), t ? t->scroll_y : 0);
                else
                    au_bw_scroll_to(b->bw, t ? t->scroll_x : 0, GetControlValue(c));
            } else {
                TrackControl(c, pt, action);
            }
            g_tracking = NULL;
            flush(b);
        }
        return;
    }
    if (b->field_focus && h.part == AU_HIT_PAGE) {
        field_revert(b);
        focus_field(b, false);
    }
    track_mouse(b, ev, pt);
}

static void page_key(BrowserWin *b, int key, uint32_t ch, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.key = key;
    e.ch = ch;
    e.mods = mods;
    au_bw_key(b->bw, &e);
}

static bool bw_key(MacWin *mw, EventRecord *ev)
{
    BrowserWin *b = (BrowserWin *)mw;
    unsigned char ch = (unsigned char)(ev->message & charCodeMask);
    SetPort((GrafPtr)mw->win);
    if (b->field_focus) {
        switch (ch) {
        case 0x0D: /* Return */
        case 0x03: /* Enter */
            field_commit(b);
            return true;
        case 0x1B: /* Escape */
            field_revert(b);
            return true;
        case 0x09: /* Tab: leave the field */
            field_revert(b);
            focus_field(b, false);
            return true;
        default:
            break;
        }
        bool printable = ch >= 0x20 && ch != 0x7F;
        undo_snapshot(b);
        TEKey((CharParameter)ch, b->te);
        field_sync_model(b);
        if (printable)
            autocomplete(b);
        field_show_caret(b);
        return true;
    }
    uint32_t mods = mods_of(ev->modifiers);
    switch (ch) {
    case 0x1E: page_key(b, AU_KEY_UP, 0, mods); break;
    case 0x1F: page_key(b, AU_KEY_DOWN, 0, mods); break;
    case 0x1C: page_key(b, AU_KEY_LEFT, 0, mods); break;
    case 0x1D: page_key(b, AU_KEY_RIGHT, 0, mods); break;
    case 0x0B: page_key(b, AU_KEY_PAGE_UP, 0, mods); break;
    case 0x0C: page_key(b, AU_KEY_PAGE_DOWN, 0, mods); break;
    case 0x01: page_key(b, AU_KEY_HOME, 0, mods); break;
    case 0x04: page_key(b, AU_KEY_END, 0, mods); break;
    case ' ': page_key(b, 0, ' ', mods); break;
    case 0x1B: au_bw_stop(b->bw); break;
    default: return false;
    }
    return true;
}

static bool bw_edit(MacWin *mw, int item)
{
    BrowserWin *b = (BrowserWin *)mw;
    if (!b->field_focus) {
        /* The page: Copy the selection, Select All. (Item 0 asks about
         * text editing, which the page does not do; main.c enables
         * Copy and Select All itself.) */
        if (item == 4) {
            AuBuf t;
            au_buf_init(&t);
            if (au_bw_selection_text(b->bw, &t))
                mac_put_scrap(t.data, t.len);
            au_buf_free(&t);
            return true;
        }
        if (item == 7) {
            au_bw_select_all(b->bw);
            flush(b);
            return true;
        }
        return false;
    }
    if (item == 0)
        return true;
    SetPort((GrafPtr)mw->win);
    switch (item) {
    case 1: /* Undo: swap the field with the snapshot */
        if (b->undo) {
            char *u = mac_roman_to_utf8(b->undo, b->undo_len);
            undo_snapshot(b);
            field_set(b, u, false);
            au_free(u);
        }
        break;
    case 3:
        undo_snapshot(b);
        TECut(b->te);
        ZeroScrap();
        TEToScrap();
        break;
    case 4:
        TECopy(b->te);
        ZeroScrap();
        TEToScrap();
        return true;
    case 5: {
        undo_snapshot(b);
        TEFromScrap();
        /* The field is one line: pasted line breaks and tabs become
         * spaces (an address copied across lines still works). */
        Handle scrap = TEScrapHandle();
        long n = TEGetScrapLength();
        if (scrap && *scrap) {
            for (long i = 0; i < n; i++) {
                char c = (*scrap)[i];
                if (c == '\r' || c == '\n' || c == '\t')
                    (*scrap)[i] = ' ';
            }
        }
        TEPaste(b->te);
        break;
    }
    case 6:
        undo_snapshot(b);
        TEDelete(b->te);
        break;
    case 7: TESetSelect(0, 32767, b->te); return true;
    default: return false;
    }
    field_sync_model(b);
    field_show_caret(b);
    return true;
}

static void bw_idle(MacWin *mw)
{
    BrowserWin *b = (BrowserWin *)mw;
    au_bw_idle(b->bw);
    if (mw->win == FrontWindow() && g_in_front) {
        SetPort((GrafPtr)mw->win);
        if (b->field_focus)
            TEIdle(b->te);
        Point p;
        GetMouse(&p);
        Rect r;
        GetWindowPortBounds(mw->win, &r);
        if (p.h != b->last_mouse.h || p.v != b->last_mouse.v) {
            b->last_mouse = p;
            if (PtInRect(p, &r)) {
                AuEvent e;
                memset(&e, 0, sizeof e);
                e.type = AU_EVENT_MOUSE_MOVE;
                e.x = p.h;
                e.y = p.v;
                au_bw_mouse(b->bw, &e);
            }
            Rect field = qd_rect(b->bw->layout.address_text);
            if (PtInRect(p, &field))
                SetCursor(*GetCursor(iBeamCursor));
            else
                InitCursor();
        }
    }
    flush(b);
}

static void bw_grow(MacWin *mw)
{
    BrowserWin *b = (BrowserWin *)mw;
    SetPort((GrafPtr)mw->win);
    make_gworld(b);
    au_present_init(&b->pres); /* a pending move was for the old GWorld */
    int w, h;
    content_size(b, &w, &h);
    au_bw_resize(b->bw, w, h);
    place_controls(b);
    invalidate_all(b);
    Rect r;
    GetWindowPortBounds(mw->win, &r);
    InvalRect(&r);
    /* The size is remembered for new windows. */
    g_browser->prefs.window_width = w;
    g_browser->prefs.window_height = h;
    g_browser->prefs_gen++;
}

void browserwin_moved(BrowserWin *b)
{
    GrafPtr old;
    GetPort(&old);
    SetPort((GrafPtr)b->base.win);
    Point p = {0, 0};
    LocalToGlobal(&p);
    SetPort(old);
    au_bw_set_position(b->bw, p.h, p.v);
}

static void bw_close(MacWin *mw)
{
    browserwin_close((BrowserWin *)mw);
}

void browserwin_close(BrowserWin *b)
{
    for (BrowserWin **p = &g_wins; *p; p = &(*p)->next) {
        if (*p == b) {
            *p = b->next;
            break;
        }
    }
    au_browser_window_free(b->bw);
    if (b->te)
        TEDispose(b->te);
    DisposeWindow(b->base.win); /* disposes the scroll bar too */
    if (b->surf)
        au_surface_free(b->surf);
    if (b->gw)
        DisposeGWorld(b->gw);
    au_free(b->undo);
    au_free(b);
    mac_menus_dirty();
}

void browserwin_flush_all(void)
{
    BrowserWin *b = g_wins;
    while (b) {
        BrowserWin *next = b->next;
        if (b->closing)
            browserwin_close(b);
        else
            flush(b);
        b = next;
    }
}

BrowserWin *browserwin_new(const char *url, const AuSessionWindow *sw)
{
    Rect screen = qd.screenBits.bounds;
    int count = 0;
    for (BrowserWin *x = g_wins; x; x = x->next)
        count++;
    int cw = g_browser->prefs.window_width > 0 ? g_browser->prefs.window_width : 640;
    int ch = g_browser->prefs.window_height > 0 ? g_browser->prefs.window_height : 480;
    if (sw && sw->width >= 300 && sw->height >= 200) {
        cw = sw->width;
        ch = sw->height;
    }
    int x = 20 + (count % 8) * 20, y = 50 + (count % 8) * 20;
    /* A restored window goes back where it was when its title bar is
     * still on this screen (the screen may have changed since). */
    if (sw && sw->x >= 0 && sw->y >= GetMBarHeight() + 18 && sw->x <= screen.right - 80 &&
        sw->y <= screen.bottom - 60) {
        x = sw->x;
        y = sw->y;
    }
    if (cw > screen.right - x - 4)
        cw = screen.right - x - 4;
    if (ch > screen.bottom - y - 26) /* clear of the Control Strip */
        ch = screen.bottom - y - 26;
    if (cw < 300)
        cw = 300;
    if (ch < 200)
        ch = 200;
    BrowserWin *b = au_calloc(sizeof *b);
    au_present_init(&b->pres);
    b->base.kind = WIN_BROWSER;
    b->base.update = bw_update;
    b->base.activate = bw_activate;
    b->base.click = bw_click;
    b->base.key = bw_key;
    b->base.idle = bw_idle;
    b->base.grow = bw_grow;
    b->base.close = bw_close;
    b->base.edit = bw_edit;
    Rect wr;
    SetRect(&wr, (short)x, (short)y, (short)(x + cw), (short)(y + ch));
    WindowPtr w = NewCWindow(NULL, &wr, (ConstStr255Param) "\pAura", false, kZoomDocProc, (WindowPtr)-1, true,
                             (long)b);
    if (!w)
        mac_out_of_memory(sizeof(WindowRecord));
    b->base.win = w;
    SetPort((GrafPtr)w);
    Rect sr = {0, 0, 10, 10};
    short sbproc = live_scroll_bars() ? kLiveScrollBarProc : scrollBarProc;
    b->vscroll = NewControl(w, &sr, (ConstStr255Param) "\p", true, 0, 0, 0, sbproc, 0);
    b->hscroll = NewControl(w, &sr, (ConstStr255Param) "\p", false, 0, 0, 0, sbproc, 0);
    if (!b->vscroll || !b->hscroll)
        mac_out_of_memory(256);
    make_gworld(b);
    int iw, ih;
    content_size(b, &iw, &ih);
    b->bw = au_browser_window_new(g_browser, iw, ih);
    b->bw->platform = b;
    au_bw_set_position(b->bw, x, y);
    b->bw->native_field = true;
    b->next = g_wins;
    g_wins = b;
    /* The field: Geneva 12, one line that never wraps. */
    TextFont(3);
    TextSize(12);
    TextFace(0);
    Rect fr = qd_rect(b->bw->layout.address_text);
    b->te = TENew(&fr, &fr);
    if (!b->te)
        mac_out_of_memory(1024);
    (**b->te).crOnly = -1;
    place_controls(b);
    if (sw && sw->ntabs)
        au_bw_restore(b->bw, sw);
    else
        au_bw_new_tab(b->bw, url ? url : g_browser->prefs.home_page, true, -1);
    field_set(b, au_bw_field_text(b->bw), false);
    update_scroll(b);
    host_changed(NULL, b->bw, AU_CHANGED_TITLE);
    invalidate_all(b);
    AuPresentRound r;
    paint_pending(b, &r); /* shown by the first update event */
    ShowWindow(w);
    SelectWindow(w);
    mac_menus_dirty();
    return b;
}

/* ---- Edit > Find ---- */

enum { FIND_OK = 1, FIND_CANCEL, FIND_TEXT, FIND_LABEL, FIND_CASE };

void browserwin_find(BrowserWin *b)
{
    if (!au_bw_active_tab(b->bw))
        return;
    DialogPtr d = GetNewDialog(201, NULL, (WindowPtr)-1);
    if (!d)
        return;
    short type;
    Handle h;
    Rect r;
    Str255 p;
    mac_pstr(g_browser->find_text ? g_browser->find_text : "", p);
    GetDialogItem(d, FIND_TEXT, &type, &h, &r);
    SetDialogItemText(h, p);
    SelectDialogItemText(d, FIND_TEXT, 0, 32767);
    GetDialogItem(d, FIND_CASE, &type, &h, &r);
    ControlHandle check = (ControlHandle)h;
    SetControlValue(check, g_browser->find_case ? 1 : 0);
    SetDialogDefaultItem(d, FIND_OK);
    SetDialogCancelItem(d, FIND_CANCEL);
    ShowWindow((WindowPtr)d);
    short item = 0;
    for (;;) {
        ModalDialog(NULL, &item);
        if (item == FIND_CASE)
            SetControlValue(check, !GetControlValue(check));
        else if (item == FIND_OK || item == FIND_CANCEL)
            break;
    }
    char *text = NULL;
    bool match_case = GetControlValue(check) != 0;
    if (item == FIND_OK) {
        GetDialogItem(d, FIND_TEXT, &type, &h, &r);
        GetDialogItemText(h, p);
        text = mac_roman_to_utf8((const char *)p + 1, p[0]);
    }
    DisposeDialog(d);
    if (text) {
        SetPort((GrafPtr)b->base.win);
        if (!au_bw_find(b->bw, text, match_case))
            SysBeep(1);
        au_free(text);
        flush(b);
    }
}

/* ---- Debug > Page Timings ---- */

static void ms10(char *out, size_t cap, uint32_t tenths)
{
    snprintf(out, cap, "%lu.%lu", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
}

typedef struct TimedScroll {
    int steps;
    uint32_t total_us, paint_us, blit_us;
    size_t hits, misses, evictions; /* glyph cache */
} TimedScroll;

/* Scrolls by step up to n times, flushing after each as the event loop
 * does, and times it. */
static TimedScroll timed_scrolls(BrowserWin *b, AuTab *t, int step, int n)
{
    TimedScroll r;
    memset(&r, 0, sizeof r);
    flush(b);
    AuGlyphCacheStats g0, g1;
    au_glyph_cache_stats(&g0);
    b->paint_us = b->blit_us = 0;
    uint32_t u0 = au_mac_clock_us();
    for (int i = 0; i < n; i++) {
        int before = t->scroll_y;
        au_bw_scroll_by(b->bw, 0, step);
        if (t->scroll_y == before)
            break;
        flush(b);
        r.steps++;
    }
    r.total_us = au_mac_clock_us() - u0;
    r.paint_us = b->paint_us;
    r.blit_us = b->blit_us;
    au_glyph_cache_stats(&g1);
    r.hits = g1.hits - g0.hits;
    r.misses = g1.misses - g0.misses;
    r.evictions = g1.evictions - g0.evictions;
    return r;
}

/* "12.3 = 4.5 + 6.7" per step, in milliseconds. */
static void timed_text(char *out, size_t cap, const TimedScroll *s)
{
    char a[16], p[16], c[16];
    uint32_t n = s->steps > 0 ? (uint32_t)s->steps : 1;
    ms10(a, sizeof a, s->total_us / 100 / n);
    ms10(p, sizeof p, s->paint_us / 100 / n);
    ms10(c, sizeof c, s->blit_us / 100 / n);
    snprintf(out, cap, "%s = %s + %s", a, p, c);
}

void browserwin_page_timings(BrowserWin *b)
{
    AuTab *t = au_bw_active_tab(b->bw);
    if (!t)
        return;
    SetCursor(*GetCursor(watchCursor));
    AuPageStats st;
    au_page_stats(t->page, &st);
    /* Scrolling: ten steps of 32 pixels and five page downs (up near the
     * end), each painted and shown as a scroll by the user is, then a
     * repaint of the whole page; flush() times painting and copying. */
    int mx, my;
    au_bw_scroll_max(b->bw, &mx, &my);
    int start = t->scroll_y;
    const AuChromeLayout *l = &b->bw->layout;
    int page = (l->page.y1 - l->page.y0) - 40;
    TimedScroll small = timed_scrolls(b, t, start + 320 <= my ? 32 : -32, 10);
    TimedScroll big = timed_scrolls(b, t, start + 5 * page <= my ? page : -page, 5);
    au_bw_scroll_to(b->bw, t->scroll_x, start);
    flush(b);
    TimedScroll full;
    memset(&full, 0, sizeof full);
    AuGlyphCacheStats g0, g1;
    au_glyph_cache_stats(&g0);
    for (int i = 0; i < 3; i++) {
        b->paint_us = b->blit_us = 0;
        au_present_invalidate(&b->pres, l->page);
        uint32_t u0 = au_mac_clock_us();
        flush(b);
        full.total_us += au_mac_clock_us() - u0;
        full.paint_us += b->paint_us;
        full.blit_us += b->blit_us;
        full.steps++;
    }
    au_glyph_cache_stats(&g1);
    update_scroll(b);
    long heap_free = FreeMem(), heap_block = MaxBlock();
    long temp_free = (long)TempFreeMem();
    char l0[200], l1[200], l2[400], l3[256], a[3][64];
    snprintf(l0, sizeof l0, "%s: first paint %lu ms, loaded in %lu ms (emulated timings vary).", au_page_title(t->page),
             (unsigned long)st.first_paint_ms, (unsigned long)st.load_ms);
    snprintf(l1, sizeof l1, "Layout: last pass %lu ms, %lu passes %lu ms in all; last paint %lu ms; %lu boxes, %lu bytes, %lu resources.",
             (unsigned long)st.layout_ms, (unsigned long)st.layouts, (unsigned long)st.layout_total_ms,
             (unsigned long)st.paint_ms, (unsigned long)st.boxes, (unsigned long)st.bytes,
             (unsigned long)st.resources);
    if (small.steps) {
        timed_text(a[0], sizeof a[0], &small);
        timed_text(a[1], sizeof a[1], &big);
        timed_text(a[2], sizeof a[2], &full);
        snprintf(l2, sizeof l2,
                 "Scrolling (ms: total = painting + copying to the screen): 32-pixel step %s; page down %s; "
                 "whole page %s.",
                 a[0], a[1], a[2]);
    } else {
        timed_text(a[2], sizeof a[2], &full);
        snprintf(l2, sizeof l2, "Scrolling: the page does not scroll; whole page %s ms.", a[2]);
    }
    AuGlyphCacheStats gs;
    au_glyph_cache_stats(&gs);
    snprintf(l3, sizeof l3,
             "Memory: heap %ldK free (largest block %ldK), temporary memory %ldK free. Glyph cache: %luK, "
             "%lu glyphs; in these scrolls %lu hits, %lu misses, %lu evictions.",
             heap_free / 1024, heap_block / 1024, temp_free / 1024, (unsigned long)(gs.bytes / 1024),
             (unsigned long)gs.entries, (unsigned long)(small.hits + big.hits + g1.hits - g0.hits),
             (unsigned long)(small.misses + big.misses + g1.misses - g0.misses),
             (unsigned long)(small.evictions + big.evictions + g1.evictions - g0.evictions));
    Str255 p[4];
    mac_pstr(l0, p[0]);
    mac_pstr(l1, p[1]);
    mac_pstr(l2, p[2]);
    mac_pstr(l3, p[3]);
    ParamText(p[0], p[1], p[2], p[3]);
    InitCursor();
    NoteAlert(131, NULL);
}

/* ---- Debug > Benchmark ---- */

void browserwin_benchmark(BrowserWin *b)
{
    SetCursor(*GetCursor(watchCursor));
    AuSurface *s = b->surf;
    AuCanvas *c = au_canvas_new(s);
    const AuChromeLayout *l = &b->bw->layout;
    int w = l->page.x1 - l->page.x0, h = l->page.y1 - l->page.y0;
    au_canvas_translate(c, (float)l->page.x0, (float)l->page.y0);
    uint32_t t0 = au_platform_time_ms();
    for (int i = 0; i < 10; i++)
        au_scene_draw(c, g_fonts, w, h, 0);
    uint32_t warm = (au_platform_time_ms() - t0) * 10 / 10;
    t0 = au_platform_time_ms();
    for (int i = 0; i < 3; i++) {
        au_glyph_cache_clear();
        au_scene_draw(c, g_fonts, w, h, 0);
    }
    uint32_t cold = (au_platform_time_ms() - t0) * 10 / 3;
    t0 = au_platform_time_ms();
    for (int i = 0; i < 20; i++)
        au_fill_rect(c, (AuRect){0.5f, 0.5f, (float)w, (float)h}, AU_RGBA(10, 20, 30, 128));
    uint32_t fill = (au_platform_time_ms() - t0) * 10 / 20;
    au_canvas_translate(c, (float)-l->page.x0, (float)-l->page.y0);
    t0 = au_platform_time_ms();
    for (int i = 0; i < 10; i++)
        au_bw_paint(b->bw, c, (AuIRect){0, 0, l->width, l->page.y0});
    uint32_t chrome = (au_platform_time_ms() - t0) * 10 / 10;
    au_canvas_free(c);
    char l0[96], l1[96], l2[96], l3[96];
    snprintf(l0, sizeof l0, "Scene %dx%d: %lu.%lu ms (cached glyphs), %lu.%lu ms (cold)", w, h,
             (unsigned long)(warm / 10), (unsigned long)(warm % 10), (unsigned long)(cold / 10),
             (unsigned long)(cold % 10));
    snprintf(l1, sizeof l1, "Alpha fill %dx%d: %lu.%lu ms", w, h, (unsigned long)(fill / 10),
             (unsigned long)(fill % 10));
    snprintf(l2, sizeof l2, "Chrome (toolbar and tabs): %lu.%lu ms", (unsigned long)(chrome / 10),
             (unsigned long)(chrome % 10));
    AuFontSetStats fst;
    au_fontset_stats(g_fonts, &fst);
    snprintf(l3, sizeof l3, "Fonts: %u faces in %lu ms (%d bundled, %lu ms); %u loaded, %lu KB",
             (unsigned)au_fontset_count(g_fonts), (unsigned long)mac_fonts_ms(), mac_fonts_bundled(),
             (unsigned long)mac_fonts_bundled_ms(), (unsigned)fst.loaded, (unsigned long)(fst.loaded_bytes / 1024));
    Str255 p[4];
    mac_pstr(l0, p[0]);
    mac_pstr(l1, p[1]);
    mac_pstr(l2, p[2]);
    mac_pstr(l3, p[3]);
    ParamText(p[0], p[1], p[2], p[3]);
    InitCursor();
    invalidate_all(b);
    flush(b);
    NoteAlert(129, NULL);
}
