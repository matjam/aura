/* TrueType (sfnt) parsing: the table directory, cmap, metrics, names,
 * kerning and glyf outlines. Every read goes through the bounds-checked
 * readers below; a read outside the data returns 0. */
#include "font_internal.h"

#include <math.h>
#include <string.h>

/* ---- Bounds-checked big-endian readers ---- */

static inline bool in_range(const AuFontFace *f, uint32_t off, uint32_t n)
{
    return off <= f->len && n <= f->len - off;
}

static inline uint8_t u8(const AuFontFace *f, uint32_t off)
{
    return in_range(f, off, 1) ? f->data[off] : 0;
}

static inline uint16_t u16(const AuFontFace *f, uint32_t off)
{
    if (!in_range(f, off, 2))
        return 0;
    return (uint16_t)((f->data[off] << 8) | f->data[off + 1]);
}

static inline int16_t s16(const AuFontFace *f, uint32_t off)
{
    return (int16_t)u16(f, off);
}

static inline uint32_t u32(const AuFontFace *f, uint32_t off)
{
    if (!in_range(f, off, 4))
        return 0;
    const uint8_t *p = f->data + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static bool is_sfnt_version(uint32_t v)
{
    return v == 0x00010000 || v == TAG('t', 'r', 'u', 'e') || v == TAG('O', 'T', 'T', 'O');
}

int au_font_face_count(const void *data, size_t len)
{
    AuFontFace tmp;
    tmp.data = data;
    tmp.len = len > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)len;
    uint32_t v = u32(&tmp, 0);
    if (v == TAG('t', 't', 'c', 'f')) {
        uint32_t n = u32(&tmp, 8);
        if (n > 1024 || !in_range(&tmp, 12, n * 4))
            return 0;
        return (int)n;
    }
    return is_sfnt_version(v) ? 1 : 0;
}

/* ---- Mac Roman ---- */

static const uint16_t mac_roman_high[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

static int unicode_to_mac_roman(uint32_t cp)
{
    if (cp < 128)
        return (int)cp;
    for (int i = 0; i < 128; i++)
        if (mac_roman_high[i] == cp)
            return 128 + i;
    return -1;
}

/* ---- Names ---- */

static void utf8_put(char *out, size_t cap, size_t *n, uint32_t cp)
{
    char tmp[4];
    int k = au_utf8_encode(cp, tmp);
    if (*n + (size_t)k + 1 > cap)
        return;
    memcpy(out + *n, tmp, (size_t)k);
    *n += (size_t)k;
}

/* Copies name record nameid into out as UTF-8. Prefers Windows Unicode
 * English, then any Windows Unicode, then Mac Roman. */
static bool read_name(const AuFontFace *f, uint16_t nameid, char *out, size_t cap)
{
    out[0] = 0;
    if (!f->name_len)
        return false;
    uint32_t base = f->name;
    uint16_t count = u16(f, base + 2);
    uint32_t strings = base + u16(f, base + 4);
    int best = -1, best_score = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t r = base + 6 + i * 12;
        if (!in_range(f, r, 12))
            break;
        if (u16(f, r + 6) != nameid)
            continue;
        uint16_t plat = u16(f, r), enc = u16(f, r + 2), lang = u16(f, r + 4);
        int score = 0;
        if (plat == 3 && (enc == 1 || enc == 10))
            score = lang == 0x409 ? 4 : 3;
        else if (plat == 0)
            score = 2;
        else if (plat == 1 && enc == 0 && lang == 0)
            score = 1;
        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    if (best < 0)
        return false;
    uint32_t r = base + 6 + (uint32_t)best * 12;
    uint16_t plat = u16(f, r);
    uint32_t len = u16(f, r + 8), off = strings + u16(f, r + 10);
    if (!in_range(f, off, len))
        return false;
    size_t n = 0;
    if (plat == 1) {
        for (uint32_t i = 0; i < len; i++) {
            uint8_t ch = f->data[off + i];
            utf8_put(out, cap, &n, ch < 128 ? ch : mac_roman_high[ch - 128]);
        }
    } else {
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            uint32_t cp = u16(f, off + i);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < len) {
                uint32_t lo = u16(f, off + i + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            utf8_put(out, cap, &n, cp);
        }
    }
    out[n] = 0;
    return n > 0;
}

bool au_font_name_string(const AuFontFace *f, uint16_t nameid, char *out, size_t cap)
{
    if (!cap)
        return false;
    char tmp[256];
    if (!read_name(f, nameid, tmp, sizeof tmp))
        return false;
    size_t n = strlen(tmp);
    if (n >= cap) {
        n = cap - 1;
        while (n && (tmp[n] & 0xC0) == 0x80)
            n--; /* do not cut a UTF-8 sequence */
    }
    memcpy(out, tmp, n);
    out[n] = 0;
    return n > 0;
}

/* ---- cmap ---- */

static void choose_cmap(AuFontFace *f)
{
    if (f->cmap_len < 4)
        return;
    uint32_t base = f->cmap;
    uint16_t n = u16(f, base + 2);
    int best = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = base + 4 + i * 8;
        if (!in_range(f, r, 8))
            break;
        uint16_t plat = u16(f, r), enc = u16(f, r + 2);
        uint32_t off = u32(f, r + 4);
        if (off >= f->cmap_len)
            continue;
        uint32_t sub = base + off;
        uint16_t fmt = u16(f, sub);
        int score = 0;
        bool macroman = false, symbol = false;
        if (fmt == 12 && ((plat == 3 && enc == 10) || plat == 0))
            score = 6;
        else if (fmt == 4 && plat == 3 && enc == 1)
            score = 5;
        else if (fmt == 4 && plat == 0)
            score = 4;
        else if (fmt == 4 && plat == 3 && enc == 0)
            score = 3, symbol = true;
        else if ((fmt == 0 || fmt == 6) && plat == 1 && enc == 0)
            score = 2, macroman = true;
        if (score > best) {
            best = score;
            f->cmap_sub = sub;
            f->cmap_format = fmt;
            f->cmap_macroman = macroman;
            f->cmap_symbol = symbol;
        }
    }
}

