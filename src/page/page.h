/* Aura document view: one HTML document from bytes to pixels.
 *
 * An AuDocView owns a document and everything derived from it: the
 * parser (fed incrementally; with scripting off, the default, <noscript>
 * content shows and <script> is ignored; see Scripts), the style sheets (<style>, <link
 * rel=stylesheet>, @import) and images fetched through a loader, the CSS
 * resolver, the layout, hover and active state, and link activation.
 *
 * To show a page:
 *
 *     AuDocView *dv = au_docview_new(fonts, &loader, &callbacks);
 *     au_docview_begin(dv, "file:///path/page.html");
 *     au_docview_feed(dv, bytes, len);         // as often as data arrives
 *     au_docview_finish(dv);
 *     au_docview_set_viewport(dv, 800, 600);
 *     au_docview_paint(dv, canvas, view, scroll_x, scroll_y);
 *
 * Resources. The loader is asynchronous in shape: request() only starts a
 * fetch and returns an id; the bytes arrive in later calls to
 * au_docview_resource_data and au_docview_resource_done (never from inside
 * request()). Style sheets are applied in document order whatever order
 * they arrive in. Images change layout when their size becomes known.
 *
 * Work is lazy: feeding, resources and input mark what is out of date;
 * au_docview_update (called by paint, hit testing and the size queries)
 * re-parses sheets, restyles and relays out only what is needed.
 *
 * Engine rules: no OS calls; no global mutable state. */
#ifndef AURA_PAGE_H
#define AURA_PAGE_H

#include "../dom/control.h"
#include "../core/coro.h"
#include "../html/html.h"
#include "../layout/layout.h"
#include "../platform/platform.h" /* AuEvent: a plain struct */
#include "perf.h"

typedef struct AuDocView AuDocView;

/* AU_RES_FRAME: the document of a frame (iframe, frame, object, embed). */
typedef enum { AU_RES_STYLESHEET, AU_RES_IMAGE, AU_RES_OTHER, AU_RES_FONT, AU_RES_FRAME } AuResourceKind;

/* A request with its context (au_docview_set_request_ex). */
typedef struct AuResourceRequest {
    const char *url;
    AuResourceKind kind;
    /* The URL of the document making the request: a nested document's
     * own URL for its resources, the embedding document's for a frame's
     * document. The loader's dv argument is always the top-level view. */
    const char *document_url;
    /* AU_RES_FRAME: a form submitted into the frame (NULL method: GET). */
    const char *method;
    const void *body;
    size_t body_len;
    const char *body_type;
    /* A request made by script (fetch, XMLHttpRequest, a worker) or a
     * resource fetched with CORS (a crossorigin attribute); zero for the
     * rest. method is then any method. headers: name, value pairs to send
     * as given. script_mode: 0 for a resource of the page, else 1 cors,
     * 2 no-cors, 3 same-origin, 4 a WebSocket handshake. cache: 0 the page's mode, 1 no-store,
     * 2 reload, 3 no-cache (revalidate), 4 force-cache and only-if-cached
     * (any stored copy). referrer: NULL for the document's URL, "" for
     * none, else the URL to send. */
    const char *const *headers;
    size_t nheaders; /* pairs */
    uint8_t script_mode;
    uint8_t cache;
    bool omit_credentials;
    bool manual_redirect; /* deliver a 3xx response instead of following it */
    const char *referrer;
    uint8_t referrer_policy; /* 0: the document's; else 1 no-referrer ... 8 unsafe-url, as AuReferrerPolicy */
    /* A resource of the page fetched with CORS (an element with a
     * crossorigin attribute): 1 anonymous, 2 with credentials. The view
     * checks the response itself. */
    uint8_t cors_resource;
} AuResourceRequest;

