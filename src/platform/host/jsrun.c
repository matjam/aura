/* aura-host js: runs JavaScript files against a document.
 *
 *   aura-host js [--html PAGE.html] [--quiet] [--mem MB] [--kill MS]
 *                [--stack KB] [--expect-errors N] [--module] [--negative TYPE] [--stats]
 *                [--clock MS] [--zone SECONDS] FILE.js...
 *
 * --clock gives Date a fixed time (UTC milliseconds since 1970) and --zone
 * a fixed zone (seconds east of UTC) through au_js_set_clock, as the Mac
 * OS 9 application supplies its own; without them Date reads the host.
 *
 * --stats prints the runtime's start-up time and memory, and its memory
 * after the run.
 * --module loads the last file with import(); --negative TYPE passes only
 * when exactly one uncaught TYPE error is reported (test262's negative
 * tests).
 *
 * The files run in order as classic scripts in one window, against an
 * empty document or the parsed PAGE (parsed without running its scripts;
 * page scripts run through src/page, see `aura-host page`). Then the event
 * loop runs until nothing is left (timers jump ahead on a virtual clock,
 * so tests with long timeouts finish at once). file: URLs load through a
 * fetcher that reads local files; other schemes fail.
 *
 * console output goes to stdout ("error: " and "warn: " prefixes). The
 * exit status is 1 when the number of uncaught errors reported differs
 * from --expect-errors (default 0) or a line starts with "FAIL", else 0.
 * The garbage collector runs between files. */
#include "host.h"
#include "js/js.h"
#include "js/clock.h"
#include "html/html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef struct HostReq {
    uint32_t id;
    AuJs *js; /* who asked */
    char *url;
    struct HostReq *next;
} HostReq;

typedef struct JsRun {
    double skipped;     /* virtual time added while waiting for timers */
    double start;
    bool quiet;
    int fails, errors;
    const char *negative; /* --negative TYPE: the test must throw TYPE */
    int negative_seen;
    HostReq *reqs, **tail;
    uint32_t next_id;
} JsRun;

static double real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double h_now(void *ctx)
{
    JsRun *r = ctx;
    return real_ms() - r->start + r->skipped;
}

/* crypto.getRandomValues: the system's random device. */
static bool h_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return false;
    size_t got = len ? fread(buf, 1, len, f) : 0;
    fclose(f);
    return got == len;
}

static void h_console(void *ctx, AuJsLogLevel level, const char *text, size_t len)
{
    JsRun *r = ctx;
    bool is_fail = len >= 4 && !memcmp(text, "FAIL", 4);
    if (r->negative && (level == AU_JS_ERROR || is_fail)) {
        /* "Uncaught TYPE: ..." or "FAIL TYPE: ..." */
        size_t tl = strlen(r->negative);
        const char *p = is_fail ? text + 5 : text + 9;
        size_t rest = (size_t)(text + len - p);
        if ((is_fail ? len > 5 : len > 9) && rest > tl && !memcmp(p, r->negative, tl) && p[tl] == ':') {
            r->negative_seen++;
            return;
        }
    }
    if (level == AU_JS_ERROR)
        r->errors++;
    if (is_fail)
        r->fails++;
    if (r->quiet && level != AU_JS_ERROR && !(len >= 4 && !memcmp(text, "FAIL", 4)))
        return;
    const char *pre = level == AU_JS_ERROR ? "error: " : level == AU_JS_WARN ? "warn: " : "";
    printf("%s%.*s\n", pre, (int)len, text);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 65536;
            buf = au_realloc(buf, cap);
        }
        size_t got = fread(buf + n, 1, 4096, f);
        n += got;
        if (got < 4096)
            break;
    }
    fclose(f);
    buf[n] = 0;
    *len = n;
    return buf;
}

static uint32_t h_fetch_start(void *ctx, AuJs *js, const AuFetchRequest *req)
{
    JsRun *r = ctx;
    if (strncmp(req->url, "file:", 5) != 0)
        return 0;
    HostReq *q = au_calloc(sizeof *q);
    q->js = js;
    q->id = ++r->next_id;
    q->url = au_strndup(req->url, strlen(req->url));
    *r->tail = q;
    r->tail = &q->next;
    return q->id;
}

