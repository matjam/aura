/* Deterministic fuzzer for the CSS tokenizer, parser, cascade and the
 * layout and paint code that reads computed values. Each iteration takes
 * a corpus entry (sample sheets, the case files, the snippets below),
 * mutates it with CSS tokens (functions nested deeply, huge, negative and
 * non-finite numbers, selectors nested deeply), and
 *   - parses it as a sheet, an inline style, a selector list, a media
 *     query and a supports condition, and serializes what parsed;
 *   - loads a small document whose linked sheet is the input (delivered
 *     in pieces; its @imports answered with other mutated entries), with
 *     a piece of it as a style attribute, then styles, lays out at two
 *     sizes, paints to a small surface, hit tests and dumps the boxes.
 * It checks termination: an iteration that takes more than
 * AURA_FUZZ_SLOW_MS (default 20000) of CPU time aborts. Under ASan/UBSan
 * it finds memory errors.
 *
 * usage: fuzz_css ITERATIONS SEED SECONDS [CORPUS...]
 *        fuzz_css replay FILE... */
#include "../fuzz.h"
#include "../layout/ahem.h"
#include "page/page.h"

static const char *const dict[] = {
    "{", "}", "(", ")", "[", "]", ";", ":", ",", "!important", "/*", "*/", "\\", "\"", "'", "\n", " ", "url(", "url(x)",
    "calc(", "calc(1px/0)", "calc(0/0)", "calc(1e38px*1e38)", "calc(infinity*1px)", "calc(-infinity*1px)", "calc(NaN*1px)",
    "calc(100% - 1e9px)", "min(", "max(", "clamp(", "clamp(1px,2em,3%)", "var(--a)", "var(--a,", "var(--b,var(--a))",
    "--a:", "--b:var(--a)var(--a)var(--a);", "--a:var(--b);", "env(", "attr(title)", "attr(", "sin(", "pow(1e30,1e30)",
    "sqrt(-1)", "mod(1,0)", "round(1px,0px)", "abs(", "sign(", "hypot(", "log(0)", "exp(1e9)", "rgb(", "rgb(1e9 0 0 / 2)",
    "hsl(1e38 200% 50%)", "color-mix(in srgb, red, blue)", "light-dark(", "rgb(from red r g b / alpha)",
    "hsl(from rgb(from red r g b) h s calc(l * 1e38 / 0))", "calc(r * g + b - alpha)", "from", "rgb(from", "linear-gradient(", "radial-gradient(",
    "conic-gradient(", "repeating-linear-gradient(red 0 0)", "linear-gradient(red -1e30px, blue 1e30px)", "1e38px",
    "-1e38px", "1e9em", "-9999999px", "0.0000001px", "99999999999999999999px", "1e400px", "-0", "100000%", "1e9", "1e38vw",
    "1ch", "1rem", "1lh", "1fr", "1e9fr", "1deg", "1e38turn", "1ms", "1dppx", "#fff", "#12345678", "auto", "none",
    "inherit", "initial", "unset", "revert", "revert-layer", "currentColor", "width:", "height:", "min-width:",
    "max-height:", "margin:", "padding:", "border:", "border-width:", "border-radius:", "font:", "font-size:",
    "line-height:", "letter-spacing:", "word-spacing:", "text-indent:", "display:", "position:", "float:", "top:", "left:",
    "z-index:", "flex:", "flex-basis:", "flex-grow:", "flex-shrink:", "order:", "gap:", "grid-template-columns:",
    "repeat(1000000,1fr)", "repeat(auto-fill,minmax(0,1fr))", "column-count:", "transform:", "matrix(1e38,0,0,1e38,0,0)",
    "rotate(1e38deg)", "scale(0)", "translate(1e38px,-1e38px)", "matrix3d(", "perspective(0)", "box-shadow:",
    "0 0 1e9px 1e9px red", "text-shadow:", "outline:", "opacity:", "background:", "background-size:", "background-position:",
    "mask-image:", "clip-path:", "filter:", "blur(1e9px)", "content:", "counter(a)", "counters(a,'.')", "counter-reset:a 2147483647",
    "counter-increment:a -2147483648", "quotes:", "open-quote", "tab-size:", "aspect-ratio:", "1e38/1e-38", "0/0",
    "object-fit:", "overflow:", "white-space:", "vertical-align:", "table-layout:", "border-spacing:", "list-style:",
    "animation:", "transition:", "will-change:", "contain:", "inset:", "all:", "block", "inline", "inline-block", "flex",
    "inline-flex", "grid", "table", "table-cell", "table-row", "list-item", "contents", "flow-root", "absolute", "fixed",
    "sticky", "relative", "hidden", "scroll", "nowrap", "pre-wrap", "@media", "@media (min-width:", "@media not all and",
    "@supports", "@supports (", "@supports selector(", "@import", "@import url(a.css)", "@import 'b.css' layer(x) supports(display:flex) screen;",
    "@layer", "@layer a,b;", "@container", "@container (min-width:", "@container card (width > 1e9px) and (height <",
    "@container not (orientation: portrait) or (aspect-ratio > 1/0)", "@container style(--a: 1)", "@container x y (",
    "container-type:", "container-type:size", "container-type:inline-size", "container-name:", "container:",
    "container: card / size", "1cqw", "1e38cqh", "1cqi", "1cqb", "1cqmin", "-1e9cqmax", "(inline-size", "(block-size",
    "@scope", "@starting-style", "@keyframes k{", "from{", "to{", "50%{",
    "@font-face{", "font-family:", "src:", "unicode-range:U+0-10FFFF", "U+??????", "@namespace", "@page", "@charset \"x\";",
    "@property --a{syntax:'<length>';inherits:false;initial-value:0px}", "@counter-style", "&", "& &", ">", "+", "~", "||",
    "*", ".c", "#i", "div", "p", "span", "a", "td", "li", "[title]", "[a=b i]", "[a^='", "[a|=b]", ":is(", ":not(", ":where(",
    ":has(", ":has(> ", ":nth-child(", ":nth-child(2n+1 of ", ":nth-last-of-type(-n+3)", ":nth-child(99999999999n-99999999999)",
    ":hover", ":focus", ":root", ":empty", ":first-child", ":checked", ":lang(en)", ":dir(rtl)", ":host(", "::before",
    "::after", "::marker", "::first-line", "::first-letter", "::placeholder", "::selection", "::slotted(", ":scope",
    "<!--", "-->", "\\0", "\\10FFFF ", "\\d800 ", "\xC3", "\xEF\xBF\xBD", "\0", "e", "-", "+", ".", "%", "/", "<", "=",
};
#define NDICT ((int)(sizeof dict / sizeof dict[0]))

