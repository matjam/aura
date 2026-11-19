/* The runtime: QuickJS setup, allocation and limits, error reporting,
 * running scripts, microtask checkpoints. */
#include "js_int.h"
#include "../codec/codec.h"

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
 * it again.
 *
 * The header holds the block's size and, 8 bytes in, the slot of the
 * context it is charged to (JsMemSlot in js_int.h). */
#define OOM_RESERVE (64u << 10)
#define JS_PACK_MS 5000 /* how often js_code_expire packs source text */
#define HDR_SLOT(p) (*(uint32_t *)((char *)(p) + 8))

/* Nearing the limit, the collector must run before the limit refuses an
 * allocation: QuickJS collects only when the heap has grown by half since
 * the last collection, which with a 20 MB heap is past a 24 MB limit. A
 * collection inside the allocator is not safe (the engine may be in the
 * middle of building an object), so the allocator lowers the engine's
 * threshold instead, and the engine collects at its next object creation.
 * Each time, the level moves half way to the limit, so a heap that really
 * is that full is collected a few times, not at every allocation. */
static void gc_pressure(AuJs *js, size_t more)
{
    size_t limit = js->settings.memory_limit;
    if (!js->gc_force_at || js->live_bytes < limit / 4 * 3)
        js->gc_force_at = limit / 8 * 7;
    if (js->live_bytes + more <= js->gc_force_at || !js->rt)
        return;
    size_t gap = limit > js->live_bytes ? limit - js->live_bytes : 0;
    if (gap < (128u << 10)) {
        js->gc_force_at = (size_t)-1 / 2; /* full: no more forced collections until it empties */
        return;
    }
    js->gc_force_at = js->live_bytes + gap / 2;
    JS_SetGCThreshold(js->rt, 0);
}

static bool over_limit(AuJs *js, size_t more)
{
    size_t limit = js->settings.memory_limit + (js->oom ? OOM_RESERVE : 0);
    bool machine = false;
    if (js->live_bytes + more <= limit && more <= limit) {
        /* The engine asks for 4 KB arenas and larger blocks, so this is
         * not asked for every object. The reserve is exempt: it is what
         * reports the error. */
        machine = more >= 2048 && !js->oom && au_memory_low(more);
        if (!machine) {
            gc_pressure(js, more);
            return false;
        }
    }
    if (!js->oom) {
        js->oom_machine = machine;
        js->oom_live = js->live_bytes;
        js->oom_asked = more;
        snprintf(js->oom_url, sizeof js->oom_url, "%s", js->compiling ? js->compiling : "");
        js->oom_noted = true;
    }
    js->oom = true;
    return true;
}

static void charge(AuJs *js, char *p, size_t size)
{
    js->live_bytes += size;
    if (js->live_bytes > js->peak_bytes)
        js->peak_bytes = js->live_bytes;
    uint32_t slot = js->cur_slot < js->nslots ? js->cur_slot : 0;
    HDR_SLOT(p) = slot;
    if (js->nslots) {
        JsMemSlot *m = &js->slots[slot];
        m->live += size;
        if (m->live > m->peak)
            m->peak = m->live;
    }
}

static void uncharge(AuJs *js, const char *p, size_t size)
{
    js->live_bytes -= size;
    uint32_t slot = HDR_SLOT(p);
    if (slot < js->nslots)
        js->slots[slot].live -= size;
}

static void *qa_malloc(void *opaque, size_t size)
{
    if (over_limit(opaque, size))
        return NULL;
    char *p = au_try_alloc(size + HDR);
    if (!p)
        return NULL;
    *(size_t *)p = size;
    charge(opaque, p, size);
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
        uncharge(opaque, p, *(size_t *)p);
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
    AuJs *js = opaque;
    size_t old = *(size_t *)((char *)ptr - HDR);
    if (size > old && over_limit(js, size - old))
        return NULL;
    char *p = au_try_realloc((char *)ptr - HDR, size + HDR);
    if (!p)
        return NULL;
    /* the block stays with the context that first allocated it */
    uint32_t slot = HDR_SLOT(p);
    *(size_t *)p = size;
    js->live_bytes += size - old;
    if (js->live_bytes > js->peak_bytes)
        js->peak_bytes = js->live_bytes;
    if (slot < js->nslots) {
        JsMemSlot *m = &js->slots[slot];
        m->live += size - old;
        if (m->live > m->peak)
            m->peak = m->live;
    }
    return p + HDR;
}

static size_t qa_usable(const void *ptr)
{
    return ptr ? *(const size_t *)((const char *)ptr - HDR) : 0;
}

