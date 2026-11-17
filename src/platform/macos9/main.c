/* Aura for Mac OS 9: start-up, the event loop, menus and Apple events.
 *
 * A classic Toolbox PowerPC application. Each window carries a MacWin
 * (mac.h) whose functions the loop calls, so browser, list and
 * preferences windows share one WaitNextEvent loop. The menu bar is
 * native; the Go, Bookmarks and Window menus are rebuilt from the model
 * when it changes, just before a menu is shown or a command key is
 * looked up. */
#include "mac.h"

#include "core/region.h"

#include <stdio.h>
#include <string.h>

/* Menu IDs. The menus themselves come from the command table
 * (src/browser/commands.h). */
enum { kAppleMenu = 128, kFileMenu, kEditMenu, kViewMenu, kGoMenu, kBookmarksMenu, kWindowMenu, kDevelopMenu = 135,
       kTestsMenu = 190, /* Develop > Tests, a submenu */
       kHelpMenu = -16490 /* the system's Help menu (kHMHelpMenuID) */ };
enum { kFirstFolderMenu = 200, kLastFolderMenu = 255 };

AuBrowser *g_browser;
AuApp *g_app;
AuFontSet *g_fonts;
bool g_quitting;
bool g_in_front = true;

static Handle reserve; /* released when memory runs out, for the alert */
static bool menus_dirty = true;

/* The dynamic menus' entries (commands.h), and for the bookmarks, which
 * menu and item each entry went to. */
static AuMenuList go_list, bm_list, win_list;
typedef struct BmItem {
    short menu, item;
    int entry; /* index in bm_list */
} BmItem;
static BmItem *bm_items;
static int nbm_items, bm_cap;
static short folder_menus_used; /* hierarchical menus currently inserted */
static short help_first; /* the first of Aura's items in the Help menu; 0 when there is no Help menu */

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
    mac_net_emergency_close();
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

/* Platform memory for the pooled allocator (au_heap_enable): slabs and
 * large blocks.
 *
 * They are pointer blocks in the application heap while it has room, and
 * come from temporary memory (the rest of the machine's memory) when it
 * is short: less than HEAP_FLOOR would stay free, or NewPtr fails. So a
 * page that needs more than the partition holds can still run on a
 * machine with memory to spare, and the Toolbox keeps the floor.
 *
 * Temporary memory is taken a TEMP_CHUNK at a time (one locked handle),
 * and blocks smaller than half a chunk are cut from it in order: a large
 * page holds hundreds of the pool's 32 KB slabs, and a handle for each
 * would fill the system's handle tables. A chunk is returned when its last
 * block is freed; space inside a chunk is not reused before that. Larger
 * blocks get a handle each.
 *
 * Every block starts with an 8-byte header that says where it came from. */
#define HEAP_FLOOR (2L * 1024 * 1024)
#define TEMP_CHUNK (256L * 1024)

typedef struct TempChunk {
    Handle h;
    long live;  /* blocks in use */
    long used;  /* bytes handed out, the chunk's own header included */
    struct TempChunk *prev, *next;
} TempChunk;

typedef struct PlatHdr {
    void *owner;  /* NULL: a pointer block; else the Handle, the TempChunk or the long-lived region */
    long chunked; /* 1: owner is a TempChunk; 2: a long-lived block's own Handle; 3: in long_region */
} PlatHdr;

static TempChunk *temp_chunks; /* the newest first: it is the one being filled */
static long temp_heap_bytes;   /* temporary memory the allocator holds */

long mac_heap_temp_bytes(void)
{
    return temp_heap_bytes;
}

/* Temporary memory starts in one zone (71 MB on the 512 MB test machine);
 * beyond it the system makes more zones (16 MB areas when that zone is
 * full). Mac OS 9.2.2 can then hang for good in the Memory Manager: an area
 * ends up linked to itself in its list of areas, and the next one added
 * loops in that list (seen in the guest with YouTube's page after GitHub's,
 * in builds before and after the long-lived zone, and with YouTube's page
 * alone once the long-lived zone used temporary memory; docs/memory.md,
 * "Temporary memory areas"). It did not happen once no zone outside the
 * first emptied: the first handle of Aura's freed in each such zone is
 * shrunk to TEMP_PIN bytes and kept until Aura quits. */
#define TEMP_PIN 16
#define TEMP_PINS 256

static Ptr temp_home_lo, temp_home_hi; /* the zone temporary memory starts in; NULL: unknown */
static THz temp_pinned[TEMP_PINS];     /* the areas that hold a kept handle */
static int temp_npinned;

long mac_temp_pins(void)
{
    return temp_npinned;
}

static void temp_home_init(void)
{
    OSErr err = noErr;
    Handle h = TempNewHandle(TEMP_PIN, &err);
    if (!h || err != noErr)
        return;
    THz z = HandleZone(h);
    if (z && *h > (Ptr)z && *h < z->bkLim) {
        temp_home_lo = (Ptr)z;
        temp_home_hi = z->bkLim;
    }
    DisposeHandle(h);
}

