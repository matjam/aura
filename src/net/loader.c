/* The resource loader (see loader.h). */
#include "loader.h"
#include "blocklist.h"
#include "cache.h"
#include "dataurl.h"
#include "policy.h"
#include "../url/url.h"

#include <stdio.h>
#include <string.h>

typedef struct Req Req;

struct AuNetDownload {
    AuNetLoader *l;
    AuHttpTransfer *t; /* own transfer (au_net_download_start) */
    Req *adopted;      /* or an adopted loader request */
    AuDownloadSink sink;
    int64_t offset, received, total;
    bool done, finished_transfer;
    char *error;
};

enum { MODE_PASS, MODE_UTF8, MODE_CP1252 };

struct Req {
    Req *next;
    AuNetLoader *l;
    uint32_t id;
    AuLoadCallbacks cb;
    AuHttpTransfer *t;
    AuRequestDest dest;
    bool to_utf8;
    /* data: URLs and failures before any transfer: delivered by poll */
    bool is_data, early, pending;
    AuNetError early_err;
    AuDataUrl data;
    /* response */
    char *url;
    int status;
    AuBuf mime, charset, filename, csp, rpolicy, xfo;
    char encoding[32];
    char reason[48];
    char cipher[64];
    uint16_t tls_version;
    bool mixed;
    int64_t content_length, received;
    bool from_cache, attachment, nosniff, have_response, response_sent, no_store;
    AuLoadSecurity security;
    AuCertInfo cert;
    bool have_cert;
    AuNetError fallback_cause;
    char blocked[96]; /* "blocked: LIST" when a blocklist refused it */
    char *top;        /* the top-level URL, for redirects (NULL: a navigation) */
    /* holding the first bytes for sniffing / encoding detection */
    AuBuf hold;
    size_t hold_need;
    bool sniff, holding;
    int mode;
    /* state */
    AuNetDownload *dl;
    bool finished, cancelled;
};

struct AuNetLoader {
    AuNet *net;
    Req *reqs;
    uint32_t next_id;
    AuBlocklist *blocklist;
};

AuNetLoader *au_net_loader_new(AuNet *net)
{
    AuNetLoader *l = au_calloc(sizeof *l);
    l->net = net;
    return l;
}

AuNet *au_net_loader_net(AuNetLoader *l) { return l->net; }
void au_net_loader_set_blocklist(AuNetLoader *l, AuBlocklist *bl) { l->blocklist = bl; }
AuBlocklist *au_net_loader_blocklist(AuNetLoader *l) { return l->blocklist; }

static void req_free(Req *r)
{
    au_free(r->url);
    au_free(r->top);
    au_buf_free(&r->mime);
    au_buf_free(&r->charset);
    au_buf_free(&r->filename);
    au_buf_free(&r->csp);
    au_buf_free(&r->xfo);
    au_buf_free(&r->rpolicy);
    au_buf_free(&r->hold);
    if (r->is_data)
        au_data_url_free(&r->data);
    au_free(r);
}

void au_net_loader_free(AuNetLoader *l)
{
    if (!l)
        return;
    for (Req *r = l->reqs, *n; r; r = n) {
        n = r->next;
        if (r->t && !r->finished)
            au_http_cancel(r->t);
        if (r->dl)
            r->dl->adopted = NULL;
        req_free(r);
    }
    au_free(l);
}

static Req *find(const AuNetLoader *l, uint32_t id)
{
    for (Req *r = l->reqs; r; r = r->next)
        if (r->id == id && !r->cancelled)
            return r;
    return NULL;
}

/* ---- Info ---- */

static void fill_info(Req *r, AuLoadInfo *i)
{
    memset(i, 0, sizeof *i);
    i->url = r->url ? r->url : "";
    i->status = r->status;
    i->reason = r->reason;
    i->mime = au_buf_cstr(&r->mime);
    i->charset = au_buf_cstr(&r->charset);
    i->encoding = r->encoding;
    i->content_length = r->content_length;
    i->security = r->security;
    i->cert = r->have_cert ? &r->cert : NULL;
    i->from_cache = r->from_cache;
    i->no_store = r->no_store;
    i->attachment = r->attachment;
    i->filename = au_buf_cstr(&r->filename);
    i->csp = au_buf_cstr(&r->csp);
    i->x_frame_options = au_buf_cstr(&r->xfo);
    i->referrer_policy = au_buf_cstr(&r->rpolicy);
    i->tls_version = r->tls_version;
    i->cipher = r->cipher;
    i->fallback_cause = r->fallback_cause;
    i->mixed = r->mixed;
    i->request_head = "";
    i->blocked_reason = r->blocked[0] ? r->blocked : NULL;
    if (r->t && r->have_response && !r->finished)
        i->response_headers = au_http_response_headers(r->t);
}

