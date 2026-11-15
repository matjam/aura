/* aura-host page: loads an HTML file through the document view with
 * scripting on, the way the browser would, and runs its scripts.
 *
 *   aura-host page [--quiet] [--expect-errors N] [--chunk BYTES]
 *                  [--click ID]... [--no-script] PAGE.html
 *
 * The page is fed to the parser in chunks (default 256 bytes, so parser
 * pauses and document.write meet chunk boundaries); resources come from
 * the file: loader. Then the loop runs scripts and delivers resources on a
 * virtual clock (each round advances it by 50 ms) until the page has
 * loaded and nothing has run for ten virtual seconds. --click ID presses
 * and releases the mouse over the centre of element ID after the load.
 *
 * console output goes to stdout ("error: ", "warn: " prefixes); the view's
 * link requests print as "open: URL". The exit status is 1 when the
 * number of console errors differs from --expect-errors (default 0) or a
 * line starts with "FAIL". */
#include "host.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct PageRun {
    bool quiet;
    int errors, fails;
    bool loaded;
} PageRun;

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

int au_host_page_main(int argc, char **argv)
{
    PageRun run;
    memset(&run, 0, sizeof run);
    int expect = 0;
    size_t chunk = 256;
    bool scripting = true;
    const char *page = NULL;
    const char *clicks[16];
    int nclicks = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--quiet"))
            run.quiet = true;
        else if (!strcmp(argv[i], "--expect-errors") && i + 1 < argc)
            expect = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc)
            chunk = (size_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--click") && i + 1 < argc && nclicks < 16)
            clicks[nclicks++] = argv[++i];
        else if (!strcmp(argv[i], "--no-script"))
            scripting = false;
        else if (argv[i][0] != '-' && !page)
            page = argv[i];
        else {
            fprintf(stderr, "usage: aura-host page [--quiet] [--expect-errors N] [--chunk BYTES] [--click ID]... "
                            "[--no-script] PAGE.html\n");
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
    cb.open_url = c_open;
    cb.load_finished = c_loaded;
    AuDocView *dv = au_docview_new(fonts, &li, &cb);
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
    if (!run.loaded)
        printf("FAIL the page never finished loading\n");
    if (run.fails == 0 && !run.loaded)
        run.fails++;
    au_docview_free(dv); /* unload */
    au_host_loader_free(loader);
    au_fontset_free(fonts);
    au_free(data);
    if (run.errors != expect)
        printf("aura-host page: %d console errors, %d expected\n", run.errors, expect);
    return run.errors != expect || run.fails ? 1 : 0;
}
