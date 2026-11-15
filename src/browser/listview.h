/* A simple list for the bookmarks and history windows: rows of text with
 * an indent level, folder disclosure triangles, day headings, a
 * selection and a scroll offset, painted with Aura's canvas. Portable
 * C99; the shells put it in a window with a native scroll bar. */
#ifndef AURA_BROWSER_LISTVIEW_H
#define AURA_BROWSER_LISTVIEW_H

#include "bookmarks.h"
#include "history.h"

#include "../paint/font.h"

#define AU_LIST_ROW_H 18

typedef enum { AU_ROW_HEADER, AU_ROW_FOLDER, AU_ROW_LINK } AuRowKind;

typedef struct AuListRow {
    AuRowKind kind;
    int indent;
    bool expanded;  /* folders */
    char *text;
    char *detail;   /* drawn dimmed after the text (a URL, a time) */
    uint32_t id;    /* bookmark id, or history index */
} AuListRow;

typedef struct AuListView {
    AuListRow *rows;
    int n, cap;
    int selected; /* row index or -1 */
    int scroll;   /* pixels */
    int width, height;
    bool active;  /* window active: selection drawn highlighted */
} AuListView;

void au_listview_init(AuListView *lv);
void au_listview_free(AuListView *lv);
void au_listview_clear(AuListView *lv);
void au_listview_add(AuListView *lv, AuRowKind kind, int indent, bool expanded, const char *text,
                     const char *detail, uint32_t id);

/* Fills the list from the bookmark tree (folders that are not expanded
 * hide their contents), keeping the selection on the same bookmark. */
void au_listview_from_bookmarks(AuListView *lv, const AuBookmarks *bm);
/* Fills the list from history, grouped by day with headings, most recent
 * first. Row ids are history indices. */
void au_listview_from_history(AuListView *lv, const AuHistory *h, AuTime now);

int au_listview_content_height(const AuListView *lv);
/* The row at (x, y) in list coordinates (scroll applied), or -1.
 * *disclosure is set when the point is on a folder's triangle. */
int au_listview_hit(const AuListView *lv, int x, int y, bool *disclosure);
/* Scrolls so row i is visible. */
void au_listview_reveal(AuListView *lv, int i);
/* Paints the list into (0, 0, width, height) in canvas coordinates. */
void au_listview_paint(const AuListView *lv, AuCanvas *c, AuFontSet *fonts);

#endif
