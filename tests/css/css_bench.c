/* Timings of sheet parsing and document styling, and the memory the
 * computed styles take per element.
 *
 *   css_bench [--small] [file.css ...]
 *
 * Parses a generated stress sheet (and each file given), styles a
 * generated document with it, and prints the times and the bytes of
 * computed style per element. */
#include "../../src/css/css_int.h"
#include "../../src/html/html.h"

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
    size_t rules = s->nstyle_rules; /* without parsing the blocks that are still text */
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
               slash ? slash + 1 : argv[i], got, (size_t)s->nstyle_rules, after - before,
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


/* ---- css_bench --where: what each structure of the parsed sheets takes ----
 * Bytes for this host and as they would be on 32-bit PowerPC (from the
 * struct layouts there). Rules, selectors, media lists and parsed
 * declaration arrays live in each sheet's arena (a few chunks per
 * sheet); the rows marked "heap" are allocations of their own. With
 * --style PAGE.html the page is styled first, so the rows show what a
 * page keeps once the rules that match have parsed their blocks. */
typedef struct {
    const char *name;
    size_t bytes, bytes32, blocks;
} Where;
enum { W_RULE, W_SELLIST, W_COMP, W_SELEXT, W_DECLS, W_MEDIA, W_LAYER, W_COND, W_TEXT, W_BLOB, W_CUSTOM, W_VAR,
       W_OTHER, W_COUNT };
static Where wh[W_COUNT] = {{"rules", 0, 0, 0},
                            {"selector lists and selectors", 0, 0, 0},
                            {"components and filter hashes", 0, 0, 0},
                            {"component extras and strings", 0, 0, 0},
                            {"parsed declaration blocks", 0, 0, 0},
                            {"media lists", 0, 0, 0},
                            {"layer names", 0, 0, 0},
                            {"condition text", 0, 0, 0},
                            {"heap: sheet text", 0, 0, 0},
                            {"heap: declared value trees (blobs)", 0, 0, 0},
                            {"heap: custom property token lists", 0, 0, 0},
                            {"heap: var() token lists", 0, 0, 0},
                            {"heap: sheet, imports, faces, keyframes", 0, 0, 0}};
static size_t n_rules, n_style, n_lazy, n_lazy_bytes, n_lazy_media, n_selectors, n_comps, n_decls;

static void w_add(int w, size_t b, size_t b32, size_t blocks)
{
    wh[w].bytes += b;
    wh[w].bytes32 += b32;
    wh[w].blocks += blocks;
}

static const void **seen_p;
static size_t seen_n, seen_cap;
static bool seen(const void *p)
{
    if ((seen_n + 1) * 2 > seen_cap) {
        size_t nc = seen_cap ? seen_cap * 2 : 4096;
        const void **nt = calloc(nc, sizeof *nt);
        for (size_t i = 0; i < seen_cap; i++)
            if (seen_p[i]) {
                size_t h = ((uintptr_t)seen_p[i] >> 2) * 2654435761u & (nc - 1);
                while (nt[h])
                    h = (h + 1) & (nc - 1);
                nt[h] = seen_p[i];
            }
        free(seen_p);
        seen_p = nt;
        seen_cap = nc;
    }
    size_t h = ((uintptr_t)p >> 2) * 2654435761u & (seen_cap - 1);
    while (seen_p[h]) {
        if (seen_p[h] == p)
            return true;
        h = (h + 1) & (seen_cap - 1);
    }
    seen_p[h] = p;
    seen_n++;
    return false;
}

static size_t up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

static size_t tree32(const AuCssValue *v)
{
    /* the 32-bit size of what css_blob_pack stores after the root */
    size_t n = 0;
    if (v->type == AU_CSSV_IDENT || v->type == AU_CSSV_STRING || v->type == AU_CSSV_URL)
        return (v->n + 1 + 7) & ~(size_t)7;
    if (v->type == AU_CSSV_FUNCTION || v->type == AU_CSSV_CALC || v->type == AU_CSSV_LIST ||
        v->type == AU_CSSV_COMMA_LIST) {
        n += (v->n * 12 + 7) & ~(size_t)7;
        for (uint32_t i = 0; i < v->n; i++)
            n += tree32(&v->u.items[i]);
    }
    return n;
}

/* The declarations of a block: what they refer to on the heap. The array
 * itself is counted by the caller (arena or heap). */
