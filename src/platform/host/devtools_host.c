/* aura-host chrome: the developer tools' --do actions. They print what
 * the Mac application's developer windows show, from the same models.
 *
 *   netlog           the active tab's request log: one line per request,
 *                    then the summary
 *   netlog:details   the same with each request's details and headers
 *   netlog:problems  only the failed and blocked requests; netlog:blocked
 *                    only the blocked ones; netlog:sort=COLUMN (status,
 *                    method, type, size, time, from, url) sorted;
 *                    netlog:filter=TEXT only the matching ones. These
 *                    print the lines as the window shows them, 100 cells
 *                    wide.
 *   perf             the Performance report
 *   status           the System Status report
 *   bugreport        the bug report of Help > Report a Problem (Markdown);
 *                    bugreport:tabs includes the other tabs' addresses
 *   devconsole       the Console window's lines; devconsole:errors,
 *                    :warnings, :logs or :filter=TEXT as its filters
 *   eval:EXPR        runs EXPR in the page as the Console's input line
 *                    does, and prints the console
 *   source           the Page Source window's text: numbered lines; with
 *                    source:FIRST,COUNT only those lines, each followed
 *                    by its colour runs
 *   devshot:KIND:FILE.png  paints that window's text view (source,
 *                    console, network, perf, status or report), 624 by
 *                    380, the size it has on a 640 by 480 screen;
 *                    devshot:KIND:WxH:FILE.png another size */
#include "host.h"
#include "png.h"
#include "browser/browser.h"
#include "browser/bugreport.h"
#include "browser/devtools.h"
#include "browser/netlog.h"
#include "browser/sourceview.h"
#include "browser/sysinfo.h"
#include "net/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* What the host tool knows about its machine: next to nothing, which
 * also shows how the reports read where a fact is unknown. */
