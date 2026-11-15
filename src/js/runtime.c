/* The runtime: QuickJS setup, allocation and limits, error reporting,
 * running scripts, microtask checkpoints. */
#include "js_int.h"

#include <stdio.h>
#include <string.h>

/* ---- Allocation ----
 * QuickJS allocates through these, so its memory comes from Aura's
 * non-fatal allocator: a failure returns NULL, which QuickJS turns into an
 * out-of-memory exception for the script. A size header lets QuickJS count
 * large blocks exactly (its memory limit relies on usable_size). The
 * header is 16 bytes to keep the alignment malloc gives. */
#define HDR 16

/* opaque is the AuJs: live counts what the runtime holds, so au_js_free
 * can report a leak (QuickJS only asserts in debug builds), and enforces
 * the memory limit. After the first refusal a small reserve opens, so
 * QuickJS can build its "out of memory" error (without it the error
 * object itself fails and script sees a catchable null); js_leave closes
 * it again. */
#define OOM_RESERVE (64u << 10)

static bool over_limit(AuJs *js, size_t more)
{
    size_t limit = js->settings.memory_limit + (js->oom ? OOM_RESERVE : 0);
    if (js->live_bytes + more <= limit && more <= limit)
        return false;
    js->oom = true;
    return true;
}

static void *qa_malloc(void *opaque, size_t size)
{
    if (over_limit(opaque, size))
        return NULL;
    char *p = au_try_alloc(size + HDR);
    if (!p)
        return NULL;
    *(size_t *)p = size;
    ((AuJs *)opaque)->live_bytes += size;
    return p + HDR;
}

static void *qa_calloc(void *opaque, size_t count, size_t size)
{
    if (size && count > ((size_t)-1 - HDR) / size)
        return NULL;
    void *p = qa_malloc(opaque, count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

static void qa_free(void *opaque, void *ptr)
{
    if (ptr) {
        char *p = (char *)ptr - HDR;
        ((AuJs *)opaque)->live_bytes -= *(size_t *)p;
        au_free(p);
    }
}

static void *qa_realloc(void *opaque, void *ptr, size_t size)
{
    if (!ptr)
        return qa_malloc(opaque, size);
    if (!size) {
        qa_free(opaque, ptr);
        return NULL;
    }
    size_t old = *(size_t *)((char *)ptr - HDR);
    if (size > old && over_limit(opaque, size - old))
        return NULL;
    char *p = au_try_realloc((char *)ptr - HDR, size + HDR);
    if (!p)
        return NULL;
    *(size_t *)p = size;
    ((AuJs *)opaque)->live_bytes += size - old;
    return p + HDR;
}

static size_t qa_usable(const void *ptr)
{
    return ptr ? *(const size_t *)((const char *)ptr - HDR) : 0;
}

static const JSMallocFunctions qa_funcs = {qa_calloc, qa_malloc, qa_free, qa_realloc, qa_usable};

/* ---- Helpers ---- */

AuJs *js_of_ctx(JSContext *ctx)
{
    return JS_GetContextOpaque(ctx);
}

double js_now(const AuJs *js)
{
    return js->hooks.now_ms ? js->hooks.now_ms(js->hooks.ctx) : 0;
}

JSValue js_str(JSContext *ctx, const char *s, size_t len)
{
    return JS_NewStringLen(ctx, s ? s : "", s ? len : 0);
}

char *js_tostr(JSContext *ctx, JSValueConst v, size_t *len)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return NULL;
    char *r = au_strndup(s, n);
    JS_FreeCString(ctx, s);
    if (len)
        *len = n;
    return r;
}

void js_console(AuJs *js, AuJsLogLevel level, const char *text, size_t len)
{
    if (js->hooks.console)
        js->hooks.console(js->hooks.ctx, level, text, len);
}

void au_js_settings_default(AuJsSettings *s)
{
    memset(s, 0, sizeof *s);
    s->memory_limit = 32u << 20;
    s->stack_limit = 512u << 10;
    /* AddressSanitizer frames in unoptimized builds are about a hundred
     * times larger (QuickJS's interpreter frame with redzones); the host
     * tools run script on a thread with a stack big enough for this. */
#if defined(__SANITIZE_ADDRESS__)
    s->stack_limit *= 64;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    s->stack_limit *= 64;
#endif
#endif
    s->slice_ms = 1000;
    s->kill_ms = 10000;
    s->online = true;
    s->cookies_enabled = true;
    s->storage_quota = 5u << 20;
    s->screen_width = 1024;
    s->screen_height = 768;
}

/* ---- Limits ---- */

static int interrupt_cb(JSContext *cctx, void *opaque)
{
    (void)cctx;
    AuJs *js = opaque;
    if (js->depth == 0 || js->closing)
        return js->closing;
    if (js->interrupted)
        return 1;
    double now = js_now(js);
    if (now < js->next_check)
        return 0;
    uint32_t elapsed = (uint32_t)(now - js->run_start);
    uint32_t step = js->settings.slice_ms;
    if (!js->hooks.long_script && js->settings.kill_ms < step)
        step = js->settings.kill_ms;
    js->next_check = now + step;
    bool go_on;
    if (js->hooks.long_script)
        go_on = js->hooks.long_script(js->hooks.ctx, elapsed);
    else
        go_on = elapsed < js->settings.kill_ms;
    if (go_on)
        return 0;
    js->interrupted = true;
    js->interrupts++;
    return 1;
}

void js_enter(AuJs *js)
{
    if (js->depth++ == 0) {
        JS_UpdateStackTop(js->rt);
        js->run_start = js_now(js);
        js->next_check = js->run_start + js->settings.slice_ms;
        js->interrupted = false;
    }
}

void js_leave(AuJs *js)
{
    if (--js->depth == 0) {
        js->interrupted = false;
        js->oom = false;
        js_safe_point(js);
    }
}

/* ---- Errors ---- */

static void append_value(JSContext *ctx, AuBuf *b, JSValueConst v)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (s) {
        au_buf_append(b, s, n);
        JS_FreeCString(ctx, s);
    } else {
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
        au_buf_puts(b, "(unprintable)");
    }
}

