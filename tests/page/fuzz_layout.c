/* Deterministic fuzzer for style, layout and paint on mutated pages, with
 * script off. Each iteration takes a sample page (the corpus: the pages
 * of tests/page/samples and the layout snippets below), mutates it with
 * markup, style attributes and declarations that hold hostile values,
 * and runs the document view on it:
 *   - feeds it in pieces, answers style sheet requests with a mutated
 *     sample sheet and image requests with corpus images or failures;
 *   - lays it out at two viewport sizes and paints each to a small
 *     surface, at the top and scrolled;
 *   - hit tests, moves and clicks the mouse, types, tabs, scrolls inner
 *     boxes, asks for scroll damage;
 *   - selects text (positions from points, words, paragraphs, the whole
 *     text), reads and measures the range, paints with the selection,
 *     and searches.
 * An iteration over AURA_FUZZ_SLOW_MS (default 30000) of CPU time
 * aborts: layout must not hang. Set AURA_FUZZ_STACK_KB in a plain build
 * to find recursion that would overflow the Mac OS 9 stack.
 *
 * usage: fuzz_layout ITERATIONS SEED SECONDS [PAGE.html | SHEET.css | IMAGE]...
 *        fuzz_layout replay FILE... */
#include "../fuzz.h"
#include "../layout/ahem.h"
#include "layout/selection.h"
#include "page/page.h"

