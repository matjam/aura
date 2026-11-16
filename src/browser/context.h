/* Contextual menus: what a Control-click (or click and hold) offers at a
 * point of a browser window, and the commands the model runs itself.
 * Portable C99; the shell shows the menu natively.
 *
 * To show one, call au_bw_context_menu with the window point, show the
 * enabled items' labels in order (AU_CMD_SEPARATOR is a divider line),
 * then pass the chosen command to au_command_run_context (commands.h),
 * which runs the model's commands with au_bw_context_command and hands
 * the rest to the port's AuCommandHost: copying to the clipboard
 * (AU_CMD_COPY_LINK, AU_CMD_COPY_IMAGE, AU_CMD_COPY), the address
 * field's editing commands, AU_CMD_OPEN_LINK_WINDOW and the Page Source
 * window. Free the menu with au_context_menu_free. Item labels are the
 * command table's titles (au_command_title). */
#ifndef AURA_BROWSER_CONTEXT_H
#define AURA_BROWSER_CONTEXT_H

#include "browser.h"
#include "commands.h" /* AuCommand: the contextual commands and their titles */

typedef enum AuContextKind {
    AU_CONTEXT_NONE, /* nothing to offer here */
    AU_CONTEXT_PAGE, /* the page, a link or an image in it */
    AU_CONTEXT_FIELD,
    AU_CONTEXT_TAB
} AuContextKind;

/* What the shell knows about the address field and the clipboard. */
enum {
    AU_CONTEXT_FIELD_SELECTION = 1, /* the field has a non-empty selection */
    AU_CONTEXT_FIELD_TEXT = 2,      /* the field holds text */
    AU_CONTEXT_CLIPBOARD_TEXT = 4   /* the clipboard holds text */
};

#define AU_CONTEXT_MAX 16

typedef struct AuContextItem {
    AuCommand cmd;
    const char *label; /* static */
    bool enabled;
} AuContextItem;

typedef struct AuContextMenu {
    AuContextKind kind;
    int n;
    AuContextItem items[AU_CONTEXT_MAX];
    int tab;     /* AU_CONTEXT_TAB: the tab's index */
    char *link;  /* AU_CONTEXT_PAGE: the link's absolute URL, or NULL */
    char *image; /* AU_CONTEXT_PAGE: the image's absolute URL, or NULL */
} AuContextMenu;

/* Builds the menu for window point (x, y); flags are AU_CONTEXT_*. A
 * point over the page with a link offers the link's commands (and the
 * image's when the link is an image); over an image, the image's; else
 * the page's. Copy is added when the page has a selection. Returns
 * false (kind AU_CONTEXT_NONE, no items) where there is nothing to
 * offer. */
bool au_bw_context_menu(AuBrowserWindow *w, int x, int y, unsigned flags, AuContextMenu *m);
void au_context_menu_free(AuContextMenu *m);
/* Runs cmd from menu m. Returns false when it is the shell's to do. */
bool au_bw_context_command(AuBrowserWindow *w, const AuContextMenu *m, AuCommand cmd);

/* Content blocking per site: the registrable domains in the
 * blocking.sites_off preference. au_bw_site_blocking says whether the
 * active page is a site blocking can apply to (http or https, with the
 * blocking preference on) and whether it is switched off there;
 * au_bw_toggle_site_blocking flips it, bumps prefs_gen (the shell saves
 * the preferences and passes them to the blocklists) and reloads. */
bool au_bw_site_blocking(AuBrowserWindow *w, bool *off);
void au_bw_toggle_site_blocking(AuBrowserWindow *w);

#endif
