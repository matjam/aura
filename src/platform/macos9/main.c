/* Aura for Mac OS 9: start-up, the event loop, menus and Apple events.
 *
 * A classic Toolbox PowerPC application. Each window carries a MacWin
 * (mac.h) whose functions the loop calls, so browser, list and
 * preferences windows share one WaitNextEvent loop. The menu bar is
 * native; the Go, Bookmarks and Window menus are rebuilt from the model
 * when it changes, just before a menu is shown or a command key is
 * looked up. */
#include "mac.h"

#include <stdio.h>
#include <string.h>

enum { kAppleMenu = 128, kFileMenu, kEditMenu, kViewMenu, kGoMenu, kBookmarksMenu, kWindowMenu, kDebugMenu = 135 };
enum { kFirstFolderMenu = 200, kLastFolderMenu = 255 };

enum { FILE_NEW_WINDOW = 1, FILE_NEW_TAB, FILE_OPEN_LOCATION, FILE_OPEN_FILE, FILE_CLOSE_TAB = 6, FILE_CLOSE_WINDOW,
       FILE_SAVE_AS = 9, FILE_QUIT = 11 };
enum { EDIT_UNDO = 1, EDIT_CUT = 3, EDIT_COPY, EDIT_PASTE, EDIT_CLEAR, EDIT_SELECT_ALL, EDIT_FIND = 9, EDIT_FIND_AGAIN,
       EDIT_FIND_PREVIOUS, EDIT_PREFS = 13 };
enum { VIEW_RELOAD = 1, VIEW_STOP, VIEW_SOURCE, VIEW_ZOOM_IN = 5, VIEW_ZOOM_OUT, VIEW_ZOOM_RESET, VIEW_TABS_TOP = 9,
       VIEW_TABS_LEFT, VIEW_DEBUG = 12 };
enum { DEBUG_SCENE = 1, DEBUG_FONTS, DEBUG_BENCH, DEBUG_DOWNLOADS, DEBUG_TIMINGS };
enum { GO_BACK = 1, GO_FORWARD, GO_HOME, GO_HISTORY = 5, GO_FIRST_ENTRY = 7 };
enum { BM_ADD = 1, BM_SHOW, BM_FIRST_ITEM = 4 };
enum { WIN_DOWNLOADS_ITEM = 1, WIN_FIRST_WINDOW = 3 };
enum { kGoEntries = 15 };

AuBrowser *g_browser;
AuFontSet *g_fonts;
bool g_quitting;
bool g_in_front = true;

static Handle reserve; /* released when memory runs out, for the alert */
static bool menus_dirty = true;

/* Menu item -> data maps for the dynamic menus. */
static char *go_urls[kGoEntries];
typedef struct BmItem {
    short menu, item;
    uint32_t id;
} BmItem;
static BmItem *bm_items;
static int nbm_items, bm_cap;
static short folder_menus_used; /* hierarchical menus currently inserted */
static WindowPtr window_items[64];
static int nwindow_items;

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
void mac_out_of_memory(size_t wanted)
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

static void oom_handler(size_t wanted)
{
    mac_out_of_memory(wanted);
}

MacWin *mac_win_of(WindowPtr w)
{
    if (!w || GetWindowKind(w) != userKind)
        return NULL;
    return (MacWin *)GetWRefCon(w);
}

void mac_menus_dirty(void)
{
    menus_dirty = true;
}

void mac_open_url(const char *url, bool new_tab)
{
    BrowserWin *b = browserwin_front();
    if (!b) {
        browserwin_new(url, NULL);
        return;
    }
    AuBrowserWindow *w = browserwin_model(b);
    if (new_tab)
        au_bw_new_tab(w, url, true, w->ntabs);
    else
        au_bw_navigate(w, url);
    SelectWindow(w->platform ? ((MacWin *)w->platform)->win : FrontWindow());
}

/* ---- Menus ---- */

