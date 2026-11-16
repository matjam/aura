/* Dedicated workers.
 *
 * A worker is a QuickJS runtime of its own (js_create_worker: no document,
 * the worker prelude), so its heap, its memory limit and its collector are
 * separate from the page's, and it can run on a stack of its own. Aura
 * has one thread of control: workers run only inside au_js_run_workers,
 * which the embedder calls from its event loop, one time slice each in
 * turn.
 *
 * With coroutines (AuJsHooks.coro, core/coro.h) each worker has its own
 * stack. The whole life of its runtime happens there: creation, the
 * script, every task, and freeing. When a slice is used up the script
 * engine's interrupt check (js_worker_interrupt) yields to the event
 * loop from wherever the script is, and the next slice resumes it, so a
 * long computation (a proof of work) leaves the page running. A worker
 * that waits (importScripts, a synchronous XMLHttpRequest) parks its stack
 * the same way until the network answers. terminate() and the end of the
 * document set a flag and resume the worker, whose interrupt check then
 * unwinds the script.
 *
 * Without coroutines a worker runs on the caller's stack, one task per
 * slice; a long task then blocks the page until it returns (the ordinary
 * long-script limit applies), and nothing can wait for the network.
 *
 * The two sides share no script value. What crosses is C data in two
 * queues per worker, filled by one side and drained by the other on its
 * own stack: messages as bytes (the prelude turns a value into a plain
 * tree, scEncode, and JS_WriteObject writes it), port numbers, errors,
 * and what the embedder reports for a worker's requests
 * (js_worker_defer_fetch).
 *
 * MessagePorts that cross runtimes: a channel (JsPortChan, in the page's
 * root AuJs) names the AuJs that holds each end. A message posted to an
 * end held elsewhere is queued for its holder; while an end travels (it
 * was sent in a message and not yet received) its messages wait in the
 * channel. */
#include "js_int.h"

#include <string.h>

enum {
    WM_MESSAGE,    /* a message for the worker global, or from it for the Worker object */
    WM_PORT,       /* a message for the port end of channel id */
    WM_ERROR,      /* worker to page: an uncaught error (text, url, status the line) */
    WM_CLOSED,     /* worker to page: the worker has ended */
    WM_FETCH_HEAD, /* embedder to worker: what it reported for request id */
    WM_FETCH_DATA,
    WM_FETCH_DONE,
    WM_FETCH_FAIL
};

typedef struct JsWkMsg {
    struct JsWkMsg *next;
    int kind;
    uint32_t id;
    int status;
    char *text, *url;
    AuBuf data;    /* message bytes, body bytes, or packed headers (name NUL value NUL) */
    uint32_t nhead; /* fetch head: header pairs in data */
    uint32_t *cids; /* the channels of the ports sent with a message */
    uint32_t ncids;
} JsWkMsg;

typedef struct JsPortChan {
    uint32_t cid;
    AuJs *holder[2];       /* who holds each end; NULL while it travels */
    uint8_t travel;        /* 1 + the end that was let go last (0: none yet) */
    JsWkMsg *wait, *wait_tail; /* messages for the end that travels */
} JsPortChan;

typedef struct JsWorker {
    struct JsWorker *next;
    AuJs *owner; /* the document that made it */
    AuJs *js;    /* its runtime, while it lives */
    uint32_t id;
    char *url, *name, *source;
    size_t source_len;
    uint8_t *code; /* the script's bytecode, when the page had compiled the same script */
    size_t code_len;
    bool module, opaque;
    void *co; /* its coroutine, or NULL (it runs on the caller's stack) */
    JsWkMsg *in, *in_tail;   /* for the worker */
    JsWkMsg *out, *out_tail; /* for the document */
    bool started;   /* its script arrived */
    bool running;   /* its stack runs now */
    bool finished;  /* its runtime is gone */
    bool terminate; /* the page ended it */
    bool closing;   /* it called close() */
    bool busy;      /* it yielded with work left */
    bool loading;   /* a module worker whose script has not finished loading: messages wait */
    double slice_end;
    size_t stack;
} JsWorker;

static void msg_free(JsWkMsg *m)
{
    au_free(m->text);
    au_free(m->url);
    au_buf_free(&m->data);
    au_free(m->cids);
    au_free(m);
}

