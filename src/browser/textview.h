/* A fixed-width text view for the developer windows (Page Source,
 * Console, Network, Performance): lines of text in a monospace face with
 * optional line numbers, wrapping, colour runs, a text selection or a
 * selected row, find, and scrolling by rows. Portable C99; the shells
 * put it in a window with a native scroll bar and paint it with Aura's
 * canvas.
 *
 * The view owns no text. It asks an AuTextSource for the number of lines
 * and for one line at a time, and lays out only what it paints: a layout
 * pass (au_textview_layout) reads each line's cell count once to know
 * how many rows it wraps to, and painting reads only the lines on
 * screen. A 1.3 MB document of 30000 lines lays out in a few
 * milliseconds and paints in the time of one screen of text.
 *
 * Cells: every code point takes one cell of the face's digit width; a
 * tab moves to the next multiple of 4 cells. Wide characters drawn by a
 * fallback face may overhang their cell.
 *
 * Positions are (line, byte offset in the line). */
#ifndef AURA_BROWSER_TEXTVIEW_H
#define AURA_BROWSER_TEXTVIEW_H

#include "../paint/font.h"
#include "../platform/platform.h" /* AU_KEY_*: plain constants */

typedef enum {
    AU_TS_PLAIN,
    AU_TS_TAG,     /* source: a tag's brackets and name */
    AU_TS_ATTR,    /* source: an attribute name */
    AU_TS_STRING,  /* source: an attribute value */
    AU_TS_COMMENT, /* source: a comment */
    AU_TS_DOCTYPE, /* source: <!DOCTYPE and other <! declarations */
    AU_TS_DIM,     /* secondary text */
    AU_TS_ERROR,
    AU_TS_WARN,
    AU_TS_HEADING, /* a section title: drawn on a grey band */
    AU_TS_INPUT,   /* console: the expression typed */
    AU_TS_RESULT,  /* console: its result */
    AU_TS_COUNT
} AuTextStyle;

typedef struct AuTextRun {
    uint32_t start; /* byte offset in the line */
    uint8_t style;  /* AuTextStyle */
} AuTextRun;

typedef struct AuTextLine {
    const char *text; /* UTF-8, without a line break; valid until the next call */
    size_t len;
    uint32_t cells;   /* display cells when the source knows them, else 0 with known_cells false */
    bool known_cells;
    uint32_t number;  /* the line number to show in the gutter; 0 for none */
    uint8_t style;    /* the line's style where no run says otherwise */
    bool joined;      /* continues the previous line: a copy puts no line break between them */
} AuTextLine;

typedef struct AuTextSource {
    void *ctx;
    uint32_t (*count)(void *ctx);
    void (*line)(void *ctx, uint32_t i, AuTextLine *out);
    /* Optional: colour runs for bytes [from, to) of line i, at most max,
     * the first starting at from. Returns how many (0: the line's style). */
    int (*runs)(void *ctx, uint32_t i, size_t from, size_t to, AuTextRun *out, int max);
} AuTextSource;

typedef struct AuTvPos {
    uint32_t line, off;
} AuTvPos;

typedef struct AuTextView {
    AuTextSource src;
    /* Settings; call au_textview_layout after changing them. */
    bool wrap;        /* wrap long lines to the width (else they are cut at the edge) */
    bool numbers;     /* a gutter with line numbers */
    bool select_rows; /* a click selects a line (a list) instead of starting a text selection */
    float size;       /* the em in pixels; 10 by default */
    AuColor highlight; /* the selection colour; 0 for a default */
    bool active;      /* the window is active: the selection is drawn in the highlight colour */
    /* Layout. */
    AuFontList *font;
    int width, height;
    float cell_w;
    int row_h, baseline, gutter, cols, visible_rows;
    uint32_t nlines;
    uint32_t *first_row; /* nlines + 1 entries: the first row of each line, then the total */
    uint32_t total_rows;
    uint32_t top;        /* the first row shown */
    /* Selection. */
    bool has_sel;
    AuTvPos anchor, focus;
    int32_t selected; /* select_rows: the selected line, or -1 */
    int drag_mode;    /* 0 none, 1 characters, 2 words, 3 lines */
    AuTvPos drag_a, drag_b;
    AuBuf scratch;
} AuTextView;

void au_textview_init(AuTextView *tv);
void au_textview_free(AuTextView *tv);
void au_textview_set_source(AuTextView *tv, const AuTextSource *src);
/* Lays the lines out for a view of width by height pixels: call it when
 * the size, the settings or the text change. The scroll position is kept
 * (clamped); a selection that no longer exists is dropped. */
void au_textview_layout(AuTextView *tv, AuFontSet *fonts, int width, int height);
/* Paints the view with its top-left at (x, y); paints every pixel of
 * its width and height. */
void au_textview_paint(AuTextView *tv, AuCanvas *c, int x, int y);

/* Scrolling, in rows. */
uint32_t au_textview_max_top(const AuTextView *tv);
void au_textview_scroll_to(AuTextView *tv, uint32_t top);
bool au_textview_at_end(const AuTextView *tv);
/* Scrolls so the row holding pos is shown (a third of the way down when
 * it was off screen). */
void au_textview_reveal(AuTextView *tv, AuTvPos pos);

/* The mouse, in view pixels. clicks: 1 starts a character selection (or
 * selects a row), 2 selects a word, 3 a line; shift extends. Each
 * returns true when the view needs repainting. */
bool au_textview_mouse_down(AuTextView *tv, int x, int y, int clicks, bool shift);
bool au_textview_mouse_drag(AuTextView *tv, int x, int y);
void au_textview_mouse_up(AuTextView *tv);
/* The line at view point (x, y), or -1 below the last one. */
int32_t au_textview_line_at(const AuTextView *tv, int x, int y);
/* A key: arrows, page up and down, home and end scroll (or move the
 * selected row). Returns true when it was used. */
bool au_textview_key(AuTextView *tv, int key);

bool au_textview_has_selection(const AuTextView *tv);
void au_textview_select_all(AuTextView *tv);
void au_textview_clear_selection(AuTextView *tv);
/* Appends the selected text (the selected row with select_rows); false
 * when nothing is selected. */
bool au_textview_selection_text(AuTextView *tv, AuBuf *out);
/* Appends every line. */
void au_textview_all_text(AuTextView *tv, AuBuf *out);
/* Finds text (ASCII case-insensitive) after the selection, or before it,
 * wrapping around; selects and reveals the match. A match does not span
 * lines. False when there is none. */
bool au_textview_find(AuTextView *tv, const char *text, bool forward);

/* The cells a line takes (tabs expanded). */
uint32_t au_textview_cells(const char *text, size_t len);

/* ---- A list of lines the view can show ----
 * For text made on the spot (the console, the request log, a report). */
typedef struct AuTextList {
    char **text;
    uint8_t *style;
    uint32_t n, cap;
} AuTextList;

void au_textlist_init(AuTextList *l);
void au_textlist_free(AuTextList *l);
void au_textlist_clear(AuTextList *l);
void au_textlist_add(AuTextList *l, int style, const char *text, size_t len);
/* Adds text line by line (split at line breaks). */
void au_textlist_add_text(AuTextList *l, int style, const char *text);
void au_textlist_source(AuTextList *l, AuTextSource *out);

#endif
