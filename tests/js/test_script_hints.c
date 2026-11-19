/* Script hints (QuickJS patch 0022, js/runtime.c "Script hints",
 * browser/script_hints.c).
 *
 * A large script's functions that were compiled at their first call
 * are recorded in the hint store; the next runtime that runs the same
 * script compiles them with it, so calling them compiles nothing and
 * records nothing. A function not called before is still compiled at
 * its first call. The store keeps the most recently used scripts and
 * survives a round trip through its file, and a damaged file loses
 * only its damaged lines. */
#include "js/js.h"
#include "dom/dom.h"
#include "core/core.h"
#include "browser/script_hints.h"

#include <stdio.h>
#include <string.h>

static int failures;
static double clock_ms;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL line %d: ", __LINE__);                                                                       \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static double t_now(void *ctx)
{
    (void)ctx;
    return clock_ms += 1;
}

/* The store, counting what the runtime adds. */
static const AuJsHintStore *inner;
static int adds;

static const uint32_t *w_get(void *ctx, uint32_t len, uint32_t hash, size_t *count)
{
    (void)ctx;
    return inner->get(inner->ctx, len, hash, count);
}

static void w_add(void *ctx, uint32_t len, uint32_t hash, uint32_t ofs)
{
    (void)ctx;
    adds++;
    inner->add(inner->ctx, len, hash, ofs);
}

static const AuJsHintStore wrap = {NULL, w_get, w_add};

/* Ten functions of about 200 bytes; f5 holds a nested one, g. Then a
 * comment to make the script large (64 KB or more). */
