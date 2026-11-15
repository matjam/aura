/* Requests made by script (fetch, XMLHttpRequest, module and script
 * loads, workers): the Fetch Standard's main fetch, CORS and redirects,
 * over the embedder's fetcher.
 *
 * This file is the boundary between script and other origins. Script
 * hands a request to fetchStart; everything from there to the events
 * script receives happens here, in C, and script gets only what the
 * standard lets it read:
 *
 * - Methods CONNECT, TRACE and TRACK and the forbidden request headers
 *   are refused or dropped (src/net/cors.c has the lists).
 * - A same-origin request goes out as it is; its response is "basic"
 *   (every header but Set-Cookie).
 * - Cross-origin, mode "same-origin": a network error, also when a
 *   redirect leaves the origin.
 * - Cross-origin, mode "no-cors": GET, HEAD and POST with safelisted
 *   headers only; the response is "opaque" (status 0, no headers, no body,
 *   no URL), and Cross-Origin-Resource-Policy may refuse it. The bindings'
 *   own loads (a classic script to run, an image to measure) get the body,
 *   which page script never sees.
 * - Cross-origin, mode "cors": the request carries Origin. A method or
 *   header that is not safelisted (or an XMLHttpRequest with upload
 *   listeners) first sends a preflight OPTIONS request, unless the
 *   preflight cache already allows it; when the preflight is refused the
 *   request itself is never sent. The response must pass the CORS check
 *   (Access-Control-Allow-Origin, and -Allow-Credentials with
 *   credentials), else it is a network error and nothing of it reaches
 *   script. A "cors" response shows the safelisted headers and those in
 *   Access-Control-Expose-Headers.
 * - Credentials (cookies, HTTP authentication) go with a hop when the
 *   mode is "include", or "same-origin" and the hop is same-origin and
 *   untainted. The network layer still applies the cookie rules
 *   (SameSite, third-party blocking) to hops that carry them.
 * - Redirects are followed here, one hop at a time (the embedder is asked
 *   not to follow them), so that each hop is checked like the first: a
 *   redirect to another origin makes the request cross-origin from then
 *   on, a cross-origin redirect from a cross-origin URL sends "Origin:
 *   null", 301, 302 and 303 change the method as HTTP says, and
 *   Authorization does not cross origins. redirect "error" fails and
 *   "manual" gives an "opaqueredirect" response.
 *
 * file: URLs count as one origin among themselves (Aura opens local test
 * pages that load their own resources); they never mix with network
 * origins. about: and data: documents have opaque origins. data: and
 * blob: URLs are answered by the prelude and never get here. */
#include "js_int.h"
#include "../net/cors.h"

#include <string.h>

enum { TAINT_BASIC, TAINT_CORS, TAINT_OPAQUE };
enum { REDIRECT_FOLLOW, REDIRECT_ERROR, REDIRECT_MANUAL };
enum { PH_PREFLIGHT, PH_MAIN };

struct JsFetch {
    uint32_t id;     /* what script holds */
    uint32_t hop;    /* the embedder's id of the request in flight; 0: none */
    JSValue cb;      /* cb([kind, ...]) as a task */
    /* the request */
    char *method, *url, *referrer;
    char **hv; /* name, value, ...: what script set, less what a redirect removed */
    size_t nh;
    uint8_t *body;
    size_t body_len;
    uint8_t credentials, mode, dest, cache, redirect, rpolicy;
    bool force_preflight;
    /* state */
    uint8_t taint, phase;
    bool tainted_origin; /* Origin is "null" from now on */
    bool redirected, final, hide_body;
    int redirects;
    /* a synchronous request (a worker's importScripts or XMLHttpRequest):
     * events are kept here instead of being queued for script */
    bool sync, sync_done, sync_ok;
    int sync_status;
    AuBuf sync_head, sync_body;
    JsFetch *next;
};

void js_origin_of(const char *url, char *out, size_t cap)
{
    AuBuf b;
    au_buf_init(&b);
    au_cors_origin_of(url, &b);
    snprintf(out, cap, "%s", au_buf_cstr(&b));
    au_buf_free(&b);
}

