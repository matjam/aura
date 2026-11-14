/* Aura for Mac OS 9: the application shell.
 *
 * A classic Toolbox PowerPC application: menu bar, one resizable document
 * window with a vertical scroll bar, a WaitNextEvent loop, the required
 * Apple events. The engine paints into an offscreen 32-bit GWorld: its
 * pixmap is the engine's surface (big-endian xRGB is the engine's pixel
 * layout, see src/paint/paint.h), so painting needs no copy, and CopyBits
 * puts it on screen at whatever depth the screen has (32, 16 or 8 bits;
 * CopyBits converts and dithers). */
#include <Multiverse.h>

#include <stdio.h>
#include <string.h>

#include "platform/platform.h"
#include "platform/scene.h"

/* Constants the Multiversal headers leave out. */
enum {
    kZoomDocProc = 8,          /* documentProc with a zoom box */
    kSuspendResumeMessage = 1, /* osEvt message >> 24 */
    kResumeFlag = 1,
    kScrollBarWidth = 16,
    kGrowBox = 15
};

enum { kAppleMenu = 128, kFileMenu = 129, kEditMenu = 130 };
enum { kFileScene = 1, kFileFonts = 2, kFileBench = 3, kFileQuit = 5 };

typedef enum { VIEW_SCENE, VIEW_FONTS } View;

static WindowPtr win;
static ControlHandle vscroll;
static GWorldPtr gworld;
static AuSurface *surface;
static AuFontSet *fonts;
static View view = VIEW_SCENE;
static int scroll_y;
static bool quitting;
static uint32_t font_load_ms, last_render_ms;
static Handle reserve; /* released when memory runs out, for the alert */

/* ---- Out of memory ----
 * au_alloc calls this when malloc fails. For now: free the reserve so the
 * Dialog Manager has room, tell the user, and quit.
 *
 * To make it recoverable later: allocate each page's memory from an arena
 * in temporary memory, and setjmp at the top of the event loop before any
 * engine work. The handler then longjmps back to the loop; the loop frees
 * the page's arena (dropping the page: layout, DOM and images go
 * together), shows "page too large" in the window and carries on. The
 * engine must not hold locks or half-built global state across a
 * possible allocation, which the arena design keeps true by keeping all
 * page state in the arena. */
static void out_of_memory(size_t wanted)
{
    if (reserve) {
        DisposeHandle(reserve);
        reserve = NULL;
    }
    Str255 num;
    NumToString((long)wanted, num);
    ParamText(num, (ConstStringPtr) "\p", (ConstStringPtr) "\p", (ConstStringPtr) "\p");
    InitCursor();
    StopAlert(130, NULL);
    ExitToShell();
}

/* ---- Content ---- */

/* The content area: the window minus the scroll bar column. */
static void content_rect(Rect *r)
{
    GetWindowPortBounds(win, r);
    r->right -= kScrollBarWidth - 1;
    if (r->right < r->left)
        r->right = r->left;
}

static int content_height(void)
{
    Rect r;
    content_rect(&r);
    return r.bottom - r.top;
}

static int view_height(void)
{
    if (view == VIEW_FONTS)
        return 40 + 22 * (int)au_fontset_count(fonts) + 20;
    return AU_SCENE_HEIGHT;
}

static void draw_font_list(AuCanvas *c, int w, int h)
{
    au_surface_clear(au_canvas_target(c), AU_RGB(255, 255, 255));
    au_canvas_translate(c, 0, (float)-scroll_y);
    AuFontFace *ui = au_fontset_match(fonts, "system-ui", 400, false);
    char line[160];
    snprintf(line, sizeof line, "%u faces registered, loaded in %lu ms", (unsigned)au_fontset_count(fonts),
             (unsigned long)font_load_ms);
    if (ui)
        au_draw_text(c, ui, 13, 12, 24, line, strlen(line), AU_RGB(0, 0, 0));
    for (size_t i = 0; i < au_fontset_count(fonts); i++) {
        int weight;
        bool italic;
        const char *fam = au_fontset_entry(fonts, i, &weight, &italic);
        AuFontFace *f = au_fontset_match(fonts, fam, weight, italic);
        float y = 52 + 22 * (float)i;
        snprintf(line, sizeof line, "%s %d%s", fam, weight, italic ? " italic" : "");
        if (ui)
            au_draw_text(c, ui, 12, 12, y, line, strlen(line), AU_RGB(80, 80, 80));
        if (f)
            au_draw_text(c, f, 15, 230, y, "The quick brown fox 0123", 24, AU_RGB(0, 0, 0));
    }
    au_canvas_translate(c, 0, (float)scroll_y);
    (void)w;
    (void)h;
}

