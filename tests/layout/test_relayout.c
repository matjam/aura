/* Incremental layout: a document view whose viewport changes must end up
 * with the same boxes and pixels as one loaded at that size, and must do
 * no more work than the change needs.
 *
 * Each page is loaded once and resized through a sequence of viewports;
 * after each step its box tree dump and its painted pixels are compared
 * with a fresh document view at that size. Counters then check what the
 * caches keep: the box tree on a viewport change, intrinsic widths across
 * widths, and that a style, font or content change drops them.
 *
 * Usage: test_relayout SAMPLES_DIR */
#include "host.h"
#include "render.h"
#include "page/script.h" /* au_dv_dom_changed */

#include <stdio.h>
#include <stdlib.h>
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

static AuFontSet *fonts;

typedef struct View {
    AuDocView *dv;
    AuHostLoader *loader;
} View;

static void pump(View *v)
{
    for (int round = 0; round < 20; round++) {
        au_docview_update(v->dv);
        if (!au_host_loader_pump(v->loader, v->dv))
            break;
    }
}

static View view_new(const char *html, size_t len, const char *url, int w, int h)
{
    View v;
    v.loader = au_host_loader_new();
    AuLoader li;
    au_host_loader_interface(v.loader, &li);
    v.dv = au_docview_new(fonts, &li, NULL);
    au_docview_load(v.dv, html, len, url);
    au_docview_set_viewport(v.dv, w, h);
    pump(&v);
    return v;
}

static void view_free(View *v)
{
    au_docview_free(v->dv);
    au_host_loader_free(v->loader);
}

static void view_resize(View *v, int w, int h)
{
    au_docview_set_viewport(v->dv, w, h);
    pump(v);
}

static char *dump(View *v)
{
    AuBuf b;
    au_buf_init(&b);
    au_layout_dump(au_docview_layout(v->dv), &b);
    return au_buf_take(&b, NULL);
}

static AuSurface *paint(View *v, int w, int h, int scroll)
{
    AuSurface *s = au_surface_new(w, h);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(v->dv, c, (AuRect){0, 0, (float)w, (float)h}, 0, scroll);
    /* Images are requested by the first paint: deliver them, paint again. */
    for (int round = 0; round < 5 && au_host_loader_pump(v->loader, v->dv); round++) {
        au_docview_update(v->dv);
        au_docview_paint(v->dv, c, (AuRect){0, 0, (float)w, (float)h}, 0, scroll);
    }
    au_canvas_free(c);
    return s;
}

static bool same_pixels(const AuSurface *a, const AuSurface *b)
{
    if (a->width != b->width || a->height != b->height)
        return false;
    for (int y = 0; y < a->height; y++)
        if (memcmp(a->pixels + (size_t)y * (size_t)a->stride, b->pixels + (size_t)y * (size_t)b->stride,
                   (size_t)a->width * sizeof(AuPixel)) != 0)
            return false;
    return true;
}

/* Where two dumps first differ, for the failure message. */
static void first_diff(const char *a, const char *b, char *out, size_t cap)
{
    size_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    size_t s = i;
    while (s > 0 && a[s - 1] != '\n')
        s--;
    size_t ea = s, eb = s;
    while (a[ea] && a[ea] != '\n')
        ea++;
    while (b[eb] && b[eb] != '\n')
        eb++;
    snprintf(out, cap, "resized \"%.*s\", fresh \"%.*s\"", (int)(ea - s > 150 ? 150 : ea - s), a + s,
             (int)(eb - s > 150 ? 150 : eb - s), b + s);
}

static const int sizes[][2] = {{1000, 700}, {656, 700}, {656, 684}, {1200, 700}, {400, 300},
                               {401, 300},  {1000, 700}, {1000, 300}, {320, 700}, {1440, 900}};
#define NSIZES (sizeof sizes / sizeof sizes[0])

/* Resizes one view through the sizes and compares it with a fresh view
 * at each. */