typedef struct AuLoader {
    void *ctx;
    /* Starts fetching an absolute URL. Returns a nonzero id, or 0 when the
     * URL cannot be fetched (no data will come). */
    uint32_t (*request)(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind);
    /* Stops a fetch; no more data arrives for id. Optional. */
    void (*cancel)(void *ctx, uint32_t id);
    /* Unused: the view decodes images itself (src/image, through
     * docimages.c). Kept so existing loaders still compile. */
    bool (*decode_image)(void *ctx, const void *data, size_t len, int *w, int *h, AuSurface **pixels);
    /* Backpressure for a script's request: paused, stop reading id's
     * response from the network until a call with paused false. Data
     * already received may still arrive. Optional. */
    void (*pause)(void *ctx, uint32_t id, bool paused);
    /* A script's WebSocket request (script_mode 4) after its response:
     * bytes to send on the connection. False when it cannot. Optional. */
    bool (*send)(void *ctx, uint32_t id, const void *data, size_t len);
} AuLoader;

/* A form submission for the embedder to navigate to. */
typedef struct AuFormSubmission {
    const char *url;       /* absolute; for GET the query holds the entry list */
    const char *method;    /* "GET" or "POST" */
    const void *body;      /* POST: the encoded entry list */
    size_t body_len;
    const char *body_type; /* POST: the Content-Type (with the multipart boundary) */
    bool new_tab;          /* target _blank or another name */
    const char *target;    /* the target attribute (form, submitter or <base>), or NULL */
} AuFormSubmission;

struct AuJsStorageCall;

/* A change to a storage area, for the documents that share it
 * (au_docview_storage_event). key is NULL for clear(), old_value NULL
 * for a new key, new_value NULL for a removal. */
typedef struct AuDocStorageEvent {
    int kind;                /* AU_JS_STORAGE_LOCAL or _SESSION */
    const char *storage_key; /* the area's name in the embedder's service */
    const void *writer;      /* AuJsStorageCall.writer of the document that wrote */
    const char *key, *old_value, *new_value;
    size_t key_len, old_len, new_len;
    const char *url;         /* the writer's */
} AuDocStorageEvent;

