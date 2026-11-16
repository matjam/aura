/* Shared declarations of the Mac OS 9 shell's modules:
 *
 *   main.c        start-up, the event loop, menus, Apple events, saving
 *   browserwin.c  browser windows (GWorld, chrome, page, address field)
 *   listwin.c     the Bookmarks, History, Downloads and Cookies windows
 *   devwin.c      the developer windows: Page Source, Console, Network,
 *                 Performance, System Status and Report a Problem
 *   sysinfo_mac.c the machine's facts (Gestalt) for those reports
 *   network.c     the network stack (Open Transport, stores, fetchers)
 *   downloads_mac.c  download files, Reveal in Finder
 *   prefswin.c    the Preferences window (Appearance controls)
 *   storage.c     files in the Preferences folder, file: URLs, dialogs
 *   text.c        UTF-8 and Mac Roman, Pascal strings
 *   aboutbox.c    the About box
 *   contextmenu.c contextual menus (Contextual Menu Manager or pop-up)
 *   internetconfig.c  helper applications, download folder, file mappings
 *
 * Every window Aura opens carries a MacWin (its refCon), whose function
 * pointers the event loop calls; that is how several kinds of window
 * share one loop. */
#ifndef AURA_MAC_H
#define AURA_MAC_H

#include <Multiverse.h>

#include "appearance.h"
#include "browser/browser.h"
#include "browser/favicon.h"
#include "browser/context.h"
#include "browser/listview.h"
#include "browser/url.h"

enum {
    kZoomDocProc = 8, /* documentProc with a zoom box */
    kMovableModalProc = 5,
    kScrollBarWidth = 16,
    kGrowBox = 15,
    /* CopyBits mode flag: dither when the screen has fewer colours than
     * the GWorld (256 colours), so gradients do not collapse into bands. */
    kDitherCopy = 64
};

typedef enum { WIN_BROWSER = 1, WIN_BOOKMARKS, WIN_HISTORY, WIN_PREFS, WIN_DOWNLOADS, WIN_COOKIES,
               /* The developer windows (devwin.c), in this order. */
               WIN_SOURCE, WIN_CONSOLE, WIN_NETWORK, WIN_PERF, WIN_STATUS, WIN_REPORT } WinKind;

typedef struct MacWin MacWin;
struct MacWin {
    WinKind kind;
    WindowPtr win;
    void (*update)(MacWin *w);
    void (*activate)(MacWin *w, bool on);
    void (*click)(MacWin *w, EventRecord *ev);
    /* Returns true when the key was used. */
    bool (*key)(MacWin *w, EventRecord *ev);
    void (*idle)(MacWin *w);
    void (*grow)(MacWin *w); /* after the window was resized */
    void (*moved)(MacWin *w); /* after the window was dragged; may be NULL */
    void (*close)(MacWin *w);
    /* Edit menu: 1 undo, 3 cut, 4 copy, 5 paste, 6 clear, 7 select all.
     * Returns true when handled. Item 0 only asks whether a text field
     * has the focus. NULL when the window has no text. */
    bool (*edit)(MacWin *w, int item);
};

MacWin *mac_win_of(WindowPtr w); /* NULL for windows Aura does not own */

static inline WindowPtr mac_next_window(WindowPtr w)
{
    return (WindowPtr)((WindowPeek)w)->nextWindow;
}

/* ---- platform_mac.c ---- */
uint32_t mac_fonts_ms(void);  /* time au_platform_load_fonts took */
uint32_t mac_fonts_bundled_ms(void); /* of which, registering the Fonts folder */
int mac_fonts_bundled(void);  /* faces registered from the Fonts folder */

/* ---- main.c ---- */
extern AuBrowser *g_browser;
extern AuFontSet *g_fonts;
extern bool g_quitting;
extern bool g_in_front; /* the application is not suspended */
void mac_out_of_memory(size_t wanted);
/* The free application heap plus free temporary memory, in KB, and the
 * temporary memory the allocator holds, in bytes (main.c). */
uint32_t mac_memory_free_kb(void);
long mac_heap_temp_bytes(void);
void mac_menus_dirty(void);
/* Brings the menu bar in step with the front window (enables, checks). */
void mac_adjust_menus(void);

/* ---- browserwin.c ---- */
typedef struct BrowserWin BrowserWin;
/* Opens a browser window. With a session window its tabs are restored;
 * else url (NULL: the home page) opens in its one tab. */
