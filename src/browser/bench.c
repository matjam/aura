/* The page benchmark (bench.h). */
#include "bench.h"

#include "page.h"
#include "sysinfo.h"

#include "../page/page.h" /* au_url_resolve_dup */

#include <stdlib.h>
#include <string.h>

#define MAX_PAGES 200

typedef enum { B_MANIFEST, B_START, B_LOADING, B_SETTLING, B_POST, B_POSTING, B_DONE, B_FAILED } BState;

struct AuBench {
    AuBrowserWindow *w;
    uint32_t tab_id;
    char *manifest;
    char *post; /* NULL: post nothing */
    char *pages[MAX_PAGES];
    int npages;
    int repeat;
    uint32_t settle_ms, timeout_ms;
    int page, round; /* the load under way */
    BState state;
    uint32_t started, loaded;
    /* The manifest or the post under way. */
    const AuPageFetcher *fetcher;
    uint32_t fetch_id; /* 0 inside start, before the id is known */
    bool fetching, fetch_done, fetch_ok;
    int fetch_status;
    AuBuf body;
    AuBuf out;
    char *error;
};

static uint32_t now_ms(AuBench *b)
{
    AuBrowser *br = b->w->browser;
    return br->host.time_ms ? br->host.time_ms(br->host.ctx) : 0;
}

static AuTab *bench_tab(AuBench *b)
{
    for (int i = 0; i < b->w->ntabs; i++)
        if (b->w->tabs[i]->id == b->tab_id)
            return b->w->tabs[i];
    return NULL;
}

static void put_escaped(AuBuf *o, const char *s)
{
    for (; *s; s++) {
        if (*s == '<')
            au_buf_puts(o, "&lt;");
        else if (*s == '&')
            au_buf_puts(o, "&amp;");
        else
            au_buf_putc(o, *s);
    }
}

/* Shows the run's state in the tab. */
static void show(AuBench *b, const char *heading)
{
    AuTab *t = bench_tab(b);
    if (!t)
        return;
    AuBuf h;
    au_buf_init(&h);
    au_buf_puts(&h, "<!DOCTYPE html><title>Benchmark</title><h1>");
    put_escaped(&h, heading);
    au_buf_puts(&h, "</h1><p>Manifest: ");
    put_escaped(&h, b->manifest);
    au_buf_puts(&h, "</p>");
    if (b->error) {
        au_buf_puts(&h, "<p><b>");
        put_escaped(&h, b->error);
        au_buf_puts(&h, "</b></p>");
    }
    if (b->out.len) {
        au_buf_puts(&h, "<pre>");
        put_escaped(&h, au_buf_cstr(&b->out));
        au_buf_puts(&h, "</pre>");
    }
    au_page_load_internal(t->page, "about:bench", h.data, h.len);
    au_buf_free(&h);
}

static void fail(AuBench *b, const char *why)
{
    au_free(b->error);
    b->error = au_strndup(why, strlen(why));
    b->state = B_FAILED;
    show(b, "Benchmark failed");
}

/* ---- Fetching the manifest and posting the results ---- */

static bool ours(const AuBench *b, uint32_t id)
{
    return b->fetching && !b->fetch_done && (!b->fetch_id || id == b->fetch_id);
}

static void f_response(void *ctx, uint32_t id, const AuFetchResponse *r)
{
    AuBench *b = ctx;
    if (ours(b, id))
        b->fetch_status = r->status;
}

static void f_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    AuBench *b = ctx;
    if (ours(b, id) && b->body.len < 256 * 1024)
        au_buf_append(&b->body, bytes, len);
}

static void f_done(void *ctx, uint32_t id, bool ok, const AuFetchInfo *info)
{
    (void)info;
    AuBench *b = ctx;
    if (!ours(b, id))
        return;
    b->fetch_done = true;
    b->fetch_ok = ok && (b->fetch_status == 0 || (b->fetch_status >= 200 && b->fetch_status < 300));
}