/* Gives js a slot of its own in the runtime's accounting. */
static void slot_new(AuJs *js)
{
    AuJs *r = js->root;
    uint32_t n = r->nslots ? r->nslots : 1; /* slot 0: no context */
    r->slots = au_realloc(r->slots, (n + 1) * sizeof *r->slots);
    if (!r->nslots)
        memset(&r->slots[0], 0, sizeof r->slots[0]);
    memset(&r->slots[n], 0, sizeof r->slots[n]);
    r->nslots = n + 1;
    js->slot = n;
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
    /* 512 KB with 32-bit pointers (Mac OS 9), 1 MB with 64-bit ones: a
     * call from script to script takes one C frame of QuickJS's
     * interpreter, 608 bytes on PowerPC and 960 on a 64-bit ARM host,
     * and YouTube's home page goes about 600 calls deep, which 512 KB
     * did not hold on the host. */
    s->stack_limit = ((size_t)128 << 10) * sizeof(void *);
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

/* hooks.yield, when settings.yield_ms has passed since the last call.
 * Returns nonzero when the embedder stops the script. */
static int maybe_yield(AuJs *js, double *now)
{
    if (!js->hooks.yield || !js->settings.yield_ms || *now < js->next_yield)
        return 0;
    double t0 = *now;
    int stop = js->hooks.yield(js->hooks.ctx, (uint32_t)(t0 - js->run_start));
    *now = js_now(js);
    /* the time away is not the script's */
    js->run_start += *now - t0;
    js->next_check += *now - t0;
    js->next_yield = *now + js->settings.yield_ms;
    return stop || js->closing;
}

static int interrupt_check(AuJs *js, bool may_yield)
{
    if (js->worker) {
        int r = js_worker_interrupt(js);
        if (r >= 0)
            return r;
    }
    if (js->depth == 0 || js->closing)
        return js->closing;
    if (js->interrupted)
        return 1;
    double now = js_now(js);
    if (may_yield && maybe_yield(js, &now)) {
        js->interrupted = true;
        js->interrupts++;
        return 1;
    }
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

/* Function source text, which only Function.prototype.toString reads, is
 * deflated while nothing reads it (au_js_pack_sources). Minified script
 * packs to about a third. */
static void *source_pack(JSRuntime *rt, void *opaque, const char *text, size_t len, size_t *packed_len)
{
    (void)opaque;
    AuBuf z;
    au_buf_init(&z);
    au_deflate_all(AU_DEFLATE_RAW, 1, text, len, &z);
    void *p = z.len && z.len < len ? js_malloc_rt(rt, z.len) : NULL;
    if (p) {
        memcpy(p, z.data, z.len);
        *packed_len = z.len;
    }
    au_buf_free(&z);
    return p;
}

static int source_unpack(JSRuntime *rt, void *opaque, const void *packed, size_t packed_len, char *out, size_t len)
{
    (void)rt;
    (void)opaque;
    AuInflate *z = au_inflate_new(AU_INFLATE_RAW);
    if (!z)
        return -1;
    const uint8_t *in = packed;
    size_t in_len = packed_len, out_len = len;
    uint8_t *o = (uint8_t *)out;
    AuCodecStatus st = au_inflate_run(z, &in, &in_len, &o, &out_len);
    au_inflate_free(z);
    return st == AU_CODEC_DONE && out_len == 0 ? 0 : -1;
}

static int interrupt_cb(JSContext *cctx, void *opaque)
{
    (void)cctx;
    return interrupt_check(opaque, true);
}

/* The engine asks interrupt_cb only once in ten thousand steps (loop
 * iterations and calls). That is often enough for script that computes,
 * but a native that forces a layout can take a large part of a second,
 * and ten thousand of those before the first check is longer than anyone
 * waits: a mutation observer that read the page's size on every change
 * ran for minutes. Natives that can be slow call js_out_of_time first
 * and, when it is true, return js_throw_stopped: the same uncatchable
 * error the interrupt raises. */
bool js_out_of_time(AuJs *js)
{
    AuJs *r = js->root;
    if (r->depth == 0 || r->closing)
        return false;
    return interrupt_check(r, false) != 0; /* a native does not leave its stack */
}

JSValue js_throw_stopped(JSContext *ctx)
{
    JS_ThrowInternalError(ctx, "interrupted");
    JSValue e = JS_GetException(ctx);
    JS_SetUncatchableError(ctx, e);
    return JS_Throw(ctx, e);
}

/* The entry depth, the budget and the interrupt state belong to the
 * runtime, so they live in the root AuJs: a frame's script that runs
 * inside its parent's call shares the parent's budget. root->entered
 * names the AuJs of each entry, innermost last (postMessage's source). */
void js_enter(AuJs *js)
{
    AuJs *r = js->root;
    if (r->depth < (int)JS_COUNTOF(r->entered))
        r->entered[r->depth] = js;
    r->cur_slot = js->slot;
    if (r->depth++ == 0) {
        JS_UpdateStackTop(r->rt);
        r->run_start = js_now(r);
        r->next_check = r->run_start + r->settings.slice_ms;
        r->next_yield = r->run_start + r->settings.yield_ms;
        r->interrupted = false;
    }
}

void js_leave(AuJs *js)
{
    AuJs *r = js->root;
    if (--r->depth > 0 && r->depth <= (int)JS_COUNTOF(r->entered))
        r->cur_slot = r->entered[r->depth - 1]->slot;
    if (r->depth == 0) {
        r->cur_slot = 0;
        r->interrupted = false;
        r->oom = false;
        r->oom_noted = false;
        js_safe_point(js);
        for (AuJs *g = r; g; g = g->group_next)
            if (g->canvas_pending)
                js_canvas_sync(g); /* width or height attributes script changed */
    }
}

const char *au_js_origin(const AuJs *js)
{
    return js->origin;
}

bool au_js_running(const AuJs *js)
{
    return js->root->depth > 0;
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
    if (js->worker && js_worker_terminating(js))
        return; /* terminate() stopped it: nothing to report */
    AuBuf b;
    au_buf_init(&b);
    if (js->root->interrupted || js->closing) {
        au_buf_puts(&b, "Script stopped: it ran too long or the page was closed");
    } else if (JS_IsUncatchableError(exc) && js->root->oom_noted) {
        /* Which runtime hit its limit, how full it was and what it was
         * doing, so that the line says what to change. */
        AuJs *r = js->root;
        r->oom_noted = false;
        if (r->oom_machine && r->settings.memory_limit >= (size_t)-1 / 2)
            au_buf_printf(&b, "Script stopped: out of memory. Aura has too little memory left to give the %s %s more (its script memory is unlimited; %lu KB in use, %lu KB more asked for) ",
                          r->worker ? "worker" : "page", r->url, (unsigned long)(r->oom_live / 1024),
                          (unsigned long)((r->oom_asked + 1023) / 1024));
        else if (r->oom_machine)
            au_buf_printf(&b, "Script stopped: out of memory. Aura has too little memory left to give the %s %s more (its script memory limit is %lu KB; %lu KB in use, %lu KB more asked for) ",
                          r->worker ? "worker" : "page", r->url, (unsigned long)(r->settings.memory_limit / 1024),
                          (unsigned long)(r->oom_live / 1024), (unsigned long)((r->oom_asked + 1023) / 1024));
        else
            au_buf_printf(&b, "Script stopped: out of memory. The %s %s reached its script memory limit of %lu KB (%lu KB in use, %lu KB more asked for) ",
                          r->worker ? "worker" : "page", r->url, (unsigned long)(r->settings.memory_limit / 1024),
                          (unsigned long)(r->oom_live / 1024), (unsigned long)((r->oom_asked + 1023) / 1024));
        if (r->oom_url[0])
            au_buf_printf(&b, "while compiling %s", r->oom_url);
        else
            au_buf_puts(&b, "while running script");
        if (js != r)
            au_buf_printf(&b, " in the frame %s", js->url);
        if (r->worker) {
            char note[160];
            snprintf(note, sizeof note, "out of memory: the worker %s its script memory limit of %lu KB",
                     r->oom_machine ? "could not grow to" : "reached", (unsigned long)(r->settings.memory_limit / 1024));
            js_worker_fatal(r, note);
        }
        if (js->hooks.out_of_memory)
            js->hooks.out_of_memory(js->hooks.ctx, r->worker, r->oom_machine,
                                    (uint32_t)(r->settings.memory_limit / 1024));
        if (r->oom_machine)
            au_buf_puts(&b, ". Close other tabs or applications, or give Aura more memory in its Get Info window.");
        else
            au_buf_printf(&b, ". To allow more, raise \"Script memory per %s\" in Preferences > Content (the %s preference).",
                          r->worker ? "worker" : "page", r->worker ? "content.worker_memory_kb" : "content.script_memory_kb");
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
    if (!JS_IsUncatchableError(exc) && !js->root->interrupted && !JS_IsUndefined(js->P) && !js->closing) {
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
    AuJs *js = opaque; /* the root: rejections of every context are kept there */
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
    AuJs *root = js->root;
    while (root->nrejections) {
        JsRejection r = root->rejections[--root->nrejections];
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
        /* The script's time is up: the jobs left wait for the next
         * entry. Each further job would run up to ten thousand steps
         * before the engine asked again, and jobs that queue jobs
         * (observers that change the tree) kept that up for minutes. */
        if (js->root->interrupted)
            break;
    }
}

void au_js_checkpoint(AuJs *js)
{
    /* Microtasks run only when no script is on the stack. The job queue
     * is the runtime's, so one checkpoint serves every document of the
     * page: script in one frame may have changed another's tree. */
    AuJs *root = js->root;
    if (root->in_checkpoint || js->closing || root->depth > 0)
        return;
    root->in_checkpoint = true;
    js_enter(js);
    for (int round = 0; round < 64; round++) {
        run_jobs(js);
        bool more = false;
        for (AuJs *g = root; g && !root->interrupted; g = g->group_next) {
            if (g->closing)
                continue;
            if (g->nreacts) {
                js_run_reactions(g);
                more = true;
            }
            if (g->nmuts || g->slots_dirty) {
                js_deliver_mutations(g, true);
                more = true;
            }
        }
        if (root->interrupted || (!more && !JS_IsJobPending(js->rt)))
            break;
    }
    report_rejections(js);
    js_leave(js);
    root->in_checkpoint = false;
}

static bool gc_stress;

void au_js_set_gc_stress(bool on)
{
    gc_stress = on;
}

void js_safe_point(AuJs *js)
{
    js_drain_pending(js);
    js_code_expire(js);
    /* For tests and fuzzers (au_js_set_gc_stress): collect whenever
     * script has been left, so a node whose wrapper is garbage in a
     * reference cycle (an event object and its target) is freed as soon
     * as script lets go of it, not at some later collection. */
    if (gc_stress && !js->root->closing) {
        JS_RunGC(js->rt);
        js_drain_pending(js);
    }
}

/* ---- Natives every part uses ---- */

static JSValue eval_classic(AuJs *js, const char *src, size_t len, const char *url, int line, char *owned);
static uint32_t hints_for(JSContext *ctx, void *opaque, const char *text, size_t len, const uint32_t **ofs,
                          size_t *count);
static void hint_compiled(JSContext *ctx, void *opaque, uint32_t id, uint32_t len, uint32_t ofs);

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

/* The UTF-8 bytes of a typed array or ArrayBuffer as a C string (without
 * a byte order mark); the buffer is then detached, which frees its
 * memory in the script heap. NULL when v is neither. */
char *js_take_script_bytes(JSContext *ctx, JSValueConst v, size_t *len)
{
    size_t n = 0, off = 0, blen = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, NULL);
    if (JS_IsException(buf)) {
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
        buf = JS_DupValue(ctx, v);
        const uint8_t *q = JS_GetArrayBuffer(ctx, &n, buf);
        if (!q) {
            e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, buf);
            return NULL;
        }
        blen = n;
    }
    const uint8_t *p = JS_GetArrayBuffer(ctx, &n, buf);
    char *out = NULL;
    if (p && off <= n && blen <= n - off) {
        p += off;
        if (blen >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
            p += 3;
            blen -= 3;
        }
        out = au_strndup((const char *)p, blen);
        *len = blen;
        JS_DetachArrayBuffer(ctx, buf);
    } else if (!p) {
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, buf);
    return out;
}

/* runScript(source, url, element or null): runs a classic script inserted
 * by script, with element as currentScript. Errors are reported, not
 * thrown. Returns whether it completed. source is a string; or the
 * script's UTF-8 bytes in a typed array, which is emptied (importScripts:
 * a large script is then held once while it compiles, not as bytes, a
 * string and a copy); or null in a worker, for the worker's own script
 * (worker.c keeps it). The text is freed once compiled. */
static JSValue n_run_script(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    size_t len = 0;
    char *src = NULL;
    if (argc > 0 && JS_IsString(argv[0]))
        src = js_tostr(ctx, argv[0], &len);
    else if (argc > 0 && JS_IsNull(argv[0]) && js->worker)
        src = js_worker_take_source(js, &len);
    else if (argc > 0 && JS_IsObject(argv[0]))
        src = js_take_script_bytes(ctx, argv[0], &len);
    else if (argc > 0)
        src = js_tostr(ctx, argv[0], &len);
    char *url = argc > 1 && JS_IsString(argv[1]) ? js_tostr(ctx, argv[1], NULL) : NULL;
    if (!src) {
        au_free(url);
        return JS_EXCEPTION;
    }
    AuNode *n = argc > 2 ? js_unwrap(js, argv[2]) : NULL;
    AuElement *el = n && n->type == AU_NODE_ELEMENT ? (AuElement *)au_node_ref(n) : NULL;
    AuElement *prev = js->current_script;
    js->current_script = el;
    JSValue r = eval_classic(js, src, len, url, 1, src);
    bool ok = !JS_IsException(r);
    if (!ok) {
        /* An interrupt must keep unwinding to the embedder. */
        JSValue e = JS_GetException(ctx);
        if (JS_IsUncatchableError(e) || js->root->interrupted) {
            js->current_script = prev;
            if (el)
                au_node_unref(&el->node);
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
    au_free(url);
    return JS_NewBool(ctx, ok);
}

void au_js_mute_script(AuJs *js, const char *url)
{
    for (uint32_t i = 0; i < js->nmuted; i++)
        if (!strcmp(js->muted[i], url))
            return;
    if (js->nmuted >= 512)
        return;
    js->muted = au_realloc(js->muted, (js->nmuted + 1) * sizeof *js->muted);
    js->muted[js->nmuted++] = au_strndup(url, strlen(url));
}

static void free_muted(AuJs *js)
{
    for (uint32_t i = 0; i < js->nmuted; i++)
        au_free(js->muted[i]);
    au_free(js->muted);
}

/* muteScript(url) marks a script muted; scriptMuted(url) asks. */
static JSValue n_mute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!url)
        return JS_FALSE;
    bool r = false;
    if (magic) {
        au_js_mute_script(js, url);
    } else {
        for (uint32_t i = 0; i < js->nmuted && !r; i++)
            r = !strcmp(js->muted[i], url);
    }
    JS_FreeCString(ctx, url);
    return JS_NewBool(ctx, r);
}

#ifdef AU_JS_PRELUDE_SOURCE
extern const char au_js_intl_src[];
extern const size_t au_js_intl_len;
#else
extern const unsigned char au_js_intl_bc[];
extern const size_t au_js_intl_bc_len;
#endif

/* N.intl(): reads src/js/intl.js (its bytecode, intl.bc) and returns the
 * function it evaluates to. The prelude calls it once per context, the
 * first time a page touches Intl or a toLocale method, so a page that
 * never does holds none of it. */
static JSValue n_intl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
#ifdef AU_JS_PRELUDE_SOURCE
    return JS_Eval(ctx, au_js_intl_src, au_js_intl_len, "aura:intl", JS_EVAL_TYPE_GLOBAL);
#else
    JSValue fn = JS_ReadObject(ctx, au_js_intl_bc, au_js_intl_bc_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(fn))
        return fn;
    return JS_EvalFunction(ctx, fn);
#endif
}

#ifdef AU_JS_PRELUDE_SOURCE
extern const char au_js_streams_src[], au_js_eventsource_src[];
extern const size_t au_js_streams_len, au_js_eventsource_len;
#else
extern const unsigned char au_js_streams_bc[], au_js_eventsource_bc[];
extern const size_t au_js_streams_bc_len, au_js_eventsource_bc_len;
#endif

/* N.streams() and N.eventSource(): read src/js/streams.js (streams.bc)
 * or src/js/eventsource.js (eventsource.bc) as N.intl() reads Intl: once
 * per context, the first time script touches a stream class or a body's
 * stream, or EventSource. */
static JSValue n_lazy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc;
    (void)argv;
    /* The codec and event stream natives join N (this) with the first
     * module that needs them, so a page that uses neither pays nothing. */
    if (JS_IsObject(this_val)) {
        JSAtom a = JS_NewAtom(ctx, "zNew");
        int has = JS_HasProperty(ctx, this_val, a);
        JS_FreeAtom(ctx, a);
        if (has == 0)
            js_streams_natives_init(JS_GetContextOpaque(ctx), (JSValue)this_val);
    }
#ifdef AU_JS_PRELUDE_SOURCE
    if (magic)
        return JS_Eval(ctx, au_js_eventsource_src, au_js_eventsource_len, "aura:eventsource", JS_EVAL_TYPE_GLOBAL);
    return JS_Eval(ctx, au_js_streams_src, au_js_streams_len, "aura:streams", JS_EVAL_TYPE_GLOBAL);
#else
    JSValue fn = magic ? JS_ReadObject(ctx, au_js_eventsource_bc, au_js_eventsource_bc_len, JS_READ_OBJ_BYTECODE)
                       : JS_ReadObject(ctx, au_js_streams_bc, au_js_streams_bc_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(fn))
        return fn;
    return JS_EvalFunction(ctx, fn);
#endif
}