/* Writes an uncaught error to the console. handled: the page's error or
 * unhandledrejection listener cancelled the report; the line then goes
 * out at debug level and is not counted, so that a failure a page hides
 * (Google's window.onerror returns true) still shows in the console. */
static void report_value(AuJs *js, JSValueConst exc, const char *prefix, bool handled)
{
    JSContext *ctx = js->ctx;
    AuBuf b;
    au_buf_init(&b);
    if (js->interrupted || js->closing) {
        au_buf_puts(&b, "Script stopped: it ran too long or the page was closed");
    } else if (JS_IsUncatchableError(exc)) {
        au_buf_puts(&b, "Script stopped: ");
        append_value(ctx, &b, exc);
    } else {
        au_buf_puts(&b, prefix);
        append_value(ctx, &b, exc);
        if (JS_IsError(exc)) {
            JSValue st = JS_GetPropertyStr(ctx, exc, "stack");
            if (JS_IsString(st)) {
                au_buf_putc(&b, '\n');
                append_value(ctx, &b, st);
                while (b.len && b.data[b.len - 1] == '\n')
                    b.len--;
            }
            JS_FreeValue(ctx, st);
        }
    }
    if (!handled)
        js->errors++;
    js_console(js, handled ? AU_JS_DEBUG : AU_JS_ERROR, b.data ? b.data : "", b.len);
    au_buf_free(&b);
}

