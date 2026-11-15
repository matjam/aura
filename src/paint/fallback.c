/* Shaping with per-character font fallback: cluster segmentation, the
 * Unicode properties it needs, and the line metrics of fallback faces. */
#include "font_internal.h"

#include <string.h>

/* ---- Unicode properties ---- */

typedef struct Range {
    uint32_t lo, hi;
} Range;

static bool in_table(const Range *t, size_t n, uint32_t cp)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < t[mid].lo)
            hi = mid;
        else if (cp > t[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

/* Combining marks (Mn, Me, and the Mc vowel signs of Indic scripts) of the
 * scripts Aura meets most, sorted. */
static const Range marks[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
    {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711},
    {0x0730, 0x074A}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1},
    {0x0EB4, 0x0EBC}, {0x0EC8, 0x0ECE}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF},
    {0x302A, 0x302F}, {0x3099, 0x309A}, {0xFE20, 0xFE2F},
};

/* Default-ignorable code points (DerivedCoreProperties), sorted. */
static const Range ignorables[] = {
    {0x00AD, 0x00AD}, {0x034F, 0x034F}, {0x061C, 0x061C}, {0x115F, 0x1160}, {0x17B4, 0x17B5},
    {0x180B, 0x180F}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x206F}, {0x3164, 0x3164},
    {0xFE00, 0xFE0F}, {0xFEFF, 0xFEFF}, {0xFFA0, 0xFFA0}, {0xFFF0, 0xFFF8}, {0x1BCA0, 0x1BCA3},
    {0x1D173, 0x1D17A}, {0xE0000, 0xE0FFF},
};

/* Emoji_Presentation=Yes (emoji-data.txt), merged into ranges, sorted. */
static const Range emoji[] = {
    {0x231A, 0x231B},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},   {0x23F3, 0x23F3},   {0x25FD, 0x25FE},
    {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},   {0x26D4, 0x26D4},
    {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},   {0x26FA, 0x26FA},   {0x26FD, 0x26FD},
    {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},
    {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F201}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F236}, {0x1F238, 0x1F23A}, {0x1F250, 0x1F251}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
};

#define COUNT(t) (sizeof(t) / sizeof((t)[0]))

bool au_uc_is_mark(uint32_t cp)
{
    if (cp < 0x0300)
        return false;
    if (cp >= 0x0900 && cp <= 0x0DFF) {
        /* The Brahmic blocks share one layout: signs at 00-03, vowel
         * signs and virama at 3A-4F (less a few letters), 51-57, 62-63. */
        unsigned o = cp & 0x7F;
        return o <= 0x03 || (o >= 0x3A && o <= 0x4F && o != 0x3D) || (o >= 0x51 && o <= 0x57) ||
               (o >= 0x62 && o <= 0x63);
    }
    return in_table(marks, COUNT(marks), cp);
}

bool au_uc_is_ignorable(uint32_t cp)
{
    return cp >= 0x00AD && in_table(ignorables, COUNT(ignorables), cp);
}

bool au_uc_emoji_presentation(uint32_t cp)
{
    return cp >= 0x231A && in_table(emoji, COUNT(emoji), cp);
}

static bool is_emoji_modifier(uint32_t cp)
{
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

/* Code points that join the cluster before them. */
static bool extends(uint32_t cp)
{
    return au_uc_is_mark(cp) || cp == 0x200D || cp == 0x200C || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xE0020 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF) || is_emoji_modifier(cp);
}

/* ---- Shaping ---- */

#define CLUSTER_MAX 8

static AuGlyph *push_glyph(AuGlyphRun *r)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 32;
        r->glyphs = au_realloc(r->glyphs, r->cap * sizeof(AuGlyph));
    }
    return &r->glyphs[r->n++];
}

/* Appends glyph id of face f, kerning it with the glyph before when that
 * one is of the same face. */
static void emit_id(AuGlyphRun *r, const AuFontFace *f, float size, uint16_t id, uint32_t cluster, float *pen)
{
    if (!id)
        r->missing++;
    float s = size / f->units_per_em;
    AuGlyph *g = push_glyph(r);
    g->id = id;
    g->face = f;
    g->cluster = cluster;
    g->y = 0;
    g->advance = au_font_advance_units(f, id) * s;
    if (r->n > 1 && g[-1].face == f) {
        float k = au_font_kern_units(f, g[-1].id, id) * s;
        if (k) {
            g[-1].advance += k;
            *pen += k;
        }
    }
    g->x = *pen;
    *pen += g->advance;
}

static void emit(AuGlyphRun *r, const AuFontFace *f, float size, uint32_t cp, uint32_t cluster, float *pen)
{
    emit_id(r, f, size, au_font_glyph(f, cp), cluster, pen);
}

