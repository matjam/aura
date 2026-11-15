/* Tests for src/layout/selection.c with the Ahem-like test font (every
 * glyph a square 1em wide, so text at 10px puts character k at x = 10k):
 * document order, positions from points (on text, past line ends,
 * between blocks), words and paragraphs, range text with block breaks,
 * table cells and <br>, find across inline boundaries with case folding
 * and wrap-around, range rectangles, and the painted highlight. */
#include "ahem.h"
#include "layout/selection.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define CHECK_STR(a, b) \
    do { \
        const char *a_ = (a), *b_ = (b); \
        if (strcmp(a_, b_) != 0) { \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); \
            exit(1); \
        } \
    } while (0)

static const char doc[] =
    "<!DOCTYPE html><body style='margin:0;font:10px/20px Ahem'>"
    "<p id=p1 style='margin:0'>Hello <b>wor</b>ld foo</p>"
    "<p id=p2 style='margin:0'>Second findme FindMe find<i>me</i></p>"
    "<table style='border-spacing:0'><tr><td style='padding:0'>a1</td><td style='padding:0'>b2</td></tr></table>"
    "<div>line<br>break</div>"
    "<p style='margin:0'>last findme</p>";

static AuNode *text_of(AuDocView *dv, const char *s)
{
    AuDocument *d = au_docview_document(dv);
    for (AuNode *n = au_node_next(&d->node, &d->node); n; n = au_node_next(n, &d->node))
        if (n->type == AU_NODE_TEXT && strncmp(au_as_text(n)->data, s, strlen(s)) == 0)
            return n;
    return NULL;
}

static char *range_text(AuLayout *L, const AuTextRange *r)
{
    static char buf[1024];
    AuBuf b;
    au_buf_init(&b);
    au_layout_range_text(L, r, &b);
    au_buf_cstr(&b);
    snprintf(buf, sizeof buf, "%s", b.data ? b.data : "");
    au_buf_free(&b);
    return buf;
}

static uint32_t pixel(AuSurface *s, int x, int y)
{
    return (uint32_t)s->pixels[y * s->stride + x] | 0xFF000000u;
}

/* DOM offsets (UTF-16 code units of a text node's data) to text
 * positions and back, and the rectangles of a range per line. */