typedef struct AuDocViewCallbacks {
    void *ctx;
    void (*title_changed)(void *ctx, AuDocView *dv, const char *title);
    /* Something needs repainting (document coordinates). */
    void (*invalidate)(void *ctx, AuDocView *dv, AuIRect r);
    /* The document size changed. */
    void (*size_changed)(void *ctx, AuDocView *dv);
    /* Status text: a hovered link's URL, or "" to clear. */
    void (*status_text)(void *ctx, AuDocView *dv, const char *text);
    /* The user activated a link: url is absolute; new_tab for
     * Command-click or target=_blank. */
    void (*open_url)(void *ctx, AuDocView *dv, const char *url, bool new_tab);
    /* Parsing finished and no resources are pending. */
    void (*load_finished)(void *ctx, AuDocView *dv);
    /* The following are used only with scripting on; all are optional. */
    /* Monotonic milliseconds (timers, script time limits). Without it the
     * view counts its own clock, which advances by the budget of each
     * au_docview_run_scripts call. */
    double (*now_ms)(void *ctx, AuDocView *dv);
    /* Script asked to scroll the viewport to document point (x, y). */
    void (*scroll_to)(void *ctx, AuDocView *dv, int x, int y);
    /* A console line from script (level: 0 log, 1 info, 2 warn, 3 error,
     * 4 debug). */
    void (*console)(void *ctx, AuDocView *dv, int level, const char *text, size_t len);
    /* Optional. Script stopped out of memory (AuJsHooks.out_of_memory):
     * worker, machine, the limit in KB. Called with script on the stack:
     * note it and show it later. A nested view's goes to the top view's. */
    void (*script_oom)(void *ctx, AuDocView *dv, bool worker, bool machine, uint32_t limit_kb);
    /* Cryptographically strong random bytes for script (crypto
     * .getRandomValues); false when the source is not ready. Optional:
     * without it pages have no crypto object. */
    bool (*random)(void *ctx, AuDocView *dv, void *buf, size_t len);
    /* Coroutines for workers (core/coro.h). Optional: without them a
     * worker runs on the caller's stack, a task at a time. */
    const AuCoroOps *coro;
    /* Optional: called about every script_yield_ms while the page's
     * script runs (AuJsHooks.yield). The embedder that runs
     * au_docview_run_scripts on a coroutine leaves it here, so that the
     * window scrolls and paints while a long task runs; while it is away
     * the view is frozen (au_docview_set_frozen). Return nonzero to stop
     * the script. Only the top-level view's callback is called. */
    int (*script_yield)(void *ctx, AuDocView *dv);
    uint32_t script_yield_ms; /* 0: 100 */
    /* document.cookie of the document at url (the view's own, or a nested
     * document's when nested is set; dv is the top-level view either
     * way): get_cookie returns au_alloc'd text or NULL. Optional: without
     * them document.cookie reads "" and writes are dropped. */
    char *(*get_cookie)(void *ctx, AuDocView *dv, const char *url, bool nested);
    void (*set_cookie)(void *ctx, AuDocView *dv, const char *url, bool nested, const char *value);
    /* localStorage and sessionStorage: one operation (AuJsStorageCall,
     * src/js/js.h) for the document whose origin, URL and, when it is a
     * nested document, top-level URL the call names; dv is the top-level
     * view. Returns an AU_JS_STORAGE_* result. Optional: without it each
     * document keeps storage of its own that ends with it. */
    int (*storage)(void *ctx, AuDocView *dv, struct AuJsStorageCall *call);
    /* Forms (all optional). A form was submitted: navigate as s says
     * (without it, GET submissions go through open_url and POST ones are
     * dropped). */
    void (*submit)(void *ctx, AuDocView *dv, const AuFormSubmission *s);
    /* Shows a select's pop-up menu of n items at document rectangle at
     * (the control), with item selected checked; returns the item chosen
     * or -1 (-2: no menu after all). Without it the view cycles through
     * the options on a click. */
    int (*popup)(void *ctx, AuDocView *dv, const AuPopupItem *items, int n, int selected, AuIRect at);
    /* Asks for files for an input type=file; true with *files (n of them,
     * au_alloc'd, taken over) when the user chose. */
    bool (*choose_file)(void *ctx, AuDocView *dv, bool multiple, AuControlFile **files, uint32_t *n);
    /* Asks the user to confirm (insecure submission); true to go on.
     * Without it the action goes on. */
    bool (*confirm)(void *ctx, AuDocView *dv, const char *message);
    /* A validation message to show (status bar); "" clears it. */
    void (*message)(void *ctx, AuDocView *dv, const char *text);
    /* The history API of the top-level document (all optional; without
     * them history.length is 1 and the rest does nothing beyond the
     * document's URL). history_push: pushState or replaceState changed
     * the URL to url (called with script on the stack); a new entry
     * unless replace, named key (au_docview_pop_state hands it back).
     * history_go: history.go(delta), delta not 0, after the script turn.
     * history_length: the number of entries. */
    void (*history_push)(void *ctx, AuDocView *dv, const char *url, bool replace, uint32_t key);
    void (*history_go)(void *ctx, AuDocView *dv, int delta);
    int (*history_length)(void *ctx, AuDocView *dv);
    /* Optional. Script puts text on the clipboard; false when it may not
     * (no clipboard, a tab in the background). */
    bool (*copy_text)(void *ctx, AuDocView *dv, const char *utf8, size_t len);
} AuDocViewCallbacks;

AuDocView *au_docview_new(AuFontSet *fonts, const AuLoader *loader, const AuDocViewCallbacks *cb);
/* A request function that gets the requesting document and a body
 * (ctx is the loader's). With it set, every request goes through it
 * instead of AuLoader.request; without it a frame's form POST loads with
 * GET. Nested documents load everything through the top-level view's
 * loader, and their data is delivered to the top-level view
 * (au_docview_resource_head, _data, _done), which passes it on. */