static void check_page(const char *name, const char *html, size_t len, const char *url)
{
    View a = view_new(html, len, url, 800, 600);
    for (size_t i = 0; i < NSIZES; i++) {
        int w = sizes[i][0], h = sizes[i][1];
        view_resize(&a, w, h);
        View b = view_new(html, len, url, w, h);
        /* Paint first: a paint may bring in images, which change layout. */
        int cw, ch;
        au_docview_content_size(b.dv, &cw, &ch);
        int scroll = ch > 2 * h ? h / 2 : 0;
        AuSurface *sa = paint(&a, w, h, scroll), *sb = paint(&b, w, h, scroll);
        AuSurface *sa2 = paint(&a, w, h, scroll), *sb2 = paint(&b, w, h, scroll);
        char *da = dump(&a), *db = dump(&b);
        if (strcmp(da, db) != 0) {
            char msg[400];
            first_diff(da, db, msg, sizeof msg);
            CHECK(false, "%s at %dx%d: boxes differ from a fresh layout: %s", name, w, h, msg);
        } else {
            CHECK(true, "ok");
        }
        CHECK(same_pixels(sa2, sb2), "%s at %dx%d: pixels differ from a fresh layout", name, w, h);
        au_surface_free(sa);
        au_surface_free(sb);
        au_surface_free(sa2);
        au_surface_free(sb2);
        au_free(da);
        au_free(db);
        view_free(&b);
    }
    view_free(&a);
}

static void check_file(const char *dir, const char *file)
{
    char path[2048], url[2100];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    char *data;
    size_t len;
    if (!au_platform_read_file(path, &data, &len)) {
        CHECK(false, "cannot read %s", path);
        return;
    }
    snprintf(url, sizeof url, "file://%s", path);
    check_page(file, data, len, url);
    au_free(data);
}

static void check_inline(const char *name, const char *html)
{
    check_page(name, html, strlen(html), "file:///relayout/test.html");
}

static void counts(View *v, uint32_t *builds, uint32_t *passes, uint32_t *intrinsic)
{
    AuLayoutStats st;
    au_layout_stats(au_docview_layout(v->dv), &st);
    *builds = st.tree_builds;
    *passes = st.layout_passes;
    *intrinsic = st.intrinsic_runs;
}

static AuElement *by_id(AuDocument *doc, const char *id)
{
    AuAtom a = au_atom_lookup(id, strlen(id));
    for (AuNode *n = au_node_next(&doc->node, &doc->node); a && n; n = au_node_next(n, &doc->node))
        if (au_is_element(n) && au_as_element(n)->id == a)
            return au_as_element(n);
    return NULL;
}

static float width_of(View *v, const char *id)
{
    AuRect r = {0, 0, 0, 0};
    AuElement *e = by_id(au_docview_document(v->dv), id);
    if (!e || !au_layout_element_rect(au_docview_layout(v->dv), e, &r))
        return -1;
    return r.w;
}

