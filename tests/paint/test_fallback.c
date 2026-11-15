/* Bundled fonts, the alias table and per-character fallback, on the real
 * Fonts folder (assets/fonts): the index is current and every face parses
 * and draws; aliases resolve with and without a system face of the name;
 * text segments into one face per cluster; the fallback cache; faces load
 * only when needed; a damaged Fonts folder and index do not crash. */
#include "test_paint.h"
#include "fontdir.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define EXPECT(cond)                                                                     \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

static const char *family_of(AuFontSet *s, const char *families, int weight, bool italic)
{
    AuFontFace *f = au_fontset_match(s, families, weight, italic);
    return f ? f->family : "(none)";
}

#define EXPECT_FAMILY(set, families, want)                                                          \
    do {                                                                                           \
        const char *got_ = family_of(set, families, 400, false);                                   \
        if (strcmp(got_, want)) {                                                                  \
            fprintf(stderr, "%s:%d: '%s' matched %s, expected %s\n", __FILE__, __LINE__, families, got_, \
                    want);                                                                         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static int entry_of(AuFontSet *s, const char *family, int weight, bool italic)
{
    for (size_t i = 0; i < au_fontset_count(s); i++) {
        int w;
        bool it;
        const char *f = au_fontset_entry(s, i, &w, &it);
        if (!strcmp(f, family) && w == weight && it == italic)
            return (int)i;
    }
    return -1;
}

static bool loaded(AuFontSet *s, const char *family, int weight, bool italic)
{
    int i = entry_of(s, family, weight, italic);
    return i >= 0 && au_fontset_entry_loaded(s, (size_t)i);
}

static size_t count_loaded(AuFontSet *s)
{
    size_t n = 0;
    for (size_t i = 0; i < au_fontset_count(s); i++)
        n += au_fontset_entry_loaded(s, i);
    return n;
}

/* ---- The index and the faces ---- */

static void test_index_and_faces(void)
{
    AuBuf now;
    au_buf_init(&now);
    CHECK(au_host_font_index(AURA_FONTS_DIR, &now));
    char *idx;
    size_t len;
    char path[1024];
    snprintf(path, sizeof path, "%s/fonts.idx", AURA_FONTS_DIR);
    CHECK(au_platform_read_file(path, &idx, &len));
    if (len != now.len || memcmp(idx, now.data, len)) {
        fprintf(stderr, "assets/fonts/fonts.idx is out of date: run aura-host font-index assets/fonts\n");
        failures++;
    }
    au_free(idx);
    au_buf_free(&now);

    AuFontSet *set = au_fontset_new();
    AuHostFontDir *dir = au_host_add_font_dir(set, AURA_FONTS_DIR, AU_FONT_BUNDLED);
    EXPECT(au_host_font_dir_count(dir) == 16);
    EXPECT(au_fontset_count(set) == 16);
    /* Registering opens nothing. */
    EXPECT(count_loaded(set) == 0 && au_host_font_dir_loads(dir) == 0);
    for (size_t i = 0; i < au_fontset_count(set); i++) {
        EXPECT(au_fontset_entry_flags(set, i) & AU_FONT_BUNDLED);
        int w;
        bool it;
        const char *fam = au_fontset_entry(set, i, &w, &it);
        char q[80];
        snprintf(q, sizeof q, "'%s'", fam);
        AuFontFace *f = au_fontset_match(set, q, w, it);
        CHECK(f);
        EXPECT(!strcmp(f->family, fam) && f->synth == 0);
        EXPECT(f->cff_len == 0 && f->glyf_len > 0);
        /* Each draws ink: text, or an emoji for the emoji face. */
        bool emoji = !strcmp(fam, "Noto Emoji");
        const char *sample = emoji ? "\xF0\x9F\x98\x80" : "Hamburg 0123";
        AuSurface *s = au_surface_new(160, 40);
        au_surface_clear(s, AU_RGB(255, 255, 255));
        AuCanvas *c = au_canvas_new(s);
        float adv = au_draw_text(c, f, 20, 4, 28, sample, strlen(sample), AU_RGB(0, 0, 0));
        EXPECT(adv > 10);
        int ink = 0;
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < 160; x++)
                ink += au_surface_get(s, x, y) != AU_RGB(255, 255, 255);
        EXPECT(ink > 40);
        AuGlyphRun r;
        au_glyph_run_init(&r);
        au_shape_text(f, 20, sample, strlen(sample), &r);
        EXPECT(r.missing == 0);
        au_glyph_run_free(&r);
        au_canvas_free(c);
        au_surface_free(s);
    }
    au_fontset_free(set);
    au_host_font_dir_free(dir);
}

/* ---- Aliases ---- */

static void test_aliases_bundled_only(void)
{
    AuFontSet *set = au_fontset_new();
    AuHostFontDir *dir = au_host_add_font_dir(set, AURA_FONTS_DIR, AU_FONT_BUNDLED);
    EXPECT_FAMILY(set, "Arial", "Aura Sans");
    /* Only the face matched is loaded. */
    EXPECT(count_loaded(set) == 1 && loaded(set, "Aura Sans", 400, false));
    EXPECT_FAMILY(set, "ARIAL", "Aura Sans");
    EXPECT_FAMILY(set, "Helvetica", "Aura Sans");
    EXPECT_FAMILY(set, "'Helvetica Neue', sans-serif", "Aura Sans");
    EXPECT_FAMILY(set, "Liberation Sans", "Aura Sans");
    EXPECT_FAMILY(set, "Arimo", "Aura Sans");
    EXPECT_FAMILY(set, "Verdana", "DejaVu Sans");
    EXPECT_FAMILY(set, "Tahoma", "DejaVu Sans");
    EXPECT_FAMILY(set, "Geneva", "DejaVu Sans");
    EXPECT_FAMILY(set, "Trebuchet MS", "Inter");
    EXPECT_FAMILY(set, "Segoe UI", "Inter");
    EXPECT_FAMILY(set, "Roboto", "Inter");
    EXPECT_FAMILY(set, "Open Sans", "Inter");
    EXPECT_FAMILY(set, "Lato", "Inter");
    EXPECT_FAMILY(set, "Noto Sans", "Inter");
    EXPECT_FAMILY(set, "Ubuntu", "Inter");
    EXPECT_FAMILY(set, "-apple-system, BlinkMacSystemFont, sans-serif", "Inter");
    EXPECT_FAMILY(set, "San Francisco", "Inter");
    EXPECT_FAMILY(set, "SF Pro", "Inter");
    EXPECT_FAMILY(set, "Inter", "Inter");
    EXPECT_FAMILY(set, "Lucida Grande", "DejaVu Sans");
    EXPECT_FAMILY(set, "Times", "Aura Serif");
    EXPECT_FAMILY(set, "'Times New Roman'", "Aura Serif");
    EXPECT_FAMILY(set, "Georgia", "Aura Serif");
    EXPECT_FAMILY(set, "Palatino", "Aura Serif");
    EXPECT_FAMILY(set, "Garamond", "Aura Serif");
    EXPECT_FAMILY(set, "Cambria", "Aura Serif");
    EXPECT_FAMILY(set, "Noto Serif", "Aura Serif");
    EXPECT_FAMILY(set, "Courier", "Aura Mono");
    EXPECT_FAMILY(set, "Courier New", "Aura Mono");
    EXPECT_FAMILY(set, "Monaco", "Aura Mono");
    EXPECT_FAMILY(set, "Menlo", "Aura Mono");
    EXPECT_FAMILY(set, "Consolas", "Aura Mono");
    EXPECT_FAMILY(set, "SFMono-Regular", "Aura Mono");
    EXPECT_FAMILY(set, "Lucida Console", "Aura Mono");
    EXPECT_FAMILY(set, "Andale Mono", "Aura Mono");
    EXPECT_FAMILY(set, "DejaVu Sans Mono", "Aura Mono");
    EXPECT_FAMILY(set, "Source Code Pro", "Aura Mono");
    EXPECT_FAMILY(set, "serif", "Aura Serif");
    EXPECT_FAMILY(set, "sans-serif", "Aura Sans");
    EXPECT_FAMILY(set, "monospace", "Aura Mono");
    EXPECT_FAMILY(set, "system-ui", "Aura Sans");
    EXPECT_FAMILY(set, "cursive", "Aura Serif");
    EXPECT_FAMILY(set, "fantasy", "Aura Serif");
    EXPECT_FAMILY(set, "ui-serif", "Aura Serif");
    EXPECT_FAMILY(set, "ui-sans-serif", "Aura Sans");
    EXPECT_FAMILY(set, "ui-monospace", "Aura Mono");
    EXPECT_FAMILY(set, "ui-rounded", "Inter");
    EXPECT_FAMILY(set, "math", "DejaVu Sans");
    EXPECT_FAMILY(set, "emoji", "Noto Emoji");
    EXPECT_FAMILY(set, "'Apple Color Emoji'", "Noto Emoji");
    EXPECT_FAMILY(set, "Nonexistent Family", "Aura Sans");
    EXPECT_FAMILY(set, "", "Aura Sans");
    /* A generic family ends the search: what follows it is not used. */
    EXPECT_FAMILY(set, "serif, Inter", "Aura Serif");
    /* A quoted generic name is a family name, not the generic. */
    EXPECT_FAMILY(set, "'monospace', serif", "Aura Serif");
    /* Styles: real faces where they exist, synthesized otherwise. */
    AuFontFace *f = au_fontset_match(set, "Arial", 700, true);
    EXPECT(f && f->synth == 0 && f->weight == 700 && f->italic);
    f = au_fontset_match(set, "Menlo", 400, true);
    EXPECT(f && !strcmp(f->family, "Aura Mono") && f->synth == AU_FONT_SYNTH_ITALIC);
    f = au_fontset_match(set, "Verdana", 700, false);
    EXPECT(f && f->synth == AU_FONT_SYNTH_BOLD);
    /* The table itself. */
    EXPECT(au_font_alias("helvetica neue", 14) != NULL);
    EXPECT(au_font_generic("Serif", 5) && au_font_generic("Serif", 5)->generic == AU_GENERIC_SERIF);
    EXPECT(au_font_generic("Arial", 5) == NULL);
    EXPECT(au_font_alias("No Such Font", 12) == NULL);
    au_fontset_free(set);
    au_host_font_dir_free(dir);
}

/* A "system" face: a lazily registered entry whose data is a bundled
 * file, under the family name the test gives it. */
static char *sys_data;
static size_t sys_len;
static int sys_loads;
static bool sys_loader(void *ctx, const void **data, size_t *len)
{
    (void)ctx;
    sys_loads++;
    *data = sys_data;
    *len = sys_len;
    return true;
}

static void test_aliases_with_system(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/inter/Inter-Regular.ttf", AURA_FONTS_DIR);
    CHECK(au_platform_read_file(path, &sys_data, &sys_len));
    AuFontSet *set = au_fontset_new();
    au_fontset_add_lazy(set, "Arial", 400, false, 0, 0, sys_loader, NULL);
    au_fontset_add_lazy(set, "Monaco", 400, false, 0, 0, sys_loader, NULL);
    AuHostFontDir *dir = au_host_add_font_dir(set, AURA_FONTS_DIR, AU_FONT_BUNDLED);
    int arial = entry_of(set, "Arial", 400, false);
    /* The installed face of the name comes first. */
    AuFontFace *a = au_fontset_match(set, "Arial", 400, false);
    EXPECT(a && au_fontset_entry_loaded(set, (size_t)arial) && !(au_fontset_entry_flags(set, (size_t)arial) & AU_FONT_BUNDLED));
    EXPECT(!loaded(set, "Aura Sans", 400, false));
    /* An installed alias target comes before the bundled one. */
    EXPECT(au_fontset_match(set, "Helvetica", 400, false) == a);
    EXPECT(au_fontset_match(set, "sans-serif", 400, false) != NULL);
    EXPECT(au_fontset_match(set, "Menlo", 400, false) == au_fontset_match(set, "Monaco", 400, false));
    /* A named family that is installed wins over the alias of an earlier
     * one: Roboto's alias is Inter, but the page also names Arial. */
    EXPECT(au_fontset_match(set, "Roboto, Arial, sans-serif", 400, false) == a);
    EXPECT_FAMILY(set, "Roboto, sans-serif", "Inter");
    /* The user's generic preference comes before the generic's row. */
    au_fontset_set_generic(set, AU_GENERIC_SERIF, "Monaco");
    EXPECT(au_fontset_match(set, "serif", 400, false) == au_fontset_match(set, "Monaco", 400, false));
    /* A preference naming a missing family falls through to the row. */
    au_fontset_set_generic(set, AU_GENERIC_SERIF, "Gone Font");
    EXPECT_FAMILY(set, "serif", "Aura Serif");
    au_fontset_set_generic(set, AU_GENERIC_SANS_SERIF, "Arial");
    EXPECT(au_fontset_match(set, "sans-serif", 400, false) == a);
    au_fontset_free(set);
    au_host_font_dir_free(dir);
    au_free(sys_data);
}

/* ---- Fallback ---- */

static size_t shape(AuFontList *l, const char *t, AuGlyphRun *r)
{
    au_shape_text_list(l, 16, t, strlen(t), r);
    return r->n;
}

static const char *glyph_family(const AuGlyphRun *r, size_t i)
{
    return i < r->n && r->glyphs[i].face ? r->glyphs[i].face->family : "(none)";
}

static void test_fallback(void)
{
    AuFontSet *set = au_fontset_new();
    AuHostFontDir *dir = au_host_add_font_dir(set, AURA_FONTS_DIR, AU_FONT_BUNDLED);
    AuFontList *l = au_fontlist_parse(set, "Arial, sans-serif", 400, false);
    AuFontFace *primary = au_fontlist_primary(l);
    CHECK(primary && !strcmp(primary->family, "Aura Sans"));
    AuGlyphRun r;
    au_glyph_run_init(&r);

    /* Latin, Greek and Cyrillic are in the primary face; nothing else
     * loads. */
    EXPECT(shape(l, "Abc \xCE\xB1\xCE\xB2 \xD0\x96", &r) == 8 && r.missing == 0);
    for (size_t i = 0; i < r.n; i++)
        EXPECT(r.glyphs[i].face == primary);
    EXPECT(count_loaded(set) == 1);
    EXPECT(!loaded(set, "DejaVu Sans", 400, false) && !loaded(set, "Noto Emoji", 400, false));

    /* A symbol the primary lacks comes from DejaVu Sans. */
    CHECK(!au_font_glyph(primary, 0x2318));
    EXPECT(shape(l, "a\xE2\x8C\x98" "b", &r) == 3 && r.missing == 0);
    EXPECT(r.glyphs[0].face == primary && r.glyphs[2].face == primary);
    EXPECT(!strcmp(glyph_family(&r, 1), "DejaVu Sans"));
    EXPECT(r.glyphs[1].cluster == 1 && r.glyphs[2].cluster == 4);
    EXPECT(!loaded(set, "Noto Emoji", 400, false));

    /* Emoji: a supplementary-plane code point, drawn by Noto Emoji. */
    EXPECT(shape(l, "x\xF0\x9F\x98\x80y", &r) == 3 && r.missing == 0);
    EXPECT(!strcmp(glyph_family(&r, 1), "Noto Emoji") && r.glyphs[2].cluster == 5);
    /* U+2764 with U+FE0F: emoji presentation, one glyph (FE0F dropped);
     * without it, text presentation from a text face. */
    EXPECT(shape(l, "\xE2\x9D\xA4\xEF\xB8\x8F", &r) == 1 && !strcmp(glyph_family(&r, 0), "Noto Emoji"));
    EXPECT(shape(l, "\xE2\x9D\xA4", &r) == 1 && !strcmp(glyph_family(&r, 0), "DejaVu Sans"));
    /* ...and U+FE0E asks for text presentation of an emoji. */
    EXPECT(shape(l, "\xE2\x98\x95\xEF\xB8\x8E", &r) == 1 && strcmp(glyph_family(&r, 0), "Noto Emoji"));
    EXPECT(shape(l, "\xE2\x98\x95", &r) == 1 && !strcmp(glyph_family(&r, 0), "Noto Emoji"));
    /* A ZWJ sequence draws its components; a skin tone modifier goes. */
    EXPECT(shape(l, "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", &r) == 2 && r.missing == 0);
    EXPECT(!strcmp(glyph_family(&r, 0), "Noto Emoji") && !strcmp(glyph_family(&r, 1), "Noto Emoji"));
    EXPECT(shape(l, "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", &r) == 1);
    /* A keycap: the digit and U+20E3 in one face, the keycap centred on
     * the digit with no advance. */
    EXPECT(shape(l, "1\xEF\xB8\x8F\xE2\x83\xA3", &r) == 2 && r.missing == 0);
    EXPECT(r.glyphs[0].face == r.glyphs[1].face && r.glyphs[1].advance == 0);

    /* Combining marks stay with their base, centred over it. */
    EXPECT(shape(l, "e\xCC\x81z", &r) == 3 && r.missing == 0);
    EXPECT(r.glyphs[0].face == primary && r.glyphs[1].face == primary);
    EXPECT(r.glyphs[0].cluster == 0 && r.glyphs[1].cluster == 0 && r.glyphs[2].cluster == 3);
    EXPECT(r.glyphs[1].advance == 0);
    EXPECT(r.glyphs[1].x > r.glyphs[0].x && r.glyphs[1].x < r.glyphs[0].x + r.glyphs[0].advance);
    {
        AuGlyphRun plain;
        au_glyph_run_init(&plain);
        au_shape_text_list(l, 16, "ez", 2, &plain);
        EXPECT(r.width == plain.width);
        au_glyph_run_free(&plain);
    }
    /* A mark over a capital sits above its ink (raised when it would
     * collide). */
    EXPECT(shape(l, "Z\xCC\x81", &r) == 2);
    {
        int16_t zb[4], mb[4];
        CHECK(au_font_glyph_box(primary, r.glyphs[0].id, zb) && au_font_glyph_box(primary, r.glyphs[1].id, mb));
        float units = 16.0f / primary->units_per_em;
        EXPECT(mb[1] - r.glyphs[1].y / units > zb[3]);
    }
    /* Stacked marks over a capital: the second goes above the first. */
    EXPECT(shape(l, "Z\xCC\x81\xCC\x81", &r) == 3 && r.glyphs[2].y < r.glyphs[1].y);
    /* A base and a mark no single face has: the base's face draws both. */
    EXPECT(shape(l, "\xE2\x8C\x98\xCC\x81", &r) == 2);
    EXPECT(r.glyphs[0].face == r.glyphs[1].face);

    /* Malformed UTF-8 (an encoded surrogate) becomes U+FFFD per byte. */
    EXPECT(shape(l, "\xED\xA0\x80", &r) == 3);
    for (size_t i = 0; i < r.n; i++)
        EXPECT(r.glyphs[i].id != 0 || r.missing > 0);
    /* A code point no face has: the primary's missing-glyph box. */
    EXPECT(shape(l, "a\xEE\x80\x80" "b", &r) == 3 && r.missing == 1);
    EXPECT(r.glyphs[1].face == primary && r.glyphs[1].id == 0);
    /* Default-ignorable code points draw nothing, even alone. */
    EXPECT(shape(l, "\xE2\x80\x8D\xEF\xB8\x8F\xC2\xAD", &r) == 0 && r.missing == 0);

    /* Style: the fallback face is synthesized to the list's weight. */
    AuFontList *bold = au_fontlist_parse(set, "Arial", 700, false);
    au_shape_text_list(bold, 16, "\xE2\x8C\x98", 3, &r);
    EXPECT(r.n == 1 && !strcmp(glyph_family(&r, 0), "DejaVu Sans") && (r.glyphs[0].face->synth & AU_FONT_SYNTH_BOLD));
    AuFontList *it = au_fontlist_parse(set, "Arial", 400, true);
    au_shape_text_list(it, 16, "\xE2\x8C\x98", 3, &r);
    EXPECT(r.n == 1 && r.glyphs[0].face->italic);

    /* The next family of the page's list comes before the last-resort
     * faces: a character Inter and DejaVu Sans have and Aura Mono lacks
     * comes from Inter when the page lists Inter second. */
    AuFontList *two = au_fontlist_parse(set, "'Aura Mono', Inter", 400, false);
    CHECK(!strcmp(au_fontlist_primary(two)->family, "Aura Mono"));
    AuFontFace *inter = au_fontset_match(set, "Inter", 400, false);
    AuFontFace *dejavu = au_fontset_match(set, "DejaVu Sans", 400, false);
    uint32_t probe = 0;
    for (uint32_t cp = 0x2000; cp < 0x2C00 && !probe; cp++)
        if (au_font_glyph(inter, cp) && au_font_glyph(dejavu, cp) && !au_font_glyph(au_fontlist_primary(two), cp) &&
            !au_uc_emoji_presentation(cp) && !au_uc_is_ignorable(cp) && !au_uc_is_mark(cp))
            probe = cp;
    CHECK(probe);
    char pb[4];
    int pn = au_utf8_encode(probe, pb);
    au_shape_text_list(two, 16, pb, (size_t)pn, &r);
    EXPECT(r.n == 1 && r.glyphs[0].face == inter);
    /* ...and not from Inter when the page does not list it (the default
     * sans-serif faces and DejaVu Sans come first). */
    AuFontList *mono = au_fontlist_parse(set, "Courier New", 400, false);
    au_shape_text_list(mono, 16, pb, (size_t)pn, &r);
    EXPECT(r.n == 1 && r.missing == 0 && r.glyphs[0].face != inter);
    au_fontlist_free(mono);

    /* Line metrics: only faces other than the primary count. */
    float a, d;
    au_shape_text_list(l, 16, "abc", 3, &r);
    au_glyph_run_fallback_extents(&r, primary, 16, &a, &d);
    EXPECT(a == 0 && d == 0);
    au_shape_text_list(l, 16, "a\xF0\x9F\x98\x80", 5, &r);
    au_glyph_run_fallback_extents(&r, primary, 16, &a, &d);
    EXPECT(a > 10 && d > 2);

    /* The cache: a code point the fallback faces resolved once per set
     * and style serves every list. The emoji above was resolved for l. */
    AuFontSetStats st0, st1;
    au_fontset_stats(set, &st0);
    AuFontList *other = au_fontlist_parse(set, "Helvetica", 400, false);
    au_shape_text_list(other, 16, "\xF0\x9F\x98\x80", 4, &r);
    au_fontset_stats(set, &st1);
    EXPECT(st1.fallback_hits == st0.fallback_hits + 1 && st1.fallback_misses == st0.fallback_misses);
    EXPECT(r.n == 1 && !strcmp(glyph_family(&r, 0), "Noto Emoji"));
    au_shape_text_list(other, 16, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 8, &r);
    au_fontset_stats(set, &st0);
    EXPECT(st0.fallback_hits == st1.fallback_hits); /* the list's own cache answered */
    /* Another style is another key. */
    au_shape_text_list(bold, 16, "\xF0\x9F\x98\x80", 4, &r);
    au_fontset_stats(set, &st1);
    EXPECT(st1.fallback_misses == st0.fallback_misses + 1);
    EXPECT(st0.loaded == count_loaded(set) && st0.loaded_bytes > 0);

    /* Drawing a mixed run puts ink for every glyph. */
    AuSurface *s = au_surface_new(200, 30);
    au_surface_clear(s, AU_RGB(255, 255, 255));
    AuCanvas *c = au_canvas_new(s);
    float w = au_draw_text_list(c, l, 16, 2, 20, "A\xE2\x8C\x98\xF0\x9F\x98\x80", 8, AU_RGB(0, 0, 0));
    EXPECT(w > 30);
    au_canvas_free(c);
    au_surface_free(s);

    au_glyph_run_free(&r);
    au_fontlist_free(l);
    au_fontlist_free(bold);
    au_fontlist_free(it);
    au_fontlist_free(two);
    au_fontlist_free(other);
    au_fontset_free(set);
    au_host_font_dir_free(dir);
}

/* An empty set: lists have no primary and shape nothing. */
static void test_empty(void)
{
    AuFontSet *set = au_fontset_new();
    AuFontList *l = au_fontlist_parse(set, "Arial", 400, false);
    EXPECT(au_fontlist_primary(l) == NULL);
    AuGlyphRun r;
    au_glyph_run_init(&r);
    au_shape_text_list(l, 16, "abc", 3, &r);
    EXPECT(r.n == 0);
    EXPECT(au_fontlist_face_for(l, 'a', false) == NULL);
    au_glyph_run_free(&r);
    au_fontlist_free(l);
    au_fontset_free(set);
}

/* ---- A damaged Fonts folder ---- */

static void write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fwrite(data, 1, len, f);
    fclose(f);
}