#ifdef AU_JS_PRELUDE_SOURCE
extern const char au_js_range_src[], au_js_cssom_src[], au_js_canvas_src[], au_js_idb_src[], au_js_urlpattern_src[];
extern const size_t au_js_range_len, au_js_cssom_len, au_js_canvas_len, au_js_idb_len, au_js_urlpattern_len;
#else
extern const unsigned char au_js_range_bc[], au_js_cssom_bc[], au_js_canvas_bc[], au_js_idb_bc[],
    au_js_urlpattern_bc[];
extern const size_t au_js_range_bc_len, au_js_cssom_bc_len, au_js_canvas_bc_len, au_js_idb_bc_len,
    au_js_urlpattern_bc_len;
#endif

/* N.lazy(i): reads module i (0 src/js/range.js, 1 cssom.js, 2 canvas.js,
 * 3 idb.js, 4 urlpattern.js, each compiled to its own bytecode) and
 * returns the function it
 * evaluates to. lazyModule in prelude.js calls it once per context, the
 * first time script touches one of the module's globals or members. */
static JSValue n_lazy_module(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    int32_t i = -1;
    if (argc < 1 || JS_ToInt32(ctx, &i, argv[0]) || i < 0 || i > 4)
        return JS_ThrowRangeError(ctx, "lazy: no such module");
#ifdef AU_JS_PRELUDE_SOURCE
    static const char *const names[] = {"aura:range", "aura:cssom", "aura:canvas", "aura:idb", "aura:urlpattern"};
    const char *const srcs[] = {au_js_range_src, au_js_cssom_src, au_js_canvas_src, au_js_idb_src,
                                au_js_urlpattern_src};
    const size_t lens[] = {au_js_range_len, au_js_cssom_len, au_js_canvas_len, au_js_idb_len, au_js_urlpattern_len};
    return JS_Eval(ctx, srcs[i], lens[i], names[i], JS_EVAL_TYPE_GLOBAL);
#else
    const unsigned char *const bcs[] = {au_js_range_bc, au_js_cssom_bc, au_js_canvas_bc, au_js_idb_bc,
                                        au_js_urlpattern_bc};
    const size_t lens[] = {au_js_range_bc_len, au_js_cssom_bc_len, au_js_canvas_bc_len, au_js_idb_bc_len,
                           au_js_urlpattern_bc_len};
    const unsigned char *bc = bcs[i];
    size_t len = lens[i];
    JSValue fn = JS_ReadObject(ctx, bc, len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(fn))
        return fn;
    return JS_EvalFunction(ctx, fn);
#endif
}

