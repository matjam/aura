/* Data-driven layout tests.
 *
 * A case file holds cases separated by "=== name" lines. Each case is an
 * HTML document, then a line "---", then expectations, one per line:
 *
 *   viewport W H            (before the HTML; default 800 600)
 *   #id x=10 y=20 w=100 h=50 lines=2
 *                           border box in document coordinates (any
 *                           subset of keys; lines: line boxes of the
 *                           element's box)
 *   hit X Y #id             topmost element at viewport point (X, Y)
 *   hit X Y <tag>           ... or by tag name
 *   pixel X Y RRGGBB        colour at viewport pixel (X, Y), +-8 per channel
 *   doc w=W h=H             document size
 *   scroll Y                paint and hit test with the document scrolled
 *   link X Y url            the link under (X, Y) resolves to url
 *
 * The font is Ahem-like (tests/layout/ahem.h): every character is a 1em
 * square, the default 16px gives 16px glyphs and 16px lines, so geometry
 * does not depend on system fonts. Values compare within 0.05px. */
#include "ahem.h"
#include "page/page.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static const char *cur_file, *cur_case;

static void fail(int line, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void fail(int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: [%s] ", cur_file, line, cur_case);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    failures++;
}

static AuElement *by_id(AuDocument *doc, const char *id)
{
    AuAtom a = au_atom_lookup(id, strlen(id));
    if (!a)
        return NULL;
    for (AuNode *n = au_node_next(&doc->node, &doc->node); n; n = au_node_next(n, &doc->node))
        if (au_is_element(n) && au_as_element(n)->id == a)
            return au_as_element(n);
    return NULL;
}

static bool near(float a, float b)
{
    return fabsf(a - b) <= 0.05f;
}

typedef struct LineCount {
    int n;
} LineCount;

/* Line boxes of an element's box, from the dump (keeps the test on the
 * public API). */
static int count_lines(AuLayout *L, AuElement *el)
{
    AuBuf b;
    au_buf_init(&b);
    au_layout_dump(L, &b);
    au_buf_cstr(&b);
    /* Find the element's block line: "<tag id=ID>" then count "line " at
     * the next indentation level until the indentation drops. */
    char key[160];
    snprintf(key, sizeof key, " id=%s>", au_atom_str(el->id));
    char *p = strstr(b.data, key);
    int n = -1;
    if (p) {
        char *ls = p;
        while (ls > b.data && ls[-1] != '\n')
            ls--;
        int ind = 0;
        while (ls[ind] == ' ')
            ind++;
        n = 0;
        char *q = strchr(p, '\n');
        while (q && *++q) {
            int d = 0;
            while (q[d] == ' ')
                d++;
            if (d <= ind)
                break;
            if (d == ind + 2 && !strncmp(q + d, "line ", 5))
                n++;
            q = strchr(q, '\n');
        }
    }
    au_buf_free(&b);
    return n;
}

static void run_case(AuFontSet *fonts, const char *html, size_t hlen, char *expect, int line0, int vw, int vh)
{
    AuDocView *dv = au_docview_new(fonts, NULL, NULL);
    au_docview_load(dv, html, hlen, "file:///test/case.html");
    au_docview_set_viewport(dv, vw, vh);
    au_docview_update(dv);
    AuDocument *doc = au_docview_document(dv);
    AuLayout *L = au_docview_layout(dv);
    AuSurface *surf = NULL;
    int scroll = 0;
    int line = line0;
    for (char *ln = strtok(expect, "\n"); ln; ln = strtok(NULL, "\n"), line++) {
        while (*ln == ' ' || *ln == '\t')
            ln++;
        if (!*ln || *ln == '#' ? (*ln != '#') : false)
            continue;
        if (!*ln)
            continue;
        if (ln[0] == '/' && ln[1] == '/')
            continue;
        if (!strncmp(ln, "scroll ", 7)) {
            scroll = atoi(ln + 7);
            if (surf) {
                au_surface_free(surf);
                surf = NULL;
            }
            continue;
        }
        if (ln[0] == '#') {
            char id[128];
            int k = 0;
            const char *p = ln + 1;
            while (*p && *p != ' ' && k < 127)
                id[k++] = *p++;
            id[k] = 0;
            AuElement *el = by_id(doc, id);
            if (!el) {
                fail(line, "no element #%s", id);
                continue;
            }
            AuRect r;
            if (!au_layout_element_rect(L, el, &r)) {
                fail(line, "#%s has no box", id);
                continue;
            }
            while (*p) {
                while (*p == ' ')
                    p++;
                if (!*p)
                    break;
                char key[16];
                int kk = 0;
                while (*p && *p != '=' && kk < 15)
                    key[kk++] = *p++;
                key[kk] = 0;
                if (*p != '=') {
                    fail(line, "bad expectation '%s'", ln);
                    break;
                }
                p++;
                float v = strtof(p, (char **)&p);
                checks++;
                float got;
                if (!strcmp(key, "x"))
                    got = r.x;
                else if (!strcmp(key, "y"))
                    got = r.y;
                else if (!strcmp(key, "w"))
                    got = r.w;
                else if (!strcmp(key, "h"))
                    got = r.h;
                else if (!strcmp(key, "lines"))
                    got = (float)count_lines(L, el);
                else {
                    fail(line, "unknown key %s", key);
                    continue;
                }
                if (!near(got, v))
                    fail(line, "#%s %s: expected %g, got %g", id, key, v, got);
            }
            continue;
        }
        if (!strncmp(ln, "hit ", 4) || !strncmp(ln, "link ", 5)) {
            bool is_link = ln[0] == 'l';
            char *p = ln + (is_link ? 5 : 4);
            float x = strtof(p, &p), y = strtof(p, &p);
            while (*p == ' ')
                p++;
            AuHitResult hr;
            au_docview_set_scroll(dv, 0, scroll);
            au_docview_hit_test(dv, (int)x, (int)y + scroll, &hr);
            checks++;
            if (is_link) {
                char url[2048] = "";
                if (hr.link) {
                    AuAtom ha = au_atom_lookup("href", 4);
                    const char *href = au_element_get_attr(hr.link, ha);
                    au_url_resolve(au_docview_base_url(dv), href, url, sizeof url);
                }
                if (strcmp(url, p) != 0)
                    fail(line, "link at %g,%g: expected %s, got %s", x, y, p, url);
                continue;
            }
            const char *got = "none";
            char buf[160];
            if (hr.element) {
                if (*p == '#' && hr.element->id)
                    snprintf(buf, sizeof buf, "#%s", au_atom_str(hr.element->id));
                else
                    snprintf(buf, sizeof buf, "<%s>", au_atom_str(hr.element->tag));
                got = buf;
            }
            if (strcmp(got, p) != 0)
                fail(line, "hit %g,%g: expected %s, got %s", x, y, p, got);
            continue;
        }
        if (!strncmp(ln, "pixel ", 6)) {
            char *p = ln + 6;
            int x = (int)strtol(p, &p, 10), y = (int)strtol(p, &p, 10);
            unsigned want = (unsigned)strtoul(p, &p, 16);
            if (!surf) {
                surf = au_surface_new(vw, vh);
                AuCanvas *c = au_canvas_new(surf);
                au_docview_paint(dv, c, (AuRect){0, 0, (float)vw, (float)vh}, 0, scroll);
                au_canvas_free(c);
            }
            checks++;
            AuColor got = au_surface_get(surf, x, y) & 0xFFFFFF;
            int dr = (int)((got >> 16) & 255) - (int)((want >> 16) & 255);
            int dg = (int)((got >> 8) & 255) - (int)((want >> 8) & 255);
            int db = (int)(got & 255) - (int)(want & 255);
            if (abs(dr) > 8 || abs(dg) > 8 || abs(db) > 8)
                fail(line, "pixel %d,%d: expected %06x, got %06x", x, y, want, (unsigned)got);
            continue;
        }
        if (!strncmp(ln, "doc ", 4)) {
            int w, h;
            au_docview_content_size(dv, &w, &h);
            char *p = ln + 4;
            while (*p) {
                while (*p == ' ')
                    p++;
                if (!*p)
                    break;
                char k = *p;
                p += 2;
                int v = (int)strtol(p, &p, 10);
                checks++;
                int got = k == 'w' ? w : h;
                if (got != v)
                    fail(line, "doc %c: expected %d, got %d", k, v, got);
            }
            continue;
        }
        fail(line, "unknown expectation '%s'", ln);
    }
    if (surf)
        au_surface_free(surf);
    au_docview_free(dv);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_layout case-file...\n");
        return 2;
    }
    static uint8_t fontdata[16384];
    AuFontSet *fonts = ahem_fontset(fontdata);
    int ncases = 0;
    for (int a = 1; a < argc; a++) {
        cur_file = argv[a];
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            fprintf(stderr, "cannot open %s\n", argv[a]);
            return 1;
        }
        static char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        char *p = strstr(buf, "=== ");
        int line = 1;
        for (char *q = buf; q < p; q++)
            line += *q == '\n';
        while (p) {
            char *name = p + 4;
            char *eol = strchr(name, '\n');
            if (!eol)
                break;
            *eol = 0;
            cur_case = name;
            line++;
            char *body = eol + 1;
            char *next = strstr(body, "\n=== ");
            if (next)
                *next = 0;
            int vw = 800, vh = 600;
            if (!strncmp(body, "viewport ", 9)) {
                vw = (int)strtol(body + 9, &body, 10);
                vh = (int)strtol(body, &body, 10);
                while (*body && *body != '\n')
                    body++;
                if (*body)
                    body++;
                line++;
            }
            char *sep = strstr(body, "\n---\n");
            int body_lines = 0;
            if (!sep) {
                fail(line, "case without ---");
            } else {
                for (char *q = body; q < sep; q++)
                    body_lines += *q == '\n';
                *sep = 0;
                run_case(fonts, body, strlen(body), sep + 5, line + body_lines + 2, vw, vh);
            }
            ncases++;
            int total_lines = 0;
            for (char *q = body; *q; q++)
                total_lines += *q == '\n';
            if (sep)
                for (char *q = sep + 5; *q; q++)
                    total_lines += *q == '\n';
            line += total_lines + 3;
            p = next ? next + 1 : NULL;
        }
    }
    au_fontset_free(fonts);
    printf("%d cases, %d checks, %d failures\n", ncases, checks, failures);
    return failures ? 1 : 0;
}
