/* aura-host render and dump for http and https URLs: the page and its
 * resources through the network stack and the resource loader
 * (src/net/loader.h), then the same document view, layout and paint as
 * the file renderer.
 *
 *   aura-host render URL -w W [-h H] [-s scrollY] -o out.png [font options]
 *                    [--script] [--wait MS] [--eval EXPR]...
 *   aura-host dump URL -w W [-h H] [--script] [--wait MS] [--eval EXPR]...
 *
 * --script runs the page's scripts: after the page and its resources
 * load, timers and fetches run until the page is quiet or MS (default
 * 5000) have passed. Console lines go to stderr as "console: ...",
 * navigations print as "navigate: URL" (not followed), and each --eval
 * prints "eval: RESULT" at the end.
 *
 * Cookies and HSTS state come from and go back to the profile folder
 * (see net_host.h). Prints what was fetched and how long it took. */
#include "host.h"
#include "net/loader.h"
#include "net_host.h"
#include "png.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SECONDS 120 /* give up on resources after this */

typedef struct NetRender {
    AuNetLoader *l;
    AuDocView *dv;
    char *doc_url;
    /* the main document */
    AuBuf body;
    bool main_done;
    AuNetError main_err;
    char encoding[32];
    char mime[64];
    AuLoadSecurity security;
    /* resources */
    int requested, failed, blocked;
    uint32_t next_id;
    int64_t res_bytes;
    AuLoader image_decoder; /* the file loader's decode_image */
} NetRender;

typedef struct ResReq {
    NetRender *nr;
    uint32_t docview_id;
} ResReq;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static bool is_http(const char *s) { return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8); }

bool au_host_net_render_wanted(int argc, char **argv)
{
    for (int i = 2; i < argc; i++)
        if (is_http(argv[i]))
            return true;
    return false;
}

/* ---- The main document ---- */

static void main_response(void *ctx, uint32_t id, const AuLoadInfo *info)
{
    (void)id;
    NetRender *nr = ctx;
    snprintf(nr->encoding, sizeof nr->encoding, "%s", info->encoding);
    snprintf(nr->mime, sizeof nr->mime, "%s", info->mime);
    nr->security = info->security;
}

static void main_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    (void)id;
    NetRender *nr = ctx;
    au_buf_append(&nr->body, bytes, len);
}

static void main_done(void *ctx, uint32_t id, AuNetError err, const AuLoadInfo *info)
{
    (void)id;
    NetRender *nr = ctx;
    nr->main_done = true;
    nr->main_err = err;
    au_free(nr->doc_url);
    nr->doc_url = au_strndup(info->url, strlen(info->url));
    if (info->security != AU_LOAD_SECURITY_NONE)
        nr->security = info->security;
    if (err != AU_NET_OK && info->cert && info->cert->reason)
        fprintf(stderr, "aura-host: certificate: %s\n", info->cert->reason);
}

/* ---- Resources for the document view ---- */

static void res_response(void *ctx, uint32_t id, const AuLoadInfo *info)
{
    (void)id;
    ResReq *r = ctx;
    const char *hdrs[2] = {"content-type", info->mime};
    au_docview_resource_head(r->nr->dv, r->docview_id, info->status ? info->status : 200, "", info->url, hdrs,
                             info->mime && info->mime[0] ? 1 : 0);
}

static void res_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    (void)id;
    ResReq *r = ctx;
    r->nr->res_bytes += (int64_t)len;
    au_docview_resource_data(r->nr->dv, r->docview_id, bytes, len);
}

static void res_done(void *ctx, uint32_t id, AuNetError err, const AuLoadInfo *info)
{
    (void)id;
    ResReq *r = ctx;
    NetRender *nr = r->nr;
    if (err != AU_NET_OK) {
        nr->failed++;
        if (err == AU_NET_ERR_BLOCKED)
            nr->blocked++;
        fprintf(stderr, "  resource failed (%s): %s\n", au_net_error_name(err), info->url);
    }
    au_docview_resource_done(nr->dv, r->docview_id, err == AU_NET_OK);
    au_free(r);
}

static uint32_t res_request(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    (void)dv;
    NetRender *nr = ctx;
    if (nr->requested >= 500)
        return 0;
    ResReq *r = au_calloc(sizeof *r);
    r->nr = nr;
    r->docview_id = ++nr->next_id;
    AuLoadRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.dest = kind == AU_RES_STYLESHEET ? AU_DEST_STYLE
              : kind == AU_RES_IMAGE    ? AU_DEST_IMAGE
              : kind == AU_RES_FONT     ? AU_DEST_FONT
                                        : AU_DEST_EMPTY;
    rq.document_url = nr->doc_url;
    rq.top_level_url = nr->doc_url;
    AuLoadCallbacks cb = {r, res_response, res_data, res_done, NULL};
    au_net_loader_start(nr->l, &rq, &cb);
    nr->requested++;
    /* The loader's id is not needed: the document view's id is ours. */
    return r->docview_id;
}