static void w_decl_refs(const AuCssDeclBlock *b)
{
    n_decls += b->n;
    for (uint32_t i = 0; i < b->n; i++) {
        const AuCssDecl *d = &b->d[i];
        if (d->kind == DK_BLOB && !seen(d->u.blob)) {
            size_t tail = d->u.blob->base_url ? strlen(d->u.blob->base_url) + 1 : 0;
            w_add(W_BLOB, d->u.blob->size, 24 + tree32(&d->u.blob->value) + tail, 1);
        } else if ((d->kind == DK_VAR || d->kind == DK_CUSTOM) && !seen(d->u.toks)) {
            const CssTokList *t = d->u.toks;
            size_t b64, b32, blocks;
            css_toklist_mem(t, &b64, &b32, &blocks);
            w_add(d->kind == DK_VAR ? W_VAR : W_CUSTOM, b64, b32, blocks);
        }
    }
}

static void w_sellist(const AuSelList *l)
{
    if (!l || seen(l))
        return;
    w_add(W_SELLIST, sizeof *l + l->n * sizeof(AuSelector), 4 + l->n * 12, 0);
    n_selectors += l->n;
    for (uint32_t i = 0; i < l->n; i++) {
        const AuSelector *s = &l->sel[i];
        size_t tail = (s->nhash + (s->pseudo == AU_CSS_PSEUDO_UNKNOWN_WEBKIT ? 1 : 0)) * 4;
        w_add(W_COMP, up(s->ncomp * sizeof(AuSelComp) + tail, sizeof(void *)), s->ncomp * 8 + tail, 0);
        n_comps += s->ncomp;
        for (uint32_t k = 0; k < s->ncomp; k++) {
            const AuSelComp *c = &s->comp[k];
            bool list = c->type == SC_NESTING ||
                        (c->type == SC_PSEUDO &&
                         (c->op == PC_IS || c->op == PC_WHERE || c->op == PC_NOT || c->op == PC_HAS));
            if (list) {
                if (c->type != SC_NESTING)
                    w_sellist(c->u.list);
                continue;
            }
            if (!(c->flags & SCF_EXT) || seen(c->u.ext))
                continue;
            w_add(W_SELEXT, sizeof(CssSelExt), 16, 0);
            if (c->type == SC_PSEUDO && c->op >= PC_NTH_CHILD && c->op <= PC_NTH_LAST_OF_TYPE)
                w_sellist(c->u.ext->u.nth.of);
            else if (c->u.ext->u.value.s)
                w_add(W_SELEXT, up(c->u.ext->u.value.len + 1, sizeof(void *)), up(c->u.ext->u.value.len + 1, 4), 0);
        }
    }
}

void css_media_mem(const AuCssMediaList *m, size_t *bytes, size_t *bytes32, size_t *blocks);

static void w_media(const AuCssMediaList *m)
{
    if (!m || seen(m))
        return;
    size_t b = 0, b32 = 0, bl = 0;
    css_media_mem(m, &b, &b32, &bl);
    w_add(W_MEDIA, b, b32, bl);
}

static void w_rules(const CssRule *r)
{
    for (; r; r = r->next) {
        n_rules++;
        w_add(W_RULE, sizeof *r, 20, 0);
        bool lazy = (r->flags & RF_LAZY) != 0;
        if (r->type == RULE_STYLE) {
            n_style++;
            w_sellist(r->u.sel);
        }
        if (r->type == RULE_STYLE || r->type == RULE_NESTED_DECLS) {
            if (lazy) {
                n_lazy++;
                n_lazy_bytes += r->c.lazy_len;
            } else {
                if (r->b.decls->n)
                    w_add(W_DECLS, sizeof(AuCssDeclBlock) + r->b.decls->n * sizeof(AuCssDecl),
                          12 + r->b.decls->n * 20, 0);
                w_decl_refs(r->b.decls);
            }
        } else if (r->type == RULE_MEDIA || r->type == RULE_IMPORT) {
            w_media(r->u.media);
            if (lazy) {
                n_lazy_media++;
                n_lazy_bytes += r->c.lazy_len;
            }
        } else if (r->type == RULE_LAYER_BLOCK || r->type == RULE_LAYER_STMT) {
            w_add(W_LAYER, r->b.nlayers * sizeof(CssLayerName), r->b.nlayers * 8, 0);
            for (uint32_t i = 0; i < r->b.nlayers; i++)
                w_add(W_LAYER, up(r->u.layers[i].n * sizeof(AuAtom), sizeof(void *)),
                      r->u.layers[i].n * sizeof(AuAtom), 0);
        } else if (r->u.text) {
            w_add(W_COND, up(strlen(r->u.text) + 1, 8), up(strlen(r->u.text) + 1, 8), 0);
        }
        if (!lazy)
            w_rules(r->c.children);
    }
}

static size_t arena_bytes, arena_used, arena_chunks;

