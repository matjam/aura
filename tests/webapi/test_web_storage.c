/* localStorage and sessionStorage in a browser window over the fake
 * transport, and the storage service's files: persistence across loads
 * and across a restart; isolation between origins and between http and
 * https; two tabs sharing an area, with the storage event in the other
 * tab only; sessionStorage across navigation, Back, an opened and a
 * duplicated tab, a closed and reopened tab and the session file;
 * third-party frames (partitioned, session only under the cookie
 * preference); quotas; strings with lone surrogates and NUL; named
 * properties and enumeration; SecurityError; damaged and truncated
 * files; clearing with cookies; document.referrer. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
#include "net/cookie_view.h"
#include "net/cookies.h"
#include "net/loader.h"
#include "net/roots.h"
#include "net/web_storage.h"

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

#define ROOTS_DATE 1700000000LL

static AuFontSet *fonts;

/* ---- The storage folder, in memory ---- */

typedef struct MemFiles {
    struct {
        char name[40];
        char *d;
        size_t n;
    } f[64];
    int n;
    int reads, writes, removes, damaged;
    bool fail_writes;
} MemFiles;

static int mf_find(MemFiles *m, const char *name)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->f[i].name, name))
            return i;
    return -1;
}

static bool mf_read(void *ctx, const char *name, AuBuf *out)
{
    MemFiles *m = ctx;
    m->reads++;
    int i = mf_find(m, name);
    if (i < 0)
        return false;
    au_buf_append(out, m->f[i].d, m->f[i].n);
    return true;
}

static bool mf_write(void *ctx, const char *name, const void *data, size_t len)
{
    MemFiles *m = ctx;
    if (m->fail_writes)
        return false;
    m->writes++;
    int i = mf_find(m, name);
    if (i < 0) {
        if (m->n == 64 || strlen(name) >= sizeof m->f[0].name)
            return false;
        i = m->n++;
        snprintf(m->f[i].name, sizeof m->f[i].name, "%s", name);
        m->f[i].d = NULL;
    }
    free(m->f[i].d);
    m->f[i].d = malloc(len ? len : 1);
    memcpy(m->f[i].d, data, len);
    m->f[i].n = len;
    return true;
}

static void mf_remove(void *ctx, const char *name)
{
    MemFiles *m = ctx;
    int i = mf_find(m, name);
    if (i < 0)
        return;
    m->removes++;
    free(m->f[i].d);
    m->f[i] = m->f[--m->n];
}

static void mf_damaged(void *ctx, const char *name)
{
    MemFiles *m = ctx;
    m->damaged++;
    mf_remove(ctx, name);
}

static void mf_free(MemFiles *m)
{
    for (int i = 0; i < m->n; i++)
        free(m->f[i].d);
    memset(m, 0, sizeof *m);
}

/* The first area file (any file but the index). */
static int mf_area(MemFiles *m)
{
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->f[i].name, "Index"))
            return i;
    return -1;
}

/* ---- The browser ---- */

typedef struct Env {
    FakeNet f;
    AuNet *net;
    AuCookieJar *jar;
    AuRootStore *roots;
    AuWebStorage *ws;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
    FakeServer *a, *bs, *c, *tls, *tls80;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }

static void env_init(Env *e, MemFiles *files)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->jar = au_cookie_jar_new();
    au_cookie_jar_set_policy(e->jar, AU_COOKIE_POLICY_ALL, true);
    au_net_set_cookie_jar(e->net, e->jar);
    e->roots = au_roots_new(ROOTS_DATE);
    au_roots_add_der(e->roots, tls_fixtures[TLS_FIX_CA].cert.data, tls_fixtures[TLS_FIX_CA].cert.data_len);
    au_net_set_roots(e->net, e->roots);
    e->ws = au_web_storage_new();
    au_web_storage_set_cookie_jar(e->ws, e->jar);
    if (files) {
        AuWebStorageFiles wf = {files, mf_read, mf_write, mf_remove, mf_damaged};
        au_web_storage_set_files(e->ws, &wf);
    }
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    e->b = au_browser_new(fonts, &h);
    e->b->prefs.https_first = false;
    au_browser_set_storage(e->b, e->ws);
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    au_browser_set_fetcher(e->b, "https", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
    e->a = fake_server(&e->f, "a.test", 80);
    e->bs = fake_server(&e->f, "b.test", 80);
    e->c = fake_server(&e->f, "c.test", 80);
    e->tls80 = fake_server(&e->f, "tls.test", 80);
    e->tls = fake_server(&e->f, "tls.test", 443);
    e->tls->tls = true;
    e->tls->chain = &tls_fixtures[TLS_FIX_VALID].cert;
    e->tls->chain_len = 1;
    e->tls->key = &tls_fixtures[TLS_FIX_VALID].key;
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_web_storage_free(e->ws);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    au_roots_free(e->roots);
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

static char *responses[256];
static int nresponses;

static void answer(FakeServer *s, const char *extra, const char *body)
{
    size_t cap = strlen(body) + strlen(extra) + 256;
    char *b = malloc(cap);
    if (nresponses < 256)
        responses[nresponses++] = b;
    snprintf(b, cap, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%sContent-Length: %u\r\n\r\n%s", extra,
             (unsigned)strlen(body), body);
    fake_add(s, b);
}

/* A page that records the storage events it gets. */
#define PAGE                                                                                                           \
    "<!DOCTYPE html><title>t</title><script>var ev=[];onstorage=function(e){ev.push([e.key,e.oldValue,e.newValue,"   \
    "e.url,e.storageArea===localStorage?'L':e.storageArea===sessionStorage?'S':'?'].join(','))}</script>"

/* Evaluates src in tab i. */
static const char *jst(Env *e, int tab, const char *src)
{
    static AuBuf out;
    au_buf_free(&out);
    au_buf_init(&out);
    au_page_eval(e->w->tabs[tab]->page, src, &out);
    settle(e, 50);
    return au_buf_cstr(&out);
}

static const char *js(Env *e, const char *src) { return jst(e, e->w->active, src); }

static void load(Env *e, const char *url)
{
    au_bw_new_tab(e->w, url, true, -1);
    settle(e, 300);
}

static void go(Env *e, const char *url)
{
    au_bw_navigate(e->w, url);
    settle(e, 300);
}

#define EQ(expr, want, what)                                                                                           \
    do {                                                                                                               \
        const char *r_ = (expr);                                                                                       \
        CHECK(!strcmp(r_, (want)), "%s: '%s' (want '%s')", (what), r_, (want));                                        \
    } while (0)

/* ---- The map ---- */

static void test_map(void)
{
    AuStorageMap *m = au_storage_map_new();
    char k[32], v[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof k, "key%d", i);
        snprintf(v, sizeof v, "value%d", i);
        CHECK(au_storage_map_set(m, k, strlen(k), v, strlen(v)), "set %d", i);
    }
    CHECK(au_storage_map_count(m) == 1000, "1000 items");
    CHECK(!au_storage_map_set(m, "key5", 4, "value5", 6), "the same value changes nothing");
    /* Remove two of every three: holes, a rebuild, and the order kept. */
    for (int i = 0; i < 1000; i++)
        if (i % 3) {
            snprintf(k, sizeof k, "key%d", i);
            CHECK(au_storage_map_remove(m, k, strlen(k)), "remove %d", i);
        }
    CHECK(au_storage_map_count(m) == 334, "334 left: %u", au_storage_map_count(m));
    bool order = true;
    for (uint32_t i = 0; i < 334; i++) {
        const char *p;
        size_t n;
        snprintf(k, sizeof k, "key%u", i * 3);
        if (!au_storage_map_key(m, i, &p, &n) || n != strlen(k) || memcmp(p, k, n))
            order = false;
    }
    CHECK(order, "insertion order after removals");
    const char *p;
    size_t n;
    CHECK(!au_storage_map_key(m, 334, &p, &n), "key past the end");
    CHECK(au_storage_map_key(m, 100, &p, &n) && n == 6 && !memcmp(p, "key300", 6), "key(100) out of order");
    CHECK(au_storage_map_get(m, "key999", 6, &p, &n) && n == 8 && !memcmp(p, "value999", 8), "get");
    CHECK(!au_storage_map_get(m, "key998", 6, &p, &n), "a removed key is gone");
    /* Sizes: two bytes per UTF-16 unit; U+1F600 is two units, a lone
     * surrogate (three bytes) one, a NUL one. */
    au_storage_map_clear(m);
    CHECK(au_storage_map_bytes(m) == 0 && au_storage_map_count(m) == 0, "cleared");
    au_storage_map_set(m, "a\0b", 3, "\xF0\x9F\x98\x80\xED\xA0\x80", 7);
    CHECK(au_storage_map_bytes(m) == 2 * (3 + 3), "bytes: %u", au_storage_map_bytes(m));
    CHECK(au_storage_map_get(m, "a\0b", 3, &p, &n) && n == 7, "a key with NUL");
    CHECK(!au_storage_map_get(m, "a", 1, &p, &n), "not its prefix");
    au_storage_map_set(m, "", 0, "", 0);
    CHECK(au_storage_map_get(m, "", 0, &p, &n) && n == 0, "the empty key and value");
    au_storage_map_free(m);
}

