/* Aura JavaScript: QuickJS-ng behind a narrow interface.
 *
 * One AuJs per document: a QuickJS runtime and context whose global object
 * is the document's window, the DOM bindings, and an event loop (tasks,
 * microtasks, timers, animation frames). Nothing outside src/js sees a
 * QuickJS type.
 *
 * To run script in a document:
 *
 *     AuJsHooks h = {0};  h.ctx = me;  h.now_ms = my_clock;  ...
 *     AuJs *js = au_js_new(doc, &settings, &h);
 *     au_js_run_classic(js, script_el, text, len, url, 1);
 *     ...
 *     au_js_run_tasks(js, 20);           // timers and queued tasks, 20 ms budget
 *     if (au_js_wants_frame(js)) au_js_run_frame(js);   // before a repaint
 *     au_js_free(js);                    // before the document goes
 *
 * Limits. Each AuJs has a memory cap (QuickJS's allocation counter; a
 * failed allocation becomes a script out-of-memory error, never the
 * application's fatal handler), a C stack cap (scripts that recurse past
 * it get a RangeError, never a crash) and a run-time budget: while one
 * script, callback or microtask checkpoint runs longer than
 * settings.slice_ms, QuickJS's interrupt handler asks hooks.long_script
 * whether to continue. Without a hook a script is stopped after
 * settings.kill_ms (default 10 s). A stopped script ends with an
 * uncatchable error; later tasks still run.
 *
 * Wrapper ownership. See js_int.h: a node's wrapper keeps the node alive
 * (one node reference); the node keeps its wrapper alive (one QuickJS
 * reference) while the node has a parent or other references; a wrapper
 * held only by script is collected normally. Nodes without script interest
 * have no wrapper and cost nothing. au_js_free drops every wrapper before
 * the document is released.
 *
 * Re-entrancy. Every entry point may run script, and script may mutate or
 * replace the document, navigate (through a hook: the embedder must not
 * free the AuJs from inside a hook; it defers that) or call back into the
 * embedder. The embedder must not call au_js_free from inside a hook. */
#ifndef AURA_JS_H
#define AURA_JS_H

#include "../dom/dom.h"
#include "../html/html.h"

typedef struct AuJs AuJs;

typedef enum {
    AU_JS_LOG = 0,
    AU_JS_INFO,
    AU_JS_WARN,
    AU_JS_ERROR,
    AU_JS_DEBUG
} AuJsLogLevel;

typedef struct AuJsSettings {
    size_t memory_limit;  /* bytes the runtime may allocate; 0: 32 MB */
    size_t stack_limit;   /* bytes of C stack script may use below the entry point; 0: 512 KB.
                           * The thread must have this much stack free, plus margin. */
    uint32_t slice_ms;    /* long-script check interval; 0: 1000 */
    uint32_t kill_ms;     /* default limit without a long_script hook; 0: 10000 */
    const char *user_agent; /* navigator.userAgent; NULL: "Mozilla/5.0 (Macintosh; PPC Mac OS 9) Aura/1" */
    const char *language;   /* navigator.language; NULL: "en-US" */
    bool do_not_track;      /* navigator.doNotTrack "1" */
    bool global_privacy_control;
    bool online;            /* navigator.onLine */
    bool cookies_enabled;
    uint32_t storage_quota; /* bytes per origin for each storage area; 0: 5 MB */
    int screen_width, screen_height; /* 0: 1024x768 */
    /* Frames. origin replaces the origin taken from the document URL (an
     * about:blank or srcdoc frame has its parent's); it is copied.
     * sandbox_origin gives the document an origin equal to no other (a
     * sandboxed frame without allow-same-origin). no_top_navigation: the
     * document may navigate only itself and its descendants. */
    const char *origin;
    bool sandbox_origin;
    bool no_top_navigation;
} AuJsSettings;

void au_js_settings_default(AuJsSettings *s);

