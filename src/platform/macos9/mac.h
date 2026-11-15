/* Shared declarations of the Mac OS 9 shell's modules:
 *
 *   main.c        start-up, the event loop, menus, Apple events, saving
 *   browserwin.c  browser windows (GWorld, chrome, page, address field)
 *   listwin.c     the Bookmarks and History windows
 *   prefswin.c    the Preferences window (Appearance controls)
 *   storage.c     files in the Preferences folder, file: URLs, dialogs
 *   text.c        UTF-8 and Mac Roman, Pascal strings
 *   contextmenu.c contextual menus (Contextual Menu Manager or pop-up)
 *
 * Every window Aura opens carries a MacWin (its refCon), whose function
 * pointers the event loop calls; that is how several kinds of window
 * share one loop. */
#ifndef AURA_MAC_H
#define AURA_MAC_H

#include <Multiverse.h>

#include "appearance.h"
#include "browser/browser.h"
#include "browser/context.h"
#include "browser/listview.h"
#include "browser/url.h"

enum {
    kZoomDocProc = 8, /* documentProc with a zoom box */
    kMovableModalProc = 5,
    kScrollBarWidth = 16,
    kGrowBox = 15,
    /* CopyBits mode flag: dither when the screen has fewer colours than
     * the GWorld (256 colours), so gradients do not collapse into bands. */
    kDitherCopy = 64
};

typedef enum { WIN_BROWSER = 1, WIN_BOOKMARKS, WIN_HISTORY, WIN_PREFS, WIN_DOWNLOADS } WinKind;

typedef struct MacWin MacWin;
struct MacWin {
    WinKind kind;
    WindowPtr win;
    void (*update)(MacWin *w);
    void (*activate)(MacWin *w, bool on);
    void (*click)(MacWin *w, EventRecord *ev);
    /* Returns true when the key was used. */
    bool (*key)(MacWin *w, EventRecord *ev);
    void (*idle)(MacWin *w);
    void (*grow)(MacWin *w); /* after the window was resized */
    void (*close)(MacWin *w);
    /* Edit menu: 1 undo, 3 cut, 4 copy, 5 paste, 6 clear, 7 select all.
     * Returns true when handled. Item 0 only asks whether a text field
     * has the focus. NULL when the window has no text. */
    bool (*edit)(MacWin *w, int item);
};

MacWin *mac_win_of(WindowPtr w); /* NULL for windows Aura does not own */

static inline WindowPtr mac_next_window(WindowPtr w)
{
    return (WindowPtr)((WindowPeek)w)->nextWindow;
}

/* ---- platform_mac.c ---- */
uint32_t mac_fonts_ms(void);  /* time au_platform_load_fonts took */
uint32_t mac_fonts_bundled_ms(void); /* of which, registering the Fonts folder */
int mac_fonts_bundled(void);  /* faces registered from the Fonts folder */

/* ---- main.c ---- */
extern AuBrowser *g_browser;
extern AuFontSet *g_fonts;
extern bool g_quitting;
extern bool g_in_front; /* the application is not suspended */
void mac_out_of_memory(size_t wanted);
void mac_menus_dirty(void);
/* Brings the menu bar in step with the front window (enables, checks). */
void mac_adjust_menus(void);

/* ---- browserwin.c ---- */
typedef struct BrowserWin BrowserWin;
/* Opens a browser window. With a session window its tabs are restored;
 * else url (NULL: the home page) opens in its one tab. */
BrowserWin *browserwin_new(const char *url, const AuSessionWindow *sw);
BrowserWin *browserwin_front(void); /* the frontmost browser window or NULL */
AuBrowserWindow *browserwin_model(BrowserWin *bw);
void browserwin_focus_field(BrowserWin *bw);
void browserwin_close(BrowserWin *bw);
/* Paints and shows pending damage in every browser window. */
void browserwin_flush_all(void);
/* The AuBrowserHost callbacks. */
void browserwin_host(AuBrowserHost *h);
/* Debug menu: benchmark the scene in the front window. */
void browserwin_benchmark(BrowserWin *bw);
/* Debug menu: the active page's load and layout times, a timed scroll
 * repaint, and free memory. */
void browserwin_page_timings(BrowserWin *bw);
/* Edit > Find: asks for text and finds it in the active page. */
void browserwin_find(BrowserWin *bw);

/* ---- listwin.c ---- */
void listwin_show(WinKind kind); /* WIN_BOOKMARKS, WIN_HISTORY or WIN_DOWNLOADS */
/* Opens url in the front browser window (one is made if needed). */
void mac_open_url(const char *url, bool new_tab);

/* ---- prefswin.c ---- */
void prefswin_show(void);

/* ---- storage.c ---- */
bool storage_init(void); /* finds or makes the Aura preferences folder */
void storage_load_all(void);
/* Saves files whose data changed; force saves them even if the change is
 * recent (quitting). */
void storage_save_changed(bool force);
bool storage_session(AuSession *s);
AuTime mac_now(void);
bool mac_read_file_url(const char *url, char **data, size_t *len);
/* Asks for a file with the standard Open dialog; returns a file: URL to
 * free with au_free, or NULL. */
char *mac_choose_file_url(void);
/* File > Save Page As: asks for a file (StandardPutFile) and writes the
 * active page's source to it, type TEXT, creator AURA. */
void mac_save_page(AuBrowserWindow *w);

/* ---- contextmenu.c ---- */
/* Finds the Contextual Menu Manager (call once at start-up). */
void mac_context_init(void);
/* Whether a mouse-down asks for a contextual menu (Control-click). */
bool mac_context_click(const EventRecord *ev);
/* Shows the menu at a global point; returns the chosen item's index in
 * m->items, or -1. */
int mac_context_select(const AuContextMenu *m, Point global);

/* ---- text.c ---- */
/* UTF-8 to a Pascal string in Mac Roman ('?' for characters it lacks),
 * cut to fit. */
void mac_pstr(const char *utf8, Str255 out);
/* Mac Roman bytes to UTF-8 (au_alloc'd, NUL-terminated). */
char *mac_roman_to_utf8(const char *s, size_t n);
/* UTF-8 to Mac Roman bytes (au_alloc'd, NUL-terminated); *n gets the
 * length. */
char *mac_utf8_to_roman(const char *s, size_t *n);
/* Puts UTF-8 text on the clipboard as TEXT (Mac Roman, transliterated,
 * carriage return lines) and 'utxt' (UTF-16). False when it fails. */
bool mac_put_scrap(const char *utf8, size_t n);
/* Sets a menu item's text exactly (no metacharacter parsing). */
void mac_set_item_text(MenuHandle m, short item, const char *utf8);
void mac_append_item(MenuHandle m, const char *utf8);

#endif