void mac_temp_dispose(Handle h)
{
    Ptr p = *h;
    if (temp_home_hi && (p < temp_home_lo || p >= temp_home_hi) && temp_npinned < TEMP_PINS) {
        THz z = HandleZone(h);
        int i = 0;
        while (i < temp_npinned && temp_pinned[i] != z)
            i++;
        if (z && p > (Ptr)z && p < z->bkLim && i == temp_npinned) {
            HUnlock(h);
            SetHandleSize(h, TEMP_PIN);
            temp_pinned[temp_npinned++] = z;
            return;
        }
    }
    HUnlock(h);
    DisposeHandle(h);
}

static Handle temp_handle(long n)
{
    OSErr err = noErr;
    Handle h = TempNewHandle((Size)n, &err);
    if (!h || err != noErr)
        return NULL;
    HLock(h);
    temp_heap_bytes += n;
    return h;
}

static void *temp_get(size_t n)
{
    long need = (long)((n + sizeof(PlatHdr) + 7) & ~(size_t)7);
    if (need > TEMP_CHUNK / 2) {
        Handle h = temp_handle(need);
        if (!h)
            return NULL;
        PlatHdr *hd = (PlatHdr *)*h;
        hd->owner = h;
        hd->chunked = 0;
        return hd + 1;
    }
    TempChunk *c = temp_chunks;
    if (!c || c->used + need > TEMP_CHUNK) {
        Handle h = temp_handle(TEMP_CHUNK);
        if (!h)
            return NULL;
        c = (TempChunk *)*h;
        c->h = h;
        c->live = 0;
        c->used = (long)((sizeof(TempChunk) + 7) & ~(size_t)7);
        c->prev = NULL;
        c->next = temp_chunks;
        if (temp_chunks)
            temp_chunks->prev = c;
        temp_chunks = c;
    }
    PlatHdr *hd = (PlatHdr *)((char *)c + c->used);
    c->used += need;
    c->live++;
    hd->owner = c;
    hd->chunked = 1;
    return hd + 1;
}

/* The long-lived zone (core.h, "Zones"): the slabs and large blocks of
 * the network stack, the HTTP cache, TLS, fonts, atoms and this shell.
 * They come from temporary memory, so that they never sit among a page's
 * blocks in the application heap and a closed page leaves its space in
 * one piece: slabs (about 32 KB each) are cut from chunks of LONG_CHUNK
 * (an AuRegion, first fit, so freed space is used again), and large
 * blocks, which come and go with transfers (receive buffers, TLS records,
 * bodies), get a handle each, so that they do not leave chunks held by a
 * few slabs. A chunk whose slabs are all freed goes back, except the last
 * one. When temporary memory has no room, the block comes from the heap
 * as a page's does. */
#define LONG_CHUNK (512L * 1024)

static AuRegion long_region;
static Handle long_handles[AU_REGION_CHUNKS]; /* the handle of long_region.chunk[i] */
static long long_temp_bytes;                  /* temporary memory the long-lived zone holds */

long mac_heap_long_temp_bytes(void)
{
    return long_temp_bytes;
}

static Handle long_handle(long n)
{
    OSErr err = noErr;
    Handle h = TempNewHandle((Size)n, &err);
    if (!h || err != noErr)
        return NULL;
    HLock(h);
    long_temp_bytes += n;
    return h;
}

static void long_handle_free(Handle h)
{
    long_temp_bytes -= GetHandleSize(h);
    mac_temp_dispose(h);
}

/* A slab from the chunks, adding one when none has room; NULL when the
 * region has all its chunks or temporary memory has no room. */
static PlatHdr *long_chunked(long need)
{
    PlatHdr *hd = au_region_alloc(&long_region, (size_t)need, false);
    if (!hd && long_region.nchunks < AU_REGION_CHUNKS) {
        Handle h = long_handle(LONG_CHUNK);
        if (!h)
            return NULL;
        if (!au_region_add(&long_region, *h, LONG_CHUNK)) {
            long_handle_free(h);
            return NULL;
        }
        long_handles[long_region.nchunks - 1] = h;
        hd = au_region_alloc(&long_region, (size_t)need, false);
    }
    if (hd) {
        hd->owner = &long_region;
        hd->chunked = 3;
    }
    return hd;
}

static void *long_get(size_t n, bool slab)
{
    long need = (long)((n + sizeof(PlatHdr) + 7) & ~(size_t)7);
    PlatHdr *hd = slab ? long_chunked(need) : NULL;
    if (!hd) {
        Handle h = long_handle(need);
        if (!h)
            return NULL;
        hd = (PlatHdr *)*h;
        hd->owner = h;
        hd->chunked = 2;
    }
    return hd + 1;
}

static void long_release(PlatHdr *hd)
{
    if (hd->chunked == 2) {
        long_handle_free((Handle)hd->owner);
        return;
    }
    au_region_free(&long_region, hd);
    int i;
    while (long_region.nchunks > 1 && (i = au_region_drop_free_chunk(&long_region)) >= 0) {
        long_handle_free(long_handles[i]);
        long_handles[i] = long_handles[long_region.nchunks]; /* the region moved its last chunk to i */
    }
}

