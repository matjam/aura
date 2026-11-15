/* Fonts, text measurement with a word-width cache, per-character font
 * fallback, line-break classes and text drawing. */
#include "layout_int.h"

/* ---- Arena ---- */
struct LArenaChunk {
    LArenaChunk *next;
    size_t size;
};

#define AR_CHUNK 32768

void ar_init(LArena *a)
{
    memset(a, 0, sizeof *a);
}

void *ar_alloc(LArena *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    a->total += n;
    if ((size_t)(a->end - a->ptr) < n) {
        size_t sz = n > AR_CHUNK / 4 ? n : AR_CHUNK;
        size_t hdr = (sizeof(LArenaChunk) + 7) & ~(size_t)7;
        LArenaChunk *c = au_alloc(hdr + sz);
        c->size = sz;
        if (sz != AR_CHUNK) {
            /* A large block gets its own chunk; keep bumping in the current one. */
            c->next = a->chunks ? a->chunks->next : NULL;
            if (a->chunks)
                a->chunks->next = c;
            else
                a->chunks = c;
            void *p = (char *)c + hdr;
            memset(p, 0, n);
            return p;
        }
        c->next = a->chunks;
        a->chunks = c;
        a->ptr = (char *)c + hdr;
        a->end = a->ptr + sz;
    }
    void *p = a->ptr;
    a->ptr += n;
    memset(p, 0, n);
    return p;
}

char *ar_strndup(LArena *a, const char *s, size_t n)
{
    char *p = ar_alloc(a, n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void ar_free_all(LArena *a)
{
    LArenaChunk *c = a->chunks;
    while (c) {
        LArenaChunk *n = c->next;
        au_free(c);
        c = n;
    }
    memset(a, 0, sizeof *a);
}

/* ---- Fonts ---- */

static uint32_t font_hash(const void *fam, unsigned weight, bool italic, float size)
{
    uint32_t h = (uint32_t)(uintptr_t)fam * 2654435761u;
    h ^= weight * 40503u;
    h ^= italic ? 0x5bd1e995u : 0;
    h ^= (uint32_t)(size * 64) * 0x27d4eb2fu;
    return (h ^ (h >> 16)) & 63;
}

LFont *l_font(AuLayout *L, const AuStyle *s)
{
    const AuStyleInh *in = s->inh;
    float size = in->font_size;
    if (!(size > 0))
        size = 0.0001f;
    if (size > 2000)
        size = 2000;
    bool italic = in->font_style != AU_FONT_STYLE_NORMAL;
    uint32_t h = font_hash(in->font_family, in->font_weight, italic, size);
    for (LFont *f = L->font_hash[h]; f; f = f->next)
        if (f->family == in->font_family && f->weight == in->font_weight && f->italic == italic && f->size == size)
            return f;
    LFont *f = ar_alloc(&L->font_arena, sizeof *f);
    f->family = in->font_family;
    f->weight = in->font_weight;
    f->italic = italic;
    f->size = size;
    const char *names[16];
    size_t n = 0;
    if (in->font_family) {
        uint32_t cnt;
        const AuCssValue *it = l_items(&in->font_family->value, &cnt);
        for (uint32_t i = 0; i < cnt && n < 16; i++) {
            size_t len;
            const char *nm = l_val_name(&it[i], &len);
            if (nm && *nm)
                names[n++] = nm;
        }
    }
    f->list = L->env.fonts ? au_fontlist_new(L->env.fonts, names, n, in->font_weight, italic) : NULL;
    f->face = au_fontlist_primary(f->list);
    if (f->face)
        au_font_metrics(f->face, size, &f->m);
    else {
        memset(&f->m, 0, sizeof f->m);
        f->m.size = size;
        f->m.ascent = size * 0.8f;
        f->m.descent = size * 0.2f;
        f->m.x_height = size * 0.5f;
        f->m.ch_advance = size * 0.5f;
        f->m.space_advance = size * 0.25f;
        f->m.underline_pos = size * 0.1f;
        f->m.underline_thickness = size / 14;
        f->m.strike_pos = size * 0.3f;
        f->m.strike_thickness = size / 14;
    }
    if (f->m.underline_thickness <= 0)
        f->m.underline_thickness = size / 14;
    if (f->m.strike_thickness <= 0)
        f->m.strike_thickness = f->m.underline_thickness;
    if (f->m.strike_pos <= 0)
        f->m.strike_pos = f->m.x_height > 0 ? f->m.x_height / 2 : size * 0.3f;
    if (f->m.x_height <= 0)
        f->m.x_height = size * 0.5f;
    f->next = L->font_hash[h];
    L->font_hash[h] = f;
    return f;
}

float l_line_height(const AuStyle *s, LFont *f)
{
    const AuLen *lh = &s->inh->line_height;
    switch (lh->kind) {
    case AU_LEN_PX:
        return lh->u.px;
    case AU_LEN_NUMBER:
        return lh->u.num * s->inh->font_size;
    case AU_LEN_PCT:
        return lh->u.pct * s->inh->font_size / 100;
    default:
        return f->m.ascent + f->m.descent + f->m.line_gap;
    }
}

void au_layout_css_metrics(void *layout, const AuStyle *style, float *ex_px, float *ch_px)
{
    AuLayout *L = layout;
    LFont *f = l_font(L, style);
    *ex_px = f->m.x_height;
    *ch_px = f->m.ch_advance;
}

/* ---- Measurement ----
 * Per-character font fallback is src/paint's: au_shape_text_list picks a
 * face per cluster from the LFont's family list. */
struct LWordEntry {
    LWordEntry *next;
    const void *face;
    float size, w;
    uint16_t len;
    char text[];
};

#define WORD_MAX 64
#define WORDS_LIMIT 60000

static uint32_t word_hash(const void *face, float size, const char *t, size_t n)
{
    uint32_t h = 2166136261u ^ (uint32_t)(uintptr_t)face ^ (uint32_t)(size * 16);
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)t[i]) * 16777619u;
    return h;
}

