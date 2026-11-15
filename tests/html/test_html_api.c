/* Tests of the HTML parser API: serialization, fragments, encodings,
 * incremental feeding, script pause and document.write, and linear-time
 * behaviour on inputs that are quadratic in naive implementations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "html/html.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

static char *serialize(const AuNode *n)
{
    AuBuf b;
    au_buf_init(&b);
    au_html_serialize(n, &b, false);
    if (!b.data)
        au_buf_puts(&b, "");
    return au_buf_take(&b, NULL);
}

static void expect_body(const char *html, const char *want)
{
    AuDocument *d = au_html_parse(html, strlen(html), NULL);
    char *got = serialize(&au_document_body(d)->node);
    CHECK(!strcmp(got, want), "%s\n  want %s\n  got  %s", html, want, got);
    au_free(got);
    au_node_unref(&d->node);
}

static void test_serialize(void)
{
    expect_body("<p>a &amp; b &lt; c &gt; d&nbsp;e", "<p>a &amp; b &lt; c &gt; d&nbsp;e</p>");
    expect_body("<a title='x\"y&amp;<>'>t</a>", "<a title=\"x&quot;y&amp;&lt;&gt;\">t</a>");
    expect_body("<br><img src=a><hr>", "<br><img src=\"a\"><hr>");
    expect_body("<p>x</p><script>a<b&&c</script><style>x>y</style>",
                "<p>x</p><script>a<b&&c</script><style>x>y</style>");
    expect_body("<svg viewbox='0 0 1 1'><foreignobject/></svg>",
                "<svg viewBox=\"0 0 1 1\"><foreignObject></foreignObject></svg>");
    expect_body("<math><mi xlink:href=a></mi></math>", "<math><mi xlink:href=\"a\"></mi></math>");
    expect_body("<p>x</p><template><p>in</p></template>x", "<p>x</p><template><p>in</p></template>x");
    expect_body("<body><!--c--><?pi data?>", "<!--c--><?pi data?>");
    expect_body("<pre>\n\nx</pre>", "<pre>\nx</pre>");

    AuDocument *d = au_html_parse("<!DOCTYPE html><title>t</title>", 31, NULL);
    char *s = serialize(&d->node);
    CHECK(!strcmp(s, "<!DOCTYPE html><html><head><title>t</title></head><body></body></html>"), "doc: %s", s);
    au_free(s);
    CHECK(d->quirks == AU_QUIRKS_NONE, "no-quirks expected");
    au_node_unref(&d->node);

    d = au_html_parse("<p>x", 4, NULL);
    CHECK(d->quirks == AU_QUIRKS_FULL, "quirks expected without doctype");
    au_node_unref(&d->node);
}

static void test_fragment(void)
{
    AuDocument *d = au_html_parse("<body><table></table>", 21, NULL);
    AuElement *table = (AuElement *)au_document_body(d)->node.first_child;
    AuNode *f = au_html_parse_fragment(table, "<tr><td>1</td></tr>x", 20, NULL);
    char *s = serialize(f);
    /* No table is on the stack, so "x" is foster parented into the root. */
    CHECK(!strcmp(s, "<tbody><tr><td>1</td></tr></tbody>x"), "table fragment: %s", s);
    CHECK(f->first_child->doc == d, "fragment nodes belong to the context's document");
    au_free(s);
    au_node_unref(f);

    AuElement *body = au_document_body(d);
    f = au_html_parse_fragment(body, "<b>1<p>2</b>3", 13, NULL);
    s = serialize(f);
    CHECK(!strcmp(s, "<b>1</b><p><b>2</b>3</p>"), "body fragment: %s", s);
    au_free(s);
    au_node_unref(f);
    au_node_unref(&d->node);
}

static char *parse_bytes_body_text(const char *bytes, size_t len, AuHtmlEncoding enc)
{
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.encoding = enc;
    AuDocument *d = au_html_parse(bytes, len, &o);
    char *s = serialize(&au_document_body(d)->node);
    au_node_unref(&d->node);
    return s;
}

