/* API tests of src/css that do not fit the case files: restyling and
 * invalidation, imports, deep documents, sharing, the typed accessors. */
#include "../../src/css/css_int.h"

#include <stdio.h>

static int failures, checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

static AuElement *add(AuDocument *doc, AuNode *parent, const char *tag, const char *id, const char *cls)
{
    AuNode *n = au_element_new(doc, au_atom_cstr(tag), AU_NS_HTML);
    AuElement *el = (AuElement *)n;
    if (id)
        au_element_set_attr(el, AU_ATOM_id, id, strlen(id));
    if (cls)
        au_element_set_attr(el, AU_ATOM_class, cls, strlen(cls));
    au_node_append(parent, n);
    au_node_unref(n);
    return el;
}

static AuDocument *skeleton(AuElement **body)
{
    AuDocument *doc = au_document_new();
    AuElement *html = add(doc, &doc->node, "html", NULL, NULL);
    add(doc, &html->node, "head", NULL, NULL);
    *body = add(doc, &html->node, "body", NULL, NULL);
    return doc;
}

static AuCssSheet *sheet(const char *css)
{
    return au_css_sheet_parse(css, strlen(css), AU_CSS_ORIGIN_AUTHOR, "http://x/", NULL);
}

static bool prop_is(const AuElement *el, const char *prop, const char *want)
{
    AuBuf b;
    au_buf_init(&b);
    au_style_serialize_property(au_element_style(el), au_atom_cstr(prop), &b);
    bool ok = strcmp(au_buf_cstr(&b), want) == 0;
    if (!ok)
        fprintf(stderr, "    %s: want %s, got %s\n", prop, want, au_buf_cstr(&b));
    au_buf_free(&b);
    return ok;
}

static void free_doc(AuDocument *doc)
{
    au_css_clear_document(doc);
    au_node_unref(&doc->node);
}

/* ---- Restyling ---- */

static void test_invalidation(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *a = add(doc, &body->node, "div", "a", NULL);
    AuElement *b = add(doc, &body->node, "div", "b", NULL);
    AuElement *c = add(doc, &a->node, "span", "c", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet(".on { color: red; } .on + div { color: blue; } .on > span { width: 5px; }"
                                       " div:hover { width: 7px; } body:hover { height: 3px; }"
                                       " [data-x=y] { margin-top: 2px; }"));
    au_css_style_document(r, doc);
    CHECK(prop_is(a, "color", "rgb(0, 0, 0)"));
    CHECK(prop_is(b, "color", "rgb(0, 0, 0)"));

    au_element_set_attr(a, AU_ATOM_class, "on", 2);
    au_css_invalidate(r, a, AU_CSS_CHANGE_ATTR, AU_ATOM_class);
    au_css_restyle(r, doc);
    CHECK(prop_is(a, "color", "rgb(255, 0, 0)"));
    CHECK(prop_is(b, "color", "rgb(0, 0, 255)"));
    CHECK(prop_is(c, "width", "5px"));
    CHECK(prop_is(c, "color", "rgb(255, 0, 0)"));

    /* hover propagates to ancestors */
    au_element_set_state(b, AU_STATE_HOVER, true);
    au_css_invalidate(r, b, AU_CSS_CHANGE_STATE, 0);
    au_css_invalidate(r, body, AU_CSS_CHANGE_STATE, 0);
    au_css_restyle(r, doc);
    CHECK(prop_is(b, "width", "7px"));
    CHECK(prop_is(body, "height", "3px"));
    CHECK(prop_is(a, "width", "auto"));

    au_element_set_state(b, AU_STATE_HOVER, false);
    au_css_invalidate(r, b, AU_CSS_CHANGE_STATE, 0);
    au_css_invalidate(r, body, AU_CSS_CHANGE_STATE, 0);
    au_css_restyle(r, doc);
    CHECK(prop_is(b, "width", "auto"));
    CHECK(prop_is(body, "height", "auto"));

    /* inline style */
    au_element_set_attr(b, AU_ATOM_style, "color: green", 12);
    au_css_invalidate(r, b, AU_CSS_CHANGE_INLINE_STYLE, AU_ATOM_style);
    au_css_restyle(r, doc);
    CHECK(prop_is(b, "color", "rgb(0, 128, 0)"));
    au_element_set_attr(b, AU_ATOM_style, "color: purple", 13);
    au_css_invalidate(r, b, AU_CSS_CHANGE_INLINE_STYLE, AU_ATOM_style);
    au_css_restyle(r, doc);
    CHECK(prop_is(b, "color", "rgb(128, 0, 128)"));

    /* attribute selectors */
    au_element_set_attr(c, au_atom_cstr("data-x"), "y", 1);
    au_css_invalidate(r, c, AU_CSS_CHANGE_ATTR, au_atom_cstr("data-x"));
    au_css_restyle(r, doc);
    CHECK(prop_is(c, "margin-top", "2px"));

    /* a new sheet restyles everything */
    au_css_resolver_add_sheet(r, sheet("span { color: orange; }"));
    au_css_restyle(r, doc);
    CHECK(prop_is(c, "color", "rgb(255, 165, 0)"));

    free_doc(doc);
    au_css_resolver_free(r);
}

