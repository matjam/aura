/* The browser's AuPageFetcher (src/browser/page.h) on the loader: see the
 * notes in loader.h. Only page.h's fetcher types are used from
 * src/browser. */
#include "loader.h"
#include "blocklist.h"
#include "tls.h"
#include "../browser/page.h"

#include <stdio.h>
#include <string.h>

/* page.h mirrors three of src/net's enums in the same order. */
typedef char check_errors[(int)AU_FETCH_ERR_CANCELLED == (int)AU_NET_ERR_CANCELLED ? 1 : -1];
typedef char check_phases[(int)AU_FETCH_PHASE_DONE == (int)AU_PHASE_DONE ? 1 : -1];
typedef char check_policies[(int)AU_FETCH_RP_UNSAFE_URL == (int)AU_RP_UNSAFE_URL ? 1 : -1];

typedef struct PfReq {
    struct PfReq *next;
    AuNetPageFetcher *f;
    uint32_t id; /* loader id, also the fetcher id */
    AuFetchSink sink;
    bool done;
} PfReq;

/* A document response handed to the download manager: its bytes are held
 * here until the transfer source attaches the file (au_net_page_fetcher_
 * take_download), then written straight through. */
typedef struct Parked {
    struct Parked *next;
    AuNetPageFetcher *f;
    uint32_t token;
    AuNetDownload *dl;
    AuBuf held;
    AuDownloadSink real;
    bool attached;
} Parked;

struct AuNetPageFetcher {
    AuNetLoader *l;
    PfReq *reqs;
    Parked *parked;
    uint32_t next_token;
    unsigned (*entropy_bits)(void *ctx);
    void *entropy_ctx;
    unsigned entropy_needed;
    const char *(*offline)(void *ctx);
    void *offline_ctx;
};

AuNetPageFetcher *au_net_page_fetcher_new(AuNetLoader *l)
{
    AuNetPageFetcher *f = au_calloc(sizeof *f);
    f->l = l;
    return f;
}

static void parked_free(Parked *p)
{
    if (p->dl)
        au_net_download_stop(p->dl);
    au_buf_free(&p->held);
    au_free(p);
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
    for (Parked *p = f->parked, *n; p; p = n) {
        n = p->next;
        parked_free(p);
    }
    au_free(f);
}

void au_net_page_fetcher_set_entropy(AuNetPageFetcher *f, unsigned (*bits)(void *ctx), void *ctx, unsigned needed)
{
    f->entropy_bits = bits;
    f->entropy_ctx = ctx;
    f->entropy_needed = needed;
}

void au_net_page_fetcher_set_offline(AuNetPageFetcher *f, const char *(*why)(void *ctx), void *ctx)
{
    f->offline = why;
    f->offline_ctx = ctx;
}

/* ---- Conversions ---- */

static AuSecurity security_of(AuLoadSecurity s)
{
    switch (s) {
    case AU_LOAD_SECURITY_SECURE: return AU_SECURITY_SECURE;
    case AU_LOAD_SECURITY_NOT_SECURE: return AU_SECURITY_NOT_SECURE;
    case AU_LOAD_SECURITY_CERT_ERROR: return AU_SECURITY_CERT_ERROR;
    default: return AU_SECURITY_NONE;
    }
}

void au_net_fetch_cert_from(AuFetchCert *out, const AuCertInfo *ci, uint16_t version, const char *cipher)
{
    memset(out, 0, sizeof *out);
    snprintf(out->host, sizeof out->host, "%s", ci->host);
    out->count = ci->count;
    int n = ci->count < AU_CERT_MAX_CHAIN ? ci->count : AU_CERT_MAX_CHAIN;
    if (n > AU_FETCH_CERT_CHAIN)
        n = AU_FETCH_CERT_CHAIN;
    for (int i = 0; i < n; i++) {
        const AuCertEntry *e = &ci->chain[i];
        AuFetchCertEntry *o = &out->chain[i];
        snprintf(o->subject, sizeof o->subject, "%s", e->subject);
        snprintf(o->issuer, sizeof o->issuer, "%s", e->issuer);
        o->not_before = e->not_before;
        o->not_after = e->not_after;
        memcpy(o->sha256, e->sha256, 32);
        snprintf(o->san, sizeof o->san, "%s", e->san);
    }
    out->error = ci->error != 0;
    if (out->error && ci->reason)
        snprintf(out->reason, sizeof out->reason, "%s", ci->reason);
    out->clock_wrong = ci->clock_wrong;
    out->hsts = ci->hsts;
    out->overridden = ci->overridden;
    out->checked_at = ci->checked_at;
    if (version)
        snprintf(out->protocol, sizeof out->protocol, "%s", au_tls_version_name(version));
    snprintf(out->cipher, sizeof out->cipher, "%s", cipher ? cipher : "");
}