static void queue_free(JsWkMsg *m)
{
    while (m) {
        JsWkMsg *n = m->next;
        msg_free(m);
        m = n;
    }
}

static void push(JsWkMsg **head, JsWkMsg **tail, JsWkMsg *m)
{
    m->next = NULL;
    if (*tail)
        (*tail)->next = m;
    else
        *head = m;
    *tail = m;
}

static JsWkMsg *pop(JsWkMsg **head, JsWkMsg **tail)
{
    JsWkMsg *m = *head;
    if (m) {
        *head = m->next;
        if (!*head)
            *tail = NULL;
    }
    return m;
}

static JsWkMsg *msg_new(int kind)
{
    JsWkMsg *m = au_calloc(sizeof *m);
    m->kind = kind;
    au_buf_init(&m->data);
    return m;
}

/* The document of a worker's page, whatever js is. */
static AuJs *page_root(AuJs *js)
{
    return js->worker ? js->wk->owner->root : js->root;
}

/* ---- Messages as bytes ---- */

/* Writes tree (the prelude's encoding of a value) and the channel numbers
 * in cids (an array, or undefined) into a new message. NULL with an
 * exception pending when the tree cannot be written. */
static JsWkMsg *msg_from_js(JSContext *ctx, int kind, JSValueConst tree, JSValueConst cids)
{
    size_t len = 0;
    uint8_t *bytes = JS_WriteObject(ctx, &len, tree, 0);
    if (!bytes)
        return NULL;
    JsWkMsg *m = msg_new(kind);
    au_buf_append(&m->data, bytes, len);
    js_free(ctx, bytes);
    int64_t n = 0;
    if (JS_IsArray(cids) && !JS_GetLength(ctx, cids, &n) && n > 0 && n <= 256) {
        m->cids = au_alloc((size_t)n * sizeof *m->cids);
        for (int64_t i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, cids, (uint32_t)i);
            uint32_t c = 0;
            JS_ToUint32(ctx, &c, v);
            JS_FreeValue(ctx, v);
            m->cids[m->ncids++] = c;
        }
    }
    return m;
}

/* Hands a message to the prelude of js: P.workerDeliver(kind, id, tree,
 * cids), a task of its own. */
static void deliver_js(AuJs *js, int kind, uint32_t id, JsWkMsg *m)
{
    JSContext *ctx = js->ctx;
    if (js->closing || JS_IsUndefined(js->P))
        return;
    JSValue args[6];
    int n = 4;
    args[0] = JS_NewInt32(ctx, kind);
    args[1] = JS_NewUint32(ctx, id);
    if (m->kind == WM_ERROR) {
        args[2] = JS_NewString(ctx, m->text ? m->text : "");
        args[3] = JS_NewString(ctx, m->url ? m->url : "");
        args[4] = JS_NewInt32(ctx, m->status);
        n = 5;
    } else if (m->kind == WM_CLOSED) {
        args[2] = JS_UNDEFINED;
        args[3] = JS_UNDEFINED;
    } else {
        js_enter(js);
        args[2] = JS_ReadObject(ctx, (const uint8_t *)m->data.data, m->data.len, 0);
        js_leave(js);
        if (JS_IsException(args[2])) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            args[2] = JS_UNDEFINED;
            args[0] = JS_NewInt32(ctx, kind | 16); /* could not be read: messageerror */
        }
        args[3] = JS_NewArray(ctx);
        for (uint32_t i = 0; i < m->ncids; i++)
            JS_SetPropertyUint32(ctx, args[3], i, JS_NewUint32(ctx, m->cids[i]));
    }
    JSValue r = js_call_prelude(js, "workerDeliver", n, (JSValueConst *)args);
    JS_FreeValue(ctx, r);
    for (int i = 0; i < n; i++)
        JS_FreeValue(ctx, args[i]);
    au_js_checkpoint(js);
}

/* ---- Channels ---- */

static JsPortChan *chan_find(AuJs *root, uint32_t cid)
{
    for (uint32_t i = 0; i < root->nchans; i++)
        if (root->chans[i].cid == cid)
            return &root->chans[i];
    return NULL;
}

/* Queues m for holder (a document or a worker). */
static void hand_to(AuJs *holder, JsWkMsg *m)
{
    if (holder->worker) {
        push(&holder->wk->in, &holder->wk->in_tail, m);
        holder->wk->busy = true;
    } else {
        push(&holder->port_in, &holder->port_in_tail, m);
    }
}

