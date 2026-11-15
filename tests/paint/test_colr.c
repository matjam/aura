/* Colour glyphs (COLR v0 and v1, CPAL) and emoji sequences (GSUB).
 *
 *   test_colr FONTS_DIR BUNDLED_FONTS_DIR [--bench]
 *
 * FONTS_DIR is tests/paint/fonts: colr-test.ttf (every paint format, made
 * by tools/test-fonts.py), and subsets of Twemoji Mozilla (COLR v0) and
 * Noto Color Emoji (COLR v1). --bench prints rasterization timings and
 * cache sizes. */
#include "core/core.h"
#include "paint/font.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1); \
        } \
    } while (0)

static char *load(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char *data;
    if (!au_platform_read_file(path, &data, len)) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    return data;
}

/* Draws code point cp of f at 40px with its square glyphs filling
 * (0, 0)-(40, 40) on white, with text colour fg. */
static AuSurface *draw_cp(AuFontFace *f, uint32_t cp, AuColor fg)
{
    AuSurface *s = au_surface_new(48, 48);
    au_surface_clear(s, 0xFFFFFFFF);
    AuCanvas *c = au_canvas_new(s);
    char text[8];
    int n = au_utf8_encode(cp, text);
    au_draw_text(c, f, 40, 0, 40, text, (size_t)n, fg);
    au_canvas_free(c);
    return s;
}

static int near(AuColor a, AuColor b, int tol)
{
    for (int sh = 0; sh < 32; sh += 8) {
        int x = (int)((a >> sh) & 255), y = (int)((b >> sh) & 255);
        if (abs(x - y) > tol)
            return 0;
    }
    return 1;
}

#define RED 0xFFFF0000u
#define BLUE 0xFF0000FFu
#define GREEN 0xFF00FF00u
#define WHITE 0xFFFFFFFFu
#define BLACK 0xFF000000u

static void expect(AuFontFace *f, uint32_t cp, int x, int y, AuColor want, int tol)
{
    AuSurface *s = draw_cp(f, cp, 0xFF123456);
    AuColor got = au_surface_get(s, x, y);
    if (!near(got, want, tol)) {
        fprintf(stderr, "U+%04X at (%d, %d): %08X, expected %08X\n", (unsigned)cp, x, y, (unsigned)got,
                (unsigned)want);
        exit(1);
    }
    au_surface_free(s);
}

