/* The fetch interface between script and the embedder (fetch, XHR,
 * module and external script loads), with the same-origin policy.
 *
 * Policy, enforced here before a request reaches the embedder:
 * - Same-origin requests go out in any mode.
 * - Cross-origin, mode "same-origin": a network error.
 * - Cross-origin, mode "no-cors": only GET, HEAD and POST; the response
 *   is opaque to script (status 0, no headers, no body), except for
 *   classic scripts, which run.
 * - Cross-origin, mode "cors": CORS CHECK POINT. Simple requests (GET,
 *   HEAD, POST with only safelisted headers) go out and the response must
 *   carry Access-Control-Allow-Origin naming the document origin or "*"
 *   ("*" not allowed with credentials "include"), else a network error.
 *   Requests that need a preflight are refused (network error) until the
 *   network work adds preflight requests: PREFLIGHT POINT.
 * file: URLs count as one origin among themselves (Aura opens local test
 * pages that load their own resources); about: and data: documents have
 * opaque origins. */
#include "js_int.h"

#include <string.h>

struct JsFetch {
    uint32_t id;
    JSValue cb;     /* cb([kind, ...]) as a task */
    bool cors_check; /* cross-origin cors: check the response head */
    bool opaque;     /* cross-origin no-cors: hide the response */
    bool credentials_include;
    JsFetch *next;
};

void js_origin_of(const char *url, char *out, size_t cap)
{
    JsUrl u;
    if (!js_url_parse(url, NULL, &u)) {
        snprintf(out, cap, "null");
        return;
    }
    if (!strcmp(u.protocol, "file:"))
        snprintf(out, cap, "file://");
    else if (!strcmp(u.protocol, "http:") || !strcmp(u.protocol, "https:") || !strcmp(u.protocol, "ws:") ||
             !strcmp(u.protocol, "wss:") || !strcmp(u.protocol, "ftp:"))
        snprintf(out, cap, "%s//%s%s%s", u.protocol, u.hostname, u.port[0] ? ":" : "", u.port);
    else
        snprintf(out, cap, "null");
    js_url_free(&u);
}

bool js_same_origin(const AuJs *js, const char *url)
{
    char o[512];
    js_origin_of(url, o, sizeof o);
    return strcmp(o, "null") != 0 && strcmp(o, js->origin) == 0;
}

static JsFetch *find_fetch(AuJs *js, uint32_t id, JsFetch ***link)
{
    for (JsFetch **pp = &js->fetches; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) {
            if (link)
                *link = pp;
            return *pp;
        }
    return NULL;
}

static void deliver(AuJs *js, JsFetch *f, JSValue arr)
{
    js_queue_task(js, f->cb, arr);
    JS_FreeValue(js->ctx, arr);
}

static void drop_fetch(AuJs *js, uint32_t id)
{
    JsFetch **pp;
    JsFetch *f = find_fetch(js, id, &pp);
    if (!f)
        return;
    *pp = f->next;
    JS_FreeValue(js->ctx, f->cb);
    au_free(f);
}

static bool safelisted_header(const char *name, const char *value)
{
    if (au_ieq(name, strlen(name), "accept", 6) || au_ieq(name, strlen(name), "accept-language", 15) ||
        au_ieq(name, strlen(name), "content-language", 16))
        return true;
    if (au_ieq(name, strlen(name), "content-type", 12)) {
        const char *ok[] = {"application/x-www-form-urlencoded", "multipart/form-data", "text/plain"};
        for (size_t i = 0; i < 3; i++)
            if (!strncmp(value, ok[i], strlen(ok[i])))
                return true;
    }
    return false;
}

/* Headers script may not set (forbidden request headers). */
static bool forbidden_header(const char *name)
{
    static const char *const f[] = {"accept-charset", "accept-encoding", "access-control-request-headers",
                                    "access-control-request-method", "connection", "content-length", "cookie",
                                    "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin", "referer",
                                    "te", "trailer", "transfer-encoding", "upgrade", "via", "sec-gpc"};
    size_t n = strlen(name);
    for (size_t i = 0; i < JS_COUNTOF(f); i++)
        if (au_ieq(name, n, f[i], strlen(f[i])))
            return true;
    return (n >= 6 && au_ieq(name, 6, "proxy-", 6)) || (n >= 4 && au_ieq(name, 4, "sec-", 4));
}

