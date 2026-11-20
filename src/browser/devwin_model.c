/* The developer windows' models. See devwin_model.h. */
#include "devwin_model.h"

#include "browser.h"
#include "bugreport.h"

#include <stdio.h>
#include <string.h>

/* ---- What each window has ---- */

/* The filter field in the tool band. */
enum { FIELD_X = 46, FIELD_W = 150, FIELD_TOP = 4, FIELD_BOTTOM = 20 };

static const AuDevWinKindInfo kinds[AU_DEVWIN_KINDS] = {
    {"source", "Page Source", 360, false, false, false, false, false, true, true, false, {NULL, NULL}},
    {"console", "Console", 300, true, true, false, false, true, true, false, false, {NULL, NULL}},
    {"network", "Network", 380, true, true, true, true, false, false, false, true, {NULL, NULL}},
    {"performance", "Performance", 400, false, false, false, false, false, true, false, false, {NULL, NULL}},
    {"status", "System Status", 400, false, false, false, false, false, true, false, false, {NULL, NULL}},
    {"report",
     "Report a Problem",
     400,
     false,
     true,
     false,
     false,
     false,
     true,
     false,
     false,
     {"This is all that Copy and Save take. It has no cookies, passwords or history. You can edit it after pasting.",
      "Paste it into a new issue at github.com/matjam/aura/issues"}},
};

static const AuDevWinButton text_buttons[] = {{AU_DEVWIN_COPY_ALL, "Copy All", 80}, {AU_DEVWIN_FIND, "Find...", 66}};
static const AuDevWinButton console_buttons[] = {
    {AU_DEVWIN_COPY_ALL, "Copy All", 80}, {AU_DEVWIN_FIND, "Find...", 66}, {AU_DEVWIN_CLEAR, "Clear", 64}};
static const AuDevWinButton perf_buttons[] = {
    {AU_DEVWIN_COPY_ALL, "Copy All", 80}, {AU_DEVWIN_FIND, "Find...", 66}, {AU_DEVWIN_RUN_TIMING, "Run Timing Test", 130}};
static const AuDevWinButton status_buttons[] = {
    {AU_DEVWIN_COPY_ALL, "Copy All", 80}, {AU_DEVWIN_FIND, "Find...", 66}, {AU_DEVWIN_REFRESH, "Refresh", 76}};
static const AuDevWinButton report_buttons[] = {{AU_DEVWIN_COPY_ALL, "Copy Report", 104},
                                                {AU_DEVWIN_SAVE, "Save Report...", 116},
                                                {AU_DEVWIN_OPEN_ISSUES, "Open GitHub Issues", 150}};

static const AuDevWinToggle console_toggles[] = {{AU_DEVWIN_SHOW_ERRORS, "Errors", 62, false, true},
                                                 {AU_DEVWIN_SHOW_WARNINGS, "Warnings", 76, false, true},
                                                 {AU_DEVWIN_SHOW_LOGS, "Logs", 52, false, true},
                                                 {AU_DEVWIN_TRACE_SCROLLS, "Trace Scrolls", 104, false, false},
                                                 {AU_DEVWIN_TRACE_NET, "Trace Network", 110, false, false}};
/* In AuNetShow order: the index of the chosen one is the view's show. */
static const AuDevWinToggle network_toggles[] = {{AU_DEVWIN_NET_ALL, "All", 40, true, true},
                                                 {AU_DEVWIN_NET_PROBLEMS, "Problems", 76, true, false},
                                                 {AU_DEVWIN_NET_BLOCKED, "Blocked", 68, true, false}};
static const AuDevWinToggle report_toggles[] = {
    {AU_DEVWIN_REPORT_OTHER_TABS, "Include other tabs' addresses", 190, false, false}};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const AuDevWinKindInfo *au_devwin_kind(AuDevWinKind kind)
{
    return (unsigned)kind < AU_DEVWIN_KINDS ? &kinds[kind] : &kinds[0];
}

int au_devwin_buttons(AuDevWinKind kind, const AuDevWinButton **out)
{
    switch (kind) {
    case AU_DEVWIN_CONSOLE: *out = console_buttons; return COUNT(console_buttons);
    case AU_DEVWIN_PERF: *out = perf_buttons; return COUNT(perf_buttons);
    case AU_DEVWIN_STATUS: *out = status_buttons; return COUNT(status_buttons);
    case AU_DEVWIN_REPORT: *out = report_buttons; return COUNT(report_buttons);
    default: *out = text_buttons; return COUNT(text_buttons);
    }
}

int au_devwin_toggles(AuDevWinKind kind, const AuDevWinToggle **out)
{
    switch (kind) {
    case AU_DEVWIN_CONSOLE: *out = console_toggles; return COUNT(console_toggles);
    case AU_DEVWIN_NETWORK: *out = network_toggles; return COUNT(network_toggles);
    case AU_DEVWIN_REPORT: *out = report_toggles; return COUNT(report_toggles);
    default: *out = NULL; return 0;
    }
}

/* ---- The host ---- */

static AuPage *cur_page(const AuDevWin *dw)
{
    return dw->host.page ? dw->host.page(dw->host.ctx) : NULL;
}

/* Trace Scrolls and Trace Network are the Console's fourth and fifth
 * toggles: the AU_DV_TRACE_* bits they ask of the page. */
