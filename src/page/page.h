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
#include "../html/html.h"
#include "../layout/layout.h"
#include "../platform/platform.h" /* AuEvent: a plain struct */
#include "perf.h"

typedef struct AuDocView AuDocView;

typedef enum { AU_RES_STYLESHEET, AU_RES_IMAGE, AU_RES_OTHER, AU_RES_FONT } AuResourceKind;

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
} AuLoader;

/* A form submission for the embedder to navigate to. */
typedef struct AuFormSubmission {
    const char *url;       /* absolute; for GET the query holds the entry list */
    const char *method;    /* "GET" or "POST" */
    const void *body;      /* POST: the encoded entry list */
    size_t body_len;
    const char *body_type; /* POST: the Content-Type (with the multipart boundary) */
    bool new_tab;          /* target _blank or another name */
} AuFormSubmission;

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
} AuDocViewCallbacks;

AuDocView *au_docview_new(AuFontSet *fonts, const AuLoader *loader, const AuDocViewCallbacks *cb);
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
/* Requests still outstanding. */
uint32_t au_docview_pending(const AuDocView *dv);

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
/* The view is about to be hidden (pagehide) or was shown again
 * (pageshow), both with persisted true. A hidden view drops its decoded
 * image pixels; images decode again from their bytes when next painted. */
void au_docview_set_hidden(AuDocView *dv, bool hidden);
/* An estimate of the memory the document holds, in bytes: the tree, its
 * style sheets, the layout, images (decoded and compressed) and the
 * script runtime. */
size_t au_docview_memory_estimate(AuDocView *dv);

/* ---- Geometry and painting ---- */
void au_docview_set_viewport(AuDocView *dv, int width, int height);
/* Brings style and layout up to date. Returns true if geometry changed. */
bool au_docview_update(AuDocView *dv);
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
 * the result does not fit. */
bool au_url_resolve(const char *base, const char *rel, char *out, size_t cap);

#endif
