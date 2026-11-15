/* The event loop: timers, tasks and animation frame callbacks. The
 * embedder drives it (au_js_run_tasks, au_js_run_frame) from its own loop
 * and clock (hooks.now_ms). */
#include "js_int.h"

#include <string.h>

#define MAX_TIMERS 10000
#define MAX_TASKS 100000

static void free_timer(JSContext *ctx, JsTimer *t)
{
    JS_FreeValue(ctx, t->fn);
    for (int i = 0; i < t->nargs; i++)
        JS_FreeValue(ctx, t->args[i]);
    au_free(t->args);
}

/* setTimeout(fn, ms, ...args) and setInterval. magic 1 for interval. */
static JSValue n_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 1)
        return JS_NewInt32(ctx, 0);
    if (js->ntimers >= MAX_TIMERS)
        return JS_ThrowRangeError(ctx, "too many timers");
    double ms = 0;
    if (argc > 1 && JS_ToFloat64(ctx, &ms, argv[1]))
        return JS_EXCEPTION;
    if (!(ms >= 0) || ms > 2147483647.0)
        ms = 0;
    JSValue fn;
    if (JS_IsFunction(ctx, argv[0])) {
        fn = JS_DupValue(ctx, argv[0]);
    } else {
        fn = JS_ToString(ctx, argv[0]); /* a string to evaluate */
        if (JS_IsException(fn))
            return fn;
    }
    /* Timers nested more than five deep are clamped to 4 ms. */
    uint32_t nesting = js->timer_nesting + 1;
    if (nesting > 5 && ms < 4)
        ms = 4;
    if (js->ntimers == js->timers_cap) {
        js->timers_cap = js->timers_cap ? js->timers_cap * 2 : 16;
        js->timers = au_realloc(js->timers, js->timers_cap * sizeof *js->timers);
    }
    JsTimer *t = &js->timers[js->ntimers++];
    memset(t, 0, sizeof *t);
    t->id = ++js->next_timer_id;
    if (t->id == 0)
        t->id = ++js->next_timer_id;
    t->due = js_now(js) + ms;
    t->interval = magic ? (ms < 1 ? 1 : ms) : -1;
    t->nesting = nesting;
    t->fn = fn;
    if (argc > 2) {
        t->nargs = argc - 2;
        t->args = au_alloc((size_t)t->nargs * sizeof *t->args);
        for (int i = 0; i < t->nargs; i++)
            t->args[i] = JS_DupValue(ctx, argv[i + 2]);
    }
    return JS_NewUint32(ctx, t->id);
}

static JSValue n_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    if (argc < 1 || JS_ToUint32(ctx, &id, argv[0]) || !id)
        return JS_UNDEFINED;
    for (uint32_t i = 0; i < js->ntimers; i++)
        if (js->timers[i].id == id) {
            JsTimer t = js->timers[i];
            memmove(&js->timers[i], &js->timers[i + 1], (js->ntimers - i - 1) * sizeof t);
            js->ntimers--;
            free_timer(ctx, &t);
            break;
        }
    return JS_UNDEFINED;
}

void js_queue_task(AuJs *js, JSValueConst fn, JSValueConst arg)
{
    if (js->closing || js->ntasks - js->task_head >= MAX_TASKS)
        return;
    if (js->task_head && js->task_head == js->ntasks)
        js->task_head = js->ntasks = 0;
    if (js->ntasks == js->tasks_cap) {
        if (js->task_head) {
            memmove(js->tasks, js->tasks + js->task_head, (js->ntasks - js->task_head) * sizeof *js->tasks);
            js->ntasks -= js->task_head;
            js->task_head = 0;
        }
        if (js->ntasks == js->tasks_cap) {
            js->tasks_cap = js->tasks_cap ? js->tasks_cap * 2 : 16;
            js->tasks = au_realloc(js->tasks, js->tasks_cap * sizeof *js->tasks);
        }
    }
    js->tasks[js->ntasks].fn = JS_DupValue(js->ctx, fn);
    js->tasks[js->ntasks].arg = JS_DupValue(js->ctx, arg);
    js->ntasks++;
}