typedef uint32_t (*AuLoaderRequestEx)(void *ctx, AuDocView *dv, const AuResourceRequest *rq);
void au_docview_set_request_ex(AuDocView *dv, AuLoaderRequestEx fn);
void au_docview_free(AuDocView *dv);

/* ---- Loading ---- */
/* Starts a new, empty document whose URL (and initial base URL) is url.
 * Pending resource requests of the previous document are cancelled. */
void au_docview_begin(AuDocView *dv, const char *url);
void au_docview_feed(AuDocView *dv, const char *bytes, size_t len);
void au_docview_finish(AuDocView *dv);
/* begin + feed + finish. */
void au_docview_load(AuDocView *dv, const char *bytes, size_t len, const char *url);

/* The response head of request id, before its data (optional). status is
 * the HTTP status (200 for file: and data:), final_url the URL after
 * redirects (NULL: unchanged), headers name/value pairs (lower-case names).
 * Script fetches (fetch, XMLHttpRequest) and script elements use it; a
 * loader that never calls it makes script see status 200, no headers and
 * a content type guessed from the URL for every successful load. */
void au_docview_resource_head(AuDocView *dv, uint32_t id, int status, const char *status_text,
                              const char *final_url, const char *const *headers, size_t nheaders);
void au_docview_resource_data(AuDocView *dv, uint32_t id, const void *data, size_t len);
void au_docview_resource_done(AuDocView *dv, uint32_t id, bool ok);
/* Parsing is not finished or resources are pending. */
bool au_docview_loading(const AuDocView *dv);
/* A style sheet (a <link>'s or an @import's) whose media matches is still
 * being fetched: the page holds its first paint for it (page_doc.c). */
bool au_docview_sheets_pending(const AuDocView *dv);
/* Requests still outstanding. */
uint32_t au_docview_pending(const AuDocView *dv);

/* Whether the image at url (absolute) may be read by the document's
 * script, for a canvas: true when it came from the document's origin (and
 * no redirect left it), from a data: URL, or passed the CORS check because
 * its <img> has a crossorigin attribute. A canvas that draws an image for
 * which this is false becomes tainted: its pixels must not be read back
 * (getImageData, toDataURL, toBlob). An image script made but never put
 * in the document (new Image) is fetched without CORS, whatever its
 * crossOrigin property says. */
bool au_docview_image_origin_clean(AuDocView *dv, const char *url);

/* The embedder moved to another history entry of the shown document, one
 * history_push named key (0: the entry it loaded in): the document's URL
 * becomes url and its script gets popstate. */
void au_docview_pop_state(AuDocView *dv, const char *url, uint32_t key);
const char *au_docview_url(const AuDocView *dv);      /* never NULL */
const char *au_docview_base_url(const AuDocView *dv); /* never NULL */
const char *au_docview_title(const AuDocView *dv);    /* "" when none */
AuDocument *au_docview_document(const AuDocView *dv);
AuLayout *au_docview_layout(const AuDocView *dv);

/* Sets the user style sheet (the reader's defaults: font size, family),
 * cascaded below the author's sheets; NULL removes it. It applies to
 * this and later documents. */
void au_docview_set_user_css(AuDocView *dv, const char *css);
/* Sets the font preferences (default sizes, text zoom, minimum size; see
 * au_css_resolver_set_font_prefs) for this and later documents. A change
 * restyles the document at the next update. */
void au_docview_set_font_prefs(AuDocView *dv, const AuCssFontPrefs *p);
/* Sets the colour scheme the user prefers (the prefers-color-scheme media
 * feature: dark or light) for this document, its frames and later
 * documents. A change restyles at the next update without reloading:
 * sheets deferred for their media query start, <picture> sources are
 * chosen again, and matchMedia lists fire change. */
void au_docview_set_dark(AuDocView *dv, bool dark);
bool au_docview_dark(const AuDocView *dv);