static bool is_regional(uint32_t cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

/* An emoji sequence (ZWJ sequence, flag, keycap, modifier or tag
 * sequence) in face f: the font's ligature for it through GSUB. all holds
 * every code point of the cluster. Returns true when it emitted glyphs
 * (a ligature, or the components the font's GSUB left); false when the
 * font has no GSUB, so the caller draws the components as before. */
static bool emit_sequence(AuGlyphRun *r, const AuFontFace *f, float size, const uint32_t *all, size_t nall,
                          uint32_t cluster, float *pen)
{
    if (!f->gsub_len || nall < 2)
        return false;
    uint16_t g[CLUSTER_MAX * 2];
    uint32_t tag[CLUSTER_MAX * 2];
    size_t n = 0;
    for (size_t i = 0; i < nall; i++) {
        uint16_t id = au_font_glyph(f, all[i]);
        if (!id && (au_uc_is_ignorable(all[i]) || is_emoji_modifier(all[i])))
            continue; /* the font has no glyph for a joiner or selector: leave it out */
        g[n] = id;
        tag[n] = all[i];
        n++;
    }
    n = au_gsub_apply(f, g, tag, n);
    if (n == 1) {
        emit_id(r, f, size, g[0], cluster, pen);
        return true;
    }
    /* No ligature for the whole sequence: draw what is left, without the
     * joiners, selectors and modifiers (drawn alone they are swatches and
     * blanks). */
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (tag[i] && (au_uc_is_ignorable(tag[i]) || is_emoji_modifier(tag[i])))
            continue;
        emit_id(r, f, size, g[i], cluster, pen);
        any = true;
    }
    return any;
}

/* Places a combining mark over (or under) the base glyph at index base,
 * as HarfBuzz's fallback positioning does for fonts without GPOS (Aura
 * reads none): centred on the base's ink, raised above a tall base or
 * lowered below a deep one, and stacked on the marks before it. *top and
 * *bottom track the ink extent so far, in font units. */
static void place_mark(AuGlyphRun *r, const AuFontFace *f, float size, uint32_t cp, uint32_t cluster, size_t base,
                       int *top, int *bottom)
{
    uint16_t id = au_font_glyph(f, cp);
    if (!id)
        r->missing++;
    float s = size / f->units_per_em;
    AuGlyph *g = push_glyph(r);
    const AuGlyph *b = &r->glyphs[base];
    g->id = id;
    g->face = f;
    g->cluster = cluster;
    g->advance = 0;
    g->y = 0;
    int16_t bb[4], mb[4];
    float bc;
    if (au_font_glyph_box(f, b->id, bb))
        bc = (bb[0] + bb[2]) / 2.0f;
    else
        bc = au_font_advance_units(f, b->id) / 2.0f;
    if (!au_font_glyph_box(f, id, mb)) {
        g->x = b->x + bc * s - au_font_advance_units(f, id) * s / 2;
        return;
    }
    g->x = b->x + (bc - (mb[0] + mb[2]) / 2.0f) * s;
    int gap = f->units_per_em / 20;
    if (mb[1] > 0) {
        int dy = *top + gap - mb[1];
        if (dy < 0)
            dy = 0;
        g->y = -dy * s;
        *top = mb[3] + dy;
    } else if (mb[3] <= 0) {
        int dy = mb[3] - (*bottom - gap);
        if (dy < 0)
            dy = 0;
        g->y = dy * s;
        *bottom = mb[1] - dy;
    }
}

