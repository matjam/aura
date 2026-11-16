/* Internals of src/js. Only src/js includes this (and quickjs.h).
 *
 * Wrapper ownership model
 * -----------------------
 * A node that script has touched has a wrapper: a QuickJS object of class
 * cls_node whose opaque is an AuJsWrap, and AuNode.js points to the same
 * AuJsWrap. Nodes script never touched have AuNode.js == NULL and no extra
 * memory.
 *
 * - The wrapper holds one node reference (au_node_ref), released by its
 *   finalizer. So a node lives at least as long as its wrapper: no
 *   dangling AuNode pointers from script, whatever script removes.
 * - The node holds one QuickJS reference to its wrapper (flag
 *   AU_NODEF_JS_STRONG) while anything else holds the node
 *   (au_node_held_elsewhere: a parent, or references beyond the
 *   wrapper's). Expando properties and event listeners (kept on the
 *   wrapper) then survive while script holds no reference. When the node
 *   loses its other holders, the QuickJS reference is dropped and the
 *   wrapper is an ordinary object: collected once script drops it, which
 *   releases the node.
 * - The DOM tells us about holders through the hold_changed hook
 *   (src/dom). Gaining a holder takes the QuickJS reference at once;
 *   losing one only queues the node (AuJs.pending), and the reference is
 *   dropped at the next safe point (end of a native call, task or
 *   checkpoint), never inside a DOM operation, because dropping it may
 *   free the wrapper and with it the node.
 * - au_js_free drops every node's reference first (walking AuJs.wraps),
 *   then frees the context and runtime; finalizers release the nodes, and
 *   the document is released last.
 *
 * Known limits of the model: a reference cycle that passes through the
 * node tree (a listener on a detached child that captures its detached
 * parent) is not visible to QuickJS's cycle collector and lives until the
 * document is discarded. A detached subtree is kept alive from its root
 * down: script holding only a descendant of a detached, unreferenced root
 * keeps the descendant (and its subtree) but not the root. */
#ifndef AURA_JS_INT_H
#define AURA_JS_INT_H

#include "js.h"

#include <quickjs.h>

typedef struct AuJsWrap AuJsWrap;
struct AuJsWrap {
    JSValue obj; /* the wrapper object (a QuickJS reference only while strong) */
    AuNode *node;
    AuJs *js;
    AuJsWrap *prev, *next; /* every wrapper of the runtime */
    AuJsWrap *pnext;       /* pending list */
    bool pending;
};

typedef struct JsTimer {
    uint32_t id;
    double due;
    double interval; /* < 0: one-shot */
    uint32_t nesting;
    JSValue fn;      /* function, or a string to evaluate */
    JSValue *args;
    int nargs;
} JsTimer;

typedef struct JsTask {
    JSValue fn;
    JSValue arg;
} JsTask;

typedef struct JsFrameCb {
    uint32_t id;
    JSValue fn;
} JsFrameCb;

/* A raw DOM mutation queued by the hooks, converted to MutationRecords at
 * the next checkpoint. Nodes are referenced. */
/* A node with MutationObserver registrations (set by the prelude). */
#define JS_NODEF_OBSERVED AU_NODEF_JS_MARK
#define JS_MUT_MAX_OBSERVED 8

typedef struct JsMutation {
    uint8_t kind;      /* 1 childList, 2 attributes, 3 characterData */
    uint8_t nobs;
    AuNode *target;
    AuNode *added, *removed, *prev, *next;
    AuAtom attr;
    char *old;         /* old value (attributes, characterData), or NULL */
    size_t old_len;
    uint32_t batch;    /* records of one DOM call share a batch */
    /* The observed inclusive ancestors of target when the mutation
     * happened (interest is decided then, not at delivery). */
    AuNode *obs[JS_MUT_MAX_OBSERVED];
} JsMutation;

/* Custom element reactions queued by the hooks. */
typedef struct JsReaction {
    uint8_t kind;      /* 1 connected, 2 disconnected, 3 attribute, 5 script connected */
    AuElement *el;     /* referenced */
    AuAtom attr;
    char *old;
    bool had_old;
} JsReaction;

