/* Streaming bodies and EventSource in a browser window, over the fake
 * transport: a server that sends its body slowly in pieces is read
 * through Response.body as it arrives, XMLHttpRequest reports progress as
 * it arrives, a reader that falls behind pauses the request, and an
 * event stream server is read by EventSource (events, retry,
 * Last-Event-ID, CORS with credentials, a document that goes). Each case
 * loads a page from a.test whose script leaves a result in window.R. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
#include "net/loader.h"

#include <ctype.h>
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
    FakeServer *a, *bs;
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
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    fake_free(&e->f);
}

static AuPage *page(Env *e) { return au_bw_active_tab(e->w)->page; }

static char *responses[64];
static int nresponses;

/* A response: the status line, extra header lines, a body (with a
 * Content-Length unless the extra headers frame it themselves). */
static FakeResponse *answer_n(FakeServer *s, const char *status, const char *extra, const char *body, size_t blen)
{
    size_t cap = blen + strlen(extra) + 256;
    char *b = malloc(cap);
    if (nresponses < 64)
        responses[nresponses++] = b;
    int n;
    if (strstr(extra, "Transfer-Encoding") || strstr(extra, "event-stream"))
        n = snprintf(b, cap, "HTTP/1.1 %s\r\n%s\r\n", status, extra);
    else
        n = snprintf(b, cap, "HTTP/1.1 %s\r\n%sContent-Length: %u\r\n\r\n", status, extra, (unsigned)blen);
    memcpy(b + n, body, blen);
    b[n + (int)blen] = 0;
    FakeResponse *r = fake_add(s, b);
    r->len = (size_t)n + blen;
    return r;
}

static FakeResponse *answer(FakeServer *s, const char *status, const char *extra, const char *body)
{
    return answer_n(s, status, extra, body, strlen(body));
}

static bool result_ready(Env *e)
{
    AuBuf out;
    au_buf_init(&out);
    au_page_eval(page(e), "typeof R=='string'&&R!='pending'", &out);
    bool done = !strcmp(au_buf_cstr(&out), "true");
    au_buf_free(&out);
    return done;
}

/* Runs the event loop, 10 ms a round, until the page's script has left
 * its result. */
static void wait_result(Env *e)
{
    for (int i = 0; i < 6000; i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
        if (i % 10 == 9 && result_ready(e))
            break;
    }
}

static void rounds(Env *e, int n)
{
    for (int i = 0; i < n; i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
    }
}

static const char *js(Env *e, const char *src)
{
    static AuBuf out;
    au_buf_free(&out);
    au_buf_init(&out);
    au_page_eval(page(e), src, &out);
    return au_buf_cstr(&out);
}

/* Loads http://a.test/ whose script is body (an async function body). */
static void load(Env *e, const char *body)
{
    static char html[8192];
    snprintf(html, sizeof html,
             "<!DOCTYPE html><title>t</title><script>var R='pending';"
             "(async function(){%s})().then(function(v){R=String(v)},function(e){R='threw '+e})</script>",
             body);
    /* the page is the next answer, before those the test queued */
    FakeServer *s = e->a;
    answer(s, "200 OK", "Content-Type: text/html\r\n", html);
    FakeResponse pr = s->resp[s->nresp - 1];
    memmove(&s->resp[s->next + 1], &s->resp[s->next], (size_t)(s->nresp - 1 - s->next) * sizeof pr);
    s->resp[s->next] = pr;
    au_bw_new_tab(e->w, "http://a.test/", true, -1);
}

static const char *run(Env *e, const char *body)
{
    load(e, body);
    wait_result(e);
    return js(e, "R");
}

static const char *req(FakeServer *s, int i) { return i < s->nreq ? s->requests[i] : ""; }

/* A header line in a request, names compared without case. */
static bool has_header(const char *request, const char *name, const char *value)
{
    size_t nl = strlen(name);
    for (const char *p = request; (p = strstr(p, "\r\n")) != NULL;) {
        p += 2;
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)name[i]))
            i++;
        if (i == nl && p[i] == ':') {
            const char *v = p + i + 1;
            while (*v == ' ')
                v++;
            if (!value || !strncmp(v, value, strlen(value)))
                return true;
        }
    }
    return false;
}

