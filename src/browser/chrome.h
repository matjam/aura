/* The browser chrome: layout, painting and hit testing of a browser
 * window's tab strip (or tab column), toolbar and status bar. Portable
 * C99; it paints with Aura's canvas into the window surface, so the host
 * tool renders the same chrome to PNG and tests check its geometry.
 *
 * The look follows Mac OS 9's Platinum appearance: grey faces, one-pixel
 * bevels, Charcoal for labels. Icons are vector paths.
 *
 * Window layout, top to bottom, for tabs on top:
 *     tab strip   AU_CHROME_TAB_H
 *     toolbar     AU_CHROME_TOOLBAR_H
 *     page area   (vertical scroll bar column on the right)
 *     status bar  AU_CHROME_STATUS_H (grow box at its right end)
 * For tabs on the left the toolbar spans the top, and the tab column runs
 * down the left side between toolbar and status bar, with a splitter on
 * its right edge that drags to resize it.
 *
 * The scroll bar and grow box rectangles are reserved for the platform's
 * native controls; the chrome paints a stand-in there only when asked
 * (the host tool). The address field's text is painted here unless the
 * platform edits it with a native text editor placed over address_text. */
#ifndef AURA_BROWSER_CHROME_H
#define AURA_BROWSER_CHROME_H

#include "../paint/font.h"
#include "page.h"
#include "prefs.h"

#define AU_CHROME_TAB_H 24     /* tab strip height (top tabs) */
#define AU_CHROME_TOOLBAR_H 32
#define AU_CHROME_STATUS_H 15
#define AU_CHROME_SCROLL_W 16  /* native scroll bar width */
#define AU_CHROME_ROW_H 22     /* a tab row in the left column */
#define AU_CHROME_SPLITTER_W 5
#define AU_CHROME_TAB_MIN_W 72 /* tabs shrink to this, then scroll */
#define AU_CHROME_TAB_MAX_W 180
#define AU_CHROME_PAGE_MIN_W 160 /* the left column leaves at least this */

typedef enum {
    AU_HIT_NONE,
    AU_HIT_TAB,         /* index: tab */
    AU_HIT_TAB_CLOSE,   /* index: tab */
    AU_HIT_NEW_TAB,
    AU_HIT_TAB_SCROLL_BACK, /* left or up */
    AU_HIT_TAB_SCROLL_FWD,  /* right or down */
    AU_HIT_TAB_AREA,    /* empty space in the strip or column */
    AU_HIT_BACK,
    AU_HIT_FORWARD,
    AU_HIT_RELOAD,      /* reload, or stop while loading */
    AU_HIT_HOME,
    AU_HIT_SECURITY,
    AU_HIT_ADDRESS,
    AU_HIT_BOOKMARK,
    AU_HIT_THROBBER,
    AU_HIT_TOOLBAR,     /* empty toolbar space */
    AU_HIT_SPLITTER,
    AU_HIT_PAGE,
    AU_HIT_VSCROLL,
    AU_HIT_STATUS,
    AU_HIT_GROW,
    AU_HIT_HSCROLL,
    AU_HIT_CORNER       /* the blank square where the two scroll bars meet */
} AuHitPart;

typedef struct AuHit {
    AuHitPart part;
    int index;
} AuHit;

typedef struct AuChromeLayout {
    int width, height;
    AuTabPosition position;
    int ntabs;
    /* Regions. Empty (all zero) when not present. */
    AuIRect tabstrip;   /* the strip (top) or column (left) */
    AuIRect tabs_area;  /* where tabs are drawn, inside tabstrip */
    AuIRect toolbar, page, vscroll, status, grow, splitter;
    AuIRect hscroll, corner; /* empty unless au_chrome_layout_hscroll turned them on */
    AuIRect back, forward, reload, home, address, security, address_text, bookmark, throbber;
    AuIRect new_tab, scroll_back, scroll_fwd;
    int tab_size;   /* width of a top tab, height of a left row */
    int tab_scroll; /* pixels the tabs are scrolled by */
    int tab_scroll_max;
    bool overflow;  /* tabs do not fit: scroll buttons shown */
} AuChromeLayout;