static void render(void)
{
    if (!surface)
        return;
    AuCanvas *c = au_canvas_new(surface);
    uint32_t t0 = au_platform_time_ms();
    if (view == VIEW_FONTS)
        draw_font_list(c, surface->width, surface->height);
    else
        au_scene_draw(c, fonts, surface->width, surface->height, scroll_y);
    last_render_ms = au_platform_time_ms() - t0;
    au_canvas_free(c);
    /* The render time goes in the title, for measuring in the guest. */
    char title[64];
    snprintf(title, sizeof title, "Aura - %dx%d - %lu ms", surface->width, surface->height,
             (unsigned long)last_render_ms);
    Str255 pt;
    size_t n = strlen(title);
    pt[0] = (unsigned char)n;
    memcpy(pt + 1, title, n);
    SetWTitle(win, pt);
}

/* (Re)creates the GWorld for the content area and points the engine
 * surface at its pixels. */
static void make_gworld(void)
{
    Rect r;
    content_rect(&r);
    OffsetRect(&r, -r.left, -r.top);
    if (r.right < 1)
        r.right = 1;
    if (r.bottom < 1)
        r.bottom = 1;
    if (surface) {
        au_surface_free(surface);
        surface = NULL;
    }
    if (gworld) {
        DisposeGWorld(gworld);
        gworld = NULL;
    }
    /* Temporary memory first, so the window size does not eat into the
     * application heap; the application heap if that fails. */
    QDErr err = NewGWorld(&gworld, 32, &r, NULL, NULL, useTempMem);
    if (err != noErr || !gworld)
        err = NewGWorld(&gworld, 32, &r, NULL, NULL, 0);
    if (err != noErr || !gworld)
        out_of_memory((size_t)(r.right * r.bottom * 4));
    PixMapHandle pm = GetGWorldPixMap(gworld);
    LockPixels(pm);
    Ptr base = GetPixBaseAddr(pm);
    int rowbytes = (**pm).rowBytes & 0x3FFF;
    surface = au_surface_wrap(base, r.right, r.bottom, rowbytes);
}

AuSurface *au_platform_window_surface(void)
{
    return surface;
}

void au_platform_present(int x, int y, int w, int h)
{
    if (!gworld || !win)
        return;
    GrafPtr saved;
    GetPort(&saved);
    SetPort((GrafPtr)win);
    Rect src, dst;
    SetRect(&src, (short)x, (short)y, (short)(x + w), (short)(y + h));
    dst = src;
    /* CopyBits colourises with the port's colours; black on white is the
     * identity. */
    ForeColor(blackColor);
    BackColor(whiteColor);
    /* srcCopy maps each pixel to the nearest colour at 256 colours, which
     * bands gradients. ditherCopy (64) looks better but, tried in the 9.2.2
     * guest, it left the Appearance scroll bar thumb drawn as noise from
     * then on; dithering in the engine is the alternative to try. */
    CopyBits(&((GrafPtr)gworld)->portBits, &((GrafPtr)win)->portBits, &src, &dst, srcCopy, NULL);
    SetPort(saved);
}

static void present_all(void)
{
    if (surface)
        au_platform_present(0, 0, surface->width, surface->height);
}

static void update_scroll_range(void)
{
    int max = view_height() - content_height();
    if (max < 0)
        max = 0;
    if (scroll_y > max)
        scroll_y = max;
    SetControlMaximum(vscroll, (short)max);
    SetControlValue(vscroll, (short)scroll_y);
}

static void place_scroll_bar(void)
{
    Rect r;
    GetWindowPortBounds(win, &r);
    HideControl(vscroll);
    MoveControl(vscroll, (short)(r.right - kScrollBarWidth + 1), -1);
    SizeControl(vscroll, kScrollBarWidth, (short)(r.bottom - r.top - kGrowBox + 2));
    ShowControl(vscroll);
}