static void test_has_invalidation(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *a = add(doc, &body->node, "div", "a", NULL);
    AuElement *b = add(doc, &a->node, "p", "b", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet("div:has(.x) { color: red; }"));
    au_css_style_document(r, doc);
    CHECK(prop_is(a, "color", "rgb(0, 0, 0)"));
    au_element_set_attr(b, AU_ATOM_class, "x", 1);
    au_css_invalidate(r, b, AU_CSS_CHANGE_ATTR, AU_ATOM_class);
    au_css_restyle(r, doc);
    CHECK(prop_is(a, "color", "rgb(255, 0, 0)"));
    free_doc(doc);
    au_css_resolver_free(r);
}

static void test_device_and_metrics(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *a = add(doc, &body->node, "div", "a", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet("@media (max-width: 500px) { #a { color: red; } } #a { width: 10vw; "
                                       "height: 2ex; margin-left: 3ch; }"));
    au_css_style_document(r, doc);
    CHECK(prop_is(a, "color", "rgb(0, 0, 0)"));
    CHECK(prop_is(a, "width", "102.4px"));
    CHECK(prop_is(a, "height", "16px"));
    CHECK(prop_is(a, "margin-left", "24px"));
    AuCssDevice d = *au_css_resolver_device(r);
    d.width = 400;
    au_css_resolver_set_device(r, &d);
    au_css_restyle(r, doc);
    CHECK(prop_is(a, "color", "rgb(255, 0, 0)"));
    CHECK(prop_is(a, "width", "40px"));
    CHECK(au_css_media_matches(r, "(max-width: 400px)", 18));
    free_doc(doc);
    au_css_resolver_free(r);
}

static void metrics(void *ctx, const AuStyle *s, float *ex, float *ch)
{
    (*(int *)ctx)++;
    *ex = s->inh->font_size * 0.25f;
    *ch = s->inh->font_size * 0.75f;
}

static void test_metrics_callback(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *a = add(doc, &body->node, "div", "a", NULL);
    AuCssResolver *r = au_css_resolver_new();
    int calls = 0;
    au_css_resolver_set_metrics(r, metrics, &calls);
    au_css_resolver_add_sheet(r, sheet("#a { font-size: 20px; height: 2ex; width: 2ch; }"));
    au_css_style_document(r, doc);
    CHECK(prop_is(a, "height", "10px"));
    CHECK(prop_is(a, "width", "30px"));
    CHECK(calls >= 1);
    free_doc(doc);
    au_css_resolver_free(r);
}

/* ---- Imports ---- */

static void test_imports(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *a = add(doc, &body->node, "div", "a", NULL);
    AuCssResolver *r = au_css_resolver_new();
    AuCssSheet *s = sheet("@import url(lib.css) layer(lib); @import url(print.css) print; #a { width: 3px; }");
    CHECK(au_css_sheet_import_count(s) == 2);
    const AuCssImport *im = au_css_sheet_import(s, 0);
    CHECK(strcmp(im->url, "lib.css") == 0);
    au_css_sheet_set_import(s, 0, sheet("#a { color: red; width: 9px; } div { height: 4px; }"));
    au_css_sheet_set_import(s, 1, sheet("#a { margin-top: 8px; }"));
    au_css_resolver_add_sheet(r, s);
    au_css_style_document(r, doc);
    CHECK(prop_is(a, "color", "rgb(255, 0, 0)"));
    CHECK(prop_is(a, "width", "3px")); /* layered import loses to unlayered */
    CHECK(prop_is(a, "height", "4px"));
    CHECK(prop_is(a, "margin-top", "0px")); /* print only */
    au_css_resolver_remove_sheet(r, s);
    au_css_restyle(r, doc);
    CHECK(prop_is(a, "color", "rgb(0, 0, 0)"));
    free_doc(doc);
    au_css_resolver_free(r);
}

