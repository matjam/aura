/* Data-driven tests of src/css. Each case file holds blocks:
 *
 *   === name
 *   --- options        quirks | width N | height N | dark | print | user
 *   --- css            an author style sheet (several css sections add several sheets)
 *   --- html           an indented element tree:
 *                        tag#id.class[attr=value][attr="v w"] "text"
 *                        svg:tag for SVG elements, "text" alone for a text node
 *   --- expect         SELECTOR -> property: computed value
 *                      SELECTOR::before -> ... for pseudo-elements ("(none)" if absent)
 *   --- decls          inline style text => serialized declaration block
 *   --- get            inline style text | property => declared value ("(unset)")
 *   --- media          media query list => true | false
 *   --- supports       condition => true | false, or decl property | value => true | false
 *   --- selectors      selector => serialization (a,b,c), or => invalid
 *   --- tokens         css text => serialized tokens
 *   --- rules          expected style rule count of the first sheet
 *   --- imports        url | media | layer | supports | supported, one line per @import
 *   --- fontfaces      font-family value | src value, one line per @font-face
 *   --- keyframes      name: offsets, one line per @keyframes
 *   --- matches        SELECTOR => ids of matching elements in document order
 *
 * Lines starting with "# " outside css and html sections are comments. */
#include "../../src/css/css_int.h"

#include <stdio.h>
#include <stdlib.h>

static int failures, checks;
static const char *cur_file, *cur_case;

static void fail(int line, const char *what, const char *want, const char *got)
{
    failures++;
    fprintf(stderr, "%s:%d: [%s] %s\n    want: %s\n    got:  %s\n", cur_file, line, cur_case, what, want, got);
}

static void check_str(int line, const char *what, const char *want, const char *got)
{
    checks++;
    if (getenv("CSS_TEST_VERBOSE"))
        fprintf(stderr, "%s:%d: [%s] %s => %s\n", cur_file, line, cur_case, what, got);
    if (strcmp(want, got) != 0)
        fail(line, what, want, got);
}

/* ---- Text helpers ---- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    return s;
}

/* Splits "a SEP b" at the last occurrence of sep. */
static bool split(char *s, const char *sep, char **a, char **b)
{
    char *p = NULL, *q = s;
    size_t sl = strlen(sep);
    while ((q = strstr(q, sep)) != NULL) {
        p = q;
        q += sl;
    }
    if (!p) {
        /* "a SEP" with an empty right side (the line was trimmed) */
        size_t n = strlen(s), tl = sl;
        while (tl && sep[tl - 1] == ' ')
            tl--;
        if (n >= tl && strncmp(s + n - tl, sep, tl) == 0) {
            s[n - tl] = 0;
            *a = trim(s);
            *b = s + n; /* "" */
            return true;
        }
        return false;
    }
    *p = 0;
    *a = trim(s);
    *b = trim(p + sl);
    return true;
}

/* Splits at the first occurrence. */
static bool split_first(char *s, const char *sep, char **a, char **b)
{
    char *p = strstr(s, sep);
    if (!p)
        return false;
    *p = 0;
    *a = trim(s);
    *b = trim(p + strlen(sep));
    return true;
}

/* ---- DOM builder ---- */