/* ---- windows-1252 to UTF-8 ---- */

static const uint16_t cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

static void emit(Req *r, const void *bytes, size_t len)
{
    if (!len || r->cancelled)
        return;
    if (r->dl) {
        AuNetDownload *d = r->dl;
        if (!d->error && !d->sink.write(d->sink.ctx, bytes, len))
            d->error = au_strndup("cannot write the file", 21);
        d->received += (int64_t)len;
        return;
    }
    if (r->mode == MODE_CP1252) {
        const uint8_t *s = bytes;
        uint8_t out[4096];
        size_t o = 0;
        for (size_t i = 0; i < len; i++) {
            uint32_t cp = s[i] < 0x80 ? s[i] : (s[i] < 0xA0 ? cp1252_high[s[i] - 0x80] : s[i]);
            if (o + 3 > sizeof out) {
                r->cb.data(r->cb.ctx, r->id, out, o);
                o = 0;
                if (r->cancelled)
                    return;
            }
            if (cp < 0x80) {
                out[o++] = (uint8_t)cp;
            } else if (cp < 0x800) {
                out[o++] = (uint8_t)(0xC0 | cp >> 6);
                out[o++] = (uint8_t)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (uint8_t)(0xE0 | cp >> 12);
                out[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (uint8_t)(0x80 | (cp & 0x3F));
            }
        }
        if (o && r->cb.data)
            r->cb.data(r->cb.ctx, r->id, out, o);
        return;
    }
    if (r->cb.data)
        r->cb.data(r->cb.ctx, r->id, bytes, len);
}

static bool displayable(const char *mime)
{
    return !strncmp(mime, "text/", 5) || !strncmp(mime, "image/", 6) || !strcmp(mime, "application/xhtml+xml") ||
           !strcmp(mime, "application/xml") || !strcmp(mime, "application/json") || !mime[0];
}

/* The first bytes are in (or the body ended): sniff, choose the encoding,
 * send the response and the held bytes. */
static void decide(Req *r)
{
    r->holding = false;
    const uint8_t *h = (const uint8_t *)r->hold.data;
    size_t hl = r->hold.len;
    if (r->sniff) {
        const char *m = au_mime_sniff(au_buf_cstr(&r->mime), r->nosniff, h, hl, r->dest);
        if (strcmp(m, au_buf_cstr(&r->mime))) {
            au_buf_clear(&r->mime);
            au_buf_puts(&r->mime, m);
        }
    }
    bool html = !strcmp(au_buf_cstr(&r->mime), "text/html");
    if (r->dest == AU_DEST_DOCUMENT || r->dest == AU_DEST_IFRAME) {
        if (html) {
            const char *enc = au_detect_encoding(h, hl, au_buf_cstr(&r->charset));
            snprintf(r->encoding, sizeof r->encoding, "%s", enc);
            if (r->to_utf8) {
                bool bom8 = hl >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF;
                if (!strcmp(enc, "UTF-8"))
                    r->mode = bom8 ? MODE_PASS : MODE_UTF8;
                else if (!strcmp(enc, "windows-1252"))
                    r->mode = MODE_CP1252;
            }
        }
    }
    /* Downloads. */
    r->response_sent = true;
    AuLoadInfo info;
    fill_info(r, &info);
    if ((r->dest == AU_DEST_DOCUMENT || r->dest == AU_DEST_IFRAME) &&
        (r->attachment || !displayable(au_buf_cstr(&r->mime))) && r->cb.download &&
        r->cb.download(r->cb.ctx, r->id, &info)) {
        /* Adopted (au_net_download_adopt set r->dl) or refused outright. */
        if (!r->dl) {
            if (r->t && !r->finished)
                au_http_cancel(r->t);
            r->cancelled = true;
            return;
        }
        r->mode = MODE_PASS;
    } else if (r->cb.response) {
        r->cb.response(r->cb.ctx, r->id, &info);
    }
    if (r->cancelled)
        return;
    if (r->mode == MODE_UTF8 || r->mode == MODE_CP1252) {
        int mode = r->mode;
        r->mode = MODE_PASS;
        emit(r, "\xEF\xBB\xBF", 3);
        r->mode = mode;
    }
    if (hl)
        emit(r, h, hl);
    au_buf_clear(&r->hold);
}

/* ---- Transfer callbacks ---- */

static void parse_disposition(Req *r, const char *v)
{
    while (*v == ' ' || *v == '\t')
        v++;
    r->attachment = au_ieq(v, strcspn(v, "; \t"), "attachment", 10);
    const char *fn = NULL;
    for (const char *p = v; *p; p++)
        if ((p == v || p[-1] == ';' || p[-1] == ' ') && !strncmp(p, "filename=", 9)) {
            fn = p + 9;
            break;
        }
    if (!fn)
        return;
    au_buf_clear(&r->filename);
    if (*fn == '"') {
        fn++;
        for (; *fn && *fn != '"' && r->filename.len < 255; fn++) {
            if (*fn == '\\' && fn[1])
                fn++;
            au_buf_putc(&r->filename, *fn);
        }
    } else {
        size_t n = strcspn(fn, "; \t");
        au_buf_append(&r->filename, fn, n > 255 ? 255 : n);
    }
    /* No path components. */
    char *s = au_buf_cstr(&r->filename);
    for (char *p = s; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 0x20)
            *p = '_';
}

static void t_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *resp)
{
    Req *r = ctx;
    if (r->blocked[0]) {
        /* A redirect to a listed host (t_redirect): r->url names it. */
        au_http_cancel(t);
        r->t = NULL;
        r->finished = true;
        AuLoadInfo info;
        fill_info(r, &info);
        if (r->cb.done)
            r->cb.done(r->cb.ctx, r->id, AU_NET_ERR_BLOCKED, &info);
        return;
    }
    au_free(r->url);
    r->url = au_strndup(resp->url, strlen(resp->url));
    r->status = resp->status;
    snprintf(r->reason, sizeof r->reason, "%s", resp->reason ? resp->reason : "");
    au_buf_clear(&r->mime);
    au_buf_puts(&r->mime, resp->mime);
    au_buf_clear(&r->charset);
    au_buf_puts(&r->charset, resp->charset);
    r->content_length = resp->content_encoding[0] ? -1 : resp->content_length;
    if (resp->from_cache)
        r->content_length = resp->content_length;
    r->from_cache = resp->from_cache;
    r->nosniff = au_headers_has_token(resp->headers, "x-content-type-options", "nosniff");
    AuCacheControl cc;
    au_cache_control_parse(resp->headers, &cc);
    r->no_store = cc.no_store;
    const char *cd = au_headers_get(resp->headers, "content-disposition");
    if (cd)
        parse_disposition(r, cd);
    const char *v = au_headers_get(resp->headers, "content-security-policy");
    au_buf_clear(&r->csp);
    if (v)
        au_buf_append(&r->csp, v, strlen(v) > 4096 ? 4096 : strlen(v));
    v = au_headers_get(resp->headers, "x-frame-options");
    au_buf_clear(&r->xfo);
    if (v)
        au_buf_append(&r->xfo, v, strlen(v) > 64 ? 64 : strlen(v));
    v = au_headers_get(resp->headers, "referrer-policy");
    au_buf_clear(&r->rpolicy);
    if (v)
        au_buf_append(&r->rpolicy, v, strlen(v) > 256 ? 256 : strlen(v));
    r->security = resp->security.tls ? AU_LOAD_SECURITY_SECURE : AU_LOAD_SECURITY_NOT_SECURE;
    r->tls_version = resp->security.tls ? resp->security.version : 0;
    snprintf(r->cipher, sizeof r->cipher, "%s", resp->security.tls && resp->security.cipher ? resp->security.cipher : "");
    const AuCertInfo *ci = au_http_cert_info(t);
    if (ci) {
        r->cert = *ci;
        r->have_cert = true;
        /* The user's exception let a bad certificate through. */
        if (ci->overridden)
            r->security = AU_LOAD_SECURITY_CERT_ERROR;
    }
    r->have_response = true;
    if (r->dl) {
        if (resp->content_length >= 0 && !resp->content_encoding[0])
            r->dl->total = r->dl->offset + resp->content_length;
        return;
    }
    /* Style sheets and scripts under nosniff must have the right type. */
    if (!au_mime_allowed(au_buf_cstr(&r->mime), r->nosniff, r->dest)) {
        au_http_cancel(t); /* no more transfer callbacks */
        r->t = NULL;
        r->finished = true;
        AuLoadInfo info;
        fill_info(r, &info);
        if (r->cb.done)
            r->cb.done(r->cb.ctx, r->id, AU_NET_ERR_BLOCKED, &info);
        return;
    }
    const char *m = au_buf_cstr(&r->mime);
    bool doc = r->dest == AU_DEST_DOCUMENT || r->dest == AU_DEST_IFRAME;
    r->sniff = (doc && !r->nosniff &&
                (!m[0] || !strcmp(m, "unknown/unknown") || !strcmp(m, "application/unknown") || !strcmp(m, "*/*"))) ||
               r->dest == AU_DEST_IMAGE;
    size_t need = 0;
    if (r->sniff)
        need = AU_SNIFF_BYTES;
    if (doc && (r->sniff || !strcmp(m, "text/html")) && need < 1024)
        need = 1024;
    if (resp->status == 204 || resp->status == 304)
        need = 0;
    r->hold_need = need;
    r->holding = need > 0;
    if (!r->holding)
        decide(r);
}