/* A runtime is going: its ends close, and what waited for them goes. */
static void chans_drop_holder(AuJs *root, AuJs *js)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < root->nchans; i++) {
        JsPortChan *c = &root->chans[i];
        bool gone = false;
        for (int s = 0; s < 2; s++)
            if (c->holder[s] == js) {
                c->holder[s] = NULL;
                gone = true;
            }
        if (gone && !c->holder[0] && !c->holder[1]) {
            queue_free(c->wait);
            continue;
        }
        root->chans[k++] = *c;
    }
    root->nchans = k;
}

/* portNew(): a channel whose two ends nobody holds yet. */
static JSValue n_port_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *root = page_root(js_of_ctx(ctx));
    if (root->nchans >= 4096)
        return JS_ThrowRangeError(ctx, "too many message channels");
    root->chans = au_realloc(root->chans, (root->nchans + 1) * sizeof *root->chans);
    JsPortChan *c = &root->chans[root->nchans++];
    memset(c, 0, sizeof *c);
    c->cid = ++root->next_chan ? root->next_chan : ++root->next_chan;
    return JS_NewUint32(ctx, c->cid);
}

/* portHold(cid): this runtime holds an end of the channel from now on (an
 * end nobody holds), and gets what waited for it. portRelease(cid): it
 * lets go of its end, which travels with a message. */
static JSValue n_port_hold(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *root = page_root(js);
    uint32_t cid = 0;
    if (argc < 1 || JS_ToUint32(ctx, &cid, argv[0]))
        return JS_FALSE;
    JsPortChan *c = chan_find(root, cid);
    if (!c)
        return JS_FALSE;
    if (magic) { /* release */
        for (int s = 0; s < 2; s++)
            if (c->holder[s] == js) {
                c->holder[s] = NULL;
                c->travel = (uint8_t)(s + 1);
                return JS_TRUE;
            }
        return JS_FALSE;
    }
    /* The end that was let go is the one taken; a channel just made has
     * two free ends, taken in order. */
    int s = c->travel && !c->holder[c->travel - 1] ? c->travel - 1 : !c->holder[0] ? 0 : !c->holder[1] ? 1 : -1;
    if (s < 0)
        return JS_FALSE;
    c->holder[s] = js;
    /* Messages that waited for a travelling end are for this one, unless
     * the other end is the one still in transit. */
    if (c->holder[0] && c->holder[1]) {
        JsWkMsg *m;
        while ((m = pop(&c->wait, &c->wait_tail)))
            hand_to(js, m);
    }
    return JS_TRUE;
}

/* portPost(cid, tree, cids): a message for the other end of the channel.
 * portForward(cid, tree, cids): a message that had reached this runtime's
 * end, which is travelling now: it goes to whoever receives that end. */
static JSValue n_port_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *root = page_root(js);
    uint32_t cid = 0;
    if (argc < 2 || JS_ToUint32(ctx, &cid, argv[0]))
        return JS_UNDEFINED;
    JsPortChan *c = chan_find(root, cid);
    if (!c)
        return JS_UNDEFINED;
    JsWkMsg *m = msg_from_js(ctx, WM_PORT, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED);
    if (!m)
        return JS_EXCEPTION;
    m->id = cid;
    AuJs *to = NULL;
    if (!magic)
        to = c->holder[0] == js ? c->holder[1] : c->holder[1] == js ? c->holder[0] : NULL;
    if (to)
        hand_to(to, m);
    else
        push(&c->wait, &c->wait_tail, m);
    return JS_UNDEFINED;
}

/* ---- The page's natives ---- */

static JsWorker *worker_of(AuJs *js, uint32_t id)
{
    for (JsWorker *w = js->workers; w; w = w->next)
        if (w->id == id)
            return w;
    return NULL;
}

int au_js_worker_count(const AuJs *js)
{
    int n = 0;
    for (const JsWorker *w = js->workers; w; w = w->next)
        n++;
    return n;
}

/* workerCreate(url, name, module, opaque) -> id, or 0 when the page has
 * its limit of workers. The worker waits for its script (workerSource). */
