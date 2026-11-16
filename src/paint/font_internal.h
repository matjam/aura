/* Shared internals of the font engine (sfnt.c, fontvar.c, colr.c,
 * gsub.c, text.c). Not for use outside src/paint.
 *
 * Every read of font data goes through the bounds-checked readers here: a
 * read outside the data returns 0. Offsets are uint32_t and the readers
 * check off + n against the length without overflowing. */
#ifndef AURA_FONT_INTERNAL_H
#define AURA_FONT_INTERNAL_H

#include "font.h"

#define AU_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static inline bool au_fd_in(const AuFontFace *f, uint32_t off, uint32_t n)
{
    return off <= f->len && n <= f->len - off;
}

static inline uint8_t au_fd_u8(const AuFontFace *f, uint32_t off)
{
    return au_fd_in(f, off, 1) ? f->data[off] : 0;
}

static inline uint16_t au_fd_u16(const AuFontFace *f, uint32_t off)
{
    if (!au_fd_in(f, off, 2))
        return 0;
    return (uint16_t)((f->data[off] << 8) | f->data[off + 1]);
}

static inline int16_t au_fd_s16(const AuFontFace *f, uint32_t off)
{
    return (int16_t)au_fd_u16(f, off);
}

static inline uint32_t au_fd_u24(const AuFontFace *f, uint32_t off)
{
    if (!au_fd_in(f, off, 3))
        return 0;
    const uint8_t *p = f->data + off;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint32_t au_fd_u32(const AuFontFace *f, uint32_t off)
{
    if (!au_fd_in(f, off, 4))
        return 0;
    const uint8_t *p = f->data + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline int32_t au_fd_s32(const AuFontFace *f, uint32_t off)
{
    return (int32_t)au_fd_u32(f, off);
}

/* Adds two offsets, saturating at UINT32_MAX (which no read accepts). */
static inline uint32_t au_fd_add(uint32_t a, uint32_t b)
{
    return a > 0xFFFFFFFFu - b ? 0xFFFFFFFFu : a + b;
}

/* Copies name record nameid as UTF-8 (sfnt.c); false when absent. */
bool au_font_name_string(const AuFontFace *f, uint16_t nameid, char *out, size_t cap);

/* ---- Variations (fontvar.c) ---- */

/* Parses fvar, avar, gvar, HVAR and MVAR of a new face; f->var stays NULL
 * for a static font or one whose variation tables are unusable. */
void au_fontvar_init(AuFontFace *f);
/* Takes and drops a reference to the shared variation data (NULL is
 * fine). */
void au_fontvar_retain(AuFontVar *v);
void au_fontvar_release(AuFontVar *v);
/* Adds the gvar deltas of glyph g at f's coordinates to dx, dy (n points:
 * the outline points then the four phantom points). When ends is not NULL
 * (a simple glyph: ncont contour end indices, x and y the original
 * coordinates), points a tuple leaves untouched are interpolated (IUP).
 * Returns false when the glyph's variation data is malformed (the deltas
 * are then left partly applied: the caller treats the glyph as failed). */
bool au_fontvar_glyph_deltas(const AuFontFace *f, uint16_t g, int n, const uint16_t *ends, int ncont,
                             const float *x, const float *y, float *dx, float *dy);
/* The varied advance of glyph g (HVAR, else gvar phantom points), in font
 * units. */
float au_fontvar_advance(const AuFontFace *f, uint16_t g, int base);
/* Applies MVAR deltas to f's metrics at its coordinates. */
void au_fontvar_apply_metrics(AuFontFace *f);

/* Per-face caches built on first use; freed with the face. A synthesized
 * copy starts with none. */
typedef struct AuFontCache AuFontCache;
AuFontCache *au_font_cache(const AuFontFace *f);
void au_font_cache_free(AuFontCache *c);

/* The glyph's point count (outline points, without phantoms) and, for a
 * composite, its component count; false for a malformed glyph. Used for
 * phantom-point advances. */
bool au_font_glyph_point_count(const AuFontFace *f, uint16_t g, int *npoints, bool *composite);

/* ---- Paged faces (fontpage.c) ---- */

/* Parses face index of data (sfnt.c). With a pager, the glyf table is
 * the pager's (its record in data is not checked against len) and glyph
 * outlines are read through it. */
AuFontFace *au_font_face_parse(const void *data, size_t len, int index, AuFontPager *pager);
/* The length of the paged face's glyf table in the file. */
uint32_t au_font_pager_glyf_len(const AuFontPager *p);
/* n bytes at offset off of the paged face's glyf table: a pointer to the
 * kept copy of the glyph, valid until the next call on the pager, or, for
 * a glyph larger than the whole glyph cache, a copy that is also stored
 * in *own (au_alloc'd; the caller frees it). NULL when the range is
 * outside glyf, over AU_FONT_GLYPH_MAX bytes, or cannot be read. */
#define AU_FONT_GLYPH_MAX (256u * 1024)
const uint8_t *au_font_pager_glyf(AuFontPager *p, uint32_t off, uint32_t n, uint8_t **own);

/* ---- GSUB (gsub.c) ---- */
typedef struct AuGsub AuGsub;
AuGsub *au_gsub_new(const AuFontFace *f);
void au_gsub_free(AuGsub *g);
/* Applies the face's default-on single and ligature substitutions to
 * glyphs[0..n) in place; tags (may be NULL) travel with the glyphs and a
 * ligature's tag becomes 0. Returns the new count (never more than n). The
 * GSUB tables are parsed on first use and cached with the face. */
size_t au_gsub_apply(const AuFontFace *f, uint16_t *glyphs, uint32_t *tags, size_t n);

/* Appends glyph's outline through the affine transform m (x' = m0 x +
 * m2 y + m4, y' = m1 x + m3 y + m5; font units in, y up), unsynthesized.
 * False for a missing or malformed glyph (path unchanged). */
bool au_font_glyph_path_m(const AuFontFace *f, uint16_t glyph, const float m[6], AuPath *path);

/* ---- COLR (colr.c) ---- */
typedef struct AuColr AuColr;
AuColr *au_colr_new(const AuFontFace *f);
void au_colr_free(AuColr *c);

/* A colour glyph rasterized: premultiplied ARGB pixels (au_alloc'd) with
 * their offset from the pen (top: y down). */
typedef struct AuColrBitmap {
    int left, top, w, h;
    uint32_t *px;
    bool uses_fg; /* the foreground colour (palette entry 0xFFFF) was drawn */
} AuColrBitmap;
/* Whether glyph g of f has a colour version (COLR v0 or v1). */
bool au_colr_has(const AuFontFace *f, uint16_t g);
/* Renders the colour version of g at sx, sy pixels per font unit, the pen
 * at x offset ox, with fg as the foreground colour. The bitmap has at most
 * max_pixels pixels. False when g has no colour version, its paint graph
 * is malformed or empty, or it would be too large. */
bool au_colr_render(const AuFontFace *f, uint16_t g, float sx, float sy, float ox, AuColor fg, size_t max_pixels,
                    AuColrBitmap *out);

/* Shaping looks up the same few code points and glyph pairs for every
 * word, at layout and again at every paint. The face remembers the glyph
 * of each code point below AU_FONT_CMAP_DIRECT and the last kerning
 * answers (a direct-mapped table keyed by the glyph pair). */
#define AU_FONT_CMAP_DIRECT 0x250 /* Latin, Latin-1, Latin Extended-A and B */
#define AU_FONT_KERN_SLOTS 1024
typedef struct AuKernSlot {
    uint32_t key;  /* left << 16 | right; valid when used */
    int16_t value; /* font units */
    uint8_t used;
} AuKernSlot;

struct AuFontCache {
    uint16_t *glyph_of; /* AU_FONT_CMAP_DIRECT entries; 0xFFFF: not looked up */
    AuKernSlot *kern;   /* AU_FONT_KERN_SLOTS entries */
    uint16_t *adv;   /* varied advances (0xFFFF: not computed), num_glyphs entries */
    AuGsub *gsub;    /* parsed GSUB lookups, or NULL */
    bool gsub_done;
    AuColr *colr;
    bool colr_done;
};

#endif
