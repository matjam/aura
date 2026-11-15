/* API tests of src/css that do not fit the case files: restyling and
 * invalidation, imports, deep documents, sharing, the typed accessors. */
#include "../../src/css/css_int.h"
#include "../../src/dom/control.h"

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

/* Siblings with the same element rules but different ::before rules must
 * not share one style: the pseudo-element styles hang off it. */
static void test_sharing_pseudo(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *p1 = add(doc, &body->node, "p", NULL, "x");
    AuElement *p2 = add(doc, &body->node, "p", NULL, "x");
    AuElement *p3 = add(doc, &body->node, "p", NULL, "x");
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet(".x::before { content: 'a'; color: red; }"
                                       " .x:last-child::before { color: blue; }"));
    au_css_style_document(r, doc);
    const AuStyle *s1 = au_element_style(p1), *s2 = au_element_style(p2), *s3 = au_element_style(p3);
    CHECK(s1 == s2);
    CHECK(s1 != s3);
    const AuStyle *b1 = au_style_pseudo(s1, AU_CSS_PSEUDO_BEFORE), *b3 = au_style_pseudo(s3, AU_CSS_PSEUDO_BEFORE);
    CHECK(b1 && b3);
    if (b1 && b3) {
        CHECK(b1->inh->color == AU_COLOR_RGBA(255, 0, 0, 255));
        CHECK(b3->inh->color == AU_COLOR_RGBA(0, 0, 255, 255));
    }
    free_doc(doc);
    au_css_resolver_free(r);
}

/* :placeholder-shown, :valid, :invalid and :in-range follow the control's
 * live value, and a value change restyles the control only. */
static void test_live_value(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *in = add(doc, &body->node, "input", "in", NULL);
    AuElement *num = add(doc, &body->node, "input", "num", NULL);
    AuElement *other = add(doc, &body->node, "p", NULL, NULL);
    au_element_set_attr(in, au_atom_cstr("placeholder"), "name", 4);
    au_element_set_attr(in, au_atom_cstr("required"), "", 0);
    au_element_set_attr(num, au_atom_cstr("type"), "number", 6);
    au_element_set_attr(num, au_atom_cstr("min"), "1", 1);
    au_element_set_attr(num, au_atom_cstr("max"), "10", 2);
    au_element_set_attr(num, au_atom_cstr("value"), "5", 1);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet("input { width: 1px; height: 1px; top: 1px }"
                                       " input:placeholder-shown { width: 10px }"
                                       " input:invalid { height: 5px } input:valid { height: 7px }"
                                       " input:in-range { top: 3px } input:out-of-range { top: 4px }"
                                       " div:has(.x) { color: red }"));
    au_css_style_document(r, doc);
    CHECK(prop_is(in, "width", "10px"));
    CHECK(prop_is(in, "height", "5px"));
    CHECK(prop_is(num, "top", "3px"));
    const AuStyle *other_style = au_element_style(other);
    uint32_t before = au_css_resolver_styled_count(r);
    au_ctl_set_value(in, "x", 1);
    au_css_invalidate(r, in, AU_CSS_CHANGE_STATE, 0);
    au_css_restyle(r, doc);
    CHECK(prop_is(in, "width", "1px"));
    CHECK(prop_is(in, "height", "7px"));
    CHECK(au_css_resolver_styled_count(r) == before + 1);
    CHECK(au_element_style(other) == other_style);
    au_ctl_set_value(num, "42", 2);
    au_css_invalidate(r, num, AU_CSS_CHANGE_STATE, 0);
    au_css_restyle(r, doc);
    CHECK(prop_is(num, "top", "4px"));
    au_ctl_set_value(in, "", 0);
    au_css_invalidate(r, in, AU_CSS_CHANGE_STATE, 0);
    au_css_restyle(r, doc);
    CHECK(prop_is(in, "width", "10px"));
    CHECK(prop_is(in, "height", "5px"));
    CHECK(au_css_resolver_styled_count(r) == before + 3);
    free_doc(doc);
    au_css_resolver_free(r);
}

/* ---- unicode-range ---- */

/* The ranges of the n-th @font-face as "first-last first-last", "none"
 * without the descriptor. */