/* ---- Images ----
 * Images decode through src/image: sizes reach layout as soon as each
 * file's header arrives, pixels decode when an image comes within a
 * viewport of the visible area (at the size it is shown), large images
 * fill in as their bytes arrive, and animations advance in
 * au_docview_animate. See src/image/cache.h for the memory policy. */
/* The memory budget for decoded images (0: AU_IMAGE_CACHE_DEFAULT_BUDGET). */
void au_docview_set_image_budget(AuDocView *dv, size_t bytes);
/* A millisecond clock for decode timings (optional). */
void au_docview_set_clock(AuDocView *dv, uint32_t (*now_ms)(void *ctx), void *ctx);
/* Advances animated images to now_ms, invalidating what changes. Returns
 * the milliseconds until the next frame is due (UINT32_MAX: none). */
uint32_t au_docview_animate(AuDocView *dv, uint32_t now_ms);
/* An animation in view is running: call au_docview_animate soon. */
bool au_docview_animating(const AuDocView *dv);
struct AuImageCacheStats;
void au_docview_image_stats(const AuDocView *dv, struct AuImageCacheStats *s);
/* What updates have cost since the view was created: to see what one
 * event costs (a keystroke, a hover change), read the counts before and
 * after it. A keystroke in a text field should add nothing here. */
typedef struct AuDocViewStats {
    uint32_t style_passes;    /* restyles, whole or partial */
    uint32_t styled_elements; /* elements whose style was computed */
    uint32_t tree_builds;     /* box tree builds */
    uint32_t layouts;         /* layout passes */
    uint32_t subtree_builds;  /* box tree rebuilds of a subtree (a DOM change) */
    uint32_t subtree_boxes;   /* the boxes those built */
} AuDocViewStats;
void au_docview_stats(const AuDocView *dv, AuDocViewStats *s);
/* The differential check (for tests; on in a build with
 * -DAURA_INCR_CHECK): after a restyle that followed child or text
 * changes, the document is restyled whole and compared; after a subtree
 * rebuild, the whole box tree is built and laid out beside it and
 * compared (au_layout_set_check). au_docview_check_failures returns the
 * differences found and appends the first to msg (may be NULL). */
void au_docview_set_check(AuDocView *dv, bool on);
uint32_t au_docview_check_failures(const AuDocView *dv, AuBuf *msg);

/* ---- Scripts ----
 * Scripting is off by default: <script> is ignored and <noscript> shows.
 * Turned on, it applies from the next au_docview_begin: scripts run as
 * the parser reaches them (src/js), external scripts load through the
 * loader as AU_RES_OTHER, and javascript: links run. Script navigation
 * arrives through open_url like a link. The embedder calls
 * au_docview_run_scripts from its idle loop (timers, tasks, animation
 * frames); it returns true if anything ran. au_docview_scripts_due says
 * how soon it needs calling again (0: now; UINT32_MAX: no timer or frame
 * is waiting). */
void au_docview_set_scripting(AuDocView *dv, bool on);
/* The memory limits of the script runtime of the page and of each of its
 * workers, in bytes, for runtimes started from now on (set it before
 * au_docview_begin; the top-level view's values serve its frames). 0
 * keeps the engine's defaults (32 MB and 24 MB). */
void au_docview_set_script_memory(AuDocView *dv, size_t page_bytes, size_t worker_bytes);
/* The string script sees as navigator.userAgent, from the next load. It
 * must be the one the network sends. NULL or "": the script default. */
void au_docview_set_user_agent(AuDocView *dv, const char *ua);
/* The string script sees as document.referrer in the view's own document
 * (not its frames'), from the next load: the Referer value the
 * navigation carried. NULL or "": none. */
