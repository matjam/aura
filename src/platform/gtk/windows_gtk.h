/* The GTK4 port's secondary windows: Preferences, the four lists
 * (Bookmarks, History, Downloads, Cookies), the six developer windows
 * (Page Source, Console, Network, Performance, System Status, Report a
 * Problem) and the About box.
 *
 * Each window is a view of a model in src/browser (prefs_ui.h, lists.h,
 * devwin_model.h, credits.h). These files hold no browser state and
 * decide nothing: they build widgets from the model's descriptions, turn
 * GTK events into the model's calls and give the model what it asks of
 * the system (the clipboard, a file chooser, a line of text). At most one
 * window of each kind is open; showing a kind that is open presents it.
 *
 * What the caller (the application, src/platform/gtk) must do:
 *
 *   1. Once, after the AuBrowser, the fonts and the AuApp exist and
 *      before any window is shown, fill an AuGtkWindowsHost and call
 *      au_gtk_windows_init. The struct is copied; the pointers in it must
 *      stay valid until au_gtk_windows_shutdown.
 *   2. Show windows from the command host (AuCommandHost.show_window):
 *        AU_WINDOW_PREFERENCES   au_gtk_prefs_show
 *        AU_WINDOW_HISTORY .. COOKIES  au_gtk_list_show(kind)
 *        AU_WINDOW_PAGE_SOURCE .. REPORT  au_gtk_devwin_show(kind)
 *        AU_WINDOW_TIMING_TEST     au_gtk_devwin_run_timing
 *        AU_WINDOW_PAINT_BENCHMARK au_gtk_devwin_run_benchmark
 *        AU_WINDOW_ABOUT           au_gtk_about_show
 *      au_gtk_window_kind maps an AuWindowKind to the right call.
 *   3. Route the Edit and Find commands to a developer window when it is
 *      the active window (au_gtk_devwin_owns): au_gtk_devwin_edit,
 *      au_gtk_devwin_find, au_gtk_devwin_find_again and
 *      au_gtk_devwin_can_find_again (AuCommandState.tool_can_find_again).
 *      The windows also handle Ctrl+C, Ctrl+V, Ctrl+A, Ctrl+F and Ctrl+G
 *      themselves, so this is needed only for the menu bar.
 *   4. After the cookie store changed outside the Cookies window (a page
 *      set cookies, Clear Cookies elsewhere): au_gtk_list_touch
 *      (AU_LIST_COOKIES). The lists also poll their models every 250 ms
 *      (au_list_stale), so this only makes the change show sooner.
 *   5. At quit, before freeing the browser: au_gtk_windows_shutdown,
 *      which closes every window (an open Preferences window is
 *      cancelled) and frees their models.
 *
 * Every window is a plain GtkWindow added to host->app, so the
 * application stays alive while one is open. */
#ifndef AURA_GTK_WINDOWS_H
#define AURA_GTK_WINDOWS_H

#include <gtk/gtk.h>

#include "browser/app.h"
#include "browser/browser.h"
#include "browser/commands.h"
#include "browser/devtools.h"
#include "browser/devwin_model.h"
#include "browser/lists.h"
#include "browser/prefs_ui.h"
#include "browser/sysinfo.h"
#include "paint/font.h"

