/* Tests of the <meta charset> prescan (HTML 13.2.3.2) in src/html/input.c,
 * and of its use by the parser's SNIFF mode. With an argument N, also
 * fuzzes the prescan for N iterations (mutated cases, bounds only). */
#include "html/html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* src/html/input.c */
size_t au_html_prescan_charset(const uint8_t *data, size_t len, bool (*valid)(void *ctx, const char *label, size_t n),
                               void *ctx, char *out, size_t cap);

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

/* Accepts a few labels, as an encoding table would. */
static bool valid(void *ctx, const char *l, size_t n)
{
    (void)ctx;
    static const char *known[] = {"utf-8", "utf8", "windows-1252", "iso-8859-1", "latin1", "shift_jis",
                                  "euc-jp",  "utf-16", "x-user-defined", "koi8-r", "us-ascii"};
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        if (strlen(known[i]) == n && !strncmp(known[i], l, n))
            return true;
    return false;
}

static void scan_case(const char *html, const char *want)
{
    char out[64];
    size_t n = au_html_prescan_charset((const uint8_t *)html, strlen(html), valid, NULL, out, sizeof out);
    if (!want)
        CHECK(n == 0, "%s: none expected, got %.*s", html, (int)n, out);
    else
        CHECK(n == strlen(want) && !memcmp(out, want, n), "%s: got \"%.*s\", want %s", html, (int)n, out, want);
}

static void test_prescan(void)
{
    scan_case("<meta charset=utf-8>", "utf-8");
    scan_case("<META CHARSET=\"UTF-8\">", "utf-8");
    scan_case("<meta charset='windows-1252'>", "windows-1252");
    scan_case("<meta  charset = utf-8 >", "utf-8");
    scan_case("<meta charset=utf-8/>", NULL); /* the unquoted value runs to '>': "utf-8/" is not a label */
    scan_case("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=shift_jis\">", "shift_jis");
    scan_case("<meta content=\"text/html; charset=euc-jp\" http-equiv=content-type>", "euc-jp");
    scan_case("<meta content=\"text/html; charset='koi8-r'\" http-equiv=\"Content-Type\">", "koi8-r");
    scan_case("<meta content=\"text/html; charset=utf-8\">", NULL); /* no http-equiv */
    scan_case("<meta http-equiv=\"Content-Type\">", NULL);
    scan_case("<meta charset=bogus><meta charset=utf-8>", "utf-8");
    scan_case("<!-- <meta charset=koi8-r> --><meta charset=utf-8>", "utf-8");
    scan_case("<!--><meta charset=utf-8>", "utf-8"); /* "<!-->" is a whole comment */
    scan_case("<p title='<meta charset=koi8-r>'><meta charset=utf-8>", "utf-8");
    scan_case("<script>/* <meta charset=koi8-r> */</script>", "koi8-r"); /* the prescan does not know script */
    scan_case("<metacharset=utf-8>", NULL);
    scan_case("<meta charset=utf-8 charset=koi8-r>", "utf-8"); /* first attribute wins */
    scan_case("<meta charset=\"utf-8", NULL); /* the input ends inside the value */
    scan_case("<?xml version=\"1.0\"?><meta charset=utf-8>", "utf-8");
    scan_case("</div foo=\"<meta charset=koi8-r>\"><meta charset=utf-8>", "utf-8");
    scan_case("", NULL);
    scan_case("<", NULL);
    scan_case("<meta", NULL);
    /* Beyond 1024 bytes the prescan stops. */
    char big[1200];
    memset(big, ' ', sizeof big);
    memcpy(big + 1100, "<meta charset=utf-8>", 20);
    big[1120] = 0;
    scan_case(big, NULL);
    memcpy(big + 900, "<meta charset=utf-8>", 20);
    scan_case(big, "utf-8");
}

static char *body_text(const char *html, size_t len, AuHtmlEncoding enc)
{
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.encoding = enc;
    AuDocument *doc = au_html_parse(html, len, &o);
    AuBuf b;
    au_buf_init(&b);
    au_html_serialize(&doc->node, &b, false);
    au_node_unref(&doc->node);
    return au_buf_take(&b, NULL);
}