static unsigned trace_flags(const AuDevWin *dw)
{
    if (dw->kind != AU_DEVWIN_CONSOLE)
        return 0;
    unsigned f = 0;
    if (dw->ntoggles > 3 && dw->toggle_on[3])
        f |= AU_PAGE_TRACE_SCROLL;
    if (dw->ntoggles > 4 && dw->toggle_on[4])
        f |= AU_PAGE_TRACE_NET;
    return f;
}

static uint32_t now_ms(const AuDevWin *dw)
{
    return dw->host.now_ms ? dw->host.now_ms(dw->host.ctx) : 0;
}

static void connections(AuDevWin *dw, AuBuf *out)
{
    if (dw->host.connections)
        dw->host.connections(dw->host.ctx, out);
}

/* The machine's facts and, from them, the Performance report's memory
 * and machine line. machine holds the text h points to; conn the
 * connections'. */
static void host_info(AuDevWin *dw, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn, char *machine, size_t cap)
{
    memset(sys, 0, sizeof *sys);
    memset(h, 0, sizeof *h);
    if (dw->host.host_info)
        dw->host.host_info(dw->host.ctx, sys, h, conn);
    au_sysinfo_line(sys, machine, cap);
    h->machine = machine;
    h->build = au_build_identity();
}

/* ---- Init ---- */

void au_devwin_init(AuDevWin *dw, AuDevWinKind kind, struct AuBrowser *browser, AuFontSet *fonts, int scroll_w,
                    const AuDevWinHost *host)
{
    memset(dw, 0, sizeof *dw);
    if ((unsigned)kind >= AU_DEVWIN_KINDS)
        kind = AU_DEVWIN_SOURCE;
    dw->kind = kind;
    dw->browser = browser;
    dw->fonts = fonts;
    dw->scroll_w = scroll_w;
    if (host)
        dw->host = *host;
    const AuDevWinKindInfo *info = &kinds[kind];
    au_textview_init(&dw->tv);
    au_textview_init(&dw->dv);
    au_textlist_init(&dw->list);
    au_textlist_init(&dw->dlist);
    au_netview_init(&dw->nv);
    au_buf_init(&dw->input);
    au_buf_init(&dw->filter);
    au_buf_init(&dw->text);
    AuTextSource src;
    au_textlist_source(&dw->list, &src);
    au_textview_set_source(&dw->tv, &src);
    au_textlist_source(&dw->dlist, &src);
    au_textview_set_source(&dw->dv, &src);
    dw->tv.wrap = info->wrap;
    dw->tv.numbers = info->numbers;
    dw->tv.select_rows = info->select_rows;
    dw->dv.wrap = true;
    const AuDevWinToggle *t;
    dw->ntoggles = au_devwin_toggles(kind, &t);
    for (int i = 0; i < dw->ntoggles; i++)
        dw->toggle_on[i] = t[i].on;
    snprintf(dw->title, sizeof dw->title, "%s", info->title);
    if (kind == AU_DEVWIN_NETWORK && dw->host.record_headers)
        dw->host.record_headers(dw->host.ctx);
}

void au_devwin_free(AuDevWin *dw)
{
    if (trace_flags(dw))
        au_page_set_trace(cur_page(dw), 0);
    au_textview_free(&dw->tv);
    au_textview_free(&dw->dv);
    au_textlist_free(&dw->list);
    au_textlist_free(&dw->dlist);
    au_netview_free(&dw->nv);
    if (dw->have_sv)
        au_sourceview_free(&dw->sv);
    au_buf_free(&dw->input);
    au_buf_free(&dw->filter);
    au_buf_free(&dw->text);
    for (int i = 0; i < dw->nhistory; i++)
        au_free(dw->history[i]);
    au_free(dw->find);
    memset(dw, 0, sizeof *dw);
}

/* ---- Geometry ---- */

static void set_rect(AuDevRect *r, int left, int top, int right, int bottom)
{
    r->left = left;
    r->top = top;
    r->right = right;
    r->bottom = bottom;
}

void au_devwin_areas(const AuDevWin *dw, AuDevWinAreas *a)
{
    const AuDevWinKindInfo *info = &kinds[dw->kind];
    memset(a, 0, sizeof *a);
    int W = dw->width, H = dw->height;
    set_rect(&a->all, 0, 0, W, H);
    set_rect(&a->bar, 0, H - AU_DEVWIN_BAR_H, W, H);
    int top = 0, bottom = H - AU_DEVWIN_BAR_H;
    if (info->band) {
        int th = dw->kind == AU_DEVWIN_REPORT ? AU_DEVWIN_REPORT_H : AU_DEVWIN_TOOL_H;
        a->has_band = true;
        set_rect(&a->band, 0, 0, W, th);
        top = th;
    }
    if (info->filter) {
        a->has_filter = true;
        set_rect(&a->filter, FIELD_X, FIELD_TOP, FIELD_X + FIELD_W, FIELD_BOTTOM);
    }
    if (info->head) {
        a->has_head = true;
        set_rect(&a->head, 0, top, W, top + AU_DEVWIN_HEAD_H);
        top += AU_DEVWIN_HEAD_H;
    }
    if (info->detail) {
        a->has_detail = true;
        int dh = (bottom - top) * 2 / 5;
        set_rect(&a->detail, 0, bottom - dh, W - dw->scroll_w, bottom);
        bottom = bottom - dh - 1;
    }
    if (info->input) {
        a->has_input = true;
        set_rect(&a->input, 0, bottom - AU_DEVWIN_INPUT_H, W, bottom);
        bottom -= AU_DEVWIN_INPUT_H;
    }
    set_rect(&a->main, 0, top, W - dw->scroll_w, bottom);
}

