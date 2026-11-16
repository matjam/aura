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
 * - Complex scripts: au_shape_text_list segments text into clusters and
 *   picks a face per cluster, but does no reordering or joining (Arabic,
 *   Indic) and no bidirectional layout.
 * - Hinting: none. Outlines are unhinted; vertical metrics snap the
 *   baseline to whole pixels and the x-height to a whole pixel at text
 *   sizes. */
#ifndef AURA_FONT_H
#define AURA_FONT_H

#include "paint.h"

/* ---- Faces ---- */

/* Variable fonts (TrueType flavour: fvar, avar, gvar, HVAR, MVAR). A
 * variable face is registered once; an instance is a face of its own
 * (au_font_face_instance) whose outlines, advances and metrics come out
 * varied. At most AU_FONT_MAX_AXES axes are used; a font with more is
 * drawn at its default instance. */
#define AU_FONT_MAX_AXES 16

typedef struct AuFontAxis {
    uint32_t tag;          /* 'wght', 'wdth', 'opsz', 'ital', 'slnt', ... */
    float min, def, max;   /* user coordinates */
    uint16_t name_id;
    bool hidden;
} AuFontAxis;

/* The variation data shared by a variable face and its instances
 * (reference counted; fontvar.c). */
typedef struct AuFontVar AuFontVar;