static void test_parser_sniff(void)
{
    /* "é" in windows-1252 is 0xE9; with a meta naming windows-1252 the
     * parser decodes it as U+00E9 (C3 A9 in the UTF-8 output). */
    /* The decoder cannot hold bytes back (HtmlDecoder has no buffer for
     * it), so it prescans only when the first chunk fed has 1024 bytes or
     * is the last; au_html_parse feeds the whole document as one chunk
     * that is not marked last, so the document must be 1024 bytes. */
    char doc[1200];
    int dl = snprintf(doc, sizeof doc, "<meta charset=windows-1252><p>\xe9</p><!--%1100s-->", "");
    char *s = body_text(doc, (size_t)dl, AU_HTML_ENC_SNIFF);
    CHECK(strstr(s, "<p>\xc3\xa9</p>") != NULL, "meta windows-1252 honoured: %.80s", s);
    au_free(s);
    const char shortdoc[] = "<meta charset=windows-1252><p>\xe9</p>";
    s = body_text(shortdoc, sizeof shortdoc - 1, AU_HTML_ENC_SNIFF);
    CHECK(strstr(s, "<p>\xef\xbf\xbd</p>") != NULL, "short document: the UTF-8 default (known limit): %s", s);
    au_free(s);
    /* An explicit encoding wins over the meta. */
    const char doc2[] = "<meta charset=windows-1252><p>\xc3\xa9</p>";
    s = body_text(doc2, sizeof doc2 - 1, AU_HTML_ENC_UTF8);
    CHECK(strstr(s, "<p>\xc3\xa9</p>") != NULL, "explicit UTF-8 wins: %s", s);
    au_free(s);
    /* No meta: UTF-8. */
    const char doc3[] = "<p>\xc3\xa9</p>";
    s = body_text(doc3, sizeof doc3 - 1, AU_HTML_ENC_SNIFF);
    CHECK(strstr(s, "<p>\xc3\xa9</p>") != NULL, "default UTF-8: %s", s);
    au_free(s);
    /* A BOM wins over the meta. */
    const char doc4[] = "\xef\xbb\xbf<meta charset=windows-1252><p>\xc3\xa9</p>";
    s = body_text(doc4, sizeof doc4 - 1, AU_HTML_ENC_SNIFF);
    CHECK(strstr(s, "<p>\xc3\xa9</p>") != NULL, "BOM wins: %s", s);
    au_free(s);
}

static uint32_t rng = 1;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void fuzz(long iters)
{
    static const char *seeds[] = {
        "<meta charset=utf-8>",
        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=shift_jis\">",
        "<!-- x --><meta content='charset=\"koi8-r\"' http-equiv=content-type>",
        "</p a=b><?x?><!d><meta charset = 'latin1' >",
    };
    static const char dict[] = "<>!-=/'\" \t\nmetacharsethttp-equivcontent;?";
    char buf[2048];
    long found = 0;
    for (long it = 0; it < iters; it++) {
        const char *s = seeds[rnd() % 4];
        size_t len = strlen(s);
        memcpy(buf, s, len);
        int muts = 1 + (int)(rnd() % 8);
        for (int k = 0; k < muts; k++) {
            size_t pos = rnd() % (len + 1);
            switch (rnd() % 3) {
            case 0:
                if (len)
                    buf[rnd() % len] = dict[rnd() % (sizeof dict - 1)];
                break;
            case 1:
                if (len < sizeof buf - 1) {
                    memmove(buf + pos + 1, buf + pos, len - pos);
                    buf[pos] = (char)(rnd() & 0xff);
                    len++;
                }
                break;
            default:
                if (len && pos < len) {
                    memmove(buf + pos, buf + pos + 1, len - pos - 1);
                    len--;
                }
            }
        }
        /* Exact-size heap copy so a read past the end is caught. */
        uint8_t *heap = malloc(len ? len : 1);
        memcpy(heap, buf, len);
        char out[16];
        size_t n = au_html_prescan_charset(heap, len, valid, NULL, out, sizeof out);
        if (n >= sizeof out) {
            printf("prescan result too long\n");
            failures++;
        }
        found += n > 0;
        free(heap);
    }
    printf("prescan fuzz: %ld iterations, %ld found a label\n", iters, found);
}

int main(int argc, char **argv)
{
    test_prescan();
    test_parser_sniff();
    if (argc > 1)
        fuzz(atol(argv[1]));
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
