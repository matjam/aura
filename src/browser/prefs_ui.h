/* The Preferences window's contents, described as data, and the editing
 * session a port runs while the window is open. Portable C99.
 *
 * To build a Preferences window, walk au_pref_desc(0) .. AU_PREF_COUNT - 1
 * (the order is the display order, section by section) and make one
 * native control per item from its kind, caption, title, unit and
 * choices. The port keeps only its layout: positions, sizes and fonts.
 *
 * To run the window:
 *   1. au_prefs_edit_new(&browser->prefs, &host) when the window opens;
 *      load each control from au_prefs_edit_get_*.
 *   2. When the user changes a check box, radio group or choice, call
 *      au_prefs_edit_set_*; for an item with AU_PREF_LIVE, then call
 *      au_prefs_edit_apply_live, which applies it to the browser at once.
 *      After any change, ask au_prefs_edit_enabled and au_prefs_edit_text
 *      again for the items whose state or note may follow.
 *   3. For a button, call au_prefs_edit_action and do what the returned
 *      AU_PREF_DO_* bits ask (the parts that need the OS).
 *   4. On OK, set every text and number field with au_prefs_edit_set_text
 *      and call au_prefs_edit_commit. On Cancel call au_prefs_edit_cancel,
 *      which puts the live items back. Then au_prefs_edit_free.
 *
 * Values are checked and clamped when they are set: numbers to their
 * range, choices to their list, blocking addresses and domains cleaned.
 * A number field whose text is not a number keeps the value the
 * preference had when the window opened. */
#ifndef AURA_BROWSER_PREFS_UI_H
#define AURA_BROWSER_PREFS_UI_H

#include "../core/core.h"
#include "../net/blocklist_store.h"
#include "../paint/font.h"
#include "browser.h"
#include "prefs.h"

typedef enum {
    AU_PREF_SECTION_GENERAL,
    AU_PREF_SECTION_FONTS,
    AU_PREF_SECTION_CONTENT,
    AU_PREF_SECTION_PRIVACY,
    AU_PREF_SECTION_NETWORK,
    AU_PREF_SECTION_DOWNLOADS,
    AU_PREF_SECTION_BLOCKING,
    AU_PREF_SECTION_COUNT
} AuPrefSection;

typedef enum {
    AU_PREF_CHECK,  /* a check box: bool */
    AU_PREF_RADIO,  /* a group of radio buttons, one per choice: choice index */
    AU_PREF_NUMBER, /* a whole number in [min, max], with a unit: int, or text */
    AU_PREF_TEXT,   /* a line of text: string */
    AU_PREF_CHOICE, /* a pop-up menu of fixed choices: choice index */
    AU_PREF_FONT,   /* a pop-up menu of the installed font families: string, or choice index */
    AU_PREF_FOLDER, /* a folder path, as text: string */
    AU_PREF_ACTION, /* a button that runs au_prefs_edit_action */
    AU_PREF_NOTE,   /* a paragraph of small text, au_prefs_edit_text */
    AU_PREF_LIST    /* the blocking lists, a check box and a status line per row */
} AuPrefKind;

/* Display order. Edit fields of a section are in the order the keyboard
 * moves through them. */
