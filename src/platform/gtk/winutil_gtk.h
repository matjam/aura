/* Helpers shared by the GTK4 port's secondary windows (windows_gtk.h):
 * the caller's host struct, and the dialogs the models wait on.
 *
 * GTK4 has no blocking dialog call, but the models' callbacks (ask for a
 * name, choose a cookie rule, the Find dialog) return their answer. These
 * dialogs are modal windows run in a nested GMainLoop until answered.
 * While one runs, au_gtk_dialog_running is true: the windows' timers skip
 * their refresh and their close requests are refused, so the model the
 * dialog was opened from does not change under it. */
#ifndef AURA_GTK_WINUTIL_H
#define AURA_GTK_WINUTIL_H

#include "windows_gtk.h"

/* The host given to au_gtk_windows_init (zeros before it). */
extern AuGtkWindowsHost au_gtk_host;

/* Whether a nested dialog is running. */
bool au_gtk_dialog_running(void);

/* Asks for a line of text: prompt above an entry holding initial,
 * selected. Returns the text (au_alloc'd) or NULL when cancelled. */
char *au_gtk_ask_text(GtkWindow *parent, const char *title, const char *prompt, const char *initial);
/* One of n radio buttons (current checked, -1 for none); the index
 * chosen, or -1 when cancelled. */
int au_gtk_choose(GtkWindow *parent, const char *title, const char *prompt, const char *const *items, int n,
                  int current);
/* Shows text (lines of "Label: value") in a window with a Close button;
 * does not wait. */
void au_gtk_show_text(GtkWindow *parent, const char *title, const char *text);
/* An alert with one OK button; does not wait. */
void au_gtk_alert(GtkWindow *parent, const char *message, const char *detail);

/* Puts text on the clipboard of w's display. */
void au_gtk_copy(GtkWidget *w, const char *text, size_t len);

/* Seconds since the epoch, as the models' AuTime. */
AuTime au_gtk_now(void);

/* The front browser window's GtkWindow, or NULL. */
GtkWindow *au_gtk_front_parent(void);

/* Adds the CSS the windows share to the default display, once. */
void au_gtk_window_css(void);

/* Tells the caller the set of windows changed. */
void au_gtk_windows_changed(void);

#endif