static const char *const dict[] = {
    "<div>", "</div>", "<span>", "</span>", "<p>", "<br>", "<b>", "<a href=#e3>", "</a>", "<img src=i.gif>",
    "<img width=99999999 height=-5 src=i.png>", "<table>", "</table>", "<tr>", "<td>", "<td colspan=1000 rowspan=65535>",
    "<th colspan=0>", "<col span=100000>", "<caption>", "<tbody>", "<ul>", "<ol start=2147483647 reversed>", "<li value=-2147483648>",
    "<li>", "<input>", "<input type=checkbox checked>", "<input type=range min=1e38 max=-1e38 value=NaN>", "<input size=99999999>",
    "<textarea rows=99999999 cols=99999999>", "</textarea>", "<select multiple size=99999999>", "<select>", "<option>", "</select>",
    "<button>", "</button>", "<fieldset><legend>", "<details open><summary>", "<hr size=99999>", "<pre>", "<wbr>", "<q>", "<sup>",
    "<svg width=1e9 height=1e9>", "<svg viewBox='0 0 0 0'>", "</svg>", "<rect width=1e9 height=1e9 />", "<center>", "<font size=9999>",
    "<marquee>", "<iframe width=99999 height=99999>", "<object>", "<canvas width=99999 height=99999>", "<video>", "<progress value=2 max=0>",
    "<meter value=1e38>", "<body bgcolor=#zz text=red marginwidth=99999999>", "<table border=99999 cellspacing=99999999 cellpadding=-1 width=1000%>",
    "<td width=0* height=99999999 nowrap>", "<img hspace=99999999 vspace=99999999 border=99999999>", "<frameset cols='*,*'>",
    "&shy;", "&nbsp;", "&#x200B;", "&#x202E;", "&#xAD;", "\xE2\x80\x8D", "\xF0\x9F\x98\x80", "\xCC\x81\xCC\x81\xCC\x81", "\t", "\n",
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", " x x x x x x x x x x x x x x x x x x x x",
    " style=\"", "\"", " class=c", " id=e3", " dir=rtl", " hidden", " align=center", " width=100%", " height=1e9",
    "<style>", "</style>", "{", "}", ";", ":", "*{", "div{", "td{", "span{", "li::marker{", "p::before{content:", "::after{content:counter(a)}",
    ":hover{", ":focus{", ":checked+*{", "*:has(> *){", ":nth-child(2n+1){", "@media (max-width:500px){", "!important",
    "display:block", "display:inline", "display:inline-block", "display:flex", "display:inline-flex", "display:grid", "display:table",
    "display:table-cell", "display:table-row", "display:table-caption", "display:list-item", "display:contents", "display:none",
    "display:flow-root", "display:inline-table", "display:table-column", "display:table-row-group", "position:absolute", "position:fixed",
    "position:sticky", "position:relative", "float:left", "float:right", "clear:both", "overflow:scroll", "overflow:hidden", "overflow:auto",
    "overflow-x:clip", "visibility:collapse", "box-sizing:border-box", "width:", "height:", "min-width:", "max-width:", "min-height:",
    "max-height:", "margin:", "margin-left:", "margin-top:", "padding:", "border:", "border-width:", "border-radius:", "top:", "left:",
    "right:", "bottom:", "inset:", "z-index:", "font-size:", "line-height:", "letter-spacing:", "word-spacing:", "text-indent:",
    "vertical-align:", "white-space:", "tab-size:", "flex:", "flex-basis:", "flex-grow:", "flex-shrink:", "flex-wrap:wrap",
    "flex-direction:column-reverse", "order:", "gap:", "align-items:baseline", "justify-content:space-between", "aspect-ratio:",
    "columns:", "column-count:", "border-spacing:", "border-collapse:collapse", "table-layout:fixed", "caption-side:bottom",
    "list-style:inside", "counter-reset:a ", "counter-increment:a ", "transform:", "box-shadow:", "text-shadow:", "outline:",
    "opacity:", "background:", "background-size:", "background-position:", "background-attachment:fixed", "clip-path:", "mask-image:",
    "filter:", "object-fit:cover", "direction:rtl", "writing-mode:vertical-rl", "text-align:justify", "word-break:break-all",
    "overflow-wrap:anywhere", "hyphens:auto", "text-overflow:ellipsis", "text-transform:uppercase", "font-variant:small-caps",
    "content:'x'", "quotes:'<' '>'", "position-anchor:", "contain:strict", "will-change:transform", "isolation:isolate",
    "mix-blend-mode:multiply", "scroll-snap-type:x", "resize:both", "zoom:", "1e38px", "-1e38px", "1e9px", "-1e9px", "99999999px",
    "-99999999px", "0", "0px", "-1px", "1e-9px", "50%", "-50%", "1e9%", "1e38%", "1e9em", "100vw", "1e9vh", "calc(1px/0)",
    "calc(0/0*1px)", "calc(100% + 1e38px)", "calc(-1 * infinity * 1px)", "calc(1e38px * 1e38)", "min(1e38px, 100%)", "auto", "none",
    "normal", "inherit", "fit-content", "min-content", "max-content", "stretch", "1e9", "-1e9", "2147483647", "-2147483648", "1e38",
    "0.0000001", "rotate(1e38deg)", "scale(0)", "scale(1e38)", "scale(-1,1e-38)", "translate(1e38px,-1e38px)", "matrix(0,0,0,0,0,0)",
    "matrix(1e38,1e38,1e38,1e38,1e38,1e38)", "skew(90deg)", "perspective(0)", "0 0 1e9px 1e9px red", "1e9px 1e9px 0 -1e9px",
    "inset 0 0 1e9px", "1e9px solid red", "1e9px dashed", "linear-gradient(1e38deg,red -1e30px,blue 1e30px)",
    "radial-gradient(circle 0 at 1e9px 1e9px,red,blue)", "conic-gradient(from 1e38deg,red,blue)",
    "repeating-linear-gradient(red 0,blue 1e-9px)", "url(i.gif)", "url(a.css)", "circle(1e9px)", "inset(1e9px)", "polygon(0 0,1e9px 1e9px)",
    "blur(1e9px)", "drop-shadow(0 0 1e9px red)", "1e38/1e-38", "0/0", "1/0", "1e9 1e9 -1px", "repeat(1000000,1fr)", "span 1000000",
};
#define NDICT ((int)(sizeof dict / sizeof dict[0]))

