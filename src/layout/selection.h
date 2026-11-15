/* Text selection and find in a laid-out document.
 *
 * A text position is a text node and a byte offset into the text its
 * layout box shows (the processed text: white space collapsed, text
 * transforms applied), the offsets hit testing reports
 * (AuHitResult.text_offset). A range runs from a start position to an
 * end position in document order; the anchor and focus of a selection
 * are normalized into one by au_text_range_make.
 *
 * The layout keeps the selection to paint (au_layout_set_selection): text
 * inside it is drawn over a highlight. It does not hold references to
 * the nodes: the caller keeps them alive while they are set (src/browser
 * holds a reference for each end) and clears the selection before
 * changing the document.
 *
 * Text of a range comes out in document order, with a line break
 * between blocks and at <br>, and a tab between table cells of a row.
 * Find searches that text, so a match may cross inline boundaries
 * ("find<b>me</b>") but not the break between two blocks (unless the
 * needle holds a line break itself).
 *
 * Engine rules: no OS calls, no recursion over document depth (tree
 * walks are loops), allocation through au_alloc. */
#ifndef AURA_LAYOUT_SELECTION_H
#define AURA_LAYOUT_SELECTION_H

#include "layout.h"

typedef struct AuTextPos {
    AuNode *node;    /* a text node with a layout box, or NULL for none */
    uint32_t offset; /* byte offset into the box's processed text */
} AuTextPos;

typedef struct AuTextRange {
    AuTextPos start, end; /* start <= end in document order */
} AuTextRange;

/* Document order of two nodes: negative when a comes first, 0 when the
 * same node, positive after. An ancestor comes before its descendants. */
int au_node_order(const AuNode *a, const AuNode *b);
/* Order of two positions. A position without a node comes first. */
int au_text_pos_cmp(const AuTextPos *a, const AuTextPos *b);
/* The range between two positions in either order. */
AuTextRange au_text_range_make(AuTextPos a, AuTextPos b);
static inline bool au_text_range_empty(const AuTextRange *r)
{
    return !r->start.node || au_text_pos_cmp(&r->start, &r->end) >= 0;
}

/* The text position nearest viewport point (x, y) with the document
 * scrolled by (scroll_x, scroll_y): the text under the point when there
 * is some, else the nearest place on the nearest line (so dragging past
 * the end of a line or between paragraphs still extends a selection).
 * Returns false when the document has no text. */
bool au_layout_text_pos_at(AuLayout *L, float x, float y, float scroll_x, float scroll_y, AuTextPos *out);

/* The word around p (letters, digits and marks; else the single
 * character there), and the paragraph around p (the text of the block
 * whose lines hold it). Both return false when p names no text box. */
bool au_layout_word_at(AuLayout *L, const AuTextPos *p, AuTextRange *out);
bool au_layout_block_at(AuLayout *L, const AuTextPos *p, AuTextRange *out);
/* The range of all the document's text; false when there is none. */
bool au_layout_all_text(AuLayout *L, AuTextRange *out);

/* Appends the text of r to out (UTF-8, see above for breaks). */
void au_layout_range_text(AuLayout *L, const AuTextRange *r, AuBuf *out);
/* The bounding box of r's text in document coordinates (inner scroll
 * offsets and transforms not applied). False when nothing is laid out
 * in it. */
bool au_layout_range_rect(AuLayout *L, const AuTextRange *r, AuRect *out);

/* Sets the range painted as selected, with highlight colour argb (paint
 * colour: alpha in the high byte); NULL or an empty range clears it.
 * Needs a repaint to show. */
void au_layout_set_selection(AuLayout *L, const AuTextRange *r, uint32_t argb);
bool au_layout_get_selection(const AuLayout *L, AuTextRange *out);

/* Finds needle (UTF-8) in the document's text, searching forward from
 * position from (or from the start of the document when from has no
 * node) or backward from it, wrapping around once. With
 * case_insensitive, letters of Latin, Greek and Cyrillic match either
 * case. A forward search finds the first match starting at or after
 * from, a backward one the last match ending at or before it; so
 * searching forward from the current match's end (backward from its
 * start) walks the matches. Returns false when there is none. */
bool au_layout_find(AuLayout *L, const char *needle, const AuTextPos *from, bool forward, bool case_insensitive,
                    AuTextRange *out);

#endif