static void *heap_get(size_t n, unsigned how)
{
    if ((how & AU_HEAP_ZONE_MASK) == AU_HEAP_ZONE_LONG) {
        void *t = long_get(n, (how & AU_HEAP_GET_SLAB) != 0);
        if (t)
            return t;
    }
    Ptr p = NULL;
    if (FreeMem() >= (long)(n + sizeof(PlatHdr)) + HEAP_FLOOR)
        p = NewPtr((Size)(n + sizeof(PlatHdr)));
    if (!p) {
        void *t = temp_get(n);
        if (t)
            return t;
        p = NewPtr((Size)(n + sizeof(PlatHdr))); /* no temporary memory: the floor goes too */
        if (!p)
            return NULL;
    }
    PlatHdr *hd = (PlatHdr *)p;
    hd->owner = NULL;
    hd->chunked = 0;
    return hd + 1;
}

/* au_heap_set_resize: a large block from NewPtr grows or shrinks where it
 * is (SetPtrSize never moves a nonrelocatable block), so a growing
 * buffer needs no copy and leaves no hole behind. Blocks in temporary
 * memory or the long-lived region are left to the pool's copy. */
static bool heap_resize(void *p, size_t n)
{
    PlatHdr *hd = (PlatHdr *)p - 1;
    if (hd->owner || hd->chunked)
        return false;
    Size want = (Size)(n + sizeof(PlatHdr));
    Size have = GetPtrSize((Ptr)hd);
    if (want > have && FreeMem() < (long)(want - have) + HEAP_FLOOR)
        return false;
    SetPtrSize((Ptr)hd, want);
    return MemError() == noErr;
}

static void heap_release(void *p)
{
    PlatHdr *hd = (PlatHdr *)p - 1;
    if (hd->chunked >= 2) {
        long_release(hd);
    } else if (!hd->owner) {
        DisposePtr((Ptr)hd);
    } else if (!hd->chunked) {
        Handle h = (Handle)hd->owner;
        temp_heap_bytes -= GetHandleSize(h);
        mac_temp_dispose(h);
    } else {
        TempChunk *c = (TempChunk *)hd->owner;
        if (--c->live > 0)
            return;
        if (c == temp_chunks && c->next == NULL) {
            /* the only chunk: keep it and start again at its beginning */
            c->used = (long)((sizeof(TempChunk) + 7) & ~(size_t)7);
            return;
        }
        if (c->prev)
            c->prev->next = c->next;
        else
            temp_chunks = c->next;
        if (c->next)
            c->next->prev = c->prev;
        Handle h = c->h;
        temp_heap_bytes -= TEMP_CHUNK;
        mac_temp_dispose(h);
    }
}

/* What the application can still get: the free application heap and free
 * temporary memory. The Automatic script memory limits follow it
 * (browser/scriptmem.h). */
uint32_t mac_memory_free_kb(void)
{
    long a = FreeMem(), t = (long)TempFreeMem();
    if (a < 0)
        a = 0;
    if (t < 0)
        t = 0;
    return (uint32_t)(a / 1024) + (uint32_t)(t / 1024);
}

/* The script engine asks before it takes more memory (au_memory_low):
 * when less than this would be left for the page's documents, layout and
 * images, the script gets an out-of-memory error instead, which the page
 * survives; au_alloc running out does not. */
#define SCRIPT_RESERVE_KB (3L * 1024)

static bool low_memory(size_t n)
{
    return (long)mac_memory_free_kb() < SCRIPT_RESERVE_KB + (long)(n / 1024);
}

/* Low memory: before au_alloc gives up, the documents kept for Back and
 * Forward go (au_set_release_handler), and when none is left, the
 * background tabs that can be unloaded (browser.h, "Unloaded tabs").
 * Kept documents are hidden and the model leaves alone the tab whose
 * page is being called, so no code on the stack is using what goes. */
