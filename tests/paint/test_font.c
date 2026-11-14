/* Font tests: a TrueType font built here in memory (so the tests run
 * anywhere), macOS system fonts checked against known metrics when they are
 * present, font matching, the glyph cache, and malformed data. */
#include "test_paint.h"

#include <string.h>

/* ---- A minimal font built in memory ----
 * Glyphs: 0 .notdef (empty), 1 a square 100..900 (A), 2 a composite of
 * glyph 1 shifted right 1000 units (B), 3 a contour that starts off the
 * curve (C). units_per_em 1000. kern A,B = -100. */

typedef struct {
    uint8_t d[4096];
    size_t n;
} W;

static void w8(W *w, unsigned v) { w->d[w->n++] = (uint8_t)v; }
static void w16(W *w, unsigned v) { w8(w, v >> 8); w8(w, v); }
static void w32(W *w, uint32_t v) { w16(w, v >> 16); w16(w, v & 0xFFFF); }
static void pad4(W *w) { while (w->n & 3) w8(w, 0); }

typedef struct {
    const char *tag;
    size_t off, len;
} Tab;

static size_t build_font(uint8_t *out)
{
    W w = {{0}, 0};
    Tab t[10];
    int nt = 0;
    /* Directory placeholder: 12 + 16 * 9. */
    const int ntables = 9;
    w.n = 12 + 16 * ntables;
#define BEGIN(name) do { pad4(&w); t[nt].tag = name; t[nt].off = w.n; } while (0)
#define END() do { t[nt].len = w.n - t[nt].off; nt++; } while (0)
    BEGIN("cmap");
    w16(&w, 0); w16(&w, 1);          /* version, 1 subtable */
    w16(&w, 3); w16(&w, 1); w32(&w, 12); /* Windows Unicode BMP */
    /* format 4: segments [A..C] -> delta, [FFFF] */
    w16(&w, 4); w16(&w, 32); w16(&w, 0);
    w16(&w, 4); w16(&w, 4); w16(&w, 1); w16(&w, 0); /* segX2, searchRange, entrySel, rangeShift */
    w16(&w, 'C'); w16(&w, 0xFFFF);  /* endCode */
    w16(&w, 0);                     /* pad */
    w16(&w, 'A'); w16(&w, 0xFFFF);  /* startCode */
    w16(&w, (unsigned)(1 - 'A') & 0xFFFF); w16(&w, 1); /* idDelta */
    w16(&w, 0); w16(&w, 0);         /* idRangeOffset */
    END();
    BEGIN("glyf");
    size_t g0 = w.n - t[nt].off;
    size_t g1 = g0; /* glyph 0 is empty */
    /* glyph 1: square, one contour of four on-curve points */
    w16(&w, 1); w16(&w, 100); w16(&w, 0); w16(&w, 900); w16(&w, 800);
    w16(&w, 3);   /* endPts */
    w16(&w, 0);   /* no instructions */
    for (int i = 0; i < 4; i++)
        w8(&w, 1); /* on curve, long coordinates */
    w16(&w, 100); w16(&w, 800); w16(&w, 0); w16(&w, (unsigned)-800 & 0xFFFF); /* x: 100,900,900,100 */
    w16(&w, 0); w16(&w, 0); w16(&w, 800); w16(&w, 0);        /* y: 0,0,800,800 */
    pad4(&w);
    size_t g2 = w.n - t[nt].off;
    /* glyph 2: composite of glyph 1 at +1000, 0 */
    w16(&w, (unsigned)-1 & 0xFFFF); w16(&w, 1100); w16(&w, 0); w16(&w, 1900); w16(&w, 800);
    w16(&w, 0x0001 | 0x0002); w16(&w, 1); w16(&w, 1000); w16(&w, 0);
    pad4(&w);
    size_t g3 = w.n - t[nt].off;
    /* glyph 3: a diamond of four off-curve points (a rounded shape) */
    w16(&w, 1); w16(&w, 0); w16(&w, 0); w16(&w, 800); w16(&w, 800);
    w16(&w, 3); w16(&w, 0);
    for (int i = 0; i < 4; i++)
        w8(&w, 0); /* off curve */
    w16(&w, 400); w16(&w, 400); w16(&w, (unsigned)-400 & 0xFFFF); w16(&w, (unsigned)-400 & 0xFFFF);
    w16(&w, 0); w16(&w, 400); w16(&w, 400); w16(&w, (unsigned)-400 & 0xFFFF);
    pad4(&w);
    size_t g4 = w.n - t[nt].off;
    END();
    BEGIN("head");
    w32(&w, 0x00010000); w32(&w, 0); w32(&w, 0); w32(&w, 0x5F0F3CF5);
    w16(&w, 0); w16(&w, 1000);       /* flags, unitsPerEm */
    for (int i = 0; i < 4; i++) w32(&w, 0); /* created, modified */
    w16(&w, 0); w16(&w, 0); w16(&w, 1900); w16(&w, 800);
    w16(&w, 0); w16(&w, 8); w16(&w, 2); /* macStyle, lowestRec, direction */
    w16(&w, 0); w16(&w, 0);          /* indexToLocFormat short, glyphDataFormat */
    END();
    BEGIN("hhea");
    w32(&w, 0x00010000); w16(&w, 800); w16(&w, (unsigned)-200 & 0xFFFF); w16(&w, 50);
    for (int i = 0; i < 12; i++) w16(&w, 0);
    w16(&w, 4); /* numberOfHMetrics */
    END();
    BEGIN("hmtx");
    w16(&w, 500); w16(&w, 0); w16(&w, 1000); w16(&w, 100); w16(&w, 2000); w16(&w, 1100); w16(&w, 800); w16(&w, 0);
    END();
    BEGIN("kern");
    w16(&w, 0); w16(&w, 1);
    w16(&w, 0); w16(&w, 6 + 8 + 6); w16(&w, 0x0001); /* version, length, coverage: horizontal format 0 */
    w16(&w, 1); w16(&w, 6); w16(&w, 0); w16(&w, 0);
    w16(&w, 1); w16(&w, 2); w16(&w, (unsigned)-100 & 0xFFFF);
    END();
    BEGIN("loca");
    w16(&w, (unsigned)(g0 / 2)); w16(&w, (unsigned)(g1 / 2)); w16(&w, (unsigned)(g2 / 2));
    w16(&w, (unsigned)(g3 / 2)); w16(&w, (unsigned)(g4 / 2));
    END();
    BEGIN("maxp");
    w32(&w, 0x00005000); w16(&w, 4);
    END();
    BEGIN("name");
    w16(&w, 0); w16(&w, 1); w16(&w, 6 + 12);
    w16(&w, 3); w16(&w, 1); w16(&w, 0x409); w16(&w, 1); w16(&w, 16); w16(&w, 0);
    const char *nm = "TestSans";
    for (const char *c = nm; *c; c++) w16(&w, (unsigned char)*c);
    END();
    pad4(&w);
    size_t total = w.n;
    w.n = 0;
    w32(&w, 0x00010000); w16(&w, (unsigned)nt); w16(&w, 0); w16(&w, 0); w16(&w, 0);
    for (int i = 0; i < nt; i++) {
        w8(&w, t[i].tag[0]); w8(&w, t[i].tag[1]); w8(&w, t[i].tag[2]); w8(&w, t[i].tag[3]);
        w32(&w, 0); w32(&w, (uint32_t)t[i].off); w32(&w, (uint32_t)t[i].len);
    }
    CHECK(nt == ntables);
    memcpy(out, w.d, total);
    return total;
#undef BEGIN
#undef END
}