void au_docview_set_referrer(AuDocView *dv, const char *referrer);
bool au_docview_scripting(const AuDocView *dv);
bool au_docview_run_scripts(AuDocView *dv, uint32_t budget_ms);
uint32_t au_docview_scripts_due(AuDocView *dv);
/* Evaluates src as script in the document (debugging tools); the result,
 * converted to a string, goes in out. False without a runtime or when it
 * throws (out then holds the error). */
bool au_docview_eval(AuDocView *dv, const char *src, AuBuf *out);

/* ---- Measurements (the developer tools) ---- */
/* The document's time counters (perf.h): script, style, layout and paint
 * are timed by the view when the embedder gives a clock (now_ms); the
 * shell adds the screen copy. Cleared by au_docview_begin. */
AuPerf *au_docview_perf(AuDocView *dv);
/* Parsing is over and, with script, DOMContentLoaded has fired. */
bool au_docview_content_loaded(const AuDocView *dv);
/* Bytes the document's script runtime has allocated and its limit; 0, 0
 * without a runtime. */
void au_docview_script_memory(const AuDocView *dv, size_t *used, size_t *limit);
/* Script memory in detail: a line per runtime, document and worker of
 * the page (bytes now, peak, limit), and the peaks alone (the page's
 * runtime; the largest live worker). */
void au_docview_script_summary(const AuDocView *dv, AuBuf *out);
void au_docview_script_peaks(const AuDocView *dv, size_t *page_peak, size_t *worker_peak);

/* ---- Keeping a document while it is not shown ----
 * An embedder that keeps a view alive while another document is shown
 * (the browser's document cache for Back and Forward) calls none of the
 * view's functions meanwhile: timers, animation frames, image animation
 * and the caret then stand still, and resume with the next
 * au_docview_run_scripts and au_docview_animate. */
/* False when the document must be discarded rather than kept: it is
 * still loading, has requests outstanding, or window has an unload or
 * beforeunload listener. */
bool au_docview_can_persist(AuDocView *dv);
/* True when the document holds something a reload would not bring back:
 * a form control the user changed (or chose files for), a canvas that
 * has a bitmap, or a worker; in the document or a nested one. An
 * embedder that frees background documents to save memory (the
 * browser's unloaded tabs) leaves such a document alone. */
bool au_docview_has_state(AuDocView *dv);
/* The view is about to be hidden (pagehide) or was shown again
 * (pageshow), both with persisted true. A hidden view drops its decoded
 * image pixels; images decode again from their bytes when next painted. */
void au_docview_set_hidden(AuDocView *dv, bool hidden);
/* Script in some document changed a storage area: queues the storage
 * event in this view's document and its nested documents, in each one
 * that has that area and is not the writer (the callbacks' storage is
 * asked with AU_JS_STORAGE_MATCH). */
void au_docview_storage_event(AuDocView *dv, const AuDocStorageEvent *ev);
/* An estimate of the memory the document holds, in bytes: the tree, its
 * style sheets, the layout, images (decoded and compressed) and the
 * script runtime. */
size_t au_docview_memory_estimate(AuDocView *dv);
/* Packs the text that one style sheet of the document keeps (deflated;
 * read back when a rule's block is parsed late or the CSSOM reads the
 * sheet); false when none is left to pack. au_docview_update does this
 * for a loaded document, one sheet on each pass that has no other work;
 * call it for a document that is no longer updated (hidden, kept). */
bool au_docview_pack_sheets(AuDocView *dv);

/* ---- Geometry and painting ---- */
void au_docview_set_viewport(AuDocView *dv, int width, int height);
/* Brings style and layout up to date. Returns true if geometry changed. */
bool au_docview_update(AuDocView *dv);
/* While the page's script is parked in the middle of a task
 * (AuDocViewCallbacks.script_yield), the embedder freezes the top-level
 * view: au_docview_update and au_docview_settle do nothing, and
 * au_docview_paint, scroll_at, scroll_damage and content_size work from
 * the last layout. The document may be half changed, so the embedder
 * must not call anything else on the view (or its frames) until it
 * thaws it. Boxes hold their nodes, styles and text, so painting the
 * last layout is safe. */