/* queueTask(fn, arg): a task for the prelude (events fired
 * asynchronously, fetch callbacks, message events). */
static JSValue n_queue_task(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    js_queue_task(js, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED);
    return JS_UNDEFINED;
}

static JSValue n_raf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "requestAnimationFrame: not a function");
    if (js->nframes >= MAX_TIMERS)
        return JS_ThrowRangeError(ctx, "too many animation frame callbacks");
    if (js->nframes == js->frames_cap) {
        js->frames_cap = js->frames_cap ? js->frames_cap * 2 : 8;
        js->frames = au_realloc(js->frames, js->frames_cap * sizeof *js->frames);
    }
    JsFrameCb *f = &js->frames[js->nframes++];
    f->id = ++js->next_frame_id;
    f->fn = JS_DupValue(ctx, argv[0]);
    return JS_NewUint32(ctx, f->id);
}

static JSValue n_caf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    if (argc < 1 || JS_ToUint32(ctx, &id, argv[0]))
        return JS_UNDEFINED;
    for (uint32_t i = 0; i < js->nframes; i++)
        if (js->frames[i].id == id) {
            JS_FreeValue(ctx, js->frames[i].fn);
            memmove(&js->frames[i], &js->frames[i + 1], (js->nframes - i - 1) * sizeof *js->frames);
            js->nframes--;
            break;
        }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry loop_natives[] = {
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, n_set_timer, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, n_set_timer, 1),
    JS_CFUNC_DEF("clearTimer", 1, n_clear_timer),
    JS_CFUNC_DEF("queueTask", 2, n_queue_task),
    JS_CFUNC_DEF("requestAnimationFrame", 1, n_raf),
    JS_CFUNC_DEF("cancelAnimationFrame", 1, n_caf),
};

void js_loop_init(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, loop_natives, JS_COUNTOF(loop_natives));
}

void js_loop_free(AuJs *js)
{
    JSContext *ctx = js->ctx;
    for (uint32_t i = 0; i < js->ntimers; i++)
        free_timer(ctx, &js->timers[i]);
    au_free(js->timers);
    js->timers = NULL;
    js->ntimers = 0;
    for (uint32_t i = js->task_head; i < js->ntasks; i++) {
        JS_FreeValue(ctx, js->tasks[i].fn);
        JS_FreeValue(ctx, js->tasks[i].arg);
    }
    au_free(js->tasks);
    js->tasks = NULL;
    js->ntasks = js->task_head = 0;
    for (uint32_t i = 0; i < js->nframes; i++)
        JS_FreeValue(ctx, js->frames[i].fn);
    au_free(js->frames);
    js->frames = NULL;
    js->nframes = 0;
}

/* Runs the earliest due timer. Returns false when none is due. */
static bool run_timer(AuJs *js, double now)
{
    int best = -1;
    for (uint32_t i = 0; i < js->ntimers; i++)
        if (js->timers[i].due <= now && (best < 0 || js->timers[i].due < js->timers[best].due))
            best = (int)i;
    if (best < 0)
        return false;
    JSContext *ctx = js->ctx;
    JsTimer t = js->timers[best];
    uint32_t id = t.id;
    if (t.interval >= 0) {
        /* Keep it; reschedule before running so clearInterval inside works. */
        js->timers[best].due = now + t.interval;
        t.fn = JS_DupValue(ctx, t.fn);
        JSValue *args = t.nargs ? au_alloc((size_t)t.nargs * sizeof *args) : NULL;
        for (int i = 0; i < t.nargs; i++)
            args[i] = JS_DupValue(ctx, t.args[i]);
        t.args = args;
    } else {
        memmove(&js->timers[best], &js->timers[best + 1], (js->ntimers - (uint32_t)best - 1) * sizeof t);
        js->ntimers--;
    }
    (void)id;
    uint32_t saved = js->timer_nesting;
    js->timer_nesting = t.nesting;
    js_enter(js);
    if (JS_IsString(t.fn)) {
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, t.fn);
        if (s) {
            JSValue r = JS_Eval(ctx, s, n, "timer", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(r))
                js_report_exception(js);
            JS_FreeValue(ctx, r);
            JS_FreeCString(ctx, s);
        }
    } else {
        JSValue r = JS_Call(ctx, t.fn, js->global, t.nargs, (JSValueConst *)t.args);
        if (JS_IsException(r))
            js_report_exception(js);
        JS_FreeValue(ctx, r);
    }
    js_leave(js);
    js->timer_nesting = saved;
    free_timer(ctx, &t);
    au_js_checkpoint(js);
    return true;
}

