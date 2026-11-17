/* The Linux GTK4 port (docs/gtk.md): a GtkApplication whose windows show
 * the browser model (src/browser) in native chrome mode, with the POSIX
 * services (src/platform/posix) underneath and the GLib main loop driving
 * the network stack.
 *
 * The port holds no browser state of its own: what a command does and
 * whether it is enabled come from commands.h, what a widget shows from
 * the au_bw_* accessors, and every user action calls an au_bw_* action.
 *
 * Files:
 *     main.c          start, the tick, quit, session restore
 *     window.c        a browser window: header bar, tabs, page, status
 *     input.c         keys, the input method, mouse and scroll on the page
 *     menus.c         the menu bar and actions from the command table,
 *                     the command host, contextual menus
 *     host.c          AuBrowserHost, and the dialogs it waits for
 *     net_gtk.c       descriptor watches and the network pump
 *     files_gtk.c     the XDG profile and cache folders, finding assets
 *     fonts_gtk.c     fontconfig and the bundled fonts, platform.h
 *     downloads_gtk.c download files in the XDG download folder
 *     sysinfo_gtk.c   AuSysInfo from uname, os-release and /proc */
#ifndef AURA_GTK_H
#define AURA_GTK_H

#include <gtk/gtk.h>

#include "browser/app.h"
#include "browser/browser.h"
#include "browser/commands.h"
#include "browser/context.h"
#include "browser/present.h"
#include "browser/sysinfo.h"
#include "net/transfer.h"
#include "platform/posix/posix.h"

/* One browser window. */
typedef struct AuGtkWin {
    struct AuGtkWin *next;
    AuBrowserWindow *bw;
    GtkWindow *window;
    GtkWidget *back, *forward, *reload, *entry;
    GtkWidget *notebook;
    GtkWidget *search_bar, *search_entry;
    GtkWidget *area, *vscroll, *hscroll;
    GtkAdjustment *hadj, *vadj;
    GtkWidget *status, *progress;
    /* The page's pixels: a Cairo image surface the model paints into
     * through an AuSurface over the same memory (docs/gtk.md, "HiDPI"). */
    cairo_surface_t *cs;
    AuSurface *surf;
    AuPresent present;
    int pw, ph; /* the page size, logical pixels */
    char *typed; /* what the user typed in the address field, before completion */
    bool syncing; /* widgets are being set from the model: ignore their signals */
    bool closing; /* the model closed the last tab: freed at the next flush */
    bool freed;   /* the model is gone, the GtkWindow may linger */
    bool field_focus;
    /* The contextual menu shown last, kept until the next one. */
    AuContextMenu menu;
    bool have_menu;
    GtkWidget *menu_popover;
    double last_x, last_y; /* the pointer on the page */
    uint32_t click_mods;
    /* AU_CHANGED_* bits not yet shown: the widgets are set at the next
     * flush, never from inside the model's call or a widget's signal. */
    unsigned pending;
    guint complete_id; /* the address completion waiting for an idle */
    /* Multiple clicks: the last press's time, place, count and button. */
    uint32_t press_ms;
    double press_x, press_y;
    int clicks, press_button;
    bool pressed;
} AuGtkWin;

typedef struct AuGtk {
    GtkApplication *app;
    AuBrowser *browser;
    AuApp *a;
    AuFontSet *fonts;
    AuGtkWin *wins;
    AuGtkWin *front; /* the browser window last active */
    int modal;       /* nested loops of dialogs the model waits for */
    bool quitting;
    bool started;
    char assets[1024]; /* the assets folder, "" when none was found */
} AuGtk;

extern AuGtk au_gtk;

/* main.c */
AuGtkWin *au_gtk_new_window(const char *url, const AuSessionWindow *sw);
void au_gtk_quit(void);
void au_gtk_kick(void); /* run the tick soon (after input, a load starting) */

/* window.c */
AuGtkWin *au_gtk_win_new(int width, int height);
void au_gtk_win_destroy(AuGtkWin *w); /* frees the model and the window */
void au_gtk_win_changed(AuGtkWin *w, unsigned what);
void au_gtk_win_flush(AuGtkWin *w);
void au_gtk_win_focus_address(AuGtkWin *w);
void au_gtk_win_find(AuGtkWin *w);
bool au_gtk_win_entry_focused(AuGtkWin *w);
AuGtkWin *au_gtk_win_of(AuBrowserWindow *bw);
AuGtkWin *au_gtk_front(void);

/* input.c */
void au_gtk_input_attach(AuGtkWin *w);
uint32_t au_gtk_mods(GdkModifierType state); /* Ctrl as AU_MOD_COMMAND: the primary modifier */
uint32_t au_gtk_current_mods(GtkWidget *widget);

/* menus.c */
void au_gtk_menus_init(void);
void au_gtk_menus_refresh(void); /* enabled, checked and titles from the model */
void au_gtk_menus_dirty(void);   /* rebuild the history, bookmarks and windows lists */
void au_gtk_command_state(AuCommandState *st);
const AuCommandHost *au_gtk_command_host(void);
void au_gtk_run(AuCommand cmd);
void au_gtk_context_menu(AuGtkWin *w, double x, double y);
void au_gtk_edit(AuEditAction a);
/* The native windows of AuCommandHost.show_window, routed in one place
 * (menus.c, "Native windows") so the window code can be attached there. */
bool au_gtk_show_window(AuWindowKind kind);

/* host.c */
void au_gtk_browser_host(AuBrowserHost *out);
void au_gtk_modal_begin(void);
void au_gtk_modal_end(void);
/* Waits in a nested main loop until *done. */
void au_gtk_modal_wait(const bool *done);
bool au_gtk_ask(GtkWindow *parent, const char *message, const char *detail, const char *yes, const char *no);
void au_gtk_notice(GtkWindow *parent, const char *message, const char *detail);
void au_gtk_copy_text(const char *utf8, size_t len);

/* net_gtk.c */
bool au_gtk_net_start(void);
void au_gtk_net_pump(void);
void au_gtk_net_pause(bool paused); /* watches off while a dialog waits */
void au_gtk_net_stop(void);

/* files_gtk.c */
bool au_gtk_files_init(AuAppPort *out);
void au_gtk_find_assets(const char *argv0);
bool au_gtk_asset_path(const char *rel, char *out, size_t cap);

/* downloads_gtk.c */
void au_gtk_download_files(AuDownloadFiles *out);
bool au_gtk_download_is_app(const char *name, const char *mime);
void au_gtk_download_folder(char *out, size_t cap);

/* fonts_gtk.c */
void au_gtk_fonts_free(void);

/* sysinfo_gtk.c */
void au_gtk_sysinfo(AuSysInfo *s);
uint32_t au_gtk_memory_free_kb(void);

#endif