bool js_same_origin(const AuJs *js, const char *url)
{
    char o[512];
    js_origin_of(url, o, sizeof o);
    return !js->opaque_origin && strcmp(o, "null") != 0 && strcmp(o, js->origin) == 0;
}

static JsFetch *find_fetch(AuJs *js, uint32_t id, bool by_hop, JsFetch ***link)
{
    for (JsFetch **pp = &js->fetches; *pp; pp = &(*pp)->next)
        if ((by_hop ? (*pp)->hop : (*pp)->id) == id) {
            if (link)
                *link = pp;
            return *pp;
        }
    return NULL;
}

static void free_fetch(AuJs *js, JsFetch *f)
{
    JS_FreeValue(js->ctx, f->cb);
    au_free(f->method);
    au_free(f->url);
    au_free(f->referrer);
    for (size_t i = 0; i < f->nh * 2; i++)
        au_free(f->hv[i]);
    au_free(f->hv);
    au_free(f->body);
    au_buf_free(&f->sync_head);
    au_buf_free(&f->sync_body);
    au_free(f);
}

static void drop_fetch(AuJs *js, JsFetch *f)
{
    for (JsFetch **pp = &js->fetches; *pp; pp = &(*pp)->next)
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    if (f->sync) {
        /* the waiting native frees it */
        f->sync_done = true;
        f->hop = 0;
        return;
    }
    free_fetch(js, f);
}

static void stop_hop(AuJs *js, JsFetch *f)
{
    if (f->hop && js->hooks.fetcher.abort)
        js->hooks.fetcher.abort(js->hooks.fetcher.ctx, f->hop);
    f->hop = 0;
}

/* ---- Events for script ----
 * An event becomes a task for the fetch's callback. A worker's fetches
 * live on the worker's stack: what the embedder reports for them is kept
 * and replayed there (js_worker_defer_fetch), so this code always runs on
 * the stack of the runtime that owns the fetch. */
typedef struct FetchEvent {
    int kind;         /* 0 head, 1 data, 2 done, 3 error */
    int status, type; /* head: type 0 basic, 1 cors, 2 opaque, 3 opaqueredirect */
    bool redirected;
    const char *text; /* head: the status text; error: why */
    const char *url;
    AuBuf headers;    /* head: name NUL value NUL ... */
    const void *data;
    size_t len;
} FetchEvent;

static JSValue header_pairs(JSContext *ctx, const AuBuf *b)
{
    JSValue h = JS_NewArray(ctx);
    const char *p = b->data, *end = p ? p + b->len : NULL;
    uint32_t i = 0;
    while (p && p < end) {
        const char *name = p;
        p += strlen(p) + 1;
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, name));
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, p));
        p += strlen(p) + 1;
        JS_SetPropertyUint32(ctx, h, i++, pair);
    }
    return h;
}

static void emit(AuJs *js, JsFetch *f, FetchEvent *ev)
{
    if (f->sync) {
        if (ev->kind == 0) {
            f->sync_status = ev->status;
            au_buf_clear(&f->sync_head);
            if (ev->headers.len)
                au_buf_append(&f->sync_head, ev->headers.data, ev->headers.len);
        } else if (ev->kind == 1) {
            au_buf_append(&f->sync_body, ev->data, ev->len);
        } else {
            f->sync_ok = ev->kind == 2;
        }
        au_buf_free(&ev->headers);
        return;
    }
    JSContext *ctx = js->ctx;
    JSValue arr = JS_NewArray(ctx);
    switch (ev->kind) {
    case 0:
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "head"));
        JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, ev->status));
        JS_SetPropertyUint32(ctx, arr, 2, JS_NewString(ctx, ev->text ? ev->text : ""));
        JS_SetPropertyUint32(ctx, arr, 3, header_pairs(ctx, &ev->headers));
        JS_SetPropertyUint32(ctx, arr, 4, JS_NewString(ctx, ev->url ? ev->url : ""));
        JS_SetPropertyUint32(ctx, arr, 5, JS_NewInt32(ctx, ev->type));
        JS_SetPropertyUint32(ctx, arr, 6, JS_NewBool(ctx, ev->redirected));
        break;
    case 1:
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "data"));
        JS_SetPropertyUint32(ctx, arr, 1, JS_NewUint8ArrayCopy(ctx, ev->data, ev->len));
        break;
    case 2:
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "done"));
        break;
    default:
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "error"));
        JS_SetPropertyUint32(ctx, arr, 1, JS_NewString(ctx, ev->text ? ev->text : "network error"));
        break;
    }
    js_queue_task(js, f->cb, arr);
    JS_FreeValue(ctx, arr);
    au_buf_free(&ev->headers);
}