static const JSCFunctionListEntry base_natives[] = {
    JS_CFUNC_MAGIC_DEF("scriptMuted", 1, n_mute, 0),
    JS_CFUNC_MAGIC_DEF("muteScript", 1, n_mute, 1),
    JS_CFUNC_DEF("print", 2, n_print),
    JS_CFUNC_DEF("now", 0, n_now),
    JS_CFUNC_DEF("runScript", 3, n_run_script),
    JS_CFUNC_DEF("intl", 0, n_intl),
    JS_CFUNC_MAGIC_DEF("streams", 0, n_lazy, 0),
    JS_CFUNC_MAGIC_DEF("eventSource", 0, n_lazy, 1),
    JS_CFUNC_DEF("lazy", 1, n_lazy_module),
};

/* ---- Creation ---- */

#ifdef AU_JS_PRELUDE_SOURCE
extern const char au_js_prelude_src[];
extern const size_t au_js_prelude_len;
#else
extern const unsigned char au_js_prelude_bc[];
extern const size_t au_js_prelude_bc_len;
#endif

#ifdef AU_JS_PRELUDE_SOURCE
extern const char au_js_worker_prelude_src[];
extern const size_t au_js_worker_prelude_len;
#else
extern const unsigned char au_js_worker_prelude_bc[];
extern const size_t au_js_worker_prelude_bc_len;
#endif

