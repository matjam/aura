/* Layout time grows with the page, not with its square.
 *
 * Each pattern is a page made at size N and at 4N; its style, layout, one
 * paint of the viewport and a hit test are timed (CPU time), and four
 * times the content may take at most ten times as long (plus a little,
 * for very fast cases). The patterns are the ones where inline layout,
 * line breaking and float placement have gone quadratic before, or
 * could: a fuzzer's slow-page check only finds such a page when it
 * happens to make a large one (a Domato page took minutes in
 * split_seg in inline.c; see docs/layout.md, "Costs that must stay
 * linear").
 *
 * usage: test_scaling [-v] [-x TIMES] [PATTERN]
 * -v prints every pattern's times; -x multiplies the sizes (a cost that
 * is hidden by a small page shows at -x 4). */
#include "ahem.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static AuFontSet *fonts;
static int failures;

typedef void (*Gen)(AuBuf *b, int n);

static void rep(AuBuf *b, const char *s, int n)
{
    for (int i = 0; i < n; i++)
        au_buf_puts(b, s);
}

/* One word of n*8 characters in a column five characters wide: it is
 * split once per line. */
static void g_split_word(AuBuf *b, int n)
{
    au_buf_puts(b, "<div style='width:80px;overflow-wrap:anywhere'>");
    rep(b, "abcdefgh", n);
    au_buf_puts(b, "</div>");
}

/* The same word with n inline boxes after it in the paragraph: each
 * split used to move all of them. */
static void g_split_word_then_boxes(AuBuf *b, int n)
{
    au_buf_puts(b, "<div style='width:80px;overflow-wrap:anywhere'>");
    rep(b, "abcdefgh", n);
    rep(b, "<b>x</b> ", n);
    au_buf_puts(b, "</div>");
}

static void g_break_all(AuBuf *b, int n)
{
    au_buf_puts(b, "<div style='width:90px;word-break:break-all'>");
    rep(b, "abcdefghijklm nop ", n);
    au_buf_puts(b, "</div>");
}

/* Split text inside nested inline boxes, with floats beside it. */
static void g_split_in_spans(AuBuf *b, int n)
{
    au_buf_puts(b, "<div style='width:120px;overflow-wrap:anywhere'><span><i>");
    for (int i = 0; i < n; i++) {
        if (i % 50 == 0)
            au_buf_puts(b, "<span style='float:left;width:20px;height:20px'></span>");
        au_buf_puts(b, "abcdefghijkl");
        if (i % 9 == 0)
            au_buf_puts(b, "</i><b>q</b><i>");
    }
    au_buf_puts(b, "</i></span></div>");
}

static void g_words(AuBuf *b, int n)
{
    au_buf_puts(b, "<p>");
    rep(b, "word and ", n);
    au_buf_puts(b, "</p>");
}

static void g_inline_boxes(AuBuf *b, int n)
{
    au_buf_puts(b, "<p>");
    rep(b, "<span>w<b>x</b></span> ", n);
    au_buf_puts(b, "</p>");
}

static void g_nowrap(AuBuf *b, int n)
{
    au_buf_puts(b, "<p style='white-space:nowrap'>");
    rep(b, "word <i>and</i> ", n);
    au_buf_puts(b, "</p>");
}

static void g_unbroken(AuBuf *b, int n)
{
    au_buf_puts(b, "<p>");
    rep(b, "abcdefgh", n);
    au_buf_puts(b, "</p>");
}

static void g_inline_blocks(AuBuf *b, int n)
{
    au_buf_puts(b, "<p>");
    rep(b, "<span style='display:inline-block;width:12px'>x</span>", n);
    au_buf_puts(b, "</p>");
}

static void g_nested_inline_blocks(AuBuf *b, int n)
{
    au_buf_puts(b, "<p>");
    for (int i = 0; i < n / 4; i++) {
        rep(b, "<span style='display:inline-block'>a ", 4);
        rep(b, "</span>", 4);
        au_buf_putc(b, ' ');
    }
    au_buf_puts(b, "</p>");
}

static void g_floats(AuBuf *b, int n)
{
    au_buf_puts(b, "<div>");
    rep(b, "<div style='float:left;width:10px;height:10px'></div>", n);
    au_buf_puts(b, "</div>");
}

static void g_floats_both_sides(AuBuf *b, int n)
{
    au_buf_puts(b, "<div>");
    for (int i = 0; i < n; i++)
        au_buf_printf(b, "<div style='float:%s;width:%dpx;height:%dpx'></div>", i % 2 ? "right" : "left", 20 + i % 7 * 30,
                      5 + i % 3 * 4);
    au_buf_puts(b, "</div>");
}

static void g_floats_in_text(AuBuf *b, int n)
{
    au_buf_puts(b, "<p>");
    rep(b, "some text <span style='float:right;width:30px;height:8px'></span>", n);
    au_buf_puts(b, "</p>");
}

static void g_tall_floats_text(AuBuf *b, int n)
{
    /* every line is tried beside floats that leave no room */
    au_buf_puts(b, "<div style='width:200px'>");
    for (int i = 0; i < n; i++)
        au_buf_puts(b, "<div style='float:left;width:190px;height:3px'></div>wide-word-here ");
    au_buf_puts(b, "</div>");
}