/* ---- Deep documents ---- */

static void test_deep(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuNode *cur = &body->node;
    const int depth = 100000;
    AuElement *last = NULL;
    for (int i = 0; i < depth; i++) {
        last = add(doc, cur, i % 2 ? "span" : "div", NULL, i % 3 ? "c" : NULL);
        cur = &last->node;
    }
    AuCssResolver *r = au_css_resolver_new();
    /* (a descendant combinator whose left side fails walks every ancestor,
     * so such rules cost O(depth) per element; these do not) */
    au_css_resolver_add_sheet(r, sheet("div div span { color: red; } .c > .c { width: 1px; } "
                                       ":is(div, span) > span { margin-top: 1px; }"));
    au_css_style_document(r, doc);
    CHECK(prop_is(last, "color", "rgb(255, 0, 0)"));
    CHECK(prop_is(last, "margin-top", "1px"));
    AuCssSelectorList *sl = au_css_selector_parse("span:last-child", 15);
    CHECK(au_css_query_selector(sl, &doc->node) != NULL);
    au_css_selector_free(sl);
    free_doc(doc);
    au_css_resolver_free(r);
}

/* ---- Sharing ---- */

static void test_sharing(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *p1 = add(doc, &body->node, "p", NULL, "x");
    AuElement *p2 = add(doc, &body->node, "p", NULL, "x");
    AuElement *p3 = add(doc, &body->node, "p", NULL, "y");
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet(".x { color: red; } .y { color: red; width: 5px; }"));
    au_css_style_document(r, doc);
    const AuStyle *s1 = au_element_style(p1), *s2 = au_element_style(p2), *s3 = au_element_style(p3);
    CHECK(s1 == s2);
    CHECK(s1 != s3);
    CHECK(s1->inh == s3->inh);     /* same inherited values */
    CHECK(s1->surr == s3->surr);   /* both the UA paragraph margins */
    CHECK(s1->box != s3->box);
    CHECK(s1->bg == css_initial_style.bg);
    free_doc(doc);
    au_css_resolver_free(r);
}

/* ---- Typed access ---- */

static void test_typed(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *a = add(doc, &body->node, "div", "a", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet(
        "#a { width: calc(50% + 10px); height: 20%; color: red; border-top: 2px solid currentcolor; "
        "background: url(a.png) no-repeat 10px 20px, url(b.png) repeat-x; background-size: cover; "
        "--x: 1px  2px; display: inline-flex; z-index: 4; }"
        "@keyframes k { from { opacity: 0 } to { opacity: 1 } }"));
    au_css_style_document(r, doc);
    const AuStyle *s = au_element_style(a);
    CHECK(s->box->width.kind == AU_LEN_CALC);
    CHECK(au_len_resolve(&s->box->width, 200) > 109.9f && au_len_resolve(&s->box->width, 200) < 110.1f);
    CHECK(s->box->height.kind == AU_LEN_PCT && s->box->height.u.pct == 20);
    CHECK(s->inh->color == AU_COLOR_RGBA(255, 0, 0, 255));
    CHECK(s->surr->border_color[0] == AU_COLOR_CURRENT);
    CHECK(au_style_color(s, s->surr->border_color[0]) == AU_COLOR_RGBA(255, 0, 0, 255));
    CHECK(s->surr->border_width[0] == 2 && s->surr->border_width[1] == 0);
    CHECK(au_display_outer(s->box->display) == AU_DISPLAY_OUTER_INLINE);
    CHECK(au_display_inner(s->box->display) == AU_DISPLAY_INNER_FLEX);
    CHECK(s->box->z_index == 4);
    CHECK(au_style_bg_layer_count(s) == 2);
    AuBgLayer l;
    au_style_bg_layer(s, 1, &l);
    CHECK(l.image && l.image->type == AU_CSSV_URL && strcmp(l.image->u.str, "b.png") == 0);
    CHECK(l.repeat_x == AU_K_repeat && l.repeat_y == AU_K_no_repeat);
    CHECK(l.size && css_is_kw(l.size, AU_K_cover)); /* the shorter list repeats */
    au_style_bg_layer(s, 0, &l);
    CHECK(l.position_x && l.position_x->type == AU_CSSV_DIMENSION && l.position_x->u.num == 10);
    size_t len;
    const char *x = au_style_custom_property(s, au_atom_cstr("--x"), &len);
    CHECK(x && strcmp(x, "1px 2px") == 0);
    const AuCssKeyframes *k = au_css_resolver_keyframes(r, au_atom_cstr("k"));
    CHECK(k && k->nframes == 2);
    CHECK(au_css_property_id(au_atom_cstr("word-wrap")) == AU_PROP_overflow_wrap);
    CHECK(au_css_property_id(au_atom_cstr("nonsense")) == AU_PROP_NONE_);
    CHECK(au_css_keyword_of(au_atom_cstr("inline-block")) == AU_K_inline_block);
    CHECK(strcmp(au_css_keyword_name(AU_K_auto_), "auto") == 0);

    /* au_css_compute without the document pass */
    AuStyle *own = au_css_compute(r, a, au_element_style(body));
    CHECK(own->box->z_index == 4);
    au_style_release(own);
    free_doc(doc);
    au_css_resolver_free(r);
}