static const char *const snippets[] = {
    "div{width:calc(100% - (2 * var(--g, 8px)));--g:4px}p{margin:max(1em, min(5vw, 40px)) auto}",
    ":root{--a:var(--b);--b:var(--c);--c:var(--a)}div{color:var(--a, red);width:var(--a)}",
    "@media (min-width: 100px) and (max-width: 2000px){@supports (display:flex){.c{display:flex;flex:1 1 0}}}",
    "div:is(.c, :not(#i, :where(p, :has(> span + a)))) > span:nth-child(2n+1 of .c){float:left}",
    "table{border-collapse:collapse;border-spacing:1e9px}td{width:1e9px;padding:50%;border:9999px solid}",
    "li::marker{content:counter(a) '.';font-size:1e9px}ol{counter-reset:a 2147483647}li{counter-increment:a 2147483647}",
    ".c{position:absolute;inset:-1e9px;transform:rotate(45deg) scale(1e9);box-shadow:0 0 1e9px 1e9px red}",
    "span{font:italic bold 1e9px/0 Ahem;letter-spacing:-1e9px;word-spacing:1e9px;text-indent:-1e9px}",
    "div{display:flex;flex-wrap:wrap;gap:1e9px}div>*{flex:1e9 1e9 -1px;order:-2147483648;min-width:0;max-width:0}",
    "@layer a{div{color:red}}@layer b,a;@layer{p{color:blue}}div{& span{color:green;& a{color:red}}}",
    "img{aspect-ratio:1e38/1e-38;width:auto;height:1e9px;object-fit:cover}input{width:1e9px;height:-5px}",
    ("div{background:linear-gradient(1e38deg, red -1e30px, blue 1e30px), radial-gradient(circle 0px at 1e9px 1e9px, red, blue);"
     "background-size:0 0, 1e9px 1e-9px;border-radius:1e9px / 0}"),
    "p{columns:1e9;line-height:1e9;tab-size:1e9;white-space:pre-wrap;overflow:scroll;height:0;width:0}",
    "@keyframes k{from{width:0}50%{width:calc(1px/0)}to{width:1e38px}}div{animation:k 0s infinite}",
    "@font-face{font-family:x;src:url(f.woff2) format('woff2');unicode-range:U+0-7F,U+1e3??}p{font-family:x}",
    "div{margin:-1e9px;padding:1e9px;border:1e9px solid;outline:1e9px solid;outline-offset:-1e9px;float:right;clear:both}",
    ("div{container:card / inline-size;width:1e9cqw}@container card (min-width: 10px) and (not (height > 1e38px)){"
     "p{width:50cqi;font-size:1e9cqmin}@container (orientation: landscape){span::before{content:'x';width:1cqb}}}"),
    "p{container-type:size;height:-1cqh}@container (aspect-ratio > 0/0) or style(--x) or (width >= 1e400px){*{width:1cqmax}}",
};