static uint16_t cmap4(const AuFontFace *f, uint32_t sub, uint32_t cp)
{
    if (cp > 0xFFFF)
        return 0;
    uint16_t segx2 = u16(f, sub + 6);
    uint32_t segs = segx2 / 2;
    if (!segs)
        return 0;
    uint32_t ends = sub + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
    /* Binary search for the first segment whose end >= cp. */
    uint32_t lo = 0, hi = segs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (u16(f, ends + mid * 2) < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= segs)
        return 0;
    uint16_t start = u16(f, starts + lo * 2);
    if (cp < start)
        return 0;
    uint16_t delta = u16(f, deltas + lo * 2);
    uint32_t roff_at = ranges + lo * 2;
    uint16_t roff = u16(f, roff_at);
    if (roff == 0)
        return (uint16_t)(cp + delta);
    uint32_t gaddr = roff_at + roff + (cp - start) * 2;
    uint16_t g = u16(f, gaddr);
    return g ? (uint16_t)(g + delta) : 0;
}

static uint16_t cmap12(const AuFontFace *f, uint32_t sub, uint32_t cp)
{
    uint32_t n = u32(f, sub + 12);
    if (!in_range(f, sub + 16, 0) || n > (f->len - sub - 16) / 12)
        return 0;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t g = sub + 16 + mid * 12;
        uint32_t start = u32(f, g), end = u32(f, g + 4);
        if (cp < start) {
            hi = mid;
        } else if (cp > end) {
            lo = mid + 1;
        } else {
            uint32_t id = u32(f, g + 8) + (cp - start);
            return id < 65536 ? (uint16_t)id : 0;
        }
    }
    return 0;
}

uint16_t au_font_glyph(const AuFontFace *f, uint32_t cp)
{
    if (!f->cmap_sub)
        return 0;
    uint32_t sub = f->cmap_sub;
    uint16_t g = 0;
    if (f->cmap_macroman) {
        int c = unicode_to_mac_roman(cp);
        if (c < 0)
            return 0;
        if (f->cmap_format == 0) {
            g = u8(f, sub + 6 + (uint32_t)c);
        } else { /* 6 */
            uint16_t first = u16(f, sub + 6), count = u16(f, sub + 8);
            if ((uint32_t)c >= first && (uint32_t)c < (uint32_t)first + count)
                g = u16(f, sub + 10 + ((uint32_t)c - first) * 2);
        }
    } else if (f->cmap_format == 4) {
        g = cmap4(f, sub, cp);
        if (!g && f->cmap_symbol && cp < 0x100)
            g = cmap4(f, sub, 0xF000 + cp);
    } else if (f->cmap_format == 12) {
        g = cmap12(f, sub, cp);
    }
    return g < f->num_glyphs ? g : 0;
}

/* ---- Metrics and kerning ---- */

