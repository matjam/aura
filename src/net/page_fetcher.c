/* The browser's AuPageFetcher (src/browser/page.h) on the loader: see the
 * adapter notes in loader.h. Only page.h's fetcher types are used from
 * src/browser. */
#include "loader.h"
#include "../browser/page.h"

#include <string.h>

#define MAX_NAV_HISTORY 8

typedef struct PfReq {
    struct PfReq *next;
    AuNetPageFetcher *f;
    uint32_t id; /* loader id, also the fetcher id */
    const AuFetchSink *sink;
    bool navigation;
    bool done;
} PfReq;

typedef struct NavRecord {
    char *url;
    AuLoadSecurity security;
    AuCertInfo cert;
    bool have_cert;
} NavRecord;

struct AuNetPageFetcher {
    AuNetLoader *l;
    PfReq *reqs;
    char *doc_url;       /* the current navigation's final URL */
    uint32_t last_activity;
    bool any_activity;
    NavRecord navs[MAX_NAV_HISTORY];
    int next_nav;
};

AuNetPageFetcher *au_net_page_fetcher_new(AuNetLoader *l)
{
    AuNetPageFetcher *f = au_calloc(sizeof *f);
    f->l = l;
    return f;
}

void au_net_page_fetcher_free(AuNetPageFetcher *f)
{
    if (!f)
        return;
    for (PfReq *r = f->reqs, *n; r; r = n) {
        n = r->next;
        if (!r->done)
            au_net_loader_cancel(f->l, r->id);
        au_free(r);
    }
    au_free(f->doc_url);
    for (int i = 0; i < MAX_NAV_HISTORY; i++)
        au_free(f->navs[i].url);
    au_free(f);
}

static uint32_t now_ms(AuNetPageFetcher *f)
{
    const AuNetOps *ops = au_net_ops(au_net_loader_net(f->l));
    return ops->now_ms(ops->ctx);
}

static bool has_ext(const char *url, const char *const *exts)
{
    size_t n = strcspn(url, "?#");
    for (; *exts; exts++) {
        size_t e = strlen(*exts);
        if (n > e && au_ieq(url + n - e, e, *exts, e))
            return true;
    }
    return false;
}

static AuRequestDest guess_dest(const char *url)
{
    static const char *const css[] = {".css", NULL};
    static const char *const js[] = {".js", ".mjs", NULL};
    static const char *const font[] = {".woff", ".woff2", ".ttf", ".otf", NULL};
    if (has_ext(url, css))
        return AU_DEST_STYLE;
    if (has_ext(url, js))
        return AU_DEST_SCRIPT;
    if (has_ext(url, font))
        return AU_DEST_FONT;
    return AU_DEST_IMAGE; /* the docview's other requests are images */
}

static void record_nav(AuNetPageFetcher *f, const AuLoadInfo *info)
{
    NavRecord *n = &f->navs[f->next_nav];
    f->next_nav = (f->next_nav + 1) % MAX_NAV_HISTORY;
    au_free(n->url);
    n->url = au_strndup(info->url, strlen(info->url));
    n->security = info->security;
    n->have_cert = info->cert != NULL;
    if (info->cert)
        n->cert = *info->cert;
}

static void pf_response(void *ctx, uint32_t id, const AuLoadInfo *info)
{
    (void)id;
    PfReq *r = ctx;
    AuNetPageFetcher *f = r->f;
    f->last_activity = now_ms(f);
    if (r->navigation) {
        au_free(f->doc_url);
        f->doc_url = au_strndup(info->url, strlen(info->url));
    }
}

static void pf_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    PfReq *r = ctx;
    r->f->last_activity = now_ms(r->f);
    if (r->sink->data)
        r->sink->data(r->sink->ctx, id, bytes, len);
}

static void pf_done(void *ctx, uint32_t id, AuNetError err, const AuLoadInfo *info)
{
    PfReq *r = ctx;
    AuNetPageFetcher *f = r->f;
    f->last_activity = now_ms(f);
    r->done = true;
    if (r->navigation)
        record_nav(f, info);
    AuFetchInfo fi;
    fi.url = info->url && info->url[0] ? info->url : NULL;
    fi.content_type = info->mime && info->mime[0] ? info->mime : NULL;
    if (r->sink->done)
        r->sink->done(r->sink->ctx, id, err == AU_NET_OK, &fi);
}

static uint32_t pf_start(void *ctx, const char *url, const AuFetchSink *sink)
{
    AuNetPageFetcher *f = ctx;
    uint32_t now = now_ms(f);
    bool outstanding = false;
    for (PfReq *r = f->reqs; r; r = r->next)
        if (!r->done)
            outstanding = true;
    bool quiet = !f->any_activity || (uint32_t)(now - f->last_activity) > AU_FETCHER_QUIET_MS;
    bool navigation = !outstanding && quiet;
    f->any_activity = true;
    f->last_activity = now;
    PfReq *r = au_calloc(sizeof *r);
    r->f = f;
    r->sink = sink;
    r->navigation = navigation || !f->doc_url;
    AuLoadRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    if (r->navigation) {
        rq.dest = AU_DEST_DOCUMENT;
        rq.user_activated = true;
        rq.to_utf8 = true;
        au_free(f->doc_url);
        f->doc_url = au_strndup(url, strlen(url));
    } else {
        rq.dest = guess_dest(url);
        rq.document_url = f->doc_url;
        rq.top_level_url = f->doc_url;
    }
    AuLoadCallbacks cb = {r, pf_response, pf_data, pf_done, NULL};
    r->id = au_net_loader_start(f->l, &rq, &cb);
    r->next = f->reqs;
    f->reqs = r;
    return r->id;
}

static void pf_cancel(void *ctx, uint32_t id)
{
    AuNetPageFetcher *f = ctx;
    for (PfReq *r = f->reqs; r; r = r->next)
        if (r->id == id && !r->done) {
            au_net_loader_cancel(f->l, id);
            r->done = true;
        }
}

static void pf_poll(void *ctx)
{
    AuNetPageFetcher *f = ctx;
    au_net_loader_poll(f->l);
    for (PfReq **pp = &f->reqs; *pp;) {
        PfReq *r = *pp;
        if (r->done) {
            *pp = r->next;
            au_free(r);
        } else {
            pp = &r->next;
        }
    }
}

void au_net_page_fetcher_interface(AuNetPageFetcher *f, struct AuPageFetcher *out)
{
    out->ctx = f;
    out->start = pf_start;
    out->cancel = pf_cancel;
    out->poll = pf_poll;
}

AuLoadSecurity au_net_page_fetcher_security(const AuNetPageFetcher *f, const char *url, const AuCertInfo **cert)
{
    if (cert)
        *cert = NULL;
    for (int k = 1; k <= MAX_NAV_HISTORY; k++) {
        const NavRecord *n = &f->navs[(f->next_nav - k + MAX_NAV_HISTORY) % MAX_NAV_HISTORY];
        if (n->url && !strcmp(n->url, url)) {
            if (cert && n->have_cert)
                *cert = &n->cert;
            return n->security;
        }
    }
    return AU_LOAD_SECURITY_NONE;
}