/* ---- Persistence, isolation ---- */

static void test_persistence(void)
{
    MemFiles files;
    memset(&files, 0, sizeof files);
    Env e;
    env_init(&e, &files);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    EQ(js(&e, "localStorage.length+':'+localStorage.getItem('n')"), "0:null", "a new origin is empty");
    js(&e, "localStorage.setItem('n','1');localStorage.setItem('m','two')");
    CHECK(files.writes == 0, "a write does not touch the disk");
    CHECK(au_web_storage_pending(e.ws) > 0, "a write is pending");
    /* another page of the origin, in the same tab */
    answer(e.a, "", PAGE);
    go(&e, "http://a.test/other");
    EQ(js(&e, "localStorage.getItem('n')+localStorage.m+localStorage.length"), "1two2", "the next page of the origin");
    /* other origins: another host, and https of a host that has http */
    answer(e.bs, "", PAGE);
    go(&e, "http://b.test/");
    EQ(js(&e, "String(localStorage.getItem('n'))+localStorage.length"), "null0", "another host");
    js(&e, "localStorage.setItem('n','b')");
    answer(e.tls80, "", PAGE);
    go(&e, "http://tls.test/");
    js(&e, "localStorage.setItem('scheme','http')");
    answer(e.tls, "", PAGE);
    go(&e, "https://tls.test/");
    EQ(js(&e, "location.protocol+String(localStorage.getItem('scheme'))"), "https:null", "https does not see http");
    js(&e, "localStorage.setItem('scheme','https')");
    answer(e.tls80, "", PAGE);
    go(&e, "http://tls.test/");
    EQ(js(&e, "localStorage.getItem('scheme')"), "http", "http keeps its own");
    answer(e.a, "", PAGE);
    go(&e, "http://a.test/");
    EQ(js(&e, "localStorage.getItem('n')"), "1", "a.test again");

    /* Saving: one file per origin and the index. */
    int reads = files.reads;
    uint32_t done = au_web_storage_save(e.ws, 0);
    CHECK(done == 5 && files.n == 5, "4 areas and the index written: %u, %d files", done, files.n);
    CHECK(au_web_storage_pending(e.ws) == 0, "nothing pending after a save");
    CHECK(mf_find(&files, "Index") >= 0, "the index exists");
    for (int i = 0; i < files.n; i++)
        CHECK(strlen(files.f[i].name) <= 8, "file name '%s' fits HFS with .tmp", files.f[i].name);
    js(&e, "localStorage.getItem('n');localStorage.length;localStorage.key(0)");
    CHECK(files.reads == reads, "reads from script do not touch the disk");
    AuWebStorageStats st;
    au_web_storage_stats(e.ws, &st);
    CHECK(st.local_areas == 4 && st.pending == 0 && st.bytes > 0, "stats: %u areas, %u pending", st.local_areas,
          st.pending);
    env_free(&e);

    /* A new run reads the files back, each when first used. */
    env_init(&e, &files);
    reads = files.reads;
    CHECK(au_web_storage_count(e.ws) == 4, "the index lists 4 areas: %u", (unsigned)au_web_storage_count(e.ws));
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    EQ(js(&e, "localStorage.getItem('n')+localStorage.getItem('m')+localStorage.key(0)+localStorage.key(1)"), "1twonm",
       "after a restart, in order");
    CHECK(files.reads == reads + 1, "one file read: %d", files.reads - reads);
    js(&e, "localStorage.getItem('m')");
    CHECK(files.reads == reads + 1, "and no more");
    /* removing the last item removes the file at the next save */
    js(&e, "localStorage.removeItem('n');localStorage.removeItem('m')");
    au_web_storage_save(e.ws, 0);
    CHECK(files.n == 4, "the emptied area's file is gone: %d files", files.n);
    /* a failed write stays pending */
    js(&e, "localStorage.setItem('again','1')");
    files.fail_writes = true;
    au_web_storage_save(e.ws, 0);
    CHECK(au_web_storage_pending(e.ws) > 0, "a failed write stays pending");
    files.fail_writes = false;
    au_web_storage_save(e.ws, 0);
    CHECK(au_web_storage_pending(e.ws) == 0 && files.n == 5, "and is written later");
    env_free(&e);
    mf_free(&files);
}