typedef enum {
    /* General */
    AU_PREF_HOME_PAGE,
    AU_PREF_USE_CURRENT_PAGE,
    AU_PREF_STARTUP,
    AU_PREF_TAB_POSITION,
    AU_PREF_APPEARANCE,
    AU_PREF_SEARCH,
    AU_PREF_SEARCH_NOTE,
    AU_PREF_EXTERNAL_NEW_WINDOW,
    AU_PREF_DEFAULT_BROWSER,
    /* Fonts */
    AU_PREF_FONT_SERIF,
    AU_PREF_FONT_SANS,
    AU_PREF_FONT_MONO,
    AU_PREF_FONT_SIZE,
    AU_PREF_FONT_SIZE_MONO,
    AU_PREF_FONT_SIZE_MIN,
    /* Content */
    AU_PREF_JAVASCRIPT,
    AU_PREF_LOAD_IMAGES,
    AU_PREF_BLOCK_POPUPS,
    AU_PREF_SCRIPT_MEMORY_MODE,
    AU_PREF_SCRIPT_MEMORY_MB,
    AU_PREF_WORKER_MEMORY_MODE,
    AU_PREF_WORKER_MEMORY_MB,
    AU_PREF_IMAGE_MEMORY_MODE,
    AU_PREF_IMAGE_MEMORY_MB,
    AU_PREF_IMAGE_COLORS,
    AU_PREF_SCRIPT_MEMORY_NOTE,
    /* Privacy and Security */
    AU_PREF_SEND_DNT,
    AU_PREF_SEND_GPC,
    AU_PREF_HTTPS_FIRST,
    AU_PREF_LEGACY_TLS,
    AU_PREF_WARN_APP_DOWNLOADS,
    AU_PREF_COOKIES,
    AU_PREF_BLOCK_THIRD_PARTY_COOKIES,
    AU_PREF_HISTORY_ENABLED,
    AU_PREF_HISTORY_DAYS,
    AU_PREF_CLEAR_HISTORY_ON_QUIT,
    AU_PREF_CLEAR_COOKIES_ON_QUIT,
    AU_PREF_CLEAR_CACHE_ON_QUIT,
    AU_PREF_CLEAR_HISTORY,
    AU_PREF_CLEAR_COOKIES,
    AU_PREF_CLEAR_CACHE,
    AU_PREF_SHOW_COOKIES,
    /* Network */
    AU_PREF_HTTP_PROXY_HOST,
    AU_PREF_HTTP_PROXY_PORT,
    AU_PREF_HTTPS_PROXY_HOST,
    AU_PREF_HTTPS_PROXY_PORT,
    AU_PREF_PROXY_NOTE,
    AU_PREF_USER_AGENT,
    AU_PREF_MEMORY_CACHE,
    AU_PREF_DISK_CACHE,
    AU_PREF_UNLOAD_TABS,
    /* Downloads */
    AU_PREF_DOWNLOAD_FOLDER,
    AU_PREF_DOWNLOAD_FOLDER_NOTE,
    AU_PREF_DECODE_DOWNLOADS,
    AU_PREF_DOWNLOADS_NOTE,
    /* Blocking */
    AU_PREF_BLOCK_TRACKERS,
    AU_PREF_BLOCK_LISTS,
    AU_PREF_BLOCK_UPDATE,
    AU_PREF_BLOCK_UPDATE_NOW,
    AU_PREF_BLOCK_CUSTOM,
    AU_PREF_BLOCK_ALLOW,
    AU_PREF_BLOCK_DENY,
    AU_PREF_COUNT,
    AU_PREF_NONE = -1
} AuPrefId;

/* AuPrefDesc.flags */
enum {
    AU_PREF_LIVE = 1, /* applies at once (au_prefs_edit_apply_live); Cancel puts it back */
    AU_PREF_STATUS = 2 /* has a status line beside it (au_prefs_edit_text), which may change */
};

/* How the item is kept in AuPrefs (internal; the port does not need it). */
typedef enum {
    AU_PREF_STORE_NONE,
    AU_PREF_STORE_BOOL,
    AU_PREF_STORE_INT,
    AU_PREF_STORE_ENUM,
    AU_PREF_STORE_STR,
    AU_PREF_STORE_SPECIAL /* code in prefs_ui.c */
} AuPrefStore;

typedef struct AuPrefDesc {
    AuPrefId id;
    AuPrefSection section;
    AuPrefKind kind;
    const char *key;     /* the preferences file key (prefs.h), NULL when none */
    const char *caption; /* the text before the control ("Home page:"), NULL for none */
    const char *title;   /* the control's own text: a check box's or button's title */
    const char *unit;    /* after a number field ("pixels"), NULL for none */
    const char *help;    /* a note's text, or NULL (au_prefs_edit_text gives the current one) */
    const char *const *choices; /* AU_PREF_RADIO and AU_PREF_CHOICE, NULL-terminated */
    const char *empty;   /* AU_PREF_FONT: the menu's single item when no family is installed */
    int min, max;        /* AU_PREF_NUMBER */
    AuPrefId enabled_by; /* AU_PREF_NONE, or the choice item whose value enables this one */
    int enabled_value;   /* ... the choice index that does */
    unsigned flags;
    AuPrefStore store;
    size_t offset;       /* in AuPrefs, for AU_PREF_STORE_BOOL .. _STR */
} AuPrefDesc;

