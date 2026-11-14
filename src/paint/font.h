/* Aura fonts: a TrueType (sfnt) parser and outline rasterizer, a glyph
 * cache, text measurement and drawing, and the font set that layout picks
 * fonts from. Portable C99; font data is a byte range the platform
 * supplies (this code never opens files).
 *
 * Every table read is bounds-checked: fonts will come from the network.
 * A malformed font gives missing glyphs, never a crash.
 *
 * Not yet here, with the place each goes:
 * - OpenType CFF outlines ('CFF ' table): au_font_face_new records the
 *   table (face->cff); au_font_glyph_path is where a Type 2 charstring
 *   interpreter plugs in beside the glyf reader.
 * - GSUB/GPOS shaping: au_shape_text is the single entry point layout uses
 *   to turn text into positioned glyphs. Today it maps through cmap,
 *   applies advances and 'kern'; GSUB ligatures and GPOS kerning and marks
 *   go inside it.
 * - Font fallback: au_shape_text reports glyphs that the face lacks
 *   (glyph 0); a fallback chain from AuFontSet (au_fontset_match with the
 *   next family) will re-shape those runs.
 * - Hinting: none. Outlines are unhinted; vertical metrics snap the
 *   baseline to whole pixels and the x-height to a whole pixel at text
 *   sizes. */
#ifndef AURA_FONT_H
#define AURA_FONT_H

#include "paint.h"

/* ---- Faces ---- */

typedef struct AuFontFace {
    const uint8_t *data; /* the whole file (a .ttc holds several faces) */
    uint32_t len;
    uint32_t id; /* unique per face, for the glyph cache */

    /* Table offsets and lengths within data; 0 length when absent. */
    uint32_t cmap, cmap_len, head, head_len, hhea, hhea_len, hmtx, hmtx_len, maxp, maxp_len;
    uint32_t loca, loca_len, glyf, glyf_len, kern, kern_len, os2, os2_len, name, name_len;
    uint32_t post, post_len, cff, cff_len;

    uint16_t units_per_em, num_glyphs, num_hmetrics;
    int16_t loca_format; /* 0 short, 1 long */

    uint32_t cmap_sub;   /* the chosen cmap subtable */
    uint16_t cmap_format;
    bool cmap_macroman;  /* the subtable is Mac Roman (1, 0) */
    bool cmap_symbol;    /* (3, 0): code points live at U+F000 + byte */

    uint32_t kern_pairs; /* format 0 pair array, or 0 */
    uint32_t kern_npairs;

    /* Metrics in font units. Descender is negative, as in the font. */
    int16_t ascender, descender, line_gap;
    int16_t x_height, cap_height;
    int16_t underline_pos, underline_thickness;
    int16_t strike_pos, strike_thickness;
    int16_t xmin, ymin, xmax, ymax;
    uint16_t weight; /* OS/2 usWeightClass, 400 when absent */
    bool italic;

    char family[64];   /* typographic family (name 16), else family (1) */
    char subfamily[64];
    char full_name[96];
} AuFontFace;

/* The number of faces in a font file: 1 for a .ttf, n for a .ttc, 0 when
 * the data is not an sfnt. */
int au_font_face_count(const void *data, size_t len);
/* Parses face index of the file. NULL when the data is not a usable
 * TrueType font. data must outlive the face. */
AuFontFace *au_font_face_new(const void *data, size_t len, int index);
void au_font_face_free(AuFontFace *f);

/* The glyph for a Unicode code point; 0 (.notdef) when missing. */
uint16_t au_font_glyph(const AuFontFace *f, uint32_t cp);
/* Advance width in font units. */
int au_font_advance_units(const AuFontFace *f, uint16_t glyph);
/* Kerning between two glyphs in font units ('kern' format 0). */
int au_font_kern_units(const AuFontFace *f, uint16_t left, uint16_t right);
/* Appends the glyph's outline to path, scaled by (sx, sy) pixels per font
 * unit with the origin at (x, y) and y down. Returns false for a missing or
 * malformed glyph (the path is left as it was). */
bool au_font_glyph_path(const AuFontFace *f, uint16_t glyph, float sx, float sy, float x, float y,
                        AuPath *path);

/* ---- Metrics ---- */