BrowserWin *browserwin_new(const char *url, const AuSessionWindow *sw);
BrowserWin *browserwin_front(void); /* the frontmost browser window or NULL */
AuBrowserWindow *browserwin_model(BrowserWin *bw);
void browserwin_focus_field(BrowserWin *bw);
void browserwin_close(BrowserWin *bw);
/* Paints and shows pending damage in every browser window. */
void browserwin_flush_all(void);
/* The AuBrowserHost callbacks. */
void browserwin_host(AuBrowserHost *h);
/* Develop > Tests > Paint Benchmark: times the test scene, a fill and the
 * chrome in the window. */
struct AuPaintBench;
void browserwin_benchmark(BrowserWin *bw, struct AuPaintBench *out);
/* Develop > Run Timing Test: times typing, scrolling and repainting of
 * the active page; false when the window has no tab. */
struct AuTimingTest;
bool browserwin_timing_test(BrowserWin *bw, struct AuTimingTest *out);
/* The window was dragged: its position is kept for the session. */
void browserwin_moved(BrowserWin *bw);
/* Edit > Find: asks for text and finds it in the active page. */
void browserwin_find(BrowserWin *bw);

/* ---- listwin.c ---- */
void listwin_show(WinKind kind); /* WIN_BOOKMARKS, WIN_HISTORY, WIN_DOWNLOADS or WIN_COOKIES */
/* The cookie jar changed outside the Cookies window (Clear Cookies). */
void listwin_cookies_changed(void);
/* Opens url in the front browser window (one is made if needed). */
void mac_open_url(const char *url, bool new_tab);

/* ---- devwin.c ----
 * The developer windows: WIN_SOURCE, WIN_CONSOLE, WIN_NETWORK, WIN_PERF,
 * WIN_STATUS (System Status) or WIN_REPORT (Report a Problem). The first
 * four show the active tab of the frontmost browser window. Showing one
 * that is open brings it forward and reads its data again. */
void devwin_show(WinKind kind);
/* Develop > Run Timing Test: runs it on the front browser window and
 * shows the Performance window, whose report then carries the result. */
void devwin_run_timing(void);
/* Develop > Tests > Paint Benchmark: runs it and shows the System Status
 * window, whose report then carries the result. */
void devwin_run_benchmark(void);
/* Edit > Find, Find Again and Find Previous while a developer window is
 * in front (mw is that window). */
void devwin_find(MacWin *mw);
void devwin_find_again(MacWin *mw, bool forward);
bool devwin_can_find_again(MacWin *mw);

/* ---- prefswin.c ---- */
void prefswin_show(void);

/* ---- storage.c ---- */
bool storage_init(void); /* finds or makes the Aura preferences folder */
void storage_load_all(void);
/* Saves files whose data changed; force saves them even if the change is
 * recent (quitting). */
void storage_save_changed(bool force);
bool storage_session(AuSession *s);
/* Marks Aura as running; returns true when the last run did not quit
 * normally (its mark was still there). */
bool storage_begin_run(void);
/* Removes the mark: call after the last save at quit. */
void storage_end_run(void);
AuTime mac_now(void);
bool mac_read_file_url(const char *url, char **data, size_t *len);
/* Asks for a file with the standard Open dialog; returns a file: URL to
 * free with au_free, or NULL. */
char *mac_choose_file_url(void);
/* File > Save Page As: asks for a file (StandardPutFile) and writes the
 * active page's source to it, type TEXT, creator AURA. */
void mac_save_page(AuBrowserWindow *w);
/* Asks for a file (StandardPutFile, with prompt and the suggested name)
 * and writes text to it, type TEXT, creator AURA. False when cancelled
 * or when writing failed (an alert says so). */
bool mac_save_text(const char *prompt, const char *name, const char *text, size_t len);

/* Files in the Aura preferences folder by name, written safely (a .tmp
 * file swapped in); for network.c's Cookies and HSTS. */
bool storage_read_named(const char *name, char **data, size_t *len);
bool storage_write_named(const char *name, const char *data, size_t len);
void storage_keep_aside_named(const char *name);
/* A folder inside the Aura preferences folder, made when missing. */
bool storage_subfolder(const char *name, short *vref, long *dir);

/* ---- network.c ----
 * The network stack: the entropy pool, Open Transport (started on the
 * first request), the AuNet with its root store, cookie jar, HSTS store
 * and cache, the loader and page fetcher (registered for http and
 * https), and real downloads. */
/* After storage_load_all: creates everything and loads Cookies and HSTS. */
void mac_net_init(void);
/* len bytes from the entropy pool; false until it has gathered enough
 * (and before mac_net_init). */
bool mac_net_random(void *buf, size_t len);
/* Every event WaitNextEvent returns, null events included. */
void mac_net_event(const EventRecord *ev);
/* Once per pass of the event loop. */
void mac_net_idle(void);
/* Network work is under way: the loop sleeps only briefly. */
bool mac_net_busy(void);
/* The preferences changed (Preferences saved, cookie rules edited). */
void mac_net_prefs_changed(void);
/* Saves Cookies and HSTS when they changed (force: now). storage.c calls
 * it from storage_save_changed. */