/* Shapes t with fallback, noting on f when another face was used. */
static void shape(LFont *f, const char *t, size_t n, AuGlyphRun *r)
{
    au_shape_text_list(f->list, f->size, t, n, r);
    if (!f->fallback)
        for (size_t i = 0; i < r->n; i++)
            if (r->glyphs[i].face != f->face) {
                f->fallback = true;
                break;
            }
}

/* Width of t with per-character fallback, no spacing. */
static float measure_uncached(AuLayout *L, LFont *f, const char *t, size_t n)
{
    if (!f->face)
        return n * f->size * 0.5f;
    shape(f, t, n, &L->run);
    return L->run.width;
}

bool l_text_fallback_extents(AuLayout *L, LFont *f, const char *t, size_t n, float *ascent, float *descent)
{
    *ascent = *descent = 0;
    if (!f->face || !f->fallback || !n)
        return false;
    shape(f, t, n, &L->run);
    au_glyph_run_fallback_extents(&L->run, f->face, f->size, ascent, descent);
    return *ascent > 0 || *descent > 0;
}

float l_measure_raw(AuLayout *L, LFont *f, const char *t, size_t n)
{
    if (!n)
        return 0;
    if (n > WORD_MAX)
        return measure_uncached(L, f, t, n);
    /* Keyed by the family list: its fallback chain decides the width. */
    uint32_t h = word_hash(f->list, f->size, t, n);
    if (L->words_cap) {
        for (LWordEntry *e = L->words[h & (L->words_cap - 1)]; e; e = e->next)
            if (e->face == f->list && e->size == f->size && e->len == n && !memcmp(e->text, t, n)) {
                L->stats.word_hits++;
                return e->w;
            }
    }
    L->stats.word_misses++;
    float w = measure_uncached(L, f, t, n);
    if (L->nwords >= WORDS_LIMIT) {
        ar_free_all(&L->word_arena);
        memset(L->words, 0, L->words_cap * sizeof *L->words);
        L->nwords = 0;
    }
    if (L->nwords + 1 > L->words_cap / 2) {
        uint32_t ncap = L->words_cap ? L->words_cap * 2 : 4096;
        LWordEntry **nw = au_calloc(ncap * sizeof *nw);
        for (uint32_t i = 0; i < L->words_cap; i++) {
            LWordEntry *e = L->words[i];
            while (e) {
                LWordEntry *nx = e->next;
                uint32_t hh = word_hash(e->face, e->size, e->text, e->len) & (ncap - 1);
                e->next = nw[hh];
                nw[hh] = e;
                e = nx;
            }
        }
        au_free(L->words);
        L->words = nw;
        L->words_cap = ncap;
    }
    LWordEntry *e = ar_alloc(&L->word_arena, sizeof *e + n);
    e->face = f->list;
    e->size = f->size;
    e->w = w;
    e->len = (uint16_t)n;
    memcpy(e->text, t, n);
    uint32_t slot = h & (L->words_cap - 1);
    e->next = L->words[slot];
    L->words[slot] = e;
    L->nwords++;
    return w;
}

/* Width including letter-spacing and word-spacing. Soft hyphens and zero
 * width characters measure 0 (the shaper maps them to whatever the font
 * has, so they are stripped first). */
