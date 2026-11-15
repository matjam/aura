/* Timings of sheet parsing and document styling, and the memory the
 * computed styles take per element.
 *
 *   css_bench [--small] [file.css ...]
 *
 * Parses a generated stress sheet (and each file given), styles a
 * generated document with it, and prints the times and the bytes of
 * computed style per element. */
#include "../../src/css/css_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void css_resolver_index_stats(AuCssResolver *r, size_t *entries, size_t *universal);

static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 8) % n;
}

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static const char *const tags[] = {"div", "span", "p", "a", "ul", "li", "section", "em", "img", "table", "td"};
static const char *const props[] = {"color: #%06x", "margin: %upx %upx", "padding-left: %u%%",
                                    "font-size: %upx", "width: calc(%u%% - 10px)", "display: block",
                                    "background: rgb(%u 20 30) url(x.png) no-repeat", "border: 1px solid red",
                                    "flex: %u 1 auto", "line-height: 1.%u", "--v%u: 3px",
                                    "height: var(--v%u, 4px)", "transform: translate(%upx, 2px) rotate(3deg)",
                                    "text-decoration: underline", "opacity: 0.%u"};

static void gen_sheet(AuBuf *b, int rules, int classes)
{
    for (int i = 0; i < rules; i++) {
        switch (rnd(9)) {
        case 0: au_buf_printf(b, ".c%u", rnd(classes)); break;
        case 1: au_buf_printf(b, "#i%u", rnd(classes)); break;
        case 2: au_buf_printf(b, "%s.c%u > %s", tags[rnd(11)], rnd(classes), tags[rnd(11)]); break;
        case 3: au_buf_printf(b, "ul li.c%u a:hover", rnd(classes)); break;
        case 4: au_buf_printf(b, "[data-k=\"v%u\"]", rnd(50)); break;
        case 5: au_buf_printf(b, "%s:nth-child(2n+1) .c%u", tags[rnd(11)], rnd(classes)); break;
        case 6: au_buf_printf(b, ":is(.c%u, .c%u) %s", rnd(classes), rnd(classes), tags[rnd(11)]); break;
        case 7: au_buf_printf(b, ".c%u .c%u ~ .c%u", rnd(classes), rnd(classes), rnd(classes)); break;
        default: au_buf_printf(b, "%s", tags[rnd(11)]); break;
        }
        au_buf_puts(b, " { ");
        int nd = 1 + (int)rnd(5);
        for (int k = 0; k < nd; k++) {
            au_buf_printf(b, props[rnd(sizeof props / sizeof props[0])], rnd(255), rnd(40), rnd(9));
            au_buf_puts(b, "; ");
        }
        au_buf_puts(b, "}\n");
        if (i % 500 == 499)
            au_buf_printf(b, "@media (min-width: %upx) { .c%u { color: red; } }\n", rnd(2000), rnd(classes));
    }
}

static AuDocument *gen_doc(int n, int classes, int *count)
{
    AuDocument *doc = au_document_new();
    AuNode *html = au_element_new(doc, au_atom_cstr("html"), AU_NS_HTML);
    au_node_append(&doc->node, html);
    AuNode *body = au_element_new(doc, au_atom_cstr("body"), AU_NS_HTML);
    au_node_append(html, body);
    AuNode *stack[64];
    int sp = 0;
    stack[0] = body;
    *count = 2;
    for (int i = 0; i < n; i++) {
        AuNode *e = au_element_new(doc, au_atom_cstr(tags[rnd(11)]), AU_NS_HTML);
        AuElement *el = (AuElement *)e;
        char buf[64];
        int nc = (int)rnd(3);
        buf[0] = 0;
        for (int k = 0; k < nc; k++) {
            char c[16];
            snprintf(c, sizeof c, "%sc%u", k ? " " : "", rnd(classes));
            strncat(buf, c, sizeof buf - strlen(buf) - 1);
        }
        if (nc)
            au_element_set_attr(el, AU_ATOM_class, buf, strlen(buf));
        if (rnd(10) == 0) {
            snprintf(buf, sizeof buf, "i%u", rnd(classes));
            au_element_set_attr(el, AU_ATOM_id, buf, strlen(buf));
        }
        if (rnd(8) == 0) {
            snprintf(buf, sizeof buf, "v%u", rnd(50));
            au_element_set_attr(el, au_atom_cstr("data-k"), buf, strlen(buf));
        }
        au_node_append(stack[sp], e);
        au_node_unref(e);
        (*count)++;
        uint32_t m = rnd(10);
        if (m < 4 && sp < 63)
            stack[++sp] = e;
        else if (m > 7 && sp > 0)
            sp--;
    }
    return doc;
}

