/* Helpers shared by the macOS port's secondary windows (windows_cocoa.h):
 * the dialogs the models wait on, the pasteboard, and small conversions.
 *
 * The models' callbacks (ask for a name, choose a cookie rule, the Find
 * dialog, Save Report) return their answer, so these dialogs run modally
 * (NSAlert runModal, NSSavePanel runModal) between the host's modal_begin
 * and modal_end. While one runs, au_cocoa_dlg_running is true: the
 * windows' timers skip their refresh and their close requests are refused,
 * so the model the dialog was opened from does not change under it. */
#ifndef AURA_COCOA_DIALOGS_H
#define AURA_COCOA_DIALOGS_H

#import "windows_cocoa.h"

/* The host given to au_cocoa_windows_init (winutil_cocoa.m). */
extern AuCocoaWindowsHost au_cocoa_wh;

/* UTF-8 (Latin-1 when it is not valid UTF-8) as an NSString; never nil. */
NSString *au_cocoa_dlg_str(const char *utf8);

/* Whether a modal dialog of the windows is running. */
bool au_cocoa_dlg_running(void);
/* Around a modal dialog the windows wait for: counts it and runs the
 * host's modal_begin and modal_end. */
void au_cocoa_dlg_begin(void);
void au_cocoa_dlg_end(void);

/* Asks for a line of text: prompt above a field holding initial, selected.
 * Returns the text (au_alloc'd) or NULL when cancelled. */
char *au_cocoa_dlg_ask_text(NSWindow *parent, const char *title, const char *prompt, const char *initial);
/* One of n radio buttons (current checked, -1 for none); the index
 * chosen, or -1 when cancelled. */
int au_cocoa_dlg_choose(NSWindow *parent, const char *title, const char *prompt, const char *const *items, int n,
                        int current);
/* Shows text (lines of "Label: value") in a window with a Close button;
 * does not wait. */
void au_cocoa_dlg_show_text(NSWindow *parent, const char *title, const char *text);
/* An alert with one OK button: a sheet on parent when there is one. */
void au_cocoa_dlg_alert(NSWindow *parent, const char *message, const char *detail);

/* Puts text on the general pasteboard; false when it could not. */
bool au_cocoa_dlg_copy(const char *text, size_t len);

/* Seconds since the epoch, as the models' AuTime. */
AuTime au_cocoa_dlg_now(void);

/* Tells the caller the set of windows or a title changed. */
void au_cocoa_dlg_windows_changed(void);

/* Each window file's stop and whether it is built (winutil_cocoa.m calls
 * them; the weak placeholders there are replaced). */
void au_cocoa_lists_shutdown(void);
bool au_cocoa_lists_built(void);
void au_cocoa_devwin_shutdown(void);
bool au_cocoa_devwin_built(void);

#endif