static void test_inline_api(void)
{
    AuCssDeclBlock *b = au_css_parse_inline_style("color: red; margin: 1px 2px !important; --v: x", 46, NULL);
    CHECK(au_css_decl_block_count(b) == 6);
    AuBuf o;
    au_buf_init(&o);
    CHECK(au_css_decl_block_get(b, au_atom_cstr("margin"), &o) && strcmp(au_buf_cstr(&o), "1px 2px") == 0);
    au_buf_clear(&o);
    CHECK(au_css_decl_block_get(b, au_atom_cstr("--v"), &o) && strcmp(au_buf_cstr(&o), "x") == 0);
    au_buf_free(&o);
    au_css_decl_block_free(b);
}

static void test_quirks_ua(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    doc->quirks = AU_QUIRKS_FULL;
    AuElement *t = add(doc, &body->node, "table", "t", NULL);
    AuElement *f = add(doc, &body->node, "form", "f", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet("body { font-size: 30px; } #t { width: 100; }"));
    au_css_style_document(r, doc);
    CHECK(prop_is(t, "font-size", "16px"));
    CHECK(prop_is(t, "width", "100px"));
    CHECK(prop_is(f, "margin-bottom", "30px")); /* 1em */
    doc->quirks = AU_QUIRKS_NONE;
    au_css_style_document(r, doc);
    CHECK(prop_is(t, "font-size", "30px"));
    CHECK(prop_is(t, "width", "auto"));
    free_doc(doc);
    au_css_resolver_free(r);
}

/* Author background, border or box-shadow on a form control is flagged
 * (layout drops the native look); UA styles and border-radius are not. */
static void test_author_decor(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *plain = add(doc, &body->node, "input", "plain", NULL);
    AuElement *bg = add(doc, &body->node, "input", "bg", NULL);
    AuElement *bd = add(doc, &body->node, "button", "bd", NULL);
    AuElement *lg = add(doc, &body->node, "select", "lg", NULL);
    AuElement *rad = add(doc, &body->node, "textarea", "rad", NULL);
    AuElement *div = add(doc, &body->node, "div", "div", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet("#bg { background: #fff } #bd { border: none } #rad { border-radius: 4px }"
                                       " #lg { border-inline-start-color: red } div { background: red }"));
    au_css_style_document(r, doc);
    CHECK(!(au_element_style(plain)->flags & AU_STYLE_F_AUTHOR_DECOR));
    CHECK(au_element_style(bg)->flags & AU_STYLE_F_AUTHOR_DECOR);
    CHECK(au_element_style(bd)->flags & AU_STYLE_F_AUTHOR_DECOR);
    CHECK(au_element_style(lg)->flags & AU_STYLE_F_AUTHOR_DECOR);
    CHECK(!(au_element_style(rad)->flags & AU_STYLE_F_AUTHOR_DECOR));
    CHECK(!(au_element_style(div)->flags & AU_STYLE_F_AUTHOR_DECOR));
    free_doc(doc);
    au_css_resolver_free(r);
}

#define FS(el) (au_element_style(el)->inh->font_size)
#define NEAR(a, b) ((a) > (b) - 0.01f && (a) < (b) + 0.01f)

