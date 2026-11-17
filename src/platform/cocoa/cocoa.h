/* The modern macOS port (docs/cocoa.md): an AppKit application whose
 * windows show the browser model (src/browser) in native chrome mode, with
 * the POSIX services (src/platform/posix) underneath and the main run loop
 * driving the network stack through dispatch sources.
 *
 * The port holds no browser state of its own: what a command does and
 * whether it is enabled come from commands.h, what a control shows from
 * the au_bw_* accessors, and every user action calls an au_bw_* action.
 *
 * Files (Objective-C, ARC):
 *     main.m             start, the tick, quit, session restore, opening URLs
 *     window.m           a browser window: toolbar, tab strip, page, status
 *     input.m            the page view: drawing, keys, text input, mouse, scroll
 *     menus.m            the menu bar from the command table, the command
 *                        host, contextual menus, editing
 *     host.m             AuBrowserHost, and the dialogs it waits for
 *     net_cocoa.m        dispatch sources on the descriptors, the network pump
 *     files_cocoa.m      the profile and cache folders, finding the assets
 *     fonts_cocoa.m      Core Text's font files and the bundled fonts, platform.h
 *     downloads_cocoa.m  download files in the Downloads folder
 *     sysinfo_cocoa.m    AuSysInfo from sysctl and NSProcessInfo
 *     appearance_cocoa.m the system's light or dark setting, the chrome's theme */
#ifndef AURA_COCOA_H
#define AURA_COCOA_H

#import <AppKit/AppKit.h>

#include "browser/app.h"
#include "browser/browser.h"
#include "browser/commands.h"
#include "browser/context.h"
#include "browser/present.h"
#include "browser/sysinfo.h"
#include "net/transfer.h"
#include "platform/posix/posix.h"

@class AuPageView;
@class AuTabStrip;

/* One browser window. Its ivars are public so the port's files can reach
 * them as the GTK4 port reaches its AuGtkWin. */
@interface AuCocoaWin : NSObject <NSWindowDelegate, NSTextFieldDelegate, NSSearchFieldDelegate, NSToolbarDelegate> {
  @public
    AuBrowserWindow *bw;
    NSWindow *window;
    NSButton *back, *forward, *reload, *addTab;
    NSTextField *address;
    NSImageView *security;
    AuTabStrip *strip;
    NSView *body;      /* the tab strip, find bar, page and scroll bars */
    NSView *findBar;
    NSSearchField *findField;
    AuPageView *page;
    NSScroller *vscroll, *hscroll;
    NSTextField *status, *progress;
    /* The page's pixels: a bitmap the model paints into through an
     * AuSurface over the same memory (docs/cocoa.md, "HiDPI"). */
    void *pixels;
    CGContextRef bitmap;
    AuSurface *surf;
    AuPresent present;
    int pw, ph; /* the page size, points */
    NSString *typed; /* what the user typed in the address field, before completion */
    bool syncing;    /* controls are being set from the model: ignore their actions */
    bool closing;    /* the model closed the last tab: freed at the next flush */
    bool freed;      /* the model is gone, the NSWindow may linger */
    bool field_focus;
    bool find_shown;
    /* The contextual menu shown last, kept until the next one. */
    AuContextMenu menu;
    bool have_menu;
    double last_x, last_y; /* the pointer on the page */
    uint32_t click_mods;
    int clicks;
    bool pressed;
    /* AU_CHANGED_* bits not yet shown: the controls are set at the next
     * flush, never from inside the model's call or a control's action. */
    unsigned pending;
}
@end

/* The page: the model's surface drawn as a bitmap, and its input. */
@interface AuPageView : NSView <NSTextInputClient> {
  @public
    __unsafe_unretained AuCocoaWin *win;
}
@end