static JSValue n_worker_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuJs *root = js->root;
    if (js->worker || argc < 4)
        return JS_NewInt32(ctx, 0);
    int n = 0, max = root->settings.max_workers ? root->settings.max_workers : 8;
    for (AuJs *g = root; g; g = g->group_next)
        n += au_js_worker_count(g);
    if (n >= max)
        return JS_NewInt32(ctx, 0);
    JsWorker *w = au_calloc(sizeof *w);
    w->owner = js;
    w->id = ++root->next_worker_id ? root->next_worker_id : ++root->next_worker_id;
    w->url = js_tostr(ctx, argv[0], NULL);
    w->name = js_tostr(ctx, argv[1], NULL);
    w->module = JS_ToBool(ctx, argv[2]) > 0;
    w->opaque = JS_ToBool(ctx, argv[3]) > 0;
    if (!w->url || !w->name) {
        au_free(w->url);
        au_free(w->name);
        au_free(w);
        return JS_EXCEPTION;
    }
    /* last, so that workers run in the order they were made */
    JsWorker **pp = &js->workers;
    while (*pp)
        pp = &(*pp)->next;
    *pp = w;
    return JS_NewUint32(ctx, w->id);
}

/* workerSource(id, text, url): the worker's script arrived (url: where
 * from, after redirects); text null: it could not be loaded. */
static JSValue n_worker_source(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    if (argc < 2 || JS_ToUint32(ctx, &id, argv[0]))
        return JS_UNDEFINED;
    JsWorker *w = worker_of(js, id);
    if (!w || w->started)
        return JS_UNDEFINED;
    if (!JS_IsString(argv[1])) {
        w->terminate = true;
        w->started = true;
        w->finished = true; /* it never ran: reaped by the next au_js_run_workers */
        return JS_UNDEFINED;
    }
    w->source = js_tostr(ctx, argv[1], &w->source_len);
    if (argc > 2 && JS_IsString(argv[2])) {
        au_free(w->url);
        w->url = js_tostr(ctx, argv[2], NULL);
    }
    if (w->source && w->url && !w->module)
        w->code = js_code_for_worker(js, w->url, w->source, w->source_len, &w->code_len);
    w->started = true;
    w->busy = true;
    return JS_UNDEFINED;
}

/* workerPost(id, tree, cids): a message for the worker's global. */
static JSValue n_worker_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    if (argc < 2 || JS_ToUint32(ctx, &id, argv[0]))
        return JS_UNDEFINED;
    JsWorker *w = worker_of(js, id);
    JsWkMsg *m = msg_from_js(ctx, WM_MESSAGE, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED);
    if (!m)
        return JS_EXCEPTION;
    if (!w || w->terminate || w->finished) {
        msg_free(m);
        return JS_UNDEFINED;
    }
    push(&w->in, &w->in_tail, m);
    w->busy = true;
    return JS_UNDEFINED;
}

static JSValue n_worker_terminate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    uint32_t id = 0;
    if (argc < 1 || JS_ToUint32(ctx, &id, argv[0]))
        return JS_UNDEFINED;
    JsWorker *w = worker_of(js, id);
    if (w) {
        w->terminate = true;
        w->busy = true;
        if (!w->started) /* its script never arrived: it never ran */
            w->started = w->finished = true;
        /* nothing more reaches the page from it */
        queue_free(w->out);
        w->out = w->out_tail = NULL;
    }
    return JS_UNDEFINED;
}

/* ---- The worker's natives ---- */

/* workerPostOut(tree, cids): a message for the Worker object. */
static JSValue n_worker_post_out(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (!js->worker || argc < 1)
        return JS_UNDEFINED;
    JsWkMsg *m = msg_from_js(ctx, WM_MESSAGE, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED);
    if (!m)
        return JS_EXCEPTION;
    JsWorker *w = js->wk;
    if (w->terminate)
        msg_free(m);
    else
        push(&w->out, &w->out_tail, m);
    return JS_UNDEFINED;
}

/* workerClose(): close() in the worker: no more tasks run. */
static JSValue n_worker_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (js->worker)
        js->wk->closing = true;
    return JS_UNDEFINED;
}

/* workerError(message, file, line): an uncaught error, for the Worker
 * object's error event. */