/* Default sizes, the monospace default, text zoom and the minimum. */
static void test_font_prefs(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *mono = add(doc, &body->node, "div", "mono", NULL);
    AuElement *monoin = add(doc, &mono->node, "span", NULL, NULL);
    AuElement *pre = add(doc, &body->node, "pre", NULL, NULL);
    AuElement *px = add(doc, &body->node, "div", "px", NULL);
    AuElement *code = add(doc, &px->node, "code", NULL, NULL);
    AuElement *codepx = add(doc, &px->node, "code", "codepx", NULL);
    AuElement *small = add(doc, &body->node, "div", "small", NULL);
    AuElement *em = add(doc, &body->node, "div", "em", NULL);
    AuElement *tiny = add(doc, &body->node, "div", "tiny", NULL);
    AuElement *tinyem = add(doc, &tiny->node, "div", "tinyem", NULL);
    AuElement *zero = add(doc, &body->node, "div", "zero", NULL);
    AuElement *ini = add(doc, &px->node, "div", "ini", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet("#mono { font-family: monospace } #px { font-size: 10px }"
                                       " #codepx { font-size: 10px } #small { font-size: small }"
                                       " #em { font-size: 2em } #tiny { font-size: 6px }"
                                       " #tinyem { font-size: 0.5em } #zero { font-size: 0 }"
                                       " #ini { font-size: initial }"));
    au_css_style_document(r, doc);
    CHECK(NEAR(FS(body), 16));
    CHECK(NEAR(FS(mono), 13) && NEAR(FS(monoin), 13) && NEAR(FS(pre), 13));
    /* A non-keyword size scales by 13/16 into monospace (as Blink),
     * unless the element sets its own size. */
    CHECK(NEAR(FS(code), 8.125f) && NEAR(FS(codepx), 10));
    CHECK(NEAR(FS(small), 13) && NEAR(FS(em), 32) && NEAR(FS(ini), 16));

    AuCssFontPrefs p;
    au_css_font_prefs_default(&p);
    p.default_size = 20;
    p.default_mono_size = 15;
    au_css_resolver_set_font_prefs(r, &p);
    au_css_style_document(r, doc);
    CHECK(NEAR(FS(body), 20) && NEAR(FS(mono), 15) && NEAR(FS(pre), 15));
    CHECK(NEAR(FS(small), 20 * 8.0f / 9) && NEAR(FS(em), 40) && NEAR(FS(px), 10));
    CHECK(NEAR(FS(code), 7.5f) && NEAR(FS(ini), 20));

    au_css_font_prefs_default(&p);
    p.text_zoom = 1.5f;
    au_css_resolver_set_font_prefs(r, &p);
    au_css_style_document(r, doc);
    CHECK(NEAR(FS(body), 24) && NEAR(FS(px), 15) && NEAR(FS(em), 48));
    CHECK(NEAR(FS(small), 19.5f) && NEAR(FS(pre), 19.5f));
    CHECK(NEAR(FS(tinyem), 4.5f)); /* 0.5em of the zoomed 9px: not zoomed again */

    au_css_font_prefs_default(&p);
    p.min_size = 9;
    au_css_resolver_set_font_prefs(r, &p);
    au_css_style_document(r, doc);
    CHECK(NEAR(FS(tiny), 9) && NEAR(au_element_style(tiny)->inh->font_size_spec, 6));
    /* 0.5em starts from the unclamped 6px: 3px, clamped to 9. */
    CHECK(NEAR(FS(tinyem), 9) && NEAR(au_element_style(tinyem)->inh->font_size_spec, 3));
    CHECK(FS(zero) == 0);
    CHECK(NEAR(FS(body), 16) && NEAR(FS(px), 10));

    /* Nonsense values fall back to the defaults. */
    p.default_size = -1;
    p.text_zoom = 0;
    au_css_resolver_set_font_prefs(r, &p);
    CHECK(au_css_resolver_font_prefs(r)->default_size == 16 && au_css_resolver_font_prefs(r)->text_zoom == 1);
    free_doc(doc);
    au_css_resolver_free(r);
}

int main(void)
{
    au_css_init();
    test_font_prefs();
    test_author_decor();
    test_invalidation();
    test_has_invalidation();
    test_device_and_metrics();
    test_metrics_callback();
    test_imports();
    test_deep();
    test_sharing();
    test_typed();
    test_inline_api();
    test_quirks_ua();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