static bool load_prelude(AuJs *js, JSValue N)
{
    JSContext *ctx = js->ctx;
#ifdef AU_JS_PRELUDE_SOURCE
    JSValue fn = js->worker ? JS_Eval(ctx, au_js_worker_prelude_src, au_js_worker_prelude_len, "aura:worker-prelude",
                                      JS_EVAL_TYPE_GLOBAL)
                            : JS_Eval(ctx, au_js_prelude_src, au_js_prelude_len, "aura:prelude", JS_EVAL_TYPE_GLOBAL);
#else
    /* The bytecode is the compiled script; running it gives the prelude
     * function, as evaluating the source does. JS_EvalFunction frees the
     * script. */
    JSValue fn = js->worker ? JS_ReadObject(ctx, au_js_worker_prelude_bc, au_js_worker_prelude_bc_len,
                                            JS_READ_OBJ_BYTECODE)
                            : JS_ReadObject(ctx, au_js_prelude_bc, au_js_prelude_bc_len, JS_READ_OBJ_BYTECODE);
    if (!JS_IsException(fn))
        fn = JS_EvalFunction(ctx, fn);
#endif
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

static AuJs *js_create(AuJs *parent, AuDocument *doc, const char *url, const AuJsSettings *settings,
                       const AuJsHooks *hooks);

AuJs *au_js_new(AuDocument *doc, const char *url, const AuJsSettings *settings, const AuJsHooks *hooks)
{
    return js_create(NULL, doc, url, settings, hooks);
}

AuJs *au_js_new_child(AuJs *parent, AuDocument *doc, const char *url, const AuJsSettings *settings,
                      const AuJsHooks *hooks)
{
    if (!parent || parent->root->closing)
        return NULL;
    return js_create(parent->root, doc, url, settings, hooks);
}

static AuJs *js_create(AuJs *root, AuDocument *doc, const char *url, const AuJsSettings *settings,
                       const AuJsHooks *hooks)
{
    AuJs *js = au_calloc(sizeof *js);
    js->root = root ? root : js;
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
    slot_new(js);
    uint32_t slot_before = js->root->cur_slot;
    js->root->cur_slot = js->slot;
    const char *origin_set = js->settings.origin;
    js->settings.origin = NULL; /* the caller's string: copied below */
    js->referrer = js->settings.referrer ? au_strndup(js->settings.referrer, strlen(js->settings.referrer)) : NULL;
    js->settings.referrer = NULL;
    if (root) {
        /* A frame: a new context in the page's runtime. */
        js->rt = root->rt;
    } else {
        js->rt = JS_NewRuntime2(&qa_funcs, js);
        if (!js->rt) {
            au_free(js->slots);
            au_free(js);
            return NULL;
        }
        JS_SetRuntimeOpaque(js->rt, js);
        /* The memory limit is the allocator's (over_limit), not
         * JS_SetMemoryLimit's, so the out-of-memory error can be built. */
        JS_SetMaxStackSize(js->rt, js->settings.stack_limit);
        JS_SetInterruptHandler(js->rt, interrupt_cb, js);
        JS_SetHostPromiseRejectionTracker(js->rt, rejection_tracker, js);
        JS_SetSourcePackHooks(js->rt, source_pack, source_unpack, NULL);
        if (js->settings.lazy_functions)
            JS_SetLazyFunctions(js->rt, js->settings.lazy_functions);
        if (js->settings.hints)
            JS_SetCompileHints(js->rt, hints_for, hint_compiled, js, JS_HINT_SCRIPT);
    }
    JSContext *ctx = JS_NewContextRaw(js->rt);
    if (!ctx || JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicDate(ctx) || JS_AddIntrinsicEval(ctx) ||
        JS_AddIntrinsicRegExp(ctx) || JS_AddIntrinsicJSON(ctx) || JS_AddIntrinsicProxy(ctx) ||
        JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicTypedArrays(ctx) || JS_AddIntrinsicPromise(ctx) ||
        JS_AddIntrinsicWeakRef(ctx) || JS_AddIntrinsicDOMException(ctx) || JS_AddIntrinsicAToB(ctx)) {
        /* DOMException by name: JS_AddIntrinsicAToB adds it only to the
         * first context of a runtime. */
        if (ctx)
            JS_FreeContext(ctx);
        if (!root)
            JS_FreeRuntime(js->rt);
        if (root)
            root->cur_slot = slot_before;
        else
            au_free(js->slots);
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
    if (origin_set)
        snprintf(origin, sizeof origin, "%s", origin_set);
    else
        js_origin_of(js->url, origin, sizeof origin);
    js->origin = au_strndup(origin, strlen(origin));
    js->opaque_origin = js->settings.sandbox_origin || !strcmp(origin, "null");
    if (js->settings.sandbox_origin) {
        au_free(js->origin);
        js->origin = au_strndup("null", 4);
    }
    if (root) {
        /* after the root, so the root stays first */
        js->group_next = root->group_next;
        root->group_next = js;
    }
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
    js_natives_ranges_init(js, N);
    js_loop_init(js, N);
    js_fetch_init(js, N);
    js_module_init(js);
    js_module_natives(js, N);
    js_frames_natives(js, N);
    js_canvas_init(js, N);
    js_worker_natives(js, N);
    bool ok = load_prelude(js, N);
    JS_FreeValue(ctx, N);
    js_leave(js);
    if (!ok) {
        au_js_free(js);
        return NULL;
    }
    return js;
}

/* A worker's runtime: no document, the natives that need none, and the
 * worker prelude. Made and freed on the worker's stack (worker.c). */
AuJs *js_create_worker(AuJs *owner, struct JsWorker *wk, const char *url, bool opaque_origin)
{
    AuJs *root = owner->root;
    AuJs *js = au_calloc(sizeof *js);
    js->root = js;
    js->worker = true;
    js->wk = wk;
    slot_new(js);
    js->cur_slot = js->slot;
    js->settings = root->settings;
    js->settings.origin = NULL;
    js->settings.memory_limit = root->settings.worker_memory_limit ? root->settings.worker_memory_limit : AU_JS_WORKER_MEMORY;
    size_t stack = root->settings.worker_stack ? root->settings.worker_stack : AU_JS_WORKER_STACK;
    js->settings.stack_limit = owner->hooks.coro ? stack - stack / 5 : root->settings.stack_limit;
    js->hooks.ctx = owner->hooks.ctx;
    js->hooks.now_ms = owner->hooks.now_ms;
    js->hooks.console = owner->hooks.console;
    js->hooks.out_of_memory = owner->hooks.out_of_memory;
    js->hooks.random = owner->hooks.random;
    js->hooks.fetcher = owner->hooks.fetcher;
    js->hooks.coro = owner->hooks.coro;
    if (!owner->hooks.coro)
        js->hooks.long_script = owner->hooks.long_script;
    js->P = JS_UNDEFINED;
    js->rt = JS_NewRuntime2(&qa_funcs, js);
    if (!js->rt) {
        au_free(js->slots);
        au_free(js);
        return NULL;
    }
    JS_SetRuntimeOpaque(js->rt, js);
    JS_SetMaxStackSize(js->rt, js->settings.stack_limit);
    JS_SetInterruptHandler(js->rt, interrupt_cb, js);
    JS_SetHostPromiseRejectionTracker(js->rt, rejection_tracker, js);
    JS_SetSourcePackHooks(js->rt, source_pack, source_unpack, NULL);
    if (js->settings.lazy_functions)
        JS_SetLazyFunctions(js->rt, js->settings.lazy_functions);
    JSContext *ctx = JS_NewContextRaw(js->rt);
    if (!ctx || JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicDate(ctx) || JS_AddIntrinsicEval(ctx) ||
        JS_AddIntrinsicRegExp(ctx) || JS_AddIntrinsicJSON(ctx) || JS_AddIntrinsicProxy(ctx) ||
        JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicTypedArrays(ctx) || JS_AddIntrinsicPromise(ctx) ||
        JS_AddIntrinsicWeakRef(ctx) || JS_AddIntrinsicDOMException(ctx) || JS_AddIntrinsicAToB(ctx)) {
        if (ctx)
            JS_FreeContext(ctx);
        JS_FreeRuntime(js->rt);
        au_free(js->slots);
        au_free(js);
        return NULL;
    }
    js->ctx = ctx;
    JS_SetContextOpaque(ctx, js);
    js->url = au_strndup(url, strlen(url));
    js->base = au_strndup(url, strlen(url));
    js->opaque_origin = opaque_origin;
    js->origin = opaque_origin ? au_strndup("null", 4) : au_strndup(owner->origin, strlen(owner->origin));
    JSValue g = JS_GetGlobalObject(ctx);
    js->global = g;
    JS_FreeValue(ctx, g);
    js_enter(js);
    JSValue N = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, N, base_natives, JS_COUNTOF(base_natives));
    js_natives_shared_init(js, N);
    js_loop_init(js, N);
    js_fetch_init(js, N);
    js_module_init(js);
    js_module_natives(js, N);
    js_worker_natives(js, N);
    bool ok = load_prelude(js, N);
    JS_FreeValue(ctx, N);
    js_leave(js);
    if (!ok) {
        au_js_free(js);
        return NULL;
    }
    return js;
}

/* A script at least this long is "large": the collector runs before it is
 * compiled when the heap has grown, because a compile makes no objects
 * and so never triggers a collection itself, while its working memory is
 * several times the script's length; and its compiled form is kept for
 * the page's other documents. */
#define JS_LARGE_SCRIPT (64u << 10)

/* ---- Compiled scripts ----
 * Compiling a large script costs far more than the result holds: an
 * 850 KB library took 46 to 144 ms on the host and 4 to 7 MB of working
 * memory, where reading its bytecode took 3 ms and no working memory. A
 * page whose frames (and worker) load the script the page itself loaded
 * therefore compiles it once: the runtime keeps the compiled script of
 * each of the last few large classic scripts, and a later request for
 * the same URL and bytes in another context writes that script as
 * bytecode (JS_WriteObject, about 1 MB for a moment) and reads it into
 * the context that asked. Functions keep their source text: the bytecode
 * names positions in the script's text, which the contexts share
 * (QuickJS patches 0008 and 0009). A script that is mostly data, whose
 * functions have texts of their own, cannot be written that way and is
 * compiled each time. A worker has its own runtime: the page writes the
 * bytecode when it hands the worker its script (js_code_for_worker). */
/* Four bytes a step in two lanes, so the multiplies do not wait on each
 * other: a page that gives eval 1.3 MB of text 93 times (Cloudflare's
 * Turnstile) had 121 MB hashed a byte at a time, about 5 cycles a byte
 * on PowerPC. The value names a script only beside its length, within
 * one run and in the hints file. */
static uint32_t code_hash(const char *src, size_t len)
{
    const uint8_t *p = (const uint8_t *)src;
    uint32_t a = 2166136261u, b = 0x9E3779B9u ^ (uint32_t)len;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint32_t w0 = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 | (uint32_t)p[i + 2] << 8 | p[i + 3];
        uint32_t w1 = (uint32_t)p[i + 4] << 24 | (uint32_t)p[i + 5] << 16 | (uint32_t)p[i + 6] << 8 | p[i + 7];
        a ^= w0;
        b ^= w1;
        a = (a << 15 | a >> 17) * 0x9E3779B1u;
        b = (b << 13 | b >> 19) * 0x85EBCA77u;
    }
    for (; i < len; i++)
        a = (a ^ p[i]) * 16777619u;
    a ^= (b << 16 | b >> 16);
    a ^= a >> 15;
    a *= 0x2C1B3C6Du;
    a ^= a >> 12;
    return a;
}