static bool release_caches(size_t wanted)
{
    (void)wanted;
    /* Also the web storage areas that are safe on disk. */
    bool freed = g_browser && au_browser_release_memory(g_browser) > 0;
    return mac_net_release_storage() > 0 || freed;
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

/* ---- Commands ----
 * What each command does and when it is enabled is decided in
 * src/browser/commands.c; this part describes the Mac's state to it and
 * opens the native windows and dialogs it asks for. */

static void close_window(WindowPtr w)
{
    MacWin *m = mac_win_of(w);
    if (m && m->close)
        m->close(m);
    else if (w && GetWindowKind(w) < 0)
        CloseDeskAcc(GetWindowKind(w));
}

void mac_command_state(AuCommandState *st)
{
    memset(st, 0, sizeof *st);
    st->browser = g_browser;
    BrowserWin *b = browserwin_front();
    st->window = b ? browserwin_model(b) : NULL;
    WindowPtr fw = FrontWindow();
    MacWin *fm = mac_win_of(fw);
    if (!fw)
        st->front = AU_FRONT_NONE;
    else if (b && fm == (MacWin *)b)
        st->front = AU_FRONT_BROWSER;
    else if (fm && fm->kind >= WIN_SOURCE)
        st->front = AU_FRONT_TOOL;
    else
        st->front = AU_FRONT_OTHER;
    /* A text field with the focus, or a desk accessory, which takes the
     * Edit menu through SystemEdit. */
    st->text_focus = (fw && GetWindowKind(fw) < 0) || (fm && fm->edit && fm->edit(fm, 0));
    st->tool_can_find_again = st->front == AU_FRONT_TOOL && devwin_can_find_again(fm);
    uint8_t k[16]; /* the KeyMap: bit (code & 7) of byte (code >> 3) */
    GetKeys(k);
    /* Key codes 0x38 (Shift) and 0x3A (Option): byte 7, bits 0 and 2. */
    if (k[7] & 0x01)
        st->mods |= AU_MOD_SHIFT;
    if (k[7] & 0x04)
        st->mods |= AU_MOD_OPTION;
}

static BrowserWin *front_browser_win(void)
{
    MacWin *m = mac_win_of(FrontWindow());
    return m && m->kind == WIN_BROWSER ? (BrowserWin *)m : NULL;
}

static AuBrowserWindow *host_new_window(void *ctx, const char *url)
{
    (void)ctx;
    return browserwin_model(browserwin_new(url, NULL));
}

static void host_raise(void *ctx, AuBrowserWindow *w)
{
    (void)ctx;
    SelectWindow(w->platform ? ((MacWin *)w->platform)->win : FrontWindow());
}

static void host_focus_address(void *ctx, AuBrowserWindow *w)
{
    (void)ctx;
    if (w->platform)
        browserwin_focus_field((BrowserWin *)w->platform);
}

static bool host_show_window(void *ctx, AuWindowKind kind)
{
    (void)ctx;
    switch (kind) {
    case AU_WINDOW_ABOUT: mac_about_box(); break;
    case AU_WINDOW_PREFERENCES: prefswin_show(); break;
    case AU_WINDOW_HISTORY: listwin_show(WIN_HISTORY); break;
    case AU_WINDOW_BOOKMARKS: listwin_show(WIN_BOOKMARKS); break;
    case AU_WINDOW_DOWNLOADS: listwin_show(WIN_DOWNLOADS); break;
    case AU_WINDOW_COOKIES: listwin_show(WIN_COOKIES); break;
    case AU_WINDOW_PAGE_SOURCE: devwin_show(WIN_SOURCE); break;
    case AU_WINDOW_CONSOLE: devwin_show(WIN_CONSOLE); break;
    case AU_WINDOW_NETWORK: devwin_show(WIN_NETWORK); break;
    case AU_WINDOW_PERFORMANCE: devwin_show(WIN_PERF); break;
    case AU_WINDOW_SYSTEM_STATUS: devwin_show(WIN_STATUS); break;
    case AU_WINDOW_REPORT: devwin_show(WIN_REPORT); break;
    case AU_WINDOW_TIMING_TEST: devwin_run_timing(); break;
    case AU_WINDOW_PAINT_BENCHMARK: devwin_run_benchmark(); break;
    default: return false;
    }
    return true;
}

static char *host_choose_file_url(void *ctx)
{
    (void)ctx;
    return mac_choose_file_url();
}

static void host_save_page(void *ctx, AuBrowserWindow *w)
{
    (void)ctx;
    mac_save_page(w);
}

static void host_close_front(void *ctx)
{
    (void)ctx;
    close_window(FrontWindow());
}

static void host_find(void *ctx, AuBrowserWindow *w)
{
    (void)ctx;
    if (w && w->platform)
        browserwin_find((BrowserWin *)w->platform);
    else if (!w && mac_win_of(FrontWindow()))
        devwin_find(mac_win_of(FrontWindow()));
}

static void host_tool_find_again(void *ctx, bool forward)
{
    (void)ctx;
    MacWin *m = mac_win_of(FrontWindow());
    if (m)
        devwin_find_again(m, forward);
}

/* The Edit menu's item numbers, which SystemEdit and MacWin.edit take. */
static void host_edit(void *ctx, AuEditAction a)
{
    (void)ctx;
    static const short items[] = {1, 3, 4, 5, 6, 7};
    if (a == AU_EDIT_PASTE_AND_GO) {
        BrowserWin *b = front_browser_win();
        if (b)
            browserwin_paste_and_go(b);
        return;
    }
    if ((int)a < 0 || (size_t)a >= sizeof items / sizeof items[0])
        return;
    short item = items[a];
    MacWin *m = mac_win_of(FrontWindow());
    if (!SystemEdit((short)(item - 1)) && m && m->edit)
        m->edit(m, item);
}

static void host_copy_text(void *ctx, const char *utf8, size_t len)
{
    (void)ctx;
    mac_put_scrap(utf8, len);
}

static void host_select_window(void *ctx, void *window)
{
    (void)ctx;
    SelectWindow((WindowPtr)window);
}

static void host_beep(void *ctx)
{
    (void)ctx;
    SysBeep(1);
}

static void host_quit(void *ctx)
{
    (void)ctx;
    g_quitting = true;
}

static const AuCommandHost command_host = {
    NULL,          host_new_window,      host_raise,     host_focus_address, host_show_window,
    host_choose_file_url, host_save_page, host_close_front, host_find,      host_tool_find_again,
    host_edit,     host_copy_text,       host_select_window, host_beep,     host_quit};

const AuCommandHost *mac_command_host(void)
{
    return &command_host;
}

void mac_open_url(const char *url, bool new_tab)
{
    AuCommandState st;
    mac_command_state(&st);
    au_command_open_url(&st, &command_host, url, new_tab);
}

/* ---- Menus ---- */

static short mac_menu_id(AuMenuId id)
{
    switch (id) {
    case AU_MENU_APPLICATION: return kAppleMenu;
    case AU_MENU_FILE: return kFileMenu;
    case AU_MENU_EDIT: return kEditMenu;
    case AU_MENU_VIEW: return kViewMenu;
    case AU_MENU_GO: return kGoMenu;
    case AU_MENU_BOOKMARKS: return kBookmarksMenu;
    case AU_MENU_DEVELOP: return kDevelopMenu;
    case AU_MENU_TESTS: return kTestsMenu;
    case AU_MENU_WINDOW: return kWindowMenu;
    case AU_MENU_HELP: return kHelpMenu;
    default: return 0;
    }
}

static AuMenuId au_menu_of(short mac_id)
{
    for (int id = AU_MENU_NONE + 1; id < AU_MENU_COUNT; id++)
        if (mac_menu_id((AuMenuId)id) == mac_id)
            return (AuMenuId)id;
    return AU_MENU_NONE;
}

/* The command at item of menu id's fixed items (first is the item
 * number of the first of them), or AU_CMD_NONE. */
static AuCommand menu_command(AuMenuId id, short item, short first)
{
    const AuMenuInfo *mi = au_menu_info(id);
    if (!mi || item < first || item - first >= mi->nitems)
        return AU_CMD_NONE;
    return mi->items[item - first];
}

/* Appends a menu's fixed items. A key equivalent is set for shortcuts
 * of Command alone; the Menu Manager cannot show Shift, so key_down
 * looks those up in the table. */
static void append_items(MenuHandle m, const AuMenuInfo *mi)
{
    for (int i = 0; i < mi->nitems; i++) {
        AuCommand c = mi->items[i];
        if (c == AU_CMD_SEPARATOR) {
            AppendMenu(m, (ConstStr255Param) "\p(-");
            continue;
        }
        const AuCommandInfo *ci = au_command_info(c);
        mac_append_item(m, ci->title);
        short item = CountMItems(m);
        if (ci->shortcut.key && ci->shortcut.mods == AU_SHORTCUT_PRIMARY)
            SetItemCmd(m, item, (CharParameter)ci->shortcut.key);
        if (ci->submenu != AU_MENU_NONE) {
            /* A submenu: the item's command character 0x1B marks it, and
             * its mark is the submenu's ID. */
            const AuMenuInfo *si = au_menu_info(ci->submenu);
            Str255 title;
            mac_pstr(si->title, title);
            MenuHandle sub = NewMenu(mac_menu_id(ci->submenu), title);
            if (!sub) {
                DisableItem(m, item);
                continue;
            }
            append_items(sub, si);
            InsertMenu(sub, -1);
            SetItemCmd(m, item, 0x1B);
            SetItemMark(m, item, (CharParameter)mac_menu_id(ci->submenu));
        }
    }
}

/* The menu bar, from the command table. */
static void build_menu_bar(void)
{
    int n;
    const AuMenuId *bar = au_menu_bar(&n);
    for (int i = 0; i < n; i++) {
        const AuMenuInfo *mi = au_menu_info(bar[i]);
        Str255 title;
        if (bar[i] == AU_MENU_APPLICATION) {
            title[0] = 1;
            title[1] = 0x14; /* the apple */
        } else {
            mac_pstr(mi->title, title);
        }
        MenuHandle m = NewMenu(mac_menu_id(bar[i]), title);
        if (!m)
            mac_out_of_memory(1024);
        append_items(m, mi);
        InsertMenu(m, 0);
        if (bar[i] == AU_MENU_APPLICATION) {
            AppendMenu(m, (ConstStr255Param) "\p(-");
            AppendResMenu(m, 'DRVR');
        }
    }
    /* Aura's Help items go after the system's own. */
    MenuHandle help = NULL;
    if (HMGetHelpMenuHandle(&help) == noErr && help) {
        AppendMenu(help, (ConstStr255Param) "\p(-");
        help_first = (short)(CountMItems(help) + 1);
        append_items(help, au_menu_info(AU_MENU_HELP));
    }
}

static void clear_from(MenuHandle m, short first)
{
    for (short n = CountMItems(m); n >= first; n--)
        DeleteMenuItem(m, n);
}

/* The number of the first dynamic item of a menu. */
static short first_listed(AuMenuId id)
{
    return (short)(au_menu_info(id)->nitems + 1);
}

/* Appends top-level entries of a dynamic list to m. */
static void append_entries(MenuHandle m, const AuMenuList *l)
{
    for (int i = 0; i < l->n; i++) {
        const AuMenuEntry *e = &l->e[i];
        if (e->kind == AU_ENTRY_SEPARATOR) {
            AppendMenu(m, (ConstStr255Param) "\p-");
            continue;
        }
        mac_append_item(m, e->title);
        short item = CountMItems(m);
        if (!e->enabled)
            DisableItem(m, item);
        CheckItem(m, item, e->checked);
    }
}

static void rebuild_go(void)
{
    MenuHandle m = GetMenuHandle(kGoMenu);
    clear_from(m, first_listed(AU_MENU_GO));
    au_menu_list_free(&go_list);
    au_menu_list_history(g_browser, &go_list);
    append_entries(m, &go_list);
}

static void bm_map(short menu, short item, int entry)
{
    if (nbm_items == bm_cap) {
        bm_cap = bm_cap ? bm_cap * 2 : 64;
        bm_items = au_realloc(bm_items, (size_t)bm_cap * sizeof *bm_items);
    }
    bm_items[nbm_items].menu = menu;
    bm_items[nbm_items].item = item;
    bm_items[nbm_items].entry = entry;
    nbm_items++;
}

/* Bookmark folders become hierarchical menus 200 to 255; a folder past
 * them is shown disabled, without its contents. */
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
    clear_from(m, first_listed(AU_MENU_BOOKMARKS));
    au_menu_list_free(&bm_list);
    au_menu_list_bookmarks(g_browser, &bm_list);
    if (!bm_list.n)
        return;
    /* The menu each folder entry opens (NULL when it has none). The list
     * puts every entry after its folder. */
    MenuHandle *sub_of = au_calloc((size_t)bm_list.n * sizeof *sub_of);
    short *id_of = au_calloc((size_t)bm_list.n * sizeof *id_of);
    for (int i = 0; i < bm_list.n; i++) {
        const AuMenuEntry *e = &bm_list.e[i];
        MenuHandle menu = m;
        short mid = kBookmarksMenu;
        if (e->parent >= 0) {
            menu = sub_of[e->parent];
            mid = id_of[e->parent];
            if (!menu)
                continue;
        }
        mac_append_item(menu, e->title);
        short item = CountMItems(menu);
        if (!e->enabled)
            DisableItem(menu, item);
        if (e->kind != AU_ENTRY_FOLDER) {
            bm_map(mid, item, i);
            continue;
        }
        short sid = (short)(kFirstFolderMenu + folder_menus_used);
        if (sid > kLastFolderMenu) {
            DisableItem(menu, item);
            continue;
        }
        Str255 pt;
        mac_pstr(e->title, pt);
        MenuHandle sub = NewMenu(sid, pt);
        if (!sub)
            continue;
        InsertMenu(sub, -1);
        folder_menus_used++;
        SetItemCmd(menu, item, 0x1B);
        SetItemMark(menu, item, (CharParameter)sid);
        sub_of[i] = sub;
        id_of[i] = sid;
    }
    au_free(sub_of);
    au_free(id_of);
}