/* ---- Two tabs, the storage event ---- */

static void test_tabs(void)
{
    Env e;
    env_init(&e, NULL);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/one");
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/two");
    answer(e.bs, "", PAGE);
    load(&e, "http://b.test/");
    /* tab 1 writes: tab 0 reads it at once and gets the event */
    jst(&e, 1, "localStorage.setItem('k','1')");
    EQ(jst(&e, 0, "localStorage.getItem('k')"), "1", "the other tab reads it");
    EQ(jst(&e, 0, "ev.join('|')"), "k,,1,http://a.test/two,L", "the other tab's event");
    EQ(jst(&e, 1, "ev.length"), "0", "no event in the writer");
    EQ(jst(&e, 2, "ev.length"), "0", "none in another origin");
    jst(&e, 1, "localStorage.setItem('k','1')");
    EQ(jst(&e, 0, "ev.length"), "1", "the same value fires nothing");
    jst(&e, 1, "localStorage.k='2';localStorage.removeItem('k');localStorage.removeItem('k');localStorage.x='y';"
               "localStorage.clear();localStorage.clear()");
    EQ(jst(&e, 0, "ev.slice(1).join('|')"),
       "k,1,2,http://a.test/two,L|k,2,,http://a.test/two,L|x,,y,http://a.test/two,L|,,,http://a.test/two,L",
       "change, removal, clear");
    EQ(jst(&e, 0, "var l=ev.length;localStorage.setItem('mine','1');l==ev.length"), "true", "none for its own write");
    EQ(jst(&e, 1, "ev.join('|')"), "mine,,1,http://a.test/one,L", "and one in the first writer");
    /* addEventListener, the event's class and null fields */
    jst(&e, 0, "var got=null;addEventListener('storage',function(e){got=e})");
    jst(&e, 1, "localStorage.setItem('q','1');localStorage.clear()");
    EQ(jst(&e, 0, "(got instanceof StorageEvent)+','+(got.key===null)+','+(got.oldValue===null)+','+(got.newValue===null)"),
       "true,true,true,true", "clear(): null key and values");
    /* sessionStorage is the tab's own */
    jst(&e, 0, "sessionStorage.setItem('s','0')");
    EQ(jst(&e, 1, "String(sessionStorage.getItem('s'))+ev.filter(function(x){return /S$/.test(x)}).length"), "null0",
       "sessionStorage is not shared between tabs, and fires nothing there");
    env_free(&e);
}

/* ---- sessionStorage and the tab ---- */

static void test_session(void)
{
    Env e;
    env_init(&e, NULL);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/1");
    js(&e, "sessionStorage.setItem('step','1');sessionStorage.setItem('u\\ttab','x%y\\n\\0z')");
    answer(e.a, "", PAGE);
    go(&e, "http://a.test/2");
    EQ(js(&e, "sessionStorage.getItem('step')"), "1", "after a navigation");
    js(&e, "sessionStorage.setItem('step','2')");
    au_bw_back(e.w);
    settle(&e, 300);
    AuPageStats st;
    au_page_stats(au_bw_active_tab(e.w)->page, &st);
    CHECK(st.restored, "Back showed the kept document");
    EQ(js(&e, "location.pathname+sessionStorage.getItem('step')"), "/12", "a kept document shares the tab's area");
    answer(e.a, "", PAGE);
    au_bw_reload(e.w);
    settle(&e, 300);
    EQ(js(&e, "sessionStorage.getItem('step')"), "2", "after a reload");
    /* another origin in the same tab has its own */
    answer(e.bs, "", PAGE);
    go(&e, "http://b.test/");
    EQ(js(&e, "String(sessionStorage.getItem('step'))"), "null", "per origin");
    au_bw_back(e.w);
    settle(&e, 300);

    /* window.open: a copy at that moment, independent after */
    answer(e.a, "", PAGE);
    js(&e, "window.open('http://a.test/opened')");
    settle(&e, 300);
    CHECK(e.w->ntabs == 2, "window.open made a tab: %d", e.w->ntabs);
    EQ(jst(&e, 1, "location.pathname+sessionStorage.getItem('step')"), "/opened2", "the opened tab has a copy");
    jst(&e, 1, "sessionStorage.setItem('step','opened')");
    jst(&e, 0, "sessionStorage.setItem('later','1')");
    EQ(jst(&e, 0, "sessionStorage.getItem('step')"), "2", "the opener keeps its own");
    EQ(jst(&e, 1, "String(sessionStorage.getItem('later'))"), "null", "later writes are not copied");
    /* Open Link in New Tab and a duplicated tab copy too; a plain new tab does not */
    answer(e.a, "", PAGE);
    au_bw_open_from(e.w, e.w->tabs[0], "http://a.test/link", false, 2);
    settle(&e, 300);
    EQ(jst(&e, 2, "sessionStorage.getItem('step')+sessionStorage.getItem('later')"), "21", "a link opened in a new tab");
    answer(e.a, "", PAGE);
    au_bw_new_tab(e.w, "http://a.test/fresh", false, 3);
    settle(&e, 300);
    EQ(jst(&e, 3, "sessionStorage.length"), "0", "a new tab starts empty");
    answer(e.a, "", PAGE);
    au_bw_duplicate_tab(e.w, 0);
    settle(&e, 300);
    EQ(jst(&e, 1, "sessionStorage.getItem('step')+sessionStorage.getItem('later')"), "21", "a duplicated tab");

    /* The session file carries it; strings with tab, %, newline and NUL
     * come back whole. */
    AuBuf ses;
    au_buf_init(&ses);
    au_browser_session_serialize(e.b, &ses);
    CHECK(strstr(au_buf_cstr(&ses), "\nstorage\thttp://a.test\t") != NULL, "storage lines in the session: %s", ses.data);
    AuSession s;
    CHECK(au_session_parse(&s, ses.data, ses.len), "the session parses");
    /* Close a tab and reopen it: its sessionStorage comes back. */
    au_bw_select_tab(e.w, 0);
    int ntabs = e.w->ntabs;
    au_bw_close_tab(e.w, 0);
    settle(&e, 50);
    CHECK(e.w->ntabs == ntabs - 1, "closed");
    answer(e.a, "", PAGE);
    AuTab *re = au_bw_reopen_closed_tab(e.w);
    settle(&e, 300);
    CHECK(re != NULL, "reopened");
    EQ(js(&e, "sessionStorage.getItem('step')+sessionStorage.getItem('later')+(sessionStorage.getItem('u\\ttab')==='x%y\\n\\0z')"),
       "21true", "a reopened tab");
    env_free(&e);

    /* A new run restores the session's tabs with their storage. */
    env_init(&e, NULL);
    CHECK(s.nwindows == 1 && s.windows[0].ntabs == 5, "5 tabs in the session: %d", s.nwindows ? s.windows[0].ntabs : -1);
    for (int i = 0; i < 5; i++)
        answer(e.a, "", PAGE);
    au_bw_restore(e.w, &s.windows[0]);
    settle(&e, 600);
    EQ(jst(&e, 0, "sessionStorage.getItem('step')+sessionStorage.getItem('later')+(sessionStorage.getItem('u\\ttab')==='x%y\\n\\0z')"),
       "21true", "restored: the first tab");
    EQ(jst(&e, 4, "sessionStorage.length"), "0", "restored: the fresh tab is still empty");
    bool opened = false;
    for (int i = 0; i < 5; i++)
        if (!strcmp(jst(&e, i, "String(sessionStorage.getItem('step'))"), "opened"))
            opened = true;
    CHECK(opened, "restored: the opened tab's own value");
    au_session_free(&s);
    au_buf_free(&ses);

    /* The cap: an area too large for the session file is left out. */
    answer(e.bs, "", PAGE);
    load(&e, "http://b.test/");
    js(&e, "sessionStorage.setItem('big','x'.repeat(300000));sessionStorage.setItem('small','1')");
    au_buf_init(&ses);
    au_browser_session_serialize(e.b, &ses);
    CHECK(ses.len < 100000 && !strstr(au_buf_cstr(&ses), "storage\thttp://b.test"), "an area over the cap is not saved: %u",
          (unsigned)ses.len);
    au_buf_free(&ses);
    env_free(&e);
}