/* Ends the fetch with a network error (why goes to the console when it is
 * a policy refusal) and frees it. */
static void fail(AuJs *js, JsFetch *f, const char *why, bool policy)
{
    stop_hop(js, f);
    if (policy && why) {
        AuBuf m;
        au_buf_init(&m);
        au_buf_printf(&m, "Access to %s from origin %s was blocked: %s", f->url, js->origin, why);
        js_console(js, AU_JS_WARN, au_buf_cstr(&m), m.len);
        au_buf_free(&m);
    }
    FetchEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = 3;
    ev.text = why;
    emit(js, f, &ev);
    drop_fetch(js, f);
}

/* ---- Hops ---- */

static void add_header(char ***hv, size_t *n, const char *name, const char *value)
{
    *hv = au_realloc(*hv, (*n + 1) * 2 * sizeof **hv);
    (*hv)[*n * 2] = au_strndup(name, strlen(name));
    (*hv)[*n * 2 + 1] = au_strndup(value, strlen(value));
    (*n)++;
}

static void remove_header(JsFetch *f, const char *name)
{
    size_t k = 0;
    for (size_t i = 0; i < f->nh; i++) {
        if (au_ieq(f->hv[2 * i], strlen(f->hv[2 * i]), name, strlen(name))) {
            au_free(f->hv[2 * i]);
            au_free(f->hv[2 * i + 1]);
            continue;
        }
        f->hv[2 * k] = f->hv[2 * i];
        f->hv[2 * k + 1] = f->hv[2 * i + 1];
        k++;
    }
    f->nh = k;
}

static bool is_http(const char *url)
{
    return !strncmp(url, "http:", 5) || !strncmp(url, "https:", 6);
}

static const char *request_origin(const AuJs *js, const JsFetch *f)
{
    return f->tainted_origin || js->opaque_origin ? "null" : js->origin;
}

static bool with_credentials(const JsFetch *f)
{
    return f->credentials == AU_FETCH_INCLUDE || (f->credentials == AU_FETCH_SAME_ORIGIN && f->taint == TAINT_BASIC);
}

/* Sends the request in flight: the preflight or the request itself. */
static bool send_hop(AuJs *js, JsFetch *f)
{
    if (!js->hooks.fetcher.start)
        return false;
    char **hv = NULL;
    size_t n = 0;
    AuFetchRequest req;
    memset(&req, 0, sizeof req);
    req.url = f->url;
    req.mode = f->mode;
    req.dest = f->dest;
    req.cache = f->cache;
    req.referrer = f->referrer;
    req.referrer_policy = f->rpolicy;
    req.manual_redirect = true;
    bool origin_header = is_http(f->url);
    if (f->phase == PH_PREFLIGHT) {
        AuBuf names;
        au_buf_init(&names);
        req.method = "OPTIONS";
        req.credentials = AU_FETCH_OMIT;
        req.cache = AU_FETCH_CACHE_NO_STORE;
        add_header(&hv, &n, "Accept", "*/*");
        add_header(&hv, &n, "Access-Control-Request-Method", f->method);
        if (au_cors_unsafe_header_names((const char *const *)f->hv, f->nh, &names))
            add_header(&hv, &n, "Access-Control-Request-Headers", au_buf_cstr(&names));
        au_buf_free(&names);
    } else {
        req.method = f->method;
        req.body = f->body;
        req.body_len = f->body_len;
        req.credentials = with_credentials(f) ? AU_FETCH_INCLUDE : AU_FETCH_OMIT;
        for (size_t i = 0; i < f->nh; i++)
            add_header(&hv, &n, f->hv[2 * i], f->hv[2 * i + 1]);
        /* Origin goes with every CORS request and with every request that
         * is not a GET or HEAD. */
        origin_header = origin_header && (f->taint == TAINT_CORS || (strcmp(f->method, "GET") && strcmp(f->method, "HEAD")));
    }
    if (origin_header)
        add_header(&hv, &n, "Origin", request_origin(js, f));
    req.headers = (const char *const *)hv;
    req.nheaders = n;
    f->hop = js->hooks.fetcher.start(js->hooks.fetcher.ctx, js, &req);
    for (size_t i = 0; i < n * 2; i++)
        au_free(hv[i]);
    au_free(hv);
    return f->hop != 0;
}

