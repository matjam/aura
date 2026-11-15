/* Runs html5lib tree-construction tests (*.dat files).
 *
 * Usage: test_tree [-v] [-x expected-failures.txt] FILE.dat...
 *
 * Each document test is parsed twice: the whole input in one call, and
 * fed one byte at a time. Both must give the expected tree. Tests without
 * #script-on or #script-off run with scripting off and on. Fragment tests
 * use au_html_parse_fragment.
 *
 * The expected-failures file lists "FILE.dat:N" (N is the 1-based test
 * number in the file), one per line, each with a reason after a '#'.
 * Listed tests still run and are reported; they do not fail the run. A
 * listed test that passes is reported so the list can be trimmed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html/html.h"
#include "js/js.h"

static int verbose;

/* ---- Tree dump ---- */

typedef struct {
    char *name; /* "xlink href" style */
    const char *val;
    size_t vlen;
} DumpAttr;

static int attr_cmp(const void *a, const void *b)
{
    return strcmp(((const DumpAttr *)a)->name, ((const DumpAttr *)b)->name);
}

static void indent(AuBuf *b, int depth)
{
    au_buf_puts(b, "| ");
    for (int i = 0; i < depth; i++)
        au_buf_puts(b, "  ");
}

static void dump_element(AuBuf *b, const AuElement *e, int depth)
{
    indent(b, depth);
    au_buf_putc(b, '<');
    if (e->ns == AU_NS_SVG)
        au_buf_puts(b, "svg ");
    else if (e->ns == AU_NS_MATHML)
        au_buf_puts(b, "math ");
    au_buf_puts(b, au_atom_str(e->tag));
    au_buf_puts(b, ">\n");
    DumpAttr *at = malloc((e->nattrs + 1) * sizeof *at);
    for (uint32_t i = 0; i < e->nattrs; i++) {
        const char *q = au_atom_str(e->attrs[i].name);
        AuBuf nb;
        au_buf_init(&nb);
        if (e->attrs[i].ns != AU_ATTR_NS_NONE) {
            const char *colon = strchr(q, ':');
            const char *prefix = e->attrs[i].ns == AU_ATTR_NS_XLINK ? "xlink"
                                 : e->attrs[i].ns == AU_ATTR_NS_XML ? "xml"
                                                                    : "xmlns";
            au_buf_puts(&nb, prefix);
            au_buf_putc(&nb, ' ');
            au_buf_puts(&nb, colon ? colon + 1 : q);
        } else {
            au_buf_puts(&nb, q);
        }
        at[i].name = au_buf_take(&nb, NULL);
        at[i].val = e->attrs[i].value;
        at[i].vlen = e->attrs[i].len;
    }
    qsort(at, e->nattrs, sizeof *at, attr_cmp);
    for (uint32_t i = 0; i < e->nattrs; i++) {
        indent(b, depth + 1);
        au_buf_puts(b, at[i].name);
        au_buf_puts(b, "=\"");
        au_buf_append(b, at[i].val, at[i].vlen);
        au_buf_puts(b, "\"\n");
        au_free(at[i].name);
    }
    free(at);
}

static void dump_node(AuBuf *b, const AuNode *n, int depth)
{
    switch (n->type) {
    case AU_NODE_ELEMENT:
        dump_element(b, (const AuElement *)n, depth);
        break;
    case AU_NODE_TEXT:
        indent(b, depth);
        au_buf_putc(b, '"');
        au_buf_append(b, ((const AuText *)n)->data, ((const AuText *)n)->len);
        au_buf_puts(b, "\"\n");
        break;
    case AU_NODE_COMMENT:
        indent(b, depth);
        au_buf_puts(b, "<!-- ");
        au_buf_append(b, ((const AuText *)n)->data, ((const AuText *)n)->len);
        au_buf_puts(b, " -->\n");
        break;
    case AU_NODE_PROCESSING_INSTRUCTION: {
        const AuProcessingInstruction *pi = (const AuProcessingInstruction *)n;
        indent(b, depth);
        au_buf_puts(b, "<?");
        au_buf_puts(b, au_atom_str(pi->target));
        au_buf_putc(b, ' ');
        au_buf_append(b, pi->text.data, pi->text.len);
        au_buf_puts(b, "?>\n");
        break;
    }
    case AU_NODE_DOCTYPE: {
        const AuDoctype *d = (const AuDoctype *)n;
        indent(b, depth);
        au_buf_puts(b, "<!DOCTYPE ");
        au_buf_puts(b, d->name);
        if (d->public_id[0] || d->system_id[0])
            au_buf_printf(b, " \"%s\" \"%s\"", d->public_id, d->system_id);
        au_buf_puts(b, ">\n");
        break;
    }
    default:
        break;
    }
}