/* ---- Frames: third parties, sandbox, data: ---- */

#define FRAME(name) "<script>var r;try{localStorage.setItem('who','" name "');sessionStorage.setItem('s','" name "');"         \
                    "r=localStorage.getItem('who')+'/'+localStorage.length}catch(x){r=x.name}"                                \
                    "parent.postMessage(['" name "',r],'*')</script>"
#define TOP "<!DOCTYPE html><title>t</title><script>var R={},ev=[];onmessage=function(e){R[e.data[0]]=e.data[1]};"            \
            "onstorage=function(e){ev.push(e.key+'='+e.newValue)}</script>"

static uint32_t find_area(AuWebStorage *ws, const char *key, AuStorageAreaInfo *out)
{
    AuStorageAreaInfo in;
    for (size_t i = 0; au_web_storage_info(ws, i, &in); i++)
        if (in.kind == AU_STORAGE_LOCAL && !strcmp(in.key, key)) {
            if (out)
                *out = in;
            return 1;
        }
    return 0;
}

static void frames_page(Env *e)
{
    answer(e->a, "",
           TOP "<iframe src='/same'></iframe><iframe src='http://b.test/f'></iframe>"
               "<iframe sandbox='allow-scripts' src='/sandboxed'></iframe>"
               "<iframe sandbox='allow-scripts allow-same-origin' src='/sandboxed-same'></iframe>"
               "<iframe src=\"data:text/html,<script>var r;try{localStorage.length;r='ok'}catch(x){r=x.name}"
               "parent.postMessage(['data',r],'*')</script>\"></iframe>"
               "<iframe srcdoc=\"<script>localStorage.setItem('sd','1');parent.postMessage(['srcdoc',localStorage.getItem('sd')],'*')</script>\"></iframe>");
    answer(e->a, "", FRAME("same"));
    answer(e->a, "", FRAME("sandboxed"));
    answer(e->a, "", FRAME("sandboxed-same"));
    answer(e->bs, "", FRAME("cross"));
    load(e, "http://a.test/");
    settle(e, 300);
}