typedef struct JsRejection {
    JSValue promise, reason;
} JsRejection;

typedef struct JsFetch JsFetch;

/* Interfaces with C-created prototypes (see wrap.c). */
enum {
#define IFACE(id, name, parent) JSI_##id,
#include "ifaces.def"
#undef IFACE
    JSI_COUNT
};

struct AuJs {
    JSRuntime *rt;
    JSContext *ctx;
    /* Frames (frames.c). The documents of one top-level page share one
     * runtime, each with its own context: root is the AuJs that owns the
     * runtime (itself for a top-level document), group links every AuJs of
     * the runtime from root. The wrapper lists, the entry depth, the time
     * budget and the allocator's counters below are used through root. */
    AuJs *root;
    AuJs *group_next;
    AuJs *entered[16];  /* root: the AuJs of each nested entry from the embedder */
    bool opaque_origin; /* a sandboxed document: same-origin with nothing */
    struct AuStorageMap *own_storage[2]; /* localStorage, sessionStorage without the embedder's hook (natives2.c) */
    bool frames_dirty;  /* a frame element went into or left this document */
    bool any_frames_dirty; /* root: some AuJs of the group has frames_dirty */
    AuDocument *doc;
    /* Documents script created (DOMParser, createHTMLDocument, a cloned
     * document), referenced until au_js_free so that no node outlives its
     * document while a wrapper holds it. */
    AuDocument **docs;
    uint32_t ndocs, docs_cap;
    AuHtmlParser *parser;
    /* document.open() after parsing ended: everything written since, which
     * the document is parsed from again at each write (natives.c). */
    AuBuf open_buf;
    bool opened;
    AuJsSettings settings;
    AuJsHooks hooks;
    char *url, *base, *origin;
    char *referrer; /* document.referrer, or NULL */
    JSClassID cls_node;
    JSValue proto[JSI_COUNT];
    JSValue ctor[JSI_COUNT];
    JSValue global;   /* not referenced: ctx owns it */
    JSValue P;        /* the prelude's exports object (internal entry points) */
    AuJsWrap *wraps;  /* all wrappers */
    AuJsWrap *pending;
    uint32_t nwraps;
    /* run state */
    int depth;        /* nested entries from the embedder */
    double run_start;
    double next_check;
    bool interrupted, closing;
    AuElement *current_script;
    /* event loop */
    JsTimer *timers;
    uint32_t ntimers, timers_cap, next_timer_id, timer_nesting;
    JsTask *tasks;
    uint32_t ntasks, tasks_cap, task_head;
    JsFrameCb *frames;
    uint32_t nframes, frames_cap, next_frame_id;
    JsRejection *rejections;
    uint32_t nrejections, rejections_cap;
    /* mutations and reactions */
    JsMutation *muts;
    uint32_t nmuts, muts_cap;
    uint32_t observers; /* MutationObserver registrations (from the prelude) */
    JsReaction *reacts;
    uint32_t nreacts, reacts_cap;
    uint32_t ce_defs;   /* custom element definitions */
    uint32_t script_watch; /* script-created scripts not yet started (from the prelude) */
    bool in_reactions, in_checkpoint;
    bool slots_dirty;
    bool delivery_queued; /* a mutation delivery microtask is queued */
    uint32_t mut_batch;   /* advanced at the end of each mutating native */
    /* live ranges (ranges.c) */
    struct JsRange *ranges;
    uint32_t nranges, live_ranges; /* slots, and slots in use */
    const AuNode *range_text_node; /* the prelude is replacing this text node's data */
    AuNode *range_pend_node;       /* a text node whose data changed (referenced), to settle */
    char *range_pend_old;          /* its data before the change */
    size_t range_pend_len;
    /* fetches and modules */
    JsFetch *fetches;
    uint32_t next_fetch_id;
    struct AuCorsCache *cors_cache; /* root: the CORS preflight cache (fetch.c) */
    void *modules;
    /* workers (worker.c): worker is set in a worker's own AuJs, which has
     * no document; wk is then its record. workers lists the workers a
     * document made. */
    bool worker;
    struct JsWorker *wk;
    struct JsWorker *workers;
    uint32_t worker_rr;          /* which worker runs first next time */
    struct JsWkMsg *port_in, *port_in_tail; /* a document: port messages from other runtimes */
    struct JsPortChan *chans;    /* root: message channels that cross runtimes */
    uint32_t nchans, next_chan, next_worker_id;
    char **muted; /* URLs of scripts whose errors are muted (au_js_mute_script) */
    uint32_t nmuted;
    /* statistics */
    uint32_t errors, interrupts;
    size_t live_bytes; /* bytes the runtime holds (runtime.c allocator) */
    size_t gc_base;   /* live_bytes after the last collection the bindings ran */
    double last_ran;  /* when a task or timer last ran (now_ms clock) */
    bool oom;          /* an allocation was refused: the reserve is open */
    /* Memory accounting (runtime.c allocator), all in the root. Every
     * block is charged to the context that was entered when it was
     * allocated (slot 0: none, the runtime's own setup and collections);
     * a freed context's slot stays, for the blocks that outlive it. */
    size_t peak_bytes;          /* the most live_bytes has been */
    struct JsMemSlot *slots;
    uint32_t nslots, cur_slot;
    uint32_t slot;              /* every AuJs: its slot in root->slots */
    size_t gc_force_at;         /* live_bytes past which the next object creation collects */
    char *compiling;            /* the URL of the script being compiled; NULL: running */
    /* The last refused allocation: what was in use, what was asked for,
     * and what was being compiled ("" when script was running). */
    size_t oom_live, oom_asked;
    char oom_url[200];
    bool oom_noted;             /* the numbers above are of an error not yet reported */
    bool oom_machine;           /* refused because the machine is short of memory, not at the limit */
    /* root: large scripts compiled in this runtime, so that another
     * document of the page that loads the same script reads its bytecode
     * instead of compiling it again (runtime.c, "Compiled scripts"). */
    struct JsCode {
        char *url;
        size_t len;
        uint32_t hash;
        int line;
        JSValue fn;   /* the compiled script, in owner's context */
        AuJs *owner;
        uint32_t used;
        double when;  /* when it was compiled or last read (now_ms clock) */
    } code[4];
    uint32_t code_clock;
    /* canvas (natives_canvas.c) */
    struct JsCanvas *canvas;
    bool canvas_pending; /* a canvas element's width or height attribute is changing */
    /* root: the classes of crypto.subtle's key handles and key generation
     * jobs (natives_crypto.c) */
    JSClassID crypto_cls[2];
};