static void w_sheet(const AuCssSheet *s)
{
    w_add(W_OTHER, sizeof *s, 96, 1);
    if (s->base_url)
        w_add(W_OTHER, strlen(s->base_url) + 1, strlen(s->base_url) + 1, 1);
    if (s->text && s->text_owned)
        w_add(W_TEXT, s->text_len + 1, s->text_len + 1, 1);
    w_media(s->media);
    w_rules(s->rules);
    size_t ab, au, ac;
    css_arena_mem(&s->arena, &ab, &au, &ac);
    arena_bytes += ab;
    arena_used += au;
    arena_chunks += ac;
    if (s->imports) {
        w_add(W_OTHER, s->imports_cap * sizeof(AuCssImport), s->imports_cap * 24, 1);
        w_add(W_OTHER, s->imports_cap * sizeof(CssRule *), s->imports_cap * 4, 1);
        w_add(W_OTHER, s->imports_cap * sizeof(CssLayerName), s->imports_cap * 8, 1);
    }
    if (s->faces)
        w_add(W_OTHER, s->faces_cap * sizeof(AuCssFontFace), s->faces_cap * 16, 1);
    for (uint32_t i = 0; i < s->nfaces; i++) {
        if (s->faces[i].descs.d)
            w_add(W_OTHER, s->faces[i].descs.cap * sizeof(AuCssDecl), s->faces[i].descs.cap * 20, 1);
        w_decl_refs(&s->faces[i].descs);
    }
    if (s->keyframes)
        w_add(W_OTHER, s->keyframes_cap * sizeof(AuCssKeyframes), s->keyframes_cap * 12, 1);
    for (uint32_t i = 0; i < s->nkeyframes; i++) {
        const AuCssKeyframes *k = &s->keyframes[i];
        if (k->frames)
            w_add(W_OTHER, k->nframes * sizeof(AuCssKeyframe), k->nframes * 12, 1);
        for (uint32_t j = 0; j < k->nframes; j++) {
            w_add(W_OTHER, k->frames[j].noffsets * sizeof(float), k->frames[j].noffsets * sizeof(float), 1);
            w_add(W_OTHER, sizeof(AuCssDeclBlock), 12, 1);
            if (k->frames[j].decls->d)
                w_add(W_OTHER, k->frames[j].decls->cap * sizeof(AuCssDecl), k->frames[j].decls->cap * 20, 1);
            w_decl_refs(k->frames[j].decls);
        }
    }
    if (s->props)
        w_add(W_OTHER, s->props_cap * sizeof(CssPropertyRule), s->props_cap * 12, 1);
}

static char *slurp(const char *path, size_t *len);

static int where_report(int argc, char **argv)
{
    size_t text = 0;
    int first = 2;
    AuDocument *doc = NULL;
    AuCssResolver *r = NULL;
    if (argc > 3 && strcmp(argv[2], "--style") == 0) {
        size_t len;
        char *html = slurp(argv[3], &len);
        if (!html) {
            fprintf(stderr, "cannot read %s\n", argv[3]);
            return 2;
        }
        doc = au_html_parse(html, len, NULL);
        free(html);
        r = au_css_resolver_new();
        first = 4;
    }
    int ns = argc - first;
    AuCssSheet **sheets = calloc((size_t)ns + 1, sizeof *sheets);
    for (int i = 0; i < ns; i++) {
        /* a file given again shares the parsed sheet, as in a document */
        bool again = false;
        for (int k = 0; k < i && !again; k++)
            if (!strcmp(argv[first + k], argv[first + i])) {
                sheets[i] = sheets[k];
                again = true;
            }
        if (!again) {
            size_t got;
            char *buf = slurp(argv[first + i], &got);
            if (!buf) {
                fprintf(stderr, "cannot read %s\n", argv[first + i]);
                return 2;
            }
            sheets[i] = au_css_sheet_parse(buf, got, AU_CSS_ORIGIN_AUTHOR, NULL, NULL);
            free(buf);
            text += got;
        }
        if (r)
            au_css_resolver_add_sheet(r, again ? au_css_sheet_retain(sheets[i]) : sheets[i]);
    }
    if (r)
        au_css_style_document(r, doc);
    for (int i = 0; i < ns; i++) {
        bool again = false;
        for (int k = 0; k < i && !again; k++)
            again = sheets[k] == sheets[i];
        if (!again)
            w_sheet(sheets[i]);
    }
    size_t tb = 0, tb32 = 0, tbl = 0, ab = 0, ab32 = 0;
    printf("%-42s %10s %8s %10s\n", "structure", "bytes", "blocks", "32-bit");
    for (int i = 0; i < W_COUNT; i++) {
        printf("%-42s %10zu %8zu %10zu\n", wh[i].name, wh[i].bytes, wh[i].blocks, wh[i].bytes32);
        if (i < W_TEXT) {
            ab += wh[i].bytes;
            ab32 += wh[i].bytes32;
        } else {
            tb += wh[i].bytes;
            tb32 += wh[i].bytes32;
            tbl += wh[i].blocks;
        }
    }
    /* On 32-bit PowerPC a chunk is the same number of bytes and holds
     * more: scale the chunks by what the records take there. */
    size_t chunks32 = ab ? (size_t)((double)arena_bytes * (double)ab32 / (double)ab) : 0;
    printf("%-42s %10zu %8zu %10zu  (%zu used; records %zu here, %zu on 32-bit)\n", "arena chunks", arena_bytes,
           arena_chunks, chunks32, arena_used, ab, ab32);
    printf("%-42s %10zu %8zu %10zu  (with 8 bytes a block on Mac OS 9: %zu)\n", "total: chunks and heap rows",
           arena_bytes + tb, arena_chunks + tbl, chunks32 + tb32, chunks32 + tb32 + 8 * (arena_chunks + tbl));
    printf("%zu bytes of text; %zu rules (%zu style rules), %zu selectors, %zu components, %zu parsed declarations\n",
           text, n_rules, n_style, n_selectors, n_comps, n_decls);
    printf("still text: %zu blocks of declarations and %zu @media blocks, %zu bytes\n", n_lazy, n_lazy_media,
           n_lazy_bytes);
    return 0;
}