/* ---- Fetch interface ----
 * Requests that script makes (fetch, XMLHttpRequest, module and external
 * classic scripts) go through this interface. The embedder starts a
 * request and later reports its response with au_js_fetch_head, _data and
 * _done (never from inside start). The JS layer enforces the same-origin
 * policy before calling start; CORS checks are marked in fetch.c. */
typedef enum { AU_FETCH_OMIT, AU_FETCH_SAME_ORIGIN, AU_FETCH_INCLUDE } AuFetchCredentials;
typedef enum { AU_FETCH_CORS, AU_FETCH_NO_CORS, AU_FETCH_SAME_ORIGIN_MODE, AU_FETCH_NAVIGATE } AuFetchMode;
/* AU_FETCH_DEST_IMAGE: an <img> loaded from script; the bytes reach only
 * the bindings, which expose the size and load or error. */
typedef enum { AU_FETCH_DEST_EMPTY, AU_FETCH_DEST_SCRIPT, AU_FETCH_DEST_MODULE, AU_FETCH_DEST_IMAGE } AuFetchDest;

typedef struct AuFetchRequest {
    const char *method;  /* upper case */
    const char *url;     /* absolute */
    const char *const *headers; /* name, value, name, value, ... */
    size_t nheaders;     /* pairs */
    const void *body;
    size_t body_len;
    uint8_t credentials; /* AuFetchCredentials */
    uint8_t mode;        /* AuFetchMode */
    uint8_t dest;        /* AuFetchDest */
} AuFetchRequest;

typedef struct AuFetcher {
    void *ctx;
    /* Starts a request. Returns a nonzero id, or 0 if it cannot start
     * (the request then fails with a network error). */
    uint32_t (*start)(void *ctx, AuJs *js, const AuFetchRequest *req);
    /* Stops a request; nothing more is reported for id. Optional. */
    void (*abort)(void *ctx, uint32_t id);
} AuFetcher;

/* Response head: status, status text, headers as name/value pairs, and the
 * final URL after redirects (NULL: the request URL). */
void au_js_fetch_head(AuJs *js, uint32_t id, int status, const char *status_text, const char *const *headers,
                      size_t nheaders, const char *final_url);
void au_js_fetch_data(AuJs *js, uint32_t id, const void *data, size_t len);
/* ok false: a network error. */
void au_js_fetch_done(AuJs *js, uint32_t id, bool ok);

/* ---- Embedder hooks ----
 * Every hook is optional except now_ms. */
typedef struct AuJsRect {
    float x, y, w, h;
} AuJsRect;

