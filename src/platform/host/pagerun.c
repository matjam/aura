/* aura-host page: loads an HTML file through the document view with
 * scripting on, the way the browser would, and runs its scripts.
 *
 *   aura-host page [--quiet] [--expect-errors N] [--chunk BYTES]
 *                  [--click ID]... [--no-script] [--real-time]
 *                  [--ref ID FILE.png]... PAGE.html
 *
 * The page is fed to the parser in chunks (default 256 bytes, so parser
 * pauses and document.write meet chunk boundaries); resources come from
 * the file: loader. Then the loop runs scripts and delivers resources on a
 * virtual clock (each round advances it by 50 ms) until the page has
 * loaded and nothing has run for ten virtual seconds. --click ID presses
 * and releases the mouse over the centre of element ID after the load.
 * --real-time runs on the machine's clock instead (a worker's time slices
 * need a clock that moves while script runs): the loop ends after half a
 * second in which nothing ran.
 * --ref ID FILE.png paints the content box of element ID (a canvas, for
 * tests/canvas) as the page shows it and compares it with the PNG: up to
 * 3 levels per channel are allowed anywhere and more than that on at most
 * 0.5% of the pixels (float rounding differs between compilers and CPUs);
 * a mismatch prints a FAIL line and writes FILE-actual.png. With
 * AURA_UPDATE_GOLDEN=1 in the environment it writes FILE instead.
 *
 * console output goes to stdout ("error: ", "warn: " prefixes); the view's
 * link requests print as "open: URL". The exit status is 1 when the
 * number of console errors differs from --expect-errors (default 0) or a
 * line starts with "FAIL". */
#include "host.h"
#include "image/image.h"
#include "image/pngenc.h"
#include "net/web_storage.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct PageRun {
    bool quiet;
    int errors, fails;
    bool loaded;
    bool real_time;
    AuWebStorage *storage;
} PageRun;

static int c_storage(void *ctx, AuDocView *dv, struct AuJsStorageCall *call)
{
    (void)dv;
    return au_web_storage_call(((PageRun *)ctx)->storage, 1, call);
}

/* A change: the storage event for the page's other documents. */
static void c_storage_changed(void *ctx, const AuStorageChange *c)
{
    AuDocStorageEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = c->kind == AU_STORAGE_SESSION ? AU_JS_STORAGE_SESSION : AU_JS_STORAGE_LOCAL;
    ev.storage_key = c->storage_key;
    ev.writer = c->writer;
    ev.key = c->key;
    ev.key_len = c->key_len;
    ev.old_value = c->old_value;
    ev.old_len = c->old_len;
    ev.new_value = c->new_value;
    ev.new_len = c->new_len;
    ev.url = c->url;
    au_docview_storage_event(ctx, &ev);
}

#include <time.h>
static double real_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* crypto for pages: the system's random device. */
static bool c_random(void *ctx, AuDocView *dv, void *buf, size_t len)
{
    (void)ctx;
    (void)dv;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return false;
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len;
}

static double c_now(void *ctx, AuDocView *dv)
{
    (void)ctx;
    (void)dv;
    return real_now_ms();
}

static void c_console(void *ctx, AuDocView *dv, int level, const char *text, size_t len)
{
    (void)dv;
    PageRun *r = ctx;
    bool is_fail = len >= 4 && !memcmp(text, "FAIL", 4);
    if (level == 3)
        r->errors++;
    if (is_fail)
        r->fails++;
    if (r->quiet && level != 3 && !is_fail)
        return;
    printf("%s%.*s\n", level == 3 ? "error: " : level == 2 ? "warn: " : "", (int)len, text);
}

static void c_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    (void)ctx;
    (void)dv;
    printf("open: %s%s\n", url, new_tab ? " (new tab)" : "");
}

static void c_loaded(void *ctx, AuDocView *dv)
{
    (void)dv;
    PageRun *r = ctx;
    r->loaded = true;
}

static void file_url(const char *path, char *out, size_t cap)
{
    char cwd[2048];
    if (path[0] == '/' || !getcwd(cwd, sizeof cwd))
        snprintf(out, cap, "file://%s", path);
    else
        snprintf(out, cap, "file://%s/%s", cwd, path);
}

/* Runs rounds until the page has loaded and idle_limit rounds in a row
 * did nothing. */