/* Dumps the children of root, iteratively. Template contents appear under
 * a "content" line. */
typedef struct {
    const AuNode *n;
    int depth;
} Item;

static void dump_children(AuBuf *b, const AuNode *root)
{
    size_t cap = 64, top = 0;
    Item *st = malloc(cap * sizeof *st);
    /* Push children in reverse so they pop in order. */
#define PUSH_CHILDREN(parent, d)                                     \
    do {                                                             \
        for (const AuNode *c = (parent)->last_child; c; c = c->prev) { \
            if (top == cap) {                                        \
                cap *= 2;                                            \
                st = realloc(st, cap * sizeof *st);                  \
            }                                                        \
            st[top].n = c;                                           \
            st[top].depth = (d);                                     \
            top++;                                                   \
        }                                                            \
    } while (0)
    PUSH_CHILDREN(root, 0);
    while (top) {
        Item it = st[--top];
        if (it.n == NULL)
            continue;
        dump_node(b, it.n, it.depth);
        if (it.n->type == AU_NODE_ELEMENT) {
            const AuElement *e = (const AuElement *)it.n;
            if (e->ns == AU_NS_HTML && e->tag == AU_ATOM_template && e->template_content) {
                /* "content" then the content's children; then the
                 * element's own children (normally none). */
                PUSH_CHILDREN(it.n, it.depth + 1);
                indent(b, it.depth + 1);
                au_buf_puts(b, "content\n");
                PUSH_CHILDREN(e->template_content, it.depth + 2);
                continue;
            }
        }
        PUSH_CHILDREN(it.n, it.depth + 1);
    }
#undef PUSH_CHILDREN
    free(st);
}

/* ---- .dat reading ---- */

typedef struct {
    char *data;
    size_t dlen;
    char *fragment; /* context, or NULL */
    int script;     /* -1 off only, 1 on only, 0 both */
    char *document;
    int line;
} Test;

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

/* Splits the file into tests. Returns the count. */
static int parse_dat(char *src, size_t len, Test **out)
{
    int n = 0, cap = 0;
    Test *tests = NULL;
    char *p = src, *end = src + len;
    int line = 1;
    enum { NONE, DATA, ERRORS, FRAG, DOC } sect = NONE;
    AuBuf acc;
    au_buf_init(&acc);
    Test *cur = NULL;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *le = nl ? nl : end;
        size_t ll = (size_t)(le - p);
        int is_hdr = ll > 0 && p[0] == '#';
        if (is_hdr && ((ll == 5 && !memcmp(p, "#data", 5)) || (ll == 12 && !memcmp(p, "#description", 12))) &&
            (sect == NONE || sect == DOC)) {
            if (cur && sect == DOC) {
                /* Drop the blank separator line. */
                while (acc.len && acc.data[acc.len - 1] == '\n' && acc.len >= 2 && acc.data[acc.len - 2] == '\n')
                    acc.len--;
                cur->document = au_buf_take(&acc, NULL);
            }
            if (ll == 5) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 64;
                    tests = realloc(tests, (size_t)cap * sizeof *tests);
                }
                cur = &tests[n++];
                memset(cur, 0, sizeof *cur);
                cur->line = line;
                sect = DATA;
                acc.len = 0;
            } else {
                sect = NONE;
                cur = NULL;
            }
        } else if (is_hdr && sect != DOC && cur) {
            if (ll == 7 && !memcmp(p, "#errors", 7)) {
                if (sect == DATA) {
                    if (acc.len && acc.data[acc.len - 1] == '\n')
                        acc.len--;
                    cur->dlen = acc.len;
                    cur->data = au_buf_take(&acc, NULL);
                }
                sect = ERRORS;
            } else if (ll == 11 && !memcmp(p, "#new-errors", 11)) {
                sect = ERRORS;
            } else if (ll == 18 && !memcmp(p, "#document-fragment", 18)) {
                sect = FRAG;
            } else if (ll == 11 && !memcmp(p, "#script-off", 11)) {
                cur->script = -1;
            } else if (ll == 10 && !memcmp(p, "#script-on", 10)) {
                cur->script = 1;
            } else if (ll == 9 && !memcmp(p, "#document", 9)) {
                sect = DOC;
                acc.len = 0;
            } else if (sect == DATA) {
                au_buf_append(&acc, p, ll);
                au_buf_putc(&acc, '\n');
            }
        } else if (cur) {
            if (sect == DATA || sect == DOC) {
                au_buf_append(&acc, p, ll);
                au_buf_putc(&acc, '\n');
            } else if (sect == FRAG && ll) {
                cur->fragment = strndup(p, ll);
            }
        }
        p = nl ? nl + 1 : end;
        line++;
    }
    if (cur && sect == DOC) {
        while (acc.len >= 2 && acc.data[acc.len - 1] == '\n' && acc.data[acc.len - 2] == '\n')
            acc.len--;
        cur->document = au_buf_take(&acc, NULL);
    }
    au_buf_free(&acc);
    *out = tests;
    return n;
}