typedef struct AuCocoa {
    AuBrowser *browser;
    AuApp *a;
    AuFontSet *fonts;
    __unsafe_unretained AuCocoaWin *front; /* the browser window last active */
    int modal;                             /* dialogs the model waits for */
    bool quitting;
    bool started;
    char assets[1024]; /* the assets folder, "" when none was found */
} AuCocoa;

extern AuCocoa au_cocoa;

/* main.m */
AuCocoaWin *au_cocoa_new_window(const char *url, const AuSessionWindow *sw);
void au_cocoa_quit(void);
void au_cocoa_kick(void); /* run a flush soon (after input, a load starting) */

/* window.m */
AuCocoaWin *au_cocoa_win_new(int width, int height);
void au_cocoa_win_destroy(AuCocoaWin *w); /* frees the model and the window */
void au_cocoa_win_changed(AuCocoaWin *w, unsigned what);
void au_cocoa_win_flush(AuCocoaWin *w);
void au_cocoa_win_focus_address(AuCocoaWin *w);
void au_cocoa_win_find(AuCocoaWin *w);
void au_cocoa_win_page_resized(AuCocoaWin *w);
AuCocoaWin *au_cocoa_win_of(AuBrowserWindow *bw);
AuCocoaWin *au_cocoa_win_for(NSWindow *nw); /* the browser window of an NSWindow, or nil */
AuCocoaWin *au_cocoa_front(void);
NSArray<AuCocoaWin *> *au_cocoa_windows(void); /* every browser window, newest first */

/* input.m */
uint32_t au_cocoa_mods(NSEventModifierFlags flags); /* Command as AU_MOD_COMMAND */
uint32_t au_cocoa_current_mods(void);

/* menus.m */
void au_cocoa_menus_init(void);
void au_cocoa_menus_dirty(void); /* rebuild the history, bookmarks and windows lists */
void au_cocoa_command_state(AuCommandState *st);
const AuCommandHost *au_cocoa_command_host(void);
void au_cocoa_run(AuCommand cmd);
void au_cocoa_context_menu(AuCocoaWin *w, NSEvent *ev, double x, double y);
void au_cocoa_edit(AuEditAction a);
bool au_cocoa_show_window(AuWindowKind kind);

/* host.m */
void au_cocoa_browser_host(AuBrowserHost *out);
/* Starts the secondary windows (windows_cocoa.h) with this port's host. */
void au_cocoa_windows_start(void);
void au_cocoa_modal_begin(void);
void au_cocoa_modal_end(void);
bool au_cocoa_ask(NSWindow *parent, const char *message, const char *detail, const char *yes, const char *no);
void au_cocoa_notice(NSWindow *parent, const char *message, const char *detail);
void au_cocoa_copy_text(const char *utf8, size_t len);
NSString *au_cocoa_str(const char *utf8); /* never nil */

/* net_cocoa.m */
bool au_cocoa_net_start(void);
void au_cocoa_net_pump(void);
void au_cocoa_net_pause(bool paused); /* sources off while a dialog waits */
void au_cocoa_net_stop(void);

/* files_cocoa.m */
bool au_cocoa_files_init(AuAppPort *out);
void au_cocoa_find_assets(void);
bool au_cocoa_asset_path(const char *rel, char *out, size_t cap);

/* downloads_cocoa.m */
void au_cocoa_download_files(AuDownloadFiles *out);
bool au_cocoa_download_is_app(const char *name, const char *mime);
void au_cocoa_download_folder(char *out, size_t cap);

/* fonts_cocoa.m */
void au_cocoa_fonts_free(void);

/* sysinfo_cocoa.m */
void au_cocoa_sysinfo(AuSysInfo *s);
uint32_t au_cocoa_memory_free_kb(void);

/* appearance_cocoa.m: start follows the system's light or dark setting
 * (after the preferences are read); apply themes the chrome for the
 * Appearance preference, after it changed. */
void au_cocoa_appearance_start(void);
void au_cocoa_appearance_apply(void);
void au_cocoa_appearance_stop(void);

#endif