static void test_frames(void)
{
    MemFiles files;
    memset(&files, 0, sizeof files);
    Env e;
    env_init(&e, &files); /* third-party cookies blocked */
    frames_page(&e);
    EQ(js(&e, "R.cross"), "cross/1", "a cross-site frame has storage");
    EQ(js(&e, "R.sandboxed"), "SecurityError", "a sandboxed frame has none");
    EQ(js(&e, "R['sandboxed-same']"), "sandboxed-same/1", "allow-same-origin gives it the origin's");
    EQ(js(&e, "R.data"), "SecurityError", "a data: document has none");
    EQ(js(&e, "localStorage.getItem('who')"), "sandboxed-same", "same-origin frames share the page's area");
    EQ(js(&e, "R.srcdoc+localStorage.getItem('sd')"), "11", "a srcdoc frame has its parent's");
    EQ(js(&e, "ev.length>0"), "true", "the page got storage events from its same-origin frames");
    AuStorageAreaInfo in;
    CHECK(find_area(e.ws, "http://b.test^http://a.test", &in) && in.session_only,
          "the cross-site frame's area is partitioned and for the session only");
    CHECK(!find_area(e.ws, "http://b.test", NULL), "and is not b.test's own");
    CHECK(find_area(e.ws, "http://a.test", &in) && !in.session_only, "the page's area is persistent");
    /* b.test at the top level does not see what its frame wrote */
    answer(e.bs, "", PAGE);
    load(&e, "http://b.test/");
    EQ(js(&e, "String(localStorage.getItem('who'))"), "null", "b.test's own area is apart");
    js(&e, "localStorage.setItem('who','top')");
    /* the same frame under another site: another partition */
    answer(e.c, "", TOP "<iframe src='http://b.test/f'></iframe>");
    answer(e.bs, "", "<script>parent.postMessage(['cross',String(localStorage.getItem('who'))],'*')</script>");
    load(&e, "http://c.test/");
    settle(&e, 200);
    EQ(js(&e, "R.cross"), "null", "under another site the frame starts empty");
    /* under the first site again: what it wrote is still there */
    answer(e.a, "", TOP "<iframe src='http://b.test/f'></iframe>");
    answer(e.bs, "", "<script>parent.postMessage(['cross',localStorage.getItem('who')+'/'+sessionStorage.getItem('s')],'*')</script>");
    load(&e, "http://a.test/again");
    settle(&e, 200);
    EQ(js(&e, "R.cross"), "cross/null", "the partition lasts for the session; sessionStorage is the tab's");
    /* Only the first-party areas are written. */
    au_web_storage_save(e.ws, 0);
    CHECK(files.n == 3, "a.test, b.test and the index on disk: %d files", files.n);
    env_free(&e);
    mf_free(&files);

    /* With third-party cookies allowed the partition is persistent. */
    env_init(&e, &files);
    au_cookie_jar_set_policy(e.jar, AU_COOKIE_POLICY_ALL, false);
    frames_page(&e);
    EQ(js(&e, "R.cross"), "cross/1", "a cross-site frame, third parties allowed");
    CHECK(find_area(e.ws, "http://b.test^http://a.test", &in) && !in.session_only,
          "still partitioned by the top-level site, but kept");
    /* the preference changes: the area becomes session only at its next use */
    au_web_storage_save(e.ws, 0);
    int nfiles = files.n;
    au_cookie_jar_set_policy(e.jar, AU_COOKIE_POLICY_ALL, true);
    answer(e.a, "", TOP "<iframe src='http://b.test/f'></iframe>");
    answer(e.bs, "", "<script>parent.postMessage(['cross',localStorage.getItem('who')],'*')</script>");
    load(&e, "http://a.test/again");
    settle(&e, 200);
    EQ(js(&e, "R.cross"), "cross", "its contents stay for the session");
    au_web_storage_save(e.ws, 0);
    CHECK(files.n == nfiles - 1 && find_area(e.ws, "http://b.test^http://a.test", &in) && in.session_only,
          "and its file is removed: %d files (were %d)", files.n, nfiles);
    env_free(&e);
    mf_free(&files);
}

/* ---- Cookie preferences and rules ---- */

static void test_policy(void)
{
    MemFiles files;
    memset(&files, 0, sizeof files);
    Env e;
    env_init(&e, &files);
    const char *probe = "(function(){try{localStorage.setItem('a','1');sessionStorage.setItem('a','1');return 'ok'}"
                        "catch(x){return x.name+(x instanceof DOMException)}})()";
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    EQ(js(&e, probe), "ok", "cookies on");
    au_cookie_jar_set_policy(e.jar, AU_COOKIE_POLICY_NONE, true);
    EQ(js(&e, probe), "SecurityErrortrue", "cookies off: SecurityError");
    EQ(js(&e, "(function(){try{return sessionStorage.length}catch(x){return x.name}})()"), "SecurityError",
       "sessionStorage too");
    au_cookie_jar_set_rule(e.jar, "a.test", AU_COOKIE_RULE_ALLOW);
    EQ(js(&e, probe), "ok", "an always-allow rule lets the site use it");
    au_cookie_jar_set_policy(e.jar, AU_COOKIE_POLICY_ALL, true);
    au_cookie_jar_set_rule(e.jar, "a.test", AU_COOKIE_RULE_BLOCK);
    EQ(js(&e, probe), "SecurityErrortrue", "a block rule");
    /* session only: usable, never written */
    au_cookie_jar_set_rule(e.jar, "a.test", AU_COOKIE_RULE_SESSION);
    EQ(js(&e, probe), "ok", "a session-only rule");
    au_web_storage_save(e.ws, 0);
    AuStorageAreaInfo in;
    CHECK(find_area(e.ws, "http://a.test", &in) && in.session_only && mf_area(&files) < 0,
          "session only: nothing on disk (%d files)", files.n);
    au_cookie_jar_set_rule(e.jar, "a.test", AU_COOKIE_RULE_NONE);
    EQ(js(&e, "localStorage.getItem('a')"), "1", "the rule removed: the data is still there");
    au_web_storage_save(e.ws, 0);
    CHECK(mf_area(&files) >= 0, "and is written now");

    /* Clearing: the Cookies window's model removes storage with cookies. */
    answer(e.bs, "Set-Cookie: c=1; Max-Age=1000\r\n", PAGE);
    load(&e, "http://b.test/");
    js(&e, "localStorage.setItem('b','1')");
    AuCookieView v;
    au_cookie_view_init(&v, e.jar);
    v.storage = e.ws;
    au_cookie_view_refresh(&v, e.f.wall);
    int row_a = -1, row_b = -1;
    for (int i = 0; i < v.list.n; i++) {
        if (!strcmp(v.list.rows[i].text, "a.test"))
            row_a = i;
        if (!strcmp(v.list.rows[i].text, "b.test"))
            row_b = i;
    }
    CHECK(row_a >= 0 && row_b >= 0, "sites with storage are listed (%d rows)", v.list.n);
    CHECK(row_a >= 0 && strstr(v.list.rows[row_a].detail, "storage"), "the site row names its storage: %s",
          row_a >= 0 ? v.list.rows[row_a].detail : "");
    CHECK(row_b >= 0 && strstr(v.list.rows[row_b].detail, "1 cookie") && strstr(v.list.rows[row_b].detail, "storage"),
          "cookies and storage: %s", row_b >= 0 ? v.list.rows[row_b].detail : "");
    AuBuf d;
    au_buf_init(&d);
    au_cookie_view_describe(&v, row_a, &d, e.f.wall);
    CHECK(strstr(au_buf_cstr(&d), "Storage:") != NULL, "the site's details: %s", d.data);
    CHECK(!strstr(d.data, "who") && !strstr(d.data, "=1"), "no contents in the details");
    /* expanded: a row per area, which Remove takes away */
    au_cookie_view_toggle(&v, row_a, e.f.wall);
    int area_row = -1;
    for (int i = 0; i < v.list.n; i++)
        if (!strncmp(v.list.rows[i].text, "Local storage", 13))
            area_row = i;
    CHECK(area_row >= 0, "an area row under the site");
    au_cookie_view_describe(&v, area_row, &d, e.f.wall);
    CHECK(strstr(au_buf_cstr(&d), "http://a.test") != NULL, "the area's details: %s", d.data);
    au_cookie_view_remove(&v, area_row, e.f.wall);
    CHECK(!find_area(e.ws, "http://a.test", NULL), "Remove on an area row removes it");
    /* Remove on a site row: cookies and storage */
    row_b = -1;
    for (int i = 0; i < v.list.n; i++)
        if (!strcmp(v.list.rows[i].text, "b.test"))
            row_b = i;
    au_cookie_view_remove(&v, row_b, e.f.wall);
    CHECK(!find_area(e.ws, "http://b.test", NULL) && au_cookie_jar_count(e.jar) == 0, "Remove on a site row");
    EQ(js(&e, "String(localStorage.getItem('b'))"), "null", "script sees it gone");
    js(&e, "localStorage.setItem('b','2');sessionStorage.setItem('b','2')");
    au_cookie_view_remove_all(&v, e.f.wall);
    CHECK(au_web_storage_count(e.ws) == 0, "Remove All clears every area: %u", (unsigned)au_web_storage_count(e.ws));
    au_web_storage_save(e.ws, 0);
    CHECK(mf_area(&files) < 0, "and their files");
    /* a block rule set in the window removes the site's storage */
    js(&e, "localStorage.setItem('b','3')");
    au_cookie_view_refresh(&v, e.f.wall);
    for (int i = 0; i < v.list.n; i++)
        if (!strcmp(v.list.rows[i].text, "b.test"))
            au_cookie_view_set_rule(&v, i, AU_COOKIE_RULE_BLOCK, e.f.wall);
    CHECK(au_web_storage_count(e.ws) == 0, "a block rule removes the site's storage");
    au_buf_free(&d);
    au_cookie_view_free(&v);
    env_free(&e);
    mf_free(&files);
}