static AuDocument *build_dom(const char *text, bool quirks)
{
    AuDocument *doc = au_document_new();
    doc->quirks = quirks ? AU_QUIRKS_FULL : AU_QUIRKS_NONE;
    AuNode *stack[256];
    int depth_of[256];
    int sp = 0;
    stack[0] = &doc->node;
    depth_of[0] = -1;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        int indent = 0;
        const char *s = p;
        while (s < eol && *s == ' ') {
            indent++;
            s++;
        }
        if (s < eol && *s != '#') {
            while (sp > 0 && depth_of[sp] >= indent)
                sp--;
            AuNode *parent = stack[sp];
            if (*s == '"') {
                const char *e = eol;
                while (e > s + 1 && e[-1] != '"')
                    e--;
                AuNode *t = au_text_new(doc, s + 1, (size_t)(e - s - 2 > 0 ? e - s - 2 : 0));
                au_node_append(parent, t);
                au_node_unref(t);
            } else {
                AuNamespace ns = AU_NS_HTML;
                if (strncmp(s, "svg:", 4) == 0) {
                    ns = AU_NS_SVG;
                    s += 4;
                } else if (strncmp(s, "math:", 5) == 0) {
                    ns = AU_NS_MATHML;
                    s += 5;
                }
                const char *ts = s;
                while (s < eol && *s != '#' && *s != '.' && *s != '[' && *s != ' ')
                    s++;
                AuNode *n = au_element_new(doc, au_atom(ts, (size_t)(s - ts)), ns);
                AuElement *el = (AuElement *)n;
                char classes[512] = "";
                while (s < eol && *s != ' ') {
                    if (*s == '#' || *s == '.') {
                        char k = *s++;
                        const char *vs = s;
                        while (s < eol && *s != '#' && *s != '.' && *s != '[' && *s != ' ')
                            s++;
                        if (k == '#')
                            au_element_set_attr(el, AU_ATOM_id, vs, (size_t)(s - vs));
                        else {
                            if (classes[0])
                                strncat(classes, " ", sizeof classes - strlen(classes) - 1);
                            strncat(classes, vs, (size_t)(s - vs) < sizeof classes - strlen(classes) - 1
                                                     ? (size_t)(s - vs)
                                                     : sizeof classes - strlen(classes) - 1);
                        }
                    } else if (*s == '[') {
                        s++;
                        const char *ns_ = s;
                        while (s < eol && *s != '=' && *s != ']')
                            s++;
                        AuAtom an = au_atom(ns_, (size_t)(s - ns_));
                        const char *vs = "", *ve = vs;
                        if (*s == '=') {
                            s++;
                            if (*s == '"') {
                                vs = ++s;
                                while (s < eol && *s != '"')
                                    s++;
                                ve = s;
                                if (*s == '"')
                                    s++;
                            } else {
                                vs = s;
                                while (s < eol && *s != ']')
                                    s++;
                                ve = s;
                            }
                        }
                        if (*s == ']')
                            s++;
                        au_element_set_attr(el, an, vs, (size_t)(ve - vs));
                    } else {
                        s++;
                    }
                }
                if (classes[0])
                    au_element_set_attr(el, AU_ATOM_class, classes, strlen(classes));
                au_node_append(parent, n);
                au_node_unref(n);
                while (s < eol && *s == ' ')
                    s++;
                if (s < eol && *s == '"') {
                    const char *e = eol;
                    while (e > s + 1 && e[-1] != '"')
                        e--;
                    AuNode *t = au_text_new(doc, s + 1, (size_t)(e - s - 2 > 0 ? e - s - 2 : 0));
                    au_node_append(n, t);
                    au_node_unref(t);
                }
                if (sp < 255) {
                    stack[++sp] = n;
                    depth_of[sp] = indent;
                }
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    return doc;
}

/* ---- Case state ---- */

typedef struct {
    char *kind;
    char *arg;
    char *body; /* lines joined with \n */
    int line;   /* line of the first body line */
} Section;

typedef struct {
    char *name;
    Section *s;
    int n, cap;
} Case;

typedef struct {
    bool quirks, dark, print, user;
    float width, height;
} Options;

static void parse_options(const char *body, Options *o)
{
    memset(o, 0, sizeof *o);
    o->width = 1024;
    o->height = 768;
    char *dup = au_strndup(body, strlen(body));
    for (char *line = strtok(dup, "\n"); line; line = strtok(NULL, "\n")) {
        char *t = trim(line);
        if (strcmp(t, "quirks") == 0)
            o->quirks = true;
        else if (strcmp(t, "dark") == 0)
            o->dark = true;
        else if (strcmp(t, "print") == 0)
            o->print = true;
        else if (strcmp(t, "user") == 0)
            o->user = true;
        else if (strncmp(t, "width ", 6) == 0)
            o->width = (float)atof(t + 6);
        else if (strncmp(t, "height ", 7) == 0)
            o->height = (float)atof(t + 7);
    }
    au_free(dup);
}

static AuCssResolver *make_resolver(const Case *c, const Options *o, AuCssSheet **first)
{
    AuCssResolver *r = au_css_resolver_new();
    AuCssDevice d;
    au_css_device_default(&d);
    d.width = o->width;
    d.height = o->height;
    d.dark = o->dark;
    d.print = o->print;
    au_css_resolver_set_device(r, &d);
    *first = NULL;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->s[i].kind, "css") != 0)
            continue;
        AuCssOrigin origin = c->s[i].arg && strcmp(c->s[i].arg, "user") == 0 ? AU_CSS_ORIGIN_USER
                                                                           : AU_CSS_ORIGIN_AUTHOR;
        AuCssSheet *s = au_css_sheet_parse(c->s[i].body, strlen(c->s[i].body), origin, "http://example.com/a/",
                                           NULL);
        if (!*first)
            *first = s;
        au_css_resolver_add_sheet(r, s);
    }
    return r;
}