void au_devwin_button_rect(const AuDevWin *dw, int i, AuDevRect *r)
{
    const AuDevWinButton *b;
    int n = au_devwin_buttons(dw->kind, &b), x = 8;
    if (i < 0 || i >= n) {
        set_rect(r, 0, 0, 0, 0);
        return;
    }
    for (int k = 0; k < i; k++)
        x += b[k].width + 8;
    int top = dw->height - AU_DEVWIN_BAR_H + 6;
    set_rect(r, x, top, x + b[i].width, top + AU_DEVWIN_BUTTON_H);
}

void au_devwin_toggle_rect(const AuDevWin *dw, int i, AuDevRect *r)
{
    const AuDevWinToggle *t;
    int n = au_devwin_toggles(dw->kind, &t);
    if (i < 0 || i >= n) {
        set_rect(r, 0, 0, 0, 0);
        return;
    }
    if (dw->kind == AU_DEVWIN_REPORT) {
        /* At the right of the band's second line. */
        int w = t[i].width, left = dw->width - w - 8 > 330 ? dw->width - w - 8 : 330;
        set_rect(r, left, 20, left + w, 20 + AU_DEVWIN_TOGGLE_H);
        return;
    }
    /* After the filter field. */
    int x = kinds[dw->kind].filter ? FIELD_X + FIELD_W + 14 : 0;
    for (int k = 0; k < i; k++)
        x += t[k].width + 6;
    set_rect(r, x, 4, x + t[i].width, 4 + AU_DEVWIN_TOGGLE_H);
}

static int clamp0(int v)
{
    return v < 0 ? 0 : v;
}

static void layout(AuDevWin *dw)
{
    AuDevWinAreas a;
    au_devwin_areas(dw, &a);
    au_textview_layout(&dw->tv, dw->fonts, clamp0(a.main.right - a.main.left), clamp0(a.main.bottom - a.main.top));
    if (a.has_detail)
        au_textview_layout(&dw->dv, dw->fonts, clamp0(a.detail.right - a.detail.left),
                           clamp0(a.detail.bottom - a.detail.top));
}

void au_devwin_resize(AuDevWin *dw, int width, int height)
{
    dw->width = width;
    dw->height = height;
    if (dw->kind == AU_DEVWIN_NETWORK && dw->built)
        au_devwin_refresh(dw, true); /* the rows are cut to the new width */
    layout(dw);
}

/* ---- Content ---- */

static void set_title(AuDevWin *dw, AuPage *p)
{
    char t[sizeof dw->title];
    const char *url = p ? au_page_url(p) : "";
    snprintf(t, sizeof t, "%s%s%.150s", kinds[dw->kind].title, url[0] ? ": " : "", url);
    if (strcmp(t, dw->title) != 0) {
        memcpy(dw->title, t, sizeof t);
        if (dw->host.title_changed)
            dw->host.title_changed(dw->host.ctx, dw->title);
    }
}

const char *au_devwin_title(const AuDevWin *dw)
{
    return dw->title;
}

static void set_footer(AuDevWin *dw, const char *text)
{
    snprintf(dw->footer, sizeof dw->footer, "%s", text);
}

/* The view's filter and options, from the tool band. */
static void network_view(AuDevWin *dw)
{
    snprintf(dw->nv.filter, sizeof dw->nv.filter, "%s", au_buf_cstr(&dw->filter));
    dw->nv.show = AU_NETSHOW_ALL;
    for (int i = 0; i < dw->ntoggles; i++)
        if (dw->toggle_on[i])
            dw->nv.show = i;
    AuBuf hd;
    au_buf_init(&hd);
    au_netlog_header_sorted(dw->nv.sort == AU_NETCOL_SEQ && !dw->nv.desc ? -1 : dw->nv.sort, dw->nv.desc, &hd);
    snprintf(dw->header, sizeof dw->header, "%s", au_buf_cstr(&hd));
    au_buf_free(&hd);
}

static const AuNetRecord *selected_in(const AuDevWin *dw, const AuNetLog *l)
{
    if (!l || dw->kind != AU_DEVWIN_NETWORK || dw->tv.selected < 0 || (uint32_t)dw->tv.selected >= dw->nv.n)
        return NULL;
    return au_netlog_at(l, dw->nv.rows[dw->tv.selected]);
}

const AuNetRecord *au_devwin_network_selected(const AuDevWin *dw)
{
    AuPage *p = cur_page(dw);
    return selected_in(dw, p ? au_page_netlog(p) : NULL);
}

static void network_footer(AuDevWin *dw, const AuNetLog *l, const AuNetRecord *sel)
{
    AuBuf f;
    au_buf_init(&f);
    if (sel) {
        /* The whole address of the selected request: the row may cut it. */
        au_buf_puts(&f, sel->url);
    } else if (l) {
        /* Short: the lower pane has the whole summary. */
        if (dw->nv.n != l->count)
            au_buf_printf(&f, "Showing %lu of ", (unsigned long)dw->nv.n);
        au_buf_printf(&f, "%lu requests, %lu blocked, %lu failed, ", (unsigned long)l->requests,
                      (unsigned long)l->blocked, (unsigned long)l->failed);
        au_put_size(&f, l->decoded_bytes);
        if (l->requests > l->finished)
            au_buf_printf(&f, ", %lu still loading", (unsigned long)(l->requests - l->finished));
    }
    set_footer(dw, au_buf_cstr(&f));
    au_buf_free(&f);
}