/* fetchStart(method, url, headers[[n, v]...], body, credentials, mode,
 * dest, cb) -> id. Throws TypeError for a request the policy refuses. */
static JSValue n_fetch_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 8 || !JS_IsFunction(ctx, argv[7]))
        return JS_ThrowTypeError(ctx, "fetchStart: bad arguments");
    char *method = js_tostr(ctx, argv[0], NULL);
    char *url = js_tostr(ctx, argv[1], NULL);
    int32_t cred = 1, mode = 0, dest = 0;
    JS_ToInt32(ctx, &cred, argv[4]);
    JS_ToInt32(ctx, &mode, argv[5]);
    JS_ToInt32(ctx, &dest, argv[6]);
    if (!method || !url) {
        au_free(method);
        au_free(url);
        return JS_EXCEPTION;
    }
    for (char *c = method; *c; c++)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 32);
    /* headers */
    int64_t nh = 0;
    JS_GetLength(ctx, argv[2], &nh);
    if (nh < 0 || nh > 256)
        nh = 0;
    char **hv = nh ? au_calloc((size_t)nh * 2 * sizeof *hv) : NULL;
    size_t kept = 0;
    bool simple = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST");
    for (int64_t i = 0; i < nh; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
        JSValue k = JS_GetPropertyUint32(ctx, pair, 0), v = JS_GetPropertyUint32(ctx, pair, 1);
        char *ks = js_tostr(ctx, k, NULL), *vs = js_tostr(ctx, v, NULL);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, pair);
        if (ks && vs && !forbidden_header(ks)) {
            if (!safelisted_header(ks, vs))
                simple = false;
            hv[kept * 2] = ks;
            hv[kept * 2 + 1] = vs;
            kept++;
        } else {
            au_free(ks);
            au_free(vs);
        }
    }
    size_t blen = 0;
    const char *body = NULL;
    char *body_str = NULL;
    if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
        uint8_t *p = JS_GetUint8Array(ctx, &blen, argv[3]);
        if (p) {
            body = (const char *)p;
        } else {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
            body_str = js_tostr(ctx, argv[3], &blen);
            body = body_str;
        }
    }
    bool same = js_same_origin(js, url);
    const char *refusal = NULL;
    bool opaque = false, cors = false;
    if (strncmp(url, "http:", 5) && strncmp(url, "https:", 6) && strncmp(url, "file:", 5))
        refusal = "unsupported URL scheme";
    else if (!same) {
        if (mode == AU_FETCH_SAME_ORIGIN_MODE)
            refusal = "cross-origin request in same-origin mode";
        else if (mode == AU_FETCH_NO_CORS) {
            if (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST"))
                refusal = "no-cors requests must be GET, HEAD or POST";
            opaque = dest != AU_FETCH_DEST_SCRIPT && dest != AU_FETCH_DEST_IMAGE;
        } else if (!strncmp(url, "file:", 5) || !strncmp(js->origin, "file:", 5)) {
            refusal = "cross-origin request between file: and network origins";
        } else if (!simple) {
            /* PREFLIGHT POINT: non-simple cross-origin requests need a CORS
             * preflight, which the network work will add. */
            refusal = "cross-origin request needs a CORS preflight (not supported yet)";
        } else {
            cors = true; /* CORS CHECK POINT: the response head is checked in au_js_fetch_head */
        }
    }
    uint32_t id = 0;
    if (!refusal && js->hooks.fetcher.start) {
        AuFetchRequest req = {method, url, (const char *const *)hv, kept, body, blen,
                              (uint8_t)cred, (uint8_t)mode, (uint8_t)dest};
        id = js->hooks.fetcher.start(js->hooks.fetcher.ctx, js, &req);
        if (!id)
            refusal = "the request could not start";
    } else if (!refusal) {
        refusal = "no network access";
    }
    for (size_t i = 0; i < kept * 2; i++)
        au_free(hv[i]);
    au_free(hv);
    au_free(body_str);
    au_free(method);
    if (refusal) {
        JSValue r = JS_ThrowTypeError(ctx, "Failed to fetch %s: %s", url, refusal);
        au_free(url);
        return r;
    }
    au_free(url);
    JsFetch *f = au_calloc(sizeof *f);
    f->id = id;
    f->cb = JS_DupValue(ctx, argv[7]);
    f->cors_check = cors;
    f->opaque = opaque;
    f->credentials_include = cred == AU_FETCH_INCLUDE;
    f->next = js->fetches;
    js->fetches = f;
    return JS_NewUint32(ctx, id);
}