/* The standard's main fetch: decides how the current URL is fetched and
 * sends the first request for it. Returns NULL, or why it is refused
 * (*policy: a same-origin policy refusal, for the console). */
static const char *main_fetch(AuJs *js, JsFetch *f, bool *policy)
{
    *policy = false;
    if (!is_http(f->url) && strncmp(f->url, "file:", 5))
        return "unsupported URL scheme";
    bool same = js_same_origin(js, f->url);
    f->phase = PH_MAIN;
    if (same && f->taint == TAINT_BASIC) {
        /* same-origin: as it is */
    } else if (f->mode == AU_FETCH_SAME_ORIGIN_MODE || f->mode == AU_FETCH_NAVIGATE) {
        *policy = true;
        return "cross-origin request in same-origin mode";
    } else if (f->mode == AU_FETCH_NO_CORS) {
        if (f->redirect != REDIRECT_FOLLOW)
            return "a no-cors request must follow redirects";
        f->taint = TAINT_OPAQUE;
    } else {
        if (!is_http(f->url) || !strncmp(js->origin, "file:", 5)) {
            *policy = true;
            return "cross-origin request between file: and network origins";
        }
        f->taint = TAINT_CORS;
        bool cred = f->credentials == AU_FETCH_INCLUDE;
        bool unsafe = f->force_preflight || !au_cors_safelisted_method(f->method);
        if (!unsafe) {
            AuBuf names;
            au_buf_init(&names);
            unsafe = au_cors_unsafe_header_names((const char *const *)f->hv, f->nh, &names);
            au_buf_free(&names);
        }
        if (unsafe) {
            AuJs *root = js->root;
            if (!root->cors_cache)
                root->cors_cache = au_cors_cache_new();
            if (!au_cors_cache_allows(root->cors_cache, request_origin(js, f), f->url, cred, f->method,
                                      (const char *const *)f->hv, f->nh, js_now(js)))
                f->phase = PH_PREFLIGHT;
        }
    }
    return send_hop(js, f) ? NULL : "the request could not start";
}

/* ---- fetchStart ---- */

static void opt_str(JSContext *ctx, JSValueConst o, const char *name, char **out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, name);
    if (JS_IsString(v))
        *out = js_tostr(ctx, v, NULL);
    JS_FreeValue(ctx, v);
}

static int opt_int(JSContext *ctx, JSValueConst o, const char *name, int def)
{
    JSValue v = JS_GetPropertyStr(ctx, o, name);
    int32_t r = def;
    if (JS_IsNumber(v) || JS_IsBool(v))
        JS_ToInt32(ctx, &r, v);
    JS_FreeValue(ctx, v);
    return r;
}

/* Builds the fetch from script's arguments; NULL with *refusal set, or
 * with an exception pending (*refusal NULL). */