static void test_encodings(void)
{
    char *s = parse_bytes_body_text("\x93q\x94 \xE9", 5, AU_HTML_ENC_WINDOWS_1252);
    CHECK(!strcmp(s, "\xE2\x80\x9Cq\xE2\x80\x9D \xC3\xA9"), "windows-1252: %s", s);
    au_free(s);
    s = parse_bytes_body_text("\xEF\xBB\xBFok", 5, AU_HTML_ENC_WINDOWS_1252);
    CHECK(!strcmp(s, "ok"), "UTF-8 BOM overrides: %s", s);
    au_free(s);
    s = parse_bytes_body_text("\xFF\xFE<\0p\0>\0\xAC\x20", 10, AU_HTML_ENC_SNIFF);
    CHECK(!strcmp(s, "<p>\xE2\x82\xAC</p>"), "UTF-16LE: %s", s);
    au_free(s);
    s = parse_bytes_body_text("a\xC3\x28" "b\xF0\x9F\x98", 7, AU_HTML_ENC_UTF8);
    CHECK(!strcmp(s, "a\xEF\xBF\xBD(b\xEF\xBF\xBD"), "malformed UTF-8: %s", s);
    au_free(s);

    /* CR LF split across chunks, and a UTF-8 character split too. */
    AuHtmlParser *p = au_html_parser_new(NULL);
    au_html_parser_feed(p, "<pre>a\r", 7);
    au_html_parser_feed(p, "\nb\xE2\x82", 4);
    au_html_parser_feed(p, "\xAC\r", 2);
    au_html_parser_finish(p);
    s = serialize(&au_document_body(au_html_parser_document(p))->node);
    CHECK(!strcmp(s, "<pre>a\nb\xE2\x82\xAC\n</pre>"), "chunked newlines: %s", s);
    au_free(s);
    au_html_parser_free(p);
}

/* ---- Scripts ---- */

typedef struct {
    int calls;
    bool pause;
    AuElement *last;
} ScriptCtx;

static void on_script(void *ctx, AuHtmlParser *p, AuElement *script)
{
    ScriptCtx *c = ctx;
    c->calls++;
    if (c->pause) {
        au_html_parser_pause(p);
        c->last = script;
        au_node_ref(&script->node);
    } else {
        au_html_parser_write(p, "<i>w</i>", 8);
    }
}

static void test_scripts(void)
{
    ScriptCtx c = {0, false, NULL};
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.scripting = true;
    o.script_ready = on_script;
    o.ctx = &c;
    AuDocument *d = au_html_parse("a<script>x</script>b", 20, &o);
    char *s = serialize(&au_document_body(d)->node);
    CHECK(!strcmp(s, "a<script>x</script><i>w</i>b"), "inline write: %s", s);
    CHECK(c.calls == 1, "one script call");
    au_free(s);
    au_node_unref(&d->node);

    /* Pause at the script, keep feeding, then run it and resume. */
    c.calls = 0;
    c.pause = true;
    AuHtmlParser *p = au_html_parser_new(&o);
    au_html_parser_feed(p, "a<script src=x></script>b", 25);
    CHECK(au_html_parser_paused(p), "paused at script");
    au_html_parser_feed(p, "c", 1);
    au_html_parser_finish(p);
    CHECK(!au_html_parser_done(p), "not done while paused");
    s = serialize(&au_document_body(au_html_parser_document(p))->node);
    CHECK(!strcmp(s, "a<script src=\"x\"></script>"), "nothing after the script yet: %s", s);
    au_free(s);
    au_html_parser_begin_script(p);
    au_html_parser_write(p, "<u>1</u>", 8);
    au_html_parser_end_script(p);
    au_html_parser_resume(p);
    CHECK(au_html_parser_done(p), "done after resume");
    s = serialize(&au_document_body(au_html_parser_document(p))->node);
    CHECK(!strcmp(s, "a<script src=\"x\"></script><u>1</u>bc"), "after resume: %s", s);
    au_free(s);
    au_node_unref(&c.last->node);
    au_html_parser_free(p);
}

/* ---- Linear time ---- */

static char *repeat(const char *prefix, const char *unit, int n, const char *suffix)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, prefix);
    for (int i = 0; i < n; i++) {
        if (strstr(unit, "%d"))
            au_buf_printf(&b, unit, i);
        else
            au_buf_puts(&b, unit);
    }
    au_buf_puts(&b, suffix);
    return au_buf_take(&b, NULL);
}

static double time_parse(const char *prefix, const char *unit, int n, const char *suffix)
{
    char *src = repeat(prefix, unit, n, suffix);
    clock_t t0 = clock();
    AuDocument *d = au_html_parse(src, strlen(src), NULL);
    double t = (double)(clock() - t0) / CLOCKS_PER_SEC;
    au_node_unref(&d->node);
    au_free(src);
    return t;
}