static bool fetch(AuBench *b, const char *url, const char *method, const AuBuf *body)
{
    const char *colon = strchr(url, ':');
    char scheme[16];
    size_t n = colon ? (size_t)(colon - url) : 0;
    if (!n || n >= sizeof scheme)
        return false;
    memcpy(scheme, url, n);
    scheme[n] = 0;
    AuBrowser *br = b->w->browser;
    b->fetcher = NULL;
    for (int i = 0; i < br->nfetchers; i++)
        if (!strcmp(br->fetchers[i].scheme, scheme))
            b->fetcher = &br->fetchers[i].fetcher;
    if (!b->fetcher || !b->fetcher->start)
        return false;
    AuFetchRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.kind = AU_FETCH_OTHER;
    rq.cache = AU_FETCH_CACHE_BYPASS;
    rq.no_store = true;
    rq.method = method;
    if (body) {
        rq.body = body->data;
        rq.body_len = body->len;
        rq.body_type = "text/tab-separated-values; charset=utf-8";
    }
    AuFetchSink sink;
    memset(&sink, 0, sizeof sink);
    sink.ctx = b;
    sink.response = f_response;
    sink.data = f_data;
    sink.done = f_done;
    au_buf_clear(&b->body);
    b->fetch_done = b->fetch_ok = false;
    b->fetch_status = 0;
    b->fetch_id = 0;
    b->fetching = true;
    uint32_t id = b->fetcher->start(b->fetcher->ctx, &rq, &sink);
    /* A fetcher may finish (a file, a refusal) inside start. */
    if (!id && !b->fetch_done) {
        b->fetching = false;
        return false;
    }
    b->fetch_id = b->fetch_done ? 0 : id;
    return true;
}

/* True once the fetch under way has finished. */
static bool fetch_finished(AuBench *b)
{
    if (!b->fetch_done && b->fetcher && b->fetcher->poll)
        b->fetcher->poll(b->fetcher->ctx);
    if (!b->fetch_done)
        return false;
    b->fetch_id = 0;
    b->fetching = false;
    return true;
}

static uint32_t number(const char *s, uint32_t lo, uint32_t hi, uint32_t def)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s)
        return def;
    return v < lo ? lo : v > hi ? hi : (uint32_t)v;
}

static void parse_manifest(AuBench *b, const char *text)
{
    char *post = NULL;
    bool no_post = false;
    while (*text) {
        const char *eol = strchr(text, '\n');
        size_t len = eol ? (size_t)(eol - text) : strlen(text);
        char *line = au_strndup(text, len);
        text += len + (eol ? 1 : 0);
        /* Trim spaces and a CR. */
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
            s[--n] = 0;
        if (!*s || *s == '#') {
            /* nothing */
        } else if (!strncmp(s, "repeat ", 7)) {
            b->repeat = (int)number(s + 7, 1, 50, 3);
        } else if (!strncmp(s, "settle ", 7)) {
            b->settle_ms = number(s + 7, 0, 600000, 1000);
        } else if (!strncmp(s, "timeout ", 8)) {
            b->timeout_ms = number(s + 8, 1000, 3600000, 120000);
        } else if (!strncmp(s, "post ", 5)) {
            const char *v = s + 5;
            while (*v == ' ')
                v++;
            au_free(post);
            post = NULL;
            no_post = !strcmp(v, "none");
            if (!no_post)
                post = au_url_resolve_dup(b->manifest, v);
        } else if (b->npages < MAX_PAGES) {
            char *u = au_url_resolve_dup(b->manifest, s);
            /* Not another run: navigating to one would replace this run
             * from inside its own idle. */
            if (u && !strncmp(u, AU_BENCH_PREFIX, strlen(AU_BENCH_PREFIX))) {
                au_free(u);
                u = NULL;
            }
            if (u)
                b->pages[b->npages++] = u;
        }
        au_free(line);
    }
    if (!no_post && !post)
        post = au_url_resolve_dup(b->manifest, "result");
    b->post = no_post ? NULL : post;
    if (no_post)
        au_free(post);
}

/* ---- Loads ---- */

static void put_header(AuBench *b)
{
    au_buf_puts(&b->out, "# aura-bench 1\n# build ");
    au_buf_puts(&b->out, au_build_identity());
    au_buf_puts(&b->out, "\n# manifest ");
    au_buf_puts(&b->out, b->manifest);
    au_buf_printf(&b->out, "\n# repeat %d settle %u\n", b->repeat, (unsigned)b->settle_ms);
    au_buf_puts(&b->out, "round\turl\tstatus\twall_ms\tfirst_paint_ms\tload_ms\tscript_ms\tstyle_ms\tlayout_ms\t"
                         "paint_ms\tcopy_ms\tscripts\tstyles\tlayouts\tpaints\tkpixels\tbytes\tresources\tboxes\t"
                         "decodes\tscript_peak_kb\tworker_peak_kb\tfree_kb\n");
}