static char body_1000[1001];

/* Response.body delivers a slow body piece by piece as it arrives, with
 * Content-Length and with chunked framing. */
static void test_slow_body(void)
{
    Env e;
    env_init(&e);
    for (int i = 0; i < 1000; i++)
        body_1000[i] = (char)('a' + i % 26);
    FakeResponse *r = answer(e.a, "200 OK", "Content-Type: text/plain\r\n", body_1000);
    r->chunk = 50;
    r->paced = true;
    /* chunked: ten chunks of 100 bytes */
    static char chunked[2000];
    size_t n = 0;
    for (int i = 0; i < 10; i++) {
        n += (size_t)snprintf(chunked + n, sizeof chunked - n, "64\r\n");
        memcpy(chunked + n, body_1000 + i * 100, 100);
        n += 100;
        n += (size_t)snprintf(chunked + n, sizeof chunked - n, "\r\n");
    }
    n += (size_t)snprintf(chunked + n, sizeof chunked - n, "0\r\n\r\n");
    r = answer_n(e.a, "200 OK", "Content-Type: text/plain\r\nTransfer-Encoding: chunked\r\n", chunked, n);
    r->chunk = 60;
    r->paced = true;
    const char *res = run(&e, "async function read(url){var res=await fetch(url);var rd=res.body.getReader();"
                              "var n=0,s='',first=-1,t0=performance.now();"
                              "for(;;){var x=await rd.read();if(x.done)break;if(first<0)first=performance.now()-t0;"
                              "n++;s+=new TextDecoder().decode(x.value)}"
                              "var all=performance.now()-t0;"
                              "return (n>5)+' '+s.length+' '+(first<all)+' '+(s.charCodeAt(999)-97)}"
                              "return [await read('/slow'),await read('/chunked')].join('|')");
    CHECK(!strcmp(res, "true 1000 true 11|true 1000 true 11"), "%s", res);
    env_free(&e);
}

/* XMLHttpRequest fires progress as the body arrives, and its
 * responseText grows. */
static void test_xhr_progress(void)
{
    Env e;
    env_init(&e);
    for (int i = 0; i < 1000; i++)
        body_1000[i] = (char)('a' + i % 26);
    FakeResponse *r = answer(e.a, "200 OK", "Content-Type: text/plain\r\n", body_1000);
    r->chunk = 20;
    r->paced = true;
    const char *res = run(&e, "return await new Promise(function(done){var x=new XMLHttpRequest();var p=[],partial=0,three=0;"
                              "x.open('GET','/slow');x.onprogress=function(ev){p.push(ev.loaded);"
                              "if(x.responseText.length<1000)partial++;};"
                              "x.onreadystatechange=function(){if(x.readyState==3)three++};"
                              "x.onloadend=function(){done((p.length>3)+' '+(partial>2)+' '+(three>1)+' '+"
                              "p[p.length-1]+' '+x.responseText.length)};x.send()})");
    CHECK(!strcmp(res, "true true true 1000 1000"), "%s", res);
    env_free(&e);
}

/* A reader that stops reading pauses the request (the transfer stays
 * open, long after an unpaused one would have ended); reading again
 * resumes it, and the body is whole. */