/* ---- Quotas ---- */

static void test_quota(void)
{
    Env e;
    env_init(&e, NULL);
    au_web_storage_set_limits(e.ws, 2000, 5000, 0);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    /* 2000 bytes: 1000 UTF-16 units of keys and values */
    EQ(js(&e, "(function(){try{localStorage.setItem('k','x'.repeat(999));return 'ok'}catch(x){return x.name}})()"), "ok",
       "exactly the quota fits");
    EQ(js(&e, "(function(){try{localStorage.setItem('j','');return 'ok'}catch(x){return x.name+':'+x.code}})()"),
       "QuotaExceededError:22", "one more unit does not");
    EQ(js(&e, "localStorage.length+':'+localStorage.getItem('k').length"), "1:999", "a refused write changes nothing");
    EQ(js(&e, "(function(){try{localStorage.k='x'.repeat(1000);return 'ok'}catch(x){return x.name}})()"),
       "QuotaExceededError", "through a named property too");
    EQ(js(&e, "localStorage.setItem('k','\\ud83d\\ude00'.repeat(499)+'x');localStorage.getItem('k').length"), "999",
       "counted in UTF-16 units, not bytes");
    EQ(js(&e, "(function(){try{sessionStorage.setItem('k','x'.repeat(1000));return 'ok'}catch(x){return x.name}})()"),
       "QuotaExceededError", "sessionStorage has the same quota");
    /* estimate */
    EQ(js(&e, "var est;navigator.storage.estimate().then(function(x){est=x.usage+'/'+x.quota});1"), "1", "estimate()");
    EQ(js(&e, "est"), "2000/2000", "usage and quota");
    /* the total over all origins: 5000 bytes */
    answer(e.bs, "", PAGE);
    load(&e, "http://b.test/");
    js(&e, "localStorage.setItem('k','x'.repeat(999))");
    answer(e.c, "", PAGE);
    load(&e, "http://c.test/");
    EQ(js(&e, "(function(){try{localStorage.setItem('k','x'.repeat(999));return 'ok'}catch(x){return x.name}})()"),
       "QuotaExceededError", "the total over all origins");
    EQ(js(&e, "(function(){try{localStorage.setItem('k','x'.repeat(400));return 'ok'}catch(x){return x.name}})()"), "ok",
       "what still fits is taken");
    env_free(&e);

    /* Session-only areas make room for a persistent one, least recently
     * used first; persistent areas are never dropped. */
    env_init(&e, NULL);
    au_web_storage_set_limits(e.ws, 2000, 5000, 0);
    au_cookie_jar_set_rule(e.jar, "a.test", AU_COOKIE_RULE_SESSION);
    au_cookie_jar_set_rule(e.jar, "b.test", AU_COOKIE_RULE_SESSION);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    js(&e, "localStorage.setItem('k','x'.repeat(999))");
    answer(e.bs, "", PAGE);
    load(&e, "http://b.test/");
    js(&e, "localStorage.setItem('k','x'.repeat(999))");
    answer(e.c, "", PAGE);
    load(&e, "http://c.test/");
    EQ(js(&e, "(function(){try{localStorage.setItem('k','x'.repeat(999));return 'ok'}catch(x){return x.name}})()"), "ok",
       "a session-only area is dropped for a persistent write");
    CHECK(!find_area(e.ws, "http://a.test", NULL) && find_area(e.ws, "http://b.test", NULL) &&
              find_area(e.ws, "http://c.test", NULL),
          "the least recently used one went");
    env_free(&e);
}

/* ---- Strings, named properties, enumeration ---- */