static const char *default_html = "html\n  head\n  body\n";

/* The non-empty, non-comment lines of a section body. */
typedef struct {
    char *buf;
    char **v;
    int *ln;
    int n;
} Lines;

static void lines_init(Lines *L, const Section *sec)
{
    L->buf = au_strndup(sec->body, strlen(sec->body));
    int cap = 1;
    for (const char *q = L->buf; *q; q++)
        cap += *q == '\n';
    L->v = au_alloc((size_t)cap * sizeof *L->v);
    L->ln = au_alloc((size_t)cap * sizeof *L->ln);
    L->n = 0;
    int line = sec->line;
    for (char *p = L->buf; *p; line++) {
        char *e = strchr(p, '\n');
        if (e)
            *e = 0;
        char *t = trim(p);
        if (*t && !(t[0] == '#' && (t[1] == ' ' || t[1] == 0))) {
            L->v[L->n] = t;
            L->ln[L->n] = line;
            L->n++;
        }
        if (!e)
            break;
        p = e + 1;
    }
}

static void lines_free(Lines *L)
{
    au_free(L->buf);
    au_free(L->v);
    au_free(L->ln);
}

#define FOR_LINES(sec, var, lineno)                                                       \
    for (Lines L_ = {0}, *once_ = (lines_init(&L_, (sec)), &L_); once_; lines_free(&L_), once_ = NULL) \
        for (int i_ = 0, lineno = 0; i_ < L_.n; i_++)                                    \
            for (char *var = L_.v[i_]; (lineno = L_.ln[i_]), var; var = NULL)

static void run_expect(const Section *sec, AuDocument *doc, AuCssResolver *r)
{
    (void)r;
    FOR_LINES(sec, line, ln)
    {
        char *sel, *rest, *prop, *want;
        char *l = trim(line);
        if (!split_first(l, " -> ", &sel, &rest) || !split_first(rest, ":", &prop, &want)) {
            fail(ln, "bad expect line", "SEL -> prop: value", l);
            continue;
        }
        AuCssPseudo pseudo = AU_CSS_PSEUDO_NONE;
        char *pp = strstr(sel, "::");
        if (pp) {
            if (strcmp(pp, "::before") == 0)
                pseudo = AU_CSS_PSEUDO_BEFORE;
            else if (strcmp(pp, "::after") == 0)
                pseudo = AU_CSS_PSEUDO_AFTER;
            else if (strcmp(pp, "::marker") == 0)
                pseudo = AU_CSS_PSEUDO_MARKER;
            *pp = 0;
        }
        AuCssSelectorList *sl = au_css_selector_parse(sel, strlen(sel));
        if (!sl) {
            fail(ln, "selector does not parse", sel, "");
            continue;
        }
        AuElement *el = au_css_query_selector(sl, &doc->node);
        au_css_selector_free(sl);
        if (!el) {
            fail(ln, "no element matches", sel, "");
            continue;
        }
        const AuStyle *st = au_element_style(el);
        if (pseudo)
            st = st ? au_style_pseudo(st, pseudo) : NULL;
        AuBuf b;
        au_buf_init(&b);
        if (!st)
            au_buf_puts(&b, "(none)");
        else if (!au_style_serialize_property(st, au_atom_cstr(prop), &b))
            au_buf_puts(&b, "(unknown property)");
        char what[256];
        snprintf(what, sizeof what, "%s %s", sel, prop);
        check_str(ln, what, want, au_buf_cstr(&b));
        au_buf_free(&b);
    }
}