void mac_net_save(bool force);
/* At quit, after the last page is gone: privacy clearing, the last save,
 * then everything is freed in reverse order. */
void mac_net_quit(void);
/* Closes Open Transport at once, allocating nothing: before ExitToShell
 * on the out-of-memory path, so no notifier outlives the heap. */
void mac_net_emergency_close(void);
void mac_net_clear_cookies(void);
void mac_net_clear_cache(void);
struct AuCookieJar;
struct AuCookieJar *mac_net_cookie_jar(void); /* NULL before init */
/* The rules in the jar were changed by the Cookies window: copied into
 * the cookies.sites preference. */
void mac_net_rules_changed(void);
/* The network sections of the System Status report: entropy, Open
 * Transport, the stores, the HTTP cache and content blocking. */
void mac_net_status_text(AuBuf *out);
/* The developer windows: the network part of the Performance report
 * (conn holds the text h points to), the open connections per host as
 * text, and whether the stack keeps request and response headers for
 * the request log. */
struct AuDevHostInfo;
void mac_net_dev_info(struct AuDevHostInfo *h, AuBuf *conn);
void mac_net_connections_text(AuBuf *out);
void mac_net_trace_headers(bool on);

/* ---- downloads_mac.c ---- */
struct AuDownloadFiles;
/* The download folder: the preference, else Internet Config's, else the
 * desktop. */
bool mac_download_folder(short *vref, long *dir);
void mac_download_files(struct AuDownloadFiles *out);
/* Shows a finished download's file in the Finder. */
bool mac_download_reveal(const AuDownload *d);
/* Whether a name or MIME type looks like an application (for the
 * "warn before downloading applications" preference). */
bool mac_download_is_app(const char *name, const char *mime);

/* ---- sysinfo_mac.c ---- */
struct AuSysInfo;
/* Fills out with the machine's facts (Gestalt), Aura's memory and the
 * clock, for the System Status window and the bug report. */
void mac_sysinfo(struct AuSysInfo *out);

/* ---- aboutbox.c ---- */
void mac_about_box(void);

/* ---- contextmenu.c ---- */
/* Finds the Contextual Menu Manager (call once at start-up). */
void mac_context_init(void);
/* Whether a mouse-down asks for a contextual menu (Control-click). */
bool mac_context_click(const EventRecord *ev);
/* Shows the menu at a global point; returns the chosen item's index in
 * m->items, or -1. */
int mac_context_select(const AuContextMenu *m, Point global);

/* ---- coro_mac.c: coroutines for workers, on the Thread Manager ---- */
const AuCoroOps *mac_coro_ops(void);

/* ---- internetconfig.c ---- */
bool mac_ic_available(void);
/* Opens url with its Internet Config helper (mailto:, ftp:, ...). */
bool mac_ic_launch_url(const char *url);
/* Whether a helper application is set for scheme (lower case, no ':'). */
bool mac_ic_has_helper(const char *scheme);
/* Aura as the http helper; making it the http, https and file helper.
 * Only the Preferences button calls the second. */
bool mac_ic_is_default_browser(void);
bool mac_ic_make_default_browser(void);
/* The user's download folder (Internet Config's DownloadFolder). */
bool mac_ic_download_folder(FSSpec *out);
/* File type, creator and MIME type for a file name, from Internet
 * Config's file mappings (for downloads). */
typedef struct MacFileMap {
    OSType type, creator;
    char mime[64];
    bool binary; /* transfer as binary */
} MacFileMap;
bool mac_ic_map_filename(const char *name, MacFileMap *out);

/* ---- text.c ---- */
/* UTF-8 to a Pascal string in Mac Roman ('?' for characters it lacks),
 * cut to fit. */
void mac_pstr(const char *utf8, Str255 out);
/* Mac Roman bytes to UTF-8 (au_alloc'd, NUL-terminated). */
char *mac_roman_to_utf8(const char *s, size_t n);
/* UTF-8 to Mac Roman bytes (au_alloc'd, NUL-terminated); *n gets the
 * length. */
char *mac_utf8_to_roman(const char *s, size_t *n);
/* Puts UTF-8 text on the clipboard as TEXT (Mac Roman, transliterated,
 * carriage return lines) and 'utxt' (UTF-16). False when it fails. */
bool mac_put_scrap(const char *utf8, size_t n);
/* Sets a menu item's text exactly (no metacharacter parsing). */
void mac_set_item_text(MenuHandle m, short item, const char *utf8);
void mac_append_item(MenuHandle m, const char *utf8);

#endif