static void draw_grow_box(void)
{
    /* Clip so DrawGrowIcon draws only the box, not the scroll bar lines
     * across the content. */
    Rect r, g;
    GetWindowPortBounds(win, &r);
    SetRect(&g, (short)(r.right - kGrowBox), (short)(r.bottom - kGrowBox), r.right, r.bottom);
    ClipRect(&g);
    DrawGrowIcon(win);
    ClipRect(&r);
}

static void resized(void)
{
    SetPort((GrafPtr)win);
    place_scroll_bar();
    make_gworld();
    update_scroll_range();
    render();
    Rect r;
    GetWindowPortBounds(win, &r);
    InvalRect(&r);
}

static void set_view(View v)
{
    view = v;
    scroll_y = 0;
    update_scroll_range();
    render();
    present_all();
}

static void scroll_to(int y)
{
    int max = GetControlMaximum(vscroll);
    if (y < 0)
        y = 0;
    if (y > max)
        y = max;
    if (y == scroll_y)
        return;
    scroll_y = y;
    SetControlValue(vscroll, (short)y);
    render();
    present_all();
}

static pascal void scroll_action(ControlHandle c, short part)
{
    int page = content_height() - 40;
    if (page < 20)
        page = 20;
    switch (part) {
    case inUpButton: scroll_to(scroll_y - 16); break;
    case inDownButton: scroll_to(scroll_y + 16); break;
    case inPageUp: scroll_to(scroll_y - page); break;
    case inPageDown: scroll_to(scroll_y + page); break;
    }
    (void)c;
}

/* ---- Benchmark (File > Benchmark) ---- */

static void benchmark(void)
{
    SetCursor(*GetCursor(watchCursor));
    AuCanvas *c = au_canvas_new(surface);
    int w = surface->width, h = surface->height;
    uint32_t t0 = au_platform_time_ms();
    int n = 10;
    for (int i = 0; i < n; i++)
        au_scene_draw(c, fonts, w, h, 0);
    uint32_t warm = (au_platform_time_ms() - t0) * 10 / n; /* tenths of ms */
    t0 = au_platform_time_ms();
    for (int i = 0; i < 3; i++) {
        au_glyph_cache_clear();
        au_scene_draw(c, fonts, w, h, 0);
    }
    uint32_t cold = (au_platform_time_ms() - t0) * 10 / 3;
    t0 = au_platform_time_ms();
    for (int i = 0; i < 20; i++)
        au_fill_rect(c, (AuRect){0.5f, 0.5f, (float)w, (float)h}, AU_RGBA(10, 20, 30, 128));
    uint32_t fill = (au_platform_time_ms() - t0) * 10 / 20;
    AuFontFace *f = au_fontset_match(fonts, "sans-serif", 400, false);
    static const char para[] = "The quick brown fox jumps over the lazy dog. Sphinx of black quartz, judge my vow.";
    uint32_t text = 0, textcold = 0;
    if (f) {
        au_draw_text(c, f, 13, 0, 20, para, sizeof para - 1, AU_RGB(0, 0, 0));
        t0 = au_platform_time_ms();
        for (int i = 0; i < 100; i++)
            au_draw_text(c, f, 13, 0, 20 + (i % 30) * 15, para, sizeof para - 1, AU_RGB(0, 0, 0));
        text = (au_platform_time_ms() - t0) * 10 / 100;
        t0 = au_platform_time_ms();
        for (int i = 0; i < 10; i++) {
            au_glyph_cache_clear();
            au_draw_text(c, f, 13, 0, 20, para, sizeof para - 1, AU_RGB(0, 0, 0));
        }
        textcold = (au_platform_time_ms() - t0) * 10 / 10;
    }
    au_canvas_free(c);
    char l0[96], l1[96], l2[96], l3[96];
    snprintf(l0, sizeof l0, "Scene %dx%d: %lu.%lu ms (cached glyphs), %lu.%lu ms (cold)", w, h,
             (unsigned long)(warm / 10), (unsigned long)(warm % 10), (unsigned long)(cold / 10),
             (unsigned long)(cold % 10));
    snprintf(l1, sizeof l1, "Alpha fill %dx%d: %lu.%lu ms", w, h, (unsigned long)(fill / 10),
             (unsigned long)(fill % 10));
    snprintf(l2, sizeof l2, "Text 13px, %u glyphs: %lu.%lu ms cached, %lu.%lu ms uncached",
             (unsigned)(sizeof para - 1), (unsigned long)(text / 10), (unsigned long)(text % 10),
             (unsigned long)(textcold / 10), (unsigned long)(textcold % 10));
    snprintf(l3, sizeof l3, "Fonts: %u faces, registered in %lu ms", (unsigned)au_fontset_count(fonts),
             (unsigned long)font_load_ms);
    Str255 p[4];
    const char *ls[4] = {l0, l1, l2, l3};
    for (int i = 0; i < 4; i++) {
        size_t k = strlen(ls[i]);
        if (k > 255)
            k = 255;
        p[i][0] = (unsigned char)k;
        memcpy(p[i] + 1, ls[i], k);
    }
    ParamText(p[0], p[1], p[2], p[3]);
    InitCursor();
    render();
    present_all();
    NoteAlert(129, NULL);
}