static bool res_decode(void *ctx, const void *data, size_t len, int *w, int *h, AuSurface **pixels)
{
    NetRender *nr = ctx;
    return nr->image_decoder.decode_image(nr->image_decoder.ctx, data, len, w, h, pixels);
}

static int usage(void)
{
    fprintf(stderr, "usage: aura-host render URL -w W [-h H] [-s scrollY] -o out.png [font options]\n"
                    "                        [--script] [--wait MS] [--eval EXPR]...\n"
                    "       aura-host dump URL -w W [-h H] [--script] [--wait MS] [--eval EXPR]...\n");
    return 2;
}

/* ---- Script callbacks (--script) ---- */

static double cb_now(void *ctx, AuDocView *dv)
{
    (void)ctx, (void)dv;
    return now_ms();
}

static void cb_console(void *ctx, AuDocView *dv, int level, const char *text, size_t len)
{
    (void)ctx, (void)dv;
    static const char *const names[] = {"log", "info", "warn", "error", "debug"};
    fprintf(stderr, "console %s: %.*s\n", names[level >= 0 && level < 5 ? level : 0], (int)len, text);
}

static void cb_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    (void)ctx, (void)dv;
    printf("navigate%s: %s\n", new_tab ? " (new tab)" : "", url);
}

/* Fetches resources and runs scripts until nothing is pending and no
 * script work is due within the wait, or until deadline_ms. */
static void run_until_quiet(NetRender *nr, AuHostSession *ses, bool scripts, double deadline_ms)
{
    for (;;) {
        au_docview_update(nr->dv);
        bool any = au_net_loader_poll(nr->l);
        uint32_t due = UINT32_MAX;
        if (scripts) {
            if (au_docview_run_scripts(nr->dv, 50))
                any = true;
            due = au_docview_scripts_due(nr->dv);
        }
        double t = now_ms();
        bool net_idle = !au_net_loader_active(nr->l) && !any && au_docview_pending(nr->dv) == 0;
        if (net_idle && (due == UINT32_MAX || t + due > deadline_ms))
            break;
        if (t > deadline_ms) {
            fprintf(stderr, "aura-host: still busy at the time limit\n");
            break;
        }
        if (!any)
            au_host_net_wait(ses->host, due < 20 ? (int)due : 20);
    }
    au_docview_update(nr->dv);
}

static const char *security_name(AuLoadSecurity s)
{
    switch (s) {
    case AU_LOAD_SECURITY_SECURE: return "secure";
    case AU_LOAD_SECURITY_NOT_SECURE: return "not secure";
    case AU_LOAD_SECURITY_CERT_ERROR: return "certificate error";
    default: return "none";
    }
}

