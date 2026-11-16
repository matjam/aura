/* The bug report (src/browser/bugreport.c) and the System Status report
 * (sysinfo.c), on the fake transport: the report's structure, that it is
 * ASCII Markdown with balanced code blocks, and what it must leave out
 * (a cookie, an Authorization header, a long query string, a password in
 * an address, a file path, other tabs' addresses, personal
 * preferences). */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
#include "browser/bugreport.h"
#include "browser/devtools.h"
#include "browser/netlog.h"
#include "browser/sysinfo.h"
#include "net/cache.h"
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
    AuHttpCache *cache;
    AuCookieJar *jar;
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
} Env;

static uint32_t t_ms(void *ctx) { return ((Env *)ctx)->f.now; }
static AuTime t_now(void *ctx) { return ((Env *)ctx)->f.wall; }

static void env_init(Env *e)
{
    memset(e, 0, sizeof *e);
    fake_init(&e->f);
    e->net = au_net_new(&e->f.ops);
    e->cache = au_cache_new(1 << 20);
    au_net_set_cache(e->net, e->cache);
    e->jar = au_cookie_jar_new();
    au_net_set_cookie_jar(e->net, e->jar);
    au_net_set_trace_headers(e->net, true);
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    e->b = au_browser_new(fonts, &h);
    e->b->prefs.https_first = false; /* the fake servers speak plain http */
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    au_browser_set_fetcher(e->b, "https", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
    au_cookie_jar_free(e->jar);
    au_cache_free(e->cache);
    fake_free(&e->f);
}

static void settle(Env *e, int rounds)
{
    for (int i = 0; i < rounds && au_browser_busy(e->b); i++) {
        e->f.now += 10;
        au_bw_idle(e->w);
    }
}

static void respond(FakeServer *s, const char *type, const char *extra, const char *body)
{
    static char bufs[16][4096];
    static int next;
    char *b = bufs[next++ % 16];
    snprintf(b, 4096, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%sContent-Length: %u\r\n\r\n%s", type, extra,
             (unsigned)strlen(body), body);
    fake_respond(s, b);
}

/* Whether every heading of want appears in text, in that order. */
static bool in_order(const char *text, const char *const *want, int n)
{
    const char *at = text;
    for (int i = 0; i < n; i++) {
        at = strstr(at, want[i]);
        if (!at)
            return false;
        at += strlen(want[i]);
    }
    return true;
}

static int count(const char *text, const char *what)
{
    int n = 0;
    for (const char *s = text; (s = strstr(s, what)) != NULL; s += strlen(what))
        n++;
    return n;
}

static void test_redaction(void)
{
    AuBuf b;
    au_buf_init(&b);
    char url[400], q[201];
    memset(q, 'q', 200);
    q[200] = 0;
    /* A query of 100 characters stays; one of 101 is cut after 100. */
    snprintf(url, sizeof url, "https://a.test/p?%.100s", q);
    au_bugreport_redact_url(url, &b);
    CHECK(!strcmp(au_buf_cstr(&b), url), "a query of 100 characters is whole");
    au_buf_clear(&b);
    snprintf(url, sizeof url, "https://a.test/p?%.101s#frag", q);
    au_bugreport_redact_url(url, &b);
    CHECK(b.len == strlen("https://a.test/p?") + 100 + 3 + 5 && strstr(au_buf_cstr(&b), "qqq...#frag"),
          "a query of 101 is cut: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    snprintf(url, sizeof url, "https://a.test/cb#access_token=%s", q);
    au_bugreport_redact_url(url, &b);
    CHECK(b.len == strlen("https://a.test/cb#") + 100 + 3, "a long fragment is cut too (%u)", (unsigned)b.len);
    au_buf_clear(&b);
    au_bugreport_redact_url("https://user:hunter2@a.test:8443/x?y=1", &b);
    CHECK(!strcmp(au_buf_cstr(&b), "https://a.test:8443/x?y=1"), "user name and password go: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    au_bugreport_redact_url("https://a.test/path/with@sign", &b);
    CHECK(!strcmp(au_buf_cstr(&b), "https://a.test/path/with@sign"), "an @ in the path stays: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    au_bugreport_redact_url("file:///Macintosh%20HD/Documents/Private%20Letters/page.html#top", &b);
    CHECK(!strcmp(au_buf_cstr(&b), "file:///.../page.html"), "a file address keeps its name only: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    au_bugreport_redact_url("about:home", &b);
    CHECK(!strcmp(au_buf_cstr(&b), "about:home"), "about: addresses pass: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    snprintf(url, sizeof url, "at f (https://u:p@a.test/s.js?%s:12:5), then file:///HD/Secret/x.html.", q);
    au_bugreport_redact_text(url, &b);
    CHECK(strstr(au_buf_cstr(&b), "at f (https://a.test/s.js?qqqq") && strstr(au_buf_cstr(&b), "...), then file:///.../x.html.") &&
              !strstr(au_buf_cstr(&b), "u:p") && !strstr(au_buf_cstr(&b), "Secret"),
          "addresses inside text: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    const char *raw = "caf\xC3\xA9 \xE2\x80\x93 \xE2\x80\x9Cq\xE2\x80\x9D \xF0\x9F\x98\x80 ```x``` a\rb\r\nc\x01";
    au_bugreport_ascii(raw, strlen(raw), &b);
    CHECK(!strcmp(au_buf_cstr(&b), "caf\\u00E9 - \"q\" \\U0001F600 '''x''' a\nb\nc?"), "ASCII form: %s", au_buf_cstr(&b));
    au_buf_free(&b);
}

static void test_report(void)
{
    Env e;
    env_init(&e);
    char page_url[400], q[201], html[2048];
    memset(q, 'k', 200);
    q[200] = 0;
    FakeServer *s = fake_server(&e.f, "bug.test", 80);
    FakeServer *other = fake_server(&e.f, "other.test", 80);
    snprintf(html, sizeof html,
             "<!DOCTYPE html><title>Bug \xE2\x80\x93 \xE2\x80\x9Creport\xE2\x80\x9D caf\xC3\xA9 ```</title>"
             "<p>page</p><script>"
             "console.warn('careful @someone');"
             "console.error('failed https://bug.test/x?%s end');"
             "console.log('a plain log line');"
             "fetch('/api?%s', {headers: {Authorization: 'Bearer SECRETTOKEN'}});"
             "</script>",
             q, q);
    respond(s, "text/html; charset=utf-8", "Set-Cookie: sid=TOPSECRETCOOKIE; Path=/\r\n", html);
    respond(s, "text/plain", "", "ok");
    respond(other, "text/html", "", "<!DOCTYPE html><title>Other</title><p>other tab");
    au_bw_new_tab(e.w, "http://other.test/private-page", true, -1);
    settle(&e, 500);
    snprintf(page_url, sizeof page_url, "http://bug.test/page?token=%s", q);
    au_bw_new_tab(e.w, page_url, true, -1);
    settle(&e, 500);
    AuPage *p = au_bw_active_tab(e.w)->page;
    const AuNetLog *l = au_page_netlog(p);

    /* A request with credentials in its address and an Authorization
     * header, as a sign-in would log it. */
    {
        AuNetLog *wl = (AuNetLog *)l;
        AuFetchInfo fi;
        AuFetchNet fn;
        memset(&fi, 0, sizeof fi);
        memset(&fn, 0, sizeof fn);
        fn.remote = fn.protocol = fn.cipher = fn.blocked_reason = "";
        fn.request_head = "POST /login HTTP/1.1\r\nHost: bug.test\r\nAuthorization: Bearer SECRETTOKEN\r\n"
                          "Cookie: sid=TOPSECRETCOOKIE\r\n";
        fn.response_head = "Set-Cookie: session=TOPSECRETCOOKIE2\r\n";
        for (int i = 0; i <= AU_FETCH_PHASE_DONE; i++)
            fn.at[i] = -1;
        fn.source = AU_FETCH_FROM_NETWORK;
        fi.status = 401;
        fi.net = &fn;
        uint32_t q2 = au_netlog_begin(wl, "http://admin:hunter2@bug.test/login", "POST", AU_FETCH_OTHER, 50);
        au_netlog_response(wl, q2, 401, "text/plain", false, 60);
        au_netlog_end(wl, q2, AU_NETREC_DONE, &fi, 70);
    }
    /* What the page holds that the report must not repeat. */
    bool auth_logged = false, cookie_sent = false;
    for (uint32_t i = 0; i < l->count; i++) {
        const AuNetRecord *r = au_netlog_at(l, i);
        if (r->request_head && strstr(r->request_head, "Authorization: Bearer SECRETTOKEN"))
            auth_logged = true;
        if (r->request_head && strstr(r->request_head, "sid=TOPSECRETCOOKIE"))
            cookie_sent = true;
    }
    CHECK(l->count == 3 && auth_logged && cookie_sent, "the log holds the secrets (%u requests, auth %d, cookie %d)",
          (unsigned)l->count, auth_logged, cookie_sent);
    CHECK(au_page_console_current(p) >= 2 && au_page_console_current(p) <= au_page_console_count(p),
          "this page's console lines are counted (%u of %u)", (unsigned)au_page_console_current(p),
          (unsigned)au_page_console_count(p));

    /* Preferences: some that show, some that never do. */
    AuPrefs *pr = &e.b->prefs;
    pr->javascript = true;
    pr->legacy_tls = false;
    au_prefs_set_str(&pr->home_page, "http://my.home.example/start");
    au_prefs_set_str(&pr->download_folder, "Macintosh HD:Private Folder");
    au_prefs_set_str(&pr->http_proxy_host, "proxy.secret.example");
    au_prefs_set_str(&pr->search_template, "http://search.secret.example/?q=%s");
    au_prefs_set_str(&pr->user_agent, "CustomAgent/1.0 (test)");
    au_prefs_set_str(&pr->block_allow, "allowed-one.example allowed-two.example");
    au_prefs_set_str(&pr->zoom_sites, "zoomed.example=150");
    au_history_visit(&e.b->history, "http://history.secret.example/", "History entry", 1);

    AuSysInfo sys;
    memset(&sys, 0, sizeof sys);
    snprintf(sys.model, sizeof sys.model, "Power Mac G4 (AGP Graphics)");
    sys.machine_type = 406;
    snprintf(sys.cpu, sizeof sys.cpu, "PowerPC G4 (7400)");
    sys.cpu_mhz = 450;
    sys.bus_mhz = 100;
    sys.ram_mb = 512;
    sys.virtual_memory = 0;
    snprintf(sys.os, sizeof sys.os, "Mac OS 9.2.2");
    snprintf(sys.carbonlib, sizeof sys.carbonlib, "1.6");
    snprintf(sys.appearance, sizeof sys.appearance, "1.1.4");
    snprintf(sys.open_transport, sizeof sys.open_transport, "2.7.9");
    sys.screen_w = 1024;
    sys.screen_h = 768;
    sys.screen_depth = 32;
    sys.enforcer = 1;
    snprintf(sys.enforcer_note, sizeof sys.enforcer_note, "protecting applications");
    sys.have_memory = true;
    sys.partition_kb = 65536;
    sys.heap_free_kb = 40000;
    sys.heap_block_kb = 30000;
    sys.temp_free_kb = 200000;
    sys.have_clock = true;
    snprintf(sys.utc, sizeof sys.utc, "2026-11-15 23:59:12");
    snprintf(sys.script_utc, sizeof sys.script_utc, "2026-11-15 23:59:12");
    snprintf(sys.local, sizeof sys.local, "2026-11-15 16:59:12");
    sys.zone_seconds = -25200;
    sys.daylight_saving = true;

    AuDevHostInfo h;
    AuBuf conn, out;
    au_buf_init(&conn);
    au_buf_init(&out);
    memset(&h, 0, sizeof h);
    au_net_dev_info(e.net, &h, &conn);
    AuBugReport br;
    memset(&br, 0, sizeof br);
    br.browser = e.b;
    br.window = e.w;
    br.sys = &sys;
    br.host = &h;
    au_bugreport_build(&br, &out);
    const char *t = au_buf_cstr(&out);

    /* Structure: the ten sections in order, each under a heading. */
    static const char *const order[] = {"## What I did",  "## What I expected", "## What happened", "## Aura\n",
                                        "## Machine",     "## Aura's state",    "## Page",          "## Console",
                                        "## Network",     "<details>",          "## Performance",   "<details>",
                                        "## Settings that differ from the defaults", "## Clock",    "\n---\n",
                                        "Left out by Aura"};
    CHECK(in_order(t, order, (int)(sizeof order / sizeof order[0])), "the sections come in order:\n%s", t);
    CHECK(count(t, "\n```\n") % 2 == 0 && count(t, "\n```\n") >= 16 && count(t, "<details>") == 2 &&
              count(t, "</details>") == 2 && count(t, "<summary>") == 2,
          "code blocks and details are balanced (%d fences)", count(t, "\n```\n"));
    /* A blank line after <summary>, or GitHub shows the block as text. */
    CHECK(strstr(t, "</summary>\n\n```\n") != NULL, "a blank line follows each summary");

    /* ASCII only: printable characters, line feeds and tabs. */
    bool ascii = true;
    for (const unsigned char *c = (const unsigned char *)t; *c; c++)
        if (*c != '\n' && *c != '\t' && (*c < 0x20 || *c > 0x7E))
            ascii = false;
    CHECK(ascii, "the report is ASCII");
    CHECK(strstr(t, "Bug - \"report\" caf\\u00E9 '''") != NULL, "the title is written in ASCII, and cannot close a block:\n%s", t);

    /* The facts. */
    CHECK(strstr(t, "  Model                  Power Mac G4 (AGP Graphics) (machine type 406)") &&
              strstr(t, "  Processor              PowerPC G4 (7400), 450 MHz (bus 100 MHz)") &&
              strstr(t, "  Memory installed       512 MB") && strstr(t, "  Virtual memory         off") &&
              strstr(t, "  System                 Mac OS 9.2.2") && strstr(t, "  CarbonLib              1.6") &&
              strstr(t, "  Screen                 1024 x 768 pixels, 32 bits per pixel") &&
              strstr(t, "  Enforcer               installed, protecting applications"),
          "the machine section");
    CHECK(strstr(t, "  Memory partition       64.00 MB") && strstr(t, "  Free heap              39.06 MB") &&
              strstr(t, "  Windows and tabs       1 window, 2 tabs") && strstr(t, "  Script heap, tab 1.2 ") &&
              strstr(t, "(the active tab)") && strstr(t, "  HTTP memory cache "),
          "Aura's state");
    CHECK(strstr(t, "  Address                http://bug.test/page?token=kkkk") &&
              strstr(t, "  Loading                finished") && strstr(t, "  User agent             CustomAgent/1.0 (test)") &&
              strstr(t, "  Security               not secure (plain HTTP)") && strstr(t, "  TLS                    none") &&
              strstr(t, "  Blocked requests       0") && strstr(t, "  Other tabs             addresses not included"),
          "the page section");
    CHECK(strstr(t, "warn   careful @someone") && strstr(t, "error  failed http") && !strstr(t, "a plain log line"),
          "the console section has warnings and errors only");
    CHECK(strstr(t, "  Requests               3 ") && strstr(t, "The request table (3 of 3 requests)") &&
              strstr(t, "Status   Method") && strstr(t, "  401       http://bug.test/login") &&
              strstr(t, "POST    other") && strstr(t, "http://bug.test/login\n"),
          "the network section");
    CHECK(strstr(t, "  Time zone              UTC-7:00") && strstr(t, "  Daylight saving        on") &&
              strstr(t, "  UTC as Aura computes   2026-11-15 23:59:12"),
          "the clock section");
    CHECK(strstr(t, "Allow old servers (legacy TLS)   false   (default: true)") &&
              strstr(t, "HTTPS first                      false   (default: true)") &&
              strstr(t, "User agent                       CustomAgent/1.0 (test)") &&
              strstr(t, "Proxies                          HTTP set, HTTPS not set") &&
              strstr(t, "Domains always allowed           2 "),
          "the settings that differ");

    /* What must not be there. */
    char cut[128];
    memset(cut, 'k', 101);
    cut[101] = 0;
    CHECK(!strstr(t, "SECRETTOKEN") && !strstr(t, "Authorization"), "no Authorization header");
    CHECK(!strstr(t, "hunter2") && !strstr(t, "admin:"), "no user name or password from an address");
    CHECK(!strstr(t, "TOPSECRETCOOKIE") && !strstr(t, "Set-Cookie") && !strstr(t, "Cookie:"), "no cookie");
    CHECK(!strstr(t, cut) && count(t, "kkkkkkkkkk...") >= 4, "no query string past 100 characters (%d cut)",
          count(t, "kkkkkkkkkk..."));
    CHECK(!strstr(t, "other.test") && !strstr(t, "private-page"), "no address of another tab");
    CHECK(!strstr(t, "my.home.example") && !strstr(t, "Private Folder") && !strstr(t, "proxy.secret.example") &&
              !strstr(t, "search.secret.example") && !strstr(t, "allowed-one.example") && !strstr(t, "zoomed.example") &&
              !strstr(t, "history.secret.example"),
          "no home page, folder, proxy address, search template, site list or history");
    CHECK(out.len < 60000, "the report fits a GitHub issue (%u bytes)", (unsigned)out.len);

    /* A page that fills the log with long addresses, all blocked, still
     * gives a report that fits an issue. */
    {
        AuNetLog *wl = (AuNetLog *)l;
        char longurl[700];
        for (int i = 0; i < 320; i++) {
            int n = snprintf(longurl, sizeof longurl, "http://ads%d.test/", i);
            memset(longurl + n, 'p', 600);
            longurl[n + 600] = 0;
            uint32_t q3 = au_netlog_begin(wl, longurl, NULL, AU_FETCH_IMAGE, 100);
            au_netlog_set_blocked_reason(wl, q3, "blocked: A list");
            au_netlog_end(wl, q3, AU_NETREC_BLOCKED, NULL, 100);
        }
        AuBuf big;
        au_buf_init(&big);
        au_bugreport_build(&br, &big);
        CHECK(big.len < 60000 && strstr(au_buf_cstr(&big), "more are in the table below") &&
                  strstr(au_buf_cstr(&big), "(200 of 3") && count(au_buf_cstr(&big), "\n```\n") % 2 == 0,
              "a full log still fits an issue (%u bytes)", (unsigned)big.len);
        au_buf_free(&big);
    }

    /* With other tabs asked for, their addresses appear, redacted. */
    au_buf_clear(&out);
    br.other_tabs = true;
    au_bugreport_build(&br, &out);
    t = au_buf_cstr(&out);
    CHECK(strstr(t, "  Tab 1.1                http://other.test/private-page") && !strstr(t, "addresses not included"),
          "other tabs on request");

    /* The System Status report carries the same machine and clock lines. */
    au_buf_clear(&out);
    au_sysinfo_report(&sys, e.b, &h, "Network\n  Open Transport         started\n", NULL, &out);
    t = au_buf_cstr(&out);
    static const char *const status_order[] = {"System Status\n", "  Aura ", "  Machine ", "\nMachine\n", "\nClock\n",
                                               "\nAura\n",        "\nFonts\n", "\nNetwork\n  Open Transport "};
    CHECK(in_order(t, status_order, (int)(sizeof status_order / sizeof status_order[0])) &&
              strstr(t, "  Processor              PowerPC G4 (7400), 450 MHz (bus 100 MHz)") &&
              strstr(t, "  Time zone              UTC-7:00") && strstr(t, "  Free memory            39.06 MB of 64.00 MB"),
          "the System Status report:\n%s", t);
    char line[256];
    au_sysinfo_line(&sys, line, sizeof line);
    CHECK(!strcmp(line, "Power Mac G4 (AGP Graphics), PowerPC G4 (7400) 450 MHz, 512 MB, Mac OS 9.2.2, 1024x768x32"),
          "the machine in one line: %s", line);
    CHECK(strstr(au_build_identity(), "0.") != NULL && au_build_date()[0], "build identity: %s, %s", au_build_identity(),
          au_build_date());

    /* No window, no facts: still a whole report. */
    au_buf_clear(&out);
    memset(&br, 0, sizeof br);
    br.browser = e.b;
    au_bugreport_build(&br, &out);
    t = au_buf_cstr(&out);
    CHECK(in_order(t, order, (int)(sizeof order / sizeof order[0]) - 6) && strstr(t, "No browser window was open") &&
              strstr(t, "  Model                  unknown") && strstr(t, "please say whether it is installed") &&
              count(t, "\n```\n") % 2 == 0,
          "a report without a page or machine facts:\n%s", t);
    au_buf_free(&conn);
    au_buf_free(&out);
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_redaction();
    test_report();
    au_fontset_free(fonts);
    printf("bugreport: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