static void test_dom_offsets(AuFontSet *fonts)
{
    static const char page[] =
        "<!DOCTYPE html><body style='margin:0;font:10px/20px Ahem'>"
        "<p style='margin:0'>  ab   cd\n ef  </p>"
        "<p style='margin:0;text-transform:uppercase'>stra\xC3\x9F" "e x</p>"
        "<p style='margin:0'>q\xF0\x9F\x98\x80z</p>"
        "<p style='margin:0'><span>k </span> m</p>"
        "<p style='margin:0;width:50px'>abcd efgh</p>"
        "<p style='margin:0;display:none'>hidden</p>"
        "<div> </div>";
    AuDocView *dv = au_docview_new(fonts, NULL, NULL);
    au_docview_load(dv, page, sizeof page - 1, "about:test");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    AuLayout *L = au_docview_layout(dv);
    AuTextPos p;
    uint32_t u;
    AuRect rc[4];

    /* collapsed white space: "  ab   cd\n ef  " shows as "ab cd ef " */
    AuNode *n = text_of(dv, "  ab");
    CHECK(n);
    static const uint32_t want[] = {0, 0, 0, 1, 2, 3, 3, 3, 4, 5, 6, 6, 7, 8, 9, 9, 9};
    for (uint32_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        CHECK(au_layout_text_pos_from_dom(L, n, i, &p));
        if (p.offset != want[i]) {
            fprintf(stderr, "offset %u: position %u, want %u\n", (unsigned)i, (unsigned)p.offset, (unsigned)want[i]);
            exit(1);
        }
    }
    CHECK(au_layout_text_pos_from_dom(L, n, 1000, &p) && p.offset == 9); /* past the end */
    /* back: the first DOM offset at or after the position, and the round
     * trip gives the position again */
    static const uint32_t back[] = {0, 3, 4, 5, 8, 9, 10, 12, 13, 14};
    for (uint32_t o = 0; o <= 9; o++) {
        p = (AuTextPos){n, o};
        CHECK(au_layout_text_pos_to_dom(L, &p, &u));
        if (u != back[o]) {
            fprintf(stderr, "position %u: offset %u, want %u\n", (unsigned)o, (unsigned)u, (unsigned)back[o]);
            exit(1);
        }
        AuTextPos q;
        CHECK(au_layout_text_pos_from_dom(L, n, u, &q) && q.offset == o);
    }
    /* "cd" (DOM offsets 7 to 9) is at x 30 to 50 on the first line */
    AuTextPos a, b;
    CHECK(au_layout_text_pos_from_dom(L, n, 7, &a) && au_layout_text_pos_from_dom(L, n, 9, &b));
    AuTextRange r = au_text_range_make(a, b);
    CHECK(au_layout_range_rects(L, &r, rc, 4) == 1);
    CHECK(rc[0].x == 30 && rc[0].w == 20 && rc[0].y == 5 && rc[0].h == 10);
    /* a collapsed range: a caret of no width */
    r = au_text_range_make(a, a);
    CHECK(au_layout_range_rects(L, &r, rc, 4) == 1 && rc[0].x == 30 && rc[0].w == 0);

    /* text-transform changes lengths: sharp s becomes SS */
    n = text_of(dv, "stra");
    CHECK(n && au_layout_text_pos_from_dom(L, n, 4, &p) && p.offset == 4);
    CHECK(au_layout_text_pos_from_dom(L, n, 5, &p) && p.offset == 6);
    CHECK(au_layout_text_pos_from_dom(L, n, 7, &p) && p.offset == 8);
    p = (AuTextPos){n, 5}; /* between the two S */
    CHECK(au_layout_text_pos_to_dom(L, &p, &u) && u == 5);
    p = (AuTextPos){n, 8};
    CHECK(au_layout_text_pos_to_dom(L, &p, &u) && u == 7);

    /* a character outside the BMP is two code units and four bytes */
    n = text_of(dv, "q");
    CHECK(n && au_layout_text_pos_from_dom(L, n, 1, &p) && p.offset == 1);
    CHECK(au_layout_text_pos_from_dom(L, n, 2, &p) && p.offset == 1); /* inside the pair: its start */
    CHECK(au_layout_text_pos_from_dom(L, n, 3, &p) && p.offset == 5);
    p = (AuTextPos){n, 5};
    CHECK(au_layout_text_pos_to_dom(L, &p, &u) && u == 3);

    /* " m" after "k ": its leading space collapsed into the one before */
    n = text_of(dv, " m");
    CHECK(n && au_layout_text_pos_from_dom(L, n, 0, &p) && p.offset == 0);
    CHECK(au_layout_text_pos_from_dom(L, n, 1, &p) && p.offset == 0);
    CHECK(au_layout_text_pos_from_dom(L, n, 2, &p) && p.offset == 1);

    /* a range over two lines: one rectangle per line */
    n = text_of(dv, "abcd efgh");
    CHECK(n && au_layout_text_pos_from_dom(L, n, 2, &a) && au_layout_text_pos_from_dom(L, n, 7, &b));
    r = au_text_range_make(a, b);
    CHECK(au_layout_range_rects(L, &r, rc, 4) == 2);
    CHECK(rc[0].x == 20 && rc[0].w == 20 && rc[1].x == 0 && rc[1].w == 20 && rc[1].y == rc[0].y + 20);
    CHECK(au_layout_range_rects(L, &r, rc, 1) == 2); /* the count, whatever fits */

    /* no box: not displayed, or white space that collapsed away */
    n = text_of(dv, "hidden");
    CHECK(n && !au_layout_text_pos_from_dom(L, n, 0, &p));
    AuDocument *d = au_docview_document(dv);
    AuNode *lastdiv = NULL;
    for (AuNode *x = au_node_next(&d->node, &d->node); x; x = au_node_next(x, &d->node))
        if (au_is_html_element(x, AU_ATOM_div))
            lastdiv = x;
    CHECK(lastdiv && lastdiv->first_child && !au_layout_text_pos_from_dom(L, lastdiv->first_child, 0, &p));
    au_docview_free(dv);
}