/* Computes the layout. column_width is the left column's width (clamped
 * here); tab_scroll is clamped to the valid range. */
void au_chrome_layout(AuChromeLayout *l, int width, int height, AuTabPosition pos, int column_width, int ntabs,
                      int tab_scroll);
/* Adds a horizontal scroll bar along the bottom of the page area (for
 * content wider than the page): the page and the vertical scroll bar
 * get shorter, and the square where the two bars meet, beside the grow
 * box's row, is a blank corner. Call after au_chrome_layout. */
void au_chrome_layout_hscroll(AuChromeLayout *l);
/* The rectangle of tab i (may lie partly or wholly outside tabs_area
 * when scrolled), and of its close box. */
AuIRect au_chrome_tab_rect(const AuChromeLayout *l, int i);
AuIRect au_chrome_tab_close_rect(const AuChromeLayout *l, int i);
/* The tab scroll that brings tab i fully into view. */
int au_chrome_scroll_to_tab(const AuChromeLayout *l, int i);
/* What is at window point (x, y). */
AuHit au_chrome_hit(const AuChromeLayout *l, int x, int y);
/* The tab index a dragged tab would move to when the pointer is at
 * (x, y). */
int au_chrome_drop_index(const AuChromeLayout *l, int x, int y);

/* ---- Painting ---- */

typedef struct AuChromeTab {
    const char *title; /* "" shows the URL or "Untitled" */
    bool loading;
    const AuSurface *icon; /* the site's 16 x 16 icon, or NULL for the page glyph */
    bool unloaded; /* its document was freed to save memory: the title is dimmed */
} AuChromeTab;

typedef struct AuChromeState {
    const AuChromeTab *tabs;
    int active;
    AuHit hover, pressed; /* pressed draws pressed only while hovered */
    int dragging;         /* tab being dragged, or -1 */
    bool can_back, can_forward, loading, bookmarked;
    AuSecurity security;
    const char *field_text;   /* address field contents */
    bool field_focused;
    bool draw_field_text;     /* false when a native editor shows it */
    const char *status_text;
    float progress;           /* 0..1, shown while loading */
    const char *progress_text; /* centred in the status bar ("Loading, 40%"); NULL or "" for none */
    int blocked;              /* requests of the page a blocklist refused: a shield and the count at the
                                 right of the status bar; 0 for none */
    int throbber_frame;       /* advances while loading */
    bool draw_native_standins; /* paint scroll bar and grow box stand-ins */
    bool active_window;       /* inactive windows draw dimmed labels */
    /* Font lists for labels (Charcoal) and small text (Geneva), so tab
     * titles and status text get per-character fallback. NULL: the plain
     * faces, without fallback. */
    AuFontList *label_list, *small_list;
} AuChromeState;

/* Paints the chrome (everything but the page area) inside clip. */
void au_chrome_paint(AuCanvas *c, AuFontSet *fonts, const AuChromeLayout *l, const AuChromeState *s, AuIRect clip);

/* The face the chrome draws labels with (Charcoal on Mac OS 9). */
AuFontFace *au_chrome_label_face(AuFontSet *fonts);
/* Fits text into max_w pixels at size, ending with an ellipsis when cut.
 * Writes into out (cap bytes) and returns it. */
char *au_chrome_ellipsize(AuFontFace *f, float size, const char *text, float max_w, char *out, size_t cap);
/* The same, measuring with per-character fallback through a font list. */
char *au_chrome_ellipsize_list(AuFontList *l, float size, const char *text, float max_w, char *out, size_t cap);
/* The families the chrome's labels and small text are drawn with. */
#define AU_CHROME_LABEL_FAMILIES "Charcoal, system-ui"
#define AU_CHROME_SMALL_FAMILIES "Geneva, system-ui"

/* Rectangle helpers. */
static inline bool au_irect_contains(AuIRect r, int x, int y)
{
    return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}
static inline bool au_irect_empty(AuIRect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
AuIRect au_irect_union(AuIRect a, AuIRect b);
AuIRect au_irect_intersect(AuIRect a, AuIRect b);

#endif