void au_docview_set_frozen(AuDocView *dv, bool frozen);
bool au_docview_frozen(const AuDocView *dv);
/* The stack a coroutine that runs au_docview_run_scripts needs: the
 * script runtime's C stack limit, and half again for the engine below
 * the script (a forced layout, a parser chunk). */
size_t au_docview_script_stack(void);
/* Script changed a top-level document (au_docview_settle_pending): its
 * turns leave the update to au_docview_settle, which the embedder calls
 * once its script slice is over (and after input). It updates and
 * invalidates what the update changed: nothing for a restyle that
 * computed the same values, the boxes restyled for paint only, or the
 * whole document when anything moved (au_layout_take_damage). */
bool au_docview_settle_pending(const AuDocView *dv);
void au_docview_settle(AuDocView *dv);
void au_docview_content_size(AuDocView *dv, int *width, int *height);
/* Paints the document so that document point (scroll_x, scroll_y) is at
 * view's top-left; paints every pixel of view. */
void au_docview_paint(AuDocView *dv, AuCanvas *c, AuRect view, int scroll_x, int scroll_y);
/* For scrolling by moving pixels: the view scrolled from (ox, oy) to
 * (nx, ny). Brings the layout up to date and writes the viewport areas
 * (whole pixels) to paint again after moving the last paint's pixels by
 * the scroll distance: those of viewport-fixed and sticky boxes (at most
 * max, 32 at most). Returns how many, or -1 when the pixels cannot be
 * moved (layout or paint out of date, a fixed canvas background; see
 * au_layout_scroll_damage). */
int au_docview_scroll_damage(AuDocView *dv, int ox, int oy, int nx, int ny, AuIRect *out, int max);

/* ---- Input ----
 * Mouse events carry document coordinates in ev->x, ev->y; the view uses
 * the scroll offset of the last paint (or au_docview_set_scroll) for
 * positioned-fixed content. Mouse move updates hover state and the status
 * text; a click on a link calls open_url. Returns true when the event did
 * something. */
void au_docview_set_scroll(AuDocView *dv, int scroll_x, int scroll_y);
bool au_docview_mouse(AuDocView *dv, const AuEvent *ev);
/* Scrolls an inner scroll container under document point (x, y). Returns
 * false when the viewport should scroll instead. */
bool au_docview_scroll_at(AuDocView *dv, int x, int y, int dx, int dy);
/* ---- Forms and keyboard focus ----
 * Clicking a focusable element focuses it (a label focuses or toggles its
 * control); Tab and Shift-Tab move through the focusable elements in tab
 * order. Keys go to the focused element: a text field edits its value
 * (src/page/editing.c), Return submits its form, Space toggles a
 * checkbox, and so on; keys the view does not use return false (the
 * browser scrolls). See forms.c. */
bool au_docview_key(AuDocView *dv, const AuEvent *ev);
/* The focused element, or NULL. */
AuElement *au_docview_focus(const AuDocView *dv);
/* Focuses el (NULL: blur), as a click would (focus events run). */
void au_docview_set_focus(AuDocView *dv, AuElement *el);
/* A text field has the focus: the Edit menu acts on it. */
bool au_docview_editing(const AuDocView *dv);
/* True when document point (x, y) is over a form control that takes
 * mouse input itself (text fields, buttons, lists), so the page should
 * not start a text selection there; also while a field drag is going. */
bool au_docview_control_at(AuDocView *dv, int x, int y);
/* Blinks the focused field's caret (now_ms: a millisecond clock),
 * invalidating only the caret. Returns milliseconds until the next
 * blink (UINT32_MAX when no field has the focus). */
uint32_t au_docview_blink(AuDocView *dv, uint32_t now_ms);
/* Edit commands on the focused text field. COPY and CUT append the
 * selected text to out; PASTE inserts text (UTF-8). False when no field
 * has the focus or nothing happened. */