static void fill_response(AuFetchResponse *r, const AuLoadInfo *i, AuFetchCert *cert)
{
    memset(r, 0, sizeof *r);
    r->status = i->status;
    r->url = i->url;
    r->content_type = i->mime;
    r->charset = i->charset;
    r->encoding = i->encoding;
    r->content_length = i->content_length;
    r->attachment = i->attachment;
    r->filename = i->filename;
    r->csp = i->csp ? i->csp : "";
    r->x_frame_options = i->x_frame_options ? i->x_frame_options : "";
    r->referrer_policy = i->referrer_policy ? i->referrer_policy : "";
    r->security = security_of(i->security);
    r->from_cache = i->from_cache;
    r->no_store = i->no_store;
    if (i->cert) {
        au_net_fetch_cert_from(cert, i->cert, i->tls_version, i->cipher);
        r->cert = cert;
    }
}

typedef char check_sources[(int)AU_FETCH_FROM_304 == (int)AU_HTTP_FROM_304 ? 1 : -1];

/* Converts the loader's trace of a finished request for page.h's request
 * log. heads receives the response headers as text when with_headers;
 * remote (64 bytes) and proto (16 bytes) hold the strings out points to. */
static void fetch_net_from(AuFetchNet *out, const AuLoadInfo *i, bool with_headers, AuBuf *heads, char *remote,
                           char *proto)
{
    memset(out, 0, sizeof *out);
    for (int k = 0; k <= AU_FETCH_PHASE_DONE; k++)
        out->at[k] = -1;
    remote[0] = proto[0] = 0;
    out->remote = remote;
    out->protocol = proto;
    out->cipher = i->cipher ? i->cipher : "";
    out->request_head = i->request_head ? i->request_head : "";
    out->response_head = "";
    out->blocked_reason = i->blocked_reason ? i->blocked_reason : i->mixed ? "mixed content" : "";
    if (i->tls_version)
        snprintf(proto, 16, "%s", au_tls_version_name(i->tls_version));
    const AuHttpTrace *t = i->trace;
    if (!t)
        return;
    for (int k = 0; k <= AU_FETCH_PHASE_DONE; k++)
        out->at[k] = t->at[k];
    out->source = (AuFetchSource)t->source;
    out->reused = t->reused;
    out->connection = t->connection;
    out->transfer_bytes = t->transfer_bytes;
    out->redirects = t->redirects;
    const uint8_t *a = t->remote.addr;
    if (t->remote.family == 4) {
        snprintf(remote, 64, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], (unsigned)t->remote_port);
    } else if (t->remote.family == 6) {
        size_t n = 0;
        remote[n++] = '[';
        for (int k = 0; k < 16; k += 2)
            n += (size_t)snprintf(remote + n, 64 - n, "%s%x", k ? ":" : "", (unsigned)(a[k] << 8 | a[k + 1]));
        snprintf(remote + n, 64 - n, "]:%u", (unsigned)t->remote_port);
    }
    if (with_headers && i->response_headers) {
        const AuHeaders *h = i->response_headers;
        for (size_t k = 0; k < h->n && heads->len < 4096; k++)
            au_buf_printf(heads, "%s: %s\n", h->items[k].name, h->items[k].value);
        out->response_head = au_buf_cstr(heads);
    }
}

static AuFetchError error_of(AuNetError e)
{
    /* The two enums list the same errors in the same order. */
    return (AuFetchError)e;
}

/* ---- Loader callbacks ---- */

static void pf_response(void *ctx, uint32_t id, const AuLoadInfo *info)
{
    PfReq *r = ctx;
    if (!r->sink.response)
        return;
    AuFetchCert *cert = au_alloc(sizeof *cert);
    AuFetchResponse resp;
    fill_response(&resp, info, cert);
    r->sink.response(r->sink.ctx, id, &resp);
    au_free(cert);
}

static void pf_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    PfReq *r = ctx;
    if (r->sink.data)
        r->sink.data(r->sink.ctx, id, bytes, len);
}