void js_report_exception(AuJs *js)
{
    JSContext *ctx = js->ctx;
    JSValue exc = JS_GetException(ctx);
    if (JS_IsUninitialized(exc)) /* none pending ("throw null" is reported) */
        return;
    /* window.onerror and error listeners first (they may cancel the
     * report); not for interrupts, which must not run more script. */
    bool report = true;
    if (!JS_IsUncatchableError(exc) && !js->interrupted && !JS_IsUndefined(js->P) && !js->closing) {
        JSValue fn = JS_GetPropertyStr(ctx, js->P, "reportException");
        if (JS_IsFunction(ctx, fn)) {
            JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&exc);
            if (JS_IsException(r)) {
                JSValue e2 = JS_GetException(ctx);
                JS_FreeValue(ctx, e2);
            } else if (JS_IsBool(r) && !JS_ToBool(ctx, r)) {
                report = false;
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, fn);
    }
    report_value(js, exc, report ? "Uncaught " : "Uncaught (handled by the page) ", !report);
    JS_FreeValue(ctx, exc);
}

JSValue js_call(AuJs *js, JSValueConst fn, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (js->closing)
        return JS_UNDEFINED;
    js_enter(js);
    JSValue r = JS_Call(js->ctx, fn, this_val, argc, argv);
    if (JS_IsException(r)) {
        js_report_exception(js);
        r = JS_UNDEFINED;
    }
    js_leave(js);
    return r;
}

JSValue js_call_prelude(AuJs *js, const char *name, int argc, JSValueConst *argv)
{
    if (JS_IsUndefined(js->P) || js->closing)
        return JS_UNDEFINED;
    JSValue fn = JS_GetPropertyStr(js->ctx, js->P, name);
    JSValue r = JS_UNDEFINED;
    if (JS_IsFunction(js->ctx, fn))
        r = js_call(js, fn, JS_UNDEFINED, argc, argv);
    JS_FreeValue(js->ctx, fn);
    return r;
}

/* ---- Promise rejections ---- */

static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason, bool is_handled,
                              void *opaque)
{
    AuJs *js = opaque;
    if (is_handled) {
        for (uint32_t i = 0; i < js->nrejections; i++)
            if (JS_VALUE_GET_PTR(js->rejections[i].promise) == JS_VALUE_GET_PTR(promise)) {
                JS_FreeValue(ctx, js->rejections[i].promise);
                JS_FreeValue(ctx, js->rejections[i].reason);
                js->rejections[i] = js->rejections[--js->nrejections];
                return;
            }
        return;
    }
    if (js->nrejections >= 256 || js_module_probing(js))
        return;
    if (js->nrejections == js->rejections_cap) {
        js->rejections_cap = js->rejections_cap ? js->rejections_cap * 2 : 8;
        js->rejections = au_realloc(js->rejections, js->rejections_cap * sizeof *js->rejections);
    }
    js->rejections[js->nrejections].promise = JS_DupValue(ctx, promise);
    js->rejections[js->nrejections].reason = JS_DupValue(ctx, reason);
    js->nrejections++;
}

static void report_rejections(AuJs *js)
{
    while (js->nrejections) {
        JsRejection r = js->rejections[--js->nrejections];
        JSValue args[2] = {r.promise, r.reason};
        JSValue h = js_call_prelude(js, "unhandledRejection", 2, args);
        bool log = !(JS_IsBool(h) && !JS_ToBool(js->ctx, h));
        JS_FreeValue(js->ctx, h);
        report_value(js, r.reason, log ? "Uncaught (in promise) " : "Uncaught (in promise, handled by the page) ", !log);
        JS_FreeValue(js->ctx, r.promise);
        JS_FreeValue(js->ctx, r.reason);
    }
}

/* ---- Checkpoints ---- */

static void run_jobs(AuJs *js)
{
    JSContext *c;
    for (int guard = 0; guard < 1000000; guard++) {
        int r = JS_ExecutePendingJob(js->rt, &c);
        if (r == 0)
            break;
        if (r < 0)
            js_report_exception(js);
    }
}