typedef struct AuFontFace {
    const uint8_t *data; /* the whole file (a .ttc holds several faces) */
    uint32_t len;
    uint32_t id; /* unique per face, for the glyph cache */

    /* Table offsets and lengths within data; 0 length when absent. */
    uint32_t cmap, cmap_len, head, head_len, hhea, hhea_len, hmtx, hmtx_len, maxp, maxp_len;
    uint32_t loca, loca_len, glyf, glyf_len, kern, kern_len, os2, os2_len, name, name_len;
    uint32_t post, post_len, cff, cff_len;
    uint32_t fvar, fvar_len, avar, avar_len, gvar, gvar_len, hvar, hvar_len, mvar, mvar_len;
    uint32_t stat, stat_len, colr, colr_len, cpal, cpal_len, gsub, gsub_len;

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
    int16_t avg_width; /* OS/2 xAvgCharWidth, 0 when absent */
    uint16_t weight; /* OS/2 usWeightClass, 400 when absent */
    bool italic;
    uint8_t synth; /* AU_FONT_SYNTH_*: styles drawn by Aura, not the font */
    uint8_t metric_src; /* where ascender etc. came from: 0 hhea, 1 OS/2 typo, 2 OS/2 win (for MVAR) */
    bool color;   /* has COLR and CPAL: glyphs may draw in colour */

    /* Variations: var is NULL for a static face. coords are the normalized
     * coordinates (F2Dot14, after avar) of each axis; all zero is the
     * default instance. varied is true when any coordinate is not zero. */
    AuFontVar *var;
    int16_t coords[AU_FONT_MAX_AXES];
    bool varied;
    struct AuFontCache *cache; /* lazily built per-face caches */
    /* A paged face (au_font_face_new_paged): data holds every table but
     * glyf, which is read from the file through the pager. NULL when the
     * whole file is in data. */
    struct AuFontPager *pager;

    char family[64];  /* typographic family (name 16), else family (1) */
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

/* Paged faces: a large font file (a CJK face of megabytes) need not be in
 * memory. A pager reads one face's tables, all but glyf, into a compact
 * sfnt in memory, and reads glyph outlines from the file on demand in
 * blocks of AU_FONT_PAGE_SIZE bytes, keeping the AU_FONT_PAGE_SLOTS most
 * recently used. read(ctx, off, buf, n) reads n bytes at offset off of
 * the file into buf and returns false when it cannot. */
#define AU_FONT_PAGE_SIZE 16384
#define AU_FONT_PAGE_SLOTS 16
typedef bool (*AuFontReadFn)(void *ctx, uint32_t off, void *buf, uint32_t n);
typedef struct AuFontPager AuFontPager;
/* NULL when the file (file_len bytes) is not a TrueType font with a
 * face index, or its tables do not fit the limits. */
AuFontPager *au_font_pager_new(AuFontReadFn read, void *ctx, uint32_t file_len, int index);
/* Frees the pager; free its faces first. */
void au_font_pager_free(AuFontPager *p);
/* The pager's face (free it with au_font_face_free), or NULL. */
AuFontFace *au_font_face_new_paged(AuFontPager *p);
typedef struct AuFontPagerStats {
    size_t table_bytes; /* the tables held in memory */
    size_t block_bytes; /* the glyph blocks cached */
    size_t reads;       /* blocks read from the file so far */
    size_t hits;        /* glyph reads answered from the cache */
} AuFontPagerStats;
void au_font_pager_stats(const AuFontPager *p, AuFontPagerStats *st);

/* Web font containers (woff.c). au_font_sniff names a file's format by
 * its first bytes. au_font_decode turns WOFF 1.0, WOFF2 (collections
 * included) or a plain sfnt into sfnt bytes the functions above load, in
 * a new au_alloc'd buffer of at most max_out bytes. CFF-flavoured fonts
 * give AU_FONT_UNSUPPORTED; corrupt data, sizes that disagree with the
 * data, or output over max_out give AU_FONT_BAD. Table checksums in the
 * output are zero. */
typedef enum {
    AU_FONT_FORMAT_UNKNOWN,
    AU_FONT_FORMAT_SFNT,  /* TrueType or a collection */
    AU_FONT_FORMAT_CFF,   /* OpenType with CFF outlines: not drawn */
    AU_FONT_FORMAT_WOFF,
    AU_FONT_FORMAT_WOFF2
} AuFontFormat;
typedef enum { AU_FONT_OK, AU_FONT_BAD, AU_FONT_UNSUPPORTED } AuFontStatus;
AuFontFormat au_font_sniff(const void *data, size_t len);
AuFontStatus au_font_decode(const void *data, size_t len, size_t max_out, uint8_t **out, size_t *out_len);

/* Synthesized styles, for a family that lacks a bold or italic face (as
 * browsers do): bold strikes each outline twice, offset to the right by
 * 1/24 of the em at 9px down to 1/32 at 36px and up (Skia's fake bold
 * strength); italic shears the outlines by 0.2 (about 11 degrees).
 * Advances do not change, so text measures as the regular face. */
enum { AU_FONT_SYNTH_BOLD = 1, AU_FONT_SYNTH_ITALIC = 2 };
/* A copy of f that draws with the given synthesized styles (its own id
 * in the glyph cache; italic and weight say what it draws). Free it with
 * au_font_face_free before f's data goes. */
AuFontFace *au_font_face_synthesize(const AuFontFace *f, unsigned synth);

#define AU_FONT_TAG(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* The axes of a variable face: their count (0 for a static face) and the
 * range of axis i. */
int au_font_axis_count(const AuFontFace *f);
const AuFontAxis *au_font_axis(const AuFontFace *f, int i);
/* The index of the axis with the tag, or -1. */
int au_font_axis_find(const AuFontFace *f, uint32_t tag);
/* The normalized coordinate (F2Dot14, through avar) of a user value on
 * axis i, clamped to the axis range. */
int au_font_normalize(const AuFontFace *f, int axis, float value);
/* Named instances (fvar): the count, and the name ("Bold", from the name
 * table) and user coordinates (one per axis) of instance i. */
int au_font_named_instance_count(const AuFontFace *f);
bool au_font_named_instance(const AuFontFace *f, int i, char *name, size_t cap, float *coords);

/* A face that draws f at other axis positions: n (tag, user value) pairs;
 * axes not named keep f's coordinates, unknown tags are ignored. Its
 * outlines, advances and metrics (MVAR) are varied, its id is its own,
 * its weight and italic say what it draws, and its subfamily names the
 * matching named instance when there is one. For a static face it is a
 * plain copy. Free it with au_font_face_free before f's data goes (it
 * shares f's variation data by reference, so f itself may go first). */
typedef struct AuFontVariation {
    uint32_t tag;
    float value;
} AuFontVariation;
AuFontFace *au_font_face_instance(const AuFontFace *f, const AuFontVariation *v, size_t n);

/* The glyph for a Unicode code point; 0 (.notdef) when missing. */
uint16_t au_font_glyph(const AuFontFace *f, uint32_t cp);
/* Advance width in font units. */
int au_font_advance_units(const AuFontFace *f, uint16_t glyph);
/* The glyph's bounding box (xmin, ymin, xmax, ymax) in font units, y up,
 * from its glyf header. False for an empty or missing glyph. */
bool au_font_glyph_box(const AuFontFace *f, uint16_t glyph, int16_t box[4]);
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
    float avg_advance; /* average character width (OS/2), else ch_advance: form control sizes */
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
    float y;          /* offset from the baseline, pixels, down positive (placed marks) */
    const struct AuFontFace *face; /* the face it is drawn with; NULL: the run's face */
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
 * contents), with one face. Default-ignorable code points the face lacks
 * (variation selectors, zero-width joiners, soft hyphens) give no
 * glyph. */