static void t_data(void *ctx, AuHttpTransfer *t, const void *data, size_t len)
{
    (void)t;
    Req *r = ctx;
    r->received += (int64_t)len;
    if (r->holding) {
        au_buf_append(&r->hold, data, len);
        if (r->hold.len >= r->hold_need)
            decide(r);
        return;
    }
    emit(r, data, len);
}

static void t_done(void *ctx, AuHttpTransfer *t, AuNetError err)
{
    Req *r = ctx;
    r->finished = true;
    if (err == AU_NET_ERR_HTTPS_FALLBACK)
        r->fallback_cause = au_http_fallback_cause(t);
    bool cert_problem = err == AU_NET_ERR_CERT || err == AU_NET_ERR_CLOCK ||
                        r->fallback_cause == AU_NET_ERR_CERT || r->fallback_cause == AU_NET_ERR_CLOCK;
    if (cert_problem || r->fallback_cause == AU_NET_ERR_TLS) {
        const AuCertInfo *ci = au_http_cert_info(t);
        if (ci) {
            r->cert = *ci;
            r->have_cert = true;
        }
        if (cert_problem)
            r->security = AU_LOAD_SECURITY_CERT_ERROR;
    }
    if (!r->url) {
        const char *u = au_http_url(t);
        r->url = au_strndup(u, strlen(u));
    }
    if (r->dl) {
        AuNetDownload *d = r->dl;
        d->finished_transfer = true;
        d->done = true;
        if (err != AU_NET_OK && !d->error) {
            const char *n = au_net_error_name(err);
            d->error = au_strndup(n, strlen(n));
        }
        return;
    }
    if (r->holding && err == AU_NET_OK)
        decide(r);
    if (r->cancelled)
        return;
    AuLoadInfo info;
    fill_info(r, &info);
    AuHttpTrace trace;
    au_http_trace(t, &trace);
    info.trace = &trace;
    info.response_headers = au_http_response_headers(t);
    info.request_head = au_http_request_head(t);
    if (r->cb.done)
        r->cb.done(r->cb.ctx, r->id, err, &info);
}

