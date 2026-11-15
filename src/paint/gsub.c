/* A minimal GSUB reader: single (type 1) and ligature (type 4)
 * substitution, reached through extension lookups (type 7), for the
 * default-on features ccmp, locl, rlig, liga and clig of the DFLT script
 * (else latn, else the first script). Enough for emoji fonts to turn ZWJ
 * sequences, flags, keycaps and skin-tone modifiers into their single
 * glyphs. Contextual lookups, lookup flags (GDEF mark filtering) and
 * feature variations are not read.
 *
 * Every read is bounds-checked through the au_fd_* readers; counts are
 * capped, and the work of one application is bounded. */
#include "font_internal.h"

#include <string.h>

#define MAX_LOOKUPS 128         /* lookups collected from the features */
#define MAX_SUBTABLES 64        /* per lookup */
#define MAX_LIGATURES 2048      /* tried per ligature set */
#define MAX_COMPONENTS 16       /* per ligature */
#define MAX_WORK 200000         /* component comparisons per application */

typedef struct Sub {
    uint16_t type; /* 1 or 4 */
    uint32_t off;  /* the subtable */
} Sub;

typedef struct Lookup {
    Sub *subs;
    uint16_t nsubs;
} Lookup;

struct AuGsub {
    Lookup *lookups;
    size_t nlookups;
};

static const uint32_t features[] = {
    AU_TAG('c', 'c', 'm', 'p'), AU_TAG('l', 'o', 'c', 'l'), AU_TAG('r', 'l', 'i', 'g'),
    AU_TAG('l', 'i', 'g', 'a'), AU_TAG('c', 'l', 'i', 'g'),
};

static bool wanted(uint32_t tag)
{
    for (size_t i = 0; i < sizeof features / sizeof features[0]; i++)
        if (features[i] == tag)
            return true;
    return false;
}

/* The default LangSys of the chosen script, or 0. */
static uint32_t pick_langsys(const AuFontFace *f, uint32_t base)
{
    uint32_t sl = base + au_fd_u16(f, base + 4);
    uint16_t n = au_fd_u16(f, sl);
    uint32_t dflt = 0, latn = 0, first = 0;
    for (uint32_t i = 0; i < n && i < 256; i++) {
        uint32_t r = sl + 2 + i * 6;
        uint32_t tag = au_fd_u32(f, r);
        uint32_t script = sl + au_fd_u16(f, r + 4);
        uint16_t def = au_fd_u16(f, script);
        if (!def)
            continue;
        uint32_t ls = script + def;
        if (!first)
            first = ls;
        if (tag == AU_TAG('D', 'F', 'L', 'T'))
            dflt = ls;
        else if (tag == AU_TAG('l', 'a', 't', 'n'))
            latn = ls;
    }
    return dflt ? dflt : latn ? latn : first;
}

static void add_lookup(uint16_t *list, size_t *n, uint16_t idx)
{
    for (size_t i = 0; i < *n; i++)
        if (list[i] == idx)
            return;
    if (*n < MAX_LOOKUPS)
        list[(*n)++] = idx;
}

static void add_feature(const AuFontFace *f, uint32_t fl, uint16_t fi, uint16_t *list, size_t *n, bool check_tag)
{
    uint16_t nf = au_fd_u16(f, fl);
    if (fi >= nf)
        return;
    uint32_t r = fl + 2 + (uint32_t)fi * 6;
    if (check_tag && !wanted(au_fd_u32(f, r)))
        return;
    uint32_t feat = fl + au_fd_u16(f, r + 4);
    uint16_t cnt = au_fd_u16(f, feat + 2);
    for (uint32_t i = 0; i < cnt && i < 256; i++)
        add_lookup(list, n, au_fd_u16(f, feat + 4 + i * 2));
}

AuGsub *au_gsub_new(const AuFontFace *f)
{
    if (f->gsub_len < 10 || au_fd_u16(f, f->gsub) != 1)
        return NULL;
    uint32_t base = f->gsub;
    uint32_t ls = pick_langsys(f, base);
    if (!ls)
        return NULL;
    uint32_t fl = base + au_fd_u16(f, base + 6);
    uint32_t ll = base + au_fd_u16(f, base + 8);
    uint16_t list[MAX_LOOKUPS];
    size_t n = 0;
    uint16_t req = au_fd_u16(f, ls + 2);
    if (req != 0xFFFF)
        add_feature(f, fl, req, list, &n, false);
    uint16_t nfi = au_fd_u16(f, ls + 4);
    for (uint32_t i = 0; i < nfi && i < 512; i++)
        add_feature(f, fl, au_fd_u16(f, ls + 6 + i * 2), list, &n, true);
    if (!n)
        return NULL;
    /* Lookups apply in lookup-list order. */
    for (size_t i = 1; i < n; i++)
        for (size_t k = i; k > 0 && list[k - 1] > list[k]; k--) {
            uint16_t t = list[k];
            list[k] = list[k - 1];
            list[k - 1] = t;
        }
    AuGsub *g = au_calloc(sizeof *g);
    g->lookups = au_calloc(n * sizeof *g->lookups);
    uint16_t nll = au_fd_u16(f, ll);
    for (size_t i = 0; i < n; i++) {
        if (list[i] >= nll)
            continue;
        uint32_t lk = ll + au_fd_u16(f, ll + 2 + (uint32_t)list[i] * 2);
        uint16_t type = au_fd_u16(f, lk), nsub = au_fd_u16(f, lk + 4);
        if (nsub > MAX_SUBTABLES)
            nsub = MAX_SUBTABLES;
        Lookup *L = &g->lookups[g->nlookups];
        L->subs = au_calloc((nsub ? nsub : 1) * sizeof *L->subs);
        for (uint32_t s = 0; s < nsub; s++) {
            uint32_t st = lk + au_fd_u16(f, lk + 6 + s * 2);
            uint16_t t = type;
            if (t == 7) { /* extension */
                if (au_fd_u16(f, st) != 1)
                    continue;
                t = au_fd_u16(f, st + 2);
                st = au_fd_add(st, au_fd_u32(f, st + 4));
            }
            if ((t == 1 || t == 4) && au_fd_in(f, st, 6)) {
                L->subs[L->nsubs].type = t;
                L->subs[L->nsubs].off = st;
                L->nsubs++;
            }
        }
        if (L->nsubs)
            g->nlookups++;
        else
            au_free(L->subs);
    }
    if (!g->nlookups) {
        au_gsub_free(g);
        return NULL;
    }
    return g;
}

