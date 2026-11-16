/* Layout with the bundled fonts (assets/fonts, no system fonts): a
 * paragraph mixing Latin, Greek, Cyrillic, symbols and emoji draws no
 * missing-glyph boxes, and fallback faces change line heights only as
 * browsers allow: with line-height: normal a line holding fallback glyphs
 * may grow to the fallback face's ascent and descent, never shrink, and
 * a line without them keeps the primary face's height; with a fixed
 * line-height nothing changes. */
#include "platform/posix/posix.h"
#include "page/page.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define EXPECT(cond)                                                                   \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static const char page[] =
    "<!DOCTYPE html><meta charset=utf-8>"
    "<style>body{margin:0;font:16px Arial, sans-serif} p{margin:0;white-space:nowrap}"
    ".fixed{line-height:20px} #wrap{white-space:normal;width:300px}</style>"
    "<p id=latin>Hamburg quiz</p>"
    "<p id=greek>\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1</p>"
    "<p id=cyr>\xD0\x96\xD0\xB8\xD0\xB7\xD0\xBD\xD1\x8C</p>"
    "<p id=sym>a \xE2\x8C\x98 \xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90 \xE2\x88\x80\xE2\x88\x88</p>"
    "<p id=emoji>a \xF0\x9F\x98\x80 \xE2\x9D\xA4\xEF\xB8\x8F b</p>"
    "<p id=fixed1 class=fixed>Hamburg</p>"
    "<p id=fixed2 class=fixed>a \xE2\x8C\x98 \xF0\x9F\x98\x80</p>"
    "<p id=wrap>Caf\xC3\xA9 \xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC "
    "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9 \xE2\x86\x92 \xE2\x88\x9E \xE2\x9C\x93 "
    "\xF0\x9F\x98\x80 \xF0\x9F\x8E\x89 e\xCC\x81 n\xCC\x83 \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB "
    "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90 \xE2\x96\x91\xE2\x96\x92\xE2\x96\x93 \xE2\x99\xA0\xE2\x99\xA3 "
    "the end of the paragraph, in plain Latin words that wrap.</p>"
    "<p id=bold style='font-weight:bold'>\xCE\xA9 \xD0\x96 \xE2\x86\x92 \xE2\x8C\x98 \xF0\x9F\x98\x80</p>"
    "<p id=italic style='font-style:italic'>\xCE\xA9 \xD0\x96 \xE2\x86\x92 \xE2\x8C\x98 \xF0\x9F\x98\x80</p>";

/* The height of the block <p id=id> and the heights of its lines, from
 * the layout dump. Returns the number of lines. */
static int block_lines(const char *dump, const char *id, float *block_h, float *lines, int max)
{
    char key[64];
    snprintf(key, sizeof key, "<p id=%s>", id);
    const char *p = strstr(dump, key);
    if (!p)
        return -1;
    const char *x = strchr(p + strlen(key), 'x');
    *block_h = x ? strtof(x + 1, NULL) : -1;
    int n = 0;
    const char *q = strchr(p, '\n');
    while (q && n < max) {
        q++;
        const char *s = q;
        while (*s == ' ')
            s++;
        if (!strncmp(s, "block ", 6))
            break;
        if (!strncmp(s, "line ", 5)) {
            const char *dims = strchr(s, ':');
            dims = dims ? strchr(dims + 2, ' ') : NULL;
            const char *xx = dims ? strchr(dims, 'x') : NULL;
            if (xx)
                lines[n++] = strtof(xx + 1, NULL);
        }
        q = strchr(q, '\n');
    }
    return n;
}