/* ---- Menus ---- */

static void do_menu(long choice)
{
    short menu = (short)(choice >> 16), item = (short)(choice & 0xFFFF);
    switch (menu) {
    case kAppleMenu:
        if (item == 1) {
            NoteAlert(128, NULL);
        } else {
            Str255 name;
            GetMenuItemText(GetMenuHandle(kAppleMenu), item, name);
            OpenDeskAcc(name);
        }
        break;
    case kFileMenu:
        if (item == kFileScene)
            set_view(VIEW_SCENE);
        else if (item == kFileFonts)
            set_view(VIEW_FONTS);
        else if (item == kFileBench)
            benchmark();
        else if (item == kFileQuit)
            quitting = true;
        break;
    case kEditMenu:
        SystemEdit((short)(item - 1));
        break;
    }
    HiliteMenu(0);
}

/* ---- Apple events ---- */

static pascal OSErr ae_open_app(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)e, (void)reply, (void)refcon;
    return noErr;
}

static pascal OSErr ae_open_docs(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    /* Opening HTML files comes with the page loader. */
    (void)e, (void)reply, (void)refcon;
    return noErr;
}

static pascal OSErr ae_quit(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)e, (void)reply, (void)refcon;
    quitting = true;
    return noErr;
}

/* ---- Events ---- */

static void activate(bool on)
{
    if (!win)
        return;
    SetPort((GrafPtr)win);
    HiliteControl(vscroll, on ? 0 : 255);
    draw_grow_box();
}

static void mouse_down(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    switch (part) {
    case inMenuBar:
        do_menu(MenuSelect(ev->where));
        break;
    case inSysWindow:
        SystemClick(ev, w);
        break;
    case inDrag:
        DragWindow(w, ev->where, &qd.screenBits.bounds);
        break;
    case inGrow: {
        Rect limits;
        SetRect(&limits, 200, 120, 4000, 4000);
        long size = GrowWindow(w, ev->where, &limits);
        if (size) {
            SizeWindow(w, (short)(size & 0xFFFF), (short)(size >> 16), true);
            resized();
        }
        break;
    }
    case inGoAway:
        if (TrackGoAway(w, ev->where))
            quitting = true; /* closing the only window quits */
        break;
    case inZoomIn:
    case inZoomOut:
        if (TrackBox(w, ev->where, part)) {
            SetPort((GrafPtr)w);
            Rect r;
            GetWindowPortBounds(w, &r);
            EraseRect(&r);
            ZoomWindow(w, part, true);
            resized();
        }
        break;
    case inContent:
        if (w != FrontWindow()) {
            SelectWindow(w);
        } else {
            Point pt = ev->where;
            SetPort((GrafPtr)w);
            GlobalToLocal(&pt);
            ControlHandle c;
            short cpart = FindControl(pt, w, &c);
            if (c == vscroll) {
                if (cpart == inThumb) {
                    TrackControl(c, pt, NULL);
                    int v = GetControlValue(c);
                    scroll_y = -1; /* the control already moved; redraw */
                    scroll_to(v);
                } else if (cpart) {
                    static ControlActionUPP action;
                    if (!action)
                        action = NewControlActionUPP(scroll_action);
                    TrackControl(c, pt, action);
                }
            }
        }
        break;
    }
}