int au_font_advance_units(const AuFontFace *f, uint16_t glyph)
{
    if (!f->num_hmetrics)
        return 0;
    uint16_t i = glyph < f->num_hmetrics ? glyph : (uint16_t)(f->num_hmetrics - 1);
    int base = u16(f, f->hmtx + (uint32_t)i * 4);
    if (!f->varied || glyph >= f->num_glyphs)
        return base;
    /* Varied advances are cached per face: HVAR is a few table reads, but
     * phantom points need the glyph's whole variation data. */
    AuFontCache *c = au_font_cache(f);
    if (!c->adv) {
        c->adv = au_alloc((size_t)f->num_glyphs * sizeof *c->adv);
        memset(c->adv, 0xFF, (size_t)f->num_glyphs * sizeof *c->adv);
    }
    if (c->adv[glyph] != 0xFFFF)
        return c->adv[glyph];
    float a = floorf(au_fontvar_advance(f, glyph, base) + 0.5f);
    int v = a < 0 ? 0 : a > 0xFFFE ? 0xFFFE : (int)a;
    c->adv[glyph] = (uint16_t)v;
    return v;
}

AuFontCache *au_font_cache(const AuFontFace *f)
{
    if (!f->cache)
        ((AuFontFace *)f)->cache = au_calloc(sizeof(AuFontCache));
    return f->cache;
}

void au_font_cache_free(AuFontCache *c)
{
    if (!c)
        return;
    au_free(c->adv);
    au_gsub_free(c->gsub);
    au_colr_free(c->colr);
    au_free(c);
}

static void find_kern(AuFontFace *f)
{
    if (f->kern_len < 4)
        return;
    uint32_t base = f->kern;
    uint32_t sub, ntables;
    bool apple = u16(f, base) == 1; /* Apple: 32-bit version 0x00010000 */
    if (apple) {
        ntables = u32(f, base + 4);
        sub = base + 8;
    } else {
        ntables = u16(f, base + 2);
        sub = base + 4;
    }
    for (uint32_t t = 0; t < ntables && t < 64; t++) {
        uint32_t len, fmt, hdr;
        bool horizontal;
        if (apple) {
            len = u32(f, sub);
            uint16_t cov = u16(f, sub + 4);
            fmt = cov & 0xFF;
            horizontal = !(cov & 0x8000) && !(cov & 0x4000) && !(cov & 0x2000);
            hdr = 8;
        } else {
            len = u16(f, sub + 2);
            uint16_t cov = u16(f, sub + 4);
            fmt = cov >> 8;
            /* horizontal, not minimum values, not cross-stream */
            horizontal = (cov & 1) && !(cov & 2) && !(cov & 4);
            hdr = 6;
        }
        if (fmt == 0 && horizontal) {
            uint32_t n = u16(f, sub + hdr);
            uint32_t pairs = sub + hdr + 8;
            if (in_range(f, pairs, n * 6)) {
                f->kern_pairs = pairs;
                f->kern_npairs = n;
                return;
            }
        }
        if (len < hdr)
            return;
        sub += len;
    }
}

int au_font_kern_units(const AuFontFace *f, uint16_t left, uint16_t right)
{
    if (!f->kern_npairs)
        return 0;
    uint32_t key = ((uint32_t)left << 16) | right;
    uint32_t lo = 0, hi = f->kern_npairs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t p = f->kern_pairs + mid * 6;
        uint32_t k = u32(f, p);
        if (k < key)
            lo = mid + 1;
        else if (k > key)
            hi = mid;
        else
            return s16(f, p + 4);
    }
    return 0;
}

/* ---- Face setup ---- */

static uint32_t next_face_id = 1;

/* Bounding box of a glyph in font units, from its glyf header. */
static bool glyph_box(const AuFontFace *f, uint16_t g, int16_t box[4]);

