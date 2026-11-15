/* The per-face shaping caches (code point to glyph, kerning pairs) and
 * the glyph cache's stored text coverage.
 *
 * Each face's answers through its caches, asked in several orders and more
 * often than the kerning table has slots, must equal those of a second
 * face object made from the same file, whose caches see each question for
 * the first time. Two different fonts must not share answers. Text drawn
 * twice (glyph cache cold, then warm) and after the cache is cleared must
 * give the same pixels.
 *
 * Usage: test_shape_cache FONTS_DIR */
#include "paint/font.h"
#include "paint/paint.h"
#include "platform/platform.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static AuFontFace *load(const char *dir, const char *file, char **data)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    size_t len;
    if (!au_platform_read_file(path, data, &len))
        return NULL;
    return au_font_face_new(*data, len, 0);
}

#define NCP 0x400
#define NG 90

static void check_face(const char *dir, const char *file)
{
    char *d1 = NULL, *d2 = NULL;
    AuFontFace *a = load(dir, file, &d1), *b = load(dir, file, &d2);
    CHECK(a && b, "cannot load %s", file);
    if (!a || !b)
        return;
    /* Code points: a ascending, then again descending; b once, in steps. */
    static uint16_t ga[NCP];
    for (uint32_t cp = 0; cp < NCP; cp++)
        ga[cp] = au_font_glyph(a, cp);
    int bad = 0;
    for (uint32_t cp = NCP; cp-- > 0;)
        if (au_font_glyph(a, cp) != ga[cp])
            bad++;
    for (uint32_t k = 0; k < NCP; k++) {
        uint32_t cp = (k * 37) % NCP;
        if (au_font_glyph(b, cp) != ga[cp])
            bad++;
    }
    CHECK(bad == 0, "%s: %d code points map to different glyphs through the cache", file, bad);
    CHECK(ga['A'] != 0 && ga['A'] != ga['B'], "%s: A and B have no distinct glyphs", file);
    /* Kerning: every pair of NG glyphs (more pairs than slots), twice in
     * a, once in b in another order. */
    uint16_t g[NG];
    for (int i = 0; i < NG; i++)
        g[i] = ga[0x21 + i];
    static int16_t ka[NG][NG];
    int nonzero = 0;
    for (int i = 0; i < NG; i++)
        for (int j = 0; j < NG; j++) {
            ka[i][j] = (int16_t)au_font_kern_units(a, g[i], g[j]);
            nonzero += ka[i][j] != 0;
        }
    bad = 0;
    for (int j = NG; j-- > 0;)
        for (int i = 0; i < NG; i++) {
            if (au_font_kern_units(a, g[i], g[j]) != ka[i][j])
                bad++;
            if (au_font_kern_units(b, g[(i * 7) % NG], g[j]) != ka[(i * 7) % NG][j])
                bad++;
        }
    CHECK(bad == 0, "%s: %d kerning pairs differ through the cache", file, bad);
    printf("%s: %d of %d pairs kern\n", file, nonzero, NG * NG);
    au_font_face_free(a);
    au_font_face_free(b);
    au_free(d1);
    au_free(d2);
}

static bool same(const AuSurface *a, const AuSurface *b)
{
    return !memcmp(a->pixels, b->pixels, (size_t)a->stride * (size_t)a->height * sizeof(AuPixel));
}

static void draw(AuSurface *s, const AuFontFace *f, float size, AuColor color)
{
    au_surface_clear(s, AU_RGBA(255, 255, 255, 255));
    AuCanvas *c = au_canvas_new(s);
    static const char *text = "Hamburgefonstiv AVATAR To Wa. 0123456789";
    au_draw_text(c, f, size, 3.25f, 30.5f, text, strlen(text), color);
    au_draw_text(c, f, size * 0.7f, 3.6f, 50, text, strlen(text), color);
    au_canvas_free(c);
}

static void check_drawing(const char *dir)
{
    char *d1 = NULL, *d2 = NULL;
    AuFontFace *sans = load(dir, "liberation/AuraSans-Regular.ttf", &d1);
    AuFontFace *serif = load(dir, "liberation/AuraSerif-Regular.ttf", &d2);
    CHECK(sans && serif, "cannot load the bundled faces");
    if (!sans || !serif)
        return;
    AuSurface *cold = au_surface_new(420, 64), *warm = au_surface_new(420, 64), *again = au_surface_new(420, 64);
    AuSurface *other = au_surface_new(420, 64);
    au_glyph_cache_clear();
    draw(cold, sans, 18, AU_RGBA(0, 0, 0, 255));
    AuGlyphCacheStats s0, s1;
    au_glyph_cache_stats(&s0);
    draw(warm, sans, 18, AU_RGBA(0, 0, 0, 255));
    au_glyph_cache_stats(&s1);
    CHECK(s1.misses == s0.misses && s1.hits > s0.hits, "the second draw did not come from the glyph cache");
    CHECK(same(cold, warm), "text drawn from the glyph cache differs from its first draw");
    /* Another face and another size between: their glyphs must not be
     * served for this face. */
    draw(other, serif, 18, AU_RGBA(0, 0, 0, 255));
    CHECK(!same(other, cold), "two fonts drew the same pixels");
    draw(other, sans, 19, AU_RGBA(0, 0, 0, 255));
    CHECK(!same(other, cold), "two sizes drew the same pixels");
    draw(again, sans, 18, AU_RGBA(0, 0, 0, 255));
    CHECK(same(cold, again), "text differs after other fonts and sizes used the cache");
    au_glyph_cache_clear();
    draw(again, sans, 18, AU_RGBA(0, 0, 0, 255));
    CHECK(same(cold, again), "text differs after the glyph cache was cleared");
    /* A translucent colour takes the other blending path. */
    draw(cold, sans, 18, AU_RGBA(200, 0, 0, 128));
    draw(again, sans, 18, AU_RGBA(200, 0, 0, 128));
    CHECK(same(cold, again), "translucent text differs between two draws");
    /* A face freed and another loaded (perhaps at the same address):
     * the new face's glyphs are its own. */
    au_surface_free(other);
    other = au_surface_new(420, 64);
    draw(other, serif, 18, AU_RGBA(0, 0, 0, 255));
    au_font_face_free(sans);
    au_free(d1);
    char *d3 = NULL;
    AuFontFace *serif2 = load(dir, "liberation/AuraSerif-Regular.ttf", &d3);
    draw(again, serif2, 18, AU_RGBA(0, 0, 0, 255));
    CHECK(same(other, again), "a face loaded after another was freed drew different glyphs");
    au_font_face_free(serif2);
    au_free(d3);
    au_font_face_free(serif);
    au_free(d2);
    au_surface_free(cold);
    au_surface_free(warm);
    au_surface_free(again);
    au_surface_free(other);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_shape_cache FONTS_DIR\n");
        return 2;
    }
    static const char *faces[] = {"liberation/AuraSans-Regular.ttf", "liberation/AuraSerif-Italic.ttf",
                                  "liberation/AuraMono-Regular.ttf", "inter/Inter-Regular.ttf",
                                  "dejavu/DejaVuSans.ttf"};
    for (size_t i = 0; i < sizeof faces / sizeof faces[0]; i++)
        check_face(argv[1], faces[i]);
    check_drawing(argv[1]);
    printf("shape cache: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