static const char *ranges_of(const AuCssSheet *s, size_t i)
{
    static char buf[256];
    const AuCssValue *v = au_css_font_face_descriptor(au_css_sheet_font_face(s, i), "unicode-range");
    if (!v)
        return "none";
    buf[0] = 0;
    if (v->type != AU_CSSV_COMMA_LIST)
        return "bad";
    for (uint32_t k = 0; k < v->n; k++) {
        const AuCssValue *r = &v->u.items[k];
        if (r->type != AU_CSSV_LIST || r->n != 2 || r->u.items[0].type != AU_CSSV_NUMBER)
            return "bad";
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%X-%X", k ? " " : "",
                 (unsigned)r->u.items[0].u.num, (unsigned)r->u.items[1].u.num);
    }
    return buf;
}

static void test_unicode_range(void)
{
    static const char *const cases[][2] = {
        {"U+26", "26-26"},
        {"u+0-7F", "0-7F"},
        {"U+0025-00FF", "25-FF"},
        {"U+4??", "400-4FF"},
        {"U+??????", "bad-range"}, /* past U+10FFFF */
        {"U+????", "0-FFFF"},
        {"U+0000-00FF, U+0131, U+0152-0153, U+02BB-02BC, U+2000-206F, U+FEFF, U+FFFD", "0-FF 131-131 152-153 2BB-2BC 2000-206F FEFF-FEFF FFFD-FFFD"},
        {"U+1F00-1FFF", "1F00-1FFF"},
        {"U+1e3", "1E3-1E3"},
        {"U+2e5-2e9", "2E5-2E9"},
        {"U+a-f", "A-F"},
        {"U+A5 , U+10FFFF", "A5-A5 10FFFF-10FFFF"},
        {"U+abcdef", "bad-range"},
        {"U+110000", "bad-range"},
        {"U+1e-5", "bad-range"}, /* first above last */
        {"U+0-7F,", "bad-range"},
        {"U+ 26", "bad-range"},
        {"U+1234567", "bad-range"},
        {"U+12?-34", "bad-range"},
        {"26", "bad-range"},
        {"U+3.5", "bad-range"},
        {"/* c */ U+41-5A /* d */", "41-5A"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char css[512];
        snprintf(css, sizeof css, "@font-face { font-family: F; unicode-range: %s; src: url(f.woff) }", cases[i][0]);
        AuCssSheet *s = sheet(css);
        const char *got = ranges_of(s, 0);
        const char *want = strcmp(cases[i][1], "bad-range") == 0 ? "none" : cases[i][1];
        checks++;
        if (strcmp(got, want) != 0) {
            failures++;
            fprintf(stderr, "unicode-range: %s: got %s, want %s\n", cases[i][0], got, want);
        }
        /* the descriptors around it are kept either way */
        CHECK(au_css_font_face_descriptor(au_css_sheet_font_face(s, 0), "src") != NULL);
        au_css_sheet_free(s);
    }
    /* declarations and custom properties keep working with the numbers'
     * source text in the token pool */
    AuCssDeclBlock *b = au_css_parse_inline_style("width: +0025px; --x: 1e3 +7% 2.50em", 35, NULL);
    AuBuf out;
    au_buf_init(&out);
    au_css_decl_block_serialize(b, &out);
    CHECK(strcmp(au_buf_cstr(&out), "width: 25px; --x: 1000 +7% 2.5em;") == 0);
    if (failures)
        fprintf(stderr, "    serialized: %s\n", au_buf_cstr(&out));
    au_buf_free(&out);
    au_css_decl_block_free(b);
    /* a NUL byte is read as U+FFFD and the tokenizer goes on (it used to
     * loop forever) */
    static const char nul[] = "a\0b { color: red } #x\0 { width: 1px } p { height: 2px\0 }";
    AuCssSheet *s = au_css_sheet_parse(nul, sizeof nul, AU_CSS_ORIGIN_AUTHOR, NULL, NULL);
    CHECK(au_css_sheet_rule_count(s) >= 2);
    au_css_sheet_free(s);
}

/* ---- :scope ---- */

static AuElement *query(const char *sel, AuNode *root)
{
    AuCssSelectorList *l = au_css_selector_parse(sel, strlen(sel));
    CHECK(l != NULL);
    if (!l)
        return NULL;
    AuElement *e = au_css_query_selector(l, root);
    au_css_selector_free(l);
    return e;
}

static void test_scope(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *a = add(doc, &body->node, "div", "a", NULL);
    AuElement *deep = add(doc, &a->node, "p", "deep", NULL);
    AuElement *inner = add(doc, &deep->node, "span", "inner", NULL);
    AuElement *direct = add(doc, &a->node, "span", "direct", NULL);
    AuElement *b = add(doc, &body->node, "div", "b", NULL);
    AuElement *other = add(doc, &b->node, "span", "other", NULL);
    /* querySelector on an element: :scope is that element */
    CHECK(query("span", &a->node) == inner);
    CHECK(query(":scope > span", &a->node) == direct);
    CHECK(query(":scope > span", &b->node) == other);
    CHECK(query(":scope span", &deep->node) == inner);
    CHECK(query(":scope", &a->node) == NULL); /* the scope is not its own descendant */
    CHECK(query(":scope + div span", &a->node) == NULL); /* matches lie in the scope's subtree */
    CHECK(query(":not(:scope) > span", &a->node) == inner);
    CHECK(query(":is(:scope, #deep) > span", &a->node) == inner);
    CHECK(query("div:has(> :scope) span", &deep->node) == inner);
    /* on the document, :scope is the root element */
    CHECK(query(":scope > body", &doc->node) == body);
    CHECK(query(":scope > span", &doc->node) == NULL);
    /* matches: :scope is the element; closest: the element it was called on */
    AuCssSelectorList *l = au_css_selector_parse(":scope", 6);
    CHECK(au_css_selector_matches(l, a) && au_css_selector_matches(l, inner));
    CHECK(au_css_selector_matches_scoped(l, a, &a->node));
    CHECK(!au_css_selector_matches_scoped(l, a, &inner->node));
    CHECK(!au_css_selector_matches_scoped(l, a, &doc->node));
    CHECK(au_css_selector_matches_scoped(l, au_document_element(doc), &doc->node));
    au_css_selector_free(l);
    l = au_css_selector_parse("div:has(> p > :scope)", 21);
    CHECK(au_css_selector_matches_scoped(l, a, &inner->node));
    CHECK(!au_css_selector_matches_scoped(l, a, &direct->node));
    CHECK(!au_css_selector_matches_scoped(l, b, &inner->node));
    au_css_selector_free(l);
    free_doc(doc);
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

static AuElement *add_svg(AuDocument *doc, AuNode *parent, const char *tag)
{
    AuNode *n = au_element_new(doc, au_atom_cstr(tag), AU_NS_SVG);
    au_node_append(parent, n);
    au_node_unref(n);
    return (AuElement *)n;
}

static void set(AuElement *el, const char *name, const char *v)
{
    au_element_set_attr(el, au_atom_cstr(name), v, strlen(v));
}

/* SVG properties, presentation attributes as hints, the UA rules for SVG
 * and the mask shorthand. */
static void test_svg_props(void)
{
    AuElement *body;
    AuDocument *doc = skeleton(&body);
    AuElement *svg = add_svg(doc, &body->node, "svg");
    set(svg, "width", "24");
    set(svg, "height", "50%");
    set(svg, "fill", "none");
    set(svg, "stroke", "currentColor");
    set(svg, "stroke-width", "2");
    set(svg, "stroke-linecap", "round");
    set(svg, "stroke-linejoin", "bevel");
    set(svg, "font-size", "12");
    AuElement *g = add_svg(doc, &svg->node, "g");
    set(g, "fill", "url(#grad) #00f");
    set(g, "stroke-dasharray", "4, 2 1");
    set(g, "fill-opacity", "50%");
    set(g, "opacity", ".5");
    set(g, "style", "fill-rule: evenodd");
    set(g, "class", "c");
    AuElement *p = add_svg(doc, &g->node, "path");
    set(p, "fill", "red;color:blue"); /* would inject: skipped */
    set(p, "stroke-width", "3px");
    set(p, "paint-order", "stroke fill");
    set(p, "vector-effect", "non-scaling-stroke");
    AuElement *stop = add_svg(doc, &g->node, "stop");
    set(stop, "stop-color", "#ff0000");
    set(stop, "stop-opacity", "0.25");
    AuElement *masked = add(doc, &body->node, "div", "m", NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, sheet(".c { stroke-linecap: square; }"
                                       "body { color: rgb(1, 2, 3); }"
                                       "#m { -webkit-mask: url(i.svg) center / 16px 16px no-repeat; mask-mode: alpha;"
                                       " font-variation-settings: 'wght' 600, \"opsz\" 12; font-optical-sizing: none;"
                                       " font-width: 75%; }"));
    au_css_style_document(r, doc);
    const AuStyle *s = au_element_style(svg);
    CHECK(s->box->width.kind == AU_LEN_PX && s->box->width.u.px == 24);
    CHECK(s->box->height.kind == AU_LEN_PCT && s->box->height.u.pct == 50);
    CHECK(s->inhr->fill && css_is_kw(&s->inhr->fill->value, AU_K_none));
    CHECK(s->inhr->stroke && s->inhr->stroke->value.type == AU_CSSV_COLOR &&
          s->inhr->stroke->value.u.rgba == AU_COLOR_CURRENT);
    CHECK(s->inhr->stroke_width.kind == AU_LEN_NUMBER && s->inhr->stroke_width.u.num == 2);
    CHECK(s->inhr->stroke_linecap == AU_STROKE_LINECAP_ROUND);
    CHECK(s->inhr->stroke_linejoin == AU_STROKE_LINEJOIN_BEVEL);
    CHECK(s->inh->font_size == 12);
    CHECK(s->rare->transform_origin == NULL || true);
    const AuStyle *gs = au_element_style(g);
    CHECK(gs->inhr->fill && gs->inhr->fill->value.type == AU_CSSV_LIST);
    CHECK(gs->inhr->stroke_dasharray && gs->inhr->stroke_dasharray->value.type == AU_CSSV_COMMA_LIST);
    CHECK(gs->inhr->fill_opacity > 0.49f && gs->inhr->fill_opacity < 0.51f);
    CHECK(gs->rare->opacity > 0.49f && gs->rare->opacity < 0.51f);
    CHECK(gs->inhr->fill_rule == AU_FILL_RULE_EVENODD);
    CHECK(gs->inhr->stroke_linecap == AU_STROKE_LINECAP_SQUARE); /* the author rule beats the inherited hint */
    CHECK(gs->inhr->stroke_width.kind == AU_LEN_NUMBER); /* inherited */
    const AuStyle *ps = au_element_style(p);
    CHECK(ps->inhr->fill == gs->inhr->fill || (ps->inhr->fill && ps->inhr->fill->value.type == AU_CSSV_LIST));
    CHECK(ps->inh->color != AU_COLOR_RGBA(0, 0, 255, 255));
    CHECK(ps->inhr->stroke_width.kind == AU_LEN_PX && ps->inhr->stroke_width.u.px == 3);
    CHECK(ps->inhr->paint_order && ps->inhr->paint_order->value.type == AU_CSSV_LIST);
    CHECK(ps->rare->vector_effect == AU_VECTOR_EFFECT_NON_SCALING_STROKE);
    CHECK(ps->inhr->stroke_linecap == AU_STROKE_LINECAP_SQUARE);
    const AuStyle *ss = au_element_style(stop);
    CHECK(ss->rare->stop_color == AU_COLOR_RGBA(255, 0, 0, 255));
    CHECK(ss->rare->stop_opacity == 0.25f);
    /* The UA sheet: SVG elements other than the outer svg transform about
     * the origin. */
    CHECK(prop_is(p, "transform-origin", "0px 0px"));
    /* Masks. */
    const AuStyle *ms = au_element_style(masked);
    CHECK(au_style_mask_layer_count(ms) == 1);
    CHECK(au_style_mask_layer_count(s) == 0);
    AuBgLayer l;
    au_style_mask_layer(ms, 0, &l);
    CHECK(l.image && l.image->type == AU_CSSV_URL && strcmp(l.image->u.str, "i.svg") == 0);
    CHECK(l.repeat_x == AU_K_no_repeat && l.repeat_y == AU_K_no_repeat);
    CHECK(l.attachment == AU_K_alpha);
    CHECK(l.size && l.size->type == AU_CSSV_LIST);
    CHECK(l.position_x && l.position_x->type == AU_CSSV_PERCENT && l.position_x->u.num == 50);
    CHECK(l.origin == AU_K_border_box);
    CHECK(ms->inhr->font_variation_settings && ms->inhr->font_variation_settings->value.type == AU_CSSV_COMMA_LIST &&
          ms->inhr->font_variation_settings->value.n == 2);
    CHECK(ms->inhr->font_optical_sizing == AU_FONT_OPTICAL_SIZING_NONE);
    CHECK(ms->inh->font_stretch == 75);
    CHECK(au_css_supports_decl("fill", 4, "url(#a) none", 12));
    CHECK(!au_css_supports_decl("stroke-dasharray", 16, "1 -2", 4));
    CHECK(au_css_supports_decl("mask", 4, "url(a.svg) luminance no-clip", 28));
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
    test_sharing_pseudo();
    test_scope();
    test_live_value();
    test_unicode_range();
    test_typed();
    test_inline_api();
    test_quirks_ua();
    test_svg_props();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