static void clear_from(MenuHandle m, short first)
{
    for (short n = CountMItems(m); n >= first; n--)
        DeleteMenuItem(m, n);
}

static void short_title(const char *title, const char *url, char *out, size_t cap)
{
    const char *t = title && *title ? title : url;
    size_t n = strlen(t);
    if (n > 48) {
        size_t k = 45;
        while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80)
            k--;
        snprintf(out, cap, "%.*s...", (int)k, t);
    } else {
        snprintf(out, cap, "%s", t);
    }
}

static void rebuild_go(void)
{
    MenuHandle m = GetMenuHandle(kGoMenu);
    clear_from(m, GO_FIRST_ENTRY - 1);
    for (int i = 0; i < kGoEntries; i++) {
        au_free(go_urls[i]);
        go_urls[i] = NULL;
    }
    AuHistory *h = &g_browser->history;
    if (h->n)
        AppendMenu(m, (ConstStr255Param) "\p-");
    char t[64];
    for (size_t i = 0; i < h->n && i < kGoEntries; i++) {
        short_title(h->e[i].title, h->e[i].url, t, sizeof t);
        mac_append_item(m, t);
        go_urls[i] = au_strndup(h->e[i].url, strlen(h->e[i].url));
    }
}

static void bm_map(short menu, short item, uint32_t id)
{
    if (nbm_items == bm_cap) {
        bm_cap = bm_cap ? bm_cap * 2 : 64;
        bm_items = au_realloc(bm_items, (size_t)bm_cap * sizeof *bm_items);
    }
    bm_items[nbm_items].menu = menu;
    bm_items[nbm_items].item = item;
    bm_items[nbm_items].id = id;
    nbm_items++;
}

static void rebuild_bookmarks(void)
{
    for (short id = kFirstFolderMenu; id < kFirstFolderMenu + folder_menus_used; id++) {
        MenuHandle sub = GetMenuHandle(id);
        if (sub) {
            DeleteMenu(id);
            DisposeMenu(sub);
        }
    }
    folder_menus_used = 0;
    nbm_items = 0;
    MenuHandle m = GetMenuHandle(kBookmarksMenu);
    clear_from(m, BM_FIRST_ITEM);
    /* Breadth-first over folders with an explicit queue: (folder, menu). */
    struct {
        AuBookmark *folder;
        MenuHandle menu;
        short id;
    } queue[kLastFolderMenu - kFirstFolderMenu + 2];
    int qh = 0, qt = 0;
    queue[qt].folder = &g_browser->bookmarks.root;
    queue[qt].menu = m;
    queue[qt].id = kBookmarksMenu;
    qt++;
    while (qh < qt) {
        AuBookmark *folder = queue[qh].folder;
        MenuHandle menu = queue[qh].menu;
        short mid = queue[qh].id;
        qh++;
        for (AuBookmark *b = folder->first; b; b = b->next) {
            char t[64];
            short_title(b->title, b->url, t, sizeof t);
            mac_append_item(menu, t);
            short item = CountMItems(menu);
            if (!b->folder) {
                bm_map(mid, item, b->id);
                continue;
            }
            short sid = (short)(kFirstFolderMenu + folder_menus_used);
            if (sid > kLastFolderMenu || qt >= (int)(sizeof queue / sizeof queue[0])) {
                DisableItem(menu, item);
                continue;
            }
            Str255 pt;
            mac_pstr(b->title, pt);
            MenuHandle sub = NewMenu(sid, pt);
            if (!sub)
                continue;
            InsertMenu(sub, -1);
            folder_menus_used++;
            SetItemCmd(menu, item, 0x1B);
            SetItemMark(menu, item, (CharParameter)sid);
            if (!b->first) {
                AppendMenu(sub, (ConstStr255Param) "\p(empty)");
                DisableItem(sub, 1);
            }
            queue[qt].folder = b;
            queue[qt].menu = sub;
            queue[qt].id = sid;
            qt++;
        }
    }
}