typedef struct AuJsHooks {
    void *ctx;
    /* Monotonic milliseconds: timers, performance.now, script budgets. */
    double (*now_ms)(void *ctx);
    /* console output, one formatted line. */
    void (*console)(void *ctx, AuJsLogLevel level, const char *text, size_t len);
    /* A script has run for elapsed_ms. Return true to let it continue. */
    bool (*long_script)(void *ctx, uint32_t elapsed_ms);
    /* The DOM changed under script: the embedder invalidates style and
     * layout and schedules a coalesced update. node is the changed node
     * (an element whose attribute changed, a parent whose children
     * changed, a text node). */
    void (*dom_changed)(void *ctx, AuNode *node, AuAtom attr, int kind);
    /* Brings layout up to date (forced synchronous layout) and returns the
     * border box of el in document coordinates; false if el has no box. */
    bool (*element_rect)(void *ctx, AuElement *el, AuJsRect *out);
    /* Brings style up to date and returns el's computed style (an AuStyle
     * from src/css), or NULL. */
    const void *(*computed_style)(void *ctx, AuElement *el);
    /* The viewport size and the document scroll offset and size. */
    void (*viewport)(void *ctx, int *width, int *height, int *scroll_x, int *scroll_y, int *doc_w, int *doc_h);
    void (*scroll_to)(void *ctx, int x, int y);
    bool (*media_matches)(void *ctx, const char *query, size_t len);
    /* Navigation: location.href, assign, replace, reload, javascript:
     * links aside. The embedder starts it after the current task. */
    void (*navigate)(void *ctx, const char *url, bool replace);
    void (*reload)(void *ctx);
    /* window.open; return false when blocked. */
    bool (*open_window)(void *ctx, const char *url, const char *target);
    /* Dialogs, naming the page origin. prompt returns an au_alloc'd string
     * or NULL for cancel. */
    void (*alert)(void *ctx, const char *origin, const char *message);
    bool (*confirm)(void *ctx, const char *origin, const char *message);
    char *(*prompt)(void *ctx, const char *origin, const char *message, const char *def);
    /* History: length, go(delta), and pushState/replaceState entries (the
     * state object stays in script; the embedder records the URL). */
    int (*history_length)(void *ctx);
    void (*history_go)(void *ctx, int delta);
    void (*history_push)(void *ctx, const char *url, bool replace);
    /* document.cookie: the cookie string for url (au_alloc'd, or NULL), and
     * a cookie set by script. */
    char *(*get_cookie)(void *ctx, const char *url);
    void (*set_cookie)(void *ctx, const char *url, const char *cookie);
    /* localStorage persistence: load returns the serialized area for an
     * origin (au_alloc'd, or NULL); save stores it. sessionStorage is not
     * persisted. */
    char *(*storage_load)(void *ctx, const char *origin, size_t *len);
    void (*storage_save)(void *ctx, const char *origin, const char *data, size_t len);
    /* Focus moved to el (NULL: the body). */
    void (*focus_changed)(void *ctx, AuElement *el);
    /* A form is to be submitted (form.submit(), or requestSubmit and
     * activation after the submit event was not cancelled and validation
     * passed): the embedder builds the entry list and navigates. */
    void (*submit_form)(void *ctx, AuElement *form, AuElement *submitter);
    /* document.open(): the embedder starts a new parser. Optional. */
    AuHtmlParser *(*parser)(void *ctx);
    /* The fetch interface. */
    AuFetcher fetcher;
    /* CSSOM: the style sheet of a style or link element. sheet_text
     * appends the sheet's text and its URL to the buffers and returns 2;
     * 3 (URL only) when the sheet is loaded but its text is not kept; 1
     * when the sheet has not loaded yet, 0 when el has no sheet.
     * sheet_set_text replaces the text the cascade uses for it (insertRule,
     * deleteRule and rule changes; the element's own text stays), and
     * sheet_disable turns it off or on. All optional. */
    int (*sheet_text)(void *ctx, AuElement *el, AuBuf *text, AuBuf *url);
    void (*sheet_set_text)(void *ctx, AuElement *el, const char *text, size_t len);
    void (*sheet_disable)(void *ctx, AuElement *el, bool disabled);
    /* Cryptographically strong random bytes for crypto.getRandomValues
     * and crypto.randomUUID. Returns false when the source cannot give
     * them yet. Without this hook the window has no crypto object. */
    bool (*random)(void *ctx, void *buf, size_t len);
    /* Frames (all optional; without them a window has no frames and is
     * its own parent). frames_changed: a frame element (iframe, frame,
     * object, embed) went into or left the document; the embedder brings
     * its frame list up to date. frame_count and frame_at: the nested
     * documents in tree order, and the element of each. frame_js: the
     * AuJs of the document nested in el, created on demand (NULL: none,
     * or script is off there). parent_js: the AuJs of the embedding
     * document (NULL at the top). frame_element: the element that holds
     * this document, in the parent document. */
    void (*frames_changed)(void *ctx);
    int (*frame_count)(void *ctx);
    AuElement *(*frame_at)(void *ctx, int i);
    AuJs *(*frame_js)(void *ctx, AuElement *el);
    AuJs *(*parent_js)(void *ctx);
    AuElement *(*frame_element)(void *ctx);
} AuJsHooks;

/* Kinds for dom_changed. */
enum { AU_JS_CHANGE_CHILDREN = 1, AU_JS_CHANGE_ATTR, AU_JS_CHANGE_TEXT, AU_JS_CHANGE_STATE,
       AU_JS_CHANGE_VALUE /* a form control's value or selection: repaint only */ };

/* Creates the runtime for doc (referenced until au_js_free). url is the
 * document URL (also its origin and base). Returns NULL if QuickJS cannot
 * start (no memory). */