void au_shape_text(const AuFontFace *f, float size, const char *text, size_t len, AuGlyphRun *r);
float au_text_width(const AuFontFace *f, float size, const char *text, size_t len);
/* Draws a shaped run with its origin (left end of the baseline) at
 * (x, y). A glyph with its own face is drawn with that face, else with
 * f. */
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
 * The platform registers the fonts it has (the system's, then the faces
 * bundled in Aura's Fonts folder); layout asks for the best match for a
 * CSS font-family list, weight and style.
 *
 * Matching tries, in order: the families the list names (up to its first
 * generic family), the alias-table targets of those families, the generic
 * family (the user's preference, then its alias row), and so on through
 * the list, then sans-serif. Names compare ASCII case-insensitively. */

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

/* Entry flags. */
enum {
    AU_FONT_BUNDLED = 1, /* from Aura's Fonts folder, not the system */
    AU_FONT_WEB = 2      /* from a page's @font-face */
};

AuFontSet *au_fontset_new(void);
/* A set layered over parent (a document's web fonts over the platform's
 * fonts): a family is looked up in the set's own faces first, then in
 * parent's; generic family preferences come from parent unless set here.
 * parent must outlive the set and is not freed with it. */
AuFontSet *au_fontset_new_layered(AuFontSet *parent);
/* Frees the set and its faces (not the font data, which the platform
 * owns, except web faces' data, which the set owns). */
void au_fontset_free(AuFontSet *s);

/* A web font face (@font-face): sfnt data the set takes ownership of
 * (au_alloc'd; freed with the set, or at once when it is not a usable
 * font), registered under family for weights wmin..wmax (the descriptor's
 * range; a variable font's wght axis is clamped to it) and the given
 * style. ranges holds nranges (first, last) code point pairs from
 * unicode-range (NULL: all); the face is used only for those characters.
 * Returns false when the data is not a font the engine draws. */
bool au_fontset_add_web(AuFontSet *s, void *data, size_t len, const char *family, int wmin, int wmax, bool italic,
                        const uint32_t *ranges, size_t nranges);

/* What a match asks for beyond the family list. weight is 1..1000;
 * stretch is the CSS font-stretch percentage (0: normal); size is the
 * font size in CSS px for optical sizing (0: none, as with
 * font-optical-sizing: none); settings are font-variation-settings, applied
 * last. A variable face takes these as axis positions (wght, wdth,
 * ital or slnt, opsz) instead of having them synthesized. */
typedef struct AuFontStyle {
    int weight;
    bool italic;
    float stretch;
    float size;
    const AuFontVariation *settings;
    size_t nsettings;
} AuFontStyle;
AuFontFace *au_fontset_match_style(AuFontSet *s, const char *families, const AuFontStyle *st);
/* Registers every face in a font file, reading family, weight and style
 * from the font. Returns the number added. data must outlive the set. */
int au_fontset_add_data(AuFontSet *s, const void *data, size_t len);
/* Registers one face whose data is loaded when it is first matched.
 * flags: AU_FONT_*. */
void au_fontset_add_lazy(AuFontSet *s, const char *family, int weight, bool italic, int face_index, unsigned flags,
                         AuFontLoader load, void *ctx);
/* A font file the platform can measure and read in pieces. When it is
 * first matched, a file of more than AU_FONT_PAGED_MIN bytes is opened as
 * a paged face (au_font_pager_new with read); a smaller one is loaded
 * whole with load. size returns false when the file cannot be opened. */
#define AU_FONT_PAGED_MIN (2u * 1024 * 1024)
typedef struct AuFontFileOps {
    AuFontLoader load;
    bool (*size)(void *ctx, uint32_t *len);
    AuFontReadFn read;
} AuFontFileOps;
/* Registers one face of such a file (ops must outlive the set). */
void au_fontset_add_file(AuFontSet *s, const char *family, int weight, bool italic, int face_index, unsigned flags,
                         const AuFontFileOps *ops, void *ctx);
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
unsigned au_fontset_entry_flags(const AuFontSet *s, size_t i);
/* Whether entry i's data has been loaded. */
bool au_fontset_entry_loaded(const AuFontSet *s, size_t i);
/* Whether a face of the family is registered (and has not failed to
 * load). */
bool au_fontset_has_family(const AuFontSet *s, const char *family, size_t len);

typedef struct AuFontSetStats {
    size_t loaded;       /* faces whose data is loaded */
    size_t loaded_bytes; /* their data (a paged face: its tables and cached blocks) */
    size_t paged;        /* loaded faces that are paged */
    size_t paged_reads;  /* glyph blocks they have read from their files */
    size_t loads;        /* faces loaded so far */
    size_t fallback_hits, fallback_misses; /* the per-set fallback cache */
} AuFontSetStats;
void au_fontset_stats(const AuFontSet *s, AuFontSetStats *st);

/* ---- The alias table (fontalias.c) ----
 * A row maps a family name, as pages write it, to the families tried in
 * its place, best first. Rows for generic families also name the
 * preference slot whose family is tried before the targets (-1: none). */
typedef struct AuFontAlias {
    const char *name;
    int generic;         /* AuGenericFamily, or -1 */
    const char *targets; /* comma-separated */
} AuFontAlias;
/* The row for a family or generic family name, or NULL. */
const AuFontAlias *au_font_alias(const char *family, size_t len);
/* The row when name is a CSS generic family keyword, else NULL. */
const AuFontAlias *au_font_generic(const char *name, size_t len);
/* Comma-separated faces tried for a character no listed family has: text
 * faces, and the faces tried first for emoji presentation. */
const char *au_font_text_fallbacks(void);
const char *au_font_emoji_fallbacks(void);

/* ---- Font lists: per-character fallback ----
 * A font list is a CSS font-family list resolved against a set, for one
 * weight and style. Its primary face is what au_fontset_match returns.
 * For a character the primary face lacks, the face is the first that has
 * it of: the list's other candidate families in matching order (so the
 * next families the page names, then their aliases, then the generic),
 * the text fallback faces (DejaVu Sans first), the emoji faces, and any
 * face already loaded. Characters with emoji presentation (by default, or
 * with U+FE0F) try the emoji faces first. Faces keep the list's weight and
 * style, synthesized when the family lacks them. Decisions are cached per
 * list and, for the fallback faces, per set by code point and style.
 * Faces load only when they are tried. */
typedef struct AuFontList AuFontList;
AuFontList *au_fontlist_new(AuFontSet *s, const char *const *families, size_t n, int weight, bool italic);
/* The same with a full style (stretch, optical size, variation settings;
 * the settings are copied). */
AuFontList *au_fontlist_new_style(AuFontSet *s, const char *const *families, size_t n, const AuFontStyle *st);
/* The same from a CSS font-family value. */
AuFontList *au_fontlist_parse(AuFontSet *s, const char *families, int weight, bool italic);
void au_fontlist_free(AuFontList *l);
AuFontFace *au_fontlist_primary(const AuFontList *l);
/* The face for a code point, or NULL when no face has it. */
AuFontFace *au_fontlist_face_for(AuFontList *l, uint32_t cp, bool emoji);
/* The face that has every code point of a cluster (a base and its
 * marks), or NULL. */
AuFontFace *au_fontlist_face_for_cluster(AuFontList *l, const uint32_t *cps, size_t n, bool emoji);

/* Shapes text with per-character fallback into r: the text is split into
 * clusters (a base with its combining marks, variation selectors and
 * emoji modifiers), each cluster is drawn with one face, and runs of a
 * face are kerned within themselves. Default-ignorable code points give
 * no glyph (so U+FE0F is dropped and an emoji ZWJ sequence draws its
 * components). r->missing counts glyphs no face had; they draw as the
 * primary face's missing-glyph box. Each glyph's face is set. */
void au_shape_text_list(AuFontList *l, float size, const char *text, size_t len, AuGlyphRun *r);
float au_text_width_list(AuFontList *l, float size, const char *text, size_t len);
float au_draw_text_list(AuCanvas *c, AuFontList *l, float size, float x, float y, const char *text, size_t len,
                        AuColor color);
/* The ascent and descent (each with half its line gap) of the faces in r
 * other than primary, at size; 0 when r uses no other face. For
 * line-height: normal, where a fallback face's metrics enlarge the line
 * only when its glyphs are on it. */
void au_glyph_run_fallback_extents(const AuGlyphRun *r, const AuFontFace *primary, float size, float *ascent,
                                   float *descent);

/* Unicode properties the segmenter uses (approximations of the UCD,
 * covering the scripts Aura draws). */
bool au_uc_is_mark(uint32_t cp);              /* combining marks: stay with their base */
bool au_uc_is_ignorable(uint32_t cp);         /* default-ignorable: never drawn */
bool au_uc_emoji_presentation(uint32_t cp);   /* Emoji_Presentation=Yes */

/* ---- The font index ----
 * A Fonts folder holds an index, fonts.idx, so the platform registers the
 * bundled faces without opening each font. One line per face, fields
 * separated by tabs:
 *
 *   path  face-index  weight  normal|italic  family
 *
 * path is relative to the folder with / between components. Lines
 * starting with # are comments. aura-host font-index writes it. */
typedef struct AuFontIndexEntry {
    char path[256];
    int face_index;
    int weight;
    bool italic;
    char family[64];
} AuFontIndexEntry;
typedef void (*AuFontIndexFn)(void *ctx, const AuFontIndexEntry *e);
/* Calls fn for each valid line; malformed lines are skipped. Returns the
 * number of entries. */
int au_font_index_parse(const char *text, size_t len, AuFontIndexFn fn, void *ctx);
/* Appends the index lines for one font file (every face in it). Returns
 * the number of faces. */
int au_font_index_write(AuBuf *b, const char *path, const void *data, size_t len);

#endif