static void rebuild_windows(void)
{
    MenuHandle m = GetMenuHandle(kWindowMenu);
    clear_from(m, WIN_FIRST_WINDOW - 1);
    nwindow_items = 0;
    bool sep = false;
    for (WindowPtr w = FrontWindow(); w && nwindow_items < 64; w = mac_next_window(w)) {
        if (!mac_win_of(w) || !((WindowPeek)w)->visible)
            continue;
        if (!sep) {
            AppendMenu(m, (ConstStr255Param) "\p-");
            sep = true;
        }
        Str255 title;
        GetWTitle(w, title);
        AppendMenu(m, (ConstStr255Param) "\px");
        short item = CountMItems(m);
        if (title[0] == 0) {
            title[0] = 1;
            title[1] = ' ';
        }
        SetMenuItemText(m, item, title);
        CheckItem(m, item, w == FrontWindow());
        window_items[nwindow_items++] = w;
    }
}

/* View Source works for documents that have bytes: files and network
 * pages, not generated about: pages or a source view itself. */
void mac_adjust_menus(void)
{
    if (menus_dirty) {
        rebuild_go();
        rebuild_bookmarks();
        menus_dirty = false;
    }
    rebuild_windows(); /* cheap, and the front window changes often */
    BrowserWin *b = browserwin_front();
    AuBrowserWindow *w = b ? browserwin_model(b) : NULL;
    AuTab *t = w ? au_bw_active_tab(w) : NULL;
    bool browser_front = b && mac_win_of(FrontWindow()) == (MacWin *)b;
    MenuHandle file = GetMenuHandle(kFileMenu), view = GetMenuHandle(kViewMenu), go = GetMenuHandle(kGoMenu),
               bm = GetMenuHandle(kBookmarksMenu);
    if (browser_front)
        EnableItem(file, FILE_CLOSE_TAB);
    else
        DisableItem(file, FILE_CLOSE_TAB);
    if (FrontWindow())
        EnableItem(file, FILE_CLOSE_WINDOW);
    else
        DisableItem(file, FILE_CLOSE_WINDOW);
    const char *src_data;
    size_t src_len;
    if (w && au_bw_page_source(w, &src_data, &src_len))
        EnableItem(file, FILE_SAVE_AS);
    else
        DisableItem(file, FILE_SAVE_AS);
    /* Undo, Cut, Copy, Paste, Clear and Select All need a text field (or
     * a desk accessory, which takes them through SystemEdit). */
    WindowPtr fw = FrontWindow();
    MacWin *fm = mac_win_of(fw);
    bool text = (fw && GetWindowKind(fw) < 0) || (fm && fm->edit && fm->edit(fm, 0));
    MenuHandle edit = GetMenuHandle(kEditMenu);
    for (short i = 1; i <= 7; i++) {
        if (i == 2)
            continue;
        if (text)
            EnableItem(edit, i);
        else
            DisableItem(edit, i);
    }
    /* In a page: Copy needs a selection, Select All a tab. */
    if (!text && browser_front && t) {
        if (au_bw_has_selection(w))
            EnableItem(edit, EDIT_COPY);
        EnableItem(edit, EDIT_SELECT_ALL);
    }
    if (t)
        EnableItem(edit, EDIT_FIND);
    else
        DisableItem(edit, EDIT_FIND);
    for (short i = EDIT_FIND_AGAIN; i <= EDIT_FIND_PREVIOUS; i++) {
        if (t && g_browser->find_text)
            EnableItem(edit, i);
        else
            DisableItem(edit, i);
    }
    if (t && t->loading)
        EnableItem(view, VIEW_STOP);
    else
        DisableItem(view, VIEW_STOP);
    if (t)
        EnableItem(view, VIEW_RELOAD);
    else
        DisableItem(view, VIEW_RELOAD);
    if (w && au_bw_can_view_source(w))
        EnableItem(view, VIEW_SOURCE);
    else
        DisableItem(view, VIEW_SOURCE);
    static const short zoom_items[3] = {VIEW_ZOOM_IN, VIEW_ZOOM_OUT, VIEW_ZOOM_RESET};
    static const int zoom_dirs[3] = {1, -1, 0};
    for (int i = 0; i < 3; i++) {
        if (w && au_bw_can_zoom(w, zoom_dirs[i]))
            EnableItem(view, zoom_items[i]);
        else
            DisableItem(view, zoom_items[i]);
    }
    CheckItem(view, VIEW_TABS_TOP, g_browser->prefs.tab_position == AU_TABS_TOP);
    CheckItem(view, VIEW_TABS_LEFT, g_browser->prefs.tab_position == AU_TABS_LEFT);
    if (t && au_nav_can_go(&t->nav, -1))
        EnableItem(go, GO_BACK);
    else
        DisableItem(go, GO_BACK);
    if (t && au_nav_can_go(&t->nav, 1))
        EnableItem(go, GO_FORWARD);
    else
        DisableItem(go, GO_FORWARD);
    if (t)
        EnableItem(bm, BM_ADD);
    else
        DisableItem(bm, BM_ADD);
}