void au_js_checkpoint(AuJs *js)
{
    /* Microtasks run only when no script is on the stack. */
    if (js->in_checkpoint || js->closing || js->depth > 0)
        return;
    js->in_checkpoint = true;
    js_enter(js);
    for (int round = 0; round < 64; round++) {
        run_jobs(js);
        bool more = false;
        if (js->nreacts) {
            js_run_reactions(js);
            more = true;
        }
        if (js->nmuts || js->slots_dirty) {
            js_deliver_mutations(js, true);
            more = true;
        }
        if (!more && !JS_IsJobPending(js->rt))
            break;
    }
    report_rejections(js);
    js_leave(js);
    js->in_checkpoint = false;
}

void js_safe_point(AuJs *js)
{
    js_drain_pending(js);
}

/* ---- Natives every part uses ---- */

static JSValue n_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    int32_t level = 0;
    JS_ToInt32(ctx, &level, argv[0]);
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!s)
        return JS_EXCEPTION;
    js_console(js, (AuJsLogLevel)(level < 0 || level > AU_JS_DEBUG ? 0 : level), s, n);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue n_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_NewFloat64(ctx, js_now(js_of_ctx(ctx)));
}

/* runScript(source, url, element or null): runs a classic script inserted
 * by script, with element as currentScript. Errors are reported, not
 * thrown. Returns whether it completed. */
static JSValue n_run_script(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    size_t len;
    char *src = argc > 0 ? js_tostr(ctx, argv[0], &len) : NULL;
    char *url = argc > 1 && JS_IsString(argv[1]) ? js_tostr(ctx, argv[1], NULL) : NULL;
    if (!src) {
        au_free(url);
        return JS_EXCEPTION;
    }
    AuNode *n = argc > 2 ? js_unwrap(js, argv[2]) : NULL;
    AuElement *el = n && n->type == AU_NODE_ELEMENT ? (AuElement *)au_node_ref(n) : NULL;
    AuElement *prev = js->current_script;
    js->current_script = el;
    JSEvalOptions o = {JS_EVAL_OPTIONS_VERSION, JS_EVAL_TYPE_GLOBAL, url ? url : js->url, 1};
    JSValue r = JS_Eval2(ctx, src, len, &o);
    bool ok = !JS_IsException(r);
    if (!ok) {
        /* An interrupt must keep unwinding to the embedder. */
        JSValue e = JS_GetException(ctx);
        if (JS_IsUncatchableError(e) || js->interrupted) {
            js->current_script = prev;
            if (el)
                au_node_unref(&el->node);
            au_free(src);
            au_free(url);
            return JS_Throw(ctx, e);
        }
        JS_Throw(ctx, e);
        js_report_exception(js);
    }
    JS_FreeValue(ctx, r);
    js->current_script = prev;
    if (el)
        au_node_unref(&el->node);
    au_free(src);
    au_free(url);
    return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry base_natives[] = {
    JS_CFUNC_DEF("print", 2, n_print),
    JS_CFUNC_DEF("now", 0, n_now),
    JS_CFUNC_DEF("runScript", 3, n_run_script),
};

/* ---- Creation ---- */

extern const char au_js_prelude_src[];
extern const size_t au_js_prelude_len;

static bool load_prelude(AuJs *js, JSValue N)
{
    JSContext *ctx = js->ctx;
    JSValue fn = JS_Eval(ctx, au_js_prelude_src, au_js_prelude_len, "aura:prelude", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        js_report_exception(js);
        return false;
    }
    JSValue args[2] = {N, JS_GetGlobalObject(ctx)};
    JSValue P = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(P)) {
        js_report_exception(js);
        return false;
    }
    js->P = P;
    return true;
}