static void test_api(void)
{
    Env e;
    env_init(&e, NULL);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    /* every UTF-16 sequence comes back: lone surrogates, pairs, NUL */
    EQ(js(&e, "var s='a\\ud800b\\udc00c\\ud83d\\ude00\\u0000z\\udfff';localStorage.setItem(s,s);"
              "var r=localStorage.getItem(s);(r===s)+','+r.length+','+(localStorage.key(0)===s)"),
       "true,10,true", "lone surrogates and NUL round-trip");
    EQ(js(&e, "localStorage.setItem('\\ud800','hi');localStorage.setItem('\\udc00','lo');"
              "localStorage.getItem('\\ud800')+localStorage.getItem('\\udc00')+String(localStorage.getItem('\\ud800\\udc00'))"),
       "hilonull", "a high and a low surrogate are two keys, and not their pair");
    js(&e, "localStorage.clear()");
    /* conversions */
    EQ(js(&e, "localStorage.setItem(1,2);localStorage.setItem('n',null);localStorage.setItem('u',undefined);"
              "localStorage.setItem('o',{});[typeof localStorage.getItem('1'),localStorage.n,localStorage.u,localStorage.o].join()"),
       "string,null,undefined,[object Object]", "keys and values are strings");
    EQ(js(&e, "localStorage.getItem('absent')===null&&localStorage.absent===undefined&&localStorage.key(99)===null"),
       "true", "missing items");
    /* order, length, key(n) */
    EQ(js(&e, "localStorage.clear();['z','a','m'].forEach(function(k,i){localStorage.setItem(k,i)});"
              "localStorage.setItem('a','again');[localStorage.length,localStorage.key(0),localStorage.key(1),localStorage.key(2)].join()"),
       "3,z,a,m", "insertion order; a changed value keeps its place");
    /* named properties */
    EQ(js(&e, "localStorage.foo='bar';localStorage['with space']=5;"
              "[localStorage.getItem('foo'),localStorage['with space'],'foo' in localStorage,'nope' in localStorage,"
              "'getItem' in localStorage,delete localStorage.foo,localStorage.getItem('foo')].join()"),
       "bar,5,true,false,true,true,", "set, get, in and delete by name");
    EQ(js(&e, "Object.keys(localStorage).join()"), "z,a,m,with space", "Object.keys");
    EQ(js(&e, "var ks=[];for(var k in localStorage)if(Object.prototype.hasOwnProperty.call(localStorage,k))ks.push(k);ks.join()"),
       "z,a,m,with space", "for-in");
    EQ(js(&e, "JSON.stringify(localStorage)"), "{\"z\":\"0\",\"a\":\"again\",\"m\":\"2\",\"with space\":\"5\"}",
       "JSON.stringify");
    EQ(js(&e, "Object.entries(localStorage).length+','+Object.getOwnPropertyNames(localStorage).length"), "4,4",
       "entries and own names");
    EQ(js(&e, "var d=Object.getOwnPropertyDescriptor(localStorage,'z');[d.value,d.enumerable,d.configurable,d.writable].join()"),
       "0,true,true,true", "descriptors");
    /* names of the interface: methods win when read; a write stores an item */
    EQ(js(&e, "localStorage.length=7;localStorage.setItem('getItem','x');"
              "[typeof localStorage.getItem,localStorage.length,localStorage.getItem('length'),localStorage.getItem('getItem')].join()"),
       "function,6,7,x", "items named like members");
    EQ(js(&e, "Object.defineProperty(localStorage,'def',{value:9});localStorage.getItem('def')"), "9", "defineProperty");
    EQ(js(&e, "localStorage.clear();localStorage.length+Object.keys(localStorage).length"), "0", "clear");
    /* the objects */
    EQ(js(&e, "[localStorage instanceof Storage,localStorage===window.localStorage,sessionStorage!==localStorage,"
              "String(localStorage),Object.prototype.toString.call(sessionStorage)].join()"),
       "true,true,true,[object Storage],[object Storage]", "the Storage objects");
    EQ(js(&e, "(function(){try{new Storage()}catch(x){return x.name}})()+"
              "(function(){try{Storage.prototype.getItem.call({},'a')}catch(x){return x.name}})()+"
              "(function(){try{localStorage.setItem('a')}catch(x){return x.name}})()"),
       "TypeErrorTypeErrorTypeError", "illegal uses");
    EQ(js(&e, "typeof navigator.storage.estimate+typeof StorageManager"), "functionfunction", "navigator.storage");
    /* many items: removal in the middle keeps key(n) right */
    EQ(js(&e, "for(var i=0;i<500;i++)localStorage.setItem('k'+i,i);for(i=0;i<500;i+=2)localStorage.removeItem('k'+i);"
              "var ok=localStorage.length==250;for(i=0;i<250;i++)ok=ok&&localStorage.key(i)=='k'+(2*i+1);ok"),
       "true", "key(n) after removals");
    env_free(&e);
}

/* ---- Files ---- */