/* ---- Script hints ----
 * A function nested in a script is compiled at its first call (QuickJS
 * patch 0021), from the script's text, which parses it a second time.
 * For a large script the runtime tells the hint store
 * (settings.hints) which functions were compiled that way, by the
 * script's length and hash and the function's offset in its text; the
 * next time the same script is compiled, in this page or a later one,
 * those functions are compiled with it (patch 0022), so they are parsed
 * once. QuickJS asks for the hints of every script, module and text
 * given to eval or Function of JS_HINT_SCRIPT bytes or more; the hash
 * names the script to QuickJS too, which reports a function compiled
 * at its first call by it. */
static uint32_t code_hash(const char *src, size_t len);

static uint32_t hints_for(JSContext *ctx, void *opaque, const char *text, size_t len, const uint32_t **ofs,
                          size_t *count)
{
    (void)ctx;
    AuJs *r = opaque;
    const AuJsHintStore *s = r->settings.hints;
    if (len > UINT32_MAX)
        return 0;
    /* eval_classic hashed it already for the compiled-script cache */
    uint32_t hash = text == r->hint_text && len == r->hint_text_len ? r->hint_text_hash : code_hash(text, len);
    if (!hash)
        hash = 1; /* 0 is no id */
    *ofs = s->get(s->ctx, (uint32_t)len, hash, count);
    return hash;
}

static void hint_compiled(JSContext *ctx, void *opaque, uint32_t id, uint32_t len, uint32_t ofs)
{
    (void)ctx;
    AuJs *r = opaque;
    const AuJsHintStore *s = r->settings.hints;
    s->add(s->ctx, len, id, ofs);
}

static struct JsCode *code_find(AuJs *r, const char *url, size_t len, uint32_t hash, int line)
{
    for (size_t i = 0; i < JS_COUNTOF(r->code); i++) {
        struct JsCode *c = &r->code[i];
        if (c->url && c->len == len && c->hash == hash && c->line == line && !strcmp(c->url, url))
            return c;
    }
    return NULL;
}

static void code_drop(struct JsCode *c)
{
    if (!c->url)
        return;
    JS_FreeValue(c->owner->ctx, c->fn);
    au_free(c->url);
    memset(c, 0, sizeof *c);
}

/* owner's context is going (NULL: the runtime is). */
static void code_drop_owner(AuJs *r, const AuJs *owner)
{
    for (size_t i = 0; i < JS_COUNTOF(r->code); i++)
        if (!owner || r->code[i].owner == owner)
            code_drop(&r->code[i]);
}

static void code_add(AuJs *js, const char *url, size_t len, uint32_t hash, int line, JSValueConst fn)
{
    AuJs *r = js->root;
    struct JsCode *c = &r->code[0];
    for (size_t i = 0; i < JS_COUNTOF(r->code); i++) {
        if (!r->code[i].url) {
            c = &r->code[i];
            break;
        }
        if (r->code[i].used < c->used)
            c = &r->code[i];
    }
    code_drop(c);
    c->url = au_strndup(url, strlen(url));
    c->len = len;
    c->hash = hash;
    c->line = line;
    c->fn = JS_DupValue(js->ctx, fn);
    c->owner = js;
    c->used = ++r->code_clock;
    c->when = js_now(r);
}

/* A kept script holds the code that ran once while the script loaded (an
 * outer function's body), which nothing else would: 250 KB for an 860 KB
 * bundle. Frames and workers that share a script start within seconds of
 * the page loading it, so an entry nothing has read for 20 seconds goes;
 * a later frame compiles the script again. */
void js_code_expire(AuJs *js)
{
    AuJs *r = js->root;
    if (r->depth > 0)
        return;
    /* A heap past three quarters of its limit keeps nothing that only
     * saves a compile: YouTube's 9.8 MB bundle left its outer function
     * (1.2 MB) in the kept script while the page, never idle for 20
     * seconds, ran into its limit. */
    bool tight = r->live_bytes > r->settings.memory_limit / 4 * 3;
    double now = -1;
    for (size_t i = 0; i < JS_COUNTOF(r->code); i++) {
        if (!r->code[i].url)
            continue;
        if (now < 0)
            now = js_now(r);
        if (tight || now - r->code[i].when >= 20000)
            code_drop(&r->code[i]);
    }
    /* Source text not read for one pass is packed on the next, so a
     * script's text is deflated 5 to 10 seconds after it last ran
     * through toString (or was compiled). */
    if (now < 0)
        now = js_now(r);
    if (now >= r->next_pack) {
        r->next_pack = now + JS_PACK_MS;
        JS_PackSourceTexts(r->rt, 4096);
    }
}

/* The script c as bytecode in a QuickJS block of c's context, or NULL. */
static uint8_t *code_write(struct JsCode *c, size_t *blen)
{
    JSContext *octx = c->owner->ctx;
    uint8_t *blob = JS_WriteObject(octx, blen, c->fn, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_SOURCE_REF);
    if (!blob) {
        JSValue e = JS_GetException(octx);
        JS_FreeValue(octx, e);
    }
    return blob;
}

uint8_t *js_code_for_worker(AuJs *js, const char *url, const char *src, size_t len, size_t *code_len)
{
    AuJs *r = js->root;
    if (len < JS_LARGE_SCRIPT || r->worker)
        return NULL;
    struct JsCode *c = code_find(r, url, len, code_hash(src, len), 1);
    if (!c)
        return NULL;
    size_t blen = 0;
    uint8_t *blob = code_write(c, &blen);
    if (!blob)
        return NULL;
    uint8_t *out = au_try_alloc(blen ? blen : 1);
    if (out) {
        memcpy(out, blob, blen);
        *code_len = blen;
        c->used = ++r->code_clock;
        c->when = js_now(r);
    }
    js_free(c->owner->ctx, blob);
    return out;
}