AuJs *au_js_new(AuDocument *doc, const char *url, const AuJsSettings *settings, const AuJsHooks *hooks);
/* Creates the AuJs of a document nested in a frame: a new context in the
 * runtime of page (any AuJs of the top-level page), so that objects pass
 * between the page's documents. The memory limit, the stack limit and the
 * time budget are the runtime's. Free it before the top-level document's
 * AuJs, and only while no script of the page runs (au_js_running):
 * objects of the freed document that other documents still hold stay
 * valid, and its document lives as long as a wrapper names one of its
 * nodes. */
AuJs *au_js_new_child(AuJs *page, AuDocument *doc, const char *url, const AuJsSettings *settings,
                      const AuJsHooks *hooks);
void au_js_free(AuJs *js);
/* The document's origin as script sees it ("null" for an opaque one). */
const char *au_js_origin(const AuJs *js);
/* Script of the page (any of its documents) is on the stack. */
bool au_js_running(const AuJs *js);
/* The frame list of js's document changed outside script (the parser, a
 * frame that loaded): gives the window its indexed and named frames. */
void au_js_frames_changed(AuJs *js);

/* The parser feeding the document, for document.write and the insertion
 * point (NULL when parsing is done). */
void au_js_set_parser(AuJs *js, AuHtmlParser *p);
/* The document URL and base URL changed (history.pushState, <base>). */
void au_js_set_url(AuJs *js, const char *url);
void au_js_set_base_url(AuJs *js, const char *base);

/* ---- Running scripts ----
 * Runs a classic script now with el as document.currentScript (NULL
 * allowed) and then a microtask checkpoint. url and line name the source
 * in error reports. Returns false if it threw (the error is reported). */
bool au_js_run_classic(AuJs *js, AuElement *el, const char *src, size_t len, const char *url, int line);
/* Runs a module script (inline when src is given, else fetched from url
 * through the fetcher); its dependencies load asynchronously. done is
 * called once the module has evaluated (or failed); it may be NULL. */
void au_js_run_module(AuJs *js, AuElement *el, const char *src, size_t len, const char *url,
                      void (*done)(void *ctx, AuElement *el, bool ok), void *ctx);
/* Evaluates src as a script and appends the result's string form to out
 * (for the host tool and tests). Returns false on an exception. */
bool au_js_eval(AuJs *js, const char *src, size_t len, AuBuf *out);
/* A javascript: URL activated by a link. */
void au_js_run_javascript_url(AuJs *js, const char *url);
/* An import map (<script type="importmap">): its JSON text. */
void au_js_add_import_map(AuJs *js, const char *src, size_t len);
/* What a script element is, from its type and language attributes. */
enum { AU_JS_SCRIPT_NONE, AU_JS_SCRIPT_CLASSIC, AU_JS_SCRIPT_MODULE, AU_JS_SCRIPT_IMPORTMAP };
int au_js_script_kind(AuJs *js, AuElement *el);
/* Converts parsed <template shadowrootmode> elements into shadow roots
 * (call after each parse chunk, and with final at the end). */
void au_js_declarative_shadow(AuJs *js, bool final);
/* The embedder moved through history to an entry made by pushState (url
 * is its URL): popstate. */
void au_js_pop_state(AuJs *js, const char *url);

/* ---- Events from the embedder ---- */
/* Fires a simple event (Event, no detail) at node; window when node is
 * NULL. Returns false if a listener called preventDefault. */
bool au_js_fire(AuJs *js, AuNode *node, const char *type, bool bubbles, bool cancelable);
/* Modifier bits for mouse and keyboard events. */
enum { AU_JS_MOD_SHIFT = 1, AU_JS_MOD_CTRL = 2, AU_JS_MOD_ALT = 4, AU_JS_MOD_META = 8 };
/* Mouse events. type is "mousedown", "click", ...; (x, y) are viewport
 * coordinates, button 0 for the main button, mods AU_JS_MOD_* bits,
 * detail the click count. For "click", activation behavior runs for form
 * controls, labels and summary unless cancelled; following a link is left
 * to the embedder (which sees the return value). Returns false if a
 * listener called preventDefault. */