static void test_paints(const char *dir)
{
    size_t len;
    char *data = load(dir, "colr-test.ttf", &len);
    AuFontFace *f = au_font_face_new(data, len, 0);
    CHECK(f && f->color);
    expect(f, 0xE000, 10, 20, RED, 2);   /* v0: left layer red */
    expect(f, 0xE000, 30, 20, BLUE, 2);  /* v0: right layer blue */
    expect(f, 0xE001, 20, 20, RED, 2);   /* solid */
    expect(f, 0xE002, 1, 20, RED, 20);   /* linear: red at the left */
    expect(f, 0xE002, 38, 20, BLUE, 20); /* blue at the right */
    expect(f, 0xE003, 20, 20, RED, 20);  /* radial: red centre */
    expect(f, 0xE003, 1, 1, BLUE, 2);    /* padded beyond the radius */
    expect(f, 0xE004, 38, 18, RED, 30);  /* sweep: red just above 0 degrees */
    expect(f, 0xE004, 38, 22, BLUE, 30); /* blue just below (360) */
    expect(f, 0xE004, 2, 20, 0xFF800080u, 40); /* half way round */
    expect(f, 0xE005, 15, 20, BLUE, 2);  /* src_in: blue inside the left half */
    expect(f, 0xE005, 25, 20, WHITE, 2); /* nothing outside the backdrop */
    expect(f, 0xE005, 5, 20, WHITE, 2);  /* the backdrop itself is not drawn */
    expect(f, 0xE006, 30, 20, GREEN, 2); /* rotated half turn: on the right */
    expect(f, 0xE006, 10, 20, WHITE, 2);
    expect(f, 0xE007, 20, 20, RED, 2);   /* PaintColrGlyph */
    expect(f, 0xE009, 20, 20, WHITE, 0); /* a cycle draws nothing */
    expect(f, 0xE00A, 20, 20, WHITE, 0); /* nesting beyond the limit draws nothing */
    expect(f, 0xE00B, 20, 20, BLUE, 2);  /* layers */
    expect(f, 0xE00B, 3, 3, RED, 2);
    expect(f, 0xE00C, 10, 20, BLUE, 2);  /* clip box keeps the left half */
    expect(f, 0xE00C, 30, 20, WHITE, 2);
    expect(f, 0xE00E, 20, 20, BLACK, 2); /* multiply */
    expect(f, 0xE00F, 19, 20, BLUE, 40); /* reflect: blue at t = 1 */
    expect(f, 0xE00F, 38, 20, RED, 40);  /* and back to red */
    /* Skew: something is drawn, sheared right at the top. */
    AuSurface *s = draw_cp(f, 0xE00D, BLACK);
    CHECK(near(au_surface_get(s, 20, 20), RED, 2));
    au_surface_free(s);
    /* The foreground entry takes the text colour; other colours do not. */
    s = draw_cp(f, 0xE008, 0xFF00AA00);
    CHECK(near(au_surface_get(s, 20, 20), 0xFF00AA00, 2));
    au_surface_free(s);
    s = draw_cp(f, 0xE008, 0xFF0000AA);
    CHECK(near(au_surface_get(s, 20, 20), 0xFF0000AA, 2));
    au_surface_free(s);
    s = draw_cp(f, 0xE001, 0xFF0000AA);
    CHECK(near(au_surface_get(s, 20, 20), RED, 2));
    au_surface_free(s);
    /* Opacity groups and clips still apply. */
    s = au_surface_new(48, 48);
    au_surface_clear(s, WHITE);
    AuCanvas *c = au_canvas_new(s);
    au_canvas_clip_push(c, (AuRect){0, 0, 20, 48});
    au_canvas_group_begin(c, (AuRect){0, 0, 48, 48});
    au_draw_text(c, f, 40, 0, 40, "\xEE\x80\x81", 3, BLACK);
    au_canvas_group_end(c, 0.5f);
    au_canvas_clip_pop(c);
    au_canvas_free(c);
    CHECK(near(au_surface_get(s, 10, 20), 0xFFFF8080u, 3));
    CHECK(near(au_surface_get(s, 30, 20), WHITE, 0));
    au_surface_free(s);
    au_font_face_free(f);
    au_free(data);
}

/* Shapes text with a font list over the emoji fonts and returns the
 * number of glyphs and the face of the first. */
static size_t shape(AuFontList *l, const char *text, const AuFontFace **face, uint16_t *first)
{
    AuGlyphRun r;
    au_glyph_run_init(&r);
    au_shape_text_list(l, 32, text, strlen(text), &r);
    size_t n = r.n;
    *face = n ? r.glyphs[0].face : NULL;
    *first = n ? r.glyphs[0].id : 0;
    au_glyph_run_free(&r);
    return n;
}

