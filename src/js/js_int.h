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
    AuDocument *doc;
    AuHtmlParser *parser;
    AuJsSettings settings;
    AuJsHooks hooks;
    char *url, *base, *origin;
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
    /* fetches and modules */
    JsFetch *fetches;
    void *modules;
    /* statistics */
    uint32_t errors, interrupts;
    size_t live_bytes; /* bytes the runtime holds (runtime.c allocator) */
    bool oom;          /* an allocation was refused: the reserve is open */
};

/* ---- runtime.c ---- */
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
/* JS string from UTF-8 / to an au_alloc'd UTF-8 copy. */
JSValue js_str(JSContext *ctx, const char *s, size_t len);
char *js_tostr(JSContext *ctx, JSValueConst v, size_t *len);

/* ---- wrap.c ---- */
void js_wrap_init(AuJs *js);
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

/* ---- module.c ---- */
void js_module_init(AuJs *js);
void js_module_natives(AuJs *js, JSValue N);
void js_module_free(AuJs *js);
bool js_modules_pending(const AuJs *js);
bool js_module_probing(const AuJs *js);

/* ---- url.c: the minimal URL helper ---- */
typedef struct JsUrl {
    char *href;     /* serialized */
    /* offsets into href for each component; -1 when absent */
    char *protocol, *username, *password, *hostname, *port, *pathname, *search, *hash;
} JsUrl;
/* Parses input against base (may be NULL). Returns false for a failure.
 * Free with js_url_free. Marked for replacement by src/url. */
bool js_url_parse(const char *input, const char *base, JsUrl *out);
void js_url_free(JsUrl *u);

#define JS_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#endif