static const char *const snippets[] = {
    "<!DOCTYPE html><div style='display:flex;flex-wrap:wrap;gap:4px'><div style='flex:1 1 0'>a</div><p style='flex:2'>b b b</p>"
    "<span style='order:-1'>c</span></div><div style='position:relative;height:50px'><div style='position:absolute;inset:5px'>abs</div>"
    "<div style='position:sticky;top:0'>sticky</div></div><p id=e3>target text for selection and find</p>",
    "<table style='border-collapse:collapse;width:100%'><caption>c</caption><colgroup><col span=2 width=40><col></colgroup>"
    "<thead><tr><th rowspan=2>h<th colspan=2>i</thead><tr><td>1<td>2<tr><td colspan=3 style='padding:5%'>wide"
    "<table><tr><td>nested<td style='width:1000%'>x</table></table>",
    "<div style='float:left;width:60%'>float one</div><div style='float:right;width:60%'>float two</div><p style='clear:left'>"
    "text <img width=40 height=30 align=left alt=a> around <span style='display:inline-block;width:50px;height:20px'>ib</span> "
    "more text that wraps around the floats and goes on for a while<br clear=all>after</p>",
    "<ol style='counter-reset:a 5' start=10><li>one<li style='list-style:inside square'>two<ul><li>deep<li style='display:block'>x</ul>"
    "</ol><p style='text-indent:2em;line-height:0.5;letter-spacing:3px;white-space:pre-wrap'>pre  wrap\ttab\nline</p>"
    "<q>quote <q>inner</q></q><div style='columns:2'>a b c d e f g h i j k l m n o p</div>",
    "<form><input value=text size=10><input type=password value=pw><input type=checkbox checked><input type=radio name=r>"
    "<input type=range value=5><input type=file><input type=submit value=Go><button>b <b>bold</b></button><select><option>a"
    "<option selected>bb</select><select multiple size=3><option>x<optgroup label=g><option>y</optgroup></select>"
    "<textarea rows=3 cols=20>line one\nline two is longer and wraps</textarea><fieldset><legend>l</legend>in</fieldset>"
    "<details><summary>sum</summary>body</details><progress value=.3></progress><meter value=.6></meter></form>",
    "<div style='overflow:scroll;height:40px;width:80px'>scroll me scroll me scroll me scroll me scroll me scroll me</div>"
    "<div style='transform:rotate(10deg) scale(1.5);opacity:.5;box-shadow:2px 2px 4px black;border-radius:8px;border:2px dashed red;"
    "background:linear-gradient(red,blue);outline:1px solid;text-shadow:1px 1px 2px gray'>painted</div>"
    "<div style='position:fixed;right:0;bottom:0;z-index:5'>fixed</div><svg width=40 height=40 viewBox='0 0 10 10'>"
    "<circle cx=5 cy=5 r=4 fill=red /><text y=8 font-size=4>s</text></svg>",
};

typedef struct Req {
    uint32_t id;
    AuResourceKind kind;
} Req;

typedef struct Loader {
    Req q[48];
    int n;
    uint32_t next;
} Loader;

static uint32_t req_fn(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    Loader *l = (Loader *)ctx;
    (void)dv, (void)url;
    if (l->n == 48 || l->next >= 40 || fz_rnd(12) == 0)
        return 0;
    l->q[l->n].id = ++l->next;
    l->q[l->n].kind = kind;
    l->n++;
    return l->next;
}

static AuFontSet *fonts;
static AuSurface *surface;
static long slow_ms = 30000;
static FzCorpus pages, sheets, images;

static void answer(Loader *l, AuDocView *dv)
{
    Req r;
    const FzCorpus *c;
    if (!l->n)
        return;
    r = l->q[0];
    memmove(l->q, l->q + 1, (size_t)(--l->n) * sizeof l->q[0]);
    c = r.kind == AU_RES_STYLESHEET ? &sheets : &images;
    if (!c->n || r.kind == AU_RES_FONT || r.kind == AU_RES_OTHER || fz_rnd(6) == 0) {
        au_docview_resource_done(dv, r.id, fz_rnd(2) != 0);
        return;
    }
    {
        FzBuf t = {0};
        const FzBuf *o = &c->v[fz_rnd((uint32_t)c->n)];
        size_t pos = 0;
        fz_set(&t, o->d, o->n);
        if (r.kind == AU_RES_STYLESHEET ? fz_rnd(4) != 0 : fz_rnd(4) == 0)
            fz_mutate(&t, dict, NDICT, &sheets, 24 << 10);
        while (pos < t.n) {
            size_t chunk = 1 + fz_rnd(fz_rnd(2) ? 100 : 8000);
            if (chunk > t.n - pos)
                chunk = t.n - pos;
            au_docview_resource_data(dv, r.id, t.d + pos, chunk);
            pos += chunk;
            if (fz_rnd(8) == 0)
                au_docview_update(dv);
        }
        au_docview_resource_done(dv, r.id, true);
        free(t.d);
    }
}

static void paint(AuDocView *dv, int sx, int sy)
{
    AuCanvas *c = au_canvas_new(surface);
    au_docview_paint(dv, c, (AuRect){0, 0, (float)surface->width, (float)surface->height}, sx, sy);
    au_canvas_free(c);
}