/* The lower pane: the selected request's details, or with nothing
 * selected the summary and the problem requests. */
static void network_detail(AuDevWin *dw, AuPage *p)
{
    const AuNetLog *l = p ? au_page_netlog(p) : NULL;
    const AuNetRecord *r = selected_in(dw, l);
    uint32_t seq = r ? r->seq : 0;
    uint32_t old_top = seq == dw->detail_seq ? dw->dv.top : 0;
    dw->detail_seq = seq;
    if (r) {
        au_dev_network_detail_lines(r, &dw->dlist);
    } else {
        AuBuf conn;
        au_buf_init(&conn);
        connections(dw, &conn);
        au_dev_network_summary_lines(l, au_buf_cstr(&conn), &dw->dlist);
        au_buf_free(&conn);
    }
    dw->dv.top = old_top;
    network_footer(dw, l, r);
}

void au_devwin_network_select(AuDevWin *dw, int32_t row)
{
    if (dw->kind != AU_DEVWIN_NETWORK)
        return;
    dw->tv.selected = row >= 0 && (uint32_t)row < dw->nv.n ? row : -1;
    network_detail(dw, cur_page(dw));
    layout(dw);
}

/* Shows the report text in the main view. */
static void show_report(AuDevWin *dw, bool markdown)
{
    if (markdown)
        au_dev_markdown_lines(au_buf_cstr(&dw->text), &dw->list);
    else
        au_dev_report_lines(au_buf_cstr(&dw->text), &dw->list);
}

bool au_devwin_stale(const AuDevWin *dw)
{
    AuPage *p = cur_page(dw);
    uint32_t now = now_ms(dw);
    bool page_changed = (const void *)p != dw->page;
    switch (dw->kind) {
    case AU_DEVWIN_SOURCE: {
        const char *d = NULL;
        size_t n = 0;
        bool ok = p && au_page_source(p, &d, &n);
        if (!page_changed)
            return ok && (d != dw->src_data || n != dw->src_len);
        /* While a page loads its source is not there yet: keep the old
         * text until the new one is. */
        return ok || !p || !au_page_loading(p);
    }
    case AU_DEVWIN_CONSOLE: return page_changed || (p ? au_page_console_total(p) : 0) != dw->seen;
    case AU_DEVWIN_NETWORK: {
        if (page_changed)
            return true;
        const AuNetLog *l = p ? au_page_netlog(p) : NULL;
        /* A loading page changes the log many times a second. */
        return (l ? l->generation : 0) != dw->seen && now - dw->last_ms >= 250;
    }
    case AU_DEVWIN_PERF: return page_changed || (now - dw->last_ms >= 1000 && !au_textview_has_selection(&dw->tv));
    case AU_DEVWIN_STATUS: return now - dw->last_ms >= 2000 && !au_textview_has_selection(&dw->tv);
    default: return false;
    }
}

static void build_source(AuDevWin *dw, AuPage *p)
{
    const char *d = NULL;
    size_t n = 0;
    bool ok = p && au_page_source(p, &d, &n);
    if (dw->have_sv)
        au_sourceview_free(&dw->sv);
    au_sourceview_init(&dw->sv, ok ? d : "", ok ? n : 0);
    dw->have_sv = true;
    dw->src_data = ok ? d : NULL;
    dw->src_len = ok ? n : 0;
    AuTextSource src;
    au_sourceview_source(&dw->sv, &src);
    au_textview_set_source(&dw->tv, &src);
    set_title(dw, ok ? p : NULL);
    if (ok) {
        AuBuf f;
        au_buf_init(&f);
        au_put_count(&f, dw->sv.nnumbers);
        au_buf_puts(&f, " lines, ");
        au_put_size(&f, (uint32_t)n);
        set_footer(dw, au_buf_cstr(&f));
        au_buf_free(&f);
    } else {
        set_footer(dw, "This page has no source to show.");
    }
}

static void build_console(AuDevWin *dw, AuPage *p, bool page_changed)
{
    bool end = au_textview_at_end(&dw->tv);
    uint32_t hidden = 0;
    if (p) {
        static const unsigned bits[AU_DEVWIN_MAX_TOGGLES] = {AU_CONSOLE_SHOW_ERRORS, AU_CONSOLE_SHOW_WARNINGS,
                                                             AU_CONSOLE_SHOW_LOGS};
        AuConsoleFilter f;
        f.show = 0;
        for (int i = 0; i < dw->ntoggles; i++)
            if (dw->toggle_on[i])
                f.show |= bits[i];
        f.text = au_buf_cstr(&dw->filter);
        hidden = au_dev_console_lines(p, &f, &dw->list);
    } else {
        au_textlist_clear(&dw->list);
    }
    dw->seen = p ? au_page_console_total(p) : 0;
    set_title(dw, p);
    unsigned long count = (unsigned long)(p ? au_page_console_count(p) : 0);
    if (hidden)
        snprintf(dw->footer, sizeof dw->footer, "%lu entries, %lu hidden by the filter.", count,
                 (unsigned long)hidden);
    else
        snprintf(dw->footer, sizeof dw->footer, "%lu entries. Return runs the expression in the page.", count);
    layout(dw);
    if (end || page_changed)
        au_textview_scroll_to(&dw->tv, au_textview_max_top(&dw->tv));
}