static JSValue n_fetch_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    if (argc > 0 && !JS_ToUint32(ctx, &id, argv[0]) && find_fetch(js, id, NULL)) {
        if (js->hooks.fetcher.abort)
            js->hooks.fetcher.abort(js->hooks.fetcher.ctx, id);
        drop_fetch(js, id);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry fetch_natives[] = {
    JS_CFUNC_DEF("fetchStart", 8, n_fetch_start),
    JS_CFUNC_DEF("fetchAbort", 1, n_fetch_abort),
};

void js_fetch_init(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, fetch_natives, JS_COUNTOF(fetch_natives));
}

void js_fetch_free(AuJs *js)
{
    while (js->fetches) {
        JsFetch *f = js->fetches;
        js->fetches = f->next;
        if (js->hooks.fetcher.abort)
            js->hooks.fetcher.abort(js->hooks.fetcher.ctx, f->id);
        JS_FreeValue(js->ctx, f->cb);
        au_free(f);
    }
}

bool js_fetch_pending(const AuJs *js)
{
    return js->fetches != NULL;
}

void au_js_fetch_head(AuJs *js, uint32_t id, int status, const char *status_text, const char *const *headers,
                      size_t nheaders, const char *final_url)
{
    JsFetch *f = find_fetch(js, id, NULL);
    if (!f || js->closing)
        return;
    JSContext *ctx = js->ctx;
    if (f->cors_check) {
        /* CORS CHECK POINT: Access-Control-Allow-Origin. */
        bool ok = false;
        for (size_t i = 0; i < nheaders; i++)
            if (au_ieq(headers[i * 2], strlen(headers[i * 2]), "access-control-allow-origin", 27)) {
                const char *v = headers[i * 2 + 1];
                ok = (!strcmp(v, "*") && !f->credentials_include) || !strcmp(v, js->origin);
            }
        if (!ok) {
            js_console(js, AU_JS_ERROR, "Cross-origin response blocked: no matching Access-Control-Allow-Origin",
                       69);
            if (js->hooks.fetcher.abort)
                js->hooks.fetcher.abort(js->hooks.fetcher.ctx, id);
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "error"));
            deliver(js, f, arr);
            drop_fetch(js, id);
            return;
        }
    }
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "head"));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, f->opaque ? 0 : status));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewString(ctx, f->opaque || !status_text ? "" : status_text));
    JSValue h = JS_NewArray(ctx);
    if (!f->opaque)
        for (size_t i = 0; i < nheaders; i++) {
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, headers[i * 2]));
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, headers[i * 2 + 1]));
            JS_SetPropertyUint32(ctx, h, (uint32_t)i, pair);
        }
    JS_SetPropertyUint32(ctx, arr, 3, h);
    JS_SetPropertyUint32(ctx, arr, 4, final_url ? JS_NewString(ctx, final_url) : JS_NULL);
    JS_SetPropertyUint32(ctx, arr, 5, JS_NewBool(ctx, f->opaque));
    deliver(js, f, arr);
}

void au_js_fetch_data(AuJs *js, uint32_t id, const void *data, size_t len)
{
    JsFetch *f = find_fetch(js, id, NULL);
    if (!f || js->closing || f->opaque || !len)
        return;
    JSContext *ctx = js->ctx;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "data"));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewUint8ArrayCopy(ctx, data, len));
    deliver(js, f, arr);
}

void au_js_fetch_done(AuJs *js, uint32_t id, bool ok)
{
    JsFetch *f = find_fetch(js, id, NULL);
    if (!f || js->closing)
        return;
    JSContext *ctx = js->ctx;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, ok ? "done" : "error"));
    deliver(js, f, arr);
    drop_fetch(js, id);
}
