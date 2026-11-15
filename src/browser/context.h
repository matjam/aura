/* Contextual menus: what a Control-click (or click and hold) offers at a
 * point of a browser window, and the commands the model runs itself.
 * Portable C99; the shell shows the menu natively.
 *
 * To show one, call au_bw_context_menu with the window point, show the
 * enabled items' labels in order (AU_CMD_SEPARATOR is a divider line),
 * then pass the chosen command to au_bw_context_command. When that
 * returns false the command is the shell's: copying to the clipboard
 * (AU_CMD_COPY_LINK, AU_CMD_COPY_IMAGE, AU_CMD_COPY), the address
 * field's editing commands and AU_CMD_OPEN_LINK_WINDOW. Free the menu
 * with au_context_menu_free. */
#ifndef AURA_BROWSER_CONTEXT_H
#define AURA_BROWSER_CONTEXT_H

#include "browser.h"

typedef enum AuCommand {
    AU_CMD_NONE,
    AU_CMD_SEPARATOR,
    /* a link */
    AU_CMD_OPEN_LINK,
    AU_CMD_OPEN_LINK_TAB,
    AU_CMD_OPEN_LINK_WINDOW, /* the shell's */
    AU_CMD_COPY_LINK,        /* the shell's: copy AuContextMenu.link */
    AU_CMD_BOOKMARK_LINK,
    /* an image */
    AU_CMD_OPEN_IMAGE_TAB,
    AU_CMD_COPY_IMAGE, /* the shell's: copy AuContextMenu.image */
    /* the page */
    AU_CMD_BACK,
    AU_CMD_FORWARD,
    AU_CMD_RELOAD,
    AU_CMD_VIEW_SOURCE,
    AU_CMD_BOOKMARK_PAGE,
    AU_CMD_COPY, /* the shell's: copy the selection */
    /* the address field (all the shell's) */
    AU_CMD_CUT,
    AU_CMD_FIELD_COPY,
    AU_CMD_PASTE,
    AU_CMD_PASTE_GO,
    AU_CMD_SELECT_ALL,
    /* a tab */
    AU_CMD_CLOSE_TAB,
    AU_CMD_CLOSE_OTHER_TABS,
    AU_CMD_RELOAD_TAB,
    AU_CMD_DUPLICATE_TAB,
    /* downloads */
    AU_CMD_SAVE_LINK,
    AU_CMD_SAVE_IMAGE
} AuCommand;

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

#endif