void au_js_free(AuJs *js)
{
    if (!js)
        return;
    if (!js->worker)
        js_workers_free(js); /* while the fetcher and the hooks are still there */
    js->closing = true;
    JSContext *ctx = js->ctx;
    js_loop_free(js);
    js_fetch_free(js);
    js_module_free(js);
    js_mutations_free(js);
    js_ranges_free(js);
    js_storage_free(js);
    au_buf_free(&js->open_buf);
    if (js->root != js) {
        /* A frame's context. The runtime, the wrappers and whatever script
         * in other documents still holds of this one live on: objects of
         * this context stay valid, and its natives then act for the root
         * (the context's opaque pointer must never dangle). The document
         * stays with the root while a wrapper still names one of its
         * nodes. The embedder calls this only while no script runs. */
        AuJs *root = js->root;
        code_drop_owner(root, js);
        JS_FreeValue(ctx, js->P);
        js->P = JS_UNDEFINED;
        js_canvas_free(js);
        js_wrap_free_child(js);
        JS_SetContextOpaque(ctx, root);
        JS_FreeContext(ctx);
        if (root->depth == 0) {
            JS_RunGC(root->rt);
            js_drain_pending(root);
        }
        js_hooks_remove(js);
        if (js->current_script)
            au_node_unref(&js->current_script->node);
        for (uint32_t i = 0; i <= js->ndocs; i++) {
            AuDocument *d = i < js->ndocs ? js->docs[i] : js->doc;
            bool held = false;
            for (AuJsWrap *w = root->wraps; w && !held; w = w->next)
                held = w->node->doc == d;
            if (held && !root->closing)
                js_keep_doc(root, d);
            au_node_unref(&d->node);
        }
        au_free(js->docs);
        for (AuJs **pp = &root->group_next; *pp; pp = &(*pp)->group_next)
            if (*pp == js) {
                *pp = js->group_next;
                break;
            }
        for (size_t i = 0; i < JS_COUNTOF(root->entered); i++)
            if (root->entered[i] == js)
                root->entered[i] = root;
        free_muted(js);
        au_free(js->url);
        au_free(js->base);
        au_free(js->origin);
        au_free(js->referrer);
        au_free(js);
        return;
    }
    /* The root goes last: contexts of frames that are still there (an
     * embedder that broke the order) are cut loose first. */
    while (js->group_next) {
        AuJs *c = js->group_next;
        c->hooks.ctx = NULL;
        memset(&c->hooks, 0, sizeof c->hooks);
        au_js_free(c);
    }
    for (uint32_t i = 0; i < js->nrejections; i++) {
        JS_FreeValue(ctx, js->rejections[i].promise);
        JS_FreeValue(ctx, js->rejections[i].reason);
    }
    js->nrejections = 0;
    au_free(js->rejections);
    code_drop_owner(js, NULL);
    js_selectors_free(js);
    JS_FreeValue(ctx, js->P);
    js->P = JS_UNDEFINED;
    js_canvas_free(js);
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
    if (js->doc)
        js_hooks_remove(js);
    if (js->current_script)
        au_node_unref(&js->current_script->node);
    for (uint32_t i = 0; i < js->ndocs; i++)
        au_node_unref(&js->docs[i]->node);
    au_free(js->docs);
    if (js->doc)
        au_node_unref(&js->doc->node);
    free_muted(js);
    au_free(js->url);
    au_free(js->base);
    au_free(js->origin);
    au_free(js->referrer);
    au_free(js->slots);
    au_free(js);
}

void au_js_set_parser(AuJs *js, AuHtmlParser *p)
{
    js->parser = p;
}