int main(void)
{
    AuFontSet *fonts = au_fontset_new();
    AuPosixFontDir *dir = au_posix_add_font_dir(fonts, AURA_FONTS_DIR, AU_FONT_BUNDLED);
    if (au_posix_font_dir_count(dir) == 0) {
        fprintf(stderr, "no bundled fonts in %s\n", AURA_FONTS_DIR);
        return 1;
    }
    AuDocView *dv = au_docview_new(fonts, NULL, NULL);
    au_docview_load(dv, page, sizeof page - 1, "file:///test/fonts.html");
    au_docview_set_viewport(dv, 2000, 600);
    au_docview_update(dv);
    AuLayout *L = au_docview_layout(dv);

    AuSurface *s = au_surface_new(800, 400);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, 800, 400}, 0, 0);
    au_canvas_free(c);
    au_surface_free(s);
    AuLayoutStats st;
    au_layout_stats(L, &st);
    EXPECT(st.missing_glyphs == 0);
    if (st.missing_glyphs)
        fprintf(stderr, "%u missing glyphs\n", st.missing_glyphs);

    AuBuf b;
    au_buf_init(&b);
    au_layout_dump(L, &b);
    const char *dump = au_buf_cstr(&b);
    float h, lines[32];
    float base;
    EXPECT(block_lines(dump, "latin", &base, lines, 32) == 1);
    /* Aura Sans at 16px: ascent 1854, descent 434 and line gap 67 units of
     * 2048 are 14.48, 3.39 and 0.52px, each rounded to a whole pixel as
     * Blink does: 14 + 3 + 1. (Arial has the same metrics, and Chrome's
     * line-height: normal for 16px Arial is 18px.) */
    EXPECT(fabsf(base - 18.0f) < 0.01f);
    /* Greek and Cyrillic are in the primary face: the same height. */
    EXPECT(block_lines(dump, "greek", &h, lines, 32) == 1 && fabsf(h - base) < 0.01f);
    EXPECT(block_lines(dump, "cyr", &h, lines, 32) == 1 && fabsf(h - base) < 0.01f);
    /* Fallback glyphs may grow a line a little, never shrink it. */
    EXPECT(block_lines(dump, "sym", &h, lines, 32) == 1 && h >= base - 0.01f && h <= base * 1.1f);
    EXPECT(block_lines(dump, "emoji", &h, lines, 32) == 1 && h >= base - 0.01f && h <= base * 1.1f);
    EXPECT(block_lines(dump, "bold", &h, lines, 32) == 1 && h >= base - 0.01f && h <= base * 1.1f);
    EXPECT(block_lines(dump, "italic", &h, lines, 32) == 1 && h >= base - 0.01f && h <= base * 1.1f);
    /* A fixed line-height is kept whatever the faces. */
    EXPECT(block_lines(dump, "fixed1", &h, lines, 32) == 1 && fabsf(h - 20) < 0.01f);
    EXPECT(block_lines(dump, "fixed2", &h, lines, 32) == 1 && fabsf(h - 20) < 0.01f);
    /* The wrapped paragraph: every line between the base height and a
     * tenth more; the last, plain Latin, line is exactly the base. */
    int n = block_lines(dump, "wrap", &h, lines, 32);
    EXPECT(n >= 3);
    for (int i = 0; i < n; i++) {
        EXPECT(lines[i] >= base - 0.01f && lines[i] <= base * 1.1f);
        if (!(lines[i] >= base - 0.01f && lines[i] <= base * 1.1f))
            fprintf(stderr, "wrap line %d: %.2f (base %.2f)\n", i, lines[i], base);
    }
    if (n > 0)
        EXPECT(fabsf(lines[n - 1] - base) < 0.01f);
    au_buf_free(&b);

    /* Only the faces the page needs were loaded: Aura Sans regular, bold
     * and italic, DejaVu Sans and Noto Emoji; not Aura Serif, Aura Mono or
     * Inter. */
    for (size_t i = 0; i < au_fontset_count(fonts); i++) {
        const char *fam = au_fontset_entry(fonts, i, NULL, NULL);
        if (!strcmp(fam, "Aura Serif") || !strcmp(fam, "Aura Mono") || !strcmp(fam, "Inter"))
            EXPECT(!au_fontset_entry_loaded(fonts, i));
    }

    /* The counter itself: a code point no face has is one box. */
    static const char none[] = "<!DOCTYPE html><meta charset=utf-8><p>a\xEE\x80\x80" "b</p>";
    AuDocView *dv2 = au_docview_new(fonts, NULL, NULL);
    au_docview_load(dv2, none, sizeof none - 1, "file:///test/none.html");
    au_docview_set_viewport(dv2, 400, 100);
    au_docview_update(dv2);
    s = au_surface_new(400, 100);
    c = au_canvas_new(s);
    au_docview_paint(dv2, c, (AuRect){0, 0, 400, 100}, 0, 0);
    au_layout_stats(au_docview_layout(dv2), &st);
    EXPECT(st.missing_glyphs == 1);
    au_canvas_free(c);
    au_surface_free(s);
    au_docview_free(dv2);

    au_docview_free(dv);
    au_fontset_free(fonts);
    au_posix_font_dir_free(dir);
    if (failures) {
        fprintf(stderr, "fonts: %d failures\n", failures);
        return 1;
    }
    printf("fonts: ok\n");
    return 0;
}