static JSValue n_worker_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (!js->worker || argc < 3 || js->wk->terminate)
        return JS_UNDEFINED;
    JsWkMsg *m = msg_new(WM_ERROR);
    m->text = js_tostr(ctx, argv[0], NULL);
    m->url = js_tostr(ctx, argv[1], NULL);
    int32_t line = 0;
    JS_ToInt32(ctx, &line, argv[2]);
    m->status = line;
    if (!m->text || !m->url) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        msg_free(m);
        return JS_UNDEFINED;
    }
    push(&js->wk->out, &js->wk->out_tail, m);
    return JS_UNDEFINED;
}

/* The worker's script was stopped by an error it cannot catch (out of
 * memory): the Worker object gets an error event with text, and the
 * worker ends, since its script was cut off at an arbitrary point. */
void js_worker_fatal(AuJs *js, const char *text)
{
    JsWorker *w = js->wk;
    if (!w || w->terminate)
        return;
    JsWkMsg *m = msg_new(WM_ERROR);
    m->text = au_strndup(text, strlen(text));
    m->url = au_strndup(w->url, strlen(w->url));
    push(&w->out, &w->out_tail, m);
    w->terminate = true;
}

/* workerReady(): a module worker's script has run (or failed): its
 * messages are delivered from now on. */
static JSValue n_worker_ready(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    if (js->worker) {
        js->wk->loading = false;
        js->wk->busy = true;
    }
    return JS_UNDEFINED;
}

/* workerInfo(): [name, can wait for the network]. */
static JSValue n_worker_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewString(ctx, js->worker && js->wk->name ? js->wk->name : ""));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewBool(ctx, js_worker_can_wait(js)));
    return a;
}

static const JSCFunctionListEntry port_natives[] = {
    JS_CFUNC_DEF("portNew", 0, n_port_new),
    JS_CFUNC_MAGIC_DEF("portHold", 1, n_port_hold, 0),
    JS_CFUNC_MAGIC_DEF("portRelease", 1, n_port_hold, 1),
    JS_CFUNC_MAGIC_DEF("portPost", 3, n_port_post, 0),
    JS_CFUNC_MAGIC_DEF("portForward", 3, n_port_post, 1),
};

static const JSCFunctionListEntry page_natives[] = {
    JS_CFUNC_DEF("workerCreate", 4, n_worker_create),
    JS_CFUNC_DEF("workerSource", 3, n_worker_source),
    JS_CFUNC_DEF("workerPost", 3, n_worker_post),
    JS_CFUNC_DEF("workerTerminate", 1, n_worker_terminate),
};

static const JSCFunctionListEntry inside_natives[] = {
    JS_CFUNC_DEF("workerPostOut", 2, n_worker_post_out),
    JS_CFUNC_DEF("workerClose", 0, n_worker_close),
    JS_CFUNC_DEF("workerError", 3, n_worker_error),
    JS_CFUNC_DEF("workerInfo", 0, n_worker_info),
    JS_CFUNC_DEF("workerReady", 0, n_worker_ready),
};

void js_worker_natives(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, port_natives, JS_COUNTOF(port_natives));
    if (js->worker)
        JS_SetPropertyFunctionList(js->ctx, N, inside_natives, JS_COUNTOF(inside_natives));
    else
        JS_SetPropertyFunctionList(js->ctx, N, page_natives, JS_COUNTOF(page_natives));
}

/* ---- The worker's side: its stack ---- */

static const AuCoroOps *coro_of(const JsWorker *w)
{
    return w->owner->hooks.coro;
}

/* Leaves the worker's stack until the next slice. */
static void worker_yield(JsWorker *w)
{
    const AuCoroOps *ops = coro_of(w);
    w->running = false;
    ops->yield(ops->ctx, w->co);
    w->running = true;
}

bool js_worker_terminating(const AuJs *js)
{
    return js->worker && js->wk->terminate;
}

int js_worker_interrupt(AuJs *js)
{
    JsWorker *w = js->wk;
    if (w->terminate)
        return 1;
    if (!w->co)
        return -1;
    if (js_now(js) >= w->slice_end) {
        w->busy = true;
        worker_yield(w);
        if (w->terminate)
            return 1;
    }
    return 0;
}

bool js_worker_can_wait(AuJs *js)
{
    return js->worker && js->wk->co != NULL;
}

/* Replays what the embedder reported for the worker's requests. Returns
 * false when the queue holds nothing of the kind. */
