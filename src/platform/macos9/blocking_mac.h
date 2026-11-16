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
/* Appends the "Content blocking" section of the System Status report. */
void mac_blocking_status_text(AuBuf *out);
AuBlockManager *mac_blocking_manager(void); /* NULL before init */

/* ---- The Preferences pane's list rows ----
 * prefswin.c builds the Blocking pane from the preference descriptions;
 * these make the rows of AU_PREF_BLOCK_LISTS: the catalogue's check boxes
 * with a status line each, from the editing session's rows. */
struct AuPrefsEdit;
/* Makes the rows from top; returns the y below them. */
short prefs_blocking_build(WindowPtr win, ControlHandle pane, struct AuPrefsEdit *edit, short top);
/* Sets the check boxes and status lines from the session. */
void prefs_blocking_load(void);
/* Redraws the status lines that changed (draw: now). */
void prefs_blocking_refresh(bool draw);
/* True when c is a row's check box: its list is set in the session
 * (the caller applies the live items). */
bool prefs_blocking_click(ControlHandle c);
/* From idle: true when the status lines should be refreshed (the
 * manager's generation changed, or a second passed while it works). */
bool prefs_blocking_due(void);
void prefs_blocking_close(void);

#endif