void au_js_set_url(AuJs *js, const char *url)
{
    /* Without a <base> the base URL is the document's URL, and follows it
     * (pushState). */
    if (js->base && js->url && !strcmp(js->base, js->url)) {
        au_free(js->base);
        js->base = au_strndup(url, strlen(url));
    }
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

JSValue js_eval_classic(AuJs *js, const char *src, size_t len, const char *url, int line)
{
    return eval_classic(js, src, len, url, line, NULL);
}

/* owned: src is the caller's au_alloc'd block, freed once compiled. */
static JSValue eval_classic(AuJs *js, const char *src, size_t len, const char *url, int line, char *owned)
{
    AuJs *r = js->root;
    JSContext *ctx = js->ctx;
    if (len >= JS_LARGE_SCRIPT && r->live_bytes > r->gc_base + (512u << 10)) {
        JS_RunGC(r->rt);
        r->gc_base = r->live_bytes;
    }
    char *prev = r->compiling;
    bool oom_before = r->oom;
    const char *name = url ? url : js->url;
    if (line < 1)
        line = 1;
    r->compiling = (char *)name;
    JSValue fn = JS_UNINITIALIZED;
    bool large = len >= JS_LARGE_SCRIPT;
    bool no_text = len >= JS_NO_TEXT_SCRIPT;
    uint32_t hash = large ? code_hash(src, len) : 0;
    if (large && r->worker) {
        /* the page compiled this script and sent its bytecode along */
        size_t clen = 0;
        uint8_t *code = js_worker_take_code(js, &clen);
        if (code) {
            fn = no_text ? JS_ReadObject(ctx, code, clen, JS_READ_OBJ_BYTECODE)
                         : JS_ReadObjectSource(ctx, code, clen, JS_READ_OBJ_BYTECODE, src, len);
            au_free(code);
        }
    } else if (large) {
        struct JsCode *c = code_find(r, name, len, hash, line);
        size_t blen = 0;
        uint8_t *blob = c ? code_write(c, &blen) : NULL;
        if (blob) {
            /* without texts the bytecode names no positions in the
             * script, and reading it needs no copy of the script */
            fn = no_text ? JS_ReadObject(ctx, blob, blen, JS_READ_OBJ_BYTECODE)
                         : JS_ReadObjectSource(ctx, blob, blen, JS_READ_OBJ_BYTECODE, src, len);
            js_free(c->owner->ctx, blob);
            c->used = ++r->code_clock;
            c->when = js_now(r);
        }
    }
    if (JS_IsException(fn)) {
        /* the bytecode could not be read (memory): compile instead */
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
        fn = JS_UNINITIALIZED;
    }
    if (JS_IsUninitialized(fn)) {
        JSEvalOptions o = {JS_EVAL_OPTIONS_VERSION,
                           JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY | (no_text ? JS_EVAL_FLAG_NO_SOURCE : 0),
                           name, line};
        r->hint_text = large ? src : NULL; /* hashed: hints_for */
        r->hint_text_len = len;
        r->hint_text_hash = hash;
        fn = JS_Eval2(ctx, src, len, &o);
        r->hint_text = NULL;
        if (large && !r->worker && !JS_IsException(fn))
            code_add(js, name, len, hash, line, fn);
    }
    r->compiling = prev;
    au_free(owned);
    if (JS_IsException(fn)) {
        /* An allocation refused inside the parser can surface as a
         * syntax error at the place it happened to be: report what it
         * was. */
        if (r->oom && !oom_before) {
            JSValue e = JS_GetException(ctx);
            if (!JS_IsUncatchableError(e)) {
                JS_FreeValue(ctx, e);
                return JS_ThrowOutOfMemory(ctx);
            }
            JS_Throw(ctx, e);
        }
        return fn;
    }
    return JS_EvalFunction(ctx, fn);
}

bool au_js_run_classic(AuJs *js, AuElement *el, const char *src, size_t len, const char *url, int line)
{
    /* QuickJS wants a NUL-terminated source; src may not be. */
    return au_js_run_classic_take(js, el, au_strndup(src, len), len, url, line);
}

bool au_js_run_classic_take(AuJs *js, AuElement *el, char *src, size_t len, const char *url, int line)
{
    if (js->closing || js->settings.scripts_off) {
        au_free(src);
        return false;
    }
    JSContext *ctx = js->ctx;
    AuElement *prev = js->current_script;
    if (el)
        au_node_ref(&el->node);
    js->current_script = el;
    js_enter(js);
    JSValue r = eval_classic(js, src, len, url, line, src);
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

void au_js_memory_report(AuJs *js, AuBuf *out)
{
    JSMemoryUsage m;
    JS_ComputeMemoryUsage(js->rt, &m);
    au_buf_printf(out, "script heap %lu KB in %lu blocks (limit %lu KB), %lu wrappers\n",
                  (unsigned long)(js->live_bytes / 1024), (unsigned long)m.malloc_count,
                  (unsigned long)(js->settings.memory_limit / 1024), (unsigned long)js->nwraps);
    /* The engine takes blocks of up to 512 bytes from 4 KB arenas of its
     * own: "in use" is what its blocks hold, the rest of the heap is free
     * blocks inside arenas that still hold a live one. */
    au_buf_printf(out, "  in use %lu KB (accounted for below: %lu KB), free inside the engine's arenas %lu KB, peak %lu KB\n",
                  (unsigned long)(m.malloc_size / 1024), (unsigned long)(m.memory_used_size / 1024),
                  (unsigned long)((js->live_bytes > (size_t)m.malloc_size ? js->live_bytes - (size_t)m.malloc_size : 0) / 1024),
                  (unsigned long)(js->peak_bytes / 1024));
    au_buf_printf(out, "  atoms       %7lu  %7lu KB\n", (unsigned long)m.atom_count, (unsigned long)(m.atom_size / 1024));
    au_buf_printf(out, "  strings     %7lu  %7lu KB\n", (unsigned long)m.str_count, (unsigned long)(m.str_size / 1024));
    au_buf_printf(out, "  objects     %7lu  %7lu KB\n", (unsigned long)m.obj_count, (unsigned long)(m.obj_size / 1024));
    au_buf_printf(out, "  properties  %7lu  %7lu KB\n", (unsigned long)m.prop_count, (unsigned long)(m.prop_size / 1024));
    au_buf_printf(out, "  shapes      %7lu  %7lu KB\n", (unsigned long)m.shape_count, (unsigned long)(m.shape_size / 1024));
    au_buf_printf(out, "  functions   %7lu  %7lu KB, of which bytecode %lu KB\n", (unsigned long)m.js_func_count,
                  (unsigned long)(m.js_func_size / 1024), (unsigned long)(m.js_func_code_size / 1024));
    au_buf_printf(out, "  source text %7lu  %7lu KB (counted in functions)\n", (unsigned long)m.js_func_source_count,
                  (unsigned long)(m.js_func_source_size / 1024));
    au_buf_printf(out, "  line tables %7lu  %7lu KB\n", (unsigned long)m.js_func_pc2line_count,
                  (unsigned long)(m.js_func_pc2line_size / 1024));
    au_buf_printf(out, "  C functions %7lu, arrays %lu (fast %lu, %lu elements, %lu KB)\n", (unsigned long)m.c_func_count,
                  (unsigned long)m.array_count, (unsigned long)m.fast_array_count, (unsigned long)m.fast_array_elements,
                  (unsigned long)(m.fast_array_elements * sizeof(JSValue) / 1024));
    au_buf_printf(out, "  binary data %7lu  %7lu KB\n", (unsigned long)m.binary_object_count,
                  (unsigned long)(m.binary_object_size / 1024));
}

bool au_js_eval(AuJs *js, const char *src, size_t len, AuBuf *out)
{
    JSContext *ctx = js->ctx;
    if (len == 11 && !memcmp(src, "aura:memory", 11) && out) {
        au_js_memory_report(js, out); /* for aura-host chrome --do js:aura:memory */
        return true;
    }
    if (len == 7 && !memcmp(src, "aura:gc", 7)) {
        au_js_gc(js); /* --do js:aura:gc */
        return true;
    }
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
    if (strncmp(url, "javascript:", 11) != 0 || js->settings.scripts_off)
        return;
    JSValue s = js_str(js->ctx, url + 11, strlen(url + 11));
    JSValue r = js_call_prelude(js, "runJavascriptUrl", 1, &s);
    JS_FreeValue(js->ctx, r);
    JS_FreeValue(js->ctx, s);
    au_js_checkpoint(js);
}

void au_js_gc(AuJs *js)
{
    if (js->root->depth > 0)
        return; /* script of the page is running: not now */
    js_drain_pending(js);
    JS_RunGC(js->rt);
    js_drain_pending(js);
    js->gc_base = js->live_bytes;
}

void au_js_memory_summary(const AuJs *js, AuBuf *out)
{
    const AuJs *r = js->root;
    au_buf_printf(out, "  script runtime of %s: %lu KB, peak %lu KB, limit %lu KB\n", r->url,
                  (unsigned long)(r->live_bytes / 1024), (unsigned long)(r->peak_bytes / 1024),
                  (unsigned long)(r->settings.memory_limit / 1024));
    size_t named = 0;
    for (const AuJs *g = r; g; g = g->group_next) {
        if (g->slot >= r->nslots)
            continue;
        const JsMemSlot *m = &r->slots[g->slot];
        named += m->live;
        au_buf_printf(out, "    %s %s: %lu KB, peak %lu KB\n", g == r ? "document" : "frame", g->url,
                      (unsigned long)(m->live / 1024), (unsigned long)(m->peak / 1024));
    }
    if (r->live_bytes > named)
        au_buf_printf(out, "    documents that have gone, and the runtime itself: %lu KB\n",
                      (unsigned long)((r->live_bytes - named) / 1024));
    for (const AuJs *g = r; g; g = g->group_next)
        js_workers_summary(g, out);
}

void au_js_stats(const AuJs *js, AuJsStats *s)
{
    /* The runtime's memory is counted once, for the root; a frame's
     * context reports none of its own. */
    s->memory_used = 0;
    if (js->root == js)
        s->memory_used = js->live_bytes; /* what the allocator gave the engine, its arenas' free blocks included */
    s->memory_used += js_workers_memory(js);
    s->memory_limit = js->root->settings.memory_limit;
    s->memory_peak = js->root->peak_bytes;
    s->worker_peak = 0;
    s->runtime_used = js->root->live_bytes;
    s->worker_used = 0;
    s->workers = 0;
    for (const AuJs *g = js->root; g; g = g->group_next) {
        size_t p = js_workers_peak(g);
        if (p > s->worker_peak)
            s->worker_peak = p;
        s->worker_used += js_workers_memory(g);
        s->workers += au_js_worker_count(g);
    }
    s->worker_limit = js->root->settings.worker_memory_limit ? js->root->settings.worker_memory_limit : AU_JS_WORKER_MEMORY;
    s->wrappers = js->root->nwraps;
    s->errors = js->errors;
    s->interrupted = js->interrupts;
}
