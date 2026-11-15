/* aura-host chrome: the developer tools' --do actions. They print what
 * the Mac application's Network, Performance, Console and Page Source
 * windows show, from the same models.
 *
 *   netlog           the active tab's request log: one line per request,
 *                    then the totals
 *   netlog:details   the same with each request's details and headers
 *   perf             the Performance report
 *   eval:EXPR        runs EXPR in the page as the Console's input line
 *                    does, and prints the console
 *   source           the Page Source window's text: numbered lines; with
 *                    source:FIRST,COUNT only those lines, each followed
 *                    by its colour runs */
#include "host.h"
#include "png.h"
#include "browser/browser.h"
#include "browser/devtools.h"
#include "browser/netlog.h"
#include "browser/sourceview.h"
#include "net/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool au_host_dev_action(AuBrowserWindow *w, AuNet *net, const char *a)
{
    AuTab *t = au_bw_active_tab(w);
    AuBuf out, conn;
    au_buf_init(&out);
    au_buf_init(&conn);
    bool handled = true;
    if (!t) {
        handled = false;
    } else if (!strcmp(a, "netlog") || !strcmp(a, "netlog:details")) {
        int total = 0, busy = 0;
        if (net)
            au_net_connections_text(net, &conn, &total, &busy);
        au_netlog_dump(au_page_netlog(t->page), au_buf_cstr(&conn), a[6] == ':', &out);
    } else if (!strcmp(a, "perf")) {
        AuDevHostInfo h;
        memset(&h, 0, sizeof h);
        h.machine = "aura-host";
        if (net)
            au_net_dev_info(net, &h, &conn);
        au_dev_perf_report(t->page, &h, au_platform_time_ms(), &out);
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
        /* devshot:source|console|network|perf:FILE.png paints that
         * window's text view, 760 by 420, as the Mac application does. */
        const char *kind = a + 8, *file = strchr(kind, ':');
        AuTextView tv;
        AuTextList list;
        AuSourceView sv;
        AuTextSource src;
        bool have_sv = false;
        au_textview_init(&tv);
        au_textlist_init(&list);
        au_textlist_source(&list, &src);
        tv.wrap = true;
        tv.active = true;
        if (!strncmp(kind, "source", 6)) {
            const char *data = "";
            size_t len = 0;
            au_page_source(t->page, &data, &len);
            au_sourceview_init(&sv, data, len);
            au_sourceview_source(&sv, &src);
            have_sv = true;
            tv.numbers = true;
        } else if (!strncmp(kind, "console", 7)) {
            au_dev_console_lines(t->page, &list);
        } else if (!strncmp(kind, "network", 7)) {
            AuBuf hd;
            au_buf_init(&hd);
            au_netlog_header(&hd);
            au_dev_network_lines(au_page_netlog(t->page), &list);
            tv.wrap = false;
            tv.select_rows = true;
            au_buf_free(&hd);
        } else {
            AuDevHostInfo h;
            memset(&h, 0, sizeof h);
            h.machine = "aura-host";
            if (net)
                au_net_dev_info(net, &h, &conn);
            au_dev_perf_report(t->page, &h, au_platform_time_ms(), &out);
            au_dev_report_lines(au_buf_cstr(&out), &list);
            au_buf_clear(&out);
        }
        au_textview_set_source(&tv, &src);
        AuSurface *s = au_surface_new(760, 420);
        AuCanvas *c = au_canvas_new(s);
        double t0 = au_platform_time_ms();
        au_textview_layout(&tv, w->browser->fonts, 760, 420);
        double t1 = au_platform_time_ms();
        if (tv.select_rows && tv.nlines)
            tv.selected = 0;
        if (have_sv)
            au_textview_find(&tv, "<body", true);
        au_textview_paint(&tv, c, 0, 0);
        double t2 = au_platform_time_ms();
        au_buf_printf(&out, "devshot %s: %lu lines, %lu rows, layout %.0f ms, paint %.0f ms\n", kind,
                      (unsigned long)tv.nlines, (unsigned long)tv.total_rows, t1 - t0, t2 - t1);
        if (file && !au_png_write(file + 1, s))
            au_buf_printf(&out, "devshot: cannot write %s\n", file + 1);
        au_canvas_free(c);
        au_surface_free(s);
        au_textview_free(&tv);
        au_textlist_free(&list);
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