static const char body[] =
    "<body><div class=c id=i title=t><p>one <span class=c>two <a href=x>three</a></span> four "
    "<b>five</b><br>six</p><ul><li>a<li class=c>b</ul><ol start=3><li>c</ol>"
    "<table><caption>cap<tr><td>1<td class=c colspan=2>2<tr><th rowspan=2>3<td>4</table>"
    "<img width=20 height=10 alt=pic><input value=v><textarea>t</textarea><select><option>o</select><button>b</button>"
    "<div class=c><span>f1</span><span>f2</span><p>f3</p></div>"
    "<svg width=20 height=20><rect width=10 height=10 class=c /><text y=10>s</text></svg>"
    "<pre>pre  text\n\tline</pre><q>quote</q><hr><details><summary>s</summary>d</details></div>";

typedef struct Req {
    uint32_t id;
    int depth;
    AuResourceKind kind;
} Req;

typedef struct Loader {
    Req q[32];
    int n;
    uint32_t next;
} Loader;

static uint32_t req_fn(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    Loader *l = (Loader *)ctx;
    (void)dv, (void)url;
    if (l->n == 32 || l->next >= 24)
        return 0;
    l->q[l->n].id = ++l->next;
    l->q[l->n].kind = kind;
    l->n++;
    return l->next;
}

static AuFontSet *fonts;
static AuSurface *surface;
static long slow_ms = 20000;

static void feed_resource(AuDocView *dv, uint32_t id, const uint8_t *d, size_t n)
{
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = 1 + fz_rnd(fz_rnd(3) ? 64 : 5000);
        if (chunk > n - pos)
            chunk = n - pos;
        au_docview_resource_data(dv, id, d + pos, chunk);
        pos += chunk;
    }
    au_docview_resource_done(dv, id, true);
}