/* A redirect is checked like the request itself: one to a listed host is
 * not followed, and the request ends as blocked. */
static bool t_redirect(void *ctx, AuHttpTransfer *t, int status, const char *url)
{
    Req *r = ctx;
    (void)t;
    (void)status;
    AuUrl u;
    if (!r->l->blocklist || r->dl || !au_url_parse(&u, url, NULL))
        return true;
    bool blocked = au_blocklist_check(r->l->blocklist, &u, r->top, r->blocked, sizeof r->blocked);
    if (blocked) {
        au_free(r->url);
        r->url = au_strndup(u.href, strlen(u.href));
    }
    au_url_free(&u);
    return !blocked;
}

/* ---- Requests ---- */

uint32_t au_net_loader_start(AuNetLoader *l, const AuLoadRequest *rq, const AuLoadCallbacks *cb)
{
    Req *r = au_calloc(sizeof *r);
    r->l = l;
    r->id = ++l->next_id ? l->next_id : ++l->next_id;
    r->cb = *cb;
    r->dest = rq->dest;
    r->to_utf8 = rq->to_utf8;
    r->content_length = -1;
    au_buf_init(&r->mime);
    au_buf_init(&r->charset);
    au_buf_init(&r->filename);
    au_buf_init(&r->csp);
    au_buf_init(&r->xfo);
    au_buf_init(&r->rpolicy);
    au_buf_init(&r->hold);
    r->next = l->reqs;
    l->reqs = r;
    const char *url = rq->url ? rq->url : "";
    AuUrl u;
    if (!au_url_parse(&u, url, NULL)) {
        r->early = r->pending = true;
        r->early_err = AU_NET_ERR_BAD_URL;
        au_body_free(rq->body);
        return r->id;
    }
    if (!strcmp(u.scheme, "data")) {
        r->is_data = r->pending = true;
        au_data_url_init(&r->data);
        r->url = au_strndup(u.href, strlen(u.href));
        if (!au_data_url_decode(url, &r->data)) {
            r->early = true;
            r->early_err = AU_NET_ERR_BAD_URL;
        }
        au_url_free(&u);
        au_body_free(rq->body);
        return r->id;
    }
    /* Mixed content. */
    if (rq->document_url && rq->dest != AU_DEST_DOCUMENT) {
        AuUrl doc;
        if (au_url_parse(&doc, rq->document_url, NULL)) {
            AuMixedClass mc = au_mixed_classify(&doc, &u, rq->dest);
            if (mc == AU_MIXED_UPGRADEABLE) {
                au_url_upgrade_to_https(&u);
            } else if (mc == AU_MIXED_BLOCKABLE) {
                r->early = true;
                r->early_err = AU_NET_ERR_BLOCKED;
                r->mixed = true;
            }
            au_url_free(&doc);
        }
    }
    /* Blocklists: a listed host is never contacted. A document without a
     * top-level URL is a navigation (the page shows the interstitial);
     * everything else is a subresource or a script's request. */
    if (!r->early && l->blocklist) {
        if (rq->top_level_url)
            r->top = au_strndup(rq->top_level_url, strlen(rq->top_level_url));
        else if (rq->dest != AU_DEST_DOCUMENT && rq->document_url)
            r->top = au_strndup(rq->document_url, strlen(rq->document_url));
        if (au_blocklist_check(l->blocklist, &u, r->top, r->blocked, sizeof r->blocked)) {
            r->early = true;
            r->early_err = AU_NET_ERR_BLOCKED;
        }
    }
    if (r->early) {
        r->pending = true;
        r->url = au_strndup(u.href, strlen(u.href));
        au_url_free(&u);
        au_body_free(rq->body);
        return r->id;
    }
    AuHttpRequest hr;
    au_http_request_init(&hr, u.href);
    hr.method = rq->method;
    hr.body = rq->body;
    if (hr.body && rq->content_type && !hr.body->content_type)
        hr.body->content_type = au_strndup(rq->content_type, strlen(rq->content_type));
    hr.dest = rq->dest;
    hr.mode = (rq->dest == AU_DEST_DOCUMENT || rq->dest == AU_DEST_IFRAME) ? AU_MODE_NAVIGATE : AU_MODE_NO_CORS;
    if (rq->mode > 0)
        hr.mode = (AuRequestMode)(rq->mode - 1);
    if (rq->headers)
        for (size_t i = 0; i < rq->headers->n; i++)
            au_headers_add(&hr.headers, rq->headers->items[i].name, rq->headers->items[i].value);
    hr.manual_redirect = rq->manual_redirect;
    hr.no_cache = rq->no_store;
    hr.referrer = rq->document_url;
    hr.referrer_policy = rq->referrer_policy;
    hr.top_level_url = rq->top_level_url;
    hr.user_activated = rq->user_activated;
    hr.reload = rq->reload;
    hr.history = rq->history && !rq->reload && !rq->revalidate;
    if (rq->revalidate && !rq->reload)
        au_headers_add(&hr.headers, "Cache-Control", "max-age=0");
    hr.allow_http_fallback = rq->allow_http_fallback;
    hr.omit_credentials = rq->omit_credentials;
    AuHttpCallbacks hc = {r, t_response, t_data, t_done, l->blocklist ? t_redirect : NULL, NULL};
    r->t = au_http_start(l->net, &hr, &hc);
    au_http_request_free(&hr);
    au_url_free(&u);
    return r->id;
}