static void put_row(AuBench *b, AuTab *t, const char *status)
{
    AuPage *p = t->page;
    AuPageStats st;
    memset(&st, 0, sizeof st);
    au_page_stats(p, &st);
    const AuPerf *pf = au_page_perf(p);
    size_t pp = 0, wp = 0;
    au_page_script_peaks(p, &pp, &wp);
    AuBrowser *br = b->w->browser;
    uint32_t free_kb = br->host.memory_free_kb ? br->host.memory_free_kb(br->host.ctx) : 0;
    au_buf_printf(&b->out, "%d\t%s\t%s\t%u\t%u\t%u", b->round + 1, b->pages[b->page], status,
                  (unsigned)(now_ms(b) - b->started), (unsigned)st.first_paint_ms, (unsigned)st.load_ms);
    for (int k = 0; k < AU_PERF_KINDS; k++)
        au_buf_printf(&b->out, "\t%u", pf ? (unsigned)pf->ms[k] : 0u);
    for (int k = 0; k < AU_PERF_COPY; k++)
        au_buf_printf(&b->out, "\t%u", pf ? (unsigned)pf->count[k] : 0u);
    au_buf_printf(&b->out, "\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", pf ? (unsigned)pf->kpixels : 0u,
                  (unsigned)st.bytes, (unsigned)st.resources, (unsigned)st.boxes, (unsigned)st.image_decodes,
                  (unsigned)(pp / 1024), (unsigned)(wp / 1024), (unsigned)free_kb);
}

static void next_load(AuBench *b)
{
    if (++b->page >= b->npages) {
        b->page = 0;
        b->round++;
    }
    b->state = b->round >= b->repeat ? B_POST : B_START;
}

/* ---- The run ---- */

AuBench *au_bench_start(AuBrowserWindow *w, const char *manifest_url)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t || !manifest_url)
        return NULL;
    AuBench *b = au_alloc(sizeof *b);
    memset(b, 0, sizeof *b);
    b->w = w;
    b->tab_id = t->id;
    b->manifest = au_strndup(manifest_url, strlen(manifest_url));
    b->repeat = 3;
    b->settle_ms = 1000;
    b->timeout_ms = 120000;
    au_buf_init(&b->body);
    au_buf_init(&b->out);
    b->state = B_MANIFEST;
    show(b, "Benchmark: fetching the manifest");
    if (!fetch(b, b->manifest, NULL, NULL))
        fail(b, "The manifest could not be requested.");
    return b;
}

void au_bench_free(AuBench *b)
{
    if (!b)
        return;
    if (b->fetch_id && b->fetcher && b->fetcher->cancel)
        b->fetcher->cancel(b->fetcher->ctx, b->fetch_id);
    for (int i = 0; i < b->npages; i++)
        au_free(b->pages[i]);
    au_free(b->manifest);
    au_free(b->post);
    au_free(b->error);
    au_buf_free(&b->body);
    au_buf_free(&b->out);
    au_free(b);
}

const char *au_bench_results(const AuBench *b)
{
    return b && b->out.len ? b->out.data : "";
}

bool au_bench_ok(const AuBench *b)
{
    return b && b->state == B_DONE;
}

bool au_bench_idle(AuBench *b)
{
    if (!b || b->state == B_DONE || b->state == B_FAILED)
        return false;
    AuTab *t = bench_tab(b);
    if (!t) {
        b->state = B_FAILED; /* the tab was closed */
        return false;
    }
    switch (b->state) {
    case B_MANIFEST:
        if (!fetch_finished(b))
            return true;
        if (!b->fetch_ok) {
            fail(b, "The manifest could not be fetched.");
            return false;
        }
        parse_manifest(b, au_buf_cstr(&b->body));
        if (!b->npages) {
            fail(b, "The manifest lists no pages.");
            return false;
        }
        put_header(b);
        b->state = B_START;
        return true;
    case B_START: {
        int i;
        for (i = 0; i < b->w->ntabs && b->w->tabs[i] != t; i++) {
        }
        if (b->w->active != i)
            au_bw_select_tab(b->w, i);
        b->started = now_ms(b);
        au_bw_navigate(b->w, b->pages[b->page]);
        b->state = B_LOADING;
        return true;
    }
    case B_LOADING: {
        uint32_t el = now_ms(b) - b->started;
        AuPageStats st;
        memset(&st, 0, sizeof st);
        au_page_stats(t->page, &st);
        if (!au_page_loading(t->page) && st.load_ms) {
            b->loaded = now_ms(b);
            b->state = B_SETTLING;
        } else if (el >= b->timeout_ms) {
            put_row(b, t, "timeout");
            au_bw_stop(b->w);
            next_load(b);
        }
        return true;
    }
    case B_SETTLING:
        if (now_ms(b) - b->loaded < b->settle_ms)
            return true;
        put_row(b, t, "ok");
        next_load(b);
        return true;
    case B_POST:
        if (!b->post) {
            b->state = B_DONE;
            show(b, "Benchmark finished");
            return false;
        }
        if (!fetch(b, b->post, "POST", &b->out)) {
            fail(b, "The results could not be posted.");
            return false;
        }
        b->state = B_POSTING;
        return true;
    case B_POSTING:
        if (!fetch_finished(b))
            return true;
        if (!b->fetch_ok) {
            fail(b, "The results could not be posted.");
            return false;
        }
        b->state = B_DONE;
        show(b, "Benchmark finished");
        return false;
    default: return false;
    }
}