static void run_loop(AuDocView *dv, AuHostLoader *loader, PageRun *r, int idle_limit)
{
    int idle = 0;
    if (r->real_time) {
        /* at most two minutes; a millisecond's sleep when nothing ran */
        double end = real_now_ms() + 120000;
        while (idle < 500 && real_now_ms() < end) {
            bool did = au_host_loader_pump(loader, dv);
            au_docview_update(dv);
            if (au_docview_run_scripts(dv, 20))
                did = true;
            idle = did || !r->loaded ? 0 : idle + 1;
            if (!did)
                usleep(1000);
        }
        return;
    }
    for (int round = 0; round < 4000 && idle < idle_limit; round++) {
        bool did = au_host_loader_pump(loader, dv);
        au_docview_update(dv);
        if (au_docview_run_scripts(dv, 50))
            did = true;
        idle = did || !r->loaded ? 0 : idle + 1;
    }
}

static void click(AuDocView *dv, const char *id)
{
    AuElement *el = au_docview_find_fragment(dv, id);
    AuRect rc;
    au_docview_update(dv);
    if (!el || !au_layout_element_rect(au_docview_layout(dv), el, &rc)) {
        printf("FAIL --click %s: no such element with a box\n", id);
        return;
    }
    AuEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.x = (int)(rc.x + rc.w / 2);
    ev.y = (int)(rc.y + rc.h / 2);
    ev.clicks = 1;
    ev.type = AU_EVENT_MOUSE_DOWN;
    au_docview_mouse(dv, &ev);
    ev.type = AU_EVENT_MOUSE_UP;
    au_docview_mouse(dv, &ev);
}

/* Writes s as a compressed PNG (the engine's encoder, so references stay
 * small in the repository). */
static void ref_row(void *ctx, int y, uint8_t *rgba)
{
    const AuSurface *s = ctx;
    for (int x = 0; x < s->width; x++) {
        AuColor c = au_surface_get(s, x, y);
        rgba[4 * x] = (uint8_t)(c >> 16);
        rgba[4 * x + 1] = (uint8_t)(c >> 8);
        rgba[4 * x + 2] = (uint8_t)c;
        rgba[4 * x + 3] = (uint8_t)(c >> 24);
    }
}

static bool ref_write(const char *path, AuSurface *s)
{
    AuBuf b;
    au_buf_init(&b);
    bool ok = au_png_encode_rgba(s->width, s->height, ref_row, s, &b);
    FILE *f = ok ? fopen(path, "wb") : NULL;
    ok = f && fwrite(b.data, 1, b.len, f) == b.len;
    if (f && fclose(f) != 0)
        ok = false;
    au_buf_free(&b);
    return ok;
}

/* --ref: the element's content box, painted, against a PNG. Returns
 * false after printing a FAIL line. */
static bool check_ref(AuDocView *dv, const char *id, const char *file)
{
    AuElement *el = au_docview_find_fragment(dv, id);
    AuRect rc;
    au_docview_update(dv);
    if (!el || !au_layout_element_content_rect(au_docview_layout(dv), el, &rc) || rc.w < 1 || rc.h < 1) {
        printf("FAIL --ref %s: no such element with a box\n", id);
        return false;
    }
    int w = (int)rc.w, h = (int)rc.h;
    AuSurface *s = au_surface_new(w, h);
    AuCanvas *c = au_canvas_new(s);
    au_docview_paint(dv, c, (AuRect){0, 0, (float)w, (float)h}, (int)rc.x, (int)rc.y);
    au_canvas_free(c);
    const char *update = getenv("AURA_UPDATE_GOLDEN");
    bool ok = true;
    if (update && *update == '1') {
        ok = ref_write(file, s);
        printf("%s %s (%dx%d)\n", ok ? "wrote" : "FAIL cannot write", file, w, h);
    } else {
        char *data;
        size_t len;
        AuSurface *g = au_platform_read_file(file, &data, &len) ? au_image_decode_buffer(data, len, NULL, NULL) : NULL;
        if (!g || g->width != w || g->height != h) {
            printf("FAIL --ref %s: %s is missing or not %dx%d\n", id, file, w, h);
            ok = false;
        } else {
            long over = 0;
            int maxd = 0;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint32_t a = s->pixels[(size_t)y * s->stride + x], b = g->pixels[(size_t)y * g->stride + x];
                    int d = 0;
                    for (int k = 0; k < 32; k += 8) {
                        int e = abs((int)((a >> k) & 255) - (int)((b >> k) & 255));
                        if (e > d)
                            d = e;
                    }
                    if (d > 3)
                        over++;
                    if (d > maxd)
                        maxd = d;
                }
            if (over > (long)w * h / 200) {
                char actual[2100];
                snprintf(actual, sizeof actual, "%s-actual.png", file);
                ref_write(actual, s);
                printf("FAIL --ref %s: %ld pixels differ from %s by more than 3 levels (most %d); wrote %s\n", id, over,
                       file, maxd, actual);
                ok = false;
            }
        }
        if (g)
            au_surface_free(g);
    }
    au_surface_free(s);
    return ok;
}

