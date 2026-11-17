/* The secondary windows' shared entry points (windows_cocoa.h): the host
 * they share, and showing a window by its AuWindowKind.
 *
 * Each window's functions have a weak placeholder here that does nothing,
 * so the application links before that window exists; the window's own
 * file (prefswin_cocoa.m, listwin_cocoa.m, devwin_cocoa.m, about_cocoa.m)
 * defines the real ones, which replace them. A kind whose window is not
 * built reports false, so the model falls back where it can (Page Source
 * opens a view-source: tab). */
#import "windows_cocoa.h"

#define WEAK __attribute__((weak))

AuCocoaWindowsHost au_cocoa_wh;

/* Each window file's own start and stop, called from init and shutdown. */
WEAK void au_cocoa_prefs_shutdown(void) {}
WEAK void au_cocoa_lists_shutdown(void) {}
WEAK void au_cocoa_devwin_shutdown(void) {}
WEAK bool au_cocoa_prefs_built(void) { return false; }
WEAK bool au_cocoa_lists_built(void) { return false; }
WEAK bool au_cocoa_devwin_built(void) { return false; }
WEAK bool au_cocoa_about_built(void) { return false; }

void au_cocoa_windows_init(const AuCocoaWindowsHost *host)
{
    au_cocoa_wh = *host;
}

void au_cocoa_windows_shutdown(void)
{
    au_cocoa_prefs_shutdown();
    au_cocoa_lists_shutdown();
    au_cocoa_devwin_shutdown();
}

bool au_cocoa_window_kind(AuWindowKind kind)
{
    switch (kind) {
    case AU_WINDOW_ABOUT:
        if (!au_cocoa_about_built())
            return false;
        au_cocoa_about_show();
        return true;
    case AU_WINDOW_PREFERENCES:
        if (!au_cocoa_prefs_built())
            return false;
        au_cocoa_prefs_show();
        return true;
    case AU_WINDOW_HISTORY: if (!au_cocoa_lists_built()) return false; au_cocoa_list_show(AU_LIST_HISTORY); return true;
    case AU_WINDOW_BOOKMARKS: if (!au_cocoa_lists_built()) return false; au_cocoa_list_show(AU_LIST_BOOKMARKS); return true;
    case AU_WINDOW_DOWNLOADS: if (!au_cocoa_lists_built()) return false; au_cocoa_list_show(AU_LIST_DOWNLOADS); return true;
    case AU_WINDOW_COOKIES: if (!au_cocoa_lists_built()) return false; au_cocoa_list_show(AU_LIST_COOKIES); return true;
    case AU_WINDOW_PAGE_SOURCE: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_show(AU_DEVWIN_SOURCE); return true;
    case AU_WINDOW_CONSOLE: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_show(AU_DEVWIN_CONSOLE); return true;
    case AU_WINDOW_NETWORK: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_show(AU_DEVWIN_NETWORK); return true;
    case AU_WINDOW_PERFORMANCE: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_show(AU_DEVWIN_PERF); return true;
    case AU_WINDOW_SYSTEM_STATUS: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_show(AU_DEVWIN_STATUS); return true;
    case AU_WINDOW_REPORT: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_show(AU_DEVWIN_REPORT); return true;
    case AU_WINDOW_TIMING_TEST: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_run_timing(); return true;
    case AU_WINDOW_PAINT_BENCHMARK: if (!au_cocoa_devwin_built()) return false; au_cocoa_devwin_run_benchmark(); return true;
    }
    return false;
}

/* ---- Placeholders, replaced by each window's file ---- */

WEAK void au_cocoa_prefs_show(void) {}
WEAK bool au_cocoa_prefs_is_open(void) { return false; }
WEAK void au_cocoa_list_show(AuListKind kind) { (void)kind; }
WEAK void au_cocoa_list_touch(AuListKind kind) { (void)kind; }
WEAK void au_cocoa_devwin_show(AuDevWinKind kind) { (void)kind; }
WEAK void au_cocoa_devwin_run_timing(void) {}
WEAK void au_cocoa_devwin_run_benchmark(void) {}
WEAK bool au_cocoa_devwin_owns(NSWindow *w) { (void)w; return false; }
WEAK bool au_cocoa_devwin_edit(NSWindow *w, AuEditAction action) { (void)w; (void)action; return false; }
WEAK void au_cocoa_devwin_find(NSWindow *w) { (void)w; }
WEAK void au_cocoa_devwin_find_again(NSWindow *w, bool forward) { (void)w; (void)forward; }
WEAK bool au_cocoa_devwin_can_find_again(NSWindow *w) { (void)w; return false; }
WEAK void au_cocoa_about_show(void) {}