static void scaling(const char *name, const char *prefix, const char *unit, const char *suffix)
{
    int n = 10000;
    double a = time_parse(prefix, unit, n, suffix);
    double b = time_parse(prefix, unit, n * 8, suffix);
    double ratio = b / (a > 1e-4 ? a : 1e-4);
    printf("  %-28s %7.3fs for %d, %7.3fs for %d (x%.1f)\n", name, a, n, b, n * 8, ratio);
    /* Linear is about x8; quadratic would be about x64. */
    CHECK(b < 0.2 || ratio < 24, "%s does not scale linearly", name);
}

static void test_scaling(void)
{
    printf("scaling:\n");
    scaling("nested div", "", "<div>", "x");
    scaling("nested b", "", "<b>", "x");
    scaling("distinct nested font", "", "<font size=%d>", "x");
    char *spans = repeat("", "<span>", 10000, "");
    scaling("unknown end tags in spans", spans, "</foo>", "");
    au_free(spans);
    char *divs = repeat("", "<div>", 10000, "");
    scaling("div then li", divs, "<li>x</li>", "");
    au_free(divs);
    scaling("misnested b p", "", "<b><p>x</b>", "");
    scaling("nested a", "", "<a>x", "");
    scaling("nested tables", "", "<table><tr><td>", "");
    scaling("foster text", "<table>", "x<tr></tr>", "</table>");
    scaling("entities", "<p>", "&amp;&lt;x", "");
    scaling("many attributes", "<div ", "a%d=1 ", ">");
    scaling("nested svg g", "<svg>", "<g>", "");
    scaling("text in table cells", "<table>", "<tr><td>a</td></tr>", "");
}

/* Tags with many attributes use a hash set of names that is filled when a
 * ninth name is looked up. A tag with exactly eight attributes after a
 * longer one once looped forever clearing a set it had never filled. */
static void test_many_attributes(void)
{
    static const char *const docs[] = {
        /* nine, then exactly eight */
        "<p a=1 b=2 c=3 d=4 e=5 f=6 g=7 h=8 i=9></p><p a=1 b=2 c=3 d=4 e=5 f=6 g=7 h=8></p>",
        /* a duplicate as the ninth name, twice, then a long tag */
        "<p a=1 b=2 c=3 d=4 e=5 f=6 g=7 h=8 a=9 b=10></p>"
        "<p a=1 b=2 c=3 d=4 e=5 f=6 g=7 h=8 i=9 j=10 k=11></p>",
        /* a long tag cut off at the end of input, then nothing */
        "<p a=1 b=2 c=3 d=4 e=5 f=6 g=7 h=8 i=9 j=10",
        /* enough names to grow the set, then eight, then nine again */
        "<p a b c d e f g h i j k l m n o p q r s t u v w x y z aa ab ac ad ae af ag ah ai aj>"
        "<p a b c d e f g h><p a b c d e f g h i>",
    };
    static const unsigned counts[][3] = { { 9, 8, 0 }, { 8, 11, 0 }, { 0, 0, 0 }, { 36, 8, 9 } };
    AuAtom p = au_atom_cstr("p");
    for (size_t d = 0; d < sizeof docs / sizeof docs[0]; d++) {
        AuDocument *doc = au_html_parse(docs[d], strlen(docs[d]), NULL);
        unsigned seen = 0;
        for (AuNode *n = &doc->node; n; n = au_node_next(n, &doc->node)) {
            if (!au_is_html_element(n, p))
                continue;
            if (seen < 3)
                CHECK(au_as_element(n)->nattrs == counts[d][seen], "doc %u p %u: %u attributes", (unsigned)d,
                      seen, (unsigned)au_as_element(n)->nattrs);
            seen++;
        }
        unsigned want = (counts[d][0] != 0) + (counts[d][1] != 0) + (counts[d][2] != 0);
        CHECK(seen == want, "doc %u: %u p elements, expected %u", (unsigned)d, seen, want);
        au_node_unref(&doc->node);
    }
}

int main(void)
{
    test_many_attributes();
    test_serialize();
    test_fragment();
    test_encodings();
    test_scripts();
    test_scaling();
    if (failures)
        printf("%d failures\n", failures);
    else
        printf("html api: all passed\n");
    return failures ? 1 : 0;
}