/* ---- Script execution ----
 * With scripting on, each script runs in the JavaScript engine (src/js)
 * when the parser reaches its end tag, with the parser as the insertion
 * point for document.write. The runtime starts at a document's first
 * script and is freed with the document. */

typedef struct TreeScript {
    AuDocument *doc;
    AuJs *js;
    double clock;
} TreeScript;

static double tree_now(void *ctx)
{
    TreeScript *t = ctx;
    return t->clock += 1;
}

static void tree_console(void *ctx, AuJsLogLevel level, const char *text, size_t len)
{
    (void)ctx;
    (void)level; /* many test scripts are not valid JavaScript: errors are expected */
    if (verbose)
        fprintf(stderr, "  script: %.*s\n", (int)len, text);
}

static void run_script(void *ctx, AuHtmlParser *p, AuElement *script)
{
    TreeScript *t = ctx;
    if (!t->js) {
        AuJsHooks h;
        memset(&h, 0, sizeof h);
        h.ctx = t;
        h.now_ms = tree_now;
        h.console = tree_console;
        t->js = au_js_new(t->doc, "about:blank", NULL, &h);
        if (!t->js)
            return;
    }
    au_js_set_parser(t->js, p);
    AuBuf src;
    au_buf_init(&src);
    for (AuNode *c = script->node.first_child; c; c = c->next)
        if (c->type == AU_NODE_TEXT)
            au_buf_append(&src, ((AuText *)c)->data, ((AuText *)c)->len);
    au_js_run_classic(t->js, script, src.data ? src.data : "", src.len, "about:blank", 1);
    au_buf_free(&src);
}

/* ---- Running ---- */

static char *run_document(const Test *t, bool scripting, bool bytewise)
{
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.scripting = scripting;
    o.encoding = AU_HTML_ENC_UTF8;
    TreeScript ts;
    memset(&ts, 0, sizeof ts);
    if (scripting) {
        o.script_ready = run_script;
        o.ctx = &ts;
    }
    AuHtmlParser *p = au_html_parser_new(&o);
    ts.doc = au_html_parser_document(p);
    if (bytewise) {
        for (size_t i = 0; i < t->dlen; i++)
            au_html_parser_feed(p, t->data + i, 1);
    } else {
        au_html_parser_feed(p, t->data, t->dlen);
    }
    au_html_parser_finish(p);
    if (ts.js)
        au_js_free(ts.js);
    AuBuf b;
    au_buf_init(&b);
    dump_children(&b, &au_html_parser_document(p)->node);
    au_html_parser_free(p);
    if (!b.data)
        au_buf_puts(&b, "");
    return au_buf_take(&b, NULL);
}