int main(void)
{
    static uint8_t fontdata[16384];
    AuFontSet *fonts = ahem_fontset(fontdata);
    AuDocView *dv = au_docview_new(fonts, NULL, NULL);
    au_docview_load(dv, doc, sizeof doc - 1, "about:test");
    au_docview_set_viewport(dv, 800, 600);
    au_docview_update(dv);
    AuLayout *L = au_docview_layout(dv);

    AuNode *hello = text_of(dv, "Hello"), *wor = text_of(dv, "wor"), *ld = text_of(dv, "ld foo");
    AuNode *second = text_of(dv, "Second"), *last = text_of(dv, "last");
    CHECK(hello && wor && ld && second && last);

    /* Order. */
    CHECK(au_node_order(hello, wor) < 0 && au_node_order(ld, wor) > 0 && au_node_order(hello, hello) == 0);
    CHECK(au_node_order(hello->parent, hello) < 0 && au_node_order(last, hello->parent) > 0);
    AuTextPos a = {hello, 3}, b = {hello, 1};
    AuTextRange r = au_text_range_make(a, b);
    CHECK(r.start.offset == 1 && r.end.offset == 3);

    /* Positions from points: on text, past the end of a line, below the
     * last line, in the left margin. */
    AuTextPos p;
    CHECK(au_layout_text_pos_at(L, 24, 10, 0, 0, &p));
    CHECK(p.node == hello && p.offset == 2); /* 24px: nearer the boundary after "He" */
    CHECK(au_layout_text_pos_at(L, 700, 10, 0, 0, &p));
    CHECK(p.node == ld && p.offset == 6); /* past the end of line 1: its end */
    CHECK(au_layout_text_pos_at(L, 30, 5000, 0, 0, &p));
    CHECK(p.node == last && p.offset == 3);
    CHECK(au_layout_text_pos_at(L, 77, 10, 0, 0, &p)); /* inside <b>: "wor" starts at 60 */
    CHECK(p.node == wor && p.offset == 2);

    /* Words and paragraphs. */
    AuTextPos in = {hello, 2};
    CHECK(au_layout_word_at(L, &in, &r));
    CHECK(r.start.node == hello && r.start.offset == 0 && r.end.offset == 5);
    AuTextPos sp = {hello, 5};
    CHECK(au_layout_word_at(L, &sp, &r)); /* at a word's end: that word */
    CHECK(r.start.offset == 0 && r.end.offset == 5);
    CHECK(au_layout_block_at(L, &in, &r));
    CHECK_STR(range_text(L, &r), "Hello world foo");

    /* Range text: inline boundaries joined, blocks broken, cells tabbed,
     * <br> as a line. */
    AuTextRange all;
    CHECK(au_layout_all_text(L, &all));
    CHECK_STR(range_text(L, &all),
              "Hello world foo\nSecond findme FindMe findme\na1\tb2\nline\nbreak\nlast findme");
    AuTextPos s1 = {hello, 2}, e1 = {second, 3};
    r = au_text_range_make(e1, s1);
    CHECK_STR(range_text(L, &r), "llo world foo\nSec");
    AuTextPos s2 = {wor, 1}, e2 = {wor, 1};
    r = au_text_range_make(s2, e2);
    CHECK(au_text_range_empty(&r));
    CHECK_STR(range_text(L, &r), "");

    /* Find: case-insensitive by default, across an inline boundary,
     * walking forward and back with wrap-around. */
    AuTextRange m;
    AuTextPos none = {NULL, 0};
    CHECK(au_layout_find(L, "WORLD", &none, true, true, &m));
    CHECK(m.start.node == wor && m.start.offset == 0 && m.end.node == ld && m.end.offset == 2);
    CHECK(!au_layout_find(L, "WORLD", &none, true, false, &m));
    CHECK(au_layout_find(L, "findme", &none, true, true, &m));
    CHECK(m.start.node == second && m.start.offset == 7);
    AuTextPos from = m.end;
    CHECK(au_layout_find(L, "findme", &from, true, true, &m));
    CHECK(m.start.node == second && m.start.offset == 14); /* "FindMe" */
    from = m.end;
    CHECK(au_layout_find(L, "findme", &from, true, true, &m));
    CHECK(m.start.node == second && m.start.offset == 21 && m.end.node != second); /* find<i>me</i> */
    from = m.end;
    CHECK(au_layout_find(L, "findme", &from, true, true, &m));
    CHECK(m.start.node == last);
    from = m.end;
    CHECK(au_layout_find(L, "findme", &from, true, true, &m)); /* wraps to the first */
    CHECK(m.start.node == second && m.start.offset == 7);
    from = m.start;
    CHECK(au_layout_find(L, "findme", &from, false, true, &m)); /* back: wraps to the last */
    CHECK(m.start.node == last);
    from = m.start;
    CHECK(au_layout_find(L, "findme", &from, false, true, &m));
    CHECK(m.start.node == second && m.start.offset == 21);
    CHECK(au_layout_find(L, "a1\tb2", &none, true, true, &m));
    CHECK(!au_layout_find(L, "foo sec", &none, true, true, &m)); /* blocks are separate lines */
    CHECK(!au_layout_find(L, "nothing here", &none, true, true, &m));
    CHECK(!au_layout_find(L, "", &none, true, true, &m));

    /* Rectangles. */
    AuRect rr;
    AuTextPos rs = {hello, 0}, re = {hello, 5};
    r = au_text_range_make(rs, re);
    CHECK(au_layout_range_rect(L, &r, &rr));
    /* The text's content area (10px font in a 20px line): "Hello" and
     * its trailing space. */
    CHECK(rr.x == 0 && rr.y == 5 && rr.h == 10 && rr.w == 60);
    r = au_text_range_make((AuTextPos){last, 0}, (AuTextPos){last, 4});
    CHECK(au_layout_range_rect(L, &r, &rr) && rr.y > 60);

    /* The highlight, behind the text: "lo " of "Hello " in red. Ahem's
     * glyphs fill the content area and its space is empty, so the
     * selected space shows red and the selected letters black. */
    r = au_text_range_make((AuTextPos){hello, 3}, (AuTextPos){hello, 6});
    au_layout_set_selection(L, &r, AU_RGB(255, 0, 0));
    AuTextRange got;
    CHECK(au_layout_get_selection(L, &got) && got.start.offset == 3);
    AuSurface *surf = au_surface_new(200, 60);
    AuCanvas *c = au_canvas_new(surf);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 60}, 0, 0);
    CHECK(pixel(surf, 55, 9) == AU_RGB(255, 0, 0));  /* the selected space */
    CHECK(pixel(surf, 45, 9) == AU_RGB(0, 0, 0));    /* "o": text over the highlight */
    CHECK(pixel(surf, 55, 2) != AU_RGB(255, 0, 0));  /* above the content area */
    CHECK(pixel(surf, 65, 9) != AU_RGB(255, 0, 0));  /* "w" not selected */
    au_layout_set_selection(L, NULL, 0);
    CHECK(!au_layout_get_selection(L, &got));
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 60}, 0, 0);
    CHECK(pixel(surf, 55, 9) != AU_RGB(255, 0, 0));
    au_canvas_free(c);
    au_surface_free(surf);

    au_docview_free(dv);
    test_dom_offsets(fonts);
    au_fontset_free(fonts);
    printf("selection: all tests passed\n");
    return 0;
}