static JsFetch *fetch_from_args(JSContext *ctx, int argc, JSValueConst *argv, const char **refusal)
{
    *refusal = NULL;
    char *method = js_tostr(ctx, argv[0], NULL);
    char *url = js_tostr(ctx, argv[1], NULL);
    if (!method || !url) {
        au_free(method);
        au_free(url);
        return NULL;
    }
    JsFetch *f = au_calloc(sizeof *f);
    f->cb = JS_UNDEFINED;
    f->method = method;
    f->url = url;
    au_buf_init(&f->sync_head);
    au_buf_init(&f->sync_body);
    /* The methods the standard normalizes are sent in upper case. */
    static const char *const norm[] = {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT", "PATCH"};
    for (size_t i = 0; i < JS_COUNTOF(norm); i++)
        if (au_ieq(method, strlen(method), norm[i], strlen(norm[i])))
            strcpy(method, norm[i]);
    int32_t cred = AU_FETCH_SAME_ORIGIN, mode = AU_FETCH_CORS, dest = 0;
    JS_ToInt32(ctx, &cred, argv[4]);
    JS_ToInt32(ctx, &mode, argv[5]);
    JS_ToInt32(ctx, &dest, argv[6]);
    f->credentials = (uint8_t)(cred < 0 || cred > AU_FETCH_INCLUDE ? AU_FETCH_SAME_ORIGIN : cred);
    f->mode = (uint8_t)(mode < 0 || mode > AU_FETCH_NAVIGATE ? AU_FETCH_CORS : mode);
    f->dest = (uint8_t)dest;
    if (argc > 8 && JS_IsObject(argv[8])) {
        f->cache = (uint8_t)opt_int(ctx, argv[8], "cache", 0);
        f->redirect = (uint8_t)opt_int(ctx, argv[8], "redirect", 0);
        f->rpolicy = (uint8_t)opt_int(ctx, argv[8], "referrerPolicy", 0);
        f->force_preflight = opt_int(ctx, argv[8], "preflight", 0) != 0;
        opt_str(ctx, argv[8], "referrer", &f->referrer);
        if (f->cache > AU_FETCH_CACHE_ONLY_IF_CACHED)
            f->cache = 0;
        if (f->redirect > REDIRECT_MANUAL)
            f->redirect = 0;
        if (f->rpolicy > 8)
            f->rpolicy = 0;
    }
    if (au_cors_forbidden_method(method)) {
        *refusal = "the method is forbidden";
        return f;
    }
    for (const char *c = method; *c; c++)
        if ((unsigned char)*c <= 0x20 || (unsigned char)*c >= 0x7f) {
            *refusal = "the method is not a token";
            return f;
        }
    if (f->mode == AU_FETCH_NO_CORS && !au_cors_safelisted_method(method)) {
        *refusal = "a no-cors request must be GET, HEAD or POST";
        return f;
    }
    /* headers */
    int64_t nh = 0;
    JS_GetLength(ctx, argv[2], &nh);
    if (nh < 0 || nh > 256)
        nh = 0;
    for (int64_t i = 0; i < nh; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
        JSValue k = JS_GetPropertyUint32(ctx, pair, 0), v = JS_GetPropertyUint32(ctx, pair, 1);
        char *ks = js_tostr(ctx, k, NULL), *vs = js_tostr(ctx, v, NULL);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, pair);
        bool keep = ks && vs && ks[0] && !au_cors_forbidden_request_header(ks, vs) && !strpbrk(vs, "\r\n") &&
                    (f->mode != AU_FETCH_NO_CORS || au_cors_no_cors_header(ks, vs));
        for (const char *c = ks; keep && *c; c++)
            if ((unsigned char)*c <= 0x20 || (unsigned char)*c >= 0x7f || *c == ':')
                keep = false;
        if (keep)
            add_header(&f->hv, &f->nh, ks, vs);
        au_free(ks);
        au_free(vs);
    }
    if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
        size_t blen = 0;
        uint8_t *p = JS_GetUint8Array(ctx, &blen, argv[3]);
        if (p) {
            f->body = au_alloc(blen ? blen : 1);
            memcpy(f->body, p, blen);
            f->body_len = blen;
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            f->body = (uint8_t *)js_tostr(ctx, argv[3], &f->body_len);
        }
    }
    return f;
}

/* fetchStart(method, url, headers[[n, v]...], body, credentials, mode,
 * dest, cb, opts) -> id. opts: cache, redirect, referrerPolicy
 * (numbers), referrer (a URL, or "" for none), preflight (always send
 * one). Throws TypeError for a request the policy refuses. */