AuFontFace *au_font_face_new(const void *data, size_t len, int index)
{
    if (!data || len < 12 || len > 0x7FFFFFFF)
        return NULL;
    AuFontFace *f = au_calloc(sizeof *f);
    f->data = data;
    f->len = (uint32_t)len;
    uint32_t dir = 0;
    if (u32(f, 0) == TAG('t', 't', 'c', 'f')) {
        uint32_t n = u32(f, 8);
        if (index < 0 || (uint32_t)index >= n || n > 1024)
            goto bad;
        dir = u32(f, 12 + (uint32_t)index * 4);
    } else if (index != 0) {
        goto bad;
    }
    if (!is_sfnt_version(u32(f, dir)))
        goto bad;
    uint16_t ntables = u16(f, dir + 4);
    if (!in_range(f, dir + 12, (uint32_t)ntables * 16))
        goto bad;
    for (uint32_t i = 0; i < ntables; i++) {
        uint32_t r = dir + 12 + i * 16;
        uint32_t tag = u32(f, r), off = u32(f, r + 8), tlen = u32(f, r + 12);
        if (!in_range(f, off, tlen))
            continue; /* a table past the end is treated as absent */
        uint32_t *o = NULL, *l = NULL;
        switch (tag) {
        case TAG('c', 'm', 'a', 'p'): o = &f->cmap, l = &f->cmap_len; break;
        case TAG('h', 'e', 'a', 'd'): o = &f->head, l = &f->head_len; break;
        case TAG('h', 'h', 'e', 'a'): o = &f->hhea, l = &f->hhea_len; break;
        case TAG('h', 'm', 't', 'x'): o = &f->hmtx, l = &f->hmtx_len; break;
        case TAG('m', 'a', 'x', 'p'): o = &f->maxp, l = &f->maxp_len; break;
        case TAG('l', 'o', 'c', 'a'): o = &f->loca, l = &f->loca_len; break;
        case TAG('g', 'l', 'y', 'f'): o = &f->glyf, l = &f->glyf_len; break;
        case TAG('k', 'e', 'r', 'n'): o = &f->kern, l = &f->kern_len; break;
        case TAG('O', 'S', '/', '2'): o = &f->os2, l = &f->os2_len; break;
        case TAG('n', 'a', 'm', 'e'): o = &f->name, l = &f->name_len; break;
        case TAG('p', 'o', 's', 't'): o = &f->post, l = &f->post_len; break;
        case TAG('C', 'F', 'F', ' '): o = &f->cff, l = &f->cff_len; break;
        case TAG('f', 'v', 'a', 'r'): o = &f->fvar, l = &f->fvar_len; break;
        case TAG('a', 'v', 'a', 'r'): o = &f->avar, l = &f->avar_len; break;
        case TAG('g', 'v', 'a', 'r'): o = &f->gvar, l = &f->gvar_len; break;
        case TAG('H', 'V', 'A', 'R'): o = &f->hvar, l = &f->hvar_len; break;
        case TAG('M', 'V', 'A', 'R'): o = &f->mvar, l = &f->mvar_len; break;
        case TAG('S', 'T', 'A', 'T'): o = &f->stat, l = &f->stat_len; break;
        case TAG('C', 'O', 'L', 'R'): o = &f->colr, l = &f->colr_len; break;
        case TAG('C', 'P', 'A', 'L'): o = &f->cpal, l = &f->cpal_len; break;
        case TAG('G', 'S', 'U', 'B'): o = &f->gsub, l = &f->gsub_len; break;
        }
        if (o) {
            *o = off;
            *l = tlen;
        }
    }
    /* Required tables. CFF-only fonts are rejected until CFF outlines are
     * written. */
    if (f->head_len < 54 || f->hhea_len < 36 || f->maxp_len < 6 || f->cmap_len < 4 || !f->glyf_len ||
        !f->loca_len || !f->hmtx_len)
        goto bad;
    f->units_per_em = u16(f, f->head + 18);
    if (f->units_per_em < 16 || f->units_per_em > 16384)
        goto bad;
    f->xmin = s16(f, f->head + 36);
    f->ymin = s16(f, f->head + 38);
    f->xmax = s16(f, f->head + 40);
    f->ymax = s16(f, f->head + 42);
    uint16_t mac_style = u16(f, f->head + 44);
    f->loca_format = s16(f, f->head + 50);
    f->num_glyphs = u16(f, f->maxp + 4);
    /* loca must hold num_glyphs + 1 entries; trust whichever is smaller. */
    uint32_t loca_entries = f->loca_len / (f->loca_format ? 4 : 2);
    if (loca_entries == 0)
        goto bad;
    if (f->num_glyphs > loca_entries - 1)
        f->num_glyphs = (uint16_t)(loca_entries - 1);
    f->num_hmetrics = u16(f, f->hhea + 34);
    if ((uint32_t)f->num_hmetrics * 4 > f->hmtx_len)
        f->num_hmetrics = (uint16_t)(f->hmtx_len / 4);
    if (!f->num_glyphs || !f->num_hmetrics)
        goto bad;

    f->ascender = s16(f, f->hhea + 4);
    f->descender = s16(f, f->hhea + 6);
    f->line_gap = s16(f, f->hhea + 8);
    f->weight = 400;
    f->italic = (mac_style & 2) != 0;
    if (mac_style & 1)
        f->weight = 700;
    if (f->os2_len >= 78) {
        int16_t aw = s16(f, f->os2 + 2);
        if (aw > 0 && aw < f->units_per_em * 2)
            f->avg_width = aw;
        uint16_t w = u16(f, f->os2 + 4);
        if (w >= 1 && w <= 1000)
            f->weight = w;
        f->strike_thickness = s16(f, f->os2 + 26);
        f->strike_pos = s16(f, f->os2 + 28);
        uint16_t sel = u16(f, f->os2 + 62);
        if (sel & 1)
            f->italic = true;
        int16_t ta = s16(f, f->os2 + 68), td = s16(f, f->os2 + 70), tg = s16(f, f->os2 + 72);
        if ((sel & 0x80) && ta) {
            /* USE_TYPO_METRICS */
            f->ascender = ta;
            f->descender = td;
            f->line_gap = tg;
            f->metric_src = 1;
        } else if (!f->ascender && !f->descender) {
            f->ascender = (int16_t)u16(f, f->os2 + 74);
            f->descender = (int16_t)-(int16_t)u16(f, f->os2 + 76);
            f->line_gap = 0;
            f->metric_src = 2;
        }
        if (u16(f, f->os2) >= 2 && f->os2_len >= 90) {
            f->x_height = s16(f, f->os2 + 86);
            f->cap_height = s16(f, f->os2 + 88);
        }
    }
    if (f->post_len >= 12) {
        f->underline_pos = s16(f, f->post + 8);
        f->underline_thickness = s16(f, f->post + 10);
    }
    choose_cmap(f);
    find_kern(f);
    /* Without OS/2 heights, measure x and H. */
    int16_t box[4];
    if (f->x_height <= 0 && glyph_box(f, au_font_glyph(f, 'x'), box))
        f->x_height = box[3];
    if (f->cap_height <= 0 && glyph_box(f, au_font_glyph(f, 'H'), box))
        f->cap_height = box[3];
    if (f->x_height <= 0)
        f->x_height = (int16_t)(f->units_per_em / 2);
    if (f->cap_height <= 0)
        f->cap_height = (int16_t)(f->units_per_em * 7 / 10);
    if (f->underline_thickness <= 0)
        f->underline_thickness = (int16_t)(f->units_per_em / 14);
    if (f->underline_pos == 0)
        f->underline_pos = (int16_t)-(f->units_per_em / 10);
    if (f->strike_thickness <= 0)
        f->strike_thickness = f->underline_thickness;
    if (f->strike_pos <= 0)
        f->strike_pos = (int16_t)(f->x_height / 2);

    if (!read_name(f, 16, f->family, sizeof f->family))
        read_name(f, 1, f->family, sizeof f->family);
    if (!read_name(f, 17, f->subfamily, sizeof f->subfamily))
        read_name(f, 2, f->subfamily, sizeof f->subfamily);
    read_name(f, 4, f->full_name, sizeof f->full_name);
    au_fontvar_init(f);
    f->color = f->colr_len >= 14 && f->cpal_len >= 12;
    f->id = next_face_id++;
    return f;
bad:
    au_free(f);
    return NULL;
}