static double glyph_area(const AuFontFace *f, uint16_t g, float scale, int w, int h)
{
    AuPath p;
    au_path_init(&p);
    CHECK(au_font_glyph_path(f, g, scale, scale, 0, (float)h, &p));
    uint8_t *m = au_calloc((size_t)w * h);
    au_raster_mask(&p, AU_FILL_NONZERO, (AuIRect){0, 0, w, h}, m);
    double s = 0;
    for (int i = 0; i < w * h; i++)
        s += m[i] / 255.0;
    au_free(m);
    au_path_free(&p);
    return s;
}

static uint32_t rng = 12345;
static uint32_t next_rand(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

/* Parses, measures and renders text with a damaged copy of data; nothing
 * may crash or read out of bounds (run under AddressSanitizer). */
static void exercise(const uint8_t *data, size_t len)
{
    int n = au_font_face_count(data, len);
    for (int i = 0; i < n && i < 4; i++) {
        AuFontFace *f = au_font_face_new(data, len, i);
        if (!f)
            continue;
        AuFontMetrics m;
        au_font_metrics(f, 16, &m);
        AuSurface *s = au_surface_new(64, 24);
        AuCanvas *c = au_canvas_new(s);
        au_draw_text(c, f, 14, 1, 16, "AVWa\xC3\xA9g0", 9, AU_RGB(0, 0, 0));
        for (uint32_t g = 0; g < 64 && g < f->num_glyphs; g++) {
            AuPath p;
            au_path_init(&p);
            au_font_glyph_path(f, (uint16_t)(next_rand() % f->num_glyphs), 0.02f, 0.02f, 2, 20, &p);
            au_path_free(&p);
        }
        au_canvas_free(c);
        au_surface_free(s);
        au_font_face_free(f);
    }
}

/* AURA_FUZZ_SCALE=n multiplies the rounds and AURA_FUZZ_SEED picks
 * another sequence, for longer runs under the sanitizers. */
static void fuzz(const uint8_t *orig, size_t len, int rounds)
{
    const char *scale = getenv("AURA_FUZZ_SCALE"), *seed = getenv("AURA_FUZZ_SEED");
    if (scale && atoi(scale) > 0)
        rounds *= atoi(scale);
    if (seed)
        rng = (uint32_t)strtoul(seed, NULL, 10);
    uint8_t *buf = au_alloc(len);
    for (int r = 0; r < rounds; r++) {
        memcpy(buf, orig, len);
        int edits = 1 + (int)(next_rand() % 16);
        for (int e = 0; e < edits; e++) {
            size_t at;
            /* Mostly hit the first 4 KB (directory and small tables). */
            if (next_rand() & 1)
                at = next_rand() % (len < 4096 ? len : 4096);
            else
                at = next_rand() % len;
            buf[at] = (uint8_t)next_rand();
        }
        size_t cut = (next_rand() % 4 == 0) ? next_rand() % len : len;
        /* A fresh allocation of exactly cut bytes, so ASan sees overreads. */
        uint8_t *exact = au_alloc(cut ? cut : 1);
        memcpy(exact, buf, cut);
        exercise(exact, cut);
        au_free(exact);
    }
    au_free(buf);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = au_alloc((size_t)n);
    *len = fread(d, 1, (size_t)n, f);
    fclose(f);
    return d;
}

static void test_built_font(void)
{
    static uint8_t data[4096];
    size_t len = build_font(data);
    CHECK(au_font_face_count(data, len) == 1);
    AuFontFace *f = au_font_face_new(data, len, 0);
    CHECK(f);
    CHECK(!strcmp(f->family, "TestSans"));
    CHECK(f->units_per_em == 1000 && f->num_glyphs == 4);
    CHECK(au_font_glyph(f, 'A') == 1 && au_font_glyph(f, 'B') == 2 && au_font_glyph(f, 'C') == 3);
    CHECK(au_font_glyph(f, 'D') == 0 && au_font_glyph(f, 0x1F600) == 0);
    CHECK(au_font_advance_units(f, 1) == 1000 && au_font_advance_units(f, 2) == 2000);
    CHECK(au_font_kern_units(f, 1, 2) == -100 && au_font_kern_units(f, 2, 1) == 0);
    /* x-height measured from glyph x when there is no OS/2: absent here,
     * so it falls back to half the em. */
    AuFontMetrics m;
    au_font_metrics(f, 10, &m);
    CHECK_NEAR(m.ascent, 8, 1e-4);
    CHECK_NEAR(m.descent, 2, 1e-4);
    CHECK_NEAR(m.line_gap, 0.5, 1e-4);
    /* Shaping: A then B, kerned. */
    CHECK_NEAR(au_text_width(f, 10, "AB", 2), 10 + 20 - 1, 1e-4);
    CHECK_NEAR(au_text_width(f, 10, "BA", 2), 30, 1e-4);
    AuGlyphRun r;
    au_glyph_run_init(&r);
    au_shape_text(f, 10, "AxB", 3, &r);
    CHECK(r.n == 3 && r.missing == 1 && r.glyphs[2].cluster == 2);
    au_glyph_run_free(&r);
    /* Outlines: the square covers 800 x 800 units; at 0.05 px/unit that is
     * 40 x 40 px. The composite covers the same area. */
    CHECK_NEAR(glyph_area(f, 1, 0.05f, 64, 64), 1600, 2);
    CHECK_NEAR(glyph_area(f, 2, 0.05f, 128, 64), 1600, 2);
    /* All-off-curve contour: the curve runs through the implied midpoints,
     * a 400-unit square, plus four parabolic bulges of 2/3 * 400 * 100:
     * 266667 units^2, or 666.7 px at 0.05 px per unit (a little less once
     * flattened). */
    CHECK_NEAR(glyph_area(f, 3, 0.05f, 64, 64), 660, 8);
    /* Drawing puts ink where the glyph is. */
    AuSurface *s = au_surface_new(40, 20);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    AuCanvas *c = au_canvas_new(s);
    float adv = au_draw_text(c, f, 10, 2, 15, "A", 1, AU_RGB(0, 0, 0));
    CHECK_NEAR(adv, 10, 1e-4);
    CHECK(au_surface_get(s, 7, 12) == AU_RGB(0, 0, 0));
    CHECK(au_surface_get(s, 7, 17) == AU_RGB(255, 255, 255));
    au_canvas_free(c);
    au_surface_free(s);

    /* Glyph cache: a tiny limit evicts; hits are counted. */
    au_glyph_cache_clear();
    AuGlyphCacheStats st0, st1;
    au_glyph_cache_stats(&st0);
    s = au_surface_new(200, 60);
    c = au_canvas_new(s);
    au_draw_text(c, f, 20, 0, 30, "AAAA", 4, AU_RGB(0, 0, 0));
    au_glyph_cache_stats(&st1);
    CHECK(st1.misses - st0.misses <= 4 && st1.hits - st0.hits >= 2);
    au_glyph_cache_set_limit(600);
    for (int size = 10; size < 40; size++)
        au_draw_text(c, f, (float)size, 0, 40, "ABC", 3, AU_RGB(0, 0, 0));
    au_glyph_cache_stats(&st1);
    CHECK(st1.bytes <= 600 || st1.entries == 1);
    CHECK(st1.evictions > 0);
    au_glyph_cache_set_limit(1024 * 1024);
    au_canvas_free(c);
    au_surface_free(s);

    /* Font set: matching by family list, weight and generics. */
    AuFontSet *set = au_fontset_new();
    CHECK(au_fontset_add_data(set, data, len) == 1);
    au_fontset_set_generic(set, AU_GENERIC_SANS_SERIF, "TestSans");
    CHECK(au_fontset_match(set, "Nope, 'TestSans'", 400, false) != NULL);
    CHECK(au_fontset_match(set, "Nope, sans-serif", 700, true) != NULL);
    CHECK(au_fontset_match(set, "", 400, false) != NULL); /* fallback */
    au_fontset_free(set);

    au_font_face_free(f);
    /* Damaged copies of the built font. */
    fuzz(data, len, 3000);
    /* Every truncation. */
    for (size_t cut = 0; cut < len; cut += 3) {
        uint8_t *exact = au_alloc(cut ? cut : 1);
        memcpy(exact, data, cut);
        exercise(exact, cut);
        au_free(exact);
    }
}

static bool loaded_lazy;
static const uint8_t *lazy_data;
static size_t lazy_len;
static bool lazy_loader(void *ctx, const void **data, size_t *len)
{
    (void)ctx;
    loaded_lazy = true;
    *data = lazy_data;
    *len = lazy_len;
    return true;
}

static void test_matching(void)
{
    static uint8_t data[4096];
    size_t len = build_font(data);
    lazy_data = data;
    lazy_len = len;
    AuFontSet *set = au_fontset_new();
    /* Three weights of one family, loaded lazily; matching follows CSS. */
    au_fontset_add_lazy(set, "Fam", 300, false, 0, lazy_loader, NULL);
    au_fontset_add_lazy(set, "Fam", 400, false, 0, lazy_loader, NULL);
    au_fontset_add_lazy(set, "Fam", 700, false, 0, lazy_loader, NULL);
    au_fontset_add_lazy(set, "Fam", 400, true, 0, lazy_loader, NULL);
    CHECK(!loaded_lazy);
    CHECK(au_fontset_match(set, "fam", 400, false));
    CHECK(loaded_lazy);
    /* Which entry won is visible through the entry list order; check the
     * weight rules through the face pointers instead. */
    AuFontFace *w300 = au_fontset_match(set, "Fam", 300, false);
    AuFontFace *w400 = au_fontset_match(set, "Fam", 400, false);
    AuFontFace *w700 = au_fontset_match(set, "Fam", 700, false);
    AuFontFace *wit = au_fontset_match(set, "Fam", 400, true);
    CHECK(w300 && w400 && w700 && wit);
    CHECK(w300 != w400 && w400 != w700 && wit != w400);
    CHECK(au_fontset_match(set, "Fam", 500, false) == w400); /* 500: 400 before 700 */
    CHECK(au_fontset_match(set, "Fam", 600, false) == w700); /* 600: heavier first */
    CHECK(au_fontset_match(set, "Fam", 200, false) == w300); /* 200: lighter first */
    CHECK(au_fontset_match(set, "Fam", 900, false) == w700);
    CHECK(au_fontset_match(set, "Fam", 700, true) == wit);    /* style before weight */
    au_fontset_free(set);
}

static void test_system_fonts(void)
{
    size_t len;
    uint8_t *arial = read_file("/System/Library/Fonts/Supplemental/Arial.ttf", &len);
    if (!arial) {
        printf("paint: Arial.ttf not present, skipping system font checks\n");
    } else {
        AuFontFace *f = au_font_face_new(arial, len, 0);
        CHECK(f);
        CHECK(!strcmp(f->family, "Arial"));
        CHECK(f->weight == 400 && !f->italic);
        /* Arial's metrics (these have been stable across its versions). */
        CHECK(f->units_per_em == 2048);
        CHECK(f->ascender == 1854 && f->descender == -434 && f->line_gap == 67);
        CHECK(f->x_height == 1062 && f->cap_height == 1467);
        CHECK(au_font_glyph(f, 'A') == 36);
        CHECK(au_font_advance_units(f, au_font_glyph(f, 'A')) == 1366);
        CHECK(au_font_advance_units(f, au_font_glyph(f, ' ')) == 569);
        CHECK(f->underline_pos == -217 && f->underline_thickness == 150);
        CHECK(au_font_kern_units(f, au_font_glyph(f, 'A'), au_font_glyph(f, 'V')) < 0);
        CHECK(au_font_glyph(f, 0x20AC) != 0); /* euro */
        AuFontMetrics m;
        au_font_metrics(f, 16, &m);
        CHECK_NEAR(m.ascent, 1854 * 16.0 / 2048, 1e-3);
        CHECK_NEAR(m.x_height, 1062 * 16.0 / 2048, 1e-3);
        CHECK_NEAR(m.ch_advance, 1139 * 16.0 / 2048, 1e-3);
        au_font_face_free(f);
        fuzz(arial, len, 300);
        au_free(arial);
    }
    uint8_t *helv = read_file("/System/Library/Fonts/Helvetica.ttc", &len);
    if (!helv) {
        printf("paint: Helvetica.ttc not present, skipping collection checks\n");
    } else {
        int n = au_font_face_count(helv, len);
        CHECK(n >= 2);
        int found = 0;
        for (int i = 0; i < n; i++) {
            AuFontFace *f = au_font_face_new(helv, len, i);
            if (f && !strcmp(f->family, "Helvetica"))
                found++;
            au_font_face_free(f);
        }
        CHECK(found >= 2);
        CHECK(au_font_face_new(helv, len, n) == NULL);
        au_free(helv);
    }
    uint8_t *times = read_file("/System/Library/Fonts/Supplemental/Times New Roman.ttf", &len);
    if (times) {
        AuFontFace *f = au_font_face_new(times, len, 0);
        CHECK(f && !strcmp(f->family, "Times New Roman"));
        CHECK(f->units_per_em == 2048 && f->ascender == 1825 && f->descender == -443);
        au_font_face_free(f);
        au_free(times);
    }
}

void test_fonts(void)
{
    test_built_font();
    test_matching();
    test_system_fonts();
    /* Garbage: not a font. */
    static const uint8_t junk[] = "this is not a font at all, it is a string";
    CHECK(au_font_face_count(junk, sizeof junk) == 0);
    CHECK(au_font_face_new(junk, sizeof junk, 0) == NULL);
    CHECK(au_font_face_new(NULL, 0, 0) == NULL);
    static const uint8_t ttc[] = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(au_font_face_count(ttc, sizeof ttc) == 0);
}
