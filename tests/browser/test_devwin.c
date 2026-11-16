/* The developer windows' models (src/browser/devwin_model.c), with no
 * network: the parts and their rectangles, when each window rebuilds its
 * text (no page, a page still loading, another page), the toggles and
 * the filter field, the Network selection and its detail pane, the
 * Console's input line (editing, history, running a line), Find, Copy
 * All and Save, and the calls to the port. */
#include "../layout/ahem.h"
#include "fakenet.h"

#include "browser/browser.h"
#include "browser/bugreport.h"
#include "browser/devwin_model.h"
#include "net/cache.h"
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
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuBrowser *b;
    AuBrowserWindow *w;
    /* What the port saw. */
    AuPage *page;     /* the page the windows show */
    bool no_page;     /* show none */
    int copies, saves, opens, asks, timings, busy_on, busy_off, titles, headers, infos, sections;
    char clip[8192];
    char saved[8192];
    char opened[256];
    char title[256];
    const char *answer; /* what ask_text answers; NULL: cancel */
    char asked_initial[64];
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
    e->l = au_net_loader_new(e->net);
    e->pf = au_net_page_fetcher_new(e->l);
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.time_ms = t_ms;
    h.now = t_now;
    e->b = au_browser_new(fonts, &h);
    AuPageFetcher fe;
    au_net_page_fetcher_interface(e->pf, &fe);
    au_browser_set_fetcher(e->b, "http", &fe);
    e->w = au_browser_window_new(e->b, 800, 600);
}