AuFontFace *au_font_face_synthesize(const AuFontFace *f, unsigned synth)
{
    AuFontFace *v = au_alloc(sizeof *v);
    *v = *f;
    v->cache = NULL;
    au_fontvar_retain(v->var);
    v->synth = (uint8_t)(f->synth | (synth & (AU_FONT_SYNTH_BOLD | AU_FONT_SYNTH_ITALIC)));
    if (v->synth & AU_FONT_SYNTH_ITALIC)
        v->italic = true;
    if ((v->synth & AU_FONT_SYNTH_BOLD) && v->weight < 700)
        v->weight = 700;
    v->id = next_face_id++;
    return v;
}

void au_font_face_free(AuFontFace *f)
{
    if (!f)
        return;
    au_glyph_cache_forget_face(f);
    au_fontvar_release(f->var);
    au_font_cache_free(f->cache);
    au_free(f);
}

/* ---- glyf ---- */

/* Offset and length of glyph g in glyf; false when empty or invalid. */
static bool glyph_loc(const AuFontFace *f, uint16_t g, uint32_t *off, uint32_t *len)
{
    if (g >= f->num_glyphs)
        return false;
    uint32_t a, b;
    if (f->loca_format) {
        a = u32(f, f->loca + (uint32_t)g * 4);
        b = u32(f, f->loca + (uint32_t)g * 4 + 4);
    } else {
        a = (uint32_t)u16(f, f->loca + (uint32_t)g * 2) * 2;
        b = (uint32_t)u16(f, f->loca + (uint32_t)g * 2 + 2) * 2;
    }
    if (b <= a || b > f->glyf_len)
        return false;
    *off = f->glyf + a;
    *len = b - a;
    return *len >= 10;
}

