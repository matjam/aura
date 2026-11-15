/* Script requests in a browser window, over the fake transport: what
 * fetch and XMLHttpRequest send, and what a page of one origin may read
 * of another. Each case loads a page from a.test whose script makes
 * requests and leaves a result in window.R; the servers' request logs
 * show what went out. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
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
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
    FakeServer *a, *bs, *c;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }

static void env_init(Env *e)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
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
    e->c = fake_server(&e->f, "c.test", 80);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
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

static char *responses[256];
static int nresponses;

/* A response: the status line, extra header lines, a body. */
static void answer(FakeServer *s, const char *status, const char *extra, const char *body)
{
    size_t cap = strlen(body) + strlen(extra) + 256;
    char *b = malloc(cap);
    if (nresponses < 256)
        responses[nresponses++] = b;
    snprintf(b, cap, "HTTP/1.1 %s\r\n%sContent-Length: %u\r\n\r\n%s", status, extra, (unsigned)strlen(body), body);
    fake_add(s, b);
}

/* Runs the event loop until the page's script has left its result. */
static void wait_result(Env *e)
{
    for (int i = 0; i < 3000; i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
        if (i % 20 == 19) {
            AuBuf out;
            au_buf_init(&out);
            au_page_eval(page(e), "typeof R=='string'&&R!='pending'", &out);
            bool done = !strcmp(au_buf_cstr(&out), "true");
            au_buf_free(&out);
            if (done)
                break;
        }
    }
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

static bool console_has(Env *e, const char *needle)
{
    for (size_t i = 0; i < au_page_console_count(page(e)); i++)
        if (strstr(au_page_console_line(page(e), i, NULL), needle))
            return true;
    return false;
}

/* Loads http://a.test/ whose script is body (an async function body that
 * may use t(), which fetches and describes the outcome) and returns
 * window.R. */
static const char *run(Env *e, const char *body)
{
    static char html[8192];
    snprintf(html, sizeof html,
             "<!DOCTYPE html><title>t</title><script>var R='pending';"
             "async function t(url,init){try{var r=await fetch(url,init);var x=await r.text();"
             "return r.type+' '+r.status+' ['+x+']'+(r.redirected?' redirected':'')}catch(e){return e.name}}"
             "function xhr(method,url,setup,body){return new Promise(function(done){var x=new XMLHttpRequest();"
             "x.open(method,url);if(setup)setup(x);x.onloadend=function(){done(x)};x.send(body)})}"
             "(async function(){%s})().then(function(v){R=String(v)},function(e){R='threw '+e})</script>",
             body);
    answer(e->a, "200 OK", "Content-Type: text/html\r\n", html);
    au_bw_new_tab(e->w, "http://a.test/", true, -1);
    wait_result(e);
    return js(e, "R");
}

static const char *req(FakeServer *s, int i) { return i < s->nreq ? s->requests[i] : ""; }

#define ACAO "Access-Control-Allow-Origin: http://a.test\r\n"

/* What goes out: methods, headers, bodies, to the page's own origin. */
static void test_same_origin(void)
{
    Env e;
    env_init(&e);
    const char *body = "var h=new Headers({'X-Custom':'1','Cookie':'no','Sec-Evil':'x'});"
                       "var a=await t('/one',{method:'DELETE',headers:h});"
                       "var b=await t('/two',{method:'PUT',body:'{\"k\":1}',headers:{'Content-Type':'application/json'}});"
                       "var c=await t('/three',{method:'POST',body:new URLSearchParams({q:'a b'})});"
                       "var fd=new FormData();fd.append('f',new File(['data'],'n.txt',{type:'text/plain'}));"
                       "var d=await t('/four',{method:'POST',body:fd});"
                       "var x=await xhr('PATCH','/five',function(x){x.setRequestHeader('X-Req','v');"
                       "x.setRequestHeader('X-Req','w');x.setRequestHeader('Referer','no')},new Uint8Array([65,66]));"
                       "var bad='';try{await fetch('/six',{method:'TRACE'})}catch(err){bad=err.name}"
                       "return [a,b,c,d,x.status+' '+x.getResponseHeader('X-Out')+' '+x.responseText,bad].join('|')";
    /* a.test answers in order: the page, then the five requests */
    static char page_html[8192];
    snprintf(page_html, sizeof page_html,
             "<!DOCTYPE html><title>t</title><script>var R='pending';"
             "async function t(url,init){try{var r=await fetch(url,init);var x=await r.text();"
             "return r.type+' '+r.status+' ['+x+']'}catch(e){return e.name}}"
             "function xhr(method,url,setup,body){return new Promise(function(done){var x=new XMLHttpRequest();"
             "x.open(method,url);if(setup)setup(x);x.onloadend=function(){done(x)};x.send(body)})}"
             "(async function(){%s})().then(function(v){R=String(v)},function(e){R='threw '+e})</script>",
             body);
    answer(e.a, "200 OK", "Content-Type: text/html\r\n", page_html);
    answer(e.a, "200 OK", "Set-Cookie: s=1\r\nX-Out: o\r\n", "one");
    answer(e.a, "201 Created", "", "two");
    answer(e.a, "200 OK", "", "three");
    answer(e.a, "200 OK", "", "four");
    answer(e.a, "200 OK", "X-Out: five\r\n", "5!");
    au_bw_new_tab(e.w, "http://a.test/", true, -1);
    wait_result(&e);
    const char *r = js(&e, "R");
    CHECK(!strcmp(r, "basic 200 [one]|basic 201 [two]|basic 200 [three]|basic 200 [four]|200 five 5!|TypeError"), "%s", r);
    CHECK(e.a->nreq == 6, "requests: %d", e.a->nreq);
    CHECK(!strncmp(req(e.a, 1), "DELETE /one ", 12) && strstr(req(e.a, 1), "x-custom: 1"), "%s", req(e.a, 1));
    CHECK(!strstr(req(e.a, 1), "Cookie: no") && !strstr(req(e.a, 1), "ec-Evil") && !strstr(req(e.a, 1), "ec-evil"),
          "forbidden headers were sent: %s", req(e.a, 1));
    CHECK(strstr(req(e.a, 1), "Origin: http://a.test"), "a DELETE carries Origin: %s", req(e.a, 1));
    CHECK(!strncmp(req(e.a, 2), "PUT /two ", 9) && strstr(req(e.a, 2), "application/json") && strstr(req(e.a, 2), "{\"k\":1}"),
          "%s", req(e.a, 2));
    CHECK(strstr(req(e.a, 3), "application/x-www-form-urlencoded;charset=UTF-8") && strstr(req(e.a, 3), "\r\n\r\nq=a+b"), "%s",
          req(e.a, 3));
    CHECK(strstr(req(e.a, 4), "multipart/form-data; boundary=") && strstr(req(e.a, 4), "filename=\"n.txt\"") &&
              strstr(req(e.a, 4), "Content-Type: text/plain\r\n\r\ndata\r\n"),
          "%s", req(e.a, 4));
    CHECK(!strncmp(req(e.a, 5), "PATCH /five ", 12) && strstr(req(e.a, 5), "X-Req: v, w") && strstr(req(e.a, 5), "\r\n\r\nAB") &&
              !strstr(req(e.a, 5), "Referer: no"),
          "%s", req(e.a, 5));
    env_free(&e);
}

/* A response of another origin without CORS headers cannot be read. */
static void test_unreadable(void)
{
    Env e;
    env_init(&e);
    answer(e.bs, "200 OK", "Content-Type: text/plain\r\n", "secret");
    answer(e.bs, "200 OK", "Content-Type: text/plain\r\n", "secret");
    answer(e.bs, "200 OK", "Content-Type: text/plain\r\n", "secret");
    const char *r = run(&e, "var a=await t('http://b.test/x');"
                            "var x=await xhr('GET','http://b.test/y');"
                            "var n=await t('http://b.test/z',{mode:'no-cors'});"
                            "var s=await t('http://b.test/w',{mode:'same-origin'});"
                            "return [a,x.status+':'+x.responseText+':'+x.getAllResponseHeaders().length+':'+x.readyState,n,s].join('|')");
    CHECK(!strcmp(r, "TypeError|0::0:4|opaque 0 []|TypeError"), "%s", r);
    CHECK(e.bs->nreq == 3, "mode same-origin sent nothing across origins: %d requests", e.bs->nreq);
    CHECK(strstr(req(e.bs, 0), "Origin: http://a.test") && strstr(req(e.bs, 0), "Sec-Fetch-Mode: cors") == NULL,
          "(http is not trustworthy: no Sec-Fetch) %s", req(e.bs, 0));
    CHECK(strstr(req(e.bs, 1), "Origin: http://a.test"), "XMLHttpRequest sends Origin: %s", req(e.bs, 1));
    CHECK(!strstr(req(e.bs, 2), "Origin:"), "a no-cors GET sends no Origin: %s", req(e.bs, 2));
    CHECK(console_has(&e, "Access-Control-Allow-Origin"), "the console names the refusal");
    env_free(&e);
}

/* The CORS check, credentials and the headers a cors response shows. */
static void test_cors_check(void)
{
    Env e;
    env_init(&e);
    answer(e.bs, "200 OK", "Access-Control-Allow-Origin: *\r\nX-Secret: s\r\nContent-Type: text/x-a\r\n", "open");
    answer(e.bs, "200 OK", ACAO "Access-Control-Expose-Headers: X-Shown\r\nX-Shown: yes\r\nX-Hidden: no\r\nSet-Cookie: k=v\r\n",
           "named");
    answer(e.bs, "200 OK", "Access-Control-Allow-Origin: http://other.test\r\n", "wrong");
    answer(e.bs, "200 OK", "Access-Control-Allow-Origin: *\r\n", "star-with-credentials");
    answer(e.bs, "200 OK", ACAO, "no-allow-credentials");
    answer(e.bs, "200 OK", ACAO "Access-Control-Allow-Credentials: true\r\n", "with-credentials");
    answer(e.bs, "200 OK", ACAO "Access-Control-Allow-Credentials: true\r\nX-Out: 1\r\n", "xhr-credentials");
    const char *r = run(&e, "var r1=await fetch('http://b.test/1');var h1=r1.headers.get('x-secret')+','+r1.headers.get('content-type');"
                            "var a=r1.type+' '+await r1.text()+' '+h1;"
                            "var r2=await fetch('http://b.test/2');"
                            "var b=await r2.text()+' '+r2.headers.get('x-shown')+','+r2.headers.get('x-hidden')+','+r2.headers.get('set-cookie');"
                            "var c=await t('http://b.test/3');"
                            "var d=await t('http://b.test/4',{credentials:'include'});"
                            "var f=await t('http://b.test/5',{credentials:'include'});"
                            "var g=await t('http://b.test/6',{credentials:'include'});"
                            "var x=await xhr('GET','http://b.test/7',function(x){x.withCredentials=true});"
                            "return [a,b,c,d,f,g,x.status+' '+x.responseText+' '+x.getResponseHeader('x-out')].join('|')");
    CHECK(!strcmp(r, "cors open null,text/x-a|named yes,null,null|TypeError|TypeError|TypeError|cors 200 [with-credentials]|"
                     "200 xhr-credentials null"),
          "%s", r);
    CHECK(e.bs->nreq == 7, "requests: %d", e.bs->nreq);
    env_free(&e);
}

/* Preflight: the OPTIONS request, the cache, and a refusal that sends no
 * actual request. */
static void test_preflight(void)
{
    Env e;
    env_init(&e);
    const char *allow = ACAO "Access-Control-Allow-Methods: PUT\r\nAccess-Control-Allow-Headers: X-A, Content-Type\r\n"
                             "Access-Control-Max-Age: 60\r\n";
    answer(e.bs, "204 No Content", allow, "");
    answer(e.bs, "200 OK", ACAO, "put-done");
    answer(e.bs, "200 OK", ACAO, "put-again"); /* the cache: no second preflight */
    answer(e.bs, "200 OK", ACAO, "");          /* a preflight that allows nothing */
    answer(e.bs, "404 Not Found", ACAO, "");   /* a preflight with an error status */
    answer(e.bs, "200 OK", "", "");            /* a preflight without CORS headers */
    answer(e.bs, "200 OK", ACAO, "simple");    /* a simple POST needs none */
    answer(e.bs, "200 OK", ACAO "Access-Control-Allow-Methods: PUT\r\n", ""); /* XMLHttpRequest with an upload listener */
    answer(e.bs, "200 OK", ACAO, "uploaded");
    const char *r = run(&e, "var init={method:'PUT',headers:{'X-A':'1','Content-Type':'application/json'},body:'{}'};"
                            "var a=await t('http://b.test/p',init);var b=await t('http://b.test/p',init);"
                            "var c=await t('http://b.test/q',{method:'DELETE'});"
                            "var d=await t('http://b.test/r',{method:'PUT',body:'x'});"
                            "var f=await t('http://b.test/s',{headers:{'X-B':'1'}});"
                            "var g=await t('http://b.test/t',{method:'POST',body:'plain'});"
                            "var x=await xhr('PUT','http://b.test/u',function(x){x.upload.onprogress=function(){}},'up');"
                            "return [a,b,c,d,f,g,x.status+' '+x.responseText].join('|')");
    CHECK(!strcmp(r, "cors 200 [put-done]|cors 200 [put-again]|TypeError|TypeError|TypeError|cors 200 [simple]|200 uploaded"), "%s",
          r);
    CHECK(e.bs->nreq == 9, "requests: %d", e.bs->nreq);
    const char *p = req(e.bs, 0);
    CHECK(!strncmp(p, "OPTIONS /p ", 11) && strstr(p, "Access-Control-Request-Method: PUT") &&
              strstr(p, "Access-Control-Request-Headers: content-type,x-a") && strstr(p, "Origin: http://a.test") &&
              !strstr(p, "X-A: 1") && !strstr(p, "x-a: 1") && !strstr(p, "Cookie:"),
          "the preflight: %s", p);
    CHECK(!strncmp(req(e.bs, 1), "PUT /p ", 7) && strstr(req(e.bs, 1), "x-a: 1") && strstr(req(e.bs, 1), "\r\n\r\n{}"), "%s",
          req(e.bs, 1));
    CHECK(!strncmp(req(e.bs, 2), "PUT /p ", 7), "the second PUT used the preflight cache: %s", req(e.bs, 2));
    CHECK(!strncmp(req(e.bs, 3), "OPTIONS /q ", 11) && !strncmp(req(e.bs, 4), "OPTIONS /r ", 11) &&
              !strncmp(req(e.bs, 5), "OPTIONS /s ", 11),
          "refused preflights are followed by no request: %s / %s / %s", req(e.bs, 3), req(e.bs, 4), req(e.bs, 5));
    CHECK(!strncmp(req(e.bs, 6), "POST /t ", 8), "a simple POST goes out without a preflight: %s", req(e.bs, 6));
    CHECK(!strncmp(req(e.bs, 7), "OPTIONS /u ", 11) && !strncmp(req(e.bs, 8), "PUT /u ", 7), "upload listeners force a preflight");
    env_free(&e);
}

/* Redirects are followed in the engine, each hop checked. */
static void test_redirects(void)
{
    Env e;
    env_init(&e);
    /* a.test: the page, then its redirects */
    static char page_html[4096];
    snprintf(page_html, sizeof page_html, "%s",
             "<!DOCTYPE html><script>var R='pending';"
             "async function t(url,init){try{var r=await fetch(url,init);var x=await r.text();"
             "return r.type+' '+r.status+' ['+x+']'+(r.redirected?' redirected ':' ')+r.url}catch(e){return e.name}}"
             "(async function(){"
             "var a=await t('/r1');var b=await t('/r2');var c=await t('/r3');var d=await t('/r4',{mode:'same-origin'});"
             "var f=await t('/r5',{redirect:'error'});var g=await t('/r6',{redirect:'manual'});"
             "var h=await t('/r7',{method:'POST',body:'p'});var i=await t('/r8',{mode:'no-cors'});"
             "return [a,b,c,d,f,g,h,i].join('|')})().then(function(v){R=String(v)},function(e){R='threw '+e})</script>");
    answer(e.a, "200 OK", "Content-Type: text/html\r\n", page_html);
    answer(e.a, "302 Found", "Location: /same\r\n", "");
    answer(e.a, "200 OK", "", "same");
    answer(e.a, "302 Found", "Location: http://b.test/closed\r\n", "");
    answer(e.a, "307 Temporary Redirect", "Location: http://b.test/open\r\n", "");
    answer(e.a, "302 Found", "Location: http://b.test/never\r\n", "");
    answer(e.a, "302 Found", "Location: /x\r\n", "");
    answer(e.a, "302 Found", "Location: /y\r\n", "body-of-redirect");
    answer(e.a, "303 See Other", "Location: /after-post\r\n", "");
    answer(e.a, "200 OK", "", "got");
    answer(e.a, "301 Moved", "Location: http://b.test/opaque\r\n", "");
    /* b.test */
    answer(e.bs, "200 OK", "", "secret");
    answer(e.bs, "302 Found", ACAO "Location: http://c.test/final\r\n", "");
    answer(e.bs, "200 OK", "", "hidden");
    /* c.test: reached from b.test, so its request names the origin "null" */
    answer(e.c, "200 OK", "Access-Control-Allow-Origin: null\r\n", "tainted");
    au_bw_new_tab(e.w, "http://a.test/", true, -1);
    wait_result(&e);
    const char *r = js(&e, "R");
    CHECK(!strcmp(r, "basic 200 [same] redirected http://a.test/same|TypeError|"
                     "cors 200 [tainted] redirected http://c.test/final|TypeError|TypeError|opaqueredirect 0 [] |"
                     "basic 200 [got] redirected http://a.test/after-post|opaque 0 [] "),
          "%s", r);
    CHECK(e.bs->nreq == 3, "b.test requests: %d (same-origin mode stopped at the redirect)", e.bs->nreq);
    CHECK(strstr(req(e.bs, 0), "Origin: http://a.test"), "the redirected request is a CORS request: %s", req(e.bs, 0));
    CHECK(e.c->nreq == 1 && strstr(req(e.c, 0), "Origin: null"), "a second cross-origin hop sends Origin: null: %s", req(e.c, 0));
    bool post_became_get = false;
    for (int i = 0; i < e.a->nreq; i++)
        if (!strncmp(req(e.a, i), "GET /after-post ", 16) && !strstr(req(e.a, i), "\r\n\r\np"))
            post_became_get = true;
    CHECK(post_became_get, "303 turned the POST into a GET without its body");
    env_free(&e);
}

/* Cross-Origin-Resource-Policy on no-cors responses; crossorigin on
 * script elements; muted errors of a cross-origin classic script. */
static void test_elements(void)
{
    Env e;
    env_init(&e);
    answer(e.a, "200 OK", "Content-Type: text/html\r\n",
           "<!DOCTYPE html><script>var log=[];window.onerror=function(m,f,l,c,err){log.push(m+'@'+f+':'+l+':'+(err===null));"
           "return true}</script>"
           "<script src='http://b.test/plain.js'></script>"
           "<script src='http://b.test/cors-denied.js' crossorigin onerror=\"log.push('denied')\"></script>"
           "<script src='http://b.test/cors-ok.js' crossorigin='anonymous'></script>"
           "<script src='http://b.test/corp.js' onerror=\"log.push('corp')\"></script>"
           "<script>var R='pending';fetch('http://b.test/corp-fetch',{mode:'no-cors'}).then(function(r){R='read '+r.type},"
           "function(e){R=e.name})</script>");
    answer(e.bs, "200 OK", "Content-Type: text/javascript\r\n", "window.plain=1;setTimeout(function(){throw new Error('secret detail')},0);");
    answer(e.bs, "200 OK", "Content-Type: text/javascript\r\n", "window.denied=1;");
    answer(e.bs, "200 OK", ACAO "Content-Type: text/javascript\r\n",
           "window.ok=1;setTimeout(function(){throw new Error('visible detail')},0);");
    answer(e.bs, "200 OK", "Cross-Origin-Resource-Policy: same-origin\r\nContent-Type: text/javascript\r\n", "window.corp=1;");
    answer(e.bs, "200 OK", "Cross-Origin-Resource-Policy: same-site\r\n", "x");
    au_bw_new_tab(e.w, "http://a.test/", true, -1);
    wait_result(&e);
    const char *r = js(&e, "[window.plain,window.denied,window.ok,window.corp,R].join()");
    CHECK(!strcmp(r, "1,,1,,TypeError"), "%s", r);
    r = js(&e, "log.join('|')");
    CHECK(strstr(r, "denied") && strstr(r, "corp"), "error events: %s", r);
    CHECK(strstr(r, "Script error.@:0:true"), "a cross-origin script's error is muted: %s", r);
    CHECK(!strstr(r, "secret detail"), "the muted error leaked: %s", r);
    CHECK(strstr(r, "visible detail@http://b.test/cors-ok.js"), "a CORS script's error is reported in full: %s", r);
    CHECK(strstr(req(e.bs, 1), "Origin: http://a.test") && !strstr(req(e.bs, 0), "Origin:"),
          "a crossorigin script is requested with Origin: %s", req(e.bs, 1));
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_same_origin();
    test_unreadable();
    test_cors_check();
    test_preflight();
    test_redirects();
    test_elements();
    au_fontset_free(fonts);
    for (int i = 0; i < nresponses; i++)
        free(responses[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