static void pf_done(void *ctx, uint32_t id, AuNetError err, const AuLoadInfo *info)
{
    PfReq *r = ctx;
    AuNetPageFetcher *f = r->f;
    r->done = true;
    AuFetchInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.url = info->url && info->url[0] ? info->url : NULL;
    fi.content_type = info->mime && info->mime[0] ? info->mime : NULL;
    fi.error = error_of(err);
    fi.status = info->status;
    fi.mixed = info->mixed;
    fi.blocked_reason = info->blocked_reason;
    fi.message = "";
    /* No TCP/IP at all reads better as such than as a DNS failure. */
    if (f->offline && (err == AU_NET_ERR_DNS || err == AU_NET_ERR_CONNECT)) {
        const char *why = f->offline(f->offline_ctx);
        if (why) {
            fi.error = AU_FETCH_ERR_NO_NETWORK;
            fi.message = why;
        }
    }
    fi.cause = error_of(info->fallback_cause);
    if (err == AU_NET_ERR_HTTPS_FALLBACK && info->fallback_cause == AU_NET_ERR_TLS && info->cert &&
        info->cert->reason)
        fi.message = info->cert->reason;
    AuNetError cert_err = err == AU_NET_ERR_HTTPS_FALLBACK ? info->fallback_cause : err;
    AuFetchCert *cert = NULL;
    if (info->cert && (cert_err == AU_NET_ERR_CERT || cert_err == AU_NET_ERR_CLOCK)) {
        cert = au_alloc(sizeof *cert);
        au_net_fetch_cert_from(cert, info->cert, info->tls_version, info->cipher);
        fi.cert = cert;
    }
    /* The request log's description of the transfer. */
    AuFetchNet fn;
    AuBuf rh;
    char remote[64], proto[16];
    au_buf_init(&rh);
    fetch_net_from(&fn, info, au_net_trace_headers(au_net_loader_net(f->l)), &rh, remote, proto);
    fi.net = &fn;
    if (r->sink.done)
        r->sink.done(r->sink.ctx, id, err == AU_NET_OK, &fi);
    au_buf_free(&rh);
    au_free(cert);
}

/* ---- Downloads handed off by a page ---- */

static bool park_write(void *ctx, const void *bytes, size_t len)
{
    Parked *p = ctx;
    if (p->attached)
        return p->real.write(p->real.ctx, bytes, len);
    if (p->held.len + len > 1024u * 1024) /* never claimed this far: give up */
        return false;
    au_buf_append(&p->held, bytes, len);
    return true;
}

static bool pf_download(void *ctx, uint32_t id, const AuLoadInfo *info)
{
    PfReq *r = ctx;
    AuNetPageFetcher *f = r->f;
    if (!r->sink.download)
        return false;
    Parked *p = au_calloc(sizeof *p);
    p->f = f;
    p->token = ++f->next_token ? f->next_token : ++f->next_token;
    au_buf_init(&p->held);
    AuFetchCert *cert = au_alloc(sizeof *cert);
    AuFetchResponse resp;
    fill_response(&resp, info, cert);
    bool take = r->sink.download(r->sink.ctx, id, &resp, p->token);
    au_free(cert);
    if (!take) {
        au_free(p);
        return false;
    }
    AuDownloadSink ds = {p, park_write};
    p->dl = au_net_download_adopt(f->l, id, &ds);
    r->done = true; /* no more sink calls */
    if (!p->dl) {
        au_free(p);
        return true; /* the loader cancels the request */
    }
    p->next = f->parked;
    f->parked = p;
    return true;
}

AuNetDownload *au_net_page_fetcher_take_download(AuNetPageFetcher *f, uint32_t token, const AuDownloadSink *sink,
                                                 char **error)
{
    for (Parked *p = f->parked; p; p = p->next) {
        if (p->token != token || p->attached)
            continue;
        p->real = *sink;
        p->attached = true;
        if (p->held.len && !sink->write(sink->ctx, p->held.data, p->held.len)) {
            *error = au_strndup("cannot write the file", 21);
            return NULL;
        }
        au_buf_free(&p->held);
        return p->dl;
    }
    return NULL;
}

