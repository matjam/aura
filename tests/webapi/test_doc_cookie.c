/* document.cookie in a browser window over the fake transport: cookies a
 * response sets and cookies script sets, read back by script; Path,
 * Max-Age, Secure on http (refused), HttpOnly (not readable, not
 * settable by script), SameSite; a same-site frame, a cross-site frame
 * with and without third-party blocking, a srcdoc frame and a sandboxed
 * frame; cookies turned off. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
#include "net/cookies.h"
#include "net/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static AuFontSet *fonts;

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuCookieJar *jar;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
    FakeServer *a, *bs;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }

static void env_init(Env *e, bool block_third_party)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->jar = au_cookie_jar_new();
    au_cookie_jar_set_policy(e->jar, AU_COOKIE_POLICY_ALL, block_third_party);
    au_net_set_cookie_jar(e->net, e->jar);
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    e->b = au_browser_new(fonts, &h);
    e->b->prefs.https_first = false;
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    au_browser_set_fetcher(e->b, "https", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
    e->a = fake_server(&e->f, "a.test", 80);
    e->bs = fake_server(&e->f, "b.test", 80);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    au_cookie_jar_free(e->jar);
    fake_free(&e->f);
}

static void settle(Env *e, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
        if (!au_browser_busy(e->b) && i > 5)
            break;
    }
}

static AuPage *page(Env *e) { return au_bw_active_tab(e->w)->page; }

static char *responses[64];
static int nresponses;

static void answer(FakeServer *s, const char *extra, const char *body)
{
    size_t cap = strlen(body) + strlen(extra) + 256;
    char *b = malloc(cap);
    if (nresponses < 64)
        responses[nresponses++] = b;
    snprintf(b, cap, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%sContent-Length: %u\r\n\r\n%s", extra,
             (unsigned)strlen(body), body);
    fake_add(s, b);
}

static const char *js(Env *e, const char *src)
{
    static AuBuf out;
    au_buf_free(&out);
    au_buf_init(&out);
    au_page_eval(page(e), src, &out);
    settle(e, 50);
    return au_buf_cstr(&out);
}

static void load(Env *e, const char *url)
{
    au_bw_new_tab(e->w, url, true, -1);
    settle(e, 300);
}

static const char *req(FakeServer *s, int i) { return i < s->nreq ? s->requests[i] : ""; }

/* Cookies of the response and of script, in the tab's own document. */
static void test_top_level(void)
{
    Env e;
    env_init(&e, true);
    answer(e.a,
           "Set-Cookie: hdr=1; Max-Age=1800\r\n"
           "Set-Cookie: hid=1; HttpOnly\r\n"
           "Set-Cookie: sec=1; Secure\r\n"
           "Set-Cookie: sub=1; Path=/sub\r\n"
           "Set-Cookie: lax=1; SameSite=Lax\r\n"
           "Set-Cookie: strict=1; SameSite=Strict\r\n"
           "Set-Cookie: none=1; SameSite=None\r\n",
           "<!DOCTYPE html><title>t</title><script>var first=document.cookie</script>");
    load(&e, "http://a.test/");
    /* The bug: this read "" although later requests carried the cookies. */
    const char *r = js(&e, "first");
    CHECK(!strcmp(r, "hdr=1; lax=1; strict=1"), "header cookies as the first script saw them: '%s'", r);
    r = js(&e, "document.cookie");
    CHECK(!strcmp(r, "hdr=1; lax=1; strict=1"), "header cookies: '%s'", r);
    CHECK(!strcmp(js(&e, "navigator.cookieEnabled"), "true"), "cookieEnabled");
    r = js(&e, "document.cookie='a=b';document.cookie='p=1; Path=/sub';document.cookie='m=1; Max-Age=60';"
               "document.cookie='s=1; Secure';document.cookie='h=1; HttpOnly';document.cookie='n=1; SameSite=None';"
               "document.cookie='st=1; SameSite=Strict';document.cookie='gone=1; Max-Age=0';"
               "document.cookie='hid=2';document.cookie");
    CHECK(!strcmp(r, "hdr=1; lax=1; strict=1; a=b; m=1; st=1"), "script cookies: '%s'", r);
    /* a later request carries them, the HttpOnly one (unchanged) included */
    answer(e.a, "", "ok");
    js(&e, "fetch('/echo')");
    settle(&e, 100);
    const char *q = req(e.a, 1);
    CHECK(strstr(q, "hid=1") && strstr(q, "a=b") && strstr(q, "m=1") && !strstr(q, "s=1;") && !strstr(q, "h=1") &&
              !strstr(q, "sec=1") && !strstr(q, "hid=2"),
          "request: %s", q);
    /* expiry by the clock, and deletion by script */
    e.f.wall += 61;
    r = js(&e, "document.cookie='a=; Max-Age=0';document.cookie");
    CHECK(!strcmp(r, "hdr=1; lax=1; strict=1; st=1"), "after a minute and a deletion: '%s'", r);
    /* Path: the cookies of /sub show on a page there */
    answer(e.a, "", "<!DOCTYPE html><title>s</title><script>var first=document.cookie</script>");
    load(&e, "http://a.test/sub/page");
    r = js(&e, "first");
    CHECK(!strcmp(r, "sub=1; p=1; hdr=1; lax=1; strict=1; st=1"), "under /sub: '%s'", r);
    env_free(&e);
}