AuJs *au_js_new(AuDocument *doc, const char *url, const AuJsSettings *settings, const AuJsHooks *hooks)
{
    AuJs *js = au_calloc(sizeof *js);
    au_js_settings_default(&js->settings);
    if (settings) {
        AuJsSettings d = js->settings;
        js->settings = *settings;
        if (!js->settings.memory_limit)
            js->settings.memory_limit = d.memory_limit;
        if (!js->settings.stack_limit)
            js->settings.stack_limit = d.stack_limit;
        if (!js->settings.slice_ms)
            js->settings.slice_ms = d.slice_ms;
        if (!js->settings.kill_ms)
            js->settings.kill_ms = d.kill_ms;
        if (!js->settings.storage_quota)
            js->settings.storage_quota = d.storage_quota;
        if (!js->settings.screen_width || !js->settings.screen_height) {
            js->settings.screen_width = d.screen_width;
            js->settings.screen_height = d.screen_height;
        }
    }
    if (hooks)
        js->hooks = *hooks;
    js->P = JS_UNDEFINED;
    js->rt = JS_NewRuntime2(&qa_funcs, js);
    if (!js->rt) {
        au_free(js);
        return NULL;
    }
    JS_SetRuntimeOpaque(js->rt, js);
    /* The memory limit is the allocator's (over_limit), not
     * JS_SetMemoryLimit's, so the out-of-memory error can be built. */
    JS_SetMaxStackSize(js->rt, js->settings.stack_limit);
    JS_SetInterruptHandler(js->rt, interrupt_cb, js);
    JS_SetHostPromiseRejectionTracker(js->rt, rejection_tracker, js);
    JSContext *ctx = JS_NewContextRaw(js->rt);
    if (!ctx || JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicDate(ctx) || JS_AddIntrinsicEval(ctx) ||
        JS_AddIntrinsicRegExp(ctx) || JS_AddIntrinsicJSON(ctx) || JS_AddIntrinsicProxy(ctx) ||
        JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicTypedArrays(ctx) || JS_AddIntrinsicPromise(ctx) ||
        JS_AddIntrinsicWeakRef(ctx) || JS_AddIntrinsicAToB(ctx)) {
        if (ctx)
            JS_FreeContext(ctx);
        JS_FreeRuntime(js->rt);
        au_free(js);
        return NULL;
    }
    js->ctx = ctx;
    JS_SetContextOpaque(ctx, js);
    js->doc = doc;
    au_node_ref(&doc->node);
    js->url = au_strndup(url ? url : "about:blank", strlen(url ? url : "about:blank"));
    js->base = au_strndup(js->url, strlen(js->url));
    char origin[512];
    js_origin_of(js->url, origin, sizeof origin);
    js->origin = au_strndup(origin, strlen(origin));
    JSValue g = JS_GetGlobalObject(ctx);
    js->global = g;
    JS_FreeValue(ctx, g); /* ctx keeps it */

    js_enter(js);
    js_wrap_init(js);
    js_hooks_install(js);
    JSValue N = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, N, base_natives, JS_COUNTOF(base_natives));
    js_natives_init(js, N);
    js_natives2_init(js, N);
    js_natives_forms_init(js, N);
    js_loop_init(js, N);
    js_fetch_init(js, N);
    js_module_init(js);
    js_module_natives(js, N);
    bool ok = load_prelude(js, N);
    JS_FreeValue(ctx, N);
    js_leave(js);
    if (!ok) {
        au_js_free(js);
        return NULL;
    }
    return js;
}

void au_js_free(AuJs *js)
{
    if (!js)
        return;
    js->closing = true;
    JSContext *ctx = js->ctx;
    js_loop_free(js);
    js_fetch_free(js);
    js_module_free(js);
    js_mutations_free(js);
    for (uint32_t i = 0; i < js->nrejections; i++) {
        JS_FreeValue(ctx, js->rejections[i].promise);
        JS_FreeValue(ctx, js->rejections[i].reason);
    }
    js->nrejections = 0;
    au_free(js->rejections);
    JS_FreeValue(ctx, js->P);
    js->P = JS_UNDEFINED;
    js_wrap_free_all(js); /* drops the nodes' references to their wrappers */
    JS_FreeContext(ctx);
    JS_FreeRuntime(js->rt);
    if (js->live_bytes) {
        /* Objects outlived the runtime: a reference leak in a binding or
         * the engine. Reported so tests catch it in release builds. */
        char msg[96];
        int n = snprintf(msg, sizeof msg, "Internal error: %lu bytes of script memory leaked at teardown",
                         (unsigned long)js->live_bytes);
        js->errors++;
        js_console(js, AU_JS_ERROR, msg, (size_t)n);
    }
    js_hooks_remove(js);
    if (js->current_script)
        au_node_unref(&js->current_script->node);
    for (uint32_t i = 0; i < js->ndocs; i++)
        au_node_unref(&js->docs[i]->node);
    au_free(js->docs);
    au_node_unref(&js->doc->node);
    au_free(js->url);
    au_free(js->base);
    au_free(js->origin);
    au_free(js);
}

