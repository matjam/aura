/* The script cache (js/runtime.c "Compiled scripts kept between pages",
 * browser/script_cache.c).
 *
 * A large classic script is stored as bytecode the second time a
 * runtime compiles it and read from the store after that; a script too
 * large to keep its text is stored the first time. A script read from
 * the store behaves as one compiled: its functions run, also those
 * compiled at their first call, toString gives their text and errors
 * give their positions. Another URL or other bytes is another script.
 * The store keeps its files within its limit, drops a damaged file,
 * queues writes when told to, and its Index survives a round trip and
 * parses every prefix. */
#include "js/js.h"
#include "dom/dom.h"
#include "core/core.h"
#include "browser/script_cache.h"
#include "browser/script_hints.h"
#include "net/disk_cache.h"

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

/* ---- A folder in memory ---- */

typedef struct File {
    char name[64];
    uint8_t *data;
    size_t len;
    uint32_t mtime;
} File;

static File files[64];
static int nfiles, writes;
static uint32_t file_clock;

static File *file_find(const char *name)
{
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i].name, name))
            return &files[i];
    return NULL;
}

static void f_list(void *ctx, void (*each)(void *, const char *, size_t, uint32_t), void *each_ctx)
{
    (void)ctx;
    /* each may remove files: list a copy of the names */
    char names[64][64];
    size_t sizes[64];
    uint32_t times[64];
    int n = nfiles;
    for (int i = 0; i < n; i++) {
        memcpy(names[i], files[i].name, sizeof names[i]);
        sizes[i] = files[i].len;
        times[i] = files[i].mtime;
    }
    for (int i = 0; i < n; i++)
        each(each_ctx, names[i], sizes[i], times[i]);
}

static bool f_read(void *ctx, const char *name, AuBuf *out)
{
    (void)ctx;
    File *f = file_find(name);
    if (!f || !f->len)
        return false;
    au_buf_append(out, f->data, f->len);
    return true;
}

static bool f_write(void *ctx, const char *name, const void *data, size_t len)
{
    (void)ctx;
    File *f = file_find(name);
    if (!f) {
        if (nfiles == 64)
            return false;
        f = &files[nfiles++];
        memset(f, 0, sizeof *f);
        snprintf(f->name, sizeof f->name, "%s", name);
    }
    au_free(f->data);
    f->data = au_alloc(len ? len : 1);
    memcpy(f->data, data, len);
    f->len = len;
    f->mtime = ++file_clock;
    writes++;
    return true;
}

static void f_remove(void *ctx, const char *name)
{
    (void)ctx;
    File *f = file_find(name);
    if (!f)
        return;
    au_free(f->data);
    *f = files[--nfiles];
}

static void files_reset(void)
{
    while (nfiles)
        f_remove(NULL, files[0].name);
    writes = 0;
}

static const AuDiskCacheFiles folder = {NULL, f_list, f_read, f_write, f_remove};

/* ---- Scripts ---- */

/* Ten functions; f5 holds a nested one. Then a comment in a function, so
 * the script is large and its functions share one text. */
