/* The macOS port's secondary windows: Preferences, the four lists
 * (Bookmarks, History, Downloads, Cookies), the six developer windows
 * (Page Source, Console, Network, Performance, System Status, Report a
 * Problem) and the About box.
 *
 * Each window is a view of a model in src/browser (prefs_ui.h, lists.h,
 * devwin_model.h, credits.h). These files hold no browser state and
 * decide nothing: they build controls from the model's descriptions, turn
 * AppKit events into the model's calls and give the model what it asks of
 * the system (the pasteboard, a file panel, a line of text). At most one
 * window of each kind is open; showing a kind that is open brings it
 * forward.
 *
 * What the caller (the application, src/platform/cocoa) must do:
 *
 *   1. Once, after the AuBrowser, the fonts and the AuApp exist and
 *      before any window is shown, fill an AuCocoaWindowsHost and call
 *      au_cocoa_windows_init. The struct is copied; the pointers in it
 *      must stay valid until au_cocoa_windows_shutdown.
 *   2. Show windows from the command host (AuCommandHost.show_window):
 *      au_cocoa_window_kind maps an AuWindowKind to the right window.
 *   3. Route the Edit and Find commands to a developer window when it is
 *      the key window (au_cocoa_devwin_owns): au_cocoa_devwin_edit,
 *      au_cocoa_devwin_find, au_cocoa_devwin_find_again and
 *      au_cocoa_devwin_can_find_again.
 *   4. After the cookie store or the history changed outside its window:
 *      au_cocoa_list_touch. The lists also poll their models every 250 ms
 *      (au_list_stale), so this only makes the change show sooner.
 *   5. At quit, before freeing the browser: au_cocoa_windows_shutdown,
 *      which closes every window (an open Preferences window is
 *      cancelled) and frees their models. */
#ifndef AURA_COCOA_WINDOWS_H
#define AURA_COCOA_WINDOWS_H

#import <AppKit/AppKit.h>

#include "browser/app.h"
#include "browser/browser.h"
#include "browser/commands.h"
#include "browser/devtools.h"
#include "browser/devwin_model.h"
#include "browser/lists.h"
#include "browser/prefs_ui.h"
#include "browser/sysinfo.h"
#include "paint/font.h"

typedef struct AuCocoaWindowsHost {
    AuBrowser *browser; /* required */
    AuFontSet *fonts;   /* required: the developer windows paint with it */
    AuApp *aura_app;    /* the application services: the cookie jar, web
                         * storage, clearing cookies and the cache, the
                         * network's status; NULL disables those parts */
    void *ctx;          /* passed back to every callback below */

    /* The front browser window's model and its active page, or NULL.
     * Required. */
    AuBrowserWindow *(*front_window)(void *ctx);
    AuPage *(*front_page)(void *ctx);
    /* The front browser window's NSWindow (the parent of sheets), or nil.
     * May be NULL. */
    NSWindow *(*front_ns_window)(void *ctx);

    /* Opens url in the front browser window, or in a new tab (a new
     * window when none is open). Required. */
    void (*open_url)(void *ctx, const char *url, bool new_tab);
    /* Shows a finished download's file in the Finder. false when it
     * cannot. May be NULL: Reveal reports that it failed. */
    bool (*reveal_download)(void *ctx, const AuDownload *d);

    /* Preferences were applied: what (AU_PREF_CHANGED_TABS: lay the
     * browser windows out again; AU_PREF_CHANGED_APPEARANCE: theme the
     * chrome) for a live change; 0 after OK. After OK the caller
     * reconfigures the network (au_app_prefs_changed). May be NULL. */
    void (*prefs_changed)(void *ctx, unsigned what, bool committed);
    /* The blocking switch or lists changed. May be NULL. */
    void (*blocking_changed)(void *ctx);
    /* The blocking manager, for the Blocking pane's list rows; NULL
     * before it runs. May be NULL. */
    AuBlockManager *(*block_manager)(void *ctx);
    /* Whether Aura is the system's default browser, and making it so.
     * Either may be NULL: the button is unavailable. */
    AuDefaultBrowser (*default_browser)(void *ctx);
    bool (*make_default_browser)(void *ctx);
    /* Free memory in KB for Automatic script memory (0: unknown). May be
     * NULL. */
    uint32_t (*memory_free_kb)(void *ctx);

    /* The developer windows' facts about the system, as
     * AuDevWinHost.host_info, .connections and .status_sections. Each may
     * be NULL. When status_sections is NULL and aura_app is set, the
     * network's status (au_app_net_status) is shown instead. */
    void (*host_info)(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn);
    void (*connections)(void *ctx, AuBuf *out);
    void (*status_sections)(void *ctx, AuBuf *out);
    /* Runs the timing test on the front page / the paint benchmark in the
     * front window. false when there is no front window. Either may be
     * NULL. */
    bool (*timing_test)(void *ctx, AuTimingTest *out);
    bool (*benchmark)(void *ctx, AuPaintBench *out);
    /* From now on the network keeps request and response headers. May be
     * NULL. */
    void (*record_headers)(void *ctx);
    /* A window opened, closed or changed its title: menus that list
     * windows should be rebuilt. May be NULL. */
    void (*windows_changed)(void *ctx);
    /* Runs a modal session's begin and end around a dialog the windows
     * wait for, so the browser model is not entered while it runs. May be
     * NULL. */
    void (*modal_begin)(void *ctx);
    void (*modal_end)(void *ctx);

    /* The words the Preferences window uses for this system, as
     * AuPrefsHost. NULL for the defaults. */
    const char *machine, *settings_name, *folder_example, *system_downloads, *no_folder;
} AuCocoaWindowsHost;

/* The host the windows share (winutil_cocoa.m), copied by
 * au_cocoa_windows_init. */
extern AuCocoaWindowsHost au_cocoa_wh;

void au_cocoa_windows_init(const AuCocoaWindowsHost *host);
void au_cocoa_windows_shutdown(void);

/* Shows the window of an AuWindowKind; false for a kind these files do
 * not make. */
bool au_cocoa_window_kind(AuWindowKind kind);

/* ---- Preferences (prefswin_cocoa.m) ---- */
void au_cocoa_prefs_show(void);
bool au_cocoa_prefs_is_open(void);
/* Cancels an open edit session and closes the window (from
 * au_cocoa_windows_shutdown). */
void au_cocoa_prefs_shutdown(void);
bool au_cocoa_prefs_built(void);

/* ---- Lists (listwin_cocoa.m) ---- */
void au_cocoa_list_show(AuListKind kind);
/* The model's data changed outside its window: refresh soon. Does nothing
 * when the window is closed. */
void au_cocoa_list_touch(AuListKind kind);

/* ---- Developer windows (devwin_cocoa.m) ---- */
void au_cocoa_devwin_show(AuDevWinKind kind);
void au_cocoa_devwin_run_timing(void);
void au_cocoa_devwin_run_benchmark(void);
bool au_cocoa_devwin_owns(NSWindow *w);
bool au_cocoa_devwin_edit(NSWindow *w, AuEditAction action);
void au_cocoa_devwin_find(NSWindow *w);
void au_cocoa_devwin_find_again(NSWindow *w, bool forward);
bool au_cocoa_devwin_can_find_again(NSWindow *w);

/* ---- About (about_cocoa.m) ---- */
void au_cocoa_about_show(void);
bool au_cocoa_about_built(void);

#endif