static void key_down(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    /* Command-Option-M: allocate until memory runs out, to test the
     * out-of-memory handler. (Key code 0x2E is M; Option changes the
     * character.) */
    if ((ev->modifiers & cmdKey) && (ev->modifiers & optionKey) && ((ev->message >> 8) & 0xFF) == 0x2E) {
        for (;;)
            (void)au_alloc(1024 * 1024);
    }
    if (ev->modifiers & cmdKey) {
        do_menu(MenuKey(ch));
        return;
    }
    int page = content_height() - 40;
    switch ((unsigned char)ch) {
    case 0x1E: scroll_to(scroll_y - 16); break;   /* up arrow */
    case 0x1F: scroll_to(scroll_y + 16); break;   /* down arrow */
    case 0x0B: scroll_to(scroll_y - page); break; /* page up */
    case 0x0C: scroll_to(scroll_y + page); break; /* page down */
    case 0x01: scroll_to(0); break;               /* home */
    case 0x04: scroll_to(1 << 15); break;         /* end */
    case ' ': scroll_to(scroll_y + page); break;
    }
}

static void handle_event(EventRecord *ev)
{
    switch (ev->what) {
    case mouseDown:
        mouse_down(ev);
        break;
    case keyDown:
    case autoKey:
        key_down(ev);
        break;
    case updateEvt: {
        WindowPtr w = (WindowPtr)ev->message;
        SetPort((GrafPtr)w);
        BeginUpdate(w);
        if (w == win) {
            present_all();
            DrawControls(w);
            draw_grow_box();
        }
        EndUpdate(w);
        break;
    }
    case activateEvt:
        if ((WindowPtr)ev->message == win)
            activate((ev->modifiers & activeFlag) != 0);
        break;
    case osEvt:
        if (((ev->message >> 24) & 0xFF) == kSuspendResumeMessage)
            activate((ev->message & kResumeFlag) != 0);
        break;
    case kHighLevelEvent:
        AEProcessAppleEvent(ev);
        break;
    }
}

int main(void)
{
    MaxApplZone();
    for (int i = 0; i < 8; i++)
        MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    reserve = NewHandle(64 * 1024);
    au_set_oom_handler(out_of_memory);

    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(ae_open_app), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, NewAEEventHandlerUPP(ae_open_docs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, NewAEEventHandlerUPP(ae_open_docs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(ae_quit), 0, false);

    Handle mbar = GetNewMBar(128);
    SetMenuBar(mbar);
    AppendResMenu(GetMenuHandle(kAppleMenu), 'DRVR');
    DisableItem(GetMenuHandle(kEditMenu), 0);
    DrawMenuBar();

    SetCursor(*GetCursor(watchCursor));
    fonts = au_fontset_new();
    uint32_t t0 = au_platform_time_ms();
    au_platform_load_fonts(fonts);
    font_load_ms = au_platform_time_ms() - t0;
    InitCursor();

    /* A 640 x 480 content area below the menu bar, or less on a small
     * screen. */
    Rect screen = qd.screenBits.bounds, wr;
    int cw = 640, ch = 480;
    if (cw > screen.right - 40 - kScrollBarWidth)
        cw = screen.right - 40 - kScrollBarWidth;
    if (ch > screen.bottom - 90)
        ch = screen.bottom - 90;
    SetRect(&wr, 20, 50, (short)(20 + cw + kScrollBarWidth - 1), (short)(50 + ch));
    win = NewCWindow(NULL, &wr, (ConstStringPtr) "\pAura", true, kZoomDocProc, (WindowPtr)-1, true, 0);
    SetPort((GrafPtr)win);
    Rect sr = {0, 0, 10, 10};
    vscroll = NewControl(win, &sr, (ConstStringPtr) "\p", true, 0, 0, 0, scrollBarProc, 0);
    resized();

    while (!quitting) {
        EventRecord ev;
        if (WaitNextEvent(everyEvent, &ev, 30, NULL))
            handle_event(&ev);
    }
    return 0;
}