static void h_fetch_abort(void *ctx, uint32_t id)
{
    JsRun *r = ctx;
    for (HostReq **pp = &r->reqs; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) {
            HostReq *q = *pp;
            *pp = q->next;
            if (!*pp)
                r->tail = pp;
            au_free(q->url);
            au_free(q);
            return;
        }
}

/* file:///a/b%20c?x -> /a/b c */
static char *file_path(const char *url)
{
    const char *p = url + 5;
    if (!strncmp(p, "//", 2)) {
        p += 2;
        while (*p && *p != '/')
            p++;
    }
    size_t n = strcspn(p, "?#");
    char *out = au_alloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '%' && i + 2 < n + 1 && p[i + 1] && p[i + 2]) {
            char hex[3] = {p[i + 1], p[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = p[i];
        }
    }
    out[o] = 0;
    return out;
}

static bool deliver_one(JsRun *r, AuJs *js)
{
    HostReq *q = r->reqs;
    if (!q)
        return false;
    js = q->js; /* the document's runtime, or the worker that asked */
    r->reqs = q->next;
    if (!r->reqs)
        r->tail = &r->reqs;
    char *path = file_path(q->url);
    size_t len = 0;
    char *data = read_file(path, &len);
    if (data) {
        size_t pl = strlen(path);
#define ENDS(s) (pl >= sizeof(s) - 1 && !strcmp(path + pl - (sizeof(s) - 1), s))
        const char *ct = ENDS(".js") || ENDS(".mjs") ? "text/javascript" : ENDS(".json") ? "application/json"
                         : ENDS(".html") ? "text/html" : "text/plain";
#undef ENDS
        const char *hdr[2] = {"content-type", ct};
        au_js_fetch_head(js, q->id, 200, "OK", hdr, 1, NULL);
        au_js_fetch_data(js, q->id, data, len);
        au_js_fetch_done(js, q->id, true);
    } else {
        au_js_fetch_done(js, q->id, false);
    }
    au_free(data);
    au_free(path);
    au_free(q->url);
    au_free(q);
    return true;
}

static int usage(void)
{
    fprintf(stderr, "usage: aura-host js [--html PAGE.html] [--quiet] [--mem MB] [--kill MS] [--stack KB]\n"
                    "                    [--expect-errors N] [--module] [--negative TYPE] [--stats]\n"
                    "                    [--clock MS] [--zone SECONDS] FILE.js...\n");
    return 2;
}

static char *file_url(const char *path)
{
    char buf[4096];
    if (path[0] == '/')
        snprintf(buf, sizeof buf, "file://%s", path);
    else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd))
            cwd[0] = 0;
        snprintf(buf, sizeof buf, "file://%s/%s", cwd, path);
    }
    return au_strndup(buf, strlen(buf));
}

/* Runs the event loop until it is idle (or a minute of virtual time). */
void au_host_js_loop(AuJs *js, void *run_ctx, double (*now)(void *), void (*skip)(void *, double),
                     bool (*deliver)(void *, AuJs *))
{
    for (int guard = 0; guard < 1000000; guard++) {
        bool did = false;
        while (deliver && deliver(run_ctx, js))
            did = true;
        if (au_js_run_tasks(js, 50))
            did = true;
        if (au_js_run_workers(js, 20))
            did = true;
        if (au_js_wants_frame(js)) {
            au_js_run_frame(js);
            au_js_layout_done(js);
            did = true;
        }
        if (did || au_js_workers_busy(js))
            continue;
        if (au_js_idle(js))
            break;
        double next = au_js_next_timer(js);
        double t = now(run_ctx);
        if (next >= 0 && next > t) {
            if (next > 600000)
                break; /* nothing but far timers left */
            skip(run_ctx, next - t);
            continue;
        }
        if (next < 0)
            break; /* waiting on something that will not come */
    }
}