int au_host_page_main(int argc, char **argv)
{
    PageRun run;
    memset(&run, 0, sizeof run);
    setvbuf(stdout, NULL, _IOLBF, 0); /* lines show as they are printed, also in a pipe */
    int expect = 0;
    size_t chunk = 256;
    bool scripting = true;
    const char *page = NULL;
    const char *clicks[16];
    int nclicks = 0;
    const char *refs[32][2];
    int nrefs = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--quiet"))
            run.quiet = true;
        else if (!strcmp(argv[i], "--expect-errors") && i + 1 < argc)
            expect = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc)
            chunk = (size_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--click") && i + 1 < argc && nclicks < 16)
            clicks[nclicks++] = argv[++i];
        else if (!strcmp(argv[i], "--ref") && i + 2 < argc && nrefs < 32) {
            refs[nrefs][0] = argv[++i];
            refs[nrefs++][1] = argv[++i];
        } else if (!strcmp(argv[i], "--no-script"))
            scripting = false;
        else if (!strcmp(argv[i], "--real-time"))
            run.real_time = true;
        else if (argv[i][0] != '-' && !page)
            page = argv[i];
        else {
            fprintf(stderr, "usage: aura-host page [--quiet] [--expect-errors N] [--chunk BYTES] [--click ID]... "
                            "[--no-script] [--ref ID FILE.png]... PAGE.html\n");
            return 2;
        }
    }
    if (!page || chunk == 0) {
        fprintf(stderr, "aura-host page: no page given\n");
        return 2;
    }
    char *data;
    size_t len;
    if (!au_platform_read_file(page, &data, &len)) {
        fprintf(stderr, "aura-host page: cannot read %s\n", page);
        return 1;
    }
    AuFontSet *fonts = au_fontset_new();
    au_host_use_system_fonts(false); /* reproducible geometry */
    au_platform_load_fonts(fonts);
    AuHostLoader *loader = au_host_loader_new();
    AuLoader li;
    au_host_loader_interface(loader, &li);
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &run;
    cb.console = c_console;
    cb.coro = au_host_coro_ops();
    cb.random = c_random;
    if (run.real_time)
        cb.now_ms = c_now;
    cb.open_url = c_open;
    cb.load_finished = c_loaded;
    /* A storage service in memory, so the page and its frames share
     * localStorage as they do in the browser. */
    run.storage = au_web_storage_new();
    cb.storage = c_storage;
    AuDocView *dv = au_docview_new(fonts, &li, &cb);
    au_web_storage_set_listener(run.storage, c_storage_changed, dv);
    au_docview_set_scripting(dv, scripting);
    au_docview_set_viewport(dv, 800, 600);
    char url[4200];
    file_url(page, url, sizeof url);
    au_docview_begin(dv, url);
    for (size_t off = 0; off < len; off += chunk) {
        au_docview_feed(dv, data + off, len - off < chunk ? len - off : chunk);
        /* resources (a blocking script) may arrive between chunks */
        au_host_loader_pump(loader, dv);
    }
    au_docview_finish(dv);
    run_loop(dv, loader, &run, 200);
    for (int i = 0; i < nclicks; i++) {
        click(dv, clicks[i]);
        run_loop(dv, loader, &run, 200);
    }
    for (int i = 0; i < nrefs; i++)
        if (!check_ref(dv, refs[i][0], refs[i][1]))
            run.fails++;
    if (!run.loaded)
        printf("FAIL the page never finished loading\n");
    if (run.fails == 0 && !run.loaded)
        run.fails++;
    au_docview_free(dv); /* unload */
    au_web_storage_free(run.storage);
    au_host_loader_free(loader);
    au_fontset_free(fonts);
    au_free(data);
    if (run.errors != expect)
        printf("aura-host page: %d console errors, %d expected\n", run.errors, expect);
    return run.errors != expect || run.fails ? 1 : 0;
}