const AuPrefDesc *au_pref_desc(AuPrefId id); /* NULL when out of range */
/* The item that shows a preferences file key, or NULL. */
const AuPrefDesc *au_pref_desc_by_key(const char *key);
const char *au_pref_section_title(AuPrefSection s);
size_t au_pref_choice_count(const AuPrefDesc *d); /* the fixed choices */

/* What the port knows about the system. Strings are copied. */
typedef enum { AU_DEFAULT_BROWSER_UNAVAILABLE, AU_DEFAULT_BROWSER_NO, AU_DEFAULT_BROWSER_YES } AuDefaultBrowser;
typedef struct AuPrefsHost {
    const AuFontSet *fonts;       /* the families of the font pop-ups; NULL for none */
    uint32_t memory_free_kb;      /* for Automatic script memory (0: unknown) */
    AuBlockManager *blocking;     /* the blocking lists' states; NULL before the manager runs */
    AuDefaultBrowser default_browser;
    const char *machine;          /* "this Mac" */
    const char *settings_name;    /* what sets the default browser: "Internet Config" */
    const char *folder_example;   /* "Macintosh HD:Downloads" */
    const char *system_downloads; /* the system's download folder's name, NULL when it has none */
    const char *no_folder;        /* where files go without a folder: "the desktop" */
} AuPrefsHost;

typedef struct AuPrefsEdit AuPrefsEdit;

/* Starts editing a copy of prefs (normally &browser->prefs). */
AuPrefsEdit *au_prefs_edit_new(const AuPrefs *prefs, const AuPrefsHost *host);
void au_prefs_edit_free(AuPrefsEdit *e);
/* The working copy, as it would be applied. */
const AuPrefs *au_prefs_edit_prefs(const AuPrefsEdit *e);
/* Updates what the system says (after the port made Aura the default
 * browser, or to refresh the free memory). */
void au_prefs_edit_set_default_browser(AuPrefsEdit *e, AuDefaultBrowser state);
void au_prefs_edit_set_memory_free(AuPrefsEdit *e, uint32_t free_kb);

/* Values. A getter of the wrong kind returns false, 0, -1 or "". A setter
 * returns false when the item is not of that kind or the value was
 * refused (then nothing changes). */
bool au_prefs_edit_get_bool(const AuPrefsEdit *e, AuPrefId id);
bool au_prefs_edit_set_bool(AuPrefsEdit *e, AuPrefId id, bool v);
int au_prefs_edit_get_int(const AuPrefsEdit *e, AuPrefId id);
bool au_prefs_edit_set_int(AuPrefsEdit *e, AuPrefId id, long v); /* clamped to [min, max] */
/* A choice, radio or font item: its index in the list, -1 when the value
 * is not in it (a font family that is not installed). An index out of
 * range is clamped, except the update interval's, which becomes daily,
 * and a font's, which is refused. */
int au_prefs_edit_get_choice(const AuPrefsEdit *e, AuPrefId id);
bool au_prefs_edit_set_choice(AuPrefsEdit *e, AuPrefId id, int index);
/* The text a field shows: a string, or a number in decimal. */
void au_prefs_edit_get_text(const AuPrefsEdit *e, AuPrefId id, AuBuf *out);
/* Sets a text, folder, number or font item from text. A number must be
 * all digits (with an optional sign); otherwise the item takes the value
 * it had when the window opened and this returns false. */
bool au_prefs_edit_set_text(AuPrefsEdit *e, AuPrefId id, const char *text);

/* Choices: the fixed list of a choice or radio item, or the installed
 * font families (unique and sorted) of a font item. */
size_t au_prefs_edit_choice_count(const AuPrefsEdit *e, AuPrefId id);
const char *au_prefs_edit_choice(const AuPrefsEdit *e, AuPrefId id, size_t i);

/* Whether the control takes input now (its enabling choice, or the
 * system's state for Make Default Browser). */
bool au_prefs_edit_enabled(const AuPrefsEdit *e, AuPrefId id);
/* The text of a note, or an item's status line ("Automatic is 32 MB on
 * this Mac"); "" when it has none. */