void au_shape_text_list(AuFontList *l, float size, const char *text, size_t len, AuGlyphRun *r)
{
    r->n = 0;
    r->width = 0;
    r->missing = 0;
    AuFontFace *primary = au_fontlist_primary(l);
    if (!primary)
        return;
    float pen = 0;
    size_t i = 0;
    while (i < len) {
        size_t start = i, adv;
        uint32_t base = (uint8_t)text[i] < 0x80 ? (adv = 1, (uint8_t)text[i]) : au_utf8_decode(text + i, len - i, &adv);
        i += adv;
        /* Fast path: an ASCII letter followed by ASCII in the primary
         * face, the common case. */
        if (base >= 0x20 && base < 0x7F && (i >= len || (uint8_t)text[i] < 0x80)) {
            if (au_font_glyph(primary, base)) {
                emit(r, primary, size, base, (uint32_t)start, &pen);
                continue;
            }
        }
        /* The cluster: the base, then the code points that extend it. An
         * emoji sequence also takes the code point after a zero width
         * joiner, and a regional indicator its partner (a flag). cps holds
         * what must be drawn (for choosing the face), all every code point
         * (for the font's ligatures). */
        uint32_t cps[CLUSTER_MAX], all[CLUSTER_MAX * 2];
        size_t ncp = 0, nall = 0;
        bool vs16 = false, vs15 = false, seq = false, joined = false;
        bool draw_base = !(base < 0x20 || base == 0x7F || au_uc_is_ignorable(base));
        if (draw_base)
            cps[ncp++] = base;
        all[nall++] = base;
        while (i < len) {
            uint32_t c = au_utf8_decode(text + i, len - i, &adv);
            bool join = joined && c >= 0x80 && !au_uc_is_ignorable(c);
            bool flag = is_regional(c) && nall == 1 && is_regional(base);
            if (!extends(c) && !join && !flag)
                break;
            i += adv;
            joined = c == 0x200D;
            if (nall < CLUSTER_MAX * 2)
                all[nall++] = c;
            if (c == 0x200D || flag || c == 0x20E3 || is_emoji_modifier(c) || (c >= 0xE0020 && c <= 0xE007F))
                seq = true;
            if (c == 0xFE0F)
                vs16 = true;
            else if (c == 0xFE0E)
                vs15 = true;
            else if (!au_uc_is_ignorable(c) && !is_emoji_modifier(c) && ncp < CLUSTER_MAX)
                cps[ncp++] = c; /* a mark or joined emoji to draw; modifiers need a ligature, so they go */
        }
        if (!ncp)
            continue;
        bool emoji = vs16 || (!vs15 && au_uc_emoji_presentation(cps[0]));
        AuFontFace *f = au_fontlist_face_for_cluster(l, cps, ncp, emoji);
        if (!f && ncp > 1) {
            /* No face has the whole cluster: the base's face draws it. */
            f = au_fontlist_face_for(l, cps[0], emoji);
        }
        if (!f)
            f = primary;
        if (seq && draw_base && emit_sequence(r, f, size, all, nall, (uint32_t)start, &pen))
            continue;
        size_t at = r->n;
        emit(r, f, size, cps[0], (uint32_t)start, &pen);
        if (ncp > 1 && draw_base) {
            /* Marks go over their base; the emoji of a sequence the font
             * cannot join follow one another. */
            int16_t bb[4];
            int top = 0, bottom = 0;
            if (au_font_glyph_box(f, r->glyphs[at].id, bb)) {
                top = bb[3];
                bottom = bb[1];
            }
            for (size_t k = 1; k < ncp; k++) {
                if (!seq || au_uc_is_mark(cps[k])) {
                    place_mark(r, f, size, cps[k], (uint32_t)start, at, &top, &bottom);
                } else {
                    at = r->n;
                    emit(r, f, size, cps[k], (uint32_t)start, &pen);
                    top = bottom = 0;
                    if (au_font_glyph_box(f, r->glyphs[at].id, bb)) {
                        top = bb[3];
                        bottom = bb[1];
                    }
                }
            }
        } else {
            for (size_t k = 1; k < ncp; k++)
                emit(r, f, size, cps[k], (uint32_t)start, &pen);
        }
    }
    r->width = pen;
}

float au_text_width_list(AuFontList *l, float size, const char *text, size_t len)
{
    AuGlyphRun r;
    au_glyph_run_init(&r);
    au_shape_text_list(l, size, text, len, &r);
    float w = r.width;
    au_glyph_run_free(&r);
    return w;
}

float au_draw_text_list(AuCanvas *c, AuFontList *l, float size, float x, float y, const char *text, size_t len,
                        AuColor color)
{
    AuFontFace *primary = au_fontlist_primary(l);
    if (!primary)
        return 0;
    AuGlyphRun r;
    au_glyph_run_init(&r);
    au_shape_text_list(l, size, text, len, &r);
    au_draw_glyph_run(c, primary, size, &r, x, y, color);
    float w = r.width;
    au_glyph_run_free(&r);
    return w;
}

void au_glyph_run_fallback_extents(const AuGlyphRun *r, const AuFontFace *primary, float size, float *ascent,
                                   float *descent)
{
    *ascent = *descent = 0;
    const AuFontFace *last = primary;
    for (size_t i = 0; i < r->n; i++) {
        const AuFontFace *f = r->glyphs[i].face;
        if (!f || f == last || f == primary)
            continue;
        last = f;
        AuFontMetrics m;
        au_font_metrics(f, size, &m);
        float a = m.ascent + m.line_gap / 2, d = m.descent + m.line_gap / 2;
        if (a > *ascent)
            *ascent = a;
        if (d > *descent)
            *descent = d;
    }
}