static uint32_t rng = 777;
static uint32_t next_rand(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void exercise_set(AuFontSet *set)
{
    static const char *const lists[] = {"Good, sans-serif", "Bad", "Trunc", "Missing", "Evil", "serif"};
    AuGlyphRun r;
    au_glyph_run_init(&r);
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
        AuFontList *l = au_fontlist_parse(set, lists[i], 400, false);
        au_shape_text_list(l, 14, "Az\xE2\x8C\x98\xF0\x9F\x98\x80" "e\xCC\x81", 12, &r);
        AuSurface *s = au_surface_new(100, 20);
        AuCanvas *c = au_canvas_new(s);
        au_draw_text_list(c, l, 14, 0, 15, "Az\xE2\x8C\x98", 5, AU_RGB(0, 0, 0));
        au_canvas_free(c);
        au_surface_free(s);
        au_fontlist_free(l);
    }
    au_glyph_run_free(&r);
}

static void test_damaged_folder(void)
{
    char tmpl[] = "/tmp/aura-fonts-XXXXXX";
    const char *tmpdir = getenv("TMPDIR");
    char base[1024];
    snprintf(base, sizeof base, "%s/aura-fonts-XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    char *root = mkdtemp(base);
    if (!root)
        root = mkdtemp(tmpl);
    CHECK(root);
    char path[1200], sub[1100];
    snprintf(sub, sizeof sub, "%s/fam", root);
    mkdir(sub, 0755);

    char *good;
    size_t good_len;
    snprintf(path, sizeof path, "%s/inter/Inter-Regular.ttf", AURA_FONTS_DIR);
    CHECK(au_platform_read_file(path, &good, &good_len));
    snprintf(path, sizeof path, "%s/good.ttf", sub);
    write_file(path, good, good_len);
    snprintf(path, sizeof path, "%s/trunc.ttf", sub);
    write_file(path, good, 3000);
    static const char junk[] = "not a font, not even close";
    snprintf(path, sizeof path, "%s/bad.ttf", sub);
    write_file(path, junk, sizeof junk);
    snprintf(path, sizeof path, "%s/empty.ttf", sub);
    write_file(path, "", 0);

    /* An index with good lines, lines naming broken or missing files, and
     * malformed lines. */
    static const char idx[] = "# comment\n"
                              "fam/good.ttf\t0\t400\tnormal\tGood\n"
                              "fam/bad.ttf\t0\t400\tnormal\tBad\n"
                              "fam/trunc.ttf\t0\t400\tnormal\tTrunc\n"
                              "fam/empty.ttf\t0\t400\titalic\tTrunc\n"
                              "fam/none.ttf\t0\t400\tnormal\tMissing\n"
                              "fam/good.ttf\t7\t400\tnormal\tEvil\n"
                              "../etc/passwd\t0\t400\tnormal\tEvil\n"
                              "/abs/path.ttf\t0\t400\tnormal\tEvil\n"
                              "fam/good.ttf\t0\t5000\tnormal\tEvil\n"
                              "fam/good.ttf\t0\t400\toblique\tEvil\n"
                              "fam/good.ttf\t0\t400\tnormal\n"
                              "fam/good.ttf\t0\t400\tnormal\tToo\tmany\n"
                              "fam/a-name-much-longer-than-thirty-one-characters.ttf\t0\t400\tnormal\tEvil\n"
                              "\t\t\t\t\n"
                              "garbage\r\n";
    snprintf(path, sizeof path, "%s/fonts.idx", root);
    write_file(path, idx, sizeof idx - 1);
    AuFontSet *set = au_fontset_new();
    AuHostFontDir *dir = au_host_add_font_dir(set, root, AU_FONT_BUNDLED);
    EXPECT(au_host_font_dir_count(dir) == 6);
    exercise_set(set);
    /* The broken faces failed and were never chosen; Good was. */
    AuFontFace *f = au_fontset_match(set, "Bad, Trunc, Missing, Evil", 400, false);
    EXPECT(f && !strcmp(f->family, "Inter"));
    EXPECT(!au_fontset_has_family(set, "Bad", 3) && !au_fontset_has_family(set, "Missing", 7));
    au_fontset_free(set);
    au_host_font_dir_free(dir);

    /* Without an index the folder is scanned; broken files are left out. */
    unlink(path);
    set = au_fontset_new();
    dir = au_host_add_font_dir(set, root, AU_FONT_BUNDLED);
    EXPECT(au_host_font_dir_count(dir) == 1);
    exercise_set(set);
    au_fontset_free(set);
    au_host_font_dir_free(dir);

    /* Damaged copies of a font in the folder: registering by scan, then
     * matching and drawing, must not crash (AddressSanitizer). */
    snprintf(path, sizeof path, "%s/good.ttf", sub);
    uint8_t *buf = au_alloc(good_len);
    for (int round = 0; round < 40; round++) {
        memcpy(buf, good, good_len);
        for (int e = 0; e < 24; e++) {
            size_t at = (next_rand() & 1) ? next_rand() % 4096 : next_rand() % good_len;
            buf[at] = (uint8_t)next_rand();
        }
        size_t cut = (next_rand() % 3 == 0) ? next_rand() % good_len : good_len;
        write_file(path, buf, cut);
        set = au_fontset_new();
        dir = au_host_add_font_dir(set, root, AU_FONT_BUNDLED);
        exercise_set(set);
        au_fontset_free(set);
        au_host_font_dir_free(dir);
    }
    au_free(buf);

    snprintf(path, sizeof path, "%s/good.ttf", sub);
    unlink(path);
    snprintf(path, sizeof path, "%s/trunc.ttf", sub);
    unlink(path);
    snprintf(path, sizeof path, "%s/bad.ttf", sub);
    unlink(path);
    snprintf(path, sizeof path, "%s/empty.ttf", sub);
    unlink(path);
    rmdir(sub);
    rmdir(root);
    au_free(good);
}

/* ---- Fuzzing the index parser ---- */

typedef struct FuzzCtx {
    int n;
} FuzzCtx;

static void check_entry(void *ctx, const AuFontIndexEntry *e)
{
    FuzzCtx *fc = ctx;
    fc->n++;
    size_t pl = strlen(e->path);
    EXPECT(pl > 0 && pl < sizeof e->path && e->path[0] != '/' && e->path[pl - 1] != '/');
    EXPECT(!strchr(e->path, ':') && !strchr(e->path, '\\'));
    /* No component is empty, longer than HFS allows, or starts with a
     * dot (so no "." or ".."). */
    const char *c = e->path;
    while (*c) {
        const char *end = strchr(c, '/');
        size_t n = end ? (size_t)(end - c) : strlen(c);
        EXPECT(n > 0 && n <= 31 && c[0] != '.');
        c += n + (end ? 1 : 0);
    }
    EXPECT(e->weight >= 1 && e->weight <= 1000 && e->face_index >= 0 && e->face_index < 1024);
    EXPECT(e->family[0] && strlen(e->family) < sizeof e->family);
}

static void test_index_fuzz(void)
{
    char *idx;
    size_t len;
    char path[1024];
    snprintf(path, sizeof path, "%s/fonts.idx", AURA_FONTS_DIR);
    CHECK(au_platform_read_file(path, &idx, &len));
    FuzzCtx fc = {0};
    EXPECT(au_font_index_parse(idx, len, check_entry, &fc) == 16 && fc.n == 16);
    int rounds = 20000;
    const char *scale = getenv("AURA_FUZZ_SCALE");
    if (scale && atoi(scale) > 0)
        rounds *= atoi(scale);
    char *buf = au_alloc(len + 64);
    for (int r = 0; r < rounds; r++) {
        memcpy(buf, idx, len);
        size_t n = len;
        int edits = 1 + (int)(next_rand() % 12);
        for (int e = 0; e < edits; e++) {
            size_t at = next_rand() % n;
            switch (next_rand() % 4) {
            case 0: buf[at] = (char)next_rand(); break;
            case 1: buf[at] = "\t\n\r/.:#\\"[next_rand() % 8]; break;
            case 2: buf[at] = (char)('0' + next_rand() % 10); break;
            default: n = at ? at : 1; break;
            }
        }
        /* An exact allocation, so AddressSanitizer sees overreads. */
        char *exact = au_alloc(n);
        memcpy(exact, buf, n);
        fc.n = 0;
        int got = au_font_index_parse(exact, n, check_entry, &fc);
        EXPECT(got == fc.n);
        au_free(exact);
    }
    au_free(buf);
    au_free(idx);
}

int main(void)
{
    test_index_and_faces();
    test_aliases_bundled_only();
    test_aliases_with_system();
    test_fallback();
    test_empty();
    test_damaged_folder();
    test_index_fuzz();
    if (failures) {
        fprintf(stderr, "fallback: %d failures\n", failures);
        return 1;
    }
    printf("fallback: ok\n");
    return 0;
}