static void test_backpressure(void)
{
    Env e;
    env_init(&e);
    size_t big = 1u << 20;
    char *body = malloc(big + 1);
    for (size_t i = 0; i < big; i++)
        body[i] = (char)('a' + i % 26);
    body[big] = 0;
    FakeResponse *r = answer_n(e.a, "200 OK", "Content-Type: application/octet-stream\r\n", body, big);
    r->chunk = 16384;
    load(&e, "var res=await fetch('/big');window.rd=res.body.getReader();var x=await rd.read();"
             "window.got=x.value.length;return 'reading'");
    wait_result(&e);
    CHECK(!strcmp(js(&e, "R"), "reading"), "%s", js(&e, "R"));
    rounds(&e, 3000); /* 30 s */
    CHECK(au_net_active(e.net) == 1, "the paused request is still open: %d", au_net_active(e.net));
    js(&e, "R='pending';(async function(){var n=got,ok=true;for(;;){var x=await rd.read();if(x.done)break;"
           "if(x.value[0]!==97+n%26)ok=false;n+=x.value.length}R=n+' '+ok})()");
    wait_result(&e);
    char want[32];
    snprintf(want, sizeof want, "%lu true", (unsigned long)big);
    CHECK(!strcmp(js(&e, "R"), want), "%s", js(&e, "R"));
    CHECK(au_net_active(e.net) == 0, "done: %d", au_net_active(e.net));
    /* a canceled body ends the request */
    r = answer_n(e.a, "200 OK", "Content-Type: application/octet-stream\r\n", body, big);
    r->chunk = 16384;
    js(&e, "R='pending';fetch('/big2').then(function(res){var rd=res.body.getReader();return rd.read().then(function(){"
           "return rd.cancel()})}).then(function(){R='canceled'})");
    wait_result(&e);
    rounds(&e, 20);
    CHECK(!strcmp(js(&e, "R"), "canceled"), "%s", js(&e, "R"));
    CHECK(au_net_active(e.net) == 0, "a canceled body ends the request: %d", au_net_active(e.net));
    free(body);
    env_free(&e);
}

/* EventSource: events from a slow server, the retry field, a reconnection
 * with Last-Event-ID, close(). */
static void test_eventsource(void)
{
    Env e;
    env_init(&e);
    FakeResponse *r = answer(e.a, "200 OK", "Content-Type: text/event-stream\r\n",
                             ": hello\n\nretry: 200\nid: 1\ndata: a\n\nid: 2\nevent: note\ndata: b\ndata: c\n\n");
    r->chunk = 7;
    r->paced = true;
    r->close_after = true;
    r = answer(e.a, "200 OK", "Content-Type: text/event-stream; charset=utf-8\r\n", "data: d\n\n");
    r->close_after = true;
    const char *res = run(&e, "var es=new EventSource('/events');var got=[],opens=0,errors=0;es.onopen=function(){opens++};"
                              "es.onerror=function(){errors++;got.push('error'+es.readyState)};"
                              "await new Promise(function(done){function on(ev){got.push(ev.type+':'+ev.data+'@'+ev.lastEventId+"
                              "(ev.origin=='http://a.test'?'':' origin '+ev.origin));if(ev.data=='d'){es.close();done()}}"
                              "es.onmessage=on;es.addEventListener('note',on)});"
                              "return got.join(',')+' opens '+opens+' state '+es.readyState");
    CHECK(!strcmp(res, "message:a@1,note:b\nc@2,error0,message:d@2 opens 2 state 2"), "%s", res);
    CHECK(e.a->nreq == 3, "requests: %d", e.a->nreq);
    CHECK(has_header(req(e.a, 1), "Accept", "text/event-stream") && !has_header(req(e.a, 1), "Last-Event-ID", NULL),
          "the first request: %s", req(e.a, 1));
    CHECK(has_header(req(e.a, 2), "Last-Event-ID", "2"), "the reconnection carries the last ID: %s", req(e.a, 2));
    rounds(&e, 100);
    CHECK(e.a->nreq == 3, "no request after close: %d", e.a->nreq);
    env_free(&e);
}

/* A response that is not 200 text/event-stream fails the connection for
 * good; one that ends with a server error status too. */
static void test_eventsource_fail(void)
{
    Env e;
    env_init(&e);
    answer(e.a, "200 OK", "Content-Type: text/plain\r\n", "data: x\n\n");
    answer(e.a, "204 No Content", "", "");
    answer(e.a, "500 Oops", "Content-Type: text/event-stream\r\nContent-Length: 0\r\n", "");
    const char *res = run(&e, "function one(url){return new Promise(function(done){var es=new EventSource(url),opened=0;"
                              "es.onopen=function(){opened++};es.onerror=function(){done(opened+':'+es.readyState)}})}"
                              "return [await one('/plain'),await one('/none'),await one('/error')].join(',')");
    CHECK(!strcmp(res, "0:2,0:2,0:2"), "%s", res);
    rounds(&e, 500);
    CHECK(e.a->nreq == 4, "no reconnection after a failure: %d", e.a->nreq);
    env_free(&e);
}