void au_net_page_fetcher_release_download(AuNetPageFetcher *f, uint32_t token)
{
    for (Parked **pp = &f->parked; *pp; pp = &(*pp)->next) {
        if ((*pp)->token == token) {
            Parked *p = *pp;
            *pp = p->next;
            parked_free(p);
            return;
        }
    }
}

/* ---- The interface ---- */

static AuRequestDest dest_of(AuFetchKind k, const char *url)
{
    switch (k) {
    case AU_FETCH_DOCUMENT: return AU_DEST_DOCUMENT;
    case AU_FETCH_STYLESHEET: return AU_DEST_STYLE;
    case AU_FETCH_IMAGE: return AU_DEST_IMAGE;
    case AU_FETCH_FONT: return AU_DEST_FONT;
    case AU_FETCH_SCRIPT: return AU_DEST_SCRIPT;
    case AU_FETCH_FRAME: return AU_DEST_IFRAME;
    default: break;
    }
    /* Other: fonts named by their extension (the document view's web
     * fonts), else an empty destination. */
    size_t n = strcspn(url, "?#");
    static const char *const font[] = {".woff", ".woff2", ".ttf", ".otf"};
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++) {
        size_t e = strlen(font[i]);
        if (n > e && au_ieq(url + n - e, e, font[i], e))
            return AU_DEST_FONT;
    }
    return AU_DEST_EMPTY;
}

static uint32_t pf_start(void *ctx, const AuFetchRequest *frq, const AuFetchSink *sink)
{
    AuNetPageFetcher *f = ctx;
    PfReq *r = au_calloc(sizeof *r);
    r->f = f;
    r->sink = *sink;
    AuLoadRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = frq->url;
    rq.dest = dest_of(frq->kind, frq->url);
    rq.document_url = frq->document_url;
    rq.top_level_url = frq->top_level_url;
    rq.omit_credentials = frq->omit_credentials;
    rq.referrer_policy = (AuReferrerPolicy)frq->referrer_policy;
    rq.user_activated = frq->user_activated;
    rq.reload = frq->cache == AU_FETCH_CACHE_BYPASS;
    rq.revalidate = frq->cache == AU_FETCH_CACHE_REVALIDATE;
    rq.history = frq->cache == AU_FETCH_CACHE_HISTORY;
    rq.allow_http_fallback = frq->allow_http_fallback;
    rq.method = frq->method;
    if (frq->body)
        rq.body = au_body_from_bytes(frq->body, frq->body_len, frq->body_type);
    rq.content_type = frq->body_type;
    rq.to_utf8 = frq->kind == AU_FETCH_DOCUMENT || frq->kind == AU_FETCH_FRAME;
    AuLoadCallbacks cb = {r, pf_response, pf_data, pf_done, pf_download};
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

static bool pf_status(void *ctx, uint32_t id, AuFetchStatus *out)
{
    AuNetPageFetcher *f = ctx;
    memset(out, 0, sizeof *out);
    AuHttpPhase ph;
    if (!au_net_loader_phase(f->l, id, &ph))
        return false;
    out->phase = (AuFetchPhase)ph; /* same order */
    if (!au_net_loader_progress(f->l, id, &out->received, &out->total))
        out->total = -1;
    if (f->entropy_bits) {
        out->entropy_bits = f->entropy_bits(f->entropy_ctx);
        out->entropy_needed = f->entropy_needed;
    }
    return true;
}

static bool pf_allow_cert(void *ctx, const char *host, const uint8_t sha256[32])
{
    AuNetPageFetcher *f = ctx;
    return au_net_allow_cert(au_net_loader_net(f->l), host, sha256);
}

static void pf_allow_blocked(void *ctx, const char *url)
{
    AuNetPageFetcher *f = ctx;
    AuBlocklist *bl = au_net_loader_blocklist(f->l);
    AuUrl u;
    if (!bl || !au_url_parse(&u, url, NULL))
        return;
    au_blocklist_allow_session(bl, u.host);
    au_url_free(&u);
}

void au_net_page_fetcher_interface(AuNetPageFetcher *f, struct AuPageFetcher *out)
{
    memset(out, 0, sizeof *out);
    out->ctx = f;
    out->start = pf_start;
    out->cancel = pf_cancel;
    out->poll = pf_poll;
    out->status = pf_status;
    out->allow_cert = pf_allow_cert;
    out->allow_blocked = pf_allow_blocked;
}

AuNetLoader *au_net_page_fetcher_loader(AuNetPageFetcher *f)
{
    return f->l;
}