static void rebuild_windows(void)
{
    MenuHandle m = GetMenuHandle(kWindowMenu);
    clear_from(m, first_listed(AU_MENU_WINDOW));
    AuWindowRef refs[64];
    int n = 0;
    for (WindowPtr w = FrontWindow(); w && n < 64; w = mac_next_window(w)) {
        if (!mac_win_of(w) || !((WindowPeek)w)->visible)
            continue;
        Str255 title;
        GetWTitle(w, title);
        refs[n].title = mac_roman_to_utf8((const char *)title + 1, title[0]);
        refs[n].window = w;
        n++;
    }
    au_menu_list_free(&win_list);
    au_menu_list_windows(refs, n, FrontWindow(), &win_list);
    for (int i = 0; i < n; i++)
        au_free((char *)refs[i].title);
    append_entries(m, &win_list);
}

/* Enables, disables and checks a menu's fixed items (the first at item
 * number first) as the command table says. */
static void apply_states(MenuHandle m, AuMenuId id, short first, const AuCommandState *st)
{
    const AuMenuInfo *mi = au_menu_info(id);
    if (!m || !mi)
        return;
    for (int i = 0; i < mi->nitems; i++) {
        AuCommand c = mi->items[i];
        short item = (short)(first + i);
        if (c == AU_CMD_SEPARATOR)
            continue;
        if (au_command_info(c)->submenu != AU_MENU_NONE) {
            apply_states(GetMenuHandle(mac_menu_id(au_command_info(c)->submenu)), au_command_info(c)->submenu, 1,
                         st);
            continue; /* the item's mark names the submenu */
        }
        if (au_command_enabled(st, c))
            EnableItem(m, item);
        else
            DisableItem(m, item);
        CheckItem(m, item, au_command_checked(st, c));
    }
}