static void g_pre_lines(AuBuf *b, int n)
{
    au_buf_puts(b, "<pre>");
    rep(b, "line of text\n", n);
    au_buf_puts(b, "</pre>");
}

static void g_br(AuBuf *b, int n)
{
    au_buf_puts(b, "<p>");
    rep(b, "x<br>", n);
    au_buf_puts(b, "</p>");
}

static void g_pre_wrap_spaces(AuBuf *b, int n)
{
    au_buf_puts(b, "<div style='white-space:pre-wrap;width:100px'>");
    rep(b, "a    b  ", n);
    au_buf_puts(b, "</div>");
}

static void g_soft_hyphens(AuBuf *b, int n)
{
    au_buf_puts(b, "<div style='width:100px'>");
    rep(b, "ab&shy;cd&shy;ef<wbr>gh", n);
    au_buf_puts(b, "</div>");
}

static void g_paragraphs(AuBuf *b, int n)
{
    rep(b, "<p>one <b>two</b> three</p>", n);
}

static void g_abspos_in_text(AuBuf *b, int n)
{
    au_buf_puts(b, "<p style='position:relative'>");
    rep(b, "t <span style='position:absolute;left:3px'>a</span>", n);
    au_buf_puts(b, "</p>");
}

static void g_justify(AuBuf *b, int n)
{
    au_buf_puts(b, "<p style='text-align:justify;width:150px'>");
    rep(b, "aa bbb c ", n);
    au_buf_puts(b, "</p>");
}

static void g_table_cells(AuBuf *b, int n)
{
    au_buf_puts(b, "<table>");
    for (int i = 0; i < n / 4; i++)
        au_buf_puts(b, "<tr><td>a</td><td>b c</td><td>d</td><td>e</td></tr>");
    au_buf_puts(b, "</table>");
}

static const struct {
    const char *name;
    Gen gen;
    int n; /* the small size; the large one is four times it */
} patterns[] = {
    {"split-word", g_split_word, 2500},
    {"split-word-then-boxes", g_split_word_then_boxes, 2500},
    {"break-all", g_break_all, 1500},
    {"split-in-spans", g_split_in_spans, 1500},
    {"words", g_words, 5000},
    {"inline-boxes", g_inline_boxes, 3000},
    {"nowrap", g_nowrap, 3000},
    {"unbroken", g_unbroken, 5000},
    {"inline-blocks", g_inline_blocks, 2000},
    {"nested-inline-blocks", g_nested_inline_blocks, 2000},
    {"floats", g_floats, 2000},
    {"floats-both-sides", g_floats_both_sides, 1500},
    {"floats-in-text", g_floats_in_text, 1500},
    {"tall-floats-text", g_tall_floats_text, 1000},
    {"pre-lines", g_pre_lines, 4000},
    {"br", g_br, 4000},
    {"pre-wrap-spaces", g_pre_wrap_spaces, 3000},
    {"soft-hyphens", g_soft_hyphens, 2000},
    {"paragraphs", g_paragraphs, 3000},
    {"abspos-in-text", g_abspos_in_text, 2000},
    {"justify", g_justify, 3000},
    {"table-cells", g_table_cells, 2000},
};

static double seconds(Gen gen, int n)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "<!DOCTYPE html><body>");
    gen(&b, n);
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    AuDocView *dv = au_docview_new(fonts, NULL, &cb);
    au_docview_set_viewport(dv, 400, 300);
    AuSurface *s = au_surface_new(200, 150);
    clock_t t0 = clock();
    au_docview_load(dv, b.data, b.len, "http://example.com/page.html");
    au_docview_update(dv);
    AuCanvas *cv = au_canvas_new(s);
    au_docview_paint(dv, cv, (AuRect){0, 0, 200, 150}, 0, 0);
    au_canvas_free(cv);
    AuHitResult hr;
    au_docview_hit_test(dv, 50, 40, &hr);
    /* a second width: a relayout */
    au_docview_set_viewport(dv, 333, 300);
    au_docview_update(dv);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    au_surface_free(s);
    au_docview_free(dv);
    au_buf_free(&b);
    return secs;
}

int main(int argc, char **argv)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    bool verbose = false;
    const char *only = NULL;
    int times = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = true;
        else if (!strcmp(argv[i], "-x") && i + 1 < argc)
            times = atoi(argv[++i]);
        else
            only = argv[i];
    }
    if (times < 1)
        times = 1;
    int ran = 0;
    for (size_t p = 0; p < sizeof patterns / sizeof patterns[0]; p++) {
        if (only && strcmp(only, patterns[p].name) != 0)
            continue;
        int n = patterns[p].n * times;
        double small = seconds(patterns[p].gen, n);
        double large = seconds(patterns[p].gen, n * 4);
        bool ok = large < 10 * small + 0.05;
        if (!ok)
            failures++;
        if (!ok || verbose)
            printf("%s%s: size %d took %.3f s, size %d took %.3f s (%.1f times)\n", ok ? "" : "FAIL ", patterns[p].name, n,
                   small, n * 4, large, small > 0 ? large / small : 0);
        ran++;
    }
    printf("layout scaling: %d patterns, %d failures\n", ran, failures);
    au_fontset_free(fonts);
    return failures ? 1 : 0;
}