static void build_network(AuDevWin *dw, AuPage *p, bool page_changed)
{
    const AuNetLog *l = p ? au_page_netlog(p) : NULL;
    bool end = au_textview_at_end(&dw->tv) && dw->tv.selected < 0;
    if (page_changed)
        dw->detail_seq = 0;
    network_view(dw);
    au_netview_build(&dw->nv, l);
    layout(dw); /* the columns the rows may take */
    if (l)
        au_dev_network_lines(l, &dw->nv, dw->tv.cols, &dw->list);
    else
        au_textlist_clear(&dw->list);
    /* The selected request keeps its selection through a new order. */
    dw->tv.selected = -1;
    for (uint32_t i = 0; l && dw->detail_seq && i < dw->nv.n; i++)
        if (au_netlog_at(l, dw->nv.rows[i])->seq == dw->detail_seq)
            dw->tv.selected = (int32_t)i;
    dw->seen = l ? l->generation : 0;
    set_title(dw, p);
    layout(dw);
    network_detail(dw, p);
    layout(dw);
    if (end)
        au_textview_scroll_to(&dw->tv, au_textview_max_top(&dw->tv));
}

static void build_perf(AuDevWin *dw, AuPage *p, uint32_t now)
{
    AuBuf conn;
    au_buf_init(&conn);
    au_buf_clear(&dw->text);
    if (p) {
        AuSysInfo sys;
        AuDevHostInfo h;
        char machine[256];
        host_info(dw, &sys, &h, &conn, machine, sizeof machine);
        h.timing = dw->host.timing ? dw->host.timing(dw->host.ctx, p) : NULL;
        au_dev_perf_report(p, &h, now, &dw->text);
    } else {
        au_buf_puts(&dw->text, "Performance\n  No page is open. Open a browser window, then choose Develop > Performance.\n");
    }
    show_report(dw, false);
    au_buf_free(&conn);
    set_title(dw, p);
    set_footer(dw, "Updates every second; waits while text is selected.");
}

static void build_status(AuDevWin *dw)
{
    AuSysInfo sys;
    AuDevHostInfo h;
    AuBuf conn, shell;
    char machine[256];
    au_buf_init(&conn);
    au_buf_init(&shell);
    host_info(dw, &sys, &h, &conn, machine, sizeof machine);
    if (dw->host.status_sections)
        dw->host.status_sections(dw->host.ctx, &shell);
    const AuPaintBench *bench = dw->host.bench ? dw->host.bench(dw->host.ctx) : NULL;
    au_buf_clear(&dw->text);
    au_sysinfo_report(&sys, dw->browser, &h, au_buf_cstr(&shell), bench, &dw->text);
    show_report(dw, false);
    au_buf_free(&conn);
    au_buf_free(&shell);
    set_title(dw, NULL);
    set_footer(dw, "Updates every two seconds; waits while text is selected.");
}

static void build_report(AuDevWin *dw)
{
    AuSysInfo sys;
    AuDevHostInfo h;
    AuBugReport br;
    AuBuf conn;
    char machine[256];
    au_buf_init(&conn);
    host_info(dw, &sys, &h, &conn, machine, sizeof machine);
    memset(&br, 0, sizeof br);
    br.browser = dw->browser;
    br.window = dw->host.window ? dw->host.window(dw->host.ctx) : NULL;
    br.sys = &sys;
    br.host = &h;
    br.other_tabs = dw->ntoggles && dw->toggle_on[0];
    au_buf_clear(&dw->text);
    au_bugreport_build(&br, &dw->text);
    show_report(dw, true);
    au_buf_free(&conn);
    set_title(dw, NULL);
    AuBuf f;
    au_buf_init(&f);
    au_put_count(&f, (unsigned long)dw->text.len);
    snprintf(dw->footer, sizeof dw->footer, "%s characters. Nothing is sent.", au_buf_cstr(&f));
    au_buf_free(&f);
    dw->tv.top = 0;
    au_textview_clear_selection(&dw->tv);
}

bool au_devwin_refresh(AuDevWin *dw, bool force)
{
    /* Each call, so a page loaded since (or a new tab) follows the box. */
    if (dw->kind == AU_DEVWIN_CONSOLE)
        au_page_set_trace(cur_page(dw), trace_flags(dw));
    if (!force && !au_devwin_stale(dw))
        return false;
    AuPage *p = cur_page(dw);
    uint32_t now = now_ms(dw);
    bool page_changed = (const void *)p != dw->page;
    switch (dw->kind) {
    case AU_DEVWIN_SOURCE: build_source(dw, p); break;
    case AU_DEVWIN_CONSOLE: build_console(dw, p, page_changed); break;
    case AU_DEVWIN_NETWORK: build_network(dw, p, page_changed); break;
    case AU_DEVWIN_PERF: build_perf(dw, p, now); break;
    case AU_DEVWIN_STATUS: build_status(dw); break;
    case AU_DEVWIN_REPORT: build_report(dw); break;
    default: return false;
    }
    dw->page = p;
    dw->last_ms = now;
    dw->built = true;
    if (dw->kind != AU_DEVWIN_CONSOLE && dw->kind != AU_DEVWIN_NETWORK)
        layout(dw);
    return true;
}

/* ---- Copy All and Save ---- */