static void test_sequences(const char *dir, const char *bundled)
{
    size_t tl, nl, ml;
    char *tw = load(dir, "twemoji-sub.ttf", &tl);
    char *noto = load(dir, "notocolr-sub.ttf", &nl);
    char *mono = load(bundled, "notoemoji/NotoEmoji-Regular.ttf", &ml);
    size_t il;
    char *inter = load(bundled, "inter/Inter-Regular.ttf", &il);
    for (int which = 0; which < 2; which++) {
        AuFontSet *s = au_fontset_new();
        CHECK(au_fontset_add_data(s, inter, il) == 1); /* a text face without emoji */
        /* Register the colour font under the first emoji fallback's name
         * by adding it as data (its own family name is used). */
        CHECK(au_fontset_add_data(s, which ? noto : tw, which ? nl : tl) == 1);
        CHECK(au_fontset_add_data(s, mono, ml) == 1);
        const char *fam = au_fontset_entry(s, 1, NULL, NULL);
        const char *names[] = {fam};
        AuFontList *l = au_fontlist_new(s, names, 1, 400, false);
        const AuFontFace *f;
        uint16_t g;
        static const char *const seqs[] = {
            "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD",                                 /* thumbs up, medium skin */
            "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5",                                 /* flag: Japan */
            "1\xEF\xB8\x8F\xE2\x83\xA3",                                        /* keycap 1 */
            "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", /* family */
            "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88",         /* rainbow flag */
            ("\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xB3\xF3\xA0\x81\xA3\xF3\xA0\x81\xB4"
             "\xF3\xA0\x81\xBF"), /* flag: Scotland (tag sequence) */
        };
        for (size_t i = 0; i < sizeof seqs / sizeof seqs[0]; i++) {
            size_t n = shape(l, seqs[i], &f, &g);
            printf("%s sequence %zu: %zu glyph(s)\n", fam, i, n);
            CHECK(n == 1);
            CHECK(f->color);
        }
        /* Without a ligature the components are drawn: two emoji that do
         * not form a sequence. */
        CHECK(shape(l, "\xF0\x9F\x98\x80\xF0\x9F\x8E\x89", &f, &g) == 2);
        /* Emoji presentation picks the colour face of the emoji fallback
         * list (Twemoji Mozilla); text presentation (U+FE0E) the
         * monochrome one, with a page font or through fallback. */
        const char *names2[] = {"serif"};
        AuFontList *l2 = au_fontlist_new(s, names2, 1, 400, false);
        CHECK(shape(l2, "\xE2\x98\xBA\xEF\xB8\x8E", &f, &g) == 1); /* smiling face, text */
        CHECK(f && !strcmp(f->family, "Noto Emoji"));
        CHECK(shape(l2, "\xE2\x98\xBA\xEF\xB8\x8F", &f, &g) == 1); /* emoji presentation */
        CHECK(f && f->color == !which);
        au_fontlist_free(l2);
        au_fontlist_free(l);
        au_fontset_free(s);
    }
    au_free(tw);
    au_free(noto);
    au_free(mono);
    au_free(inter);
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}

static void bench(const char *dir)
{
    static const char *const fonts[] = {"twemoji-sub.ttf", "notocolr-sub.ttf"};
    static const uint32_t cps[] = {0x1F600, 0x1F389, 0x1F680, 0x2764, 0x1F984, 0x1F308};
    for (int k = 0; k < 2; k++) {
        size_t len;
        char *data = load(dir, fonts[k], &len);
        AuFontFace *f = au_font_face_new(data, len, 0);
        AuSurface *s = au_surface_new(64, 64);
        AuCanvas *c = au_canvas_new(s);
        for (int size = 16; size <= 32; size += 16) {
            int reps = 200;
            AuGlyphCacheStats st0, st1;
            double t0 = now_ms();
            for (int r = 0; r < reps; r++)
                for (size_t i = 0; i < sizeof cps / sizeof cps[0]; i++) {
                    au_glyph_cache_clear();
                    char t[8];
                    int n = au_utf8_encode(cps[i], t);
                    au_draw_text(c, f, (float)size, 0, (float)size, t, (size_t)n, BLACK);
                }
            double per = (now_ms() - t0) / (reps * (double)(sizeof cps / sizeof cps[0]));
            au_glyph_cache_clear();
            au_glyph_cache_stats(&st0);
            for (size_t i = 0; i < sizeof cps / sizeof cps[0]; i++) {
                char t[8];
                int n = au_utf8_encode(cps[i], t);
                au_draw_text(c, f, (float)size, 0, (float)size, t, (size_t)n, BLACK);
            }
            au_glyph_cache_stats(&st1);
            printf("%-18s %2d px: %.3f ms per colour glyph (rasterize + draw), cache %.0f bytes per glyph\n",
                   fonts[k], size, per, (double)(st1.bytes - st0.bytes) / (sizeof cps / sizeof cps[0]));
        }
        au_canvas_free(c);
        au_surface_free(s);
        au_font_face_free(f);
        au_free(data);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    test_paints(argv[1]);
    test_sequences(argv[1], argv[2]);
    if (argc > 3 && !strcmp(argv[3], "--bench"))
        bench(argv[1]);
    return 0;
}
