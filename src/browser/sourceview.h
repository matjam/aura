/* The Page Source window's model: a document's bytes split into lines,
 * with simple syntax colouring. Portable C99.
 *
 * au_sourceview_init makes one pass over the bytes (it keeps its own
 * copy): it records where each line starts and the colouring tokenizer's
 * state there, so any line can be coloured without reading what comes
 * before it. A line longer than AU_SOURCE_SEG_MAX bytes (minified pages
 * have lines of hundreds of kilobytes) is cut into segments of that size;
 * each segment is a "line" of the view, and only the first carries the
 * line number. Memory is the copy plus 12 bytes per segment.
 *
 * The tokenizer is not the HTML parser: it knows tags, attribute names,
 * quoted and unquoted attribute values, comments, <!DOCTYPE and the raw
 * text of <script> and <style> (which ends at the matching end tag), and
 * colours nothing inside them. That is enough to read a page's markup.
 *
 * The view implements AuTextSource (textview.h), so the text view shows
 * it with line numbers, wrapping, selection and find, laying out only
 * the lines on screen. */
#ifndef AURA_BROWSER_SOURCEVIEW_H
#define AURA_BROWSER_SOURCEVIEW_H

#include "textview.h"

#define AU_SOURCE_SEG_MAX 2048

typedef AuTextRun AuSourceRun;

typedef struct AuSourceSeg {
    uint32_t start;  /* byte offset of the segment */
    uint32_t number; /* the line number (from 1) on a line's first segment, else 0 */
    uint16_t cells;  /* display cells, tabs expanded (textview.h) */
    uint8_t state;   /* the tokenizer's state at start */
} AuSourceSeg;

typedef struct AuSourceView {
    char *data; /* a copy of the bytes */
    size_t len;
    AuSourceSeg *segs; /* nlines + 1: the last is the end */
    uint32_t nlines;   /* segments */
    uint32_t nnumbers; /* real lines */
} AuSourceView;

void au_sourceview_init(AuSourceView *sv, const char *data, size_t len);
void au_sourceview_free(AuSourceView *sv);
/* Segment i's bytes (no line break). */
const char *au_sourceview_line(const AuSourceView *sv, uint32_t i, size_t *len);
/* The colour runs of bytes [from, to) of segment i: at most max, the
 * first starting at from. Returns how many. */
int au_sourceview_runs(const AuSourceView *sv, uint32_t i, size_t from, size_t to, AuSourceRun *out, int max);
const char *au_source_style_name(int style);
/* The view as a text view's source. */
void au_sourceview_source(AuSourceView *sv, AuTextSource *out);

#endif