/* css_bench --page PAGE.html SHEET.css...: the sheets parsed in the order
 * given (a file given more than once is parsed once and shared, as the
 * document view does for a page that links it twice), then the page
 * styled with them: times, and the heap the
 * sheets, the index and the styles keep (macOS only). */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

static int page_report(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: css_bench --page PAGE.html [SHEET.css ...]\n");
        return 2;
    }
    size_t len;
    char *html = slurp(argv[2], &len);
    if (!html) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    AuDocument *doc = au_html_parse(html, len, NULL);
    free(html);
    AuCssResolver *r = au_css_resolver_new();
    size_t nent, nuni;
    css_resolver_index_stats(r, &nent, &nuni);
    int nel = 0;
    for (AuNode *n = doc->node.first_child; n; n = au_node_next(n, &doc->node))
        nel += n->type == AU_NODE_ELEMENT;
    /* read the files first, so that their buffers are not in the heap figures */
    int ns = argc - 3;
    char **text = calloc((size_t)ns + 1, sizeof *text);
    size_t *tlen = calloc((size_t)ns + 1, sizeof *tlen), total = 0;
    for (int i = 0; i < ns; i++) {
        text[i] = slurp(argv[3 + i], &tlen[i]);
        if (!text[i]) {
            fprintf(stderr, "cannot read %s\n", argv[3 + i]);
            return 2;
        }
        total += tlen[i];
    }
    size_t h0 = heap_in_use();
    double t0 = now_ms();
    size_t rules = 0;
    AuCssSheet **sheets = calloc((size_t)ns + 1, sizeof *sheets);
    int shared = 0;
    for (int i = 0; i < ns; i++) {
        /* a file given again shares the parsed sheet, as in a document */
        for (int k = 0; k < i && !sheets[i]; k++)
            if (!strcmp(argv[3 + k], argv[3 + i]))
                sheets[i] = au_css_sheet_retain(sheets[k]);
        if (sheets[i])
            shared++;
        else
            sheets[i] = au_css_sheet_parse(text[i], tlen[i], AU_CSS_ORIGIN_AUTHOR, NULL, NULL);
        rules += sheets[i]->nstyle_rules; /* without parsing the blocks that are still text */
        au_css_resolver_add_sheet(r, sheets[i]);
    }
    double t1 = now_ms();
    size_t h1 = heap_in_use();
    css_resolver_index_stats(r, &nent, &nuni);
    double t2 = now_ms();
    size_t h2 = heap_in_use();
    au_css_style_document(r, doc);
    double t3 = now_ms();
    size_t h3 = heap_in_use();
    au_css_style_document(r, doc);
    double t4 = now_ms();
    printf("%d sheets (%d of them given before), %zu bytes of text, %zu style rules, %d elements\n", ns, shared,
           total, rules, nel);
    printf("  parse %.1f ms; index %.1f ms (%zu entries); first style %.1f ms; style again %.1f ms\n", t1 - t0, t2 - t1,
           nent, t3 - t2, t4 - t3);
    printf("  heap: sheets %zu bytes, index %zu, styles and what styling parsed %zu; total %zu\n", h1 - h0, h2 - h1,
           h3 - h2, h3 - h0);
    au_css_clear_document(doc);
    au_css_resolver_free(r); /* frees the sheets it was given */
    printf("  after freeing the styles and the sheets: %zu bytes more than before (atoms stay)\n",
           heap_in_use() - h0);
    au_node_unref(&doc->node);
    return 0;
}

int main(int argc, char **argv)
{
    au_css_init();
    if (argc > 1 && strcmp(argv[1], "--mem") == 0)
        return mem_report(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--page") == 0)
        return page_report(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--where") == 0)
        return where_report(argc, argv);
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