void mac_adjust_menus(void)
{
    if (menus_dirty) {
        rebuild_go();
        rebuild_bookmarks();
        menus_dirty = false;
    }
    rebuild_windows(); /* cheap, and the front window changes often */
    AuCommandState st;
    mac_command_state(&st);
    int n;
    const AuMenuId *bar = au_menu_bar(&n);
    for (int i = 0; i < n; i++)
        apply_states(GetMenuHandle(mac_menu_id(bar[i])), bar[i], 1, &st);
    if (help_first) {
        MenuHandle help = NULL;
        if (HMGetHelpMenuHandle(&help) == noErr && help)
            apply_states(help, AU_MENU_HELP, help_first, &st);
    }
}

static void run_command(AuCommand c)
{
    AuCommandState st;
    mac_command_state(&st);
    au_command_run(&st, &command_host, c);
}

static void run_entry(const AuMenuList *l, int i)
{
    if (i < 0 || i >= l->n)
        return;
    AuCommandState st;
    mac_command_state(&st);
    au_menu_entry_run(&st, &command_host, &l->e[i]);
}

static void do_menu(long choice)
{
    short menu = (short)(choice >> 16), item = (short)(choice & 0xFFFF);
    if (menu == kHelpMenu) {
        /* The system's own items (balloons) are handled by the system. */
        if (help_first)
            run_command(menu_command(AU_MENU_HELP, item, help_first));
        HiliteMenu(0);
        return;
    }
    AuMenuId id = au_menu_of(menu);
    if (id == AU_MENU_APPLICATION && item > au_menu_info(id)->nitems) {
        Str255 name;
        GetMenuItemText(GetMenuHandle(kAppleMenu), item, name);
        OpenDeskAcc(name);
    } else if (id != AU_MENU_NONE && item < first_listed(id)) {
        run_command(menu_command(id, item, 1));
    } else if (id == AU_MENU_GO) {
        run_entry(&go_list, item - first_listed(id));
    } else if (id == AU_MENU_WINDOW) {
        run_entry(&win_list, item - first_listed(id));
    } else if (id == AU_MENU_BOOKMARKS || (menu >= kFirstFolderMenu && menu <= kLastFolderMenu)) {
        for (int i = 0; i < nbm_items; i++) {
            if (bm_items[i].menu == menu && bm_items[i].item == item) {
                run_entry(&bm_list, bm_items[i].entry);
                break;
            }
        }
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
        if (m && m->kind == WIN_BROWSER)
            browserwin_moved((BrowserWin *)m);
        else if (m && m->moved)
            m->moved(m);
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
            if (m && m->kind == WIN_BROWSER)
                browserwin_moved((BrowserWin *)m);
            else if (m && m->moved)
                m->moved(m);
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
        /* Command-arrows, Command-Delete and Command-Home/End edit or move
         * in a text field (the address field or the page's); no menu has
         * them. */
        unsigned char uch = (unsigned char)ch;
        if (b && m == (MacWin *)b && m->key &&
            ((uch >= 0x1C && uch <= 0x1F) || uch == 0x08 || uch == 0x7F || uch == 0x01 || uch == 0x04)) {
            m->key(m, ev);
            return;
        }
        if (ch == '=')
            ch = '+'; /* Command-= is Zoom In without Shift */
        mac_adjust_menus();
        /* The shortcut from the command table. Shift-Command shortcuts
         * (Reopen Closed Tab, Close Window, Find Previous) are looked up
         * with Shift; other keys with Shift run their Command shortcut, as
         * MenuKey, which ignores Shift, did. */
        AuCommandState st;
        mac_command_state(&st);
        unsigned mods = AU_SHORTCUT_PRIMARY | ((ev->modifiers & shiftKey) ? AU_SHORTCUT_SHIFT : 0);
        AuCommand c = au_command_for_key(&st, ch, mods);
        if (c == AU_CMD_NONE && (mods & AU_SHORTCUT_SHIFT))
            c = au_command_for_key(&st, ch, AU_SHORTCUT_PRIMARY);
        if (c != AU_CMD_NONE && au_command_enabled(&st, c)) {
            HiliteMenu(mac_menu_id(au_command_info(c)->menu));
            au_command_run(&st, &command_host, c);
            HiliteMenu(0);
        }
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
                au_app_save(g_app, true);
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

/* The documents kept for Back and Forward are the first thing to go when
 * the heap runs low: one per pass while less than LOW_HEAP is free or no
 * block of LOW_BLOCK is left (a page load needs both). */
#define LOW_HEAP (6L * 1024 * 1024)
#define LOW_BLOCK (2L * 1024 * 1024)

/* The pooled allocator keeps a few empty slabs for reuse; they go back to
 * the heap every TRIM_TICKS while no page is loading, so that slabs a
 * closed page emptied do not stay in the middle of the free space. */
#define TRIM_TICKS (10UL * 60)

static void idle_all(void)
{
    static unsigned long last_trim;
    unsigned long now = TickCount();
    if (now - last_trim >= TRIM_TICKS) {
        last_trim = now;
        if (!au_browser_busy(g_browser))
            au_heap_trim();
    }
    if (au_browser_kept_documents(g_browser, NULL) && (FreeMem() < LOW_HEAP || MaxBlock() < LOW_BLOCK))
        au_browser_evict_document(g_browser);
    mac_net_idle();
    au_downloads_idle(&g_browser->downloads, au_platform_time_ms());
    for (WindowPtr w = FrontWindow(); w;) {
        WindowPtr next = mac_next_window(w);
        MacWin *m = mac_win_of(w);
        if (m && m->idle)
            m->idle(m);
        w = next;
    }
    browserwin_flush_all();
    au_app_save(g_app, false);
}

int main(void)
{
    /* The default stack is small, and a stack that overflows runs into
     * the top of the heap without any error. Give it 1 MB before the
     * heap takes the rest of the partition: script may use 512 KB of it
     * before it gets a RangeError (AuJsSettings.stack_limit), and layout
     * called from script needs up to 180 KB more at its depth limit
     * (L_MAX_DEPTH nested inline blocks at 1.8 KB a level). With 256 KB
     * a page whose script recursed without end froze the machine. */
    SetApplLimit(GetApplLimit() - 1024 * 1024);
    MaxApplZone();
    for (int i = 0; i < 16; i++)
        MoreMasters();
    temp_home_init();
    /* Before the first au_alloc: small blocks come from slabs, not from
     * one NewPtr each (see src/core/heap.c). */
    au_heap_enable(heap_get, heap_release);
    au_heap_set_resize(heap_resize);
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
    au_set_release_handler(release_caches);
    au_set_low_memory_check(low_memory);

    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(ae_open_app), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, NewAEEventHandlerUPP(ae_open_docs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, NewAEEventHandlerUPP(ae_open_docs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(ae_quit), 0, false);
    AEInstallEventHandler('GURL', 'GURL', NewAEEventHandlerUPP(ae_get_url), 0, false);

    build_menu_bar();
    DrawMenuBar();
    mac_context_init();

    SetCursor(*GetCursor(watchCursor));
    g_fonts = au_fontset_new();
    au_platform_load_fonts(g_fonts);
    AuBrowserHost host;
    browserwin_host(&host);
    g_browser = au_browser_new(g_fonts, &host);
    g_browser->site_icons = true; /* favicons in the tabs */
    {
        /* Selections use the highlight colour of the Appearance control
         * panel. */
        RGBColor hl = LMGetHiliteRGB();
        g_browser->highlight = 0xFF000000u | ((uint32_t)(hl.red >> 8) << 16) | ((uint32_t)(hl.green >> 8) << 8) |
                               (uint32_t)(hl.blue >> 8);
    }
    /* The files in the Aura folder (browser/app.c decides which and in
     * what order), then the network stack. */
    AuAppPort port;
    storage_port(&port);
    g_app = au_app_new(g_browser, &port);
    bool unclean = storage_init() && au_app_load(g_app);
    mac_net_init();
    InitCursor();

    /* The last tabs come back when the preference says so, or, after a
     * crash, when the user says so. */
    AuSession session;
    bool restored = false;
    AuAppRestore restore = au_app_session(g_app, unclean, &session);
    bool have = restore != AU_APP_RESTORE_NONE;
    if (restore == AU_APP_RESTORE_ASK && CautionAlert(133, NULL) != 1) {
        au_session_free(&session);
        have = false;
    }
    if (have) {
        for (int i = 0; i < session.nwindows; i++) {
            if (session.windows[i].ntabs) {
                browserwin_new(NULL, &session.windows[i]);
                restored = true;
            }
        }
        au_session_free(&session);
    }
    if (!restored)
        browserwin_new(au_app_start_url(g_app), NULL);

    int held = 0; /* idle passes skipped for queued keys */
    while (!g_quitting) {
        long sleep = 20;
        if (au_browser_busy(g_browser) || au_downloads_busy(&g_browser->downloads) || mac_net_busy())
            sleep = 2;
        else if (any_field_focused())
            sleep = 10; /* caret blink */
        if (held)
            sleep = 0;
        EventRecord ev;
        bool got = WaitNextEvent(everyEvent, &ev, sleep, NULL);
        mac_net_event(&ev); /* every event, null events included: entropy */
        if (got)
            handle_event(&ev);
        /* Keys queued behind this one (auto-repeat while a page paints
         * slowly) are handled before painting, so their scrolls add up to
         * one move; at most a few passes, so the window still updates. */
        EventRecord peek;
        if (got && (ev.what == keyDown || ev.what == autoKey) && held < 8 &&
            EventAvail(keyDownMask | autoKeyMask, &peek)) {
            held++;
            continue;
        }
        held = 0;
        idle_all();
    }

    /* Quit: privacy options, then save everything (the session first, while
     * the windows are still open), and the Running file goes. */
    au_app_quit(g_app);
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
    /* With the pages gone: clear what the privacy options say, save
     * cookies and HSTS, and free the network stack (the entropy pool
     * writes its seed file last). */
    mac_net_quit();
    au_app_free(g_app);
    g_app = NULL;
    UnregisterAppearanceClient();
    return 0;
}