/* The simulated downloads write no files, so a name is taken when another
 * item in the list has it (a real download checks the folder). */
static bool download_name_taken(void *ctx, const char *name)
{
    AuDownloads *dl = ctx;
    for (int i = 0; i < dl->n; i++)
        if (strcmp(dl->items[i]->filename, name) == 0)
            return true;
    return false;
}

static void start_sim_downloads(void)
{
    static const char *const urls[] = {
        "http://download.info.apple.com/Mac_OS/Mac_OS_9.2.2_Update.smi.bin",
        "http://macintoshgarden.org/files/StuffIt_Expander_5.5.sit",
        "http://www.info-mac.org/net/iCab_Pre2.9.9_PPC.bin",
        "http://example.com/fail/broken_archive.hqx",
    };
    for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++) {
        char name[128], unique[32];
        au_download_name_from_url(urls[i], name, sizeof name);
        au_hfs_sanitize(name, download_name_taken, &g_browser->downloads, unique, sizeof unique);
        au_downloads_add(&g_browser->downloads, urls[i], unique, "application/octet-stream", -1, mac_now());
    }
    listwin_show(WIN_DOWNLOADS);
}

static void close_window(WindowPtr w)
{
    MacWin *m = mac_win_of(w);
    if (m && m->close)
        m->close(m);
    else if (w && GetWindowKind(w) < 0)
        CloseDeskAcc(GetWindowKind(w));
}

