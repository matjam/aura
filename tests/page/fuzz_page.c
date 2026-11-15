/* Light fuzzing of the document pipeline: random documents built from
 * tags, attributes and style declarations that exercise layout (floats,
 * positioning, flex, tables, lists, generated content, overflow, deep
 * nesting), fed in random chunks, laid out at random widths, painted,
 * hit tested, hovered and resized. Deterministic for a seed.
 *
 *   fuzz_page [iterations] [seed] [current.html]
 *
 * Run under the sanitizer build for memory errors; the program itself only
 * checks that nothing crashes or hangs. */
#include "../layout/ahem.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rng;

static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return n ? rng % n : 0;
}

static const char *const tags[] = {"div", "span", "p", "a", "b", "ul", "ol", "li", "table", "tr", "td", "th",
                                   "img", "br", "wbr", "pre", "h1", "h2", "section", "button", "input", "select",
                                   "textarea", "q", "sup", "sub", "hr", "em", "code", "blockquote", "center"};

static const char *const decls[] = {
    "display:block", "display:inline", "display:inline-block", "display:flex", "display:inline-flex",
    "display:none", "display:contents", "display:flow-root", "display:list-item", "display:table",
    "display:table-cell", "display:grid", "display:-webkit-box", "float:left", "float:right", "clear:both",
    "position:relative", "position:absolute", "position:fixed", "position:sticky", "top:10px", "left:-5px",
    "right:20%", "bottom:0", "inset:3px", "z-index:3", "z-index:-1", "width:50%", "width:120px",
    "width:min-content", "width:max-content", "width:fit-content", "height:40px", "height:30%",
    "min-height:10px", "max-width:80px", "min-width:200px", "max-height:5px", "margin:10px", "margin:-8px",
    "margin:auto", "margin-top:-30px", "padding:5px", "padding:10%", "border:3px solid red",
    "border:5px dashed", "border-radius:8px", "box-sizing:border-box", "overflow:hidden", "overflow:auto",
    "overflow:scroll", "white-space:pre", "white-space:nowrap", "white-space:pre-wrap", "white-space:pre-line",
    "word-break:break-all", "overflow-wrap:anywhere", "text-align:justify", "text-align:center",
    "text-indent:-20px", "letter-spacing:3px", "word-spacing:-4px", "line-height:0", "line-height:3",
    "vertical-align:top", "vertical-align:middle", "vertical-align:-10px", "vertical-align:super",
    "font-size:40px", "font-size:0", "flex:1", "flex:0 0 50px", "flex-wrap:wrap", "flex-direction:column",
    "flex-direction:row-reverse", "flex-wrap:wrap-reverse", "justify-content:space-between",
    "align-items:baseline", "align-items:center", "align-content:space-around", "order:-2", "gap:7px",
    "aspect-ratio:2", "opacity:0.5", "transform:translate(10px,5px)", "visibility:hidden",
    "list-style:square inside", "list-style-type:lower-roman", "text-overflow:ellipsis",
    "-webkit-line-clamp:2", "-webkit-box-orient:vertical", "background:linear-gradient(red,blue)",
    "box-shadow:3px 3px 5px black", "outline:2px solid", "text-decoration:underline",
    "text-shadow:1px 1px red", "text-transform:uppercase", "tab-size:3", "columns:2", "content:'x'"};

static const char *const words[] = {"x", "xx xx", "hello world", "  spaced  ", "&shy;soft&shy;hyphen",
                                    "longwordlongwordlongword", "\t tab", "\n", "&nbsp;&nbsp;", "a-b-c"};