void au_gsub_free(AuGsub *g)
{
    if (!g)
        return;
    for (size_t i = 0; i < g->nlookups; i++)
        au_free(g->lookups[i].subs);
    au_free(g->lookups);
    au_free(g);
}

/* The coverage index of glyph in the coverage table at cov, or -1. */
static int coverage(const AuFontFace *f, uint32_t cov, uint16_t glyph)
{
    uint16_t fmt = au_fd_u16(f, cov), n = au_fd_u16(f, cov + 2);
    uint32_t lo = 0, hi = n;
    if (fmt == 1) {
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint16_t g = au_fd_u16(f, cov + 4 + mid * 2);
            if (g < glyph)
                lo = mid + 1;
            else if (g > glyph)
                hi = mid;
            else
                return (int)mid;
        }
    } else if (fmt == 2) {
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint32_t r = cov + 4 + mid * 6;
            uint16_t s = au_fd_u16(f, r), e = au_fd_u16(f, r + 2);
            if (e < glyph)
                lo = mid + 1;
            else if (s > glyph)
                hi = mid;
            else
                return au_fd_u16(f, r + 4) + (glyph - s);
        }
    }
    return -1;
}

static bool single(const AuFontFace *f, uint32_t st, uint16_t *glyph)
{
    uint16_t fmt = au_fd_u16(f, st);
    int ci = coverage(f, st + au_fd_u16(f, st + 2), *glyph);
    if (ci < 0)
        return false;
    if (fmt == 1) {
        *glyph = (uint16_t)(*glyph + au_fd_s16(f, st + 4));
        return true;
    }
    if (fmt == 2 && ci < au_fd_u16(f, st + 4)) {
        *glyph = au_fd_u16(f, st + 6 + (uint32_t)ci * 2);
        return true;
    }
    return false;
}

/* Tries the ligatures of subtable st at position i of g[0..n). Returns the
 * number of glyphs consumed (the ligature replaces g[i]), or 0. */
static int ligature(const AuFontFace *f, uint32_t st, const uint16_t *g, size_t i, size_t n, uint16_t *out,
                    uint32_t *work)
{
    if (au_fd_u16(f, st) != 1)
        return 0;
    int ci = coverage(f, st + au_fd_u16(f, st + 2), g[i]);
    if (ci < 0 || ci >= au_fd_u16(f, st + 4))
        return 0;
    uint32_t set = st + au_fd_u16(f, st + 6 + (uint32_t)ci * 2);
    uint16_t nlig = au_fd_u16(f, set);
    for (uint32_t k = 0; k < nlig && k < MAX_LIGATURES; k++) {
        uint32_t lig = set + au_fd_u16(f, set + 2 + k * 2);
        uint16_t comps = au_fd_u16(f, lig + 2);
        if (comps < 1 || comps > MAX_COMPONENTS || i + comps > n)
            continue;
        bool match = true;
        for (uint32_t c = 1; c < comps && match; c++) {
            if (++*work > MAX_WORK)
                return 0;
            match = au_fd_u16(f, lig + 4 + (c - 1) * 2) == g[i + c];
        }
        if (match) {
            *out = au_fd_u16(f, lig);
            return comps;
        }
    }
    return 0;
}

size_t au_gsub_apply(const AuFontFace *f, uint16_t *glyphs, uint32_t *tags, size_t n)
{
    AuFontCache *c = au_font_cache(f);
    if (!c->gsub_done) {
        c->gsub_done = true;
        c->gsub = au_gsub_new(f);
    }
    const AuGsub *g = c->gsub;
    if (!g || !n)
        return n;
    uint32_t work = 0;
    for (size_t l = 0; l < g->nlookups; l++) {
        const Lookup *L = &g->lookups[l];
        for (size_t i = 0; i < n; i++) {
            for (size_t s = 0; s < L->nsubs; s++) {
                const Sub *S = &L->subs[s];
                if (S->type == 1) {
                    if (single(f, S->off, &glyphs[i]))
                        break;
                } else {
                    uint16_t lig;
                    int used = ligature(f, S->off, glyphs, i, n, &lig, &work);
                    if (used > 0) {
                        glyphs[i] = lig;
                        if (tags)
                            tags[i] = 0; /* a ligature is drawn */
                        memmove(glyphs + i + 1, glyphs + i + used, (n - i - used) * sizeof *glyphs);
                        if (tags)
                            memmove(tags + i + 1, tags + i + used, (n - i - used) * sizeof *tags);
                        n -= (size_t)used - 1;
                        break;
                    }
                }
            }
        }
    }
    return n;
}