static void do_menu(long choice)
{
    short menu = (short)(choice >> 16), item = (short)(choice & 0xFFFF);
    BrowserWin *b = browserwin_front();
    AuBrowserWindow *w = b ? browserwin_model(b) : NULL;
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
        switch (item) {
        case FILE_NEW_WINDOW: browserwin_new(NULL, NULL); break;
        case FILE_NEW_TAB:
            if (w) {
                au_bw_new_tab(w, NULL, true, w->ntabs);
                browserwin_focus_field(b);
            } else {
                browserwin_focus_field(browserwin_new("about:blank", NULL));
            }
            break;
        case FILE_OPEN_LOCATION:
            if (!b)
                b = browserwin_new("about:blank", NULL);
            SelectWindow(((MacWin *)b)->win);
            browserwin_focus_field(b);
            break;
        case FILE_OPEN_FILE: {
            char *url = mac_choose_file_url();
            if (url) {
                mac_open_url(url, false);
                au_free(url);
            }
            break;
        }
        case FILE_CLOSE_TAB:
            if (w && mac_win_of(FrontWindow()) == (MacWin *)b)
                au_bw_close_tab(w, w->active);
            else
                close_window(FrontWindow());
            break;
        case FILE_CLOSE_WINDOW: close_window(FrontWindow()); break;
        case FILE_SAVE_AS:
            if (w)
                mac_save_page(w);
            break;
        case FILE_QUIT: g_quitting = true; break;
        }
        break;
    case kEditMenu: {
        if (item == EDIT_PREFS) {
            prefswin_show();
            break;
        }
        if (item == EDIT_FIND) {
            if (b)
                browserwin_find(b);
            break;
        }
        if (item == EDIT_FIND_AGAIN || item == EDIT_FIND_PREVIOUS) {
            if (w && !au_bw_find_again(w, item == EDIT_FIND_AGAIN))
                SysBeep(1);
            break;
        }
        MacWin *m = mac_win_of(FrontWindow());
        if (!SystemEdit((short)(item - 1)) && m && m->edit)
            m->edit(m, item);
        break;
    }
    case kViewMenu:
        switch (item) {
        case VIEW_RELOAD:
            if (w)
                au_bw_reload(w);
            break;
        case VIEW_STOP:
            if (w)
                au_bw_stop(w);
            break;
        case VIEW_SOURCE:
            if (w)
                au_bw_view_source(w);
            break;
        case VIEW_ZOOM_IN:
        case VIEW_ZOOM_OUT:
        case VIEW_ZOOM_RESET:
            if (w)
                au_bw_zoom(w, item == VIEW_ZOOM_IN ? 1 : item == VIEW_ZOOM_OUT ? -1 : 0);
            break;
        case VIEW_TABS_TOP:
        case VIEW_TABS_LEFT:
            g_browser->prefs.tab_position = item == VIEW_TABS_LEFT ? AU_TABS_LEFT : AU_TABS_TOP;
            au_browser_prefs_changed(g_browser);
            break;
        }
        break;
    case kDebugMenu:
        switch (item) {
        case DEBUG_SCENE: mac_open_url("about:scene", false); break;
        case DEBUG_FONTS: mac_open_url("about:fonts", false); break;
        case DEBUG_BENCH:
            if (b)
                browserwin_benchmark(b);
            break;
        case DEBUG_DOWNLOADS: start_sim_downloads(); break;
        case DEBUG_TIMINGS:
            if (b)
                browserwin_page_timings(b);
            break;
        }
        break;
    case kGoMenu:
        if (item == GO_BACK && w)
            au_bw_back(w);
        else if (item == GO_FORWARD && w)
            au_bw_forward(w);
        else if (item == GO_HOME) {
            if (w)
                au_bw_home(w);
            else
                browserwin_new(NULL, NULL);
        } else if (item == GO_HISTORY)
            listwin_show(WIN_HISTORY);
        else if (item >= GO_FIRST_ENTRY && item < GO_FIRST_ENTRY + kGoEntries && go_urls[item - GO_FIRST_ENTRY]) {
            char *u = au_strndup(go_urls[item - GO_FIRST_ENTRY], strlen(go_urls[item - GO_FIRST_ENTRY]));
            mac_open_url(u, false);
            au_free(u);
        }
        break;
    case kWindowMenu:
        if (item == WIN_DOWNLOADS_ITEM)
            listwin_show(WIN_DOWNLOADS);
        else if (item >= WIN_FIRST_WINDOW && item - WIN_FIRST_WINDOW < nwindow_items)
            SelectWindow(window_items[item - WIN_FIRST_WINDOW]);
        break;
    default:
        if (menu == kBookmarksMenu && item == BM_ADD) {
            if (w)
                au_bw_bookmark_page(w, false);
        } else if (menu == kBookmarksMenu && item == BM_SHOW) {
            listwin_show(WIN_BOOKMARKS);
        } else {
            for (int i = 0; i < nbm_items; i++) {
                if (bm_items[i].menu == menu && bm_items[i].item == item) {
                    AuBookmark *bk = au_bookmark_find_id(&g_browser->bookmarks, bm_items[i].id);
                    if (bk) {
                        char *u = au_strndup(bk->url, strlen(bk->url));
                        mac_open_url(u, false);
                        au_free(u);
                    }
                    break;
                }
            }
        }
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
    (void)reply, (void)refcon;
    AEDescList docs;
    if (AEGetParamDesc((AppleEvent *)e, keyDirectObject, typeAEList, &docs) != noErr)
        return errAEEventNotHandled;
    long n = 0;
    if (AECountItems(&docs, &n) == noErr) {
        for (long i = 1; i <= n; i++) {
            FSSpec spec;
            AEKeyword kw;
            DescType type;
            Size size;
            if (AEGetNthPtr(&docs, i, typeFSS, &kw, &type, &spec, sizeof spec, &size) != noErr)
                continue;
            /* Build the full path by walking up the parents. */
            char path[1024];
            size_t k = spec.name[0];
            memcpy(path, spec.name + 1, k);
            path[k] = 0;
            long dir = spec.parID;
            for (int depth = 0; depth < 64; depth++) {
                CInfoPBRec pb;
                Str255 name;
                memset(&pb, 0, sizeof pb);
                pb.dirInfo.ioNamePtr = name;
                pb.dirInfo.ioVRefNum = spec.vRefNum;
                pb.dirInfo.ioFDirIndex = -1;
                pb.dirInfo.ioDrDirID = dir;
                if (PBGetCatInfoSync(&pb) != noErr)
                    break;
                size_t nl = name[0], cur = strlen(path);
                if (nl + cur + 2 > sizeof path)
                    break;
                memmove(path + nl + 1, path, cur + 1);
                memcpy(path, name + 1, nl);
                path[nl] = ':';
                if (dir == 2)
                    break;
                dir = pb.dirInfo.ioDrParID;
            }
            char *utf8 = mac_roman_to_utf8(path, strlen(path));
            char *url = au_url_from_path(utf8, ':');
            mac_open_url(url, true);
            au_free(url);
            au_free(utf8);
        }
    }
    AEDisposeDesc(&docs);
    return noErr;
}