static char *run_fragment(const Test *t, bool scripting)
{
    AuDocument *doc = au_document_new();
    const char *ctx = t->fragment;
    AuNamespace ns = AU_NS_HTML;
    if (!strncmp(ctx, "svg ", 4)) {
        ns = AU_NS_SVG;
        ctx += 4;
    } else if (!strncmp(ctx, "math ", 5)) {
        ns = AU_NS_MATHML;
        ctx += 5;
    }
    AuNode *context = au_element_new(doc, au_atom_cstr(ctx), ns);
    au_node_append(&doc->node, context);
    au_node_unref(context);
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.scripting = scripting;
    AuNode *frag = au_html_parse_fragment((AuElement *)context, t->data, t->dlen, &o);
    AuBuf b;
    au_buf_init(&b);
    dump_children(&b, frag);
    au_node_unref(frag);
    au_node_unref(&doc->node);
    if (!b.data)
        au_buf_puts(&b, "");
    return au_buf_take(&b, NULL);
}

/* Returns NULL on a pass, or a description of the first mismatch. */
static char *check(const Test *t)
{
    const char *want = t->document ? t->document : "";
    for (int s = 0; s < 2; s++) {
        bool scripting = s == 1;
        if ((t->script < 0 && scripting) || (t->script > 0 && !scripting))
            continue;
        for (int bw = 0; bw < (t->fragment ? 1 : 2); bw++) {
            char *got = t->fragment ? run_fragment(t, scripting) : run_document(t, scripting, bw);
            if (strcmp(got, want)) {
                AuBuf r;
                au_buf_init(&r);
                au_buf_printf(&r, "[scripting %s%s]\n--- want\n%s--- got\n%s", scripting ? "on" : "off",
                              bw ? ", bytewise" : "", want, got);
                free(got);
                return au_buf_take(&r, NULL);
            }
            free(got);
        }
    }
    return NULL;
}

static char **xfail;
static int nxfail;

static void load_xfail(const char *path)
{
    size_t len;
    char *s = read_file(path, &len);
    if (!s) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *e = line + strlen(line);
        while (e > line && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        if (!*line)
            continue;
        xfail = realloc(xfail, (size_t)(nxfail + 1) * sizeof *xfail);
        xfail[nxfail++] = strdup(line);
    }
    free(s);
}

static bool is_xfail(const char *file, int num)
{
    char key[512];
    snprintf(key, sizeof key, "%s:%d", file, num);
    for (int i = 0; i < nxfail; i++)
        if (!strcmp(xfail[i], key))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    int tp = 0, tf = 0, tx = 0, unexpected_pass = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-v")) {
            verbose = 1;
            continue;
        }
        if (!strcmp(argv[a], "-x") && a + 1 < argc) {
            load_xfail(argv[++a]);
            continue;
        }
        size_t len;
        char *src = read_file(argv[a], &len);
        if (!src) {
            fprintf(stderr, "cannot read %s\n", argv[a]);
            return 1;
        }
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        Test *tests;
        int n = parse_dat(src, len, &tests);
        int pass = 0, fail = 0, xf = 0;
        for (int i = 0; i < n; i++) {
            char *why = check(&tests[i]);
            bool expected = is_xfail(base, i + 1);
            if (!why) {
                pass++;
                if (expected) {
                    printf("UNEXPECTED PASS %s:%d\n", base, i + 1);
                    unexpected_pass++;
                }
            } else {
                if (expected)
                    xf++;
                else
                    fail++;
                if (verbose || !expected)
                    printf("%s %s:%d (line %d)%s%s\n--- data\n%s\n%s\n", expected ? "XFAIL" : "FAIL", base, i + 1,
                           tests[i].line, tests[i].fragment ? " fragment " : "",
                           tests[i].fragment ? tests[i].fragment : "", tests[i].data ? tests[i].data : "", why);
                free(why);
            }
            free(tests[i].data);
            free(tests[i].fragment);
            free(tests[i].document);
        }
        free(tests);
        free(src);
        printf("%-36s %4d passed %4d failed %4d expected failures\n", base, pass, fail, xf);
        tp += pass;
        tf += fail;
        tx += xf;
    }
    printf("tree construction total: %d passed, %d failed, %d expected failures\n", tp, tf, tx);
    return (tf || unexpected_pass) ? 1 : 0;
}