float l_measure(AuLayout *L, LFont *f, const AuStyle *s, const char *t, size_t n)
{
    float w = 0;
    size_t i = 0, start = 0;
    while (i < n) {
        unsigned char c = (unsigned char)t[i];
        if (c == 0xC2 && i + 1 < n && (unsigned char)t[i + 1] == 0xAD) {
            w += l_measure_raw(L, f, t + start, i - start);
            i += 2;
            start = i;
        } else if (c == 0xE2 && i + 2 < n && (unsigned char)t[i + 1] == 0x80 &&
                   ((unsigned char)t[i + 2] == 0x8B || (unsigned char)t[i + 2] == 0x8C)) {
            w += l_measure_raw(L, f, t + start, i - start);
            i += 3;
            start = i;
        } else
            i++;
    }
    w += l_measure_raw(L, f, t + start, n - start);
    const AuStyleInhRare *r = s->inhr;
    if (r->letter_spacing != 0 || r->word_spacing != 0) {
        size_t chars = 0, spaces = 0;
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)t[i];
            if ((c & 0xC0) != 0x80)
                chars++;
            if (c == ' ' || (c == 0xC2 && i + 1 < n && (unsigned char)t[i + 1] == 0xA0))
                spaces++;
        }
        w += chars * r->letter_spacing + spaces * r->word_spacing;
    }
    return w;
}

void l_text_free_caches(AuLayout *L)
{
    au_glyph_run_free(&L->run);
    ar_free_all(&L->word_arena);
    au_free(L->words);
    L->words = NULL;
    L->words_cap = L->nwords = 0;
    for (int h = 0; h < 64; h++)
        for (LFont *f = L->font_hash[h]; f; f = f->next)
            au_fontlist_free(f->list);
    ar_free_all(&L->font_arena);
    memset(L->font_hash, 0, sizeof L->font_hash);
}

/* ---- Drawing ---- */

/* U+200D (zero width joiner) is left to the shaper, which draws nothing
 * for it but needs it to join emoji sequences. */
static bool skip_char(uint32_t cp)
{
    return cp == 0xAD || cp == 0x200B || cp == 0x200C || cp == 0xFEFF || cp == '\n' || cp == '\r' || cp == '\t';
}

void l_draw_text(AuLayout *L, AuCanvas *c, LFont *f, const AuStyle *s, const char *t, size_t n, float x, float y,
                 uint32_t color, float spacing)
{
    if (!f->face || !n)
        return;
    float ls = s->inhr->letter_spacing, ws = s->inhr->word_spacing + spacing;
    AuGlyphRun *run = &L->run; /* kept between calls: no allocation per fragment */
    float pen = x;
    size_t i = 0;
    while (i < n) {
        size_t adv;
        uint32_t cp = au_utf8_decode(t + i, n - i, &adv);
        if (skip_char(cp)) {
            if (cp == '\t')
                pen += f->m.space_advance;
            i += adv;
            continue;
        }
        size_t j = i + adv;
        while (j < n) {
            size_t a2;
            uint32_t c2 = au_utf8_decode(t + j, n - j, &a2);
            if (skip_char(c2))
                break;
            j += a2;
        }
        shape(f, t + i, j - i, run);
        L->stats.missing_glyphs += (uint32_t)run->missing;
        if (ls != 0 || ws != 0) {
            float extra = 0;
            for (size_t g = 0; g < run->n; g++) {
                run->glyphs[g].x += extra;
                const char *ch = t + i + run->glyphs[g].cluster;
                extra += ls;
                if (*ch == ' ' || ((unsigned char)ch[0] == 0xC2 && (unsigned char)ch[1] == 0xA0))
                    extra += ws;
            }
            run->width += extra;
        }
        au_draw_glyph_run(c, f->face, f->size, run, pen, y, color);
        pen += run->width;
        i = j;
    }
}

/* ---- Line-break classes (a pragmatic subset of UAX #14) ---- */
int l_break_class(uint32_t cp)
{
    if (cp == ' ')
        return BK_SPACE;
    if (cp == '\t')
        return BK_TAB;
    if (cp == '\n')
        return BK_NEWLINE;
    if (cp < 0x80)
        return cp == '-' ? BK_HYPHEN : (cp == ')' || cp == ']' || cp == '}' || cp == ',' || cp == '.' ||
                                        cp == ';' || cp == ':' || cp == '!' || cp == '?')
                                           ? BK_CLOSE
                                           : (cp == '(' || cp == '[' || cp == '{') ? BK_OPEN : BK_NONE;
    if (cp == 0xAD)
        return BK_SHY;
    if (cp == 0x200B)
        return BK_ZWSP;
    if (cp == 0x2010 || cp == 0x2013)
        return BK_HYPHEN;
    if (cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C || cp == 0xFF0E || cp == 0x300D || cp == 0x300F ||
        cp == 0xFF09 || cp == 0x3009 || cp == 0x300B || cp == 0x3011 || cp == 0xFF1A || cp == 0xFF1B ||
        cp == 0xFF01 || cp == 0xFF1F || cp == 0x30FC || (cp >= 0x3041 && cp <= 0x3049 && (cp & 1)) ||
        cp == 0x3063 || cp == 0x30C3)
        return BK_CLOSE;
    if (cp == 0x300C || cp == 0x300E || cp == 0xFF08 || cp == 0x3008 || cp == 0x300A || cp == 0x3010)
        return BK_OPEN;
    if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF))
        return BK_IDEO;
    return BK_NONE;
}