/* GURL ('GURL' 'GURL'): another application (through Internet Config,
 * or a script) asks Aura to open a URL, the direct object as text. It
 * opens in a new tab of the front window, or in a new window when the
 * preference says so. */
static pascal OSErr ae_get_url(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)reply, (void)refcon;
    DescType type;
    Size size = 0;
    if (AESizeOfParam((AppleEvent *)e, keyDirectObject, &type, &size) != noErr || size <= 0 || size > 32767)
        return errAEEventNotHandled;
    char *buf = au_alloc((size_t)size + 1);
    Size actual = 0;
    OSErr err = AEGetParamPtr((AppleEvent *)e, keyDirectObject, typeChar, &type, buf, size, &actual);
    if (err == noErr && actual > 0 && actual <= size) {
        char *u = mac_roman_to_utf8(buf, (size_t)actual);
        /* Trim the spaces and line breaks scripts leave around it. */
        char *s = u;
        while (*s == ' ' || *s == '\r' || *s == '\n' || *s == '\t')
            s++;
        size_t n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t'))
            s[--n] = 0;
        if (*s) {
            if (g_browser->prefs.external_new_window)
                browserwin_new(s, NULL);
            else
                mac_open_url(s, true);
        }
        au_free(u);
    }
    au_free(buf);
    return err == noErr ? noErr : errAEEventNotHandled;
}

static pascal OSErr ae_quit(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)e, (void)reply, (void)refcon;
    g_quitting = true;
    return noErr;
}

/* ---- Events ---- */