static void run_api(const FzBuf *in)
{
    const char *s = (const char *)in->d;
    AuBuf out;
    AuCssSheet *sheet = au_css_sheet_parse(s, in->n, AU_CSS_ORIGIN_AUTHOR, "http://fuzz.example/a.css", fz_rnd(4) ? NULL : "screen");
    size_t i, n;
    au_buf_init(&out);
    au_css_sheet_rule_count(sheet);
    /* the text the sheet keeps is the input, packed or not */
    if (fz_rnd(2))
        au_css_sheet_pack_text(sheet);
    if (!au_css_sheet_text_is(sheet, s, in->n))
        abort();
    au_css_sheet_text_get(sheet, &out);
    if (out.len != in->n || (in->n && memcmp(out.data, s, in->n)))
        abort();
    au_buf_clear(&out);
    n = au_css_sheet_import_count(sheet);
    for (i = 0; i < n; i++)
        (void)au_css_sheet_import(sheet, i)->url;
    n = au_css_sheet_font_face_count(sheet);
    for (i = 0; i < n; i++) {
        static const char *const names[] = {"font-family", "src", "font-weight", "font-style", "font-stretch", "unicode-range",
                                            "font-display"};
        size_t k;
        for (k = 0; k < sizeof names / sizeof names[0]; k++) {
            const AuCssValue *v = au_css_font_face_descriptor(au_css_sheet_font_face(sheet, i), names[k]);
            if (v)
                au_css_value_serialize(v, &out);
        }
    }
    au_css_sheet_free(sheet);

    /* a piece as an inline style, a selector, a media query, a condition */
    {
        size_t from = in->n ? fz_rnd((uint32_t)in->n) : 0, len = fz_rnd(600);
        AuCssDeclBlock *b;
        AuCssSelectorList *l;
        if (len > in->n - from)
            len = in->n - from;
        b = au_css_parse_inline_style(s + from, len, NULL);
        if (b) {
            au_buf_clear(&out);
            au_css_decl_block_serialize(b, &out);
            /* what was serialized parses again */
            if (out.len) {
                AuCssDeclBlock *b2 = au_css_parse_inline_style(out.data, out.len, NULL);
                if (b2)
                    au_css_decl_block_free(b2);
            }
            au_css_decl_block_free(b);
        }
        l = au_css_selector_parse(s + from, len);
        if (l)
            au_css_selector_free(l);
        au_css_supports(s + from, len);
        {
            AuCssDevice dev;
            au_css_device_default(&dev);
            au_css_media_text_matches(s + from, len, &dev);
        }
    }
    au_buf_free(&out);
}

static void esc_attr(FzBuf *b, const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (d[i] == '"')
            fz_puts(b, "&quot;");
        else if (d[i] == '&')
            fz_puts(b, "&amp;");
        else if (d[i])
            fz_insert(b, b->n, d + i, 1);
    }
}