static char *script(size_t *len, size_t size)
{
    AuBuf b;
    au_buf_init(&b);
    for (int i = 0; i < 10; i++) {
        au_buf_printf(&b, "function f%d(a) {\n", i);
        if (i == 5)
            au_buf_puts(&b, "    function g(x) { var t = x * 2; /* padding padding padding padding padding padding "
                            "padding padding padding padding */ return t + 1; }\n    return g(a);\n");
        else if (i == 9)
            au_buf_puts(&b, "    var t = a; /* padding padding padding padding padding padding padding padding "
                            "padding padding padding padding padding */\n    return t.missing.field;\n");
        else
            au_buf_printf(&b, "    var t = a + %d; /* padding padding padding padding padding padding padding "
                              "padding padding padding padding padding padding */\n    return t;\n", i);
        au_buf_puts(&b, "}\n");
    }
    au_buf_puts(&b, "var ran = (typeof ran == 'number' ? ran : 0) + 1;\n");
    au_buf_puts(&b, "function filler() { /*");
    while (b.len < size)
        au_buf_puts(&b, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n");
    au_buf_puts(&b, "*/ }\n");
    *len = b.len;
    return (char *)b.data;
}

static const AuJsCodeStore *code;
static const AuJsHintStore *hints;

static void run_at(const char *src, size_t len, const char *url, const char *calls, const char *want)
{
    AuDocument *doc = au_document_new();
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.now_ms = t_now;
    AuJsSettings s;
    au_js_settings_default(&s);
    s.code = code;
    s.hints = hints;
    s.memory_limit = (size_t)128 << 20;
    AuJs *js = au_js_new(doc, "http://a.test/", &s, &h);
    CHECK(js, "runtime");
    if (!js) {
        au_node_unref(&doc->node);
        return;
    }
    CHECK(au_js_run_classic(js, NULL, src, len, url, 1), "script ran");
    AuBuf out;
    au_buf_init(&out);
    CHECK(au_js_eval(js, calls, strlen(calls), &out) && !strcmp(au_buf_cstr(&out), want), "%s: got %s, want %s",
          calls, au_buf_cstr(&out), want);
    au_buf_free(&out);
    au_js_free(js);
    au_node_unref(&doc->node);
}

static void run(const char *src, size_t len, const char *calls, const char *want)
{
    run_at(src, len, "http://a.test/bundle.js", calls, want);
}

static AuScriptCacheStats stats(const AuScriptCache *c)
{
    AuScriptCacheStats st;
    au_script_cache_stats(c, &st);
    return st;
}

/* What a script's functions say about themselves, the same whether the
 * script was compiled or read. */
#define ABOUT                                                                                                         \
    "(function () { var e; try { f9({}) } catch (x) { e = x.stack.split('\\n')[0].trim() }"                           \
    " return [f1(1), f5(1), f7(1), f3.toString().length, f3.toString().slice(0, 14), e, ran].join('|') })()"
#define ABOUT_WANT "2|3|8|" /* then the length, which the first run gives */

static void test_runtime(void)
{
    AuScriptCache *c = au_script_cache_new(NULL, (size_t)16 << 20);
    AuScriptHints *h = au_script_hints_new();
    code = au_script_cache_store(c);
    hints = au_script_hints_store(h);
    size_t len;
    char *src = script(&len, 80u << 10);

    /* what a compiled script says, from a runtime without the store */
    char want[256];
    {
        code = NULL;
        AuDocument *doc = au_document_new();
        AuJsHooks hk;
        memset(&hk, 0, sizeof hk);
        hk.now_ms = t_now;
        AuJsSettings s;
        au_js_settings_default(&s);
        AuJs *js = au_js_new(doc, "http://a.test/", &s, &hk);
        au_js_run_classic(js, NULL, src, len, "http://a.test/bundle.js", 1);
        AuBuf out;
        au_buf_init(&out);
        au_js_eval(js, ABOUT, strlen(ABOUT), &out);
        snprintf(want, sizeof want, "%s", au_buf_cstr(&out));
        au_buf_free(&out);
        au_js_free(js);
        au_node_unref(&doc->node);
        code = au_script_cache_store(c);
    }
    CHECK(!strncmp(want, ABOUT_WANT, strlen(ABOUT_WANT)) && strstr(want, "function f3(a)") && strstr(want, "bundle.js:"),
          "a compiled script says %s", want);

    run(src, len, ABOUT, want);
    CHECK(stats(c).misses == 1 && stats(c).stores == 0 && stats(c).seen == 1, "first run: asked for, not stored");
    run(src, len, ABOUT, want);
    CHECK(stats(c).misses == 2 && stats(c).stores == 1 && stats(c).count == 1 && stats(c).seen == 0,
          "second run: stored (%u stores)", stats(c).stores);
    run(src, len, ABOUT, want);
    CHECK(stats(c).hits == 1 && stats(c).stores == 1, "third run: read (%u hits)", stats(c).hits);
    /* a function not called before, compiled at its first call from the
     * text the bytecode names */
    run(src, len, "[f2(1), f8(1), f8.toString().slice(0, 14)].join('|')", "3|9|function f8(a)");
    CHECK(stats(c).hits == 2, "fourth run: read");

    /* the same script from another URL is another script */
    run_at(src, len, "http://b.test/bundle.js", "f1(1)", "2");
    CHECK(stats(c).hits == 2 && stats(c).seen == 1, "another URL: not read");
    /* other bytes */
    src[len - 6] = 'X';
    run(src, len, "f1(1)", "2");
    CHECK(stats(c).hits == 2 && stats(c).seen == 2, "other bytes: not read");
    src[len - 6] = '\n';

    /* twice in one page: the second run reads what the first read */
    {
        AuDocument *doc = au_document_new();
        AuJsHooks hk;
        memset(&hk, 0, sizeof hk);
        hk.now_ms = t_now;
        AuJsSettings s;
        au_js_settings_default(&s);
        s.code = code;
        AuJs *js = au_js_new(doc, "http://a.test/", &s, &hk);
        unsigned hits = stats(c).hits;
        CHECK(au_js_run_classic(js, NULL, src, len, "http://a.test/bundle.js", 1), "ran");
        CHECK(au_js_run_classic(js, NULL, src, len, "http://a.test/bundle.js", 1), "ran again");
        AuBuf out;
        au_buf_init(&out);
        CHECK(au_js_eval(js, "ran + '|' + f1(1)", 17, &out) && !strcmp(au_buf_cstr(&out), "2|2"), "ran twice: %s",
              au_buf_cstr(&out));
        au_buf_free(&out);
        CHECK(stats(c).hits == hits + 1, "the page's own copy served the second run");
        au_js_free(js);
        au_node_unref(&doc->node);
    }

    /* without the store's limit nothing is asked for or kept */
    au_script_cache_set_max(c, 0);
    CHECK(stats(c).count == 0 && stats(c).seen == 0, "limit 0 empties it");
    run(src, len, "f1(1)", "2");
    run(src, len, "f1(1)", "2");
    CHECK(stats(c).count == 0 && stats(c).stores == 1, "limit 0 stores nothing");

    au_free(src);
    au_script_hints_free(h);
    au_script_cache_free(c);
    code = NULL;
    hints = NULL;
}

/* A script too large to keep its text has no functions compiled later:
 * stored the first time, read the second. */
static void test_no_text(void)
{
    AuScriptCache *c = au_script_cache_new(NULL, (size_t)16 << 20);
    code = au_script_cache_store(c);
    size_t len;
    char *src = script(&len, (2u << 20) + 4096);
    run(src, len, "[f1(1), f5(1), f7(1), ran].join('|')", "2|3|8|1");
    CHECK(stats(c).stores == 1 && stats(c).hits == 0, "stored at the first compile");
    run(src, len, "[f1(1), f5(1), f7(1), ran, /native code/.test(f1.toString())].join('|')", "2|3|8|1|true");
    CHECK(stats(c).stores == 1 && stats(c).hits == 1, "read at the second");
    au_free(src);
    au_script_cache_free(c);
    code = NULL;
}

/* A module is stored and read as a classic script is, and still
 * evaluates once, with its exports and import.meta. */
static int module_ok;

static void module_done(void *ctx, AuElement *el, bool ok)
{
    (void)ctx, (void)el;
    module_ok += ok;
}

static void run_module(const char *src, size_t len, const char *want)
{
    AuDocument *doc = au_document_new();
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.now_ms = t_now;
    AuJsSettings s;
    au_js_settings_default(&s);
    s.code = code;
    AuJs *js = au_js_new(doc, "http://a.test/", &s, &h);
    module_ok = 0;
    au_js_run_module(js, NULL, src, len, "http://a.test/page", module_done, NULL);
    for (int i = 0; i < 20; i++)
        au_js_checkpoint(js);
    CHECK(module_ok == 1, "the module evaluated");
    static const char q[] = "[modRan, modF(1), modMeta, typeof modG].join('|')";
    AuBuf out;
    au_buf_init(&out);
    CHECK(au_js_eval(js, q, sizeof q - 1, &out) && !strcmp(au_buf_cstr(&out), want), "module: got %s, want %s",
          au_buf_cstr(&out), want);
    au_buf_free(&out);
    au_js_free(js);
    au_node_unref(&doc->node);
}

static void test_module(void)
{
    AuScriptCache *c = au_script_cache_new(NULL, (size_t)16 << 20);
    code = au_script_cache_store(c);
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "export function f(a) { var t = a + 1; /* padding padding padding padding padding padding padding "
                    "padding padding padding padding padding */ return t; }\n"
                    "export function g(a) { var t = a + 2; /* padding padding padding padding padding padding padding "
                    "padding padding padding padding padding */ return t; }\n"
                    "globalThis.modRan = (globalThis.modRan || 0) + 1;\nglobalThis.modF = f;\nglobalThis.modG = g;\n"
                    "globalThis.modMeta = import.meta.url;\nfunction filler() { /*");
    while (b.len < (80u << 10))
        au_buf_puts(&b, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n");
    au_buf_puts(&b, "*/ }\n");
    static const char want[] = "1|2|http://a.test/|function";
    run_module((const char *)b.data, b.len, want);
    CHECK(stats(c).misses == 1 && stats(c).stores == 0, "first run: not stored");
    run_module((const char *)b.data, b.len, want);
    CHECK(stats(c).stores == 1, "second run: stored");
    run_module((const char *)b.data, b.len, want);
    CHECK(stats(c).hits == 1, "third run: read (%u hits)", stats(c).hits);
    au_buf_free(&b);
    au_script_cache_free(c);
    code = NULL;
}

/* ---- The store and its files ---- */

static void key_of(uint8_t key[AU_JS_CODE_KEY], int n)
{
    for (int i = 0; i < AU_JS_CODE_KEY; i++)
        key[i] = (uint8_t)(n * 31 + i * 7 + 1);
}

static bool has(const AuJsCodeStore *s, int n, const uint8_t *code_bytes, size_t len, bool *seen)
{
    uint8_t key[AU_JS_CODE_KEY];
    key_of(key, n);
    size_t got = 0;
    uint8_t *b = s->get(s->ctx, key, &got, seen);
    bool ok = b && got == len && !memcmp(b, code_bytes, len);
    au_free(b);
    return ok;
}

static void test_store(void)
{
    files_reset();
    uint8_t key[AU_JS_CODE_KEY], blob[1000];
    for (size_t i = 0; i < sizeof blob; i++)
        blob[i] = (uint8_t)(i * 13);
    bool seen;

    AuScriptCache *c = au_script_cache_new(&folder, 4000);
    const AuJsCodeStore *s = au_script_cache_store(c);
    CHECK(!has(s, 1, blob, 1000, &seen) && !seen, "nothing yet");
    CHECK(!has(s, 1, blob, 1000, &seen) && seen, "asked for before");
    CHECK(au_script_cache_dirty(c), "the Index changed");
    au_script_cache_save(c);
    CHECK(file_find(AU_SCRIPT_CACHE_INDEX) && !au_script_cache_dirty(c), "Index written");
    key_of(key, 1);
    s->put(s->ctx, key, blob, 1000);
    CHECK(nfiles == 2 && has(s, 1, blob, 1000, &seen) && seen, "stored and read");
    au_script_cache_save(c);
    CHECK(!file_find(AU_SCRIPT_CACHE_INDEX), "an empty Index is removed");
    au_script_cache_free(c);

    /* a new run finds the file */
    c = au_script_cache_new(&folder, 4000);
    s = au_script_cache_store(c);
    CHECK(stats(c).count == 1 && has(s, 1, blob, 1000, &seen), "read by the next run");

    /* past the limit the oldest written goes */
    for (int n = 2; n <= 4; n++) {
        key_of(key, n);
        s->put(s->ctx, key, blob, 1000 - (size_t)n);
    }
    CHECK(stats(c).count == 3 && stats(c).bytes <= 4000, "%u files, %lu bytes", (unsigned)stats(c).count,
          (unsigned long)stats(c).bytes);
    CHECK(!has(s, 1, blob, 1000, &seen) && has(s, 4, blob, 996, &seen), "the oldest went");
    /* one larger than the limit is not stored */
    static uint8_t big[5000];
    key_of(key, 9);
    s->put(s->ctx, key, big, sizeof big);
    CHECK(stats(c).count == 3, "too large to store");

    /* a damaged file is dropped, and its script stored again at once */
    {
        char name[64] = "";
        for (int i = 0; i < nfiles; i++)
            if (files[i].len == 996 + 48)
                snprintf(name, sizeof name, "%s", files[i].name);
        File *f = file_find(name);
        CHECK(f != NULL, "the file of script 4");
        if (f) {
            f->data[f->len - 1] ^= 1;
            CHECK(!has(s, 4, blob, 996, &seen) && seen && !file_find(name), "damaged: dropped");
            CHECK(stats(c).damaged == 1, "counted");
            /* cut short */
            key_of(key, 3);
            for (int i = 0; i < nfiles; i++)
                if (files[i].len == 997 + 48)
                    files[i].len = 20;
            CHECK(!has(s, 3, blob, 997, &seen), "truncated: dropped");
        }
    }

    /* a file that is not a script's is removed when the folder is read */
    f_write(NULL, "stray", "x", 1);
    au_script_cache_free(c);
    c = au_script_cache_new(&folder, 4000);
    s = au_script_cache_store(c);
    CHECK(!file_find("stray") && stats(c).count == 1, "strays removed, %u scripts", (unsigned)stats(c).count);

    /* deferred: queued, served from the queue, written by idle */
    au_script_cache_set_deferred(c, true);
    int w = writes;
    key_of(key, 5);
    s->put(s->ctx, key, blob, 500);
    key_of(key, 6);
    s->put(s->ctx, key, blob, 600);
    CHECK(writes == w && au_script_cache_pending(c) == 1100 + 2 * 48, "queued");
    CHECK(has(s, 5, blob, 500, &seen), "read from the queue");
    CHECK(au_script_cache_idle(c, 1) && writes == w + 1, "one written");
    CHECK(!au_script_cache_idle(c, 1) && writes == w + 2 && au_script_cache_pending(c) == 0, "the other");
    CHECK(has(s, 6, blob, 600, &seen), "read from its file");

    au_script_cache_clear(c);
    CHECK(stats(c).count == 0 && nfiles == 0, "cleared: %d files left", nfiles);
    au_script_cache_free(c);
    files_reset();
}

static void test_index(void)
{
    AuScriptCache *c = au_script_cache_new(NULL, 1 << 20);
    const AuJsCodeStore *s = au_script_cache_store(c);
    bool seen;
    uint8_t b = 0;
    for (int n = 1; n <= 3; n++)
        has(s, n, &b, 1, &seen);
    AuBuf out;
    au_buf_init(&out);
    au_script_cache_index_serialize(c, &out);
    CHECK(out.len == 22 + 3 * 33, "three lines: %lu bytes", (unsigned long)out.len);

    AuScriptCache *d = au_script_cache_new(NULL, 1 << 20);
    CHECK(au_script_cache_index_parse(d, (const char *)out.data, out.len), "parses");
    const AuJsCodeStore *ds = au_script_cache_store(d);
    CHECK(!has(ds, 2, &b, 1, &seen) && seen, "a key of the file was seen");
    CHECK(!has(ds, 7, &b, 1, &seen) && !seen, "another was not");
    AuBuf again;
    au_buf_init(&again);
    au_script_cache_free(d);

    /* every prefix parses; a damaged line is skipped */
    for (size_t n = 0; n <= out.len; n++) {
        d = au_script_cache_new(NULL, 1 << 20);
        bool ok = au_script_cache_index_parse(d, (const char *)out.data, n);
        CHECK(ok == (n >= 22), "prefix %lu", (unsigned long)n);
        CHECK(stats(d).seen == (n < 22 ? 0 : (n - 22 + 1) / 33), "prefix %lu: %lu keys", (unsigned long)n,
              (unsigned long)stats(d).seen);
        au_script_cache_free(d);
    }
    out.data[22 + 5] = 'Z';
    d = au_script_cache_new(NULL, 1 << 20);
    CHECK(au_script_cache_index_parse(d, (const char *)out.data, out.len) && stats(d).seen == 2, "a damaged line");
    au_script_cache_free(d);
    CHECK(!au_script_cache_index_parse(c, "# something else\n", 17), "another header");

    /* more keys than it remembers: the oldest are forgotten */
    for (int n = 10; n < 10 + AU_SCRIPT_CACHE_SEEN + 5; n++) {
        uint8_t key[AU_JS_CODE_KEY];
        memset(key, 0, sizeof key);
        key[0] = (uint8_t)n;
        key[1] = (uint8_t)(n >> 8);
        size_t got;
        au_free(s->get(s->ctx, key, &got, &seen));
    }
    CHECK(stats(c).seen == AU_SCRIPT_CACHE_SEEN, "%lu keys kept", (unsigned long)stats(c).seen);
    au_buf_free(&again);
    au_buf_free(&out);
    au_script_cache_free(c);
}

int main(void)
{
    test_runtime();
    test_no_text();
    test_module();
    test_store();
    test_index();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
