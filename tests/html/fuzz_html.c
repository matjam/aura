/* Deterministic fuzzer for the HTML tokenizer and tree builder. Each
 * iteration takes a corpus entry (the sample pages, the #data sections of
 * the tree-construction cases, and the fragments below), mutates it with
 * markup tokens, and runs it four ways:
 *   - au_html_parse on the whole input, with a random encoding and
 *     scripting flag, then the serializer;
 *   - the incremental parser fed at random boundaries (the UTF-8 result
 *     must serialize as the whole parse did);
 *   - the incremental parser with a script callback that writes mutated
 *     text at the insertion point (document.write), nested writes, pauses
 *     and resumes;
 *   - the fragment parser with a random context element.
 * It checks termination and that the two plain parses agree. Under
 * ASan/UBSan it finds memory errors. The tree builder does not cap depth
 * (layout does), so set AURA_FUZZ_STACK_KB to find recursion over it.
 *
 * usage: fuzz_html ITERATIONS SEED SECONDS [CORPUS...]
 *        fuzz_html replay FILE... */
#include "../fuzz.h"
#include "html/html.h"

static const char *const dict[] = {
    "<", ">", "</", "/>", "=", "\"", "'", "&", ";", "<!--", "-->", "--!>", "<!", "<?", "<![CDATA[", "]]>", "<!DOCTYPE html>",
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">", "<html>", "<head>", "<body>", "</body>", "</html>",
    "<p>", "</p>", "<div>", "</div>", "<span>", "<b>", "</b>", "<i>", "</i>", "<a href=x>", "</a>", "<nobr>", "<font>",
    "<table>", "</table>", "<tr>", "<td>", "<th>", "</td>", "</tr>", "<tbody>", "<caption>", "<colgroup>", "<col>",
    "<select>", "</select>", "<option>", "<optgroup>", "<template>", "</template>", "<script>", "</script>", "<style>",
    "</style>", "<title>", "</title>", "<textarea>", "</textarea>", "<plaintext>", "<noscript>", "</noscript>", "<xmp>",
    "<iframe>", "<noembed>", "<svg>", "</svg>", "<math>", "</math>", "<foreignObject>", "<desc>", "<mi>", "<mtext>",
    "<annotation-xml encoding=text/html>", "<annotation-xml>", "<form>", "</form>", "<button>", "<input>", "<li>", "<dd>",
    "<dt>", "<h1>", "</h1>", "<h2>", "<ul>", "<ol>", "<pre>\n", "<listing>", "<frameset>", "<frame>", "</frameset>",
    "<br>", "</br>", "<hr>", "<img>", "<image>", "<applet>", "<marquee>", "<object>", "<menu>", "<details>", "<summary>",
    "<ruby>", "<rt>", "<rp>", "<rb>", "<rtc>", "<body bgcolor=red>", "<html lang=en>", "<meta charset=utf-16>",
    "<meta http-equiv=content-type content='text/html; charset=windows-1252'>", "<base href=/>", "&amp;", "&lt", "&#0;",
    "&#x10FFFF;", "&#xD800;", "&#1114112;", "&notin;", "&notit;", "&AMP", "&#", "&#x", "&nosuch;", "<a b c=d e='f' g=\"h\">",
    "<x-custom>", "<a:b>", " xlink:href=#a", " xmlns=http://www.w3.org/2000/svg", " definitionurl=x", "\0", "\r", "\r\n",
    "\f", "\xEF\xBB\xBF", "\xFF\xFE", "\xFE\xFF", "\xC3", "\xED\xA0\x80", "\xF4\x90\x80\x80", "<!-->", "<!--->", "<!---",
    "<script><!--<script></script>--></script>", "<!doctype", " PUBLIC ", " SYSTEM ", "</p>", "</table></b></i>",
    "<svg><script/>", "<a><table><a>", "<b><p></b>", "<isindex>", "<keygen>", "<dialog>", "<search>", "<hr><select><hr>"};
#define NDICT ((int)(sizeof dict / sizeof dict[0]))