static void run_matches(const Section *sec, AuDocument *doc)
{
    FOR_LINES(sec, line, ln)
    {
        char *sel, *want;
        if (!split(trim(line), " => ", &sel, &want)) {
            fail(ln, "bad matches line", "SEL => ids", line);
            continue;
        }
        AuCssSelectorList *sl = au_css_selector_parse(sel, strlen(sel));
        AuBuf b;
        au_buf_init(&b);
        if (!sl) {
            au_buf_puts(&b, "invalid");
        } else {
            const AuNode *root = &doc->node;
            for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
                if (n->type != AU_NODE_ELEMENT)
                    continue;
                AuElement *el = (AuElement *)n;
                if (au_css_selector_matches(sl, el)) {
                    if (b.len)
                        au_buf_putc(&b, ' ');
                    au_buf_puts(&b, el->id ? au_atom_str(el->id) : au_atom_str(el->tag));
                }
            }
            if (!b.len)
                au_buf_puts(&b, "(none)");
            au_css_selector_free(sl);
        }
        check_str(ln, sel, want, au_buf_cstr(&b));
        au_buf_free(&b);
    }
}

static void run_decls(const Section *sec)
{
    FOR_LINES(sec, line, ln)
    {
        char *in, *want;
        if (!split(trim(line), " => ", &in, &want)) {
            fail(ln, "bad decls line", "css => serialization", line);
            continue;
        }
        AuCssDeclBlock *b = au_css_parse_inline_style(in, strlen(in), NULL);
        AuBuf o;
        au_buf_init(&o);
        au_css_decl_block_serialize(b, &o);
        check_str(ln, in, want, au_buf_cstr(&o));
        au_buf_free(&o);
        au_css_decl_block_free(b);
    }
}

static void run_get(const Section *sec)
{
    FOR_LINES(sec, line, ln)
    {
        char *lhs, *want, *in, *prop;
        if (!split(trim(line), " => ", &lhs, &want) || !split(lhs, " | ", &in, &prop)) {
            fail(ln, "bad get line", "css | prop => value", line);
            continue;
        }
        AuCssDeclBlock *b = au_css_parse_inline_style(in, strlen(in), NULL);
        AuBuf o;
        au_buf_init(&o);
        if (!au_css_decl_block_get(b, au_atom_cstr(prop), &o))
            au_buf_puts(&o, "(unset)");
        check_str(ln, lhs, want, au_buf_cstr(&o));
        au_buf_free(&o);
        au_css_decl_block_free(b);
    }
}

static void run_media(const Section *sec, const Options *opt)
{
    AuCssResolver *r = au_css_resolver_new();
    AuCssDevice d;
    au_css_device_default(&d);
    d.width = opt->width;
    d.height = opt->height;
    d.dark = opt->dark;
    d.print = opt->print;
    au_css_resolver_set_device(r, &d);
    FOR_LINES(sec, line, ln)
    {
        char *q, *want;
        if (!split(trim(line), " => ", &q, &want)) {
            fail(ln, "bad media line", "query => bool", line);
            continue;
        }
        bool got = au_css_media_matches(r, q, strlen(q));
        check_str(ln, q, want, got ? "true" : "false");
    }
    au_css_resolver_free(r);
}

static void run_supports(const Section *sec)
{
    FOR_LINES(sec, line, ln)
    {
        char *q, *want;
        if (!split(trim(line), " => ", &q, &want)) {
            fail(ln, "bad supports line", "cond => bool", line);
            continue;
        }
        bool got;
        char *p, *v;
        if (strncmp(q, "decl ", 5) == 0 && split_first(q + 5, " | ", &p, &v))
            got = au_css_supports_decl(p, strlen(p), v, strlen(v));
        else
            got = au_css_supports(q, strlen(q));
        check_str(ln, q, want, got ? "true" : "false");
    }
}