void au_devwin_all_text(AuDevWin *dw, AuBuf *out)
{
    AuPage *p = cur_page(dw);
    switch (dw->kind) {
    case AU_DEVWIN_NETWORK:
        if (p) {
            AuBuf conn;
            au_buf_init(&conn);
            connections(dw, &conn);
            au_netlog_dump(au_page_netlog(p), au_buf_cstr(&conn), true, out);
            au_buf_free(&conn);
        }
        break;
    case AU_DEVWIN_CONSOLE:
        if (p)
            au_dev_console_text(p, out);
        break;
    case AU_DEVWIN_PERF:
    case AU_DEVWIN_STATUS:
    case AU_DEVWIN_REPORT: au_buf_append(out, dw->text.data ? dw->text.data : "", dw->text.len); break;
    default: au_textview_all_text(&dw->tv, out); break;
    }
}

void au_devwin_save_text(const AuDevWin *dw, AuBuf *out)
{
    for (size_t i = 0; i < dw->text.len; i++) {
        if (dw->text.data[i] == '\n')
            au_buf_putc(out, '\r');
        au_buf_putc(out, dw->text.data[i]);
    }
}

static AuDevWinResult copy_all(AuDevWin *dw)
{
    AuBuf out;
    au_buf_init(&out);
    au_devwin_all_text(dw, &out);
    bool ok = out.len && dw->host.copy && dw->host.copy(dw->host.ctx, out.data, out.len);
    au_buf_free(&out);
    return ok ? AU_DEVWIN_DONE : AU_DEVWIN_FAILED;
}

static AuDevWinResult save_report(AuDevWin *dw)
{
    AuBuf out;
    au_buf_init(&out);
    au_devwin_save_text(dw, &out);
    if (out.len && dw->host.save_text)
        dw->host.save_text(dw->host.ctx, "Save the report as:", "Aura Problem Report.txt", out.data, out.len);
    au_buf_free(&out);
    return AU_DEVWIN_DONE;
}

/* ---- Find ---- */

bool au_devwin_find_next(AuDevWin *dw, bool forward)
{
    if (!dw->find || !au_textview_find(&dw->tv, dw->find, forward))
        return false;
    if (dw->kind == AU_DEVWIN_NETWORK)
        network_detail(dw, cur_page(dw));
    layout(dw);
    return true;
}

bool au_devwin_find(AuDevWin *dw, const char *text)
{
    if (!text || !*text)
        return false;
    char *copy = au_strndup(text, strlen(text));
    au_free(dw->find);
    dw->find = copy;
    return au_devwin_find_next(dw, true);
}

bool au_devwin_can_find_again(const AuDevWin *dw)
{
    return dw->find != NULL;
}

AuDevWinResult au_devwin_ask_find(AuDevWin *dw)
{
    if (!dw->host.ask_text)
        return AU_DEVWIN_NOTHING;
    char *text = dw->host.ask_text(dw->host.ctx, "Find text in this window:", dw->find ? dw->find : "");
    if (!text || !*text) {
        au_free(text);
        return AU_DEVWIN_NOTHING;
    }
    au_free(dw->find);
    dw->find = text;
    return au_devwin_find_next(dw, true) ? AU_DEVWIN_DONE : AU_DEVWIN_FAILED;
}

/* ---- Buttons and toggles ---- */

AuDevWinResult au_devwin_run(AuDevWin *dw, AuDevWinAction action)
{
    const AuDevWinButton *b;
    int n = au_devwin_buttons(dw->kind, &b);
    bool has = false;
    for (int i = 0; i < n; i++)
        has |= b[i].action == action;
    if (!has)
        return AU_DEVWIN_NOTHING;
    switch (action) {
    case AU_DEVWIN_COPY_ALL: return copy_all(dw);
    case AU_DEVWIN_FIND: return au_devwin_ask_find(dw);
    case AU_DEVWIN_CLEAR: {
        AuPage *p = cur_page(dw);
        if (!p)
            return AU_DEVWIN_NOTHING;
        au_page_console_clear(p);
        au_devwin_refresh(dw, true);
        return AU_DEVWIN_DONE;
    }
    case AU_DEVWIN_RUN_TIMING:
        if (dw->host.run_timing)
            dw->host.run_timing(dw->host.ctx);
        return AU_DEVWIN_DONE;
    case AU_DEVWIN_REFRESH: au_devwin_refresh(dw, true); return AU_DEVWIN_DONE;
    case AU_DEVWIN_SAVE: return save_report(dw);
    case AU_DEVWIN_OPEN_ISSUES:
        if (dw->host.open_url)
            dw->host.open_url(dw->host.ctx, AU_BUGREPORT_ISSUES_URL);
        return AU_DEVWIN_DONE;
    default: return AU_DEVWIN_NOTHING;
    }
}

void au_devwin_toggle(AuDevWin *dw, int which)
{
    const AuDevWinToggle *t;
    int n = au_devwin_toggles(dw->kind, &t);
    if (which < 0 || which >= n)
        return;
    if (t[which].radio) {
        for (int i = 0; i < n; i++)
            if (t[i].radio)
                dw->toggle_on[i] = i == which;
    } else {
        dw->toggle_on[which] = !dw->toggle_on[which];
    }
    au_devwin_refresh(dw, true);
}

/* ---- The Console input line ---- */