static void mouse_down(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    MacWin *m = mac_win_of(w);
    switch (part) {
    case inMenuBar:
        mac_adjust_menus();
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
        SetRect(&limits, 300, 200, 4000, 4000);
        long size = GrowWindow(w, ev->where, &limits);
        if (size) {
            SizeWindow(w, (short)(size & 0xFFFF), (short)(size >> 16), true);
            if (m && m->grow)
                m->grow(m);
        }
        break;
    }
    case inGoAway:
        if (TrackGoAway(w, ev->where))
            close_window(w);
        break;
    case inZoomIn:
    case inZoomOut:
        if (TrackBox(w, ev->where, part)) {
            SetPort((GrafPtr)w);
            EraseRect(&w->portRect);
            ZoomWindow(w, part, true);
            if (m && m->grow)
                m->grow(m);
        }
        break;
    case inContent:
        if (w != FrontWindow())
            SelectWindow(w);
        else if (m && m->click)
            m->click(m, ev);
        break;
    }
}

static void key_down(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    int code = (int)((ev->message >> 8) & 0xFF);
    MacWin *m = mac_win_of(FrontWindow());
    if (ev->modifiers & cmdKey) {
        /* Command-Option-M: allocate until memory runs out, to test the
         * out-of-memory handler. (Key code 0x2E is M; Option changes the
         * character.) */
        if ((ev->modifiers & optionKey) && code == 0x2E) {
            for (;;)
                (void)au_alloc(1024 * 1024);
        }
        BrowserWin *b = browserwin_front();
        if (ch >= '1' && ch <= '9' && b && m == (MacWin *)b) {
            AuBrowserWindow *w = browserwin_model(b);
            int i = ch == '9' ? w->ntabs - 1 : ch - '1'; /* Command-9 is the last tab */
            au_bw_select_tab(w, i);
            return;
        }
        /* Command-Shift-W closes the window; so does Command-W in a window
         * without tabs (Close Tab is greyed there). */
        if ((ch == 'w' || ch == 'W') && ((ev->modifiers & shiftKey) || !(b && m == (MacWin *)b))) {
            close_window(FrontWindow());
            return;
        }
        if (ch == '=')
            ch = '+'; /* Command-= is Zoom In without Shift */
        mac_adjust_menus();
        /* MenuKey ignores Shift: Shift-Command-G is Find Previous. */
        if ((ch == 'g' || ch == 'G') && (ev->modifiers & shiftKey)) {
            if (b && g_browser->find_text) {
                HiliteMenu(kEditMenu);
                do_menu(((long)kEditMenu << 16) | EDIT_FIND_PREVIOUS);
            }
            return;
        }
        long choice = MenuKey(ch);
        if (choice >> 16)
            do_menu(choice);
        return;
    }
    if (m && m->key)
        m->key(m, ev);
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
        MacWin *m = mac_win_of(w);
        if (m && m->update) {
            m->update(m);
        } else {
            BeginUpdate(w);
            EndUpdate(w);
        }
        break;
    }
    case activateEvt: {
        MacWin *m = mac_win_of((WindowPtr)ev->message);
        if (m && m->activate)
            m->activate(m, (ev->modifiers & activeFlag) != 0);
        menus_dirty = true;
        break;
    }
    case osEvt:
        if (((ev->message >> 24) & 0xFF) == 1 /* suspendResumeMessage */) {
            g_in_front = (ev->message & 1) != 0; /* resumeFlag */
            MacWin *m = mac_win_of(FrontWindow());
            if (m && m->activate)
                m->activate(m, g_in_front);
            if (!g_in_front)
                storage_save_changed(true);
        }
        break;
    case kHighLevelEvent:
        AEProcessAppleEvent(ev);
        break;
    }
}

static bool any_field_focused(void)
{
    MacWin *m = mac_win_of(FrontWindow());
    return m && (m->kind == WIN_BROWSER || m->kind == WIN_PREFS);
}