static bool glyph_box(const AuFontFace *f, uint16_t g, int16_t box[4])
{
    uint32_t off, len;
    if (!g || !glyph_loc(f, g, &off, &len))
        return false;
    for (int i = 0; i < 4; i++)
        box[i] = s16(f, off + 2 + (uint32_t)i * 2);
    return true;
}

bool au_font_glyph_box(const AuFontFace *f, uint16_t glyph, int16_t box[4])
{
    return glyph_box(f, glyph, box);
}

/* Affine transform from font units: x' = a x + c y + e, y' = b x + d y + f. */
typedef struct {
    float a, b, c, d, e, f;
} Xform;

static void xf_apply(const Xform *m, float x, float y, float *ox, float *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

/* Glyph point limits: a glyph with more points than this is malformed for
 * our purposes (the largest real fonts stay far below it). */
#define MAX_POINTS 20000
#define MAX_COMPOSITE_DEPTH 8

static bool simple_glyph(const AuFontFace *f, uint16_t g, uint32_t off, uint32_t len, int ncont, const Xform *m,
                         AuPath *path)
{
    uint32_t end = off + len;
    uint32_t p = off + 10;
    if (ncont == 0)
        return true;
    if (p + (uint32_t)ncont * 2 + 2 > end)
        return false;
    int npts = 0;
    int prev = -1;
    for (int i = 0; i < ncont; i++) {
        int e = u16(f, p + (uint32_t)i * 2);
        if (e <= prev && i)
            return false;
        prev = e;
    }
    npts = prev + 1;
    if (npts <= 0 || npts > MAX_POINTS)
        return false;
    uint32_t endpts = p;
    p += (uint32_t)ncont * 2;
    uint16_t ilen = u16(f, p);
    p += 2 + ilen;
    if (p > end)
        return false;
    uint8_t *flags = au_alloc((size_t)npts);
    /* Room for the four phantom points that gvar deltas address. */
    float *xs = au_alloc((size_t)(npts + 4) * 2 * sizeof(float));
    float *ys = xs + npts + 4;
    uint16_t *ends = NULL;
    float *dxs = NULL;
    bool ok = false;
    for (int i = 0; i < npts;) {
        if (p >= end)
            goto out;
        uint8_t fl = f->data[p++];
        flags[i++] = fl;
        if (fl & 8) {
            if (p >= end)
                goto out;
            int rep = f->data[p++];
            while (rep-- > 0 && i < npts)
                flags[i++] = fl;
        }
    }
    int v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & 2) {
            if (p >= end)
                goto out;
            int d = f->data[p++];
            v += (fl & 16) ? d : -d;
        } else if (!(fl & 16)) {
            if (p + 2 > end)
                goto out;
            v += s16(f, p);
            p += 2;
        }
        xs[i] = (float)v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & 4) {
            if (p >= end)
                goto out;
            int d = f->data[p++];
            v += (fl & 32) ? d : -d;
        } else if (!(fl & 32)) {
            if (p + 2 > end)
                goto out;
            v += s16(f, p);
            p += 2;
        }
        ys[i] = (float)v;
    }
    if (f->varied) {
        int n = npts + 4;
        ends = au_alloc((size_t)ncont * sizeof *ends);
        for (int c = 0; c < ncont; c++)
            ends[c] = u16(f, endpts + (uint32_t)c * 2);
        for (int i = npts; i < n; i++)
            xs[i] = ys[i] = 0;
        dxs = au_calloc((size_t)n * 2 * sizeof(float));
        if (!au_fontvar_glyph_deltas(f, g, n, ends, ncont, xs, ys, dxs, dxs + n))
            goto out;
        for (int i = 0; i < npts; i++) {
            xs[i] += dxs[i];
            ys[i] += dxs[n + i];
        }
    }
    /* Emit contours. Off-curve points imply on-curve midpoints between
     * them. A contour may start off the curve. */
    int start = 0;
    for (int c = 0; c < ncont; c++) {
        int last = u16(f, endpts + (uint32_t)c * 2);
        int n = last - start + 1;
        if (n < 2) {
            start = last + 1;
            continue;
        }
        /* Find an on-curve start, or synthesise one. */
        float sx, sy;
        int first;
        if (flags[start] & 1) {
            xf_apply(m, xs[start], ys[start], &sx, &sy);
            first = 1;
        } else if (flags[last] & 1) {
            xf_apply(m, xs[last], ys[last], &sx, &sy);
            first = 0;
            n--; /* the last point is the start; do not visit it again */
        } else {
            xf_apply(m, (xs[start] + xs[last]) / 2, (ys[start] + ys[last]) / 2, &sx, &sy);
            first = 0;
        }
        au_path_move_to(path, sx, sy);
        bool have_ctrl = false;
        float cx = 0, cy = 0;
        for (int k = first; k < n; k++) {
            int i = start + k;
            float x, y;
            xf_apply(m, xs[i], ys[i], &x, &y);
            if (flags[i] & 1) {
                if (have_ctrl)
                    au_path_quad_to(path, cx, cy, x, y);
                else
                    au_path_line_to(path, x, y);
                have_ctrl = false;
            } else {
                if (have_ctrl)
                    au_path_quad_to(path, cx, cy, (cx + x) / 2, (cy + y) / 2);
                cx = x;
                cy = y;
                have_ctrl = true;
            }
        }
        if (have_ctrl)
            au_path_quad_to(path, cx, cy, sx, sy);
        au_path_close(path);
        start = last + 1;
    }
    ok = true;