static bool drain_fetch(JsWorker *w)
{
    AuJs *js = w->js;
    bool any = false;
    JsWkMsg **pp = &w->in;
    while (*pp) {
        JsWkMsg *m = *pp;
        if (m->kind < WM_FETCH_HEAD) {
            pp = &m->next;
            continue;
        }
        *pp = m->next;
        if (!*pp) {
            /* the tail moved: find it again */
            w->in_tail = NULL;
            for (JsWkMsg *t = w->in; t; t = t->next)
                w->in_tail = t;
        }
        any = true;
        if (m->kind == WM_FETCH_HEAD) {
            const char **hv = m->nhead ? au_alloc(m->nhead * 2 * sizeof *hv) : NULL;
            const char *p = m->data.data;
            for (uint32_t i = 0; i < m->nhead * 2; i++) {
                hv[i] = p;
                p += strlen(p) + 1;
            }
            au_js_fetch_head(js, m->id, m->status, m->text ? m->text : "", hv, m->nhead, m->url);
            au_free(hv);
        } else if (m->kind == WM_FETCH_DATA) {
            au_js_fetch_data(js, m->id, m->data.data, m->data.len);
        } else {
            au_js_fetch_done(js, m->id, m->kind == WM_FETCH_DONE);
        }
        msg_free(m);
        /* a replay may have queued more or dropped the fetch: start over */
        pp = &w->in;
    }
    return any;
}

bool js_worker_wait(AuJs *js)
{
    JsWorker *w = js->wk;
    if (!w->co || w->terminate)
        return false;
    if (!drain_fetch(w)) {
        w->busy = false; /* nothing to do until the network answers */
        worker_yield(w);
        if (w->terminate)
            return false;
        drain_fetch(w);
    }
    return true;
}

bool js_worker_defer_fetch(AuJs *js, int kind, uint32_t id, int status, const char *text, const char *const *headers,
                           size_t nheaders, const char *url, const void *data, size_t len)
{
    if (!js->worker || js->wk->running)
        return false;
    JsWorker *w = js->wk;
    JsWkMsg *m = msg_new(WM_FETCH_HEAD + kind);
    m->id = id;
    m->status = status;
    if (text)
        m->text = au_strndup(text, strlen(text));
    if (url)
        m->url = au_strndup(url, strlen(url));
    for (size_t i = 0; i < nheaders * 2; i++)
        au_buf_append(&m->data, headers[i], strlen(headers[i]) + 1);
    m->nhead = (uint32_t)nheaders;
    if (len)
        au_buf_append(&m->data, data, len);
    push(&w->in, &w->in_tail, m);
    w->busy = true;
    return true;
}

/* Starts the worker's runtime and runs its script. False when the runtime
 * cannot start. */
static bool worker_boot(JsWorker *w)
{
    AuJs *js = js_create_worker(w->owner, w, w->url, w->opaque);
    if (!js)
        return false;
    w->js = js;
    w->loading = w->module;
    JSContext *ctx = js->ctx;
    /* A classic worker's script stays here until N.runScript(null) takes
     * it: making a script string of it would hold it twice more while it
     * compiles. A module's source goes through the module loader. */
    JSValue args[3] = {w->module ? JS_NewStringLen(ctx, w->source, w->source_len) : JS_NULL, JS_NewString(ctx, w->url),
                       JS_NewBool(ctx, w->module)};
    if (w->module) {
        au_free(w->source);
        w->source = NULL;
        w->source_len = 0;
    }
    JSValue r = js_call_prelude(js, "startWorker", 3, (JSValueConst *)args);
    JS_FreeValue(ctx, r);
    for (int i = 0; i < 3; i++)
        JS_FreeValue(ctx, args[i]);
    au_js_checkpoint(js);
    return true;
}

/* One slice of the worker's life, on its stack. Returns false when the
 * worker has ended (its runtime is freed). */