void au_prefs_edit_text(const AuPrefsEdit *e, AuPrefId id, AuBuf *out);

/* Puts a section's items back to their defaults (au_prefs_init). Live
 * items change in the working copy only: call au_prefs_edit_apply_live. */
void au_prefs_edit_reset_section(AuPrefsEdit *e, AuPrefSection s);

/* What a change asks of the port. */
enum {
    AU_PREF_CHANGED_TABS = 1,    /* the tab position changed: windows were laid out again */
    AU_PREF_CHANGED_BLOCKING = 2, /* the blocking switch or lists changed: tell the blocking manager */
    AU_PREF_CHANGED_APPEARANCE = 4 /* the colour scheme changed: pages restyled; the port themes its chrome */
};
/* Copies the live items of the working copy to b->prefs. Returns
 * AU_PREF_CHANGED_* for what differed (prefs_gen is bumped, and for the
 * tab position and the appearance au_browser_prefs_changed is called). */
unsigned au_prefs_edit_apply_live(AuPrefsEdit *e, AuBrowser *b);
/* Cancel: puts the live items of b->prefs back as they were when the
 * window opened. Returns AU_PREF_CHANGED_* as above. */
unsigned au_prefs_edit_cancel(AuPrefsEdit *e, AuBrowser *b);
/* Writes every shown item of the working copy into prefs. Returns true
 * when any value differed. Items the window does not show are left. */
bool au_prefs_edit_apply(const AuPrefsEdit *e, AuPrefs *prefs);
/* OK: au_prefs_edit_apply to b->prefs, then au_browser_prefs_changed. */
bool au_prefs_edit_commit(AuPrefsEdit *e, AuBrowser *b);

/* Buttons. au_prefs_edit_action does the part that is the browser
 * model's and returns what the port must do. */
enum {
    AU_PREF_DO_RELOAD = 1,          /* reload the control of *changed from the session */
    AU_PREF_DO_CLEAR_COOKIES = 2,   /* empty the cookie store */
    AU_PREF_DO_CLEAR_CACHE = 4,     /* empty the HTTP cache */
    AU_PREF_DO_SHOW_COOKIES = 8,    /* open the Cookies window */
    AU_PREF_DO_MAKE_DEFAULT = 16,   /* make Aura the default browser, then au_prefs_edit_set_default_browser */
    AU_PREF_DO_REFRESH_LISTS = 32   /* show the blocking lists' status lines again */
};
/* front: the front browser window (for Use Current Page), or NULL.
 * changed: set to the item whose value changed, AU_PREF_NONE when none. */
unsigned au_prefs_edit_action(AuPrefsEdit *e, AuPrefId id, AuBrowser *b, AuBrowserWindow *front,
                              AuPrefId *changed);

/* ---- The blocking lists (AU_PREF_BLOCK_LISTS) ----
 * One row per list of the catalogue, in its order. A row's check box is
 * live: after au_prefs_edit_set_block_list call au_prefs_edit_apply_live.
 * The status lines change while lists download: redraw them when
 * au_prefs_edit_block_generation changes. */
typedef struct AuPrefBlockRow {
    const char *id, *title;
    bool on;
    char status[256]; /* its size and date, or what the update manager is doing */
} AuPrefBlockRow;
size_t au_prefs_edit_block_list_count(const AuPrefsEdit *e);
bool au_prefs_edit_block_list(const AuPrefsEdit *e, size_t i, AuPrefBlockRow *out);
bool au_prefs_edit_set_block_list(AuPrefsEdit *e, size_t i, bool on);
uint32_t au_prefs_edit_block_generation(const AuPrefsEdit *e);
/* A list's status line, from its state. */
void au_prefs_block_status(const AuBlockListInfo *li, char *out, size_t cap);

/* The cleaning rules for the blocking fields: https addresses longer
 * than 11 and shorter than 1024 bytes, and domains (au_block_clean_domain,
 * no repeats); spaces separate, and commas too for domains. */
void au_prefs_clean_urls(const char *text, AuBuf *out);
void au_prefs_clean_domains(const char *text, AuBuf *out);

#endif