/* ---- runtime.c ---- */
typedef struct JsMemSlot {
    size_t live, peak;
} JsMemSlot;
/* Compiles and runs a classic script in js's context (the caller has
 * entered js). Notes what is being compiled for the out-of-memory report
 * and collects before a large compile. Returns the script's value or
 * JS_EXCEPTION. */
JSValue js_eval_classic(AuJs *js, const char *src, size_t len, const char *url, int line);
/* The bytecode of the script (url, src) when this page's runtime has
 * compiled it, as an au_alloc'd block for a worker to read (see
 * "Compiled scripts" in runtime.c); NULL when it has not. */
uint8_t *js_code_for_worker(AuJs *js, const char *url, const char *src, size_t len, size_t *code_len);
/* The UTF-8 bytes of a typed array or ArrayBuffer as an au_alloc'd C
 * string, without a byte order mark; the buffer is detached (its memory
 * in the script heap freed). NULL when v is neither. */
char *js_take_script_bytes(JSContext *ctx, JSValueConst v, size_t *len);
/* Lets go of compiled scripts nothing has asked for in the last 20
 * seconds (called while the page is idle). */
void js_code_expire(AuJs *js);
AuJs *js_of_ctx(JSContext *ctx);
void js_report_exception(AuJs *js);
void js_console(AuJs *js, AuJsLogLevel level, const char *text, size_t len);
/* Entering and leaving script from the embedder (stack top, budget). */
void js_enter(AuJs *js);
void js_leave(AuJs *js);
/* Calls fn(this, args) and reports an exception. Returns the result
 * (JS_UNDEFINED after an exception). */