static bool worker_step(JsWorker *w)
{
    if (!w->js && !w->terminate && !worker_boot(w)) {
        JsWkMsg *m = msg_new(WM_ERROR);
        m->text = au_strndup("The worker could not start (out of memory)", 42);
        m->url = au_strndup(w->url, strlen(w->url));
        push(&w->out, &w->out_tail, m);
        w->terminate = true;
    }
    AuJs *js = w->js;
    if (js && !w->terminate && !w->closing) {
        w->busy = false;
        drain_fetch(w);
        JsWkMsg *m;
        while (!w->terminate && !w->closing && !w->loading && (m = pop(&w->in, &w->in_tail))) {
            deliver_js(js, m->kind == WM_PORT ? 1 : 0, m->id, m);
            msg_free(m);
            drain_fetch(w);
        }
        uint32_t slice = w->owner->root->settings.worker_slice_ms ? w->owner->root->settings.worker_slice_ms : 8;
        if (!w->terminate && !w->closing && au_js_run_tasks(js, slice))
            w->busy = true;
        if (au_js_has_tasks(js) || (w->in && !w->loading))
            w->busy = true;
    }
    if (!w->terminate && !w->closing)
        return true;
    if (js) {
        w->js = NULL;
        chans_drop_holder(w->owner->root, js);
        au_js_free(js);
    }
    if (!w->terminate)
        push(&w->out, &w->out_tail, msg_new(WM_CLOSED));
    return false;
}

static void worker_main(void *arg)
{
    JsWorker *w = arg;
    w->running = true;
    while (worker_step(w))
        worker_yield(w);
    w->running = false;
    w->finished = true;
}

/* ---- The page's side ---- */

/* Gives the worker a slice. */
static void worker_run(JsWorker *w)
{
    AuJs *owner = w->owner;
    const AuCoroOps *ops = coro_of(w);
    uint32_t slice = owner->root->settings.worker_slice_ms ? owner->root->settings.worker_slice_ms : 8;
    w->slice_end = js_now(owner) + slice;
    if (ops && !w->co && !w->js) {
        w->stack = owner->root->settings.worker_stack ? owner->root->settings.worker_stack : AU_JS_WORKER_STACK;
#if defined(__SANITIZE_ADDRESS__)
        w->stack *= 64;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
        w->stack *= 64;
#endif
#endif
        w->co = ops->create(ops->ctx, w->stack, worker_main, w);
        if (!w->co)
            w->stack = 0; /* no stack to be had: it runs here */
    }
    if (w->co) {
        ops->resume(ops->ctx, w->co);
        return;
    }
    w->running = true;
    if (!worker_step(w))
        w->finished = true;
    w->running = false;
}

/* Ends a worker at once: its script is unwound and its runtime freed. */
static void worker_end_now(JsWorker *w)
{
    w->terminate = true;
    for (int i = 0; i < 100000 && !w->finished && w->started; i++) {
        if (!w->co && !w->js) {
            w->finished = true;
            break;
        }
        worker_run(w);
    }
    if (w->co) {
        const AuCoroOps *ops = coro_of(w);
        ops->destroy(ops->ctx, w->co);
        w->co = NULL;
    }
}

static void worker_free(JsWorker *w)
{
    queue_free(w->in);
    queue_free(w->out);
    au_free(w->url);
    au_free(w->name);
    au_free(w->source);
    au_free(w->code);
    au_free(w);
}

void js_workers_free(AuJs *js)
{
    while (js->workers) {
        JsWorker *w = js->workers;
        js->workers = w->next;
        worker_end_now(w);
        worker_free(w);
    }
    queue_free(js->port_in);
    js->port_in = js->port_in_tail = NULL;
    AuJs *root = js->root;
    chans_drop_holder(root, js);
    if (root == js) {
        for (uint32_t i = 0; i < root->nchans; i++)
            queue_free(root->chans[i].wait);
        au_free(root->chans);
        root->chans = NULL;
        root->nchans = 0;
    }
}

size_t js_workers_memory(const AuJs *js)
{
    size_t n = 0;
    for (const JsWorker *w = js->workers; w; w = w->next)
        n += (w->js ? w->js->live_bytes : 0) + w->stack + w->source_len + w->code_len;
    return n;
}

uint8_t *js_worker_take_code(AuJs *js, size_t *len)
{
    JsWorker *w = js->wk;
    if (!w || !w->code)
        return NULL;
    uint8_t *c = w->code;
    *len = w->code_len;
    w->code = NULL;
    w->code_len = 0;
    return c;
}

char *js_worker_take_source(AuJs *js, size_t *len)
{
    JsWorker *w = js->wk;
    if (!w || !w->source)
        return NULL;
    char *s = w->source;
    *len = w->source_len;
    w->source = NULL;
    w->source_len = 0;
    return s;
}