static void run_page(const FzBuf *in, const FzCorpus *corpus)
{
    Loader l;
    AuLoader loader;
    AuDocView *dv;
    FzBuf doc = {0};
    int k, round;
    memset(&l, 0, sizeof l);
    memset(&loader, 0, sizeof loader);
    loader.ctx = &l;
    loader.request = req_fn;

    fz_puts(&doc, fz_rnd(4) ? "<!DOCTYPE html>" : "");
    fz_puts(&doc, "<link rel=stylesheet href=a.css>");
    fz_puts(&doc, body);
    if (in->n) {
        size_t from = fz_rnd((uint32_t)in->n), len = fz_rnd(300);
        if (len > in->n - from)
            len = in->n - from;
        fz_puts(&doc, "<div class=c style=\"");
        esc_attr(&doc, in->d + from, len);
        fz_puts(&doc, "\">styled <span>inline</span></div>");
    }
    dv = au_docview_new(fonts, &loader, NULL);
    au_docview_load(dv, (const char *)doc.d, doc.n, "http://fuzz.example/dir/page.html");
    au_docview_set_viewport(dv, 300, 200);
    if (fz_rnd(2))
        au_docview_update(dv);
    /* Answer the sheet, then its imports (and theirs) with other inputs,
     * images and fonts with failures. */
    for (round = 0; round < 40 && l.n; round++) {
        Req r = l.q[0];
        memmove(l.q, l.q + 1, (size_t)(--l.n) * sizeof l.q[0]);
        if (r.kind != AU_RES_STYLESHEET) {
            au_docview_resource_done(dv, r.id, false);
        } else if (r.id == 1) {
            feed_resource(dv, r.id, in->d, in->n);
        } else {
            FzBuf t = {0};
            const FzBuf *o = &corpus->v[fz_rnd((uint32_t)corpus->n)];
            fz_set(&t, o->d, o->n);
            fz_mutate(&t, dict, NDICT, corpus, 16 << 10);
            feed_resource(dv, r.id, t.d, t.n);
            free(t.d);
        }
    }
    for (k = 0; k < 2; k++) {
        AuCanvas *c;
        AuHitResult hr;
        AuBuf d;
        AuIRect dmg[8];
        int cw, ch, m;
        /* the second round reads the sheets' texts packed: rules that
         * first match now and @media blocks that first apply now */
        if (k)
            while (au_docview_pack_sheets(dv))
                ;
        au_docview_set_viewport(dv, k ? 1 + (int)fz_rnd(900) : 300, k ? 1 + (int)fz_rnd(700) : 200);
        au_docview_update(dv);
        au_docview_content_size(dv, &cw, &ch);
        c = au_canvas_new(surface);
        au_docview_paint(dv, c, (AuRect){0, 0, 200, 150}, 0, 0);
        au_canvas_free(c);
        c = au_canvas_new(surface);
        au_docview_paint(dv, c, (AuRect){0, 0, 200, 150}, cw > 1 ? (int)fz_rnd((uint32_t)cw) : 0,
                         ch > 1 ? (int)fz_rnd((uint32_t)ch) : 0);
        au_canvas_free(c);
        for (m = 0; m < 4; m++) {
            AuEvent ev;
            memset(&ev, 0, sizeof ev);
            ev.type = AU_EVENT_MOUSE_MOVE;
            ev.x = (int)fz_rnd(300);
            ev.y = (int)fz_rnd(200);
            au_docview_mouse(dv, &ev);
            au_docview_hit_test(dv, ev.x, ev.y, &hr);
        }
        au_docview_scroll_damage(dv, 0, 0, 0, 40, dmg, 8);
        au_buf_init(&d);
        au_layout_dump(au_docview_layout(dv), &d);
        au_buf_free(&d);
    }
    au_docview_free(dv);
    free(doc.d);
}

static void run_one(const FzBuf *in, const FzCorpus *corpus)
{
    clock_t t0 = clock();
    long ms;
    run_api(in);
    run_page(in, corpus);
    ms = (long)((double)(clock() - t0) * 1000 / CLOCKS_PER_SEC);
    if (ms > slow_ms) {
        printf("fuzz_css: an input of %lu bytes took %ld ms\n", (unsigned long)in->n, ms);
        abort();
    }
}

static int fuzz_main(int argc, char **argv)
{
    long iters = 0, it;
    int first, i;
    FzCorpus corpus = {0};
    FzBuf in = {0};
    static uint8_t fontdata[16384];
    const char *e = getenv("AURA_FUZZ_SLOW_MS");
    int replay = fz_args(argc, argv, &iters, &first);

    if (e)
        slow_ms = atol(e);
    au_css_init();
    fonts = ahem_fontset(fontdata);
    surface = au_surface_new(200, 150);
    for (i = 0; i < (int)(sizeof snippets / sizeof snippets[0]); i++)
        fz_corpus_add_str(&corpus, snippets[i]);
    if (replay) {
        for (i = first; i < argc; i++) {
            if (!fz_read_file(argv[i], &in, 0))
                continue;
            fz_replay_seed(argv[i]);
            run_one(&in, &corpus);
            printf("%s: ok\n", argv[i]);
        }
        return 0;
    }
    for (i = first; i < argc; i++)
        fz_corpus_add_file(&corpus, argv[i], 32 << 10);
    for (it = fz_first; it < iters && !fz_expired(); it++) {
        fz_iteration(it);
        const FzBuf *src = &corpus.v[fz_rnd((uint32_t)corpus.n)];
        fz_set(&in, src->d, src->n);
        fz_mutate(&in, dict, NDICT, &corpus, 48 << 10);
        fz_save(in.d, in.n);
        run_one(&in, &corpus);
    }
    printf("fuzz_css: %ld iterations\n", it - fz_first);
    au_surface_free(surface);
    au_fontset_free(fonts);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