static void idle_all(void)
{
    au_downloads_idle(&g_browser->downloads, au_platform_time_ms());
    for (WindowPtr w = FrontWindow(); w;) {
        WindowPtr next = mac_next_window(w);
        MacWin *m = mac_win_of(w);
        if (m && m->idle)
            m->idle(m);
        w = next;
    }
    browserwin_flush_all();
    storage_save_changed(false);
}

int main(void)
{
    /* The default stack is small, and a stack that overflows runs into
     * the top of the heap without any error. Give it 256 KB before the
     * heap takes the rest of the partition. */
    SetApplLimit(GetApplLimit() - 256 * 1024);
    MaxApplZone();
    for (int i = 0; i < 16; i++)
        MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    RegisterAppearanceClient(); /* Platinum controls; harmless if it fails */

    reserve = NewHandle(64 * 1024);
    au_set_oom_handler(oom_handler);

    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(ae_open_app), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, NewAEEventHandlerUPP(ae_open_docs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, NewAEEventHandlerUPP(ae_open_docs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(ae_quit), 0, false);
    AEInstallEventHandler('GURL', 'GURL', NewAEEventHandlerUPP(ae_get_url), 0, false);

    Handle mbar = GetNewMBar(128);
    if (!mbar)
        mac_out_of_memory(1024);
    SetMenuBar(mbar);
    AppendResMenu(GetMenuHandle(kAppleMenu), 'DRVR');
    MenuHandle debug = GetMenu(kDebugMenu);
    if (debug) {
        InsertMenu(debug, -1);
        MenuHandle view = GetMenuHandle(kViewMenu);
        SetItemCmd(view, VIEW_DEBUG, 0x1B);
        SetItemMark(view, VIEW_DEBUG, (CharParameter)kDebugMenu);
    }
    DrawMenuBar();
    mac_context_init();

    SetCursor(*GetCursor(watchCursor));
    g_fonts = au_fontset_new();
    au_platform_load_fonts(g_fonts);
    AuBrowserHost host;
    browserwin_host(&host);
    g_browser = au_browser_new(g_fonts, &host);
    {
        /* Selections use the highlight colour of the Appearance control
         * panel. */
        RGBColor hl = LMGetHiliteRGB();
        g_browser->highlight = 0xFF000000u | ((uint32_t)(hl.red >> 8) << 16) | ((uint32_t)(hl.green >> 8) << 8) |
                               (uint32_t)(hl.blue >> 8);
    }
    if (storage_init())
        storage_load_all();
    InitCursor();

    AuSession session;
    bool restored = false;
    if (g_browser->prefs.startup == AU_STARTUP_RESTORE && storage_session(&session)) {
        for (int i = 0; i < session.nwindows; i++) {
            if (session.windows[i].ntabs) {
                browserwin_new(NULL, &session.windows[i]);
                restored = true;
            }
        }
        au_session_free(&session);
    }
    if (!restored)
        browserwin_new(g_browser->prefs.startup == AU_STARTUP_BLANK ? "about:blank" : NULL, NULL);

    while (!g_quitting) {
        long sleep = 20;
        if (au_browser_busy(g_browser) || au_downloads_busy(&g_browser->downloads))
            sleep = 2;
        else if (any_field_focused())
            sleep = 10; /* caret blink */
        EventRecord ev;
        if (WaitNextEvent(everyEvent, &ev, sleep, NULL))
            handle_event(&ev);
        idle_all();
    }

    /* Quit: privacy options, then save everything (the session first, while
     * the windows are still open). */
    if (g_browser->prefs.clear_history_on_quit)
        au_history_clear(&g_browser->history);
    storage_save_changed(true);
    /* Dispose of the windows (their controls, GWorlds and TextEdit
     * records) and leave the Appearance Manager tidily. */
    WindowPtr w = FrontWindow();
    while (w) {
        WindowPtr next = mac_next_window(w);
        MacWin *m = mac_win_of(w);
        if (m && m->close)
            m->close(m);
        w = next;
    }
    UnregisterAppearanceClient();
    return 0;
}