static JSValue n_fetch_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 8 || !JS_IsFunction(ctx, argv[7]))
        return JS_ThrowTypeError(ctx, "fetchStart: bad arguments");
    const char *refusal;
    JsFetch *f = fetch_from_args(ctx, argc, argv, &refusal);
    if (!f)
        return JS_EXCEPTION;
    bool policy = false;
    if (!refusal) {
        f->id = ++js->next_fetch_id ? js->next_fetch_id : ++js->next_fetch_id;
        f->cb = JS_DupValue(ctx, argv[7]);
        f->next = js->fetches;
        js->fetches = f;
        refusal = main_fetch(js, f, &policy);
        if (refusal) {
            js->fetches = f->next;
            f->hop = 0;
        }
    }
    if (refusal) {
        if (policy) {
            AuBuf m;
            au_buf_init(&m);
            au_buf_printf(&m, "Access to %s from origin %s was blocked: %s", f->url, js->origin, refusal);
            js_console(js, AU_JS_WARN, au_buf_cstr(&m), m.len);
            au_buf_free(&m);
        }
        JSValue r = JS_ThrowTypeError(ctx, "Failed to fetch %s: %s", f->url, refusal);
        free_fetch(js, f);
        return r;
    }
    return JS_NewUint32(ctx, f->id);
}

static JSValue n_fetch_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    JsFetch *f;
    if (argc > 0 && !JS_ToUint32(ctx, &id, argv[0]) && (f = find_fetch(js, id, false, NULL))) {
        stop_hop(js, f);
        drop_fetch(js, f);
    }
    return JS_UNDEFINED;
}

/* fetchSync(method, url, headers, body, credentials, mode, dest, null,
 * opts) -> [status, headers, bytes, url] or null for a network error.
 * Only a worker that runs on its own stack can wait for the network
 * (worker.c gives the page its turn meanwhile); elsewhere this throws. */
static JSValue n_fetch_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 7)
        return JS_ThrowTypeError(ctx, "fetchSync: bad arguments");
    if (!js->worker || !js_worker_can_wait(js))
        return JS_ThrowTypeError(ctx, "synchronous requests are not supported here");
    const char *refusal;
    JsFetch *f = fetch_from_args(ctx, argc, argv, &refusal);
    if (!f)
        return JS_EXCEPTION;
    bool policy = false;
    f->sync = true;
    if (!refusal) {
        f->id = ++js->next_fetch_id ? js->next_fetch_id : ++js->next_fetch_id;
        f->next = js->fetches;
        js->fetches = f;
        refusal = main_fetch(js, f, &policy);
        if (refusal)
            js->fetches = f->next;
    }
    if (refusal) {
        f->sync = false;
        free_fetch(js, f);
        return JS_NULL;
    }
    /* Wait: the page and the network run while this stack is parked. */
    bool alive = true;
    while (!f->sync_done && alive)
        alive = js_worker_wait(js);
    if (!f->sync_done) {
        /* the worker is being terminated */
        stop_hop(js, f);
        drop_fetch(js, f);
        f->sync = false;
        free_fetch(js, f);
        return JS_ThrowInternalError(ctx, "worker terminated");
    }
    JSValue r = JS_NULL;
    if (f->sync_ok) {
        r = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, r, 0, JS_NewInt32(ctx, f->sync_status));
        JSValue h = header_pairs(ctx, &f->sync_head);
        JS_SetPropertyUint32(ctx, r, 1, h);
        JS_SetPropertyUint32(ctx, r, 2,
                             JS_NewUint8ArrayCopy(ctx, (const uint8_t *)f->sync_body.data, f->sync_body.len));
        JS_SetPropertyUint32(ctx, r, 3, JS_NewString(ctx, f->url));
    }
    f->sync = false;
    free_fetch(js, f);
    return r;
}

static const JSCFunctionListEntry fetch_natives[] = {
    JS_CFUNC_DEF("fetchStart", 9, n_fetch_start),
    JS_CFUNC_DEF("fetchAbort", 1, n_fetch_abort),
    JS_CFUNC_DEF("fetchSync", 9, n_fetch_sync),
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
        stop_hop(js, f);
        if (f->sync) {
            f->sync_done = true; /* its native frees it */
            continue;
        }
        free_fetch(js, f);
    }
    if (js->root == js) {
        au_cors_cache_free(js->cors_cache);
        js->cors_cache = NULL;
    }
}

bool js_fetch_pending(const AuJs *js)
{
    return js->fetches != NULL;
}