size_t js_workers_peak(const AuJs *js)
{
    size_t n = 0;
    for (const JsWorker *w = js->workers; w; w = w->next)
        if (w->js && w->js->peak_bytes > n)
            n = w->js->peak_bytes;
    return n;
}

void js_workers_summary(const AuJs *js, AuBuf *out)
{
    for (const JsWorker *w = js->workers; w; w = w->next) {
        if (!w->js)
            continue;
        au_buf_printf(out, "  worker %s: %lu KB, peak %lu KB, limit %lu KB, and a %lu KB stack\n", w->url,
                      (unsigned long)(w->js->live_bytes / 1024), (unsigned long)(w->js->peak_bytes / 1024),
                      (unsigned long)(w->js->settings.memory_limit / 1024), (unsigned long)(w->stack / 1024));
    }
}

/* What a worker sent, for its document. */
static bool drain_out(AuJs *js, JsWorker *w)
{
    bool any = false;
    JsWkMsg *m;
    uint32_t id = w->id;
    while ((m = pop(&w->out, &w->out_tail))) {
        any = true;
        deliver_js(js, m->kind == WM_MESSAGE ? 0 : m->kind == WM_ERROR ? 2 : 3, id, m);
        msg_free(m);
        if (js->closing || !worker_of(js, id))
            break;
    }
    return any;
}

static bool worker_due(const JsWorker *w, double now)
{
    if (!w->started || w->finished)
        return false;
    if (w->busy || w->terminate || (w->in && !w->loading) || !w->js)
        return true;
    double t = au_js_next_timer(w->js);
    return t >= 0 && t <= now;
}

bool au_js_workers_busy(const AuJs *js)
{
    if (js->port_in)
        return true;
    double now = js_now(js);
    for (const JsWorker *w = js->workers; w; w = w->next)
        if (worker_due(w, now) || w->out || (w->finished && w->started))
            return true;
    return false;
}

double js_workers_next_timer(const AuJs *js)
{
    double best = -1;
    for (const JsWorker *w = js->workers; w; w = w->next) {
        double t = w->js ? au_js_next_timer(w->js) : -1;
        if (t >= 0 && (best < 0 || t < best))
            best = t;
    }
    return best;
}

bool js_workers_idle(const AuJs *js)
{
    if (au_js_workers_busy(js))
        return false;
    for (const JsWorker *w = js->workers; w; w = w->next)
        if (!w->started || (w->js && !au_js_idle(w->js)))
            return false;
    return true;
}

bool au_js_run_workers(AuJs *js, uint32_t budget_ms)
{
    if (js->closing || js->worker || (!js->workers && !js->port_in) || js->root->depth > 0)
        return false;
    bool ran = false;
    double start = js_now(js);
    /* port messages from other runtimes for this document */
    JsWkMsg *m;
    while ((m = pop(&js->port_in, &js->port_in_tail))) {
        deliver_js(js, 1, m->id, m);
        msg_free(m);
        ran = true;
    }
    int n = au_js_worker_count(js);
    for (int round = 0; round < 64 && n; round++) {
        bool any = false;
        for (int i = 0; i < n; i++) {
            /* in turn, starting after the one that ran first last time */
            int at = (int)((js->worker_rr + (uint32_t)i) % (uint32_t)n);
            JsWorker *w = js->workers;
            for (int k = 0; k < at && w; k++)
                w = w->next;
            if (!w)
                break;
            if (worker_due(w, js_now(js))) {
                worker_run(w);
                any = ran = true;
            }
            if (drain_out(js, w))
                ran = true;
            if (js->closing)
                return true;
        }
        /* reap the ones that ended */
        for (JsWorker **pp = &js->workers; *pp;) {
            JsWorker *w = *pp;
            if (w->finished && !w->out) {
                *pp = w->next;
                if (w->co) {
                    const AuCoroOps *ops = coro_of(w);
                    ops->destroy(ops->ctx, w->co);
                }
                worker_free(w);
            } else {
                pp = &w->next;
            }
        }
        n = au_js_worker_count(js);
        js->worker_rr++;
        if (!any || js_now(js) - start >= budget_ms)
            break;
    }
    return ran;
}