out:
    au_free(flags);
    au_free(xs);
    au_free(ends);
    au_free(dxs);
    return ok;
}

/* A composite glyph's component: flags, glyph, offset and 2x2 matrix. */
#define MAX_COMPONENTS 64
typedef struct Component {
    uint16_t flags, glyph;
    float dx, dy, a, b, c, d;
} Component;

/* Reads the components of the composite at off (MAX_COMPONENTS at most).
 * Returns their count, or -1 when the glyph is malformed. */
static int read_components(const AuFontFace *f, uint32_t off, uint32_t len, Component *out)
{
    uint32_t end = off + len, p = off + 10;
    for (int count = 0; count < MAX_COMPONENTS; count++) {
        if (p + 4 > end)
            return -1;
        Component *k = &out[count];
        uint16_t flags = u16(f, p);
        k->flags = flags;
        k->glyph = u16(f, p + 2);
        p += 4;
        if (flags & 1) { /* ARG_1_AND_2_ARE_WORDS */
            if (p + 4 > end)
                return -1;
            k->dx = s16(f, p);
            k->dy = s16(f, p + 2);
            p += 4;
        } else {
            if (p + 2 > end)
                return -1;
            k->dx = (int8_t)f->data[p];
            k->dy = (int8_t)f->data[p + 1];
            p += 2;
        }
        k->a = 1, k->b = 0, k->c = 0, k->d = 1;
        if (flags & 8) { /* WE_HAVE_A_SCALE */
            if (p + 2 > end)
                return -1;
            k->a = k->d = s16(f, p) / 16384.0f;
            p += 2;
        } else if (flags & 0x40) { /* X_AND_Y_SCALE */
            if (p + 4 > end)
                return -1;
            k->a = s16(f, p) / 16384.0f;
            k->d = s16(f, p + 2) / 16384.0f;
            p += 4;
        } else if (flags & 0x80) { /* TWO_BY_TWO */
            if (p + 8 > end)
                return -1;
            k->a = s16(f, p) / 16384.0f;
            k->b = s16(f, p + 2) / 16384.0f;
            k->c = s16(f, p + 4) / 16384.0f;
            k->d = s16(f, p + 6) / 16384.0f;
            p += 8;
        }
        if (!(flags & 0x20)) /* MORE_COMPONENTS */
            return count + 1;
    }
    return -1; /* more components than any real glyph has */
}

/* budget counts the components left to visit across the whole glyph, so
 * a malicious font cannot make nested composites cost 256^depth. */