void au_net_loader_cancel(AuNetLoader *l, uint32_t id)
{
    Req *r = find(l, id);
    if (!r)
        return;
    if (r->t && !r->finished)
        au_http_cancel(r->t);
    r->cancelled = true;
    r->finished = true;
}

void au_net_loader_pause(AuNetLoader *l, uint32_t id, bool paused)
{
    Req *r = find(l, id);
    if (r && r->t && !r->finished && !r->cancelled)
        au_http_pause(r->t, paused);
}

static void deliver_early(Req *r)
{
    AuLoadInfo info;
    if (r->is_data && !r->early) {
        r->status = 0;
        au_buf_puts(&r->mime, au_buf_cstr(&r->data.mime));
        au_buf_puts(&r->charset, au_buf_cstr(&r->data.charset));
        r->content_length = (int64_t)r->data.body.len;
        r->security = AU_LOAD_SECURITY_NONE;
        r->have_response = true;
        /* Through the same path as network bodies (sniffing, UTF-8). */
        bool doc = r->dest == AU_DEST_DOCUMENT || r->dest == AU_DEST_IFRAME;
        r->hold_need = doc ? 1024 : 0;
        r->holding = true;
        size_t len = r->data.body.len;
        const uint8_t *p = (const uint8_t *)r->data.body.data;
        size_t first = len < r->hold_need ? len : r->hold_need;
        au_buf_append(&r->hold, p, first);
        decide(r);
        if (!r->cancelled && len > first)
            emit(r, p + first, len - first);
        r->received = (int64_t)len;
        r->finished = true;
        if (r->cancelled)
            return;
        fill_info(r, &info);
        if (r->cb.done)
            r->cb.done(r->cb.ctx, r->id, AU_NET_OK, &info);
        return;
    }
    r->finished = true;
    fill_info(r, &info);
    if (r->cb.done)
        r->cb.done(r->cb.ctx, r->id, r->early_err, &info);
}