/* The frames page: a same-site frame, a cross-site frame, a srcdoc frame
 * and a sandboxed frame. Each sets a cookie and reports what it reads. */
static const char *frames_result(Env *e)
{
    answer(e->a, "Set-Cookie: top=1\r\n",
           "<!DOCTYPE html><title>t</title><script>var R={};onmessage=function(ev){R[ev.data[0]]=ev.data[1]}</script>"
           "<iframe src='/f'></iframe><iframe src='http://b.test/f'></iframe>"
           "<iframe srcdoc=\"<script>document.cookie='sd=1';parent.postMessage(['srcdoc',document.cookie],'*')</script>\"></iframe>"
           "<iframe sandbox='allow-scripts' src='/sb'></iframe>");
    answer(e->a, "", "<script>document.cookie='same=1';parent.postMessage(['same',document.cookie],'*')</script>");
    answer(e->a, "", "<script>document.cookie='sb=1';parent.postMessage(['sandbox',document.cookie],'*')</script>");
    answer(e->bs, "Set-Cookie: bh=1\r\n",
           "<script>document.cookie='cross=1';parent.postMessage(['cross',document.cookie],'*')</script>");
    load(e, "http://a.test/");
    settle(e, 200);
    return js(e, "['same','cross','srcdoc','sandbox'].map(function(k){return k+'='+R[k]}).join('|')+'|top='+document.cookie");
}

static void test_frames(void)
{
    Env e;
    env_init(&e, true);
    const char *r = frames_result(&e);
    /* The srcdoc frame needs no request, so its cookie exists before the
     * same-site frame's script runs: check each part. */
    CHECK(!strncmp(r, "same=top=1", 10) && strstr(r, "same=1|cross=") != NULL, "same-site frame: %s", r);
    CHECK(strstr(r, "|cross=|") != NULL, "cross-site frame with third-party blocking: %s", r);
    CHECK(strstr(r, "srcdoc=top=1") && strstr(r, "sd=1"), "srcdoc frame has its parent's cookies: %s", r);
    CHECK(strstr(r, "|sandbox=|") != NULL, "sandboxed frame: %s", r);
    CHECK(strstr(r, "|top=top=1") && strstr(strstr(r, "|top="), "same=1") && !strstr(r, "cross=1") &&
              !strstr(r, "sb=1"),
          "the page: %s", r);
    env_free(&e);

    env_init(&e, false);
    r = frames_result(&e);
    /* SameSite is Lax by default: the header cookie of a cross-site frame
     * response is not stored, and neither is a Lax cookie its script
     * sets; SameSite=None needs Secure, which http cannot give. */
    CHECK(strstr(r, "|cross=|") != NULL, "cross-site frame, Lax cookies: %s", r);
    CHECK(!strstr(strstr(r, "|top="), "cross"), "b.test's cookies stay off a.test: %s", r);
    env_free(&e);
}

static void test_disabled(void)
{
    Env e;
    env_init(&e, true);
    au_cookie_jar_set_policy(e.jar, AU_COOKIE_POLICY_NONE, true);
    answer(e.a, "Set-Cookie: hdr=1\r\n", "<!DOCTYPE html><title>t</title><script>document.cookie='a=b'</script>");
    load(&e, "http://a.test/");
    const char *r = js(&e, "document.cookie");
    CHECK(!strcmp(r, ""), "cookies off: '%s'", r);
    CHECK(au_cookie_jar_count(e.jar) == 0, "nothing stored");
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_top_level();
    test_frames();
    test_disabled();
    au_fontset_free(fonts);
    for (int i = 0; i < nresponses; i++)
        free(responses[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