AuDevWinResult au_devwin_console_run(AuDevWin *dw)
{
    AuPage *p = cur_page(dw);
    if (!p)
        return AU_DEVWIN_FAILED;
    if (!dw->input.len)
        return AU_DEVWIN_NOTHING;
    char *expr = au_strndup(dw->input.data, dw->input.len);
    if (dw->nhistory == AU_DEVWIN_HISTORY) {
        au_free(dw->history[0]);
        memmove(dw->history, dw->history + 1, (AU_DEVWIN_HISTORY - 1) * sizeof dw->history[0]);
        dw->nhistory--;
    }
    dw->history[dw->nhistory++] = au_strndup(expr, strlen(expr));
    dw->history_at = dw->nhistory;
    au_buf_clear(&dw->input);
    if (dw->host.busy)
        dw->host.busy(dw->host.ctx, true);
    au_dev_console_eval(p, expr);
    if (dw->host.busy)
        dw->host.busy(dw->host.ctx, false);
    au_free(expr);
    au_devwin_refresh(dw, true);
    au_textview_scroll_to(&dw->tv, au_textview_max_top(&dw->tv));
    return AU_DEVWIN_DONE;
}

void au_devwin_console_recall(AuDevWin *dw, int dir)
{
    int at = dw->history_at + dir;
    if (at < 0 || at > dw->nhistory)
        return;
    dw->history_at = at;
    au_buf_clear(&dw->input);
    if (at < dw->nhistory)
        au_buf_puts(&dw->input, dw->history[at]);
}

/* ---- Keys ---- */

/* An edit of a one-line field: Delete, Escape or typed text. Returns
 * true when the text changed. */
static bool field_edit(AuBuf *field, int key, const char *text, size_t cap)
{
    if (key == AU_KEY_BACKSPACE) {
        /* The last character, whole. */
        size_t n = field->len;
        if (!n)
            return false;
        while (n && ((unsigned char)field->data[n - 1] & 0xC0) == 0x80)
            n--;
        field->len = n ? n - 1 : 0;
        return true;
    }
    if (key == AU_KEY_ESCAPE) {
        if (!field->len)
            return false;
        au_buf_clear(field);
        return true;
    }
    if (key == AU_KEY_NONE && text && (unsigned char)text[0] >= 0x20 && text[0] != 0x7F && field->len < cap) {
        au_buf_puts(field, text);
        return true;
    }
    return false;
}

static bool scroll_key(int key)
{
    return key == AU_KEY_UP || key == AU_KEY_DOWN || key == AU_KEY_PAGE_UP || key == AU_KEY_PAGE_DOWN ||
           key == AU_KEY_HOME || key == AU_KEY_END;
}

static void key_in_list(AuDevWin *dw, int key)
{
    int32_t was = dw->tv.selected;
    au_textview_key(&dw->tv, key);
    if (dw->kind == AU_DEVWIN_NETWORK && dw->tv.selected != was) {
        network_detail(dw, cur_page(dw));
        layout(dw);
    }
}

AuDevWinResult au_devwin_key(AuDevWin *dw, int key, const char *text)
{
    const AuDevWinKindInfo *info = &kinds[dw->kind];
    bool scroll = scroll_key(key);
    if (dw->kind == AU_DEVWIN_CONSOLE && key == AU_KEY_TAB) {
        dw->filter_focus = !dw->filter_focus;
        return AU_DEVWIN_DONE;
    }
    /* The filter field: in the Network window it is the only place to
     * type, so it takes every character. */
    bool to_filter = info->filter && (dw->filter_focus || dw->kind == AU_DEVWIN_NETWORK);
    if (to_filter && !scroll && key != AU_KEY_RETURN) {
        if (field_edit(&dw->filter, key, text, AU_DEVWIN_FILTER_MAX)) {
            dw->filter_focus = true;
            au_devwin_refresh(dw, true);
            return AU_DEVWIN_DONE;
        }
        return key == AU_KEY_BACKSPACE || key == AU_KEY_ESCAPE ? AU_DEVWIN_DONE : AU_DEVWIN_NOTHING;
    }
    if (dw->kind == AU_DEVWIN_CONSOLE) {
        if (key == AU_KEY_RETURN) {
            if (dw->filter_focus) {
                dw->filter_focus = false; /* Return leaves the filter */
            } else if (au_devwin_console_run(dw) == AU_DEVWIN_FAILED) {
                return AU_DEVWIN_FAILED;
            }
        } else if (key == AU_KEY_UP || key == AU_KEY_DOWN) {
            au_devwin_console_recall(dw, key == AU_KEY_UP ? -1 : 1);
        } else if (scroll) {
            au_textview_key(&dw->tv, key);
        } else if (!field_edit(&dw->input, key, text, AU_DEVWIN_INPUT_MAX)) {
            return AU_DEVWIN_NOTHING;
        }
        return AU_DEVWIN_DONE;
    }
    if (!scroll)
        return AU_DEVWIN_NOTHING;
    key_in_list(dw, key);
    return AU_DEVWIN_DONE;
}

bool au_devwin_paste(AuDevWin *dw, const char *text)
{
    if (!kinds[dw->kind].filter)
        return false;
    bool filter = dw->filter_focus || dw->kind == AU_DEVWIN_NETWORK;
    AuBuf *field = filter ? &dw->filter : &dw->input;
    size_t cap = filter ? AU_DEVWIN_FILTER_MAX : AU_DEVWIN_INPUT_MAX;
    size_t n = text ? strlen(text) : 0;
    if (n && field->len + n < cap) {
        for (size_t i = 0; i < n; i++) {
            char c = text[i];
            au_buf_putc(field, c == '\r' || c == '\n' || c == '\t' ? ' ' : c);
        }
    }
    if (filter)
        au_devwin_refresh(dw, true);
    return true;
}