static bool glyph_outline(const AuFontFace *f, uint16_t g, const Xform *m, int depth, int *budget,
                          AuPath *path)
{
    uint32_t off, len;
    if (!glyph_loc(f, g, &off, &len))
        return g < f->num_glyphs; /* an empty glyph (space) is fine */
    int16_t ncont = s16(f, off);
    if (ncont >= 0)
        return simple_glyph(f, g, off, len, ncont, m, path);
    /* Composite: components with their own transforms. Depth is bounded by
     * MAX_COMPOSITE_DEPTH, so this recursion is bounded by a constant. */
    if (depth >= MAX_COMPOSITE_DEPTH)
        return false;
    /* On the heap: Mac OS 9 stacks are small and composites nest. */
    Component *comp = au_alloc(MAX_COMPONENTS * sizeof *comp);
    int ncomp = read_components(f, off, len, comp);
    bool ok = ncomp >= 0;
    if (ok && f->varied && ncomp) {
        /* gvar varies each component's offset (one point per component,
         * then the phantom points). */
        int n = ncomp + 4;
        float *d = au_calloc((size_t)n * 2 * sizeof(float));
        ok = au_fontvar_glyph_deltas(f, g, n, NULL, 0, NULL, NULL, d, d + n);
        for (int i = 0; ok && i < ncomp; i++)
            if (comp[i].flags & 2) {
                comp[i].dx += d[i];
                comp[i].dy += d[n + i];
            }
        au_free(d);
    }
    for (int i = 0; ok && i < ncomp; i++) {
        Component *k = &comp[i];
        float dx = k->dx, dy = k->dy;
        if (!(k->flags & 2)) /* point matching, not offsets: unsupported */
            dx = dy = 0;
        if (k->flags & 0x800) { /* SCALED_COMPONENT_OFFSET (Apple) */
            float tx = k->a * dx + k->c * dy, ty = k->b * dx + k->d * dy;
            dx = tx;
            dy = ty;
        }
        /* combined = parent * component */
        Xform cm;
        cm.a = m->a * k->a + m->c * k->b;
        cm.b = m->b * k->a + m->d * k->b;
        cm.c = m->a * k->c + m->c * k->d;
        cm.d = m->b * k->c + m->d * k->d;
        cm.e = m->a * dx + m->c * dy + m->e;
        cm.f = m->b * dx + m->d * dy + m->f;
        if (--*budget < 0 || !glyph_outline(f, k->glyph, &cm, depth + 1, budget, path))
            ok = false;
    }
    au_free(comp);
    return ok;
}

bool au_font_glyph_point_count(const AuFontFace *f, uint16_t g, int *npoints, bool *composite)
{
    uint32_t off, len;
    *npoints = 0;
    *composite = false;
    if (!glyph_loc(f, g, &off, &len))
        return g < f->num_glyphs;
    int16_t ncont = s16(f, off);
    if (ncont >= 0) {
        if (ncont == 0)
            return true;
        if (len < 10 + (uint32_t)ncont * 2)
            return false;
        *npoints = u16(f, off + 10 + (uint32_t)(ncont - 1) * 2) + 1;
        return *npoints <= MAX_POINTS;
    }
    Component *comp = au_alloc(MAX_COMPONENTS * sizeof *comp);
    int n = read_components(f, off, len, comp);
    au_free(comp);
    if (n < 0)
        return false;
    *npoints = n;
    *composite = true;
    return true;
}

bool au_font_glyph_path_m(const AuFontFace *f, uint16_t glyph, const float m[6], AuPath *path)
{
    Xform x = {m[0], m[1], m[2], m[3], m[4], m[5]};
    size_t nops = path->nops, npts = path->npts;
    int budget = 64;
    if (glyph_outline(f, glyph, &x, 0, &budget, path))
        return true;
    path->nops = nops;
    path->npts = npts;
    return false;
}

bool au_font_glyph_path(const AuFontFace *f, uint16_t glyph, float sx, float sy, float x, float y,
                        AuPath *path)
{
    /* Synthetic italic shears: x moves right by 0.2 per unit of height. */
    Xform m = {sx, 0, (f->synth & AU_FONT_SYNTH_ITALIC) ? 0.2f * sy : 0, -sy, x, y};
    size_t nops = path->nops, npts = path->npts;
    int budget = 64; /* real composites use a handful of components */
    if (glyph_outline(f, glyph, &m, 0, &budget, path)) {
        if (f->synth & AU_FONT_SYNTH_BOLD) {
            /* Strike again to the right; nonzero filling unions the two. */
            float size = sx * f->units_per_em;
            float t = size <= 9 ? 0 : size >= 36 ? 1 : (size - 9) / 27;
            m.e += size * (1.0f / 24 + t * (1.0f / 32 - 1.0f / 24));
            budget = 64;
            size_t pn = path->nops, pp = path->npts;
            if (!glyph_outline(f, glyph, &m, 0, &budget, path)) {
                path->nops = pn;
                path->npts = pp;
            }
        }
        return true;
    }
    path->nops = nops;
    path->npts = npts;
    return false;
}