enum { AU_EDITCMD_UNDO, AU_EDITCMD_CUT, AU_EDITCMD_COPY, AU_EDITCMD_PASTE, AU_EDITCMD_CLEAR, AU_EDITCMD_SELECT_ALL };
bool au_docview_edit(AuDocView *dv, int cmd, const char *text, size_t len, AuBuf *out);
/* Whether the focused field can do cmd now (menus): a selection for Cut,
 * Copy and Clear, an undo step for Undo. */
bool au_docview_can_edit(const AuDocView *dv, int cmd);
/* Submits form as if submitter (NULL: implicit) had been activated: the
 * entry list, encoding and the submit callback (and, with script, the
 * submit event). For tests and tools. */
void au_docview_submit(AuDocView *dv, AuElement *form, AuElement *submitter);

/* The hovered link's absolute URL, or "". */
const char *au_docview_hover_link(const AuDocView *dv);
bool au_docview_hit_test(AuDocView *dv, int x, int y, AuHitResult *r);

/* ---- Nested documents ----
 * iframe, frame, object and embed elements show documents of their own:
 * each is an AuDocView owned by the view of the document that holds the
 * element (src/page/frames.c), painted into the element's content box
 * with its own scroll offset. The embedder deals with the top-level view
 * only: nested views load through its loader, report through its
 * callbacks (with the top-level view as dv) and get input through
 * au_docview_mouse, _key and _scroll_at. */
/* The innermost view under document point (*x, *y), and the point in
 * that view's document coordinates; dv itself when no frame is there. */
AuDocView *au_docview_view_at(AuDocView *dv, int *x, int *y);
/* The view that has the keyboard focus: dv, or the nested view of the
 * focused frame. */
AuDocView *au_docview_focus_view(AuDocView *dv);
/* The embedding view (NULL at the top), and r, a rectangle in dv's
 * document coordinates, in the top-level view's (clipped to the frames
 * between them). */
AuDocView *au_docview_parent(const AuDocView *dv);
AuIRect au_docview_rect_to_top(const AuDocView *dv, AuIRect r);
/* Whether v is dv or a nested view somewhere below it (a view kept from
 * an earlier call may have gone with its frame). */
bool au_docview_contains(AuDocView *dv, const AuDocView *v);
/* A point in the top-level view's document coordinates, in dv's. */
void au_docview_point_from_top(const AuDocView *dv, int *x, int *y);
/* The scroll offset the view was last painted with (a nested view's is
 * its frame's). */
void au_docview_scroll_pos(const AuDocView *dv, int *x, int *y);
/* How many nested documents the page has (all levels). */
uint32_t au_docview_frame_count(const AuDocView *dv);

/* ---- Fragments ---- */
/* The element a fragment identifier names (id, or <a name>), or NULL.
 * frag is without the '#'. */
AuElement *au_docview_find_fragment(AuDocView *dv, const char *frag);
/* The document y to scroll to for a fragment, or -1 when not found. */
int au_docview_fragment_y(AuDocView *dv, const char *frag);

/* ---- URLs ----
 * Resolves rel against base into out (cap bytes). A small, limited
 * resolver for the forms pages use (absolute, scheme-relative,
 * path-absolute, path-relative with dot segments, query-only,
 * fragment-only); it does not percent-encode, apply IDNA or validate
 * hosts. A full WHATWG URL parser comes with networking. Returns false if
 * the result does not fit. Inputs of any length up to AU_URL_MAX_INPUT
 * resolve: a data: URL is often longer than a fixed buffer, so code that
 * handles image, font or style sheet URLs uses au_url_resolve_dup. */
bool au_url_resolve(const char *base, const char *rel, char *out, size_t cap);

/* The same resolution into a string from au_alloc (the caller frees it),
 * or NULL when an input is longer than AU_URL_MAX_INPUT. */
char *au_url_resolve_dup(const char *base, const char *rel);

#endif