JSValue js_call(AuJs *js, JSValueConst fn, JSValueConst this_val, int argc, JSValueConst *argv);
/* Calls a prelude entry point by name. */
JSValue js_call_prelude(AuJs *js, const char *name, int argc, JSValueConst *argv);
void js_safe_point(AuJs *js);
double js_now(const AuJs *js);
/* For natives that can be slow (a forced layout): true when the script's
 * time is up; the native then returns js_throw_stopped(ctx). See
 * runtime.c. */
bool js_out_of_time(AuJs *js);
JSValue js_throw_stopped(JSContext *ctx);
/* JS string from UTF-8 / to an au_alloc'd UTF-8 copy. */
JSValue js_str(JSContext *ctx, const char *s, size_t len);
char *js_tostr(JSContext *ctx, JSValueConst v, size_t *len);

/* ---- frames.c ---- */
void js_frames_natives(AuJs *js, JSValue N);
/* The AuJs whose window is global, or NULL (a document that has gone). */
AuJs *js_of_global(AuJs *js, JSValueConst global);
/* Tells the embedder about frame elements that came or went, and gives the
 * window its indexed and named frame properties. */
void js_frames_flush(AuJs *js);
bool js_is_frame_element(const AuNode *n);

/* ---- wrap.c ---- */
void js_wrap_init(AuJs *js);
/* A child context is going: lets go of its documents' wrappers. */
void js_wrap_free_child(AuJs *js);
void js_wrap_free_all(AuJs *js);
JSValue js_wrap(AuJs *js, AuNode *n);     /* new reference; JS_NULL for NULL */
AuNode *js_unwrap(AuJs *js, JSValueConst v); /* NULL if not a node */
void js_hold_changed(AuJs *js, AuNode *n);
void js_drain_pending(AuJs *js);
JSValue js_iface_proto(AuJs *js, int iface);
int js_iface_of(const AuNode *n);

/* ---- natives.c, natives2.c ---- */
void js_natives_init(AuJs *js, JSValue N);
void js_natives2_init(AuJs *js, JSValue N);
void js_storage_free(AuJs *js);
void js_natives_shared_init(AuJs *js, JSValue N); /* the natives a worker has too */
void js_natives_crypto_init(AuJs *js, JSValue N);  /* crypto.subtle's (natives_crypto.c), part of the shared ones */
void js_natives_forms_init(AuJs *js, JSValue N);

/* ---- natives_canvas.c ---- */
void js_canvas_init(AuJs *js, JSValue N);
void js_canvas_free(AuJs *js);
/* From the DOM hook: an attribute of el is about to change. */
void js_canvas_attr_changing(AuJs *js, AuElement *el, AuAtom name);
/* Resizes the bitmaps of canvases whose size attributes changed. */
void js_canvas_sync(AuJs *js);

/* ---- ranges.c ---- */
typedef struct JsRange JsRange;
void js_natives_ranges_init(AuJs *js, JSValue N);
/* From the DOM hooks: child went into parent, or left it (prev: its
 * previous sibling before the removal). They move live ranges' boundary
 * points and run no script. */
void js_ranges_inserted(AuJs *js, AuNode *parent, AuNode *child);
void js_ranges_removed(AuJs *js, AuNode *parent, AuNode *child, AuNode *prev);
void js_ranges_text(AuJs *js, AuText *t, const char *old, size_t old_len);
void js_ranges_free(AuJs *js);

/* ---- mutation.c ---- */
void js_hooks_install(AuJs *js);
void js_hooks_remove(AuJs *js);
/* Hands queued records to the prelude; notify false only queues them
 * there (takeRecords). */
void js_deliver_mutations(AuJs *js, bool notify);
void js_run_reactions(AuJs *js);
void js_ce_flush(AuJs *js);
void js_mutations_free(AuJs *js);