void au_js_set_parser(AuJs *js, AuHtmlParser *p)
{
    js->parser = p;
}

void au_js_set_url(AuJs *js, const char *url)
{
    au_free(js->url);
    js->url = au_strndup(url, strlen(url));
}

void au_js_set_base_url(AuJs *js, const char *base)
{
    au_free(js->base);
    js->base = au_strndup(base, strlen(base));
}

AuJs *au_js_of(const AuDocument *doc)
{
    if (!doc || !doc->hooks)
        return NULL;
    return doc->hooks_ctx;
}

/* ---- Running scripts ---- */

bool au_js_run_classic(AuJs *js, AuElement *el, const char *src, size_t len, const char *url, int line)
{
    if (js->closing)
        return false;
    JSContext *ctx = js->ctx;
    AuElement *prev = js->current_script;
    if (el)
        au_node_ref(&el->node);
    js->current_script = el;
    js_enter(js);
    JSEvalOptions o = {JS_EVAL_OPTIONS_VERSION, JS_EVAL_TYPE_GLOBAL, url ? url : js->url, line > 0 ? line : 1};
    /* QuickJS wants a NUL-terminated source; src may not be. */
    char *copy = au_strndup(src, len);
    JSValue r = JS_Eval2(ctx, copy, len, &o);
    au_free(copy);
    bool ok = !JS_IsException(r);
    if (!ok)
        js_report_exception(js);
    JS_FreeValue(ctx, r);
    js->current_script = prev;
    if (el)
        au_node_unref(&el->node);
    js_leave(js);
    au_js_checkpoint(js);
    return ok;
}

bool au_js_eval(AuJs *js, const char *src, size_t len, AuBuf *out)
{
    JSContext *ctx = js->ctx;
    js_enter(js);
    char *copy = au_strndup(src, len);
    JSValue r = JS_Eval(ctx, copy, len, "<eval>", JS_EVAL_TYPE_GLOBAL);
    au_free(copy);
    bool ok = !JS_IsException(r);
    if (ok) {
        if (out)
            append_value(ctx, out, r);
    } else {
        JSValue e = JS_GetException(ctx);
        if (out) {
            au_buf_puts(out, "Error: ");
            append_value(ctx, out, e);
        }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    js_leave(js);
    au_js_checkpoint(js);
    return ok;
}

void au_js_run_javascript_url(AuJs *js, const char *url)
{
    if (strncmp(url, "javascript:", 11) != 0)
        return;
    JSValue s = js_str(js->ctx, url + 11, strlen(url + 11));
    JSValue r = js_call_prelude(js, "runJavascriptUrl", 1, &s);
    JS_FreeValue(js->ctx, r);
    JS_FreeValue(js->ctx, s);
    au_js_checkpoint(js);
}

void au_js_gc(AuJs *js)
{
    js_drain_pending(js);
    JS_RunGC(js->rt);
    js_drain_pending(js);
}

void au_js_stats(const AuJs *js, AuJsStats *s)
{
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(js->rt, &mu);
    s->memory_used = (size_t)mu.malloc_size;
    s->memory_limit = js->settings.memory_limit;
    s->wrappers = js->nwraps;
    s->errors = js->errors;
    s->interrupted = js->interrupts;
}