static void select_text(AuDocView *dv, int sx, int sy)
{
    AuLayout *L = au_docview_layout(dv);
    AuTextPos a, b;
    AuTextRange r, w;
    AuBuf text;
    AuRect box;
    bool ha, hb;
    if (!L)
        return;
    ha = au_layout_text_pos_at(L, (float)fz_rnd(400) - 50, (float)fz_rnd(300) - 50, (float)sx, (float)sy, &a);
    hb = au_layout_text_pos_at(L, (float)fz_rnd(400), (float)fz_rnd(300), (float)sx, (float)sy, &b);
    au_buf_init(&text);
    if (ha && hb) {
        r = au_text_range_make(a, b);
        au_layout_range_text(L, &r, &text);
        au_layout_range_rect(L, &r, &box);
        au_layout_set_selection(L, &r, 0xFF3366CCu);
        paint(dv, sx, sy);
    }
    if (ha) {
        if (au_layout_word_at(L, &a, &w)) {
            au_buf_clear(&text);
            au_layout_range_text(L, &w, &text);
            au_layout_set_selection(L, &w, 0x803366CCu);
        }
        if (au_layout_block_at(L, &a, &w)) {
            au_layout_range_rect(L, &w, &box);
            au_layout_set_selection(L, &w, 0xFF000000u);
            paint(dv, sx, sy);
        }
    }
    if (au_layout_all_text(L, &w)) {
        au_buf_clear(&text);
        au_layout_range_text(L, &w, &text);
        if (text.len > (64u << 20))
            abort();
        au_layout_set_selection(L, &w, 0xFF3366CCu);
        paint(dv, sx, sy);
    }
    {
        static const char *const needles[] = {"e", "text", "x x", "\xC3\xA9", "", " ", "one"};
        const char *needle = needles[fz_rnd(sizeof needles / sizeof needles[0])];
        AuTextPos from;
        int k;
        memset(&from, 0, sizeof from);
        if (ha && fz_rnd(2))
            from = a;
        for (k = 0; k < 4; k++) {
            bool fwd = fz_rnd(2) != 0;
            if (!au_layout_find(L, needle, &from, fwd, fz_rnd(2) != 0, &w))
                break;
            au_layout_set_selection(L, &w, 0xFFFFFF00u);
            from = fwd ? w.end : w.start;
        }
    }
    /* Clear it: the selection names nodes, and the caller keeps them
     * alive only while it is set. */
    au_layout_set_selection(L, NULL, 0);
    au_buf_free(&text);
}

static void run_one(const FzBuf *in)
{
    Loader l;
    AuLoader loader;
    AuDocView *dv;
    size_t pos = 0;
    int k, m;
    clock_t t0 = clock();
    long ms;
    memset(&l, 0, sizeof l);
    memset(&loader, 0, sizeof loader);
    loader.ctx = &l;
    loader.request = req_fn;
    dv = au_docview_new(fonts, &loader, NULL);
    au_docview_begin(dv, "http://fuzz.example/dir/page.html");
    while (pos < in->n) {
        size_t chunk = 1 + fz_rnd(fz_rnd(3) ? 300 : 6000);
        if (chunk > in->n - pos)
            chunk = in->n - pos;
        au_docview_feed(dv, (const char *)in->d + pos, chunk);
        pos += chunk;
        if (fz_rnd(4) == 0) {
            au_docview_set_viewport(dv, 1 + (int)fz_rnd(900), 1 + (int)fz_rnd(700));
            au_docview_update(dv);
        }
        if (fz_rnd(3) == 0)
            answer(&l, dv);
    }
    au_docview_finish(dv);
    for (k = 0; k < 60 && l.n; k++)
        answer(&l, dv);
    for (k = 0; k < 2; k++) {
        AuIRect dmg[8];
        AuHitResult hr;
        AuBuf d;
        int cw, ch, sx, sy, w = k ? 1 + (int)fz_rnd(1200) : 640, h = k ? 1 + (int)fz_rnd(900) : 480;
        au_docview_set_viewport(dv, w, h);
        au_docview_update(dv);
        au_docview_content_size(dv, &cw, &ch);
        paint(dv, 0, 0);
        sx = cw > w ? (int)fz_rnd((uint32_t)(cw - w)) : 0;
        sy = ch > h ? (int)fz_rnd((uint32_t)(ch - h)) : 0;
        if (fz_rnd(8) == 0)
            sy = (int)fz_next(); /* anywhere, also out of range */
        au_docview_set_scroll(dv, sx, sy);
        paint(dv, sx, sy);
        au_docview_scroll_damage(dv, sx, sy, sx + (int)fz_rnd(64) - 32, sy + (int)fz_rnd(200) - 100, dmg, 8);
        for (m = 0; m < 8; m++) {
            AuEvent ev;
            memset(&ev, 0, sizeof ev);
            ev.type = m % 4 == 0 ? AU_EVENT_MOUSE_MOVE : m % 4 == 1 ? AU_EVENT_MOUSE_DOWN : m % 4 == 2 ? AU_EVENT_MOUSE_UP : AU_EVENT_MOUSE_MOVE;
            if (m % 4 != 2) {
                ev.x = (int)fz_rnd((uint32_t)w + 40) - 20;
                ev.y = (int)fz_rnd((uint32_t)h + 40) - 20;
            }
            ev.clicks = 1 + (int)fz_rnd(3);
            au_docview_mouse(dv, &ev);
            au_docview_hit_test(dv, ev.x, ev.y, &hr);
            au_docview_scroll_at(dv, ev.x, ev.y, (int)fz_rnd(200) - 100, (int)fz_rnd(200) - 100);
            au_docview_control_at(dv, ev.x, ev.y);
        }
        for (m = 0; m < 6; m++) {
            static const int keys[] = {AU_KEY_TAB, AU_KEY_LEFT, AU_KEY_RIGHT, AU_KEY_UP, AU_KEY_DOWN, AU_KEY_BACKSPACE, AU_KEY_DELETE,
                                       AU_KEY_HOME, AU_KEY_END, AU_KEY_PAGE_DOWN, AU_KEY_RETURN};
            AuEvent ev;
            memset(&ev, 0, sizeof ev);
            ev.type = AU_EVENT_KEY_DOWN;
            if (fz_rnd(2))
                ev.key = keys[fz_rnd(sizeof keys / sizeof keys[0])];
            else
                ev.ch = fz_rnd(4) ? 'a' + fz_rnd(26) : 0x1F600;
            au_docview_key(dv, &ev);
            if (fz_rnd(3) == 0)
                paint(dv, sx, sy);
        }
        au_docview_blink(dv, 600);
        select_text(dv, sx, sy);
        au_buf_init(&d);
        au_layout_dump(au_docview_layout(dv), &d);
        au_buf_free(&d);
        au_docview_fragment_y(dv, "e3");
    }
    au_docview_free(dv);
    ms = (long)((double)(clock() - t0) * 1000 / CLOCKS_PER_SEC);
    if (ms > slow_ms) {
        printf("fuzz_layout: an input of %lu bytes took %ld ms\n", (unsigned long)in->n, ms);
        abort();
    }
}