bool au_net_loader_poll(AuNetLoader *l)
{
    bool any = au_net_pump(l->net);
    /* data: URLs and failures found at start, oldest first (the list is
     * newest first, so the last match is the oldest). */
    for (;;) {
        Req *pick = NULL;
        for (Req *r = l->reqs; r; r = r->next)
            if (r->pending && !r->cancelled)
                pick = r;
        if (!pick)
            break;
        pick->pending = false;
        deliver_early(pick);
        pick->finished = true;
        any = true;
    }
    /* Reap. */
    for (Req **pp = &l->reqs; *pp;) {
        Req *r = *pp;
        if ((r->finished || r->cancelled) && !r->dl && !r->pending) {
            *pp = r->next;
            req_free(r);
            any = true;
        } else {
            pp = &r->next;
        }
    }
    return any;
}

int au_net_loader_active(const AuNetLoader *l)
{
    int n = 0;
    for (const Req *r = l->reqs; r; r = r->next)
        if (!r->finished && !r->cancelled)
            n++;
    return n;
}

bool au_net_loader_progress(const AuNetLoader *l, uint32_t id, int64_t *received, int64_t *total)
{
    Req *r = find(l, id);
    if (!r)
        return false;
    *received = r->received;
    *total = r->content_length;
    return true;
}