/* ---- loop.c ---- */
void js_loop_init(AuJs *js, JSValue N);
void js_loop_free(AuJs *js);
void js_queue_task(AuJs *js, JSValueConst fn, JSValueConst arg);

/* ---- fetch.c ---- */
void js_fetch_init(AuJs *js, JSValue N);
void js_fetch_free(AuJs *js);
bool js_fetch_pending(const AuJs *js);
/* Origin of an absolute URL into out ("null" for opaque origins). */
void js_origin_of(const char *url, char *out, size_t cap);
bool js_same_origin(const AuJs *js, const char *url);

/* ---- worker.c ---- */
/* The embedder reported something for a fetch of a worker while the
 * worker was not running: kept for the worker and replayed on its stack
 * (kind 0 head, 1 data, 2 done, 3 failed). Returns false when js is not a
 * worker or is running now (the caller then handles it at once). */
bool js_worker_defer_fetch(AuJs *js, int kind, uint32_t id, int status, const char *text, const char *const *headers,
                           size_t nheaders, const char *url, const void *data, size_t len);
/* Whether this worker can park its stack to wait for the network, and the
 * wait itself: returns false when the worker is being terminated. */
bool js_worker_can_wait(AuJs *js);
bool js_worker_wait(AuJs *js);
void js_worker_natives(AuJs *js, JSValue N);
/* A document is going: ends its workers. A root is going: frees the
 * channel table. */
void js_workers_free(AuJs *js);
/* A worker's interrupt check: 1 to stop the script (the worker is being
 * terminated), 0 to go on (after yielding when its slice is used up), -1
 * for the ordinary time budget (a worker without its own stack). */
int js_worker_interrupt(AuJs *js);
bool js_worker_terminating(const AuJs *js);
size_t js_workers_memory(const AuJs *js);
/* A worker's own script text, handed to the caller (NULL when taken). */
char *js_worker_take_source(AuJs *js, size_t *len);
/* The bytecode the page wrote for the worker's script (au_alloc'd), once. */
uint8_t *js_worker_take_code(AuJs *js, size_t *len);
void js_worker_fatal(AuJs *js, const char *text);
size_t js_workers_peak(const AuJs *js);                 /* the largest peak of a live worker */
void js_workers_summary(const AuJs *js, AuBuf *out);    /* a line per live worker */
/* The earliest timer of a document's workers (-1: none), and whether
 * every one of them only waits for a message. */
double js_workers_next_timer(const AuJs *js);
bool js_workers_idle(const AuJs *js);
/* runtime.c: the runtime of a worker, made on the worker's stack. */
AuJs *js_create_worker(AuJs *owner, struct JsWorker *wk, const char *url, bool opaque_origin);

/* ---- module.c ---- */
void js_module_init(AuJs *js);
void js_module_natives(AuJs *js, JSValue N);
void js_module_free(AuJs *js);
bool js_modules_pending(const AuJs *js);
bool js_module_probing(const AuJs *js);

/* An image file's size from its header (imagesize.c). */
bool js_image_size(const void *data, size_t len, int *w, int *h);
/* Keeps a document script created alive until au_js_free (natives.c). */
void js_keep_doc(AuJs *js, AuDocument *d);

/* ---- url.c: src/url's parser in the URL API's terms ---- */
typedef struct JsUrl {
    char *href; /* serialized */
    /* each component as the URL API returns it, "" when absent */
    char *protocol, *username, *password, *hostname, *port, *pathname, *search, *hash;
} JsUrl;
/* Parses input against base (may be NULL). Returns false for a failure.
 * Free with js_url_free. */
bool js_url_parse(const char *input, const char *base, JsUrl *out);
void js_url_free(JsUrl *u);

/* A worker coroutine's stack unless the settings say otherwise. The
 * sanitizers' frames are far larger (see au_js_settings_default). */
#define AU_JS_WORKER_STACK ((size_t)256 << 10)
#define AU_JS_WORKER_MEMORY ((size_t)24 << 20) /* a worker's memory limit when the embedder sets none */

#define JS_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#endif
