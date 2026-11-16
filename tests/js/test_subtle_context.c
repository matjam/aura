/* crypto.subtle exists only in secure contexts (src/js/prelude_web.js):
 * a document at an http: URL other than localhost has crypto with
 * getRandomValues and no subtle, SubtleCrypto or CryptoKey; https:,
 * file:, localhost and blob: URLs of those have them. */
#include "js/js.h"
#include "dom/dom.h"
#include "core/core.h"

#include <stdio.h>
#include <string.h>

static int failures;
static double clock_ms;

static double t_now(void *ctx)
{
    (void)ctx;
    return clock_ms += 1;
}

static bool t_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    memset(buf, 0x5a, len);
    return true;
}

static void check(const char *url, const char *want)
{
    AuDocument *doc = au_document_new();
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.now_ms = t_now;
    h.random = t_random;
    AuJs *js = au_js_new(doc, url, NULL, &h);
    if (!js) {
        printf("FAIL %s: no runtime\n", url);
        failures++;
        au_node_unref(&doc->node);
        return;
    }
    static const char src[] =
        "[typeof crypto.getRandomValues, 'subtle' in crypto, typeof crypto.subtle, typeof SubtleCrypto,"
        " typeof CryptoKey, 'SubtleCrypto' in globalThis].join(' ')";
    AuBuf out;
    au_buf_init(&out);
    if (!au_js_eval(js, src, sizeof src - 1, &out) || strcmp(au_buf_cstr(&out), want) != 0) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", url, au_buf_cstr(&out), want);
        failures++;
    }
    au_buf_free(&out);
    au_js_free(js);
    au_node_unref(&doc->node);
}

int main(void)
{
    static const char yes[] = "function true object function function true";
    static const char no[] = "function false undefined undefined undefined false";
    check("https://example.com/", yes);
    check("file:///tmp/page.html", yes);
    check("http://localhost:8080/", yes);
    check("http://127.0.0.1/x", yes);
    check("blob:https://example.com/0f5c", yes);
    check("blob:null/0f5c", yes);
    check("http://example.com/", no);
    check("http://localhost.example.com/", no);
    check("blob:http://example.com/0f5c", no);
    check("ftp://example.com/", no);
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
