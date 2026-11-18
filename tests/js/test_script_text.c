/* Source text and source positions of classic scripts (QuickJS patch
 * 0013).
 *
 * A script shorter than JS_NO_TEXT_SCRIPT (2 MB) keeps its functions'
 * text for Function.prototype.toString; a longer one keeps none, and
 * toString gives the native function form with the function's kind
 * (async, generator), or "class NAME { }" for a class. Both keep line and column
 * numbers, which the parser now stores in 5 bytes when both are below
 * 65536 and in 9 bytes otherwise: the errors below are thrown at a
 * column past 65535 and at a line past 65535 as well as at small ones,
 * and a frame and its other documents read the large script's bytecode
 * without its text. */
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

static void expect(AuJs *js, const char *what, const char *src, const char *want)
{
    AuBuf out;
    au_buf_init(&out);
    if (!au_js_eval(js, src, strlen(src), &out) || strcmp(au_buf_cstr(&out), want) != 0) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, au_buf_cstr(&out), want);
        failures++;
    }
    au_buf_free(&out);
}

/* A script of at least min_len bytes: f at line 1, g after 70,000
 * spaces on line 2, h at line 70,003, then a comment to fill. The
 * expected positions are what Aura gave before patch 0013. */
static char *script(const char *prefix, size_t min_len, size_t *len)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_printf(&b, "globalThis.%sf = function f(a) { throw new Error('f' + a) };\n", prefix);
    for (int i = 0; i < 70000; i++)
        au_buf_putc(&b, ' ');
    au_buf_printf(&b, "globalThis.%sg = function g() { throw new Error('g') };\n", prefix);
    for (int i = 0; i < 70000; i++)
        au_buf_putc(&b, '\n');
    au_buf_printf(&b, "  globalThis.%sh = () => { null.x };\n", prefix);
    au_buf_printf(&b, "globalThis.%sdetect = /\\bclass\\b/.test(class{}.toString());\n", prefix);
    au_buf_printf(&b, "globalThis.%sk = class Klass extends Object { constructor() { super() } };\n", prefix);
    au_buf_printf(&b, "globalThis.%sa = async function af() {};\n", prefix);
    au_buf_printf(&b, "globalThis.%sgen = function* gen() {};\n/*", prefix);
    while (b.len < min_len)
        au_buf_puts(&b, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n");
    au_buf_puts(&b, "*/\n");
    *len = b.len;
    return (char *)b.data;
}

/* The source text's bytes from the memory report ("source text N M KB"). */
static long source_kb(AuJs *js)
{
    AuBuf out;
    au_buf_init(&out);
    au_js_eval(js, "aura:memory", 11, &out);
    const char *p = strstr(au_buf_cstr(&out), "source text");
    long count = -1, kb = -1;
    if (p)
        sscanf(p + 11, "%ld %ld", &count, &kb);
    au_buf_free(&out);
    return kb;
}

/* Lets the source text go cold: two passes of JS_PackSourceTexts (one
 * every 5 seconds of the runtime's clock, when script is left) with no
 * toString between them. */
static void go_cold(AuJs *js)
{
    for (int i = 0; i < 3; i++) {
        clock_ms += 6000;
        expect(js, "a quiet eval", "1", "1");
    }
}

static const char where[] =
    "(function(fn) { try { fn(1) } catch (e) { var m = /:(\\d+):(\\d+)/.exec(e.stack); return m[1] + ':' + m[2] } })";

static void check_positions(AuJs *js, const char *p, const char *what)
{
    char src[256], name[64];
    snprintf(src, sizeof src, "%s(%sf)", where, p);
    snprintf(name, sizeof name, "%s: f's error", what);
    expect(js, name, src, "1:55");
    snprintf(src, sizeof src, "%s(%sg)", where, p);
    snprintf(name, sizeof name, "%s: g's error", what);
    expect(js, name, src, "2:70042");
    snprintf(src, sizeof src, "%s(%sh)", where, p);
    snprintf(name, sizeof name, "%s: h's error", what);
    expect(js, name, src, "70003:27");
}