static const char *const fragments[] = {
    "<!DOCTYPE html><b><i><u><s><p>x</b></i></u></s>y",
    "<a><b><big><em><strong><div>x</a>y</div>",
    "<table><tr><td>a<table><caption>c<tr><td>b</table>d</table>",
    "<table>x<b>y<tr>z<td>w</b>",
    "<select><option>a<optgroup><option>b</select><select><table>",
    "<svg><g><foreignObject><p><svg><desc><b>x</svg></p></foreignObject></g><path d=M0,0></svg>",
    "<math><mi><b>x</mi><annotation-xml encoding='application/xhtml+xml'><div>y</div></annotation-xml></math>",
    "<template><tr><td>x</td></tr><template><col></template></template>",
    "<frameset><frame><noframes>x</noframes></frameset>",
    "<p><button><p><button>x</button></p></button>",
    "<ul><li>a<li>b<ul><li>c</ul><dd>d<dt>e</ul>",
    "<script>document.write('<b>')</script><script type=module src=x></script>z",
    "<title>a&amp;b</title><textarea>\n<x></textarea><plaintext></plaintext>",
    "<!-- c --><!DOCTYPE html SYSTEM 'about:legacy-compat'><html a=b><head c=d><body e=f><body g=h><html i=j>",
    "<form><form><input><table><form><input type=hidden><input></table>",
    "<h1><h2><h3></h1></h3><nobr><nobr><nobr>",
    "<a href=1><a href=2><a href=3><p><a href=4></p></a>",
    "<body><pre>\n\nx</pre><listing>\ny</listing><textarea>\n\nz</textarea>",
    "<ruby>a<rb>b<rt>c<rtc>d<rp>e</ruby>",
    "<table><colgroup><col><template></template></colgroup><tbody><tr><th>x<caption>",
};

