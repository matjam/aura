/* Content blocking in the Mac OS 9 application (blocking_mac.c) and its
 * Preferences pane (prefs_blocking.c). */
#ifndef AURA_MAC_BLOCKING_H
#define AURA_MAC_BLOCKING_H

#include "mac.h"

#include "net/blocklist_store.h"
#include "net/loader.h"

/* From mac_net_init, after the loader exists: makes the blocklist set and
 * the update manager, attaches the compiled lists kept in the Blocklists
 * folder of the Aura preferences folder, and gives the set to the loader. */
void mac_blocking_init(AuNet *net, AuNetLoader *loader);
/* Once per event loop pass (mac_net_idle). loading: a page load or a
 * download is under way, so no list download starts. */
void mac_blocking_idle(bool loading);
/* The manager is downloading or compiling (the loop must not sleep long). */
bool mac_blocking_busy(void);
/* The blocking preferences changed (also called by the model through
 * AuBrowser.blocking_changed for the per-site switch). */
void mac_blocking_prefs_changed(void);
/* Before the AuNet is freed. */
void mac_blocking_quit(void);
/* Appends the totals for View > Debug > Network Status. */
void mac_blocking_status(char *out, size_t cap);
AuBlockManager *mac_blocking_manager(void); /* NULL before init */

/* ---- The Preferences pane ----
 * prefswin.c owns the window, the pane's user pane and the three edit
 * fields; these make and run the rest: the catalogue's checkboxes with a
 * status line each, the update interval and Update Now. The checkboxes
 * and the button act at once; prefs_blocking_cancel puts the lists back. */
void prefs_blocking_build(WindowPtr win, ControlHandle pane, short top);
void prefs_blocking_load(const AuPrefs *pr);
/* True when c is one of the pane's controls (the click was handled). */
bool prefs_blocking_click(ControlHandle c);
void prefs_blocking_idle(void);
/* OK: the interval, and the three fields' text cleaned into the
 * preferences (addresses must be https; domains are normalised). */
void prefs_blocking_store(AuPrefs *pr, const char *urls, const char *allow, const char *block);
void prefs_blocking_cancel(AuPrefs *pr);
void prefs_blocking_close(void);

#endif