static void run_selectors(const Section *sec)
{
    FOR_LINES(sec, line, ln)
    {
        char *in, *want;
        if (!split(trim(line), " => ", &in, &want)) {
            fail(ln, "bad selectors line", "sel => out", line);
            continue;
        }
        CssTokens tk = {0};
        css_tokenize(in, strlen(in), &tk);
        CssSelParseCtx cx = {0};
        AuSelList *l = css_selector_parse(&tk, 0, tk.n, &cx);
        AuBuf o;
        au_buf_init(&o);
        if (!l) {
            au_buf_puts(&o, "invalid");
        } else {
            css_sellist_serialize(l, &o);
            uint32_t sp = l->sel[0].spec;
            for (uint32_t i = 1; i < l->n; i++)
                if (l->sel[i].spec > sp)
                    sp = l->sel[i].spec;
            au_buf_printf(&o, " (%u,%u,%u)", (unsigned)(sp >> 16), (unsigned)((sp >> 8) & 255), (unsigned)(sp & 255));
            css_sellist_free(l);
        }
        css_tokens_free(&tk);
        check_str(ln, in, want, au_buf_cstr(&o));
        au_buf_free(&o);
    }
}

static void run_tokens(const Section *sec)
{
    FOR_LINES(sec, line, ln)
    {
        char *in, *want;
        if (!split(trim(line), " => ", &in, &want)) {
            fail(ln, "bad tokens line", "css => out", line);
            continue;
        }
        CssTokens tk = {0};
        css_tokenize(in, strlen(in), &tk);
        AuBuf o;
        au_buf_init(&o);
        css_tokens_serialize(&tk, 0, tk.n, &o);
        css_tokens_free(&tk);
        check_str(ln, in, want, au_buf_cstr(&o));
        au_buf_free(&o);
    }
}

static void run_sheet_checks(const Section *sec, AuCssSheet *s)
{
    if (!s) {
        fail(sec->line, "no css section", "", "");
        return;
    }
    if (strcmp(sec->kind, "rules") == 0) {
        char got[32];
        snprintf(got, sizeof got, "%u", (unsigned)au_css_sheet_rule_count(s));
        char *b = au_strndup(sec->body, strlen(sec->body));
        check_str(sec->line, "rule count", trim(b), got);
        au_free(b);
        return;
    }
    AuBuf all;
    au_buf_init(&all);
    if (strcmp(sec->kind, "imports") == 0) {
        for (size_t i = 0; i < au_css_sheet_import_count(s); i++) {
            const AuCssImport *im = au_css_sheet_import(s, i);
            au_buf_printf(&all, "%s | %s | %s | %s | %s\n", im->url, im->media, im->layer ? im->layer : "(none)",
                          im->supports ? im->supports : "(none)", im->supported ? "true" : "false");
        }
    } else if (strcmp(sec->kind, "fontfaces") == 0) {
        for (size_t i = 0; i < au_css_sheet_font_face_count(s); i++) {
            const AuCssFontFace *f = au_css_sheet_font_face(s, i);
            const AuCssValue *fam = au_css_font_face_descriptor(f, "font-family");
            const AuCssValue *src = au_css_font_face_descriptor(f, "src");
            AuBuf t;
            au_buf_init(&t);
            if (fam)
                au_css_value_serialize(fam, &t);
            au_buf_puts(&t, " | ");
            if (src)
                au_css_value_serialize(src, &t);
            au_buf_printf(&all, "%s\n", au_buf_cstr(&t));
            au_buf_free(&t);
        }
    } else if (strcmp(sec->kind, "keyframes") == 0) {
        for (uint32_t i = 0; i < s->nkeyframes; i++) {
            const AuCssKeyframes *k = &s->keyframes[i];
            au_buf_printf(&all, "%s:", au_atom_str(k->name));
            for (uint32_t f = 0; f < k->nframes; f++)
                for (uint32_t o = 0; o < k->frames[f].noffsets; o++) {
                    au_buf_putc(&all, ' ');
                    css_serialize_number(k->frames[f].offsets[o], &all);
                }
            au_buf_putc(&all, '\n');
        }
    }
    /* compare line by line against the body's non-comment lines */
    AuBuf want;
    au_buf_init(&want);
    FOR_LINES(sec, line, ln)
    {
        (void)ln;
        au_buf_printf(&want, "%s\n", trim(line));
    }
    check_str(sec->line, sec->kind, au_buf_cstr(&want), au_buf_cstr(&all));
    au_buf_free(&want);
    au_buf_free(&all);
}