typedef struct AuFontMetrics {
    float size;      /* the em, pixels */
    float ascent;    /* above the baseline, positive */
    float descent;   /* below the baseline, positive */
    float line_gap;
    float x_height;  /* CSS ex */
    float cap_height;
    float ch_advance; /* advance of "0": CSS ch */
    float space_advance;
    float underline_pos; /* below the baseline, positive */
    float underline_thickness;
    float strike_pos; /* above the baseline, positive */
    float strike_thickness;
} AuFontMetrics;

void au_font_metrics(const AuFontFace *f, float size, AuFontMetrics *m);

/* ---- Shaping, measuring and drawing ---- */

typedef struct AuGlyph {
    uint16_t id;
    float x;          /* pen position, pixels from the run start */
    float advance;    /* including kerning with the next glyph */
    uint32_t cluster; /* byte offset of its text */
} AuGlyph;

typedef struct AuGlyphRun {
    AuGlyph *glyphs;
    size_t n, cap;
    float width;
    size_t missing; /* glyphs the face lacked (id 0), for fallback */
} AuGlyphRun;

void au_glyph_run_init(AuGlyphRun *r);
void au_glyph_run_free(AuGlyphRun *r);
/* Shapes len bytes of UTF-8 at size pixels into r (replacing its
 * contents). */
void au_shape_text(const AuFontFace *f, float size, const char *text, size_t len, AuGlyphRun *r);
float au_text_width(const AuFontFace *f, float size, const char *text, size_t len);
/* Draws a shaped run with its origin (left end of the baseline) at
 * (x, y). */
void au_draw_glyph_run(AuCanvas *c, const AuFontFace *f, float size, const AuGlyphRun *r, float x,
                       float y, AuColor color);
/* Shapes and draws; returns the advance. */
float au_draw_text(AuCanvas *c, const AuFontFace *f, float size, float x, float y, const char *text,
                   size_t len, AuColor color);

/* ---- Glyph cache ----
 * Rasterized glyphs are cached process-wide, keyed by face, glyph, size
 * and horizontal subpixel position (quarter pixels), and evicted least
 * recently used when the bitmaps exceed the limit (default 1 MB). */
void au_glyph_cache_set_limit(size_t bytes);
void au_glyph_cache_clear(void);
typedef struct AuGlyphCacheStats {
    size_t entries, bytes, hits, misses, evictions;
} AuGlyphCacheStats;
void au_glyph_cache_stats(AuGlyphCacheStats *s);
/* Drops every cached glyph of a face; call before freeing it. */
void au_glyph_cache_forget_face(const AuFontFace *f);

/* ---- Font sets ----
 * The platform registers the fonts it has; layout asks for the best match
 * for a CSS font-family list, weight and style. */

typedef enum {
    AU_GENERIC_SERIF,
    AU_GENERIC_SANS_SERIF,
    AU_GENERIC_MONOSPACE,
    AU_GENERIC_SYSTEM_UI,
    AU_GENERIC_CURSIVE,
    AU_GENERIC_FANTASY,
    AU_GENERIC_COUNT
} AuGenericFamily;

typedef struct AuFontSet AuFontSet;

/* Loads a face's data on first use. Returns false when it cannot; *data
 * must then stay valid for the life of the set. */
typedef bool (*AuFontLoader)(void *ctx, const void **data, size_t *len);

AuFontSet *au_fontset_new(void);
/* Frees the set and its faces (not the font data, which the platform
 * owns). */
void au_fontset_free(AuFontSet *s);
/* Registers every face in a font file, reading family, weight and style
 * from the font. Returns the number added. data must outlive the set. */
int au_fontset_add_data(AuFontSet *s, const void *data, size_t len);
/* Registers one face whose data is loaded when it is first matched. */
void au_fontset_add_lazy(AuFontSet *s, const char *family, int weight, bool italic, int face_index,
                         AuFontLoader load, void *ctx);
/* Maps a generic family to an installed one. */
void au_fontset_set_generic(AuFontSet *s, AuGenericFamily g, const char *family);
/* The best face for a CSS font-family value ("Georgia, 'Times New Roman',
 * serif"), weight (100..900) and style. Falls back to sans-serif, then any
 * face. NULL only when the set is empty or nothing loads. */
AuFontFace *au_fontset_match(AuFontSet *s, const char *families, int weight, bool italic);
/* The same with the family list already split. */
AuFontFace *au_fontset_match_list(AuFontSet *s, const char *const *families, size_t n, int weight,
                                  bool italic);
size_t au_fontset_count(const AuFontSet *s);
/* Family, weight and style of entry i, for listings. */
const char *au_fontset_entry(const AuFontSet *s, size_t i, int *weight, bool *italic);

#endif
