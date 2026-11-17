/* @font-face loading for a document view (internal to src/page).
 *
 * An AuWebFonts holds a document's @font-face rules and a font set layered
 * over the platform set, into which loaded faces go. The document view
 * passes each style sheet it builds to au_webfonts_add_sheet, calls
 * au_webfonts_scan after styling (faces whose family the page uses for
 * text in their unicode-range are fetched), hands fetched bytes to
 * au_webfonts_done, and lays the document out again when
 * au_webfonts_take_changed reports a new face.
 *
 * font-display: with a clock (au_webfonts_set_clock), a face is in its
 * block period for AU_WEBFONT_BLOCK_MS (auto, block) or
 * AU_WEBFONT_SHORT_BLOCK_MS (swap, fallback, optional) from its first
 * fetch; au_webfonts_hidden then says its text is painted invisible
 * (laid out with the fallback's metrics). A face that fails ends its
 * block period. A fallback face arriving more than AU_WEBFONT_SWAP_MS
 * after its block period, or an optional face arriving after it, is not
 * used. Without a clock every face swaps in when it arrives and no text
 * is hidden.
 *
 * src picks the first entry that works: local() faces of the platform
 * set, then url() with format woff2, woff, truetype, opentype or
 * collection (or no format), decoded by au_font_decode; a CFF or corrupt
 * font moves on to the next entry. */
#ifndef AURA_PAGE_WEBFONT_H
#define AURA_PAGE_WEBFONT_H

#include "../layout/layout.h" /* css.h and font.h, with their AuColor typedefs reconciled */

/* The block period of font-display auto and block, from a face's first
 * fetch. On a slow link a face that takes longer shows the fallback font
 * after it and swaps in when it arrives. */
#define AU_WEBFONT_BLOCK_MS 3000
/* The block period of swap, fallback and optional. */
#define AU_WEBFONT_SHORT_BLOCK_MS 100
/* font-display: fallback's swap period after its block period. */
#define AU_WEBFONT_SWAP_MS 3000

typedef struct AuWebFonts AuWebFonts;

/* Starts a fetch of an absolute URL; returns its id, or 0. */
typedef uint32_t (*AuWebFontFetch)(void *ctx, const char *url);

AuWebFonts *au_webfonts_new(AuFontSet *platform);
/* Frees the layered set and its faces: free whatever uses them first. */
void au_webfonts_free(AuWebFonts *w);
AuFontSet *au_webfonts_set(AuWebFonts *w);

/* Takes the @font-face rules of a parsed sheet; text is the sheet's
 * source (unicode-range is read from it). Rules already known are kept
 * with their state. */
void au_webfonts_add_sheet(AuWebFonts *w, const AuCssSheet *sh, const char *text, size_t len);
/* Fetches the faces the styled document needs. */
void au_webfonts_scan(AuWebFonts *w, AuDocument *doc, AuWebFontFetch fetch, void *ctx);
/* Whether id is one of this object's fetches. */
bool au_webfonts_owns(const AuWebFonts *w, uint32_t id);
/* A fetch finished. Decodes and registers the face, or tries the rule's
 * next source. */
void au_webfonts_done(AuWebFonts *w, uint32_t id, const void *data, size_t len, bool ok, AuWebFontFetch fetch,
                      void *ctx);
/* The clock block periods are measured with (milliseconds, wrapping);
 * NULL turns them off. */
void au_webfonts_set_clock(AuWebFonts *w, uint32_t (*now)(void *ctx), void *ctx);
/* Ends block periods that have run out. Sets *changed when what
 * au_webfonts_hidden answers may have changed since the last call (also
 * after a face loaded, failed or started loading). Returns the
 * milliseconds until the next block period ends (UINT32_MAX: none). */
uint32_t au_webfonts_tick(AuWebFonts *w, bool *changed);
/* Whether text in the font-family list families (n names) at weight and
 * style is invisible: its first available font is a face in its block
 * period. Families before it that the platform has (or generic ones)
 * make it visible. */
bool au_webfonts_hidden(const AuWebFonts *w, const char *const *families, size_t n, int weight, bool italic);
/* True once after faces were added to the set. */
bool au_webfonts_take_changed(AuWebFonts *w);
/* Rules known, and faces loaded. */
void au_webfonts_counts(const AuWebFonts *w, size_t *rules, size_t *loaded);

#endif