static void run_case(Case *c)
{
    cur_case = c->name;
    Options opt;
    parse_options("", &opt);
    const char *html = NULL;
    bool has_css = false, needs_doc = false;
    for (int i = 0; i < c->n; i++) {
        const char *k = c->s[i].kind;
        if (strcmp(k, "options") == 0)
            parse_options(c->s[i].body, &opt);
        else if (strcmp(k, "html") == 0)
            html = c->s[i].body;
        else if (strcmp(k, "css") == 0)
            has_css = true;
        else if (strcmp(k, "expect") == 0 || strcmp(k, "matches") == 0)
            needs_doc = true;
    }
    AuCssResolver *r = NULL;
    AuCssSheet *first = NULL;
    AuDocument *doc = NULL;
    if (has_css || needs_doc)
        r = make_resolver(c, &opt, &first);
    if (needs_doc) {
        doc = build_dom(html ? html : default_html, opt.quirks);
        au_css_style_document(r, doc);
    }
    for (int i = 0; i < c->n; i++) {
        Section *s = &c->s[i];
        const char *k = s->kind;
        if (strcmp(k, "expect") == 0)
            run_expect(s, doc, r);
        else if (strcmp(k, "matches") == 0)
            run_matches(s, doc);
        else if (strcmp(k, "decls") == 0)
            run_decls(s);
        else if (strcmp(k, "get") == 0)
            run_get(s);
        else if (strcmp(k, "media") == 0)
            run_media(s, &opt);
        else if (strcmp(k, "supports") == 0)
            run_supports(s);
        else if (strcmp(k, "selectors") == 0)
            run_selectors(s);
        else if (strcmp(k, "tokens") == 0)
            run_tokens(s);
        else if (strcmp(k, "rules") == 0 || strcmp(k, "imports") == 0 || strcmp(k, "fontfaces") == 0 ||
                 strcmp(k, "keyframes") == 0)
            run_sheet_checks(s, first);
        else if (strcmp(k, "options") != 0 && strcmp(k, "html") != 0 && strcmp(k, "css") != 0)
            fail(s->line - 1, "unknown section", "", k);
    }
    if (doc) {
        au_css_clear_document(doc);
        au_node_unref(&doc->node);
    }
    if (r)
        au_css_resolver_free(r);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = au_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static void free_case(Case *c)
{
    for (int i = 0; i < c->n; i++) {
        au_free(c->s[i].kind);
        au_free(c->s[i].arg);
        au_free(c->s[i].body);
    }
    au_free(c->s);
    au_free(c->name);
    memset(c, 0, sizeof *c);
}

static void run_file(const char *path)
{
    cur_file = path;
    char *text = read_file(path);
    if (!text) {
        fprintf(stderr, "cannot read %s\n", path);
        failures++;
        return;
    }
    Case c = {0};
    AuBuf body;
    au_buf_init(&body);
    Section *cur = NULL;
    int lineno = 0;
    char *p = text;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol)
            *eol = 0;
        lineno++;
        if (strncmp(p, "=== ", 4) == 0) {
            if (cur) {
                cur->body = au_strndup(body.data ? body.data : "", body.len);
                cur = NULL;
            }
            if (c.name)
                run_case(&c);
            free_case(&c);
            c.name = au_strndup(p + 4, strlen(p + 4));
            au_buf_clear(&body);
        } else if (strncmp(p, "--- ", 4) == 0 && c.name) {
            if (cur)
                cur->body = au_strndup(body.data ? body.data : "", body.len);
            au_buf_clear(&body);
            if (c.n == c.cap) {
                c.cap = c.cap ? c.cap * 2 : 8;
                c.s = au_realloc(c.s, (size_t)c.cap * sizeof *c.s);
            }
            cur = &c.s[c.n++];
            memset(cur, 0, sizeof *cur);
            char *k = trim(p + 4);
            char *sp = strchr(k, ' ');
            if (sp) {
                *sp = 0;
                cur->arg = au_strndup(trim(sp + 1), strlen(trim(sp + 1)));
            }
            cur->kind = au_strndup(k, strlen(k));
            cur->line = lineno + 1;
        } else if (cur) {
            au_buf_puts(&body, p);
            au_buf_putc(&body, '\n');
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    if (cur)
        cur->body = au_strndup(body.data ? body.data : "", body.len);
    if (c.name)
        run_case(&c);
    free_case(&c);
    au_buf_free(&body);
    au_free(text);
}

int main(int argc, char **argv)
{
    au_css_init();
    for (int i = 1; i < argc; i++)
        run_file(argv[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