static void env_free(Env *e)
{
    au_browser_free(e->b);
    au_net_page_fetcher_free(e->pf);
    au_net_loader_free(e->l);
    au_net_free(e->net);
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

static void respond(FakeServer *s, const char *type, const char *body)
{
    static char bufs[32][2048];
    static int next;
    char *b = bufs[next++ % 32];
    snprintf(b, 2048, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n%s", type,
             (unsigned)strlen(body), body);
    fake_respond(s, b);
}

/* ---- The port ---- */

static AuPage *h_page(void *ctx)
{
    Env *e = ctx;
    if (e->no_page)
        return NULL;
    if (e->page)
        return e->page;
    AuTab *t = au_bw_active_tab(e->w);
    return t ? t->page : NULL;
}
static AuBrowserWindow *h_window(void *ctx) { return ((Env *)ctx)->w; }
static void h_info(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn)
{
    Env *e = ctx;
    e->infos++;
    snprintf(sys->os, sizeof sys->os, "Test OS 1.0");
    h->have_memory = true;
    h->heap_free_kb = 1234;
    au_buf_puts(conn, "test:80 x1");
    h->connection_text = au_buf_cstr(conn);
}
static void h_connections(void *ctx, AuBuf *out)
{
    (void)ctx;
    au_buf_puts(out, "dev.test:80 x1");
}
static void h_sections(void *ctx, AuBuf *out)
{
    ((Env *)ctx)->sections++;
    au_report_heading(out, "Shell section");
    au_report_kv(out, "Shell fact", "%d", 42);
    au_buf_putc(out, '\n');
}
static bool h_copy(void *ctx, const char *text, size_t len)
{
    Env *e = ctx;
    e->copies++;
    snprintf(e->clip, sizeof e->clip, "%.*s", (int)len, text);
    return true;
}
static void h_save(void *ctx, const char *prompt, const char *name, const char *text, size_t len)
{
    Env *e = ctx;
    (void)prompt;
    e->saves++;
    snprintf(e->saved, sizeof e->saved, "%s|%.*s", name, (int)len, text);
}
static void h_open(void *ctx, const char *url)
{
    Env *e = ctx;
    e->opens++;
    snprintf(e->opened, sizeof e->opened, "%s", url);
}
static char *h_ask(void *ctx, const char *prompt, const char *initial)
{
    Env *e = ctx;
    (void)prompt;
    e->asks++;
    snprintf(e->asked_initial, sizeof e->asked_initial, "%s", initial);
    return e->answer ? au_strndup(e->answer, strlen(e->answer)) : NULL;
}
static void h_timing(void *ctx) { ((Env *)ctx)->timings++; }
static void h_busy(void *ctx, bool on)
{
    Env *e = ctx;
    if (on)
        e->busy_on++;
    else
        e->busy_off++;
}
static void h_title(void *ctx, const char *title)
{
    Env *e = ctx;
    e->titles++;
    snprintf(e->title, sizeof e->title, "%s", title);
}
static void h_headers(void *ctx) { ((Env *)ctx)->headers++; }

static void make(AuDevWin *dw, Env *e, AuDevWinKind kind)
{
    AuDevWinHost h;
    memset(&h, 0, sizeof h);
    h.ctx = e;
    h.page = h_page;
    h.window = h_window;
    h.now_ms = t_ms;
    h.host_info = h_info;
    h.connections = h_connections;
    h.status_sections = h_sections;
    h.copy = h_copy;
    h.save_text = h_save;
    h.open_url = h_open;
    h.ask_text = h_ask;
    h.run_timing = h_timing;
    h.busy = h_busy;
    h.title_changed = h_title;
    h.record_headers = h_headers;
    au_devwin_init(dw, kind, e->b, fonts, 15, &h);
    au_devwin_resize(dw, 620, 380);
    au_devwin_refresh(dw, true);
}

/* The text of line i of the main view. */
static const char *line(AuDevWin *dw, uint32_t i)
{
    return i < dw->list.n ? dw->list.text[i] : "";
}

static bool has_line(AuDevWin *dw, const char *needle)
{
    for (uint32_t i = 0; i < dw->list.n; i++)
        if (strstr(dw->list.text[i], needle))
            return true;
    return false;
}

static void type(AuDevWin *dw, const char *s)
{
    char c[2] = {0, 0};
    for (; *s; s++) {
        c[0] = *s;
        au_devwin_key(dw, AU_KEY_NONE, c);
    }
}

/* ---- Tests ---- */

static void test_kinds_and_areas(void)
{
    const AuDevWinButton *b;
    const AuDevWinToggle *t;
    CHECK(!strcmp(au_devwin_kind(AU_DEVWIN_CONSOLE)->name, "console") &&
              !strcmp(au_devwin_kind(AU_DEVWIN_STATUS)->title, "System Status"),
          "names and titles");
    CHECK(au_devwin_buttons(AU_DEVWIN_SOURCE, &b) == 2 && b[0].action == AU_DEVWIN_COPY_ALL &&
              b[1].action == AU_DEVWIN_FIND,
          "Copy All first, Find second");
    CHECK(au_devwin_buttons(AU_DEVWIN_CONSOLE, &b) == 3 && b[2].action == AU_DEVWIN_CLEAR, "Console: Clear");
    CHECK(au_devwin_buttons(AU_DEVWIN_REPORT, &b) == 3 && !strcmp(b[0].label, "Copy Report") &&
              b[1].action == AU_DEVWIN_SAVE && b[2].action == AU_DEVWIN_OPEN_ISSUES,
          "Report a Problem's own buttons");
    CHECK(au_devwin_toggles(AU_DEVWIN_NETWORK, &t) == 3 && t[0].radio && t[0].on && !t[1].on,
          "Network: a radio group, All chosen");
    CHECK(au_devwin_toggles(AU_DEVWIN_CONSOLE, &t) == 3 && !t[0].radio && t[0].on && t[2].on,
          "Console: three check boxes, on");
    CHECK(au_devwin_toggles(AU_DEVWIN_PERF, &t) == 0, "Performance: none");

    Env e;
    env_init(&e);
    e.no_page = true;
    AuDevWin dw;
    make(&dw, &e, AU_DEVWIN_NETWORK);
    CHECK(e.headers == 1, "the Network window turns header recording on");
    AuDevWinAreas a;
    au_devwin_areas(&dw, &a);
    CHECK(a.has_band && a.band.bottom == 24 && a.has_filter && a.filter.left == 46 && a.filter.right == 196,
          "Network band and filter");
    CHECK(a.has_head && a.head.top == 24 && a.head.bottom == 39, "column titles under the band");
    /* Text between 39 and 348; the detail takes two fifths. */
    CHECK(a.has_detail && a.detail.bottom == 348 && a.detail.top == 348 - (348 - 39) * 2 / 5 &&
              a.main.bottom == a.detail.top - 1 && a.main.right == 605,
          "detail pane: %d..%d, main to %d", a.detail.top, a.detail.bottom, a.main.bottom);
    CHECK(a.bar.top == 348 && a.bar.bottom == 380, "button bar");
    AuDevRect r;
    au_devwin_button_rect(&dw, 1, &r);
    CHECK(r.left == 96 && r.top == 354 && r.right == 162, "second button at %d,%d", r.left, r.top);
    au_devwin_toggle_rect(&dw, 1, &r);
    CHECK(r.left == 210 + 46 && r.top == 4, "second radio after the field: %d", r.left);
    CHECK(au_devwin_pane_at(&dw, 50, 10) == AU_DEVWIN_PANE_FILTER && au_devwin_pane_at(&dw, 50, 30) ==
              AU_DEVWIN_PANE_HEAD && au_devwin_pane_at(&dw, 50, 100) == AU_DEVWIN_PANE_MAIN &&
              au_devwin_pane_at(&dw, 50, 300) == AU_DEVWIN_PANE_DETAIL && au_devwin_pane_at(&dw, 50, 360) ==
              AU_DEVWIN_PANE_NONE,
          "panes by point");
    au_devwin_free(&dw);

    make(&dw, &e, AU_DEVWIN_CONSOLE);
    au_devwin_areas(&dw, &a);
    CHECK(a.has_input && a.input.bottom == 348 && a.input.top == 329 && a.main.bottom == 329 && !a.has_detail,
          "Console input line over the bar");
    au_devwin_free(&dw);
    make(&dw, &e, AU_DEVWIN_REPORT);
    au_devwin_areas(&dw, &a);
    CHECK(a.has_band && !a.has_filter && a.band.bottom == 40 && a.main.top == 40, "Report band of two lines");
    au_devwin_toggle_rect(&dw, 0, &r);
    CHECK(r.left == 620 - 190 - 8 && r.top == 20, "Report check box at the right: %d", r.left);
    CHECK(au_devwin_kind(AU_DEVWIN_REPORT)->band_text[0] != NULL, "Report instructions");
    au_devwin_free(&dw);
    make(&dw, &e, AU_DEVWIN_SOURCE);
    au_devwin_areas(&dw, &a);
    CHECK(!a.has_band && a.main.top == 0 && a.main.bottom == 348, "Page Source: text only");
    CHECK(dw.tv.numbers && dw.tv.wrap, "Page Source numbers its lines");
    au_devwin_free(&dw);
    env_free(&e);
}

static void test_refresh(void)
{
    Env e;
    env_init(&e);
    AuDevWin src, con, net, perf, stat, rep;
    /* No page. */
    e.no_page = true;
    make(&src, &e, AU_DEVWIN_SOURCE);
    CHECK(!strcmp(src.footer, "This page has no source to show.") && !strcmp(au_devwin_title(&src), "Page Source"),
          "no page: %s", src.footer);
    make(&perf, &e, AU_DEVWIN_PERF);
    CHECK(has_line(&perf, "No page is open"), "Performance without a page");
    CHECK(!au_devwin_stale(&src), "nothing changed");

    /* A page. */
    FakeServer *s = fake_server(&e.f, "dev.test", 80);
    respond(s, "text/html", "<!DOCTYPE html><title>One</title>\n<p>one\n<p>two\n<script>console.log('hi')</script>");
    e.no_page = false;
    au_bw_new_tab(e.w, "http://dev.test/one", true, -1);
    settle(&e, 500);
    CHECK(au_devwin_stale(&src), "a page appeared");
    int titles = e.titles;
    CHECK(au_devwin_refresh(&src, false), "rebuilt");
    CHECK(strstr(src.footer, "4 lines") && !strcmp(au_devwin_title(&src), "Page Source: http://dev.test/one") &&
              e.titles == titles + 1 && !strcmp(e.title, au_devwin_title(&src)),
          "source: %s, title %s", src.footer, au_devwin_title(&src));
    CHECK(!au_devwin_stale(&src) && !au_devwin_refresh(&src, false), "the same source is not rebuilt");
    make(&con, &e, AU_DEVWIN_CONSOLE);
    CHECK(has_line(&con, "hi") && strstr(con.footer, "1 entries"), "console: %s", con.footer);
    CHECK(!au_devwin_stale(&con), "console current");
    AuPage *one = au_bw_active_tab(e.w)->page;
    au_dev_console_eval(one, "console.log('more')");
    CHECK(au_devwin_stale(&con) && au_devwin_refresh(&con, false) && has_line(&con, "more"), "a new console line");

    /* A page still loading keeps the old source. */
    fake_add(s, "")->silent = true;
    au_bw_new_tab(e.w, "http://dev.test/slow", true, -1);
    e.f.now += 10;
    au_bw_idle(e.w);
    AuPage *slow = au_bw_active_tab(e.w)->page;
    CHECK(slow && slow != one && au_page_loading(slow), "a loading page");
    CHECK(!au_devwin_stale(&src) && !au_devwin_refresh(&src, false) && strstr(src.footer, "4 lines"),
          "a loading page keeps the old text: %s", src.footer);
    CHECK(au_devwin_stale(&con), "the console follows another page at once");
    au_devwin_refresh(&con, false);
    CHECK(!has_line(&con, "more") || has_line(&con, "Page:"), "the console shows the new page");
    /* Forced, it shows the empty source. */
    CHECK(au_devwin_refresh(&src, true) && !strcmp(src.footer, "This page has no source to show."), "forced: %s",
          src.footer);

    /* Back to the first page: another page. */
    e.page = one;
    CHECK(au_devwin_stale(&src) && au_devwin_refresh(&src, false) && strstr(src.footer, "4 lines"),
          "page changed back: %s", src.footer);

    /* Performance: once a second, not while text is selected. */
    au_devwin_refresh(&perf, true);
    CHECK(!au_devwin_stale(&perf), "Performance just built");
    e.f.now += 1000;
    CHECK(au_devwin_stale(&perf), "a second later");
    au_textview_select_all(&perf.tv);
    CHECK(!au_devwin_stale(&perf), "waits while text is selected");
    au_textview_clear_selection(&perf.tv);
    au_devwin_refresh(&perf, false);
    CHECK(has_line(&perf, "Performance") && !strcmp(au_devwin_title(&perf), "Performance: http://dev.test/one"),
          "Performance title %s", au_devwin_title(&perf));

    /* System Status: every two seconds; the shell's facts and sections. */
    int infos = e.infos;
    make(&stat, &e, AU_DEVWIN_STATUS);
    CHECK(e.infos == infos + 1 && e.sections == 1 && has_line(&stat, "Shell fact") && has_line(&stat, "Test OS 1.0"),
          "System Status from the port's facts");
    CHECK(!strcmp(au_devwin_title(&stat), "System Status"), "no address in its title");
    e.f.now += 1000;
    CHECK(!au_devwin_stale(&stat), "not after one second");
    e.f.now += 1000;
    CHECK(au_devwin_stale(&stat), "after two");
    CHECK(au_devwin_run(&stat, AU_DEVWIN_REFRESH) == AU_DEVWIN_DONE && e.sections == 2 && !au_devwin_stale(&stat),
          "Refresh rebuilds it");

    /* Report a Problem: a snapshot. */
    make(&rep, &e, AU_DEVWIN_REPORT);
    CHECK(strstr(rep.footer, "characters. Nothing is sent.") && has_line(&rep, "What I did"), "report: %s",
          rep.footer);
    e.f.now += 100000;
    e.page = slow;
    CHECK(!au_devwin_stale(&rep) && !au_devwin_refresh(&rep, false), "never rebuilt by itself");

    /* Network follows the page, at most four times a second. */
    e.page = one;
    make(&net, &e, AU_DEVWIN_NETWORK);
    CHECK(net.list.n >= 1 && strstr(line(&net, 0), "document"), "network rows: %s", line(&net, 0));
    e.page = slow;
    CHECK(au_devwin_stale(&net), "another page");
    au_devwin_refresh(&net, false);
    e.page = one;
    au_devwin_refresh(&net, false);
    au_dev_console_eval(one, "1"); /* no request: the log does not change */
    CHECK(!au_devwin_stale(&net), "log unchanged");

    au_devwin_free(&src);
    au_devwin_free(&con);
    au_devwin_free(&net);
    au_devwin_free(&perf);
    au_devwin_free(&stat);
    au_devwin_free(&rep);
    env_free(&e);
}

static void test_network(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "net.test", 80);
    respond(s, "text/html",
            "<!DOCTYPE html><title>N</title><link rel=stylesheet href=/a.css><img src=/missing.png>"
            "<img src=/b.png>");
    respond(s, "text/css", "p{}");
    fake_respond(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno");
    fake_respond(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno");
    au_bw_new_tab(e.w, "http://net.test/page", true, -1);
    settle(&e, 800);
    AuDevWin dw;
    make(&dw, &e, AU_DEVWIN_NETWORK);
    const AuNetLog *l = au_page_netlog(au_bw_active_tab(e.w)->page);
    CHECK(l && l->count == 4 && dw.list.n == 4, "four requests, four rows (%u)", (unsigned)dw.list.n);
    /* Ahem's cells are wide: the rows cut the addresses. A wider window
     * builds them again with room for them. */
    CHECK(!strstr(line(&dw, 1), "a.css"), "row cut: %s", line(&dw, 1));
    au_devwin_resize(&dw, 2000, 380);
    CHECK(strstr(line(&dw, 1), "http://net.test/a.css") != NULL, "resized: %s", line(&dw, 1));
    CHECK(strstr(dw.header, "Status") != NULL, "column titles: %s", dw.header);
    CHECK(strstr(dw.footer, "4 requests") && !strstr(dw.footer, "Showing"), "footer: %s", dw.footer);
    CHECK(au_devwin_network_selected(&dw) == NULL, "nothing selected");
    bool summary = false;
    for (uint32_t i = 0; i < dw.dlist.n; i++)
        summary |= strstr(dw.dlist.text[i], "missing.png") != NULL;
    CHECK(summary, "the detail pane lists the problem requests");

    /* Problems: the two 404s. */
    au_devwin_toggle(&dw, 1);
    CHECK(!dw.toggle_on[0] && dw.toggle_on[1] && !dw.toggle_on[2], "a radio group");
    CHECK(dw.list.n == 2 && strstr(dw.footer, "Showing 2 of 4"), "problems: %u rows, %s", (unsigned)dw.list.n,
          dw.footer);
    au_devwin_toggle(&dw, 1);
    CHECK(dw.toggle_on[1] && dw.list.n == 2, "choosing the chosen one keeps it");
    au_devwin_toggle(&dw, 0);
    CHECK(dw.list.n == 4, "All again");

    /* Typing goes to the filter. */
    type(&dw, "CSS");
    CHECK(!strcmp(au_buf_cstr(&dw.filter), "CSS") && dw.filter_focus && dw.list.n == 1 &&
              strstr(line(&dw, 0), "a.css"),
          "filter %s: %u rows", au_buf_cstr(&dw.filter), (unsigned)dw.list.n);
    CHECK(au_devwin_key(&dw, AU_KEY_BACKSPACE, NULL) == AU_DEVWIN_DONE && !strcmp(au_buf_cstr(&dw.filter), "CS"),
          "Delete");
    CHECK(au_devwin_key(&dw, AU_KEY_ESCAPE, NULL) == AU_DEVWIN_DONE && dw.filter.len == 0 && dw.list.n == 4,
          "Escape clears it");
    CHECK(au_devwin_key(&dw, AU_KEY_ESCAPE, NULL) == AU_DEVWIN_DONE, "Escape on an empty field is used");
    CHECK(au_devwin_key(&dw, AU_KEY_TAB, NULL) == AU_DEVWIN_NOTHING &&
              au_devwin_key(&dw, AU_KEY_RETURN, NULL) == AU_DEVWIN_NOTHING,
          "Tab and Return are not used");
    CHECK(au_devwin_paste(&dw, "png\r\n") && !strcmp(au_buf_cstr(&dw.filter), "png  ") && dw.list.n == 0,
          "paste: one line into the filter (%u rows)", (unsigned)dw.list.n);
    au_devwin_key(&dw, AU_KEY_ESCAPE, NULL);

    /* Select a row: the detail pane and footer follow it. */
    AuDevWinAreas a;
    au_devwin_areas(&dw, &a);
    int y = a.main.top + dw.tv.row_h + dw.tv.row_h / 2; /* row 1 */
    AuDevWinPane pane = au_devwin_mouse_down(&dw, 100, y, 1, false);
    const AuNetRecord *sel = au_devwin_network_selected(&dw);
    CHECK(pane == AU_DEVWIN_PANE_MAIN && dw.tv.selected == 1 && sel, "row 1 selected (%d)", (int)dw.tv.selected);
    CHECK(sel && !strcmp(dw.footer, sel->url), "footer: the whole address: %s", dw.footer);
    bool detail = false;
    for (uint32_t i = 0; sel && i < dw.dlist.n; i++)
        detail |= strstr(dw.dlist.text[i], sel->url) != NULL;
    CHECK(detail, "the detail pane shows the request");
    uint32_t seq = sel ? sel->seq : 0;
    CHECK(au_devwin_key(&dw, AU_KEY_DOWN, NULL) == AU_DEVWIN_DONE && dw.tv.selected == 2, "Down moves it");
    sel = au_devwin_network_selected(&dw);
    CHECK(sel && sel->seq != seq && !strcmp(dw.footer, sel->url), "the detail follows");
    /* Sort by a column: the selection stays on its request. */
    seq = sel ? sel->seq : 0;
    au_netview_click_column(&dw.nv, AU_NETCOL_URL);
    au_devwin_refresh(&dw, true);
    sel = au_devwin_network_selected(&dw);
    CHECK(sel && sel->seq == seq, "selection kept through a new order");
    CHECK(strstr(dw.header, "^") || strstr(dw.header, "v"), "the sorted column is marked: %s", dw.header);
    au_devwin_network_select(&dw, -1);
    CHECK(!au_devwin_network_selected(&dw) && strstr(dw.footer, "requests"), "no selection");

    /* Copy All: every request with its details, and the connections. */
    CHECK(au_devwin_run(&dw, AU_DEVWIN_COPY_ALL) == AU_DEVWIN_DONE && e.copies == 1 &&
              strstr(e.clip, "net.test/b.png") && strstr(e.clip, "dev.test:80 x1"),
          "Copy All: %.200s", e.clip);
    /* Find selects a row and shows its details. */
    e.answer = "a.css";
    CHECK(au_devwin_run(&dw, AU_DEVWIN_FIND) == AU_DEVWIN_DONE && e.asks == 1, "Find asks");
    sel = au_devwin_network_selected(&dw);
    CHECK(sel && strstr(sel->url, "a.css") && strstr(dw.footer, "a.css"), "Find selects the request");
    au_devwin_free(&dw);
    env_free(&e);
}

static void test_console(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "con.test", 80);
    respond(s, "text/html",
            "<!DOCTYPE html><title>C</title><script>console.log('a log'); console.warn('a warning');"
            "console.error('an error');</script>");
    au_bw_new_tab(e.w, "http://con.test/", true, -1);
    settle(&e, 500);
    AuDevWin dw;
    make(&dw, &e, AU_DEVWIN_CONSOLE);
    CHECK(has_line(&dw, "a log") && has_line(&dw, "a warning") && has_line(&dw, "an error"), "all levels");
    CHECK(!strcmp(au_devwin_title(&dw), "Console: http://con.test/"), "title %s", au_devwin_title(&dw));

    /* The toggles: check boxes. */
    au_devwin_toggle(&dw, 2); /* Logs off */
    CHECK(!dw.toggle_on[2] && dw.toggle_on[0] && !has_line(&dw, "a log") && has_line(&dw, "an error") &&
              strstr(dw.footer, "hidden by the filter"),
          "Logs off: %s", dw.footer);
    au_devwin_toggle(&dw, 2);
    CHECK(dw.toggle_on[2] && has_line(&dw, "a log"), "Logs on");

    /* Typing goes to the input line; Tab moves to the filter. */
    CHECK(!dw.filter_focus, "the input line has the keyboard");
    type(&dw, "1+2");
    CHECK(!strcmp(au_buf_cstr(&dw.input), "1+2"), "typed: %s", au_buf_cstr(&dw.input));
    au_devwin_key(&dw, AU_KEY_BACKSPACE, NULL);
    type(&dw, "3");
    CHECK(!strcmp(au_buf_cstr(&dw.input), "1+3"), "Delete then type: %s", au_buf_cstr(&dw.input));
    au_devwin_key(&dw, AU_KEY_NONE, "\xc3\xa9"); /* a two-byte character */
    au_devwin_key(&dw, AU_KEY_BACKSPACE, NULL);
    CHECK(!strcmp(au_buf_cstr(&dw.input), "1+3"), "Delete removes a whole character");
    CHECK(au_devwin_key(&dw, AU_KEY_NONE, "\x01") == AU_DEVWIN_NOTHING, "a control character is not typed");
    CHECK(au_devwin_key(&dw, AU_KEY_TAB, NULL) == AU_DEVWIN_DONE && dw.filter_focus, "Tab to the filter");
    type(&dw, "warn");
    CHECK(!strcmp(au_buf_cstr(&dw.filter), "warn") && !strcmp(au_buf_cstr(&dw.input), "1+3") &&
              has_line(&dw, "a warning") && !has_line(&dw, "a log"),
          "the filter: %s", dw.footer);
    CHECK(au_devwin_key(&dw, AU_KEY_RETURN, NULL) == AU_DEVWIN_DONE && !dw.filter_focus, "Return leaves the filter");
    au_devwin_key(&dw, AU_KEY_TAB, NULL);
    au_devwin_key(&dw, AU_KEY_ESCAPE, NULL);
    CHECK(dw.filter.len == 0 && has_line(&dw, "a log"), "Escape clears the filter");
    /* A click in the text gives the keyboard back to the input line. */
    AuDevWinAreas a;
    au_devwin_areas(&dw, &a);
    CHECK(dw.filter_focus && au_devwin_mouse_down(&dw, 10, a.main.top + 4, 1, false) == AU_DEVWIN_PANE_MAIN &&
              !dw.filter_focus,
          "click in the text");
    au_devwin_mouse_up(&dw, AU_DEVWIN_PANE_MAIN);
    CHECK(au_devwin_mouse_down(&dw, a.filter.left + 2, a.filter.top + 2, 1, false) == AU_DEVWIN_PANE_FILTER &&
              dw.filter_focus,
          "click in the filter");
    au_devwin_key(&dw, AU_KEY_TAB, NULL);
    CHECK(au_devwin_paste(&dw, "\t*2") && !strcmp(au_buf_cstr(&dw.input), "1+3 *2"), "paste into the input: %s",
          au_buf_cstr(&dw.input));
    au_devwin_key(&dw, AU_KEY_BACKSPACE, NULL);
    au_devwin_key(&dw, AU_KEY_BACKSPACE, NULL);
    au_devwin_key(&dw, AU_KEY_BACKSPACE, NULL);

    /* Return runs the line. */
    int busy = e.busy_on;
    CHECK(au_devwin_key(&dw, AU_KEY_RETURN, NULL) == AU_DEVWIN_DONE, "Return runs");
    CHECK(dw.input.len == 0 && e.busy_on == busy + 1 && e.busy_off == e.busy_on, "the line is cleared, busy shown");
    CHECK(has_line(&dw, "1+3") && strcmp(line(&dw, dw.list.n - 1), "") && strstr(line(&dw, dw.list.n - 1), "4"),
          "input and result listed: %s", line(&dw, dw.list.n - 1));
    CHECK(au_textview_at_end(&dw.tv), "scrolled to the end");
    CHECK(au_devwin_key(&dw, AU_KEY_RETURN, NULL) == AU_DEVWIN_DONE && e.busy_on == busy + 1,
          "an empty line does not run");
    type(&dw, "'x'.repeat(2)");
    au_devwin_key(&dw, AU_KEY_RETURN, NULL);
    CHECK(has_line(&dw, "xx"), "second result");

    /* History. */
    au_devwin_key(&dw, AU_KEY_UP, NULL);
    CHECK(!strcmp(au_buf_cstr(&dw.input), "'x'.repeat(2)"), "Up: %s", au_buf_cstr(&dw.input));
    au_devwin_key(&dw, AU_KEY_UP, NULL);
    CHECK(!strcmp(au_buf_cstr(&dw.input), "1+3"), "Up again: %s", au_buf_cstr(&dw.input));
    au_devwin_key(&dw, AU_KEY_UP, NULL);
    CHECK(!strcmp(au_buf_cstr(&dw.input), "1+3"), "stops at the oldest");
    au_devwin_key(&dw, AU_KEY_DOWN, NULL);
    au_devwin_key(&dw, AU_KEY_DOWN, NULL);
    CHECK(dw.input.len == 0, "past the newest: empty");
    au_devwin_key(&dw, AU_KEY_DOWN, NULL);
    CHECK(dw.input.len == 0 && dw.history_at == dw.nhistory, "stays empty");
    for (int i = 0; i < AU_DEVWIN_HISTORY + 4; i++) {
        char expr[16];
        snprintf(expr, sizeof expr, "%d", i);
        type(&dw, expr);
        au_devwin_key(&dw, AU_KEY_RETURN, NULL);
    }
    CHECK(dw.nhistory == AU_DEVWIN_HISTORY && !strcmp(dw.history[0], "4"), "history keeps the last %d",
          AU_DEVWIN_HISTORY);
    /* A field holds at most its limit. */
    for (int i = 0; i < AU_DEVWIN_FILTER_MAX + 10; i++)
        au_devwin_key(&dw, AU_KEY_NONE, i == 0 ? "\t" : "y"); /* Tab is not text: no change */
    au_devwin_key(&dw, AU_KEY_TAB, NULL);
    for (int i = 0; i < AU_DEVWIN_FILTER_MAX + 10; i++)
        au_devwin_key(&dw, AU_KEY_NONE, "z");
    CHECK(dw.filter.len == AU_DEVWIN_FILTER_MAX, "the filter holds %d bytes (%u)", AU_DEVWIN_FILTER_MAX,
          (unsigned)dw.filter.len);
    au_devwin_key(&dw, AU_KEY_ESCAPE, NULL);
    au_devwin_key(&dw, AU_KEY_TAB, NULL);
    au_devwin_key(&dw, AU_KEY_ESCAPE, NULL);

    /* Copy All: every console line. Clear. */
    CHECK(au_devwin_run(&dw, AU_DEVWIN_COPY_ALL) == AU_DEVWIN_DONE && strstr(e.clip, "warn   a warning") &&
              strstr(e.clip, "1+3"),
          "Copy All: %.120s", e.clip);
    CHECK(au_devwin_run(&dw, AU_DEVWIN_CLEAR) == AU_DEVWIN_DONE && !has_line(&dw, "a log") &&
              strstr(dw.footer, "0 entries"),
          "Clear: %s", dw.footer);
    CHECK(au_devwin_run(&dw, AU_DEVWIN_SAVE) == AU_DEVWIN_NOTHING, "the Console has no Save");
    CHECK(au_devwin_run(&dw, AU_DEVWIN_COPY_ALL) == AU_DEVWIN_FAILED, "Copy All with nothing to copy fails");

    /* With no page, Return fails. */
    e.no_page = true;
    au_devwin_refresh(&dw, false);
    type(&dw, "1");
    CHECK(au_devwin_key(&dw, AU_KEY_RETURN, NULL) == AU_DEVWIN_FAILED && dw.input.len == 1, "no page to run in");
    CHECK(!strcmp(au_devwin_title(&dw), "Console"), "title without a page");
    au_devwin_free(&dw);
    env_free(&e);
}

static void test_find_and_copy(void)
{
    Env e;
    env_init(&e);
    FakeServer *s = fake_server(&e.f, "find.test", 80);
    respond(s, "text/html", "<!DOCTYPE html><title>F</title>\n<p>alpha\n<p>needle one\n<p>beta\n<p>needle two\n");
    au_bw_new_tab(e.w, "http://find.test/", true, -1);
    settle(&e, 500);
    AuDevWin dw;
    make(&dw, &e, AU_DEVWIN_SOURCE);
    CHECK(!au_devwin_can_find_again(&dw) && !au_devwin_find_next(&dw, true), "nothing to find yet");
    e.answer = NULL;
    CHECK(au_devwin_ask_find(&dw) == AU_DEVWIN_NOTHING && e.asks == 1, "cancelled");
    e.answer = "";
    CHECK(au_devwin_ask_find(&dw) == AU_DEVWIN_NOTHING && !au_devwin_can_find_again(&dw), "empty text");
    e.answer = "NEEDLE";
    CHECK(au_devwin_run(&dw, AU_DEVWIN_FIND) == AU_DEVWIN_DONE && au_devwin_can_find_again(&dw), "found");
    AuBuf t;
    au_buf_init(&t);
    CHECK(au_devwin_copy_text(&dw, &t) && !strcmp(au_buf_cstr(&t), "needle"), "selection: %s", au_buf_cstr(&t));
    uint32_t first = dw.tv.anchor.line;
    CHECK(au_devwin_find_next(&dw, true) && dw.tv.anchor.line > first, "the next one");
    CHECK(au_devwin_find_next(&dw, true) && dw.tv.anchor.line == first, "wraps around");
    CHECK(au_devwin_find_next(&dw, false) && dw.tv.anchor.line > first, "backward");
    e.answer = "nowhere";
    CHECK(au_devwin_run(&dw, AU_DEVWIN_FIND) == AU_DEVWIN_FAILED && !strcmp(e.asked_initial, "NEEDLE"),
          "no match fails; the dialog showed the last text");
    CHECK(au_devwin_find(&dw, "beta") && au_devwin_find(&dw, "BETA"), "find without a dialog");

    /* Copy All of the source: the page's text. */
    CHECK(au_devwin_run(&dw, AU_DEVWIN_COPY_ALL) == AU_DEVWIN_DONE && strstr(e.clip, "<p>needle two"),
          "Copy All: %s", e.clip);
    CHECK(au_devwin_select_all(&dw), "Select All");
    au_buf_clear(&t);
    CHECK(au_devwin_copy_text(&dw, &t) && strstr(au_buf_cstr(&t), "alpha"), "copy after Select All");
    CHECK(!au_devwin_paste(&dw, "x"), "no field to paste into");
    CHECK(au_devwin_key(&dw, AU_KEY_NONE, "x") == AU_DEVWIN_NOTHING, "typing is not used");
    CHECK(au_devwin_key(&dw, AU_KEY_PAGE_DOWN, NULL) == AU_DEVWIN_DONE, "Page Down scrolls");
    au_buf_free(&t);
    /* A drag selects text. */
    au_devwin_scroll_to(&dw, AU_DEVWIN_PANE_MAIN, 0);
    au_textview_clear_selection(&dw.tv);
    AuDevWinPane p = au_devwin_mouse_down(&dw, dw.tv.gutter + 1, 2, 1, false);
    CHECK(p == AU_DEVWIN_PANE_MAIN && !au_devwin_view(&dw, p)->select_rows, "a text pane");
    au_devwin_mouse_drag(&dw, p, 300, dw.tv.row_h * 2 + 2);
    au_devwin_mouse_up(&dw, p);
    CHECK(au_textview_has_selection(&dw.tv), "dragged a selection");
    au_devwin_free(&dw);

    /* Report a Problem: Copy Report, Save Report, Open GitHub Issues, and
     * its check box. */
    make(&dw, &e, AU_DEVWIN_REPORT);
    CHECK(!strcmp(au_devwin_title(&dw), "Report a Problem"), "title");
    CHECK(au_devwin_run(&dw, AU_DEVWIN_COPY_ALL) == AU_DEVWIN_DONE && strlen(e.clip) > 100 &&
              !strncmp(e.clip, dw.text.data, strlen(e.clip)),
          "Copy Report copies the report");
    CHECK(au_devwin_run(&dw, AU_DEVWIN_SAVE) == AU_DEVWIN_DONE && e.saves == 1 &&
              !strncmp(e.saved, "Aura Problem Report.txt|", 24) && strstr(e.saved, "\r\n") &&
              !strstr(e.saved, "\r\r"),
          "Save Report: CR LF");
    AuBuf sv;
    au_buf_init(&sv);
    au_devwin_save_text(&dw, &sv);
    size_t lfs = 0;
    for (size_t i = 0; i < dw.text.len; i++)
        lfs += dw.text.data[i] == '\n';
    CHECK(sv.len == dw.text.len + lfs, "a CR before every LF");
    au_buf_free(&sv);
    CHECK(au_devwin_run(&dw, AU_DEVWIN_OPEN_ISSUES) == AU_DEVWIN_DONE && e.opens == 1 &&
              !strcmp(e.opened, AU_BUGREPORT_ISSUES_URL),
          "Open GitHub Issues");
    size_t len = dw.text.len;
    dw.tv.top = 3;
    au_devwin_toggle(&dw, 0);
    CHECK(dw.toggle_on[0] && dw.text.len != len && dw.tv.top == 0, "the check box rebuilds the report");
    CHECK(au_devwin_run(&dw, AU_DEVWIN_CLEAR) == AU_DEVWIN_NOTHING, "no Clear here");
    au_devwin_free(&dw);

    /* Performance: Run Timing Test calls the port. */
    make(&dw, &e, AU_DEVWIN_PERF);
    CHECK(au_devwin_run(&dw, AU_DEVWIN_RUN_TIMING) == AU_DEVWIN_DONE && e.timings == 1, "Run Timing Test");
    CHECK(au_devwin_run(&dw, AU_DEVWIN_COPY_ALL) == AU_DEVWIN_DONE && strstr(e.clip, "Performance"), "Copy All");
    au_devwin_free(&dw);

    /* A model without a port does nothing harmful. */
    au_devwin_init(&dw, AU_DEVWIN_CONSOLE, e.b, fonts, 15, NULL);
    au_devwin_resize(&dw, 400, 300);
    CHECK(au_devwin_refresh(&dw, true) && strstr(dw.footer, "0 entries"), "no host: %s", dw.footer);
    CHECK(au_devwin_run(&dw, AU_DEVWIN_COPY_ALL) == AU_DEVWIN_FAILED, "no clipboard");
    CHECK(au_devwin_run(&dw, AU_DEVWIN_FIND) == AU_DEVWIN_NOTHING, "no dialog");
    au_devwin_free(&dw);
    env_free(&e);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_kinds_and_areas();
    test_refresh();
    test_network();
    test_console();
    test_find_and_copy();
    au_fontset_free(fonts);
    printf("devwin: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