static int has_suffix(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && !strcmp(s + n - m, suf);
}

static int fuzz_main(int argc, char **argv)
{
    long iters = 0, it;
    int first, i;
    FzBuf in = {0};
    static uint8_t fontdata[16384];
    const char *e = getenv("AURA_FUZZ_SLOW_MS");
    int replay = fz_args(argc, argv, &iters, &first);

    if (e)
        slow_ms = atol(e);
    fonts = ahem_fontset(fontdata);
    surface = au_surface_new(240, 180);
    fz_corpus_add_str(&sheets, "body{margin:8px;font:16px Ahem}div{border:1px solid}.c{float:left;width:50%}"
                               "td{padding:2px}@import url(b.css);a:hover{color:red}p::before{content:'>'}");
    if (replay) {
        for (i = first; i < argc; i++) {
            if (!fz_read_file(argv[i], &in, 0))
                continue;
            fz_replay_seed(argv[i]);
            run_one(&in);
            printf("%s: ok\n", argv[i]);
        }
        return 0;
    }
    for (i = 0; i < (int)(sizeof snippets / sizeof snippets[0]); i++)
        fz_corpus_add_str(&pages, snippets[i]);
    for (i = first; i < argc; i++) {
        if (has_suffix(argv[i], ".html"))
            fz_corpus_add_file(&pages, argv[i], 24 << 10);
        else if (has_suffix(argv[i], ".css"))
            fz_corpus_add_file(&sheets, argv[i], 24 << 10);
        else
            fz_corpus_add_file(&images, argv[i], 24 << 10);
    }
    for (it = fz_first; it < iters && !fz_expired(); it++) {
        fz_iteration(it);
        const FzBuf *src = &pages.v[fz_rnd((uint32_t)pages.n)];
        fz_set(&in, src->d, src->n);
        fz_mutate(&in, dict, NDICT, &pages, 96 << 10);
        if (fz_rnd(3) == 0)
            fz_mutate(&in, dict, NDICT, &pages, 96 << 10);
        fz_save(in.d, in.n);
        run_one(&in);
    }
    printf("fuzz_layout: %ld iterations\n", it - fz_first);
    au_surface_free(surface);
    au_fontset_free(fonts);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