static void host_sysinfo(AuSysInfo *s)
{
    memset(s, 0, sizeof *s);
    s->virtual_memory = -1;
    s->enforcer = -1;
    snprintf(s->os, sizeof s->os, "aura-host (the host tool)");
    snprintf(s->cpu, sizeof s->cpu, "the host's processor");
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    s->have_clock = true;
    strftime(s->utc, sizeof s->utc, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(s->script_utc, sizeof s->script_utc, "%s", s->utc);
    localtime_r(&now, &tm);
    strftime(s->local, sizeof s->local, "%Y-%m-%d %H:%M:%S", &tm);
    s->zone_seconds = (int32_t)tm.tm_gmtoff;
    s->daylight_saving = tm.tm_isdst > 0;
}

static void host_info(AuNet *net, AuDevHostInfo *h, AuBuf *conn, char *machine, size_t cap)
{
    AuSysInfo sys;
    host_sysinfo(&sys);
    memset(h, 0, sizeof *h);
    au_sysinfo_line(&sys, machine, cap);
    h->machine = machine;
    h->build = au_build_identity();
    if (net)
        au_net_dev_info(net, h, conn);
}

static void netview_options(AuNetView *v, const char *opt)
{
    static const char *const cols[] = {"#", "status", "method", "type", "size", "time", "from", "url"};
    if (!strcmp(opt, "problems"))
        v->show = AU_NETSHOW_PROBLEMS;
    else if (!strcmp(opt, "blocked"))
        v->show = AU_NETSHOW_BLOCKED;
    else if (!strncmp(opt, "filter=", 7))
        snprintf(v->filter, sizeof v->filter, "%s", opt + 7);
    else if (!strncmp(opt, "sort=", 5))
        for (int c = 0; c < AU_NETCOL_COUNT; c++)
            if (!strcmp(opt + 5, cols[c]))
                au_netview_click_column(v, c);
}

static void console_options(AuConsoleFilter *f, const char *opt)
{
    f->show = AU_CONSOLE_SHOW_ALL;
    f->text = NULL;
    if (!strcmp(opt, "errors"))
        f->show = AU_CONSOLE_SHOW_ERRORS;
    else if (!strcmp(opt, "warnings"))
        f->show = AU_CONSOLE_SHOW_WARNINGS;
    else if (!strcmp(opt, "logs"))
        f->show = AU_CONSOLE_SHOW_LOGS;
    else if (!strncmp(opt, "filter=", 7))
        f->text = opt + 7;
}

static void print_list(const AuTextList *l, AuBuf *out)
{
    for (uint32_t i = 0; i < l->n; i++)
        au_buf_printf(out, "%s%s\n", l->style[i] == AU_TS_HEADING ? "== " : "", l->text[i]);
}

bool au_host_dev_action(AuBrowserWindow *w, AuNet *net, const char *a)
{
    AuTab *t = au_bw_active_tab(w);
    AuBuf out, conn;
    char machine[256];
    au_buf_init(&out);
    au_buf_init(&conn);
    bool handled = true;
    if (!strcmp(a, "status")) {
        AuSysInfo sys;
        AuDevHostInfo h;
        host_sysinfo(&sys);
        host_info(net, &h, &conn, machine, sizeof machine);
        au_sysinfo_report(&sys, w->browser, &h, NULL, NULL, &out);
    } else if (!strcmp(a, "bugreport") || !strcmp(a, "bugreport:tabs")) {
        AuSysInfo sys;
        AuDevHostInfo h;
        AuBugReport br;
        host_sysinfo(&sys);
        host_info(net, &h, &conn, machine, sizeof machine);
        memset(&br, 0, sizeof br);
        br.browser = w->browser;
        br.window = w;
        br.sys = &sys;
        br.host = &h;
        br.other_tabs = a[9] == ':';
        au_bugreport_build(&br, &out);
    } else if (!t) {
        handled = false;
    } else if (!strcmp(a, "netlog") || !strcmp(a, "netlog:details")) {
        int total = 0, busy = 0;
        if (net)
            au_net_connections_text(net, &conn, &total, &busy);
        au_netlog_dump(au_page_netlog(t->page), au_buf_cstr(&conn), a[6] == ':', &out);
    } else if (!strncmp(a, "netlog:", 7)) {
        AuNetView v;
        AuTextList list;
        au_netview_init(&v);
        au_textlist_init(&list);
        netview_options(&v, a + 7);
        au_netview_build(&v, au_page_netlog(t->page));
        au_netlog_header_sorted(v.sort == AU_NETCOL_SEQ && !v.desc ? -1 : v.sort, v.desc, &out);
        au_buf_putc(&out, '\n');
        au_dev_network_lines(au_page_netlog(t->page), &v, 100, &list);
        print_list(&list, &out);
        au_textlist_free(&list);
        au_netview_free(&v);
    } else if (!strcmp(a, "perf")) {
        AuDevHostInfo h;
        host_info(net, &h, &conn, machine, sizeof machine);
        au_dev_perf_report(t->page, &h, au_platform_time_ms(), &out);
    } else if (!strcmp(a, "devconsole") || !strncmp(a, "devconsole:", 11)) {
        AuConsoleFilter f;
        AuTextList list;
        au_textlist_init(&list);
        console_options(&f, a[10] == ':' ? a + 11 : "");
        uint32_t hidden = au_dev_console_lines(t->page, &f, &list);
        print_list(&list, &out);
        au_buf_printf(&out, "(%lu lines, %lu entries filtered out)\n", (unsigned long)list.n, (unsigned long)hidden);
        au_textlist_free(&list);
    } else if (!strncmp(a, "eval:", 5)) {
        au_dev_console_eval(t->page, a + 5);
        au_dev_console_text(t->page, &out);
    } else if (!strcmp(a, "source") || !strncmp(a, "source:", 7)) {
        const char *data;
        size_t len;
        if (!au_page_source(t->page, &data, &len)) {
            au_buf_puts(&out, "source: this page has none\n");
        } else {
            AuSourceView sv;
            au_sourceview_init(&sv, data, len);
            uint32_t first = 0, count = sv.nlines;
            bool runs = false;
            if (a[6] == ':') {
                first = (uint32_t)strtoul(a + 7, NULL, 10);
                const char *comma = strchr(a + 7, ',');
                count = comma ? (uint32_t)strtoul(comma + 1, NULL, 10) : 1;
                first = first ? first - 1 : 0;
                runs = true;
            }
            au_buf_printf(&out, "source: %lu bytes, %lu lines\n", (unsigned long)len, (unsigned long)sv.nlines);
            for (uint32_t i = first; i < sv.nlines && i - first < count; i++) {
                size_t n;
                const char *line = au_sourceview_line(&sv, i, &n);
                au_buf_printf(&out, "%6lu  ", (unsigned long)i + 1);
                au_buf_append(&out, line, n > 2000 ? 2000 : n);
                au_buf_putc(&out, '\n');
                if (!runs)
                    continue;
                AuSourceRun r[64];
                int nr = au_sourceview_runs(&sv, i, 0, n, r, 64);
                au_buf_puts(&out, "        ");
                for (int k = 0; k < nr; k++)
                    au_buf_printf(&out, "%s%lu:%s", k ? " " : "", (unsigned long)r[k].start,
                                  au_source_style_name(r[k].style));
                au_buf_putc(&out, '\n');
            }
            au_sourceview_free(&sv);
        }
    } else if (!strncmp(a, "devshot:", 8)) {
        const char *kind = a + 8, *file = strchr(kind, ':');
        int W = 624, H = 380;
        if (file && sscanf(file + 1, "%dx%d:", &W, &H) == 2)
            file = strchr(file + 1, ':');
        if (W < 100 || H < 60 || W > 4000 || H > 4000)
            W = 624, H = 380;
        AuTextView tv;
        AuTextList list;
        AuSourceView sv;
        AuTextSource src;
        AuNetView nv;
        bool have_sv = false;
        au_textview_init(&tv);
        au_textlist_init(&list);
        au_netview_init(&nv);
        au_textlist_source(&list, &src);
        tv.wrap = true;
        tv.active = true;
        AuSysInfo sys;
        AuDevHostInfo h;
        host_sysinfo(&sys);
        host_info(net, &h, &conn, machine, sizeof machine);
        if (!strncmp(kind, "source", 6)) {
            const char *data = "";
            size_t len = 0;
            au_page_source(t->page, &data, &len);
            au_sourceview_init(&sv, data, len);
            au_sourceview_source(&sv, &src);
            have_sv = true;
            tv.numbers = true;
        } else if (!strncmp(kind, "console", 7)) {
            au_dev_console_lines(t->page, NULL, &list);
        } else if (!strncmp(kind, "network", 7)) {
            tv.wrap = false;
            tv.select_rows = true;
        } else if (!strncmp(kind, "status", 6)) {
            au_sysinfo_report(&sys, w->browser, &h, NULL, NULL, &out);
            au_dev_report_lines(au_buf_cstr(&out), &list);
            au_buf_clear(&out);
        } else if (!strncmp(kind, "report", 6)) {
            AuBugReport br;
            memset(&br, 0, sizeof br);
            br.browser = w->browser;
            br.window = w;
            br.sys = &sys;
            br.host = &h;
            au_bugreport_build(&br, &out);
            au_dev_markdown_lines(au_buf_cstr(&out), &list);
            au_buf_clear(&out);
        } else {
            au_dev_perf_report(t->page, &h, au_platform_time_ms(), &out);
            au_dev_report_lines(au_buf_cstr(&out), &list);
            au_buf_clear(&out);
        }
        au_textview_set_source(&tv, &src);
        AuSurface *s = au_surface_new(W, H);
        AuCanvas *c = au_canvas_new(s);
        double t0 = au_platform_time_ms();
        au_textview_layout(&tv, w->browser->fonts, W, H);
        if (!strncmp(kind, "network", 7)) {
            /* The rows are cut to the view's columns, as in the window. */
            au_netview_build(&nv, au_page_netlog(t->page));
            au_dev_network_lines(au_page_netlog(t->page), &nv, tv.cols, &list);
            au_textview_layout(&tv, w->browser->fonts, W, H);
        }
        double t1 = au_platform_time_ms();
        if (tv.select_rows && tv.nlines)
            tv.selected = 0;
        if (have_sv)
            au_textview_find(&tv, "<body", true);
        au_textview_paint(&tv, c, 0, 0);
        double t2 = au_platform_time_ms();
        au_buf_printf(&out, "devshot %s: %lu lines, %lu rows, %d columns, layout %.0f ms, paint %.0f ms\n", kind,
                      (unsigned long)tv.nlines, (unsigned long)tv.total_rows, tv.cols, t1 - t0, t2 - t1);
        if (file && !au_png_write(file + 1, s))
            au_buf_printf(&out, "devshot: cannot write %s\n", file + 1);
        au_canvas_free(c);
        au_surface_free(s);
        au_textview_free(&tv);
        au_textlist_free(&list);
        au_netview_free(&nv);
        if (have_sv)
            au_sourceview_free(&sv);
    } else {
        handled = false;
    }
    if (handled)
        fputs(au_buf_cstr(&out), stdout);
    au_buf_free(&out);
    au_buf_free(&conn);
    return handled;
}