static bool run_task(AuJs *js)
{
    if (js->task_head >= js->ntasks)
        return false;
    JsTask t = js->tasks[js->task_head++];
    JSValue r = js_call(js, t.fn, JS_UNDEFINED, 1, (JSValueConst *)&t.arg);
    JS_FreeValue(js->ctx, r);
    JS_FreeValue(js->ctx, t.fn);
    JS_FreeValue(js->ctx, t.arg);
    au_js_checkpoint(js);
    return true;
}

bool au_js_run_tasks(AuJs *js, uint32_t budget_ms)
{
    if (js->closing)
        return false;
    double start = js_now(js);
    bool ran = false;
    au_js_checkpoint(js);
    for (;;) {
        double now = js_now(js);
        if (ran && now - start >= budget_ms)
            break;
        /* Tasks queued before now run first, then the earliest due timer. */
        if (run_task(js) || run_timer(js, now)) {
            ran = true;
            if (js->closing)
                break;
            continue;
        }
        break;
    }
    js_safe_point(js);
    /* Collect once script has been quiet for a second and the heap has
     * grown by an eighth (at least 512 KB) since the last collection.
     * QuickJS collects by itself only when the heap is half as large
     * again as after its last collection, so up to a third of a page's
     * heap could be garbage held by reference cycles while it sits idle
     * (0.6 MB of 11.3 MB on a Wikipedia article four seconds after its
     * load). */
    if (ran) {
        js->last_ran = js_now(js);
    } else if (!js->closing) {
        size_t step = js->gc_base / 8 > (512u << 10) ? js->gc_base / 8 : (512u << 10);
        if (js->live_bytes > js->gc_base + step && js_now(js) - js->last_ran >= 1000)
            au_js_gc(js);
    }
    return ran;
}

double au_js_next_timer(const AuJs *js)
{
    double best = -1;
    for (uint32_t i = 0; i < js->ntimers; i++)
        if (best < 0 || js->timers[i].due < best)
            best = js->timers[i].due;
    return best;
}

bool au_js_has_tasks(const AuJs *js)
{
    return js->task_head < js->ntasks;
}

bool au_js_wants_frame(const AuJs *js)
{
    return js->nframes > 0;
}

void au_js_run_frame(AuJs *js)
{
    if (!js->nframes || js->closing)
        return;
    JSContext *ctx = js->ctx;
    /* Callbacks requested during this frame wait for the next one. */
    JsFrameCb *cbs = js->frames;
    uint32_t n = js->nframes;
    js->frames = NULL;
    js->nframes = js->frames_cap = 0;
    JSValue ts = JS_NewFloat64(ctx, js_now(js));
    for (uint32_t i = 0; i < n; i++) {
        if (!js->closing) {
            JSValue r = js_call(js, cbs[i].fn, js->global, 1, (JSValueConst *)&ts);
            JS_FreeValue(ctx, r);
            au_js_checkpoint(js);
        }
        JS_FreeValue(ctx, cbs[i].fn);
    }
    au_free(cbs);
    js_safe_point(js);
}

bool au_js_idle(const AuJs *js)
{
    return !au_js_has_tasks(js) && js->ntimers == 0 && js->nframes == 0 && !js_fetch_pending(js) &&
           !js_modules_pending(js);
}