bool au_net_loader_phase(const AuNetLoader *l, uint32_t id, AuHttpPhase *phase)
{
    Req *r = find(l, id);
    if (!r)
        return false;
    *phase = r->t && !r->finished ? au_http_phase(r->t) : AU_PHASE_DONE;
    return true;
}

/* ---- Downloads ---- */

static void d_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *resp)
{
    (void)t;
    AuNetDownload *d = ctx;
    if (resp->status >= 400) {
        char msg[64];
        snprintf(msg, sizeof msg, "HTTP %d %s", resp->status, resp->reason);
        d->error = au_strndup(msg, strlen(msg));
        au_http_cancel(d->t);
        d->t = NULL;
        d->done = true;
        return;
    }
    if (d->offset > 0 && resp->status != 206) {
        d->error = au_strndup("the server cannot resume this download", 38);
        au_http_cancel(d->t);
        d->t = NULL;
        d->done = true;
        return;
    }
    if (resp->content_length >= 0 && !resp->content_encoding[0])
        d->total = d->offset + resp->content_length;
}

static void d_data(void *ctx, AuHttpTransfer *t, const void *data, size_t len)
{
    AuNetDownload *d = ctx;
    if (d->error)
        return;
    if (!d->sink.write(d->sink.ctx, data, len)) {
        d->error = au_strndup("cannot write the file", 21);
        au_http_cancel(t);
        d->t = NULL;
        d->done = true;
        return;
    }
    d->received += (int64_t)len;
}

static void d_done(void *ctx, AuHttpTransfer *t, AuNetError err)
{
    (void)t;
    AuNetDownload *d = ctx;
    d->t = NULL;
    d->done = true;
    if (err != AU_NET_OK && !d->error) {
        const char *n = au_net_error_name(err);
        d->error = au_strndup(n, strlen(n));
    }
}

AuNetDownload *au_net_download_start(AuNetLoader *l, const char *url, int64_t offset, const AuDownloadSink *sink,
                                     char **error)
{
    AuUrl u;
    if (!au_url_parse(&u, url, NULL) || (strcmp(u.scheme, "http") && strcmp(u.scheme, "https"))) {
        if (u.href)
            au_url_free(&u);
        *error = au_strndup("not an http or https address", 28);
        return NULL;
    }
    au_url_free(&u);
    AuNetDownload *d = au_calloc(sizeof *d);
    d->l = l;
    d->sink = *sink;
    d->offset = offset > 0 ? offset : 0;
    d->received = d->offset;
    d->total = -1;
    AuHttpRequest hr;
    au_http_request_init(&hr, url);
    hr.dest = AU_DEST_EMPTY;
    hr.mode = AU_MODE_NAVIGATE;
    hr.user_activated = true;
    if (d->offset > 0)
        hr.range_start = d->offset;
    AuHttpCallbacks hc = {d, d_response, d_data, d_done, NULL, NULL};
    d->t = au_http_start(l->net, &hr, &hc);
    au_http_request_free(&hr);
    return d;
}

AuNetDownload *au_net_download_adopt(AuNetLoader *l, uint32_t id, const AuDownloadSink *sink)
{
    Req *r = find(l, id);
    if (!r || r->dl || r->is_data)
        return NULL;
    AuNetDownload *d = au_calloc(sizeof *d);
    d->l = l;
    d->sink = *sink;
    d->adopted = r;
    d->total = r->content_length;
    r->dl = d;
    d->done = r->finished;
    return d;
}

int64_t au_net_download_poll(AuNetDownload *d, int64_t *total, bool *done, char **error)
{
    if (d->total >= 0)
        *total = d->total;
    if (d->error) {
        *error = au_strndup(d->error, strlen(d->error));
        *done = false;
        return d->received;
    }
    *done = d->done;
    return d->received;
}

void au_net_download_stop(AuNetDownload *d)
{
    if (!d)
        return;
    if (d->t)
        au_http_cancel(d->t);
    if (d->adopted) {
        Req *r = d->adopted;
        if (r->t && !r->finished)
            au_http_cancel(r->t);
        r->dl = NULL;
        r->cancelled = true;
        r->finished = true;
    }
    au_free(d->error);
    au_free(d);
}