static void test_counters(void)
{
    static const char *html =
        "<!DOCTYPE html><style>body{margin:0;font:16px sans-serif}"
        "@media (max-width:500px){#hid{display:block !important}}"
        ".f{float:left}td{padding:2px}</style>"
        "<div class=f id=f>one two three four five six seven eight nine ten</div>"
        "<table id=t><tr><td>alpha beta gamma<td>delta epsilon</table>"
        "<div style='display:flex'><div id=x>flex item one</div><div>flex item two</div></div>"
        "<p id=p>Some text that wraps over several lines when the window is narrow enough for it to do so.</p>"
        "<div id=hid style='display:none'>hidden until narrow</div>";
    View v = view_new(html, strlen(html), "file:///relayout/c.html", 1000, 700);
    uint32_t b0, p0, i0, b1, p1, i1;
    counts(&v, &b0, &p0, &i0);

    /* Height only: one pass, the same tree, no intrinsic width again. */
    view_resize(&v, 1000, 650);
    counts(&v, &b1, &p1, &i1);
    CHECK(b1 == b0, "a height change rebuilt the box tree (%u builds)", b1 - b0);
    CHECK(p1 == p0 + 1, "a height change took %u layout passes", p1 - p0);
    CHECK(i1 == i0, "a height change measured %u intrinsic widths again", i1 - i0);

    /* Width, with no media query crossing: the tree and the intrinsic
     * widths of the float, the table and the flex items are kept. */
    view_resize(&v, 800, 650);
    counts(&v, &b0, &p0, &i0);
    CHECK(b0 == b1, "a width change rebuilt the box tree (%u builds)", b0 - b1);
    CHECK(p0 == p1 + 1, "a width change took %u layout passes", p0 - p1);
    CHECK(i0 == i1, "a width change measured %u intrinsic widths again", i0 - i1);
    CHECK(width_of(&v, "hid") < 0, "the hidden element has a box");

    /* Across the media query: the element shown has a box. */
    view_resize(&v, 480, 650);
    CHECK(width_of(&v, "hid") == 480, "an element shown by a media query has width %g", width_of(&v, "hid"));
    view_resize(&v, 800, 650);
    CHECK(width_of(&v, "hid") < 0, "an element hidden by a media query kept its box");

    /* A content change: the intrinsic width of the float follows. */
    float fw = width_of(&v, "f");
    AuDocument *doc = au_docview_document(v.dv);
    AuElement *f = by_id(doc, "f");
    AuNode *t = au_text_new(doc, " and eleven", 11);
    au_node_append(&f->node, t);
    au_dv_dom_changed(v.dv, &f->node, 0, 1);
    pump(&v);
    CHECK(width_of(&v, "f") > fw + 10, "the float kept its width %g after its text grew (now %g)", fw,
          width_of(&v, "f"));

    /* A font size change (the reader's text zoom): widths are measured
     * again, not answered from the caches. */
    fw = width_of(&v, "f");
    float xw = width_of(&v, "x");
    AuCssFontPrefs fp;
    au_css_font_prefs_default(&fp);
    fp.text_zoom = 1.5f;
    au_docview_set_font_prefs(v.dv, &fp);
    pump(&v);
    View w = view_new(html, strlen(html), "file:///relayout/c.html", 800, 650);
    AuElement *wf = by_id(au_docview_document(w.dv), "f");
    au_node_append(&wf->node, au_text_new(au_docview_document(w.dv), " and eleven", 11));
    au_dv_dom_changed(w.dv, &wf->node, 0, 1);
    au_docview_set_font_prefs(w.dv, &fp);
    pump(&w);
    CHECK(width_of(&v, "f") > fw * 1.3f, "the float is %g wide after text zoom 1.5 (was %g)", width_of(&v, "f"), fw);
    CHECK(width_of(&v, "x") > xw * 1.3f, "the flex item is %g wide after text zoom 1.5 (was %g)", width_of(&v, "x"),
          xw);
    char *dv = dump(&v), *dw = dump(&w);
    CHECK(!strcmp(dv, dw), "after a text and font change the boxes differ from a fresh layout");
    au_free(dv);
    au_free(dw);
    view_free(&w);
    view_free(&v);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_relayout SAMPLES_DIR\n");
        return 2;
    }
    au_host_use_system_fonts(false);
    fonts = au_fontset_new();
    au_platform_load_fonts(fonts);

    static const char *samples[] = {"wiki.html",     "hn.html",       "hn-item.html", "blog.html",    "docs.html",
                                    "forum.html",    "reddit.html",   "wikitable.html", "qa-boxes.html", "qa-edge.html",
                                    "qa-flex.html",  "qa-inline.html", "qa-quirks.html", "qa-tables.html", "qa-forms.html",
                                    "emoji.html",    "svg.html",      "images.html",  "grid.html"};
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++)
        check_file(argv[1], samples[i]);

    check_inline("viewport units and percentages",
                 "<!DOCTYPE html><style>html,body{height:100%;margin:0}"
                 "#a{height:50%;background:#ccd}#b{width:50vw;height:20vh;background:#dcc}"
                 "#c{min-height:10vh;width:calc(100vw - 100px);background:#cdc}"
                 "#d{font-size:3vw}</style><div id=a>half</div><div id=b>vw vh</div><div id=c>calc</div>"
                 "<p id=d>Text sized by the viewport width, long enough to wrap in a narrow window.</p>");
    check_inline("fixed, absolute and sticky boxes",
                 "<!DOCTYPE html><style>body{margin:0}#f{position:fixed;right:0;bottom:0;width:30%;height:20%;"
                 "background:#f00}#a{position:absolute;left:10%;top:50%;width:25%;background:#0a0}"
                 "#w{width:300px;overflow:hidden;background:#eee}#i{position:absolute;right:5px;top:5px;"
                 "width:10%;height:10%;background:#00f}#s{position:sticky;top:0;background:#ff0}"
                 "#r{position:relative;width:300px;height:100px;background:#ddd}"
                 "#ra{position:absolute;right:0;bottom:0;width:50%;height:50%;background:#888}</style>"
                 "<div id=s>sticky</div><div id=f>fixed</div><div id=a>absolute box with some text in it</div>"
                 "<div id=w>a fixed-width block holding <span id=i>an absolute box</span> placed against the "
                 "viewport</div><div id=r><div id=ra>inner</div></div>"
                 "<p>one</p><p>two</p><p>three</p><div style='height:1500px'></div>");
    check_inline("media queries",
                 "<!DOCTYPE html><style>body{margin:8px}#n{display:none}#m{float:left;width:200px}"
                 "@media (max-width:700px){#n{display:block}#m{float:none;width:auto}li{display:inline}"
                 "li::after{content:' / '}}@media (max-height:400px){#h{display:none}}"
                 "@media (min-width:1100px){#w{display:table-cell;padding:10px}}</style>"
                 "<div id=n>narrow only</div><div id=m>a float that stops floating</div>"
                 "<ul><li>one</li><li>two</li><li>three</li></ul><div id=h>tall only</div>"
                 "<div><div id=w>cell when wide</div><div id=w2>next</div></div>"
                 "<p>Following text that flows beside the float while the window is wide enough.</p>");
    check_inline("floats, tables and flex",
                 "<!DOCTYPE html><style>.l{float:left;width:30%;background:#fdd}.r{float:right;background:#dfd}"
                 "table{border-collapse:collapse;width:60%}td{border:1px solid #000;padding:3px}"
                 ".fx{display:flex;flex-wrap:wrap;gap:4px}.fx>div{flex:1 1 120px;background:#ddf}"
                 ".ib{display:inline-block;width:45%;vertical-align:top;background:#eee}"
                 ".col{display:flex;flex-direction:column;height:30vh}.col>div{flex:1}</style>"
                 "<div class=l>left float with enough text to wrap inside thirty percent</div>"
                 "<div class=r>right float</div><p>Paragraph text beside the floats. It is long enough to "
                 "run past them and continue below, so both the shortened and the full lines are used.</p>"
                 "<table><tr><td>a cell with words<td>another cell<td>3</tr><tr><td colspan=2>spanning "
                 "cell with a longer run of text<td><table><tr><td>nested<td>table</table></tr></table>"
                 "<div class=fx><div>one</div><div>two two</div><div>three three three</div>"
                 "<div><div class=fx><div>inner a</div><div>inner b</div></div></div></div>"
                 "<div class=ib>inline block one with text</div><div class=ib>inline block two</div>"
                 "<div class=col><div>c1</div><div>c2</div></div>");
    check_inline("quirks mode percentage heights",
                 "<!-- quirks --><html><body style='margin:0'><table height='100%' width='100%'><tr>"
                 "<td valign=middle><div style='height:50%;background:#ccc'>half of the cell</div></td>"
                 "<td width=100>side</td></tr></table><div><div style='height:30%;background:#f00'>thirty"
                 "</div></div></body></html>");
    check_inline("overflow and nested scrollers",
                 "<!DOCTYPE html><div style='overflow:auto;height:50vh;width:80%;border:1px solid'>"
                 "<div style='width:150%;height:200px;background:linear-gradient(#fff,#888)'>wide</div>"
                 "<p>text in a scroller</p></div><pre style='overflow:auto'>a long preformatted line that "
                 "does not wrap and so overflows a narrow window by quite a lot of pixels</pre>"
                 "<div style='columns:2'>not supported but must not break</div>");

    test_counters();

    au_fontset_free(fonts);
    printf("relayout: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