bool au_js_fire_mouse(AuJs *js, AuElement *target, const char *type, int x, int y, int button, uint32_t mods,
                      int detail, AuElement *related);
/* Keyboard events: key and code as KeyboardEvent names them. */
bool au_js_fire_key(AuJs *js, AuElement *target, const char *type, const char *key, const char *code,
                    uint32_t mods, bool repeat);
/* Forms. The user moved the focus to el (NULL: none): blur, focusout,
 * focus and focusin, and document.activeElement. */
void au_js_focus(AuJs *js, AuElement *el);
/* beforeinput (cancelable) or input (InputEvent with inputType and data,
 * data NULL for none) at el. Returns false if cancelled. */
bool au_js_fire_input(AuJs *js, AuElement *el, const char *type, const char *input_type, const char *data);
/* The submit event at form (SubmitEvent; submitter NULL for none).
 * Returns false if cancelled. */
bool au_js_fire_submit(AuJs *js, AuElement *form, AuElement *submitter);
/* Tests value against an HTML pattern attribute (the whole value, v
 * flag): 1 match, 0 no match, -1 an invalid pattern. */
int au_js_pattern_test(AuJs *js, const char *pattern, const char *value);
/* The document reached a readiness state: "interactive" (parsing is done:
 * readystatechange; deferred scripts run next), "contentloaded"
 * (DOMContentLoaded, after the deferred scripts) or "complete"
 * (readystatechange, load and pageshow). A later state fires the events of
 * the states skipped. */
void au_js_set_ready_state(AuJs *js, const char *state);
/* Before the document is discarded: beforeunload and unload. */
void au_js_unload(AuJs *js);
/* The document cache. Whether window has an unload or beforeunload
 * listener (such a document is discarded, not kept, when the tab leaves
 * it), and the events of a document that is kept and later shown again:
 * pagehide (show false) and pageshow (show true), persisted true. */
bool au_js_has_unload_handler(AuJs *js);
void au_js_page_transition(AuJs *js, bool show);
/* The viewport changed size (resize event, matchMedia listeners). */
void au_js_resized(AuJs *js);
/* The document scrolled (scroll event). */
void au_js_scrolled(AuJs *js);

/* ---- Event loop ----
 * Runs due timers and queued tasks for up to budget_ms, each followed by a
 * microtask checkpoint. Returns true if anything ran. */
bool au_js_run_tasks(AuJs *js, uint32_t budget_ms);
/* The time (now_ms clock) the next timer is due, or -1 when none. */
double au_js_next_timer(const AuJs *js);
/* True when tasks are queued (ready to run now). */
bool au_js_has_tasks(const AuJs *js);
/* True when requestAnimationFrame callbacks wait for a frame. */
bool au_js_wants_frame(const AuJs *js);
/* Runs the animation frame callbacks (call before painting). */
void au_js_run_frame(AuJs *js);
/* Layout was updated: IntersectionObserver and ResizeObserver run. */
void au_js_layout_done(AuJs *js);
/* Runs a microtask checkpoint (promise jobs, mutation observers,
 * custom element reactions). */
void au_js_checkpoint(AuJs *js);
/* Nothing is queued or scheduled: no tasks, timers, frames or pending
 * fetches. */
bool au_js_idle(const AuJs *js);
/* Runs the garbage collector. */
void au_js_gc(AuJs *js);

/* ---- Statistics ---- */
typedef struct AuJsStats {
    size_t memory_used;  /* bytes allocated by the runtime */
    size_t memory_limit;
    uint32_t wrappers;   /* live node wrappers */
    uint32_t errors;     /* uncaught exceptions reported */
    uint32_t interrupted; /* scripts stopped by the time limit */
} AuJsStats;
void au_js_stats(const AuJs *js, AuJsStats *s);

/* The AuJs of a document, or NULL. */
AuJs *au_js_of(const AuDocument *doc);

#endif