bool au_devwin_copy_text(AuDevWin *dw, AuBuf *out)
{
    AuTextView *tv = au_textview_has_selection(&dw->dv) && !dw->dv.select_rows ? &dw->dv : &dw->tv;
    size_t before = out->len;
    return au_textview_selection_text(tv, out) && out->len > before;
}

bool au_devwin_select_all(AuDevWin *dw)
{
    if (dw->tv.select_rows)
        return false;
    au_textview_select_all(&dw->tv);
    return true;
}

/* ---- The mouse ---- */

static bool in_rect(const AuDevRect *r, int x, int y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

AuDevWinPane au_devwin_pane_at(const AuDevWin *dw, int x, int y)
{
    AuDevWinAreas a;
    au_devwin_areas(dw, &a);
    if (a.has_filter && in_rect(&a.filter, x, y))
        return AU_DEVWIN_PANE_FILTER;
    if (a.has_head && in_rect(&a.head, x, y))
        return AU_DEVWIN_PANE_HEAD;
    if (in_rect(&a.main, x, y))
        return AU_DEVWIN_PANE_MAIN;
    if (a.has_detail && in_rect(&a.detail, x, y))
        return AU_DEVWIN_PANE_DETAIL;
    if (a.has_input && in_rect(&a.input, x, y))
        return AU_DEVWIN_PANE_INPUT;
    return AU_DEVWIN_PANE_NONE;
}

AuTextView *au_devwin_view(AuDevWin *dw, AuDevWinPane pane)
{
    if (pane == AU_DEVWIN_PANE_MAIN)
        return &dw->tv;
    if (pane == AU_DEVWIN_PANE_DETAIL && kinds[dw->kind].detail)
        return &dw->dv;
    return NULL;
}

static const AuDevRect *pane_rect(const AuDevWinAreas *a, AuDevWinPane pane)
{
    return pane == AU_DEVWIN_PANE_DETAIL ? &a->detail : &a->main;
}

AuDevWinPane au_devwin_mouse_down(AuDevWin *dw, int x, int y, int clicks, bool shift)
{
    AuDevWinAreas a;
    au_devwin_areas(dw, &a);
    /* The filter field takes the keyboard when clicked; a click in the
     * text gives it back to the console's input line. */
    if (a.has_filter) {
        bool in_field = in_rect(&a.filter, x, y);
        if (in_field != dw->filter_focus && (in_field || dw->kind == AU_DEVWIN_CONSOLE))
            dw->filter_focus = in_field;
        if (in_field)
            return AU_DEVWIN_PANE_FILTER;
    }
    /* A column title sorts the requests by it. */
    if (a.has_head && in_rect(&a.head, x, y) && dw->tv.cell_w > 0) {
        au_netview_click_column(&dw->nv, au_netlog_column_at((int)((float)(x - dw->tv.gutter) / dw->tv.cell_w)));
        au_devwin_refresh(dw, true);
        return AU_DEVWIN_PANE_HEAD;
    }
    AuDevWinPane pane = AU_DEVWIN_PANE_NONE;
    if (in_rect(&a.main, x, y))
        pane = AU_DEVWIN_PANE_MAIN;
    else if (a.has_detail && in_rect(&a.detail, x, y))
        pane = AU_DEVWIN_PANE_DETAIL;
    else
        return AU_DEVWIN_PANE_NONE;
    AuTextView *tv = au_devwin_view(dw, pane);
    const AuDevRect *r = pane_rect(&a, pane);
    /* One selection at a time between the two panes. */
    if (tv == &dw->tv && !dw->dv.select_rows)
        dw->dv.has_sel = false;
    else if (tv == &dw->dv && !dw->tv.select_rows)
        dw->tv.has_sel = false;
    int32_t was = dw->tv.selected;
    au_textview_mouse_down(tv, x - r->left, y - r->top, clicks, shift);
    if (tv == &dw->tv && dw->kind == AU_DEVWIN_NETWORK && dw->tv.selected != was) {
        network_detail(dw, cur_page(dw));
        layout(dw);
    }
    return pane;
}

bool au_devwin_mouse_drag(AuDevWin *dw, AuDevWinPane pane, int x, int y)
{
    AuTextView *tv = au_devwin_view(dw, pane);
    if (!tv)
        return false;
    AuDevWinAreas a;
    au_devwin_areas(dw, &a);
    const AuDevRect *r = pane_rect(&a, pane);
    return au_textview_mouse_drag(tv, x - r->left, y - r->top);
}

void au_devwin_mouse_up(AuDevWin *dw, AuDevWinPane pane)
{
    AuTextView *tv = au_devwin_view(dw, pane);
    if (tv)
        au_textview_mouse_up(tv);
}

void au_devwin_scroll(AuDevWin *dw, AuDevWinPane pane, int key)
{
    AuTextView *tv = au_devwin_view(dw, pane);
    if (!tv)
        return;
    bool rows = tv->select_rows;
    tv->select_rows = false;
    au_textview_key(tv, key);
    tv->select_rows = rows;
}

void au_devwin_scroll_to(AuDevWin *dw, AuDevWinPane pane, uint32_t top)
{
    AuTextView *tv = au_devwin_view(dw, pane);
    if (tv)
        au_textview_scroll_to(tv, top);
}