/* ---- Responses from the embedder ---- */

static bool redirect_status(int s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

/* Follows a redirect to location (HTTP-redirect fetch). Returns NULL, or
 * why the fetch fails. */
static const char *follow(AuJs *js, JsFetch *f, int status, const char *location, bool *policy)
{
    *policy = false;
    JsUrl u;
    if (!js_url_parse(location, f->url, &u))
        return "a redirect to an invalid URL";
    const char *why = NULL;
    bool has_cred = u.username[0] || u.password[0];
    char *next = au_strndup(u.href, strlen(u.href));
    /* the fragment does not travel */
    char *hash = strchr(next, '#');
    if (hash)
        *hash = 0;
    bool http = !strcmp(u.protocol, "http:") || !strcmp(u.protocol, "https:");
    js_url_free(&u);
    char from[512], to[512];
    js_origin_of(f->url, from, sizeof from);
    js_origin_of(next, to, sizeof to);
    bool hop_cross = strcmp(from, to) != 0 || !strcmp(to, "null");
    bool next_same = js_same_origin(js, next);
    if (!http)
        why = "a redirect to a URL that is not http or https";
    else if (++f->redirects > 20)
        why = "too many redirects";
    else if (f->mode == AU_FETCH_CORS && has_cred && !next_same) {
        why = "a cross-origin redirect to a URL with credentials";
        *policy = true;
    } else if (f->taint == TAINT_CORS && has_cred) {
        why = "a redirect of a cross-origin request to a URL with credentials";
        *policy = true;
    }
    if (why) {
        au_free(next);
        return why;
    }
    if (hop_cross && !js_same_origin(js, f->url))
        f->tainted_origin = true;
    if (((status == 301 || status == 302) && !strcmp(f->method, "POST")) ||
        (status == 303 && strcmp(f->method, "GET") && strcmp(f->method, "HEAD"))) {
        au_free(f->method);
        f->method = au_strndup("GET", 3);
        au_free(f->body);
        f->body = NULL;
        f->body_len = 0;
        remove_header(f, "content-encoding");
        remove_header(f, "content-language");
        remove_header(f, "content-location");
        remove_header(f, "content-type");
    }
    if (hop_cross)
        remove_header(f, "authorization");
    au_free(f->url);
    f->url = next;
    f->redirected = true;
    return main_fetch(js, f, policy);
}

static void pack_header(AuBuf *b, const char *name, const char *value)
{
    au_buf_append(b, name, strlen(name) + 1);
    au_buf_append(b, value, strlen(value) + 1);
}

void au_js_fetch_head(AuJs *js, uint32_t id, int status, const char *status_text, const char *const *headers,
                      size_t nheaders, const char *final_url)
{
    if (js_worker_defer_fetch(js, 0, id, status, status_text, headers, nheaders, final_url, NULL, 0))
        return;
    JsFetch *f = find_fetch(js, id, true, NULL);
    if (!f || js->closing)
        return;
    bool cred = f->credentials == AU_FETCH_INCLUDE;
    const char *origin = request_origin(js, f);
    if (f->phase == PH_PREFLIGHT) {
        const char *why = NULL;
        if (!au_cors_preflight_allows(origin, cred, f->method, (const char *const *)f->hv, f->nh, status, headers,
                                      nheaders, &why)) {
            fail(js, f, why, true);
            return;
        }
        au_cors_cache_store(js->root->cors_cache, origin, f->url, cred, headers, nheaders, js_now(js));
        stop_hop(js, f); /* its body is of no use */
        f->phase = PH_MAIN;
        if (!send_hop(js, f))
            fail(js, f, "the request could not start", false);
        return;
    }
    /* An embedder that followed a redirect itself (a loader without
     * manual redirects) reports the final URL: the response is then
     * checked for where it came from. */
    if (final_url && strcmp(final_url, f->url) != 0 && is_http(final_url)) {
        bool was_same = js_same_origin(js, f->url) && f->taint == TAINT_BASIC;
        if (was_same && !js_same_origin(js, final_url)) {
            if (f->mode == AU_FETCH_NO_CORS)
                f->taint = TAINT_OPAQUE;
            else if (f->mode == AU_FETCH_CORS)
                f->taint = TAINT_CORS;
            else {
                fail(js, f, "cross-origin redirect in same-origin mode", true);
                return;
            }
        }
        size_t n = strcspn(final_url, "#");
        au_free(f->url);
        f->url = au_strndup(final_url, n);
        f->redirected = true;
    }
    if (f->taint == TAINT_CORS && !au_cors_check(origin, cred, headers, nheaders)) {
        fail(js, f,
             cred ? "the response has no Access-Control-Allow-Origin naming this origin with "
                    "Access-Control-Allow-Credentials: true"
                  : "the response has no matching Access-Control-Allow-Origin",
             true);
        return;
    }
    if (f->taint == TAINT_OPAQUE &&
        !au_corp_allows(au_cors_header(headers, nheaders, "cross-origin-resource-policy"), js->origin, f->url)) {
        fail(js, f, "the response's Cross-Origin-Resource-Policy does not allow this origin", true);
        return;
    }
    const char *location = redirect_status(status) ? au_cors_header(headers, nheaders, "location") : NULL;
    FetchEvent ev;
    memset(&ev, 0, sizeof ev);
    au_buf_init(&ev.headers);
    if (location && f->redirect == REDIRECT_ERROR) {
        fail(js, f, "a redirect, with redirect mode \"error\"", false);
        return;
    }
    if (location && f->redirect == REDIRECT_FOLLOW) {
        bool policy = false;
        char *loc = au_strndup(location, strlen(location));
        stop_hop(js, f); /* the redirect's own body is dropped */
        const char *why = follow(js, f, status, loc, &policy);
        au_free(loc);
        if (why)
            fail(js, f, why, policy);
        return;
    }
    f->final = true;
    if (location) {
        /* redirect "manual": script learns only that there was one */
        ev.type = 3;
        f->hide_body = true;
    } else if (f->taint == TAINT_OPAQUE) {
        ev.type = 2;
        /* the bindings' own loads read the body (a classic script, an
         * image's size); page script's fetch does not */
        f->hide_body = f->dest != AU_FETCH_DEST_SCRIPT && f->dest != AU_FETCH_DEST_IMAGE &&
                       f->dest != AU_FETCH_DEST_WORKER;
        if (!f->hide_body)
            ev.status = status;
    } else {
        ev.type = f->taint == TAINT_CORS ? 1 : 0;
        ev.status = status;
        ev.text = status_text;
        ev.url = f->url;
        for (size_t i = 0; i < nheaders; i++) {
            const char *name = headers[2 * i];
            bool show = f->taint == TAINT_CORS ? au_cors_exposed_response_header(name, headers, nheaders, cred)
                                               : !au_cors_forbidden_response_header(name);
            if (show)
                pack_header(&ev.headers, name, headers[2 * i + 1]);
        }
    }
    ev.redirected = f->redirected && ev.type < 2; /* an opaque response has no URL list */
    emit(js, f, &ev);
}

void au_js_fetch_data(AuJs *js, uint32_t id, const void *data, size_t len)
{
    if (js_worker_defer_fetch(js, 1, id, 0, NULL, NULL, 0, NULL, data, len))
        return;
    JsFetch *f = find_fetch(js, id, true, NULL);
    if (!f || js->closing || !f->final || f->hide_body || !len)
        return;
    FetchEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = 1;
    ev.data = data;
    ev.len = len;
    emit(js, f, &ev);
}

void au_js_fetch_done(AuJs *js, uint32_t id, bool ok)
{
    if (js_worker_defer_fetch(js, ok ? 2 : 3, id, 0, NULL, NULL, 0, NULL, NULL, 0))
        return;
    JsFetch *f = find_fetch(js, id, true, NULL);
    if (!f || js->closing)
        return;
    f->hop = 0;
    if (!ok || !f->final) {
        /* a failure, or a preflight or redirect hop that ended without a
         * response head */
        fail(js, f, f->phase == PH_PREFLIGHT ? "the preflight request failed" : "network error",
             f->phase == PH_PREFLIGHT);
        return;
    }
    FetchEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = 2;
    emit(js, f, &ev);
    drop_fetch(js, f);
}