typedef struct AuGtkWindowsHost {
    GtkApplication *app; /* required */
    AuBrowser *browser;  /* required */
    AuFontSet *fonts;    /* required: the developer windows paint with it */
    AuApp *aura_app;     /* the application services: the cookie jar, web
                          * storage, clearing cookies and the cache, the
                          * network's status; NULL disables those parts */
    void *ctx;           /* passed back to every callback below */

    /* The front browser window's model and its active page, or NULL.
     * Required. */
    AuBrowserWindow *(*front_window)(void *ctx);
    AuPage *(*front_page)(void *ctx);
    /* The front browser window's GtkWindow (the parent of the About box
     * and Preferences), or NULL. May be NULL. */
    GtkWindow *(*front_gtk_window)(void *ctx);

    /* Opens url in the front browser window, or in a new tab (a new
     * window when none is open). Required. */
    void (*open_url)(void *ctx, const char *url, bool new_tab);
    /* Shows a finished download's file in the file manager. May be NULL:
     * the file is shown with GtkFileLauncher. */
    bool (*reveal_download)(void *ctx, const AuDownload *d);

    /* Preferences were applied: what (AU_PREF_CHANGED_TABS: lay the
     * browser windows out again) for a live change; 0 after OK. The
     * caller saves the preferences file (prefs_gen moved) and, after OK,
     * reconfigures the network (au_app_prefs_changed). May be NULL. */
    void (*prefs_changed)(void *ctx, unsigned what, bool committed);
    /* The blocking switch or lists changed: tell the blocking manager.
     * May be NULL. */
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
     * AuDevWinHost.host_info, .connections and .status_sections. Each
     * may be NULL. When status_sections is NULL and aura_app is set, the
     * network's status (au_app_net_status) is shown instead. */
    void (*host_info)(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn);
    void (*connections)(void *ctx, AuBuf *out);
    void (*status_sections)(void *ctx, AuBuf *out);
    /* Runs the timing test on the front page / the paint benchmark in
     * the front window. false when there is no front window. Either may
     * be NULL. */
    bool (*timing_test)(void *ctx, AuTimingTest *out);
    bool (*benchmark)(void *ctx, AuPaintBench *out);
    /* From now on the network keeps request and response headers. May be
     * NULL. */
    void (*record_headers)(void *ctx);
    /* A window opened, closed or changed its title: menus that list
     * windows or depend on the front window should be rebuilt. May be
     * NULL. */
    void (*windows_changed)(void *ctx);

    /* The words the Preferences window uses for this system, as
     * AuPrefsHost. NULL for the defaults: "this computer", "the system
     * settings", "/home/you/Downloads", the XDG download folder's name
     * and "your home folder". */
    const char *machine, *settings_name, *folder_example, *system_downloads, *no_folder;
} AuGtkWindowsHost;

void au_gtk_windows_init(const AuGtkWindowsHost *host);
void au_gtk_windows_shutdown(void);

/* Shows the window of an AuWindowKind; false for a kind these files do
 * not make. */
bool au_gtk_window_kind(AuWindowKind kind);

/* ---- Preferences (prefswin_gtk.c) ---- */
void au_gtk_prefs_show(void);
bool au_gtk_prefs_is_open(void);

/* ---- Lists (listwin_gtk.c) ---- */
void au_gtk_list_show(AuListKind kind);
/* The model's data changed outside its window: refresh at the next
 * tick. Does nothing when the window is closed. */
void au_gtk_list_touch(AuListKind kind);

/* ---- Developer windows (devwin_gtk.c) ---- */
void au_gtk_devwin_show(AuDevWinKind kind);
/* Runs the timing test and shows Performance; runs the paint benchmark
 * and shows System Status. Beep when there is no front window. */
void au_gtk_devwin_run_timing(void);
void au_gtk_devwin_run_benchmark(void);
/* Whether w is one of the developer windows. */
bool au_gtk_devwin_owns(GtkWindow *w);
/* Edit menu on developer window w: COPY, PASTE (filter field) and
 * SELECT_ALL. false when the action does not apply. */
bool au_gtk_devwin_edit(GtkWindow *w, AuEditAction action);
/* Find, Find Again (forward) and Find Previous on developer window w. */
void au_gtk_devwin_find(GtkWindow *w);
void au_gtk_devwin_find_again(GtkWindow *w, bool forward);
bool au_gtk_devwin_can_find_again(GtkWindow *w);

/* ---- About (about_gtk.c) ---- */
void au_gtk_about_show(void);

#endif