int au_host_net_render_main(int argc, char **argv)
{
    const char *cmd = argv[1];
    const char *url = NULL, *out = "page.png";
    int w = 800, h = 0, scroll = 0;
    bool scripts = false;
    int wait_ms = 5000;
    const char *evals[16];
    int nevals = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int used = au_host_font_option(argc, argv, i);
        if (used) {
            i += used - 1;
            continue;
        }
        if (!strcmp(a, "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "-w") && i + 1 < argc)
            w = atoi(argv[++i]);
        else if (!strcmp(a, "-h") && i + 1 < argc)
            h = atoi(argv[++i]);
        else if (!strcmp(a, "-s") && i + 1 < argc)
            scroll = atoi(argv[++i]);
        else if (!strcmp(a, "--script"))
            scripts = true;
        else if (!strcmp(a, "--wait") && i + 1 < argc)
            wait_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--eval") && i + 1 < argc && nevals < 16)
            evals[nevals++] = argv[++i];
        else if (is_http(a) && !url)
            url = a;
        else
            return usage();
    }
    if (!url || w <= 0 || w > 8192 || h < 0 || h > 16384)
        return usage();

    AuHostSession ses;
    if (!au_host_session_open(&ses, true, false)) {
        fprintf(stderr, "aura-host: cannot set up sockets\n");
        return 3;
    }
    NetRender nr;
    memset(&nr, 0, sizeof nr);
    au_buf_init(&nr.body);
    nr.l = au_net_loader_new(ses.net);
    AuHostLoader *files = au_host_loader_new();
    au_host_loader_interface(files, &nr.image_decoder);

    /* The page. */
    double t0 = now_ms();
    AuLoadRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.dest = AU_DEST_DOCUMENT;
    rq.user_activated = true;
    rq.to_utf8 = true;
    AuLoadCallbacks cb = {&nr, main_response, main_data, main_done, NULL};
    au_net_loader_start(nr.l, &rq, &cb);
    while (!nr.main_done) {
        if (!au_net_loader_poll(nr.l))
            au_host_net_wait(ses.host, 20);
    }
    double t_main = now_ms() - t0;
    int rc = 0;
    if (nr.main_err != AU_NET_OK) {
        fprintf(stderr, "aura-host: %s: %s\n", url, au_net_error_name(nr.main_err));
        rc = 3;
        goto done;
    }
    printf("%s: %zu bytes (%s, encoding %s, %s) in %.0f ms\n", nr.doc_url, nr.body.len, nr.mime,
           nr.encoding[0] ? nr.encoding : "-", security_name(nr.security), t_main);

    AuFontSet *fonts = au_fontset_new();
    au_platform_load_fonts(fonts);
    AuLoader li = {&nr, res_request, NULL, res_decode};
    AuDocViewCallbacks dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.open_url = cb_open;
    dcb.now_ms = cb_now;
    dcb.console = cb_console;
    nr.dv = au_docview_new(fonts, &li, &dcb);
    au_docview_set_scripting(nr.dv, scripts);
    au_docview_set_viewport(nr.dv, w, h ? h : 600);
    au_docview_load(nr.dv, nr.body.data ? nr.body.data : "", nr.body.len, nr.doc_url);
    /* Resources until none is pending (layout requests images), then
     * scripts until the page is quiet. */
    t0 = now_ms();
    run_until_quiet(&nr, &ses, false, t0 + MAX_SECONDS * 1000.0);
    if (scripts) {
        double ts = now_ms();
        run_until_quiet(&nr, &ses, true, ts + wait_ms);
        for (int i = 0; i < nevals; i++) {
            AuBuf o;
            au_buf_init(&o);
            bool ok = au_docview_eval(nr.dv, evals[i], &o);
            printf("eval%s: %s\n", ok ? "" : " (error)", au_buf_cstr(&o));
            au_buf_free(&o);
        }
        au_docview_update(nr.dv);
    }
    double t_res = now_ms() - t0;
    printf("  %d resources (%d failed, %d blocked as mixed content or by type), %lld bytes, %.0f ms\n", nr.requested,
           nr.failed, nr.blocked, (long long)nr.res_bytes, t_res);

    int cw, chh;
    au_docview_content_size(nr.dv, &cw, &chh);
    if (!strcmp(cmd, "dump")) {
        AuBuf b;
        au_buf_init(&b);
        au_layout_dump(au_docview_layout(nr.dv), &b);
        fwrite(b.data, 1, b.len, stdout);
        au_buf_free(&b);
    } else {
        int ph = h ? h : (chh > 16384 ? 16384 : chh);
        if (ph <= 0)
            ph = 1;
        AuSurface *s = au_surface_new(w, ph);
        AuCanvas *c = au_canvas_new(s);
        au_docview_paint(nr.dv, c, (AuRect){0, 0, (float)w, (float)ph}, 0, scroll);
        /* Background images are requested when first painted: fetch them
         * and paint again. */
        for (int round = 0; round < 3 && au_docview_pending(nr.dv) > 0; round++) {
            double tp = now_ms();
            while (au_docview_pending(nr.dv) > 0 && now_ms() - tp < MAX_SECONDS * 1000.0) {
                bool any = au_net_loader_poll(nr.l);
                if (!any)
                    au_host_net_wait(ses.host, 20);
            }
            au_docview_update(nr.dv);
            au_docview_paint(nr.dv, c, (AuRect){0, 0, (float)w, (float)ph}, 0, scroll);
        }
        if (!au_png_write(out, s)) {
            fprintf(stderr, "aura-host: cannot write %s\n", out);
            rc = 1;
        }
        printf("%s: %dx%d (document %dx%d)\n", out, w, ph, cw, chh);
        au_canvas_free(c);
        au_surface_free(s);
    }
    au_docview_free(nr.dv);
    au_fontset_free(fonts);
done:
    au_net_loader_free(nr.l);
    au_host_loader_free(files);
    au_buf_free(&nr.body);
    au_free(nr.doc_url);
    au_host_session_close(&ses);
    return rc;
}