static void h_skip(void *ctx, double ms)
{
    ((JsRun *)ctx)->skipped += ms;
}

static bool h_deliver(void *ctx, AuJs *js)
{
    return deliver_one(ctx, js);
}

/* The fixed clock and zone of --clock and --zone. */
static int64_t g_fake_ms;
static int32_t g_fake_zone;

static int64_t fake_wall_ms(void *ctx)
{
    (void)ctx;
    return g_fake_ms;
}

static int32_t fake_zone(void *ctx, int64_t utc_ms)
{
    (void)ctx;
    (void)utc_ms;
    return g_fake_zone;
}

static int js_main(int argc, char **argv)
{
    AuJsClock clock = {0};
    JsRun run;
    memset(&run, 0, sizeof run);
    run.tail = &run.reqs;
    run.start = real_ms();
    const char *html = NULL;
    AuJsSettings s;
    au_js_settings_default(&s);
    s.cookies_enabled = true;
    s.online = true;
    int first = 0;
    int expect_errors = 0;
    bool no_coro = false;
    bool as_module = false, stats = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--html") && i + 1 < argc)
            html = argv[++i];
        else if (!strcmp(argv[i], "--quiet"))
            run.quiet = true;
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc)
            s.memory_limit = (size_t)atoi(argv[++i]) * 1024 * 1024;
        else if (!strcmp(argv[i], "--kill") && i + 1 < argc)
            s.kill_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stack") && i + 1 < argc)
            s.stack_limit = (size_t)atoi(argv[++i]) * 1024;
        else if (!strcmp(argv[i], "--no-coro"))
            no_coro = true; /* workers run on this stack, a task at a time */
        else if (!strcmp(argv[i], "--expect-errors") && i + 1 < argc)
            expect_errors = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--module"))
            as_module = true;
        else if (!strcmp(argv[i], "--negative") && i + 1 < argc)
            run.negative = argv[++i];
        else if (!strcmp(argv[i], "--stats"))
            stats = true;
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) {
            g_fake_ms = strtoll(argv[++i], NULL, 10);
            clock.wall_ms = fake_wall_ms;
        } else if (!strcmp(argv[i], "--zone") && i + 1 < argc) {
            g_fake_zone = (int32_t)atoi(argv[++i]);
            clock.utc_offset_s = fake_zone;
        }
        else if (argv[i][0] == '-')
            return usage();
        else if (!first)
            first = i;
    }
    if (!first)
        return usage();
    if (clock.wall_ms || clock.utc_offset_s)
        au_js_set_clock(&clock);
    AuDocument *doc;
    char *doc_url;
    if (html) {
        size_t len;
        char *src = read_file(html, &len);
        if (!src) {
            fprintf(stderr, "aura-host js: cannot read %s\n", html);
            return 1;
        }
        AuHtmlOptions o;
        memset(&o, 0, sizeof o);
        o.scripting = true;
        doc = au_html_parse(src, len, &o);
        au_free(src);
        doc_url = file_url(html);
    } else {
        static const char blank[] = "<!DOCTYPE html><html><head><title>test</title></head><body></body></html>";
        AuHtmlOptions o;
        memset(&o, 0, sizeof o);
        o.scripting = true;
        doc = au_html_parse(blank, sizeof blank - 1, &o);
        doc_url = file_url(argv[first]);
    }
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.ctx = &run;
    h.now_ms = h_now;
    h.console = h_console;
    h.random = h_random;
    h.coro = no_coro ? NULL : au_host_coro_ops();
    h.fetcher.ctx = &run;
    h.fetcher.start = h_fetch_start;
    h.fetcher.abort = h_fetch_abort;
    double t_new = real_ms();
    AuJs *js = au_js_new(doc, doc_url, &s, &h);
    if (!js) {
        fprintf(stderr, "aura-host js: cannot start the JavaScript runtime\n");
        au_node_unref(&doc->node);
        au_free(doc_url);
        return 1;
    }
    AuJsStats st;
    if (stats) {
        t_new = real_ms() - t_new;
        au_js_stats(js, &st);
        printf("stats startup %.2f ms, %zu bytes\n", t_new, st.memory_used);
        AuBuf rep;
        au_buf_init(&rep);
        au_js_memory_report(js, &rep);
        printf("%s", au_buf_cstr(&rep));
        au_buf_free(&rep);
    }
    au_js_set_ready_state(js, "interactive");
    for (int i = first; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (!strcmp(argv[i], "--html") || !strcmp(argv[i], "--mem") || !strcmp(argv[i], "--kill") ||
                !strcmp(argv[i], "--stack") || !strcmp(argv[i], "--expect-errors") || !strcmp(argv[i], "--clock") ||
                !strcmp(argv[i], "--zone"))
                i++;
            continue;
        }
        /* A collection between files lets tests check what survives one
         * (WeakRef targets, wrappers of nodes in the tree). */
        if (i > first)
            au_js_gc(js);
        /* --module: the last file is a module, loaded with import() (the
         * earlier ones stay classic, for setup). A rejection fails. */
        if (as_module && i == argc - 1) {
            char *url = file_url(argv[i]);
            char drv[4500];
            int n = snprintf(drv, sizeof drv,
                             "import(%s%s%s).catch((e) => console.log('FAIL ' + e + (e && e.stack ? '\\n' + e.stack : '')));",
                             "\"", url, "\"");
            au_js_run_classic(js, NULL, drv, (size_t)n, "<driver>", 1);
            au_free(url);
            continue;
        }
        size_t len;
        char *src = read_file(argv[i], &len);
        if (!src) {
            fprintf(stderr, "aura-host js: cannot read %s\n", argv[i]);
            run.errors++;
            continue;
        }
        char *url = file_url(argv[i]);
        au_js_run_classic(js, NULL, src, len, url, 1);
        au_free(url);
        au_free(src);
    }
    au_js_set_ready_state(js, "complete");
    au_host_js_loop(js, &run, h_now, h_skip, h_deliver);
    if (stats) {
        au_js_stats(js, &st);
        printf("stats after run %zu bytes, %u wrappers", st.memory_used, st.wrappers);
        au_js_gc(js);
        au_js_stats(js, &st);
        printf(", after gc %zu bytes, peak %zu bytes\n", st.memory_used, st.memory_peak);
        AuBuf rep;
        au_buf_init(&rep);
        au_js_memory_report(js, &rep);
        printf("%s", au_buf_cstr(&rep));
        au_buf_free(&rep);
    }
    au_js_unload(js);
    au_js_free(js);
    while (run.reqs)
        h_fetch_abort(&run, run.reqs->id);
    au_node_unref(&doc->node);
    au_free(doc_url);
    fflush(stdout);
    if (run.errors != expect_errors)
        printf("aura-host js: %d uncaught errors reported, %d expected\n", run.errors, expect_errors);
    if (run.negative && run.negative_seen != 1)
        printf("aura-host js: expected one uncaught %s, saw %d\n", run.negative, run.negative_seen);
    return run.errors != expect_errors || run.fails || (run.negative && run.negative_seen != 1) ? 1 : 0;
}

typedef struct JsMainArgs {
    int argc;
    char **argv;
    int rc;
} JsMainArgs;

static void *js_thread(void *p)
{
    JsMainArgs *a = p;
    a->rc = js_main(a->argc, a->argv);
    return NULL;
}

/* Scripts run on a thread with a large stack: the default script stack
 * limit is 512 KB, and sanitizer builds raise it (their frames are many
 * times larger), more than the main thread may have. */
int au_host_js_main(int argc, char **argv)
{
    JsMainArgs a = {argc, argv, 1};
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)256 << 20);
    if (pthread_create(&t, &attr, js_thread, &a) != 0) {
        pthread_attr_destroy(&attr);
        return js_main(argc, argv);
    }
    pthread_join(t, NULL);
    pthread_attr_destroy(&attr);
    return a.rc;
}