static void test_files(void)
{
    MemFiles files;
    memset(&files, 0, sizeof files);
    Env e;
    env_init(&e, &files);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    js(&e, "localStorage.setItem('k1','value one');localStorage.setItem('\\ud800\\u0000','\\udc00');localStorage.setItem('','')");
    au_web_storage_save(e.ws, 0);
    env_free(&e);
    int ai = mf_area(&files);
    CHECK(ai >= 0 && files.n == 2, "one area file and the index");
    size_t n = files.f[ai].n;
    char *good = malloc(n);
    memcpy(good, files.f[ai].d, n);
    char name[40];
    snprintf(name, sizeof name, "%s", files.f[ai].name);

    /* The parser alone: the whole file, then every shorter length and
     * every single flipped byte must be refused without a crash. */
    AuStorageMap *m = au_storage_map_new();
    CHECK(au_web_storage_parse_file(good, n, "http://a.test", m) && au_storage_map_count(m) == 3, "the file parses");
    au_storage_map_clear(m);
    CHECK(!au_web_storage_parse_file(good, n, "http://b.test", m), "not as another origin's");
    int accepted = 0;
    for (size_t cut = 0; cut < n; cut++) {
        char *p = malloc(cut ? cut : 1);
        memcpy(p, good, cut);
        if (au_web_storage_parse_file(p, cut, "http://a.test", m))
            accepted++;
        CHECK(au_storage_map_count(m) == 0, "a refused file leaves nothing (cut %u)", (unsigned)cut);
        au_storage_map_clear(m);
        free(p);
    }
    CHECK(accepted == 0, "every truncation is refused: %d accepted", accepted);
    accepted = 0;
    for (size_t at = 0; at < n; at++) {
        char *p = malloc(n);
        memcpy(p, good, n);
        p[at] ^= 0x40;
        if (au_web_storage_parse_file(p, n, "http://a.test", m))
            accepted++;
        au_storage_map_clear(m);
        free(p);
    }
    CHECK(accepted == 0, "every flipped byte is refused: %d accepted", accepted);
    /* lengths that lie, with a right sum */
    {
        AuBuf b;
        au_buf_init(&b);
        static const unsigned char head[] = {'A', 'u', 'L', 'S', 0, 0, 0, 1, 0, 0, 0, 13};
        au_buf_append(&b, head, sizeof head);
        au_buf_puts(&b, "http://a.test");
        static const unsigned char lies[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 'k'};
        au_buf_append(&b, lies, sizeof lies);
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < b.len; i++)
            h = (h ^ (unsigned char)b.data[i]) * 16777619u;
        char sum[4] = {(char)(h >> 24), (char)(h >> 16), (char)(h >> 8), (char)h};
        au_buf_append(&b, sum, 4);
        CHECK(!au_web_storage_parse_file(b.data, b.len, "http://a.test", m), "a count and a length past the end");
        au_buf_free(&b);
    }
    au_storage_map_free(m);

    /* Through the service: a truncated file gives an empty area, is set
     * aside, and the origin can be written again. */
    files.f[ai].n = n / 2;
    env_init(&e, &files);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    EQ(js(&e, "localStorage.length+':'+localStorage.getItem('k1')"), "0:null", "a truncated file: an empty area");
    CHECK(files.damaged == 1 && mf_find(&files, name) < 0, "the file was set aside");
    js(&e, "localStorage.setItem('new','1')");
    au_web_storage_save(e.ws, 0);
    env_free(&e);
    env_init(&e, &files);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    EQ(js(&e, "localStorage.length+':'+localStorage.getItem('new')"), "1:1", "written again after the damage");
    env_free(&e);
    /* a damaged byte */
    ai = mf_area(&files);
    files.f[ai].d[files.f[ai].n / 2] ^= 1;
    env_init(&e, &files);
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    EQ(js(&e, "localStorage.length"), "0", "a damaged file: an empty area");
    CHECK(files.damaged == 2, "set aside too");
    env_free(&e);
    /* a file the index names but that is gone, and a damaged index */
    mf_free(&files);
    const char *idx = "# Aura local storage 1\n0000abcd\t20\t1\thttp://a.test\nzzzz\t1\t1\thttp://b.test\n"
                      "0000abce\t99999999999\t1\thttp://c.test\n0000abcf\t1\t1\t\n";
    mf_write(&files, "Index", idx, strlen(idx));
    env_init(&e, &files);
    CHECK(au_web_storage_count(e.ws) == 1 && files.damaged == 1, "a damaged index keeps its good lines: %u",
          (unsigned)au_web_storage_count(e.ws));
    answer(e.a, "", PAGE);
    load(&e, "http://a.test/");
    EQ(js(&e, "localStorage.length"), "0", "a missing area file: an empty area");
    js(&e, "localStorage.setItem('k','v')");
    au_web_storage_save(e.ws, 0);
    CHECK(mf_area(&files) >= 0 && au_web_storage_pending(e.ws) == 0, "and a new file");
    env_free(&e);
    idx = "not an index at all\n\xff\xfe";
    mf_free(&files);
    mf_write(&files, "Index", idx, strlen(idx));
    env_init(&e, &files);
    CHECK(au_web_storage_count(e.ws) == 0, "a file that is not an index");
    env_free(&e);
    mf_free(&files);
    free(good);

    /* Memory: over the resident limit, saved areas are unloaded and read
     * back when used; unsaved ones are written first. */
    env_init(&e, &files);
    au_web_storage_set_limits(e.ws, 0, 0, 6000);
    FakeServer *srv[3] = {e.a, e.bs, e.c};
    const char *urls[3] = {"http://a.test/", "http://b.test/", "http://c.test/"};
    for (int i = 0; i < 3; i++) {
        answer(srv[i], "", PAGE);
        load(&e, urls[i]);
        js(&e, "localStorage.setItem('k',location.host+'x'.repeat(4000))");
    }
    AuWebStorageStats st;
    au_web_storage_stats(e.ws, &st);
    CHECK(st.local_areas == 3 && st.loaded == 1 && st.resident < 6000, "one area in memory: %u loaded, %u bytes",
          st.loaded, (unsigned)st.resident);
    CHECK(files.writes >= 2, "the others were written to make room: %d writes", files.writes);
    EQ(jst(&e, 0, "localStorage.getItem('k').slice(0,6)+localStorage.getItem('k').length"), "a.test4006",
       "an unloaded area is read back");
    au_web_storage_save(e.ws, 0);
    CHECK(au_web_storage_release(e.ws) > 0, "release unloads saved areas");
    au_web_storage_stats(e.ws, &st);
    CHECK(st.loaded == 0 && st.resident == 0 && st.bytes > 24000, "nothing resident, the sizes still known");
    EQ(jst(&e, 1, "localStorage.getItem('k').length"), "4006", "and they come back");
    env_free(&e);
    mf_free(&files);
}

/* ---- document.referrer ---- */

static void test_referrer(void)
{
    Env e;
    env_init(&e, NULL);
    answer(e.a, "", "<!DOCTYPE html><title>t</title><a id=l href='/next?x=1'>next</a><a id=x href='http://b.test/'>b</a>");
    load(&e, "http://a.test/start?q=1#frag");
    EQ(js(&e, "document.referrer"), "", "a typed address has no referrer");
    answer(e.a, "", PAGE);
    js(&e, "document.getElementById('l').click()");
    settle(&e, 300);
    EQ(js(&e, "location.pathname+'|'+document.referrer"), "/next|http://a.test/start?q=1", "a same-origin link");
    const char *q = e.a->nreq > 1 ? e.a->requests[1] : "";
    CHECK(strstr(q, "Referer: http://a.test/start?q=1\r\n") != NULL, "the header the request carried: %s", q);
    /* cross-origin: the origin only, as the default policy sends it */
    answer(e.bs, "", PAGE);
    go(&e, "http://a.test/next?x=1");
    answer(e.bs, "", PAGE);
    answer(e.a, "", "<!DOCTYPE html><title>t</title><a id=x href='http://b.test/page'>b</a>");
    go(&e, "http://a.test/third");
    js(&e, "document.getElementById('x').click()");
    settle(&e, 300);
    EQ(js(&e, "location.host+'|'+document.referrer"), "b.test|http://a.test/", "a cross-origin link: the origin");
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_map();
    test_persistence();
    test_tabs();
    test_session();
    test_frames();
    test_policy();
    test_quota();
    test_api();
    test_files();
    test_referrer();
    au_fontset_free(fonts);
    for (int i = 0; i < nresponses; i++)
        free(responses[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