int main(void)
{
    AuDocument *doc = au_document_new();
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.now_ms = t_now;
    AuJsSettings s;
    au_js_settings_default(&s);
    s.memory_limit = 64u << 20;
    AuJs *js = au_js_new(doc, "http://a.test/", &s, &h);
    if (!js) {
        printf("FAIL: no runtime\n");
        return 1;
    }

    size_t len;
    char *small = script("s", 200u << 10, &len);
    if (!au_js_run_classic(js, NULL, small, len, "http://a.test/small.js", 1)) {
        printf("FAIL: the small script did not run\n");
        failures++;
    }
    expect(js, "the small script keeps its text", "sf.toString()",
           "function f(a) { throw new Error('f' + a) }");
    check_positions(js, "s", "small script");

    /* Text nothing reads is deflated (QuickJS patch 0017) and inflated
     * again, byte for byte, when toString asks. A bundle: one outer
     * function around 5000 small ones, so the script's whole text is
     * kept. */
    AuBuf pb;
    au_buf_init(&pb);
    au_buf_puts(&pb, "globalThis.pk = (function outer() { var fns = [];\n");
    for (int i = 0; i < 5000; i++)
        au_buf_printf(&pb, "fns.push(function f%d(a) { return a * %d + 'x%d'; });\n", i, i, i);
    au_buf_puts(&pb, "return fns; })();\n");
    long before = source_kb(js);
    if (!au_js_run_classic(js, NULL, (const char *)pb.data, pb.len, "http://a.test/bundle.js", 1)) {
        printf("FAIL: the bundle did not run\n");
        failures++;
    }
    long hot = source_kb(js) - before;
    go_cold(js);
    long cold = source_kb(js) - before;
    if (hot < 200 || cold < 0 || cold > hot / 4) {
        printf("FAIL packing: the bundle's text took %ld KB, then %ld KB cold\n", hot, cold);
        failures++;
    }
    expect(js, "a packed text's function", "pk[0].toString()", "function f0(a) { return a * 0 + 'x0'; }");
    expect(js, "a function in the middle of a packed text", "pk[2718].toString()",
           "function f2718(a) { return a * 2718 + 'x2718'; }");
    if (source_kb(js) - before < hot) {
        printf("FAIL packing: toString left the text packed\n");
        failures++;
    }
    go_cold(js);
    if (source_kb(js) - before != cold) {
        printf("FAIL packing: packing again took %ld KB, not %ld\n", source_kb(js) - before, cold);
        failures++;
    }
    /* a frame that runs the same bundle finds the packed text by its
     * content and shares it */
    {
        AuDocument *d = au_document_new();
        AuJs *f = au_js_new_child(js, d, "http://a.test/frame0", &s, &h);
        if (f) {
            if (!au_js_run_classic(f, NULL, (const char *)pb.data, pb.len, "http://a.test/bundle2.js", 1)) {
                printf("FAIL: the bundle did not run in the frame\n");
                failures++;
            }
            expect(f, "a frame's function from a shared packed text", "pk[4999].toString()",
                   "function f4999(a) { return a * 4999 + 'x4999'; }");
            /* unpacked to compare, the packed copy kept beside it */
            if (source_kb(js) - before > hot + cold + 1) {
                printf("FAIL packing: the frame's copy took its own text (%ld KB)\n", source_kb(js) - before);
                failures++;
            }
            au_js_free(f);
        } else {
            printf("FAIL: no frame\n");
            failures++;
        }
        au_node_unref(&d->node);
    }
    au_buf_free(&pb);

    char *big = script("b", (2u << 20) + 1, &len);
    if (!au_js_run_classic(js, NULL, big, len, "http://a.test/big.js", 1)) {
        printf("FAIL: the large script did not run\n");
        failures++;
    }
    expect(js, "the large script keeps no text", "bf.toString()", "function f() {\n    [native code]\n}");
    expect(js, "the large script's arrow function", "bh.toString()", "function () {\n    [native code]\n}");
    /* YouTube constructs classes with Reflect.construct only when
     * class{}.toString() says "class" */
    expect(js, "the large script's class{}.toString() test", "String(bdetect)", "true");
    expect(js, "the large script's class", "bk.toString()", "class Klass { }");
    expect(js, "the large script's async function", "ba.toString()",
           "async function af() {\n    [native code]\n}");
    expect(js, "the large script's generator", "bgen.toString()", "function *gen() {\n    [native code]\n}");
    expect(js, "the small script's class", "sk.toString()",
           "class Klass extends Object { constructor() { super() } }");
    check_positions(js, "b", "large script");

    /* A second document of the page reads the compiled large script as
     * bytecode (the compiled-script cache in runtime.c). */
    AuDocument *doc2 = au_document_new();
    AuJs *frame = au_js_new_child(js, doc2, "http://a.test/frame", &s, &h);
    if (!frame) {
        printf("FAIL: no frame\n");
        failures++;
    } else {
        if (!au_js_run_classic(frame, NULL, big, len, "http://a.test/big.js", 1)) {
            printf("FAIL: the large script did not run in the frame\n");
            failures++;
        }
        expect(frame, "the frame's copy keeps no text", "bf.toString()", "function f() {\n    [native code]\n}");
        expect(frame, "the frame's class", "bk.toString()", "class Klass { }");
        expect(frame, "the frame's class{}.toString() test", "String(bdetect)", "true");
        check_positions(frame, "b", "large script in a frame");
        au_js_free(frame);
    }
    au_node_unref(&doc2->node);

    au_free(small);
    au_free(big);
    au_js_free(js);
    au_node_unref(&doc->node);
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