static void gen(AuBuf *b, int depth, int budget)
{
    int n = (int)rnd(5) + 1;
    for (int i = 0; i < n && b->len < (size_t)budget; i++) {
        if (rnd(3) == 0 || depth > 40) {
            au_buf_puts(b, words[rnd(sizeof words / sizeof words[0])]);
            continue;
        }
        const char *t = tags[rnd(sizeof tags / sizeof tags[0])];
        au_buf_printf(b, "<%s", t);
        if (rnd(2)) {
            au_buf_puts(b, " style=\"");
            int nd = (int)rnd(5) + 1;
            for (int d = 0; d < nd; d++)
                au_buf_printf(b, "%s;", decls[rnd(sizeof decls / sizeof decls[0])]);
            au_buf_putc(b, '"');
        }
        if (rnd(4) == 0)
            au_buf_printf(b, " id=e%u", rnd(50));
        if (!strcmp(t, "a"))
            au_buf_puts(b, " href=\"../x.html#e3\"");
        if (!strcmp(t, "img"))
            au_buf_printf(b, " width=%u height=%u alt=\"pic\"", rnd(300), rnd(200));
        if (!strcmp(t, "td") && rnd(2))
            au_buf_printf(b, " colspan=%u", rnd(4));
        if (!strcmp(t, "ol") && rnd(2))
            au_buf_printf(b, " start=%d", (int)rnd(20) - 5);
        au_buf_putc(b, '>');
        if (strcmp(t, "img") && strcmp(t, "br") && strcmp(t, "wbr") && strcmp(t, "hr") && strcmp(t, "input")) {
            gen(b, depth + 1, budget);
            if (rnd(8))
                au_buf_printf(b, "</%s>", t);
        }
    }
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 100;
    rng = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 12345u;
    if (!rng)
        rng = 1;
    static uint8_t fontdata[16384];
    AuFontSet *fonts = ahem_fontset(fontdata);
    AuSurface *s = au_surface_new(320, 240);
    for (int it = 0; it < iters; it++) {
        AuBuf b;
        au_buf_init(&b);
        if (rnd(2))
            au_buf_puts(&b, "<!DOCTYPE html>");
        au_buf_puts(&b, "<style>");
        int nr = (int)rnd(6);
        static const char *const sels[] = {"div", "p::before", "li::marker", "span:hover", "*", "a:active",
                                           "#e1", "td", "ul li", "q::after"};
        for (int r = 0; r < nr; r++) {
            au_buf_printf(&b, "%s{", sels[rnd(sizeof sels / sizeof sels[0])]);
            int nd = (int)rnd(4) + 1;
            for (int d = 0; d < nd; d++)
                au_buf_printf(&b, "%s;", decls[rnd(sizeof decls / sizeof decls[0])]);
            au_buf_puts(&b, "}");
        }
        au_buf_puts(&b, "</style><body>");
        if (rnd(10) == 0) {
            /* very deep nesting */
            int d = 300 + (int)rnd(700);
            for (int k = 0; k < d; k++)
                au_buf_puts(&b, rnd(2) ? "<div style=\"padding-left:1px\">" : "<span>");
            au_buf_puts(&b, "deep");
        }
        gen(&b, 0, 6000);
        if (argc > 3) {
            /* fuzz_page N SEED FILE: each document is written to FILE
             * before it is processed, to catch one that hangs. */
            FILE *f = fopen(argv[3], "wb");
            if (f) {
                fwrite(b.data, 1, b.len, f);
                fclose(f);
            }
        }
        clock_t t0 = clock();
        AuDocView *dv = au_docview_new(fonts, NULL, NULL);
        au_docview_begin(dv, "http://fuzz.example/dir/page.html");
        size_t pos = 0;
        while (pos < b.len) {
            size_t chunk = 1 + rnd(512);
            if (chunk > b.len - pos)
                chunk = b.len - pos;
            au_docview_feed(dv, b.data + pos, chunk);
            pos += chunk;
            if (rnd(3) == 0) {
                au_docview_set_viewport(dv, 1 + (int)rnd(900), 1 + (int)rnd(700));
                au_docview_update(dv);
            }
        }
        au_docview_finish(dv);
        for (int k = 0; k < 3; k++) {
            int w = 1 + (int)rnd(1000), h = 1 + (int)rnd(800);
            au_docview_set_viewport(dv, w, h);
            int cw, ch;
            au_docview_content_size(dv, &cw, &ch);
            AuCanvas *c = au_canvas_new(s);
            int sy = ch > 1 ? (int)rnd((uint32_t)ch) : 0;
            au_docview_paint(dv, c, (AuRect){0, 0, 320, 240}, 0, sy);
            au_canvas_free(c);
            for (int m = 0; m < 6; m++) {
                AuEvent ev;
                memset(&ev, 0, sizeof ev);
                ev.type = m % 3 == 0 ? AU_EVENT_MOUSE_MOVE : m % 3 == 1 ? AU_EVENT_MOUSE_DOWN : AU_EVENT_MOUSE_UP;
                ev.x = (int)rnd(400);
                ev.y = sy + (int)rnd(300);
                au_docview_mouse(dv, &ev);
                au_docview_scroll_at(dv, ev.x, ev.y, (int)rnd(200) - 100, (int)rnd(200) - 100);
            }
            AuBuf d;
            au_buf_init(&d);
            au_layout_dump(au_docview_layout(dv), &d);
            au_buf_free(&d);
            au_docview_fragment_y(dv, "e3");
        }
        au_docview_free(dv);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        if (secs > 0.5) {
            /* Slow documents are worth a look: save them. */
            char name[64];
            snprintf(name, sizeof name, "fuzz-slow-%d.html", it);
            FILE *f = fopen(name, "wb");
            if (f) {
                fwrite(b.data, 1, b.len, f);
                fclose(f);
            }
            fprintf(stderr, "document %d took %.2f s (saved as %s)\n", it, secs, name);
        }
        au_buf_free(&b);
    }
    au_surface_free(s);
    au_fontset_free(fonts);
    printf("%d documents\n", iters);
    return 0;
}