static char *script(size_t *len)
{
    AuBuf b;
    au_buf_init(&b);
    for (int i = 0; i < 10; i++) {
        au_buf_printf(&b, "function f%d(a) {\n", i);
        if (i == 5)
            au_buf_puts(&b, "    function g(x) { var t = x * 2; /* padding padding padding padding padding padding "
                            "padding padding padding padding */ return t + 1; }\n    return g(a);\n");
        else
            au_buf_printf(&b, "    var t = a + %d; /* padding padding padding padding padding padding padding "
                              "padding padding padding padding padding padding */\n    return t;\n", i);
        au_buf_puts(&b, "}\n");
    }
    /* in a function, so that the script's functions make up most of it
     * and share one text (patch 0008): hints name offsets in it */
    au_buf_puts(&b, "function filler() { /*");
    while (b.len < (80u << 10))
        au_buf_puts(&b, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n");
    au_buf_puts(&b, "*/ }\n");
    *len = b.len;
    return (char *)b.data;
}

static void run(const char *src, size_t len, const char *calls, const char *want)
{
    AuDocument *doc = au_document_new();
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.now_ms = t_now;
    AuJsSettings s;
    au_js_settings_default(&s);
    s.hints = &wrap;
    AuJs *js = au_js_new(doc, "http://a.test/", &s, &h);
    CHECK(js, "runtime");
    if (!js) {
        au_node_unref(&doc->node);
        return;
    }
    CHECK(au_js_run_classic(js, NULL, src, len, "http://a.test/bundle.js", 1), "script ran");
    AuBuf out;
    au_buf_init(&out);
    CHECK(au_js_eval(js, calls, strlen(calls), &out) && !strcmp(au_buf_cstr(&out), want), "%s: got %s, want %s",
          calls, au_buf_cstr(&out), want);
    au_buf_free(&out);
    au_js_free(js);
    au_node_unref(&doc->node);
}

static void test_runtime(void)
{
    AuScriptHints *hints = au_script_hints_new();
    inner = au_script_hints_store(hints);
    size_t len;
    char *src = script(&len);

    adds = 0;
    run(src, len, "[f1(1), f3(1), f5(1)].join()", "2,4,3");
    CHECK(adds == 4, "first run recorded %d functions, want 4 (f1, f3, f5, g)", adds);
    CHECK(au_script_hints_count(hints) == 1, "one script");
    CHECK(au_script_hints_dirty(hints), "dirty after adds");

    adds = 0;
    run(src, len, "[f1(2), f3(2), f5(2)].join()", "3,5,5");
    CHECK(adds == 0, "second run compiled %d functions at their first call, want 0", adds);

    adds = 0;
    run(src, len, "[f1(3), f7(3)].join()", "4,10");
    CHECK(adds == 1, "a function not called before: %d recorded, want 1", adds);

    /* the same length, other bytes (in the comment): no hints */
    src[len - 6] = 'X';
    adds = 0;
    run(src, len, "f1(1)", "2");
    CHECK(adds == 1, "a changed script: %d recorded, want 1", adds);
    CHECK(au_script_hints_count(hints) == 2, "two scripts");

    /* Text given to eval, mostly a comment: its function keeps a text of
     * its own (patch 0008), whose offset in the script the hint names. */
    static const char ev[] =
        "var c = '/*' + 'x'.repeat(6000) + '*/\\nfunction q(a) { var t = a + 1; /* padding padding padding "
        "padding padding padding padding padding padding padding padding */ return t; }\\nq';"
        "(0, eval)(c)(4)";
    adds = 0;
    run(src, len, ev, "5");
    CHECK(adds == 1, "eval: first run recorded %d, want 1", adds);
    adds = 0;
    run(src, len, ev, "5");
    CHECK(adds == 0, "eval: second run compiled %d at their first call, want 0", adds);

    au_free(src);
    au_script_hints_free(hints);
}

static char *dump(const AuScriptHints *h)
{
    AuBuf b;
    au_buf_init(&b);
    au_script_hints_serialize(h, &b);
    au_buf_cstr(&b);
    return (char *)b.data;
}

static void test_store(void)
{
    AuScriptHints *h = au_script_hints_new();
    const AuJsHintStore *s = au_script_hints_store(h);
    s->add(s->ctx, 1000, 0xabc, 500);
    s->add(s->ctx, 1000, 0xabc, 10);
    s->add(s->ctx, 1000, 0xabc, 10);   /* again */
    s->add(s->ctx, 1000, 0xabc, 1000); /* past the end */
    s->add(s->ctx, 2000, 0x1, 0);
    size_t n = 0;
    const uint32_t *o = s->get(s->ctx, 1000, 0xabc, &n);
    CHECK(o && n == 2 && o[0] == 10 && o[1] == 500, "sorted, no repeats, none past the end (n %zu)", n);
    CHECK(!s->get(s->ctx, 1000, 0xabd, &n) && n == 0, "other hash");

    /* round trip; the most recently used first */
    char *text = dump(h);
    CHECK(!strcmp(text, "# Aura script hints 1\n3e8 abc a 1ea\n7d0 1 0\n"), "file: %s", text);
    AuScriptHints *h2 = au_script_hints_new();
    CHECK(au_script_hints_parse(h2, text, strlen(text)), "parse");
    char *text2 = dump(h2);
    CHECK(!strcmp(text, text2), "round trip: %s", text2);
    au_free(text2);
    au_script_hints_free(h2);
    /* every prefix parses (a cut offset only loses accuracy) */
    for (size_t cut = 0; cut <= strlen(text); cut++) {
        char *part = au_strndup(text, cut);
        h2 = au_script_hints_new();
        bool ok = au_script_hints_parse(h2, part, cut);
        CHECK(ok == (cut >= 22), "prefix %zu: header", cut);
        CHECK(au_script_hints_count(h2) <= 2, "prefix %zu: count", cut);
        au_script_hints_free(h2);
        au_free(part);
    }
    au_free(text);

    /* damage: a wrong header, a bad number, offsets that do not ascend
     * or pass the end; the good lines stay */
    h2 = au_script_hints_new();
    CHECK(!au_script_hints_parse(h2, "# Aura script hints 2\n3e8 abc a\n", 31), "wrong header");
    const char *bad = "# Aura script hints 1\n"
                      "3e8 abc a 0\n"      /* repeats */
                      "3e8 abd 3e8\n"      /* past the end */
                      "zz 1 2\n"           /* not hex */
                      "3e8 abe 123456789\n" /* too long */
                      "64 5 1 2 3\n";      /* good */
    CHECK(au_script_hints_parse(h2, bad, strlen(bad)), "damaged lines parse");
    {
        char *d = dump(h2);
        CHECK(au_script_hints_count(h2) == 1, "only the good line: %s", d);
        au_free(d);
    }
    s = au_script_hints_store(h2);
    o = s->get(s->ctx, 100, 5, &n);
    CHECK(o && n == 3 && o[0] == 1 && o[1] == 3 && o[2] == 6, "deltas (n %zu)", n);
    au_script_hints_free(h2);

    /* the least recently used script goes */
    s = au_script_hints_store(h);
    for (uint32_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS; i++)
        s->add(s->ctx, 5000, 100 + i, 1);
    CHECK(!s->get(s->ctx, 1000, 0xabc, &n) && !s->get(s->ctx, 2000, 0x1, &n), "oldest dropped");
    CHECK(s->get(s->ctx, 5000, 100, &n), "newest kept");
    CHECK(au_script_hints_count(h) == AU_SCRIPT_HINTS_SCRIPTS, "full: %zu", au_script_hints_count(h));

    au_script_hints_clear_dirty(h);
    au_script_hints_clear(h);
    CHECK(au_script_hints_count(h) == 0 && au_script_hints_dirty(h), "clear");
    au_script_hints_free(h);
}

int main(void)
{
    test_store();
    test_runtime();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("script hints: ok\n");
    return 0;
}