/* CORS: a cross-origin source needs Access-Control-Allow-Origin, and with
 * withCredentials Access-Control-Allow-Credentials too. */
static void test_eventsource_cors(void)
{
    Env e;
    env_init(&e);
    FakeResponse *r = answer(e.bs, "200 OK",
                             "Content-Type: text/event-stream\r\nAccess-Control-Allow-Origin: http://a.test\r\n"
                             "Access-Control-Allow-Credentials: true\r\n",
                             "data: cred\n\n");
    r->close_after = true;
    r = answer(e.bs, "200 OK", "Content-Type: text/event-stream\r\nAccess-Control-Allow-Origin: http://a.test\r\n",
               "data: nocred\n\n");
    r->close_after = true;
    r = answer(e.bs, "200 OK", "Content-Type: text/event-stream\r\n", "data: denied\n\n");
    r->close_after = true;
    r = answer(e.bs, "200 OK", "Content-Type: text/event-stream\r\nAccess-Control-Allow-Origin: http://a.test\r\n",
               "data: no credentials header\n\n");
    r->close_after = true;
    const char *res = run(&e, "function one(url,init){return new Promise(function(done){var es=new EventSource(url,init);"
                              "es.onmessage=function(ev){es.close();done(ev.data+'@'+ev.origin)};"
                              "es.onerror=function(){if(es.readyState==2)done('failed')}})}"
                              "var a=await one('http://b.test/c',{withCredentials:true});"
                              "var b=await one('http://b.test/n');var c=await one('http://b.test/d');"
                              "var d=await one('http://b.test/x',{withCredentials:true});"
                              "return [a,b,c,d].join(',')");
    CHECK(!strcmp(res, "cred@http://b.test,nocred@http://b.test,failed,failed"), "%s", res);
    CHECK(has_header(req(e.bs, 0), "Origin", "http://a.test"), "Origin: %s", req(e.bs, 0));
    CHECK(e.bs->nreq == 4, "requests: %d", e.bs->nreq);
    env_free(&e);
}

/* A document that goes ends its event streams; Back connects again. */
static void test_eventsource_document_goes(void)
{
    Env e;
    env_init(&e);
    /* an endless stream: a first event, then nothing more */
    answer(e.a, "200 OK", "Content-Type: text/event-stream\r\n", "data: first\n\n");
    const char *res = run(&e, "window.es=new EventSource('/forever');"
                              "return await new Promise(function(done){es.onmessage=function(ev){done(ev.data)}})");
    CHECK(!strcmp(res, "first"), "%s", res);
    rounds(&e, 50);
    CHECK(au_net_active(e.net) == 1, "the stream is open: %d", au_net_active(e.net));
    answer(e.a, "200 OK", "Content-Type: text/html\r\n", "<!DOCTYPE html><title>two</title><p>two");
    au_bw_navigate(e.w, "http://a.test/two");
    rounds(&e, 200);
    CHECK(au_net_active(e.net) == 0, "navigation ends the stream: %d", au_net_active(e.net));
    int before = e.a->nreq;
    answer(e.a, "200 OK", "Content-Type: text/event-stream\r\n", "data: again\n\n");
    answer(e.a, "200 OK", "Content-Type: text/html\r\n", "<!DOCTYPE html><title>t</title><p>reloaded");
    au_bw_back(e.w);
    rounds(&e, 300);
    CHECK(e.a->nreq > before, "back connects again: %d requests, %d before", e.a->nreq, before);
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_slow_body();
    test_xhr_progress();
    test_backpressure();
    test_eventsource();
    test_eventsource_fail();
    test_eventsource_cors();
    test_eventsource_document_goes();
    au_fontset_free(fonts);
    for (int i = 0; i < nresponses; i++)
        free(responses[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