/* Distinct styles and groups reachable from the document. */
static void memory_report(AuDocument *doc, int elements)
{
    const void **seen = NULL;
    size_t nseen = 0, cap = 0, bytes = 0, nstyles = 0;
    const AuNode *root = &doc->node;
    for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
        if (n->type != AU_NODE_ELEMENT)
            continue;
        const AuStyle *s = au_element_style((AuElement *)n);
        for (; s; s = s->pseudo ? s->next : s->pseudos) {
            const void *items[1 + G_COUNT];
            size_t sizes[1 + G_COUNT];
            items[0] = s;
            sizes[0] = sizeof(AuStyle);
            for (int g = 0; g < G_COUNT; g++) {
                items[1 + g] = css_style_group(s, g);
                sizes[1 + g] = css_group_size(g);
            }
            for (int k = 0; k < 1 + G_COUNT; k++) {
                bool dup = false;
                for (size_t q = nseen; q-- > 0 && !dup;)
                    dup = seen[q] == items[k];
                if (dup)
                    continue;
                if (nseen == cap) {
                    cap = cap ? cap * 2 : 1024;
                    seen = au_realloc(seen, cap * sizeof *seen);
                }
                seen[nseen++] = items[k];
                bytes += sizes[k];
                if (k == 0)
                    nstyles++;
            }
            if (!s->pseudo && !s->pseudos)
                break;
        }
    }
    size_t full = sizeof(AuStyle);
    for (int g = 0; g < G_COUNT; g++)
        full += css_group_size(g);
    printf("  memory: %zu distinct styles for %d elements; %zu bytes of styles and groups, %.1f bytes/element "
           "(plus the AuElement.style pointer); an unshared style is %zu bytes\n",
           nstyles, elements, bytes, (double)bytes / elements, full);
    printf("  group sizes:");
    static const char *const names[] = {"inh", "inhr", "box", "surr", "bg", "flex", "rare"};
    for (int g = 0; g < G_COUNT; g++)
        printf(" %s %zu", names[g], css_group_size(g));
    printf(", AuStyle %zu\n", sizeof(AuStyle));
    au_free(seen);
}

static void bench(const char *label, const char *css, size_t len, int elements, int classes, bool mem)
{
    double t0 = now_ms();
    AuCssSheet *s = au_css_sheet_parse(css, len, AU_CSS_ORIGIN_AUTHOR, NULL, NULL);
    double t1 = now_ms();
    size_t rules = au_css_sheet_rule_count(s);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, s);
    int count;
    AuDocument *doc = gen_doc(elements, classes, &count);
    size_t nent, nuni;
    css_resolver_index_stats(r, &nent, &nuni);
    double t2 = now_ms();
    au_css_style_document(r, doc);
    double t3 = now_ms();
    au_css_style_document(r, doc);
    double t4 = now_ms();
    printf("%s: %zu bytes, %zu style rules; parse %.1f ms (%.1f MB/s); style %d elements: first %.1f ms "
           "(index build included), again %.1f ms (%.2f us/element)\n",
           label, len, rules, t1 - t0, (double)len / 1048576.0 / ((t1 - t0) / 1000.0 + 1e-9), count, t3 - t2,
           t4 - t3, (t4 - t3) * 1000.0 / count);
    printf("  index: %zu selector entries (UA sheet included), %zu in the universal bucket\n", nent, nuni);
    if (mem)
        memory_report(doc, count);
    au_css_clear_document(doc);
    au_node_unref(&doc->node);
    au_css_resolver_free(r);
}

#ifdef __APPLE__
#include <malloc/malloc.h>
static size_t heap_in_use(void)
{
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    return st.size_in_use;
}
#else
static size_t heap_in_use(void) { return 0; }
#endif

/* css_bench --mem file.css...: the heap each parsed sheet keeps (macOS
 * only; elsewhere the sizes print as 0), and the heap of a resolver
 * holding them all with its index built. */
static int mem_report(int argc, char **argv)
{
    AuCssResolver *r = au_css_resolver_new();
    size_t nent, nuni;
    css_resolver_index_stats(r, &nent, &nuni); /* interns the UA sheet's names */
    size_t base = heap_in_use(), text = 0;
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)n + 1);
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        size_t before = heap_in_use();
        AuCssSheet *s = au_css_sheet_parse(buf, got, AU_CSS_ORIGIN_AUTHOR, NULL, NULL);
        size_t after = heap_in_use();
        free(buf);
        const char *slash = strrchr(argv[i], '/');
        printf("%-52.52s %8zu bytes of text, %6zu style rules, %9zu bytes parsed (%.1fx)\n",
               slash ? slash + 1 : argv[i], got, au_css_sheet_rule_count(s), after - before,
               got ? (double)(after - before) / (double)got : 0.0);
        text += got;
        au_css_resolver_add_sheet(r, s);
    }
    size_t sheets = heap_in_use();
    css_resolver_index_stats(r, &nent, &nuni);
    size_t indexed = heap_in_use();
    printf("total: %zu bytes of text, %zu bytes parsed (%.1fx), index %zu bytes for %zu selector entries\n", text,
           sheets - base, text ? (double)(sheets - base) / (double)text : 0.0, indexed - sheets, nent);
    au_css_resolver_free(r);
    return 0;
}

int main(int argc, char **argv)
{
    au_css_init();
    if (argc > 1 && strcmp(argv[1], "--mem") == 0)
        return mem_report(argc, argv);
    bool small = argc > 1 && strcmp(argv[1], "--small") == 0;
    int rules = small ? 2000 : 20000, elements = small ? 2000 : 20000, classes = small ? 300 : 3000;
    AuBuf b;
    au_buf_init(&b);
    gen_sheet(&b, rules, classes);
    bench("generated", b.data, b.len, elements, classes, true);
    au_buf_free(&b);
    bench("UA sheet only", "", 0, elements, classes, true);
    for (int i = small ? 2 : 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = au_alloc((size_t)n + 1);
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        const char *slash = strrchr(argv[i], '/');
        bench(slash ? slash + 1 : argv[i], buf, got, elements, classes, false);
        au_free(buf);
    }
    return 0;
}