static uint32_t hash_bytes(const char *d, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint8_t)d[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t check_doc(AuDocument *d, const char *what)
{
    AuBuf b;
    uint32_t h;
    (void)what;
    au_buf_init(&b);
    au_html_serialize(&d->node, &b, false);
    h = hash_bytes(b.data ? b.data : "", b.len);
    au_buf_free(&b);
    return h;
}

/* ---- document.write ---- */
typedef struct WriteCtx {
    const FzCorpus *corpus;
    int writes;  /* writes left for this document */
    int nesting; /* script callbacks under way */
    AuHtmlParser *paused;
} WriteCtx;

static void script_ready(void *ctx, AuHtmlParser *p, AuElement *script)
{
    WriteCtx *w = (WriteCtx *)ctx;
    (void)script;
    if (w->nesting > 8)
        return;
    w->nesting++;
    while (w->writes > 0 && fz_rnd(3)) {
        const FzBuf *o = &w->corpus->v[fz_rnd((uint32_t)w->corpus->n)];
        size_t from = o->n ? fz_rnd((uint32_t)o->n) : 0, len = fz_rnd(200);
        FzBuf t;
        memset(&t, 0, sizeof t);
        if (len > o->n - from)
            len = o->n - from;
        fz_set(&t, o->d + from, len);
        if (fz_rnd(2))
            fz_mutate(&t, dict, NDICT, w->corpus, 4096);
        w->writes--;
        au_html_parser_write(p, (const char *)t.d, t.n);
        free(t.d);
    }
    if (!w->paused && fz_rnd(5) == 0) {
        au_html_parser_pause(p);
        w->paused = p;
    }
    w->nesting--;
}

static void run_scripted(const FzBuf *in, const FzCorpus *corpus)
{
    WriteCtx w;
    AuHtmlOptions o;
    AuHtmlParser *p;
    size_t pos = 0;
    memset(&w, 0, sizeof w);
    memset(&o, 0, sizeof o);
    w.corpus = corpus;
    w.writes = 12;
    o.scripting = true;
    o.encoding = AU_HTML_ENC_UTF8;
    o.script_ready = script_ready;
    o.ctx = &w;
    p = au_html_parser_new(&o);
    while (pos < in->n) {
        size_t chunk = 1 + fz_rnd(fz_rnd(3) ? 16 : 2000);
        if (chunk > in->n - pos)
            chunk = in->n - pos;
        au_html_parser_feed(p, (const char *)in->d + pos, chunk);
        pos += chunk;
        if (w.paused && fz_rnd(2)) {
            /* the blocking script arrives and runs */
            w.paused = NULL;
            if (fz_rnd(2)) {
                au_html_parser_begin_script(p);
                if (w.writes > 0) {
                    w.writes--;
                    au_html_parser_write(p, "<p>w</p><script>x</script>", 26);
                }
                au_html_parser_end_script(p);
            }
            au_html_parser_resume(p);
        }
    }
    au_html_parser_finish(p);
    while (w.paused) {
        w.paused = NULL;
        au_html_parser_resume(p);
    }
    if (!au_html_parser_done(p)) {
        printf("fuzz_html: parser not done after finish\n");
        abort();
    }
    check_doc(au_html_parser_document(p), "scripted");
    au_html_parser_free(p);
}

static void run_fragment(const FzBuf *in)
{
    static const char *const contexts[] = {"div", "table", "tr", "td", "select", "template", "title", "textarea", "script",
                                           "style", "svg", "math", "html", "head", "body", "colgroup", "caption", "plaintext",
                                           "noscript", "frameset", "p", "tbody", "option", "foreignObject", "annotation-xml"};
    const char *name = contexts[fz_rnd(sizeof contexts / sizeof contexts[0])];
    char doc[128];
    AuDocument *d;
    AuElement *ctx = NULL;
    AuNode *n, *frag;
    AuHtmlOptions o;
    AuBuf b;
    snprintf(doc, sizeof doc, "<!DOCTYPE html><%s%s><%s id=c>", !strcmp(name, "foreignObject") ? "svg><" : "",
             !strcmp(name, "foreignObject") ? "g" : "div", name);
    d = au_html_parse(doc, strlen(doc), NULL);
    /* The last element in tree order is the context, whatever the tree
     * builder made of the name; else the body. */
    for (n = &d->node; n;) {
        if (n->type == AU_NODE_ELEMENT)
            ctx = (AuElement *)n;
        if (n->first_child)
            n = n->first_child;
        else {
            while (n && !n->next)
                n = n->parent;
            if (n)
                n = n->next;
        }
    }
    memset(&o, 0, sizeof o);
    o.scripting = fz_rnd(2) != 0;
    frag = au_html_parse_fragment(ctx, (const char *)in->d, in->n, &o);
    if (frag) {
        au_buf_init(&b);
        au_html_serialize(frag, &b, o.scripting);
        au_buf_free(&b);
        au_node_unref(frag);
    }
    au_node_unref(&d->node);
}

static void run_one(const FzBuf *in, const FzCorpus *corpus)
{
    AuHtmlOptions o;
    AuDocument *d;
    AuHtmlParser *p;
    uint32_t whole, pieces;
    size_t pos = 0;

    /* whole, in a random encoding */
    memset(&o, 0, sizeof o);
    o.scripting = fz_rnd(2) != 0;
    o.encoding = (AuHtmlEncoding)fz_rnd(5);
    d = au_html_parse((const char *)in->d, in->n, &o);
    whole = check_doc(d, "whole");
    au_node_unref(&d->node);

    /* in pieces: the same tree */
    p = au_html_parser_new(&o);
    while (pos < in->n) {
        size_t chunk = 1 + fz_rnd(fz_rnd(4) ? 8 : 3000);
        if (chunk > in->n - pos)
            chunk = in->n - pos;
        au_html_parser_feed(p, (const char *)in->d + pos, chunk);
        pos += chunk;
    }
    au_html_parser_finish(p);
    pieces = check_doc(au_html_parser_document(p), "pieces");
    au_html_parser_free(p);
    if (whole != pieces) {
        printf("fuzz_html: the tree depends on how the input was split (encoding %d)\n", (int)o.encoding);
        abort();
    }

    run_scripted(in, corpus);
    run_fragment(in);
}

/* Adds each #data section of a tree-construction file. */
static void add_dat(FzCorpus *c, const FzBuf *f)
{
    const char *s = (const char *)f->d, *end = s + f->n;
    while (s < end) {
        const char *a = strstr(s, "#data\n"), *b;
        if (!a)
            break;
        a += 6;
        b = strstr(a, "\n#errors");
        if (!b)
            break;
        fz_corpus_add(c, a, (size_t)(b - a));
        s = b + 1;
    }
}

static int fuzz_main(int argc, char **argv)
{
    long iters = 0, it;
    int first, i;
    FzCorpus corpus = {0};
    FzBuf in = {0};

    if (fz_args(argc, argv, &iters, &first)) {
        for (i = first; i < argc; i++) {
            if (!fz_read_file(argv[i], &in, 0))
                continue;
            fz_corpus_add(&corpus, in.d, in.n);
            run_one(&in, &corpus);
            printf("%s: ok\n", argv[i]);
        }
        return 0;
    }
    for (i = 0; i < (int)(sizeof fragments / sizeof fragments[0]); i++)
        fz_corpus_add_str(&corpus, fragments[i]);
    for (i = first; i < argc; i++) {
        size_t n = strlen(argv[i]);
        FzBuf f;
        if (n > 4 && !strcmp(argv[i] + n - 4, ".dat")) {
            if (fz_read_file(argv[i], &f, 1 << 20)) {
                add_dat(&corpus, &f);
                free(f.d);
            }
        } else
            fz_corpus_add_file(&corpus, argv[i], 64 << 10);
    }
    for (it = 0; it < iters && !fz_expired(); it++) {
        const FzBuf *src = &corpus.v[fz_rnd((uint32_t)corpus.n)];
        if (fz_rnd(16) == 0) {
            /* random bytes */
            size_t n = fz_rnd(300), k;
            fz_reserve(&in, n);
            for (k = 0; k < n; k++)
                in.d[k] = (uint8_t)fz_next();
            in.n = n;
            in.d[n] = 0;
        } else
            fz_set(&in, src->d, src->n);
        fz_mutate(&in, dict, NDICT, &corpus, 96 << 10);
        fz_save(in.d, in.n);
        run_one(&in, &corpus);
    }
    printf("fuzz_html: %ld iterations\n", it);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
