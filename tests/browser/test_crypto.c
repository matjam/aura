/* crypto for pages: AuBrowserHost.random reaches script as
 * crypto.getRandomValues and crypto.randomUUID. A source that is not
 * ready makes the calls throw; a host without one gives pages no crypto
 * object. */
#include "browser/browser.h"
#include "browser/page.h"
#include "core/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

static uint32_t fake_ms;
static bool ready;
static unsigned calls, bytes_given;
static uint8_t next_byte;

static uint32_t t_ms(void *ctx) { (void)ctx; return fake_ms; }
static AuTime t_now(void *ctx) { (void)ctx; return 1763100000; }
static void t_inval(void *ctx, AuBrowserWindow *w, AuIRect r) { (void)ctx, (void)w, (void)r; }
static void t_changed(void *ctx, AuBrowserWindow *w, unsigned what) { (void)ctx, (void)w, (void)what; }
static void t_close(void *ctx, AuBrowserWindow *w) { (void)ctx, (void)w; }

static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    if (strcmp(url, "file:///crypto.html") != 0)
        return false;
    static const char html[] = "<!doctype html><title>crypto</title><p id=out>x</p><script>var loaded = 1;</script>";
    *data = au_strndup(html, sizeof html - 1);
    *len = sizeof html - 1;
    return true;
}

/* A counting source: the test can tell its bytes from any others. */
static bool t_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    calls++;
    if (!ready)
        return false;
    for (size_t i = 0; i < len; i++)
        ((uint8_t *)buf)[i] = next_byte++;
    bytes_given += (unsigned)len;
    return true;
}

static AuBrowserWindow *open_page(AuBrowser *b)
{
    AuBrowserWindow *w = au_browser_window_new(b, 640, 480);
    au_bw_new_tab(w, "file:///crypto.html", true, -1);
    for (int i = 0; i < 200 && au_browser_busy(b); i++) {
        fake_ms += 50;
        au_bw_idle(w);
    }
    return w;
}

static const char *eval(AuBrowserWindow *w, const char *src, AuBuf *out)
{
    au_buf_clear(out);
    AuTab *t = au_bw_active_tab(w);
    if (!t || !au_page_eval(t->page, src, out))
        au_buf_puts(out, " (failed)");
    return au_buf_cstr(out);
}

int main(void)
{
    AuFontSet *fonts = au_fontset_new();
    AuBuf out;
    au_buf_init(&out);

    /* A host with a random source. */
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.time_ms = t_ms;
    h.now = t_now;
    h.read_file_url = t_read;
    h.invalidate = t_inval;
    h.changed = t_changed;
    h.close_window = t_close;
    h.random = t_random;
    AuBrowser *b = au_browser_new(fonts, &h);
    ready = true;
    AuBrowserWindow *w = open_page(b);
    CHECK(!strcmp(eval(w, "String(loaded)", &out), "1"), "the page's script ran: %s", out.data);
    CHECK(!strcmp(eval(w, "typeof crypto + ' ' + typeof crypto.getRandomValues + ' ' + typeof crypto.randomUUID", &out),
                  "object function function"),
          "crypto exists: %s", out.data);
    next_byte = 7;
    bytes_given = 0;
    CHECK(!strcmp(eval(w, "Array.from(crypto.getRandomValues(new Uint8Array(4))).join()", &out), "7,8,9,10"),
          "the bytes are the host's: %s", out.data);
    CHECK(bytes_given == 4, "4 bytes asked for: %u", bytes_given);
    next_byte = 0;
    CHECK(!strcmp(eval(w, "crypto.randomUUID()", &out), "00010203-0405-4607-8809-0a0b0c0d0e0f"), "randomUUID: %s",
          out.data);
    CHECK(!strcmp(eval(w, "(()=>{const a=new Uint32Array(2);return crypto.getRandomValues(a)===a&&a.byteLength})()", &out),
                  "8"),
          "other integer arrays: %s", out.data);
    CHECK(!strcmp(eval(w, "(()=>{try{crypto.getRandomValues(new Float32Array(1));}catch(e){return e.name}})()", &out),
                  "TypeMismatchError"),
          "float arrays are refused: %s", out.data);
    CHECK(!strcmp(eval(w, "(()=>{try{crypto.getRandomValues(new Uint8Array(65537));}catch(e){return e.name}})()", &out),
                  "QuotaExceededError"),
          "the size limit: %s", out.data);

    /* The source is not ready (the Mac OS 9 pool before it has gathered
     * 128 bits): the calls throw and the array is left as it was. */
    ready = false;
    calls = 0;
    CHECK(!strcmp(eval(w,
                       "(()=>{const a=new Uint8Array([1,2,3]);try{crypto.getRandomValues(a);return 'returned '+a.join()}"
                       "catch(e){return e.name+' '+a.join()}})()",
                       &out),
                  "OperationError 1,2,3"),
          "not ready: getRandomValues throws and writes nothing: %s", out.data);
    CHECK(calls >= 1, "the host was asked");
    CHECK(!strcmp(eval(w, "(()=>{try{return crypto.randomUUID()}catch(e){return e.name}})()", &out), "OperationError"),
          "not ready: randomUUID throws: %s", out.data);
    /* Ready again: it works without a reload. */
    ready = true;
    next_byte = 100;
    CHECK(!strcmp(eval(w, "String(crypto.getRandomValues(new Uint8Array(1))[0])", &out), "100"), "ready again: %s",
          out.data);
    au_browser_window_free(w);
    au_browser_free(b);

    /* A page that loads while the source is not ready still gets the
     * crypto object, and its calls throw until the source is ready. */
    b = au_browser_new(fonts, &h);
    ready = false;
    w = open_page(b);
    CHECK(!strcmp(eval(w, "typeof crypto", &out), "object"), "crypto exists before the source is ready: %s", out.data);
    CHECK(!strcmp(eval(w, "(()=>{try{return crypto.randomUUID()}catch(e){return e.name}})()", &out), "OperationError"),
          "and throws: %s", out.data);
    ready = true;
    CHECK(strlen(eval(w, "crypto.randomUUID()", &out)) == 36, "then works: %s", out.data);
    au_browser_window_free(w);
    au_browser_free(b);

    /* A host without a source: no crypto object. */
    h.random = NULL;
    b = au_browser_new(fonts, &h);
    w = open_page(b);
    CHECK(!strcmp(eval(w, "typeof crypto", &out), "undefined"), "no source, no crypto: %s", out.data);
    au_browser_window_free(w);
    au_browser_free(b);

    au_buf_free(&out);
    au_fontset_free(fonts);
    printf("browser crypto: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
