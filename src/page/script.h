/* Script execution for the document view (internal to src/page).
 *
 * docview.c owns an AuDocScript while scripting is on and calls the
 * au_docscript_* functions at fixed points (parser callbacks, resources,
 * input, the end of parsing and loading). script.c reaches back into the
 * view only through the au_dv_* functions below, defined in docview.c. */
#ifndef AURA_PAGE_SCRIPT_H
#define AURA_PAGE_SCRIPT_H

#include "page.h"

typedef struct AuDocScript AuDocScript;

/* ---- docview.c to script.c ---- */
/* A new document is starting. The JavaScript runtime is created on first
 * need. set_parser names the document's parser (the parser is created
 * after the AuDocScript, which is its script_ready context). */
AuDocScript *au_docscript_new(AuDocView *dv);
void au_docscript_set_parser(AuDocScript *s, AuHtmlParser *p, AuDocument *doc);
/* Before the document is released (runs unload). */
void au_docscript_free(AuDocScript *s);
/* AuHtmlOptions.script_ready (ctx is the AuDocScript). */
void au_docscript_script_ready(void *ctx, AuHtmlParser *p, AuElement *script);
/* The parser has consumed all input (au_html_parser_done). */
void au_docscript_parse_done(AuDocScript *s);
/* The response head of an AU_RES_OTHER request (headers are name/value
 * pairs; final_url NULL for the request URL). */
/* muted: the response is of another origin and was fetched without CORS
 * (a classic script from it runs with its errors muted). */
void au_docscript_resource_head(AuDocScript *s, uint32_t id, int status, const char *status_text,
                                const char *final_url, const char *const *headers, size_t nheaders, bool muted);
/* A resource of kind AU_RES_OTHER finished. Returns true if it was the
 * script side's request. */
bool au_docscript_resource_done(AuDocScript *s, uint32_t id, AuBuf *body, bool ok);
/* Scripts still hold the load event (a pending or deferred script, or
 * DOMContentLoaded not yet fired). */
bool au_docscript_busy(const AuDocScript *s);
/* Everything has loaded: readyState complete and the load event. */
void au_docscript_loaded(AuDocScript *s);
/* Mouse events at target (NULL: none). type is "mousedown", "mouseup",
 * "mousemove" or "click". Returns false when a listener cancelled it. */
bool au_docscript_mouse(AuDocScript *s, const AuEvent *ev, AuElement *target, const char *type, int vx, int vy);
/* The JavaScript runtime has started (the page has script or an inline
 * handler); before that, events reach no listener and the bindings run
 * no activation behaviour. NULL s is false. */
bool au_docscript_running(const AuDocScript *s);
/* Forms. A simple event at el; false when cancelled. */
bool au_docscript_fire(AuDocScript *s, AuElement *el, const char *type, bool bubbles, bool cancelable);
/* The user moved the focus to el (NULL: blurred): blur and focus events. */
void au_docscript_focus(AuDocScript *s, AuElement *el);
/* keydown, keypress or keyup at target (NULL: the body); false when
 * cancelled. */
bool au_docscript_key(AuDocScript *s, AuElement *target, const char *type, const char *key, const char *code,
                      uint32_t mods);
/* beforeinput (cancelable) or input at el. */
bool au_docscript_input(AuDocScript *s, AuElement *el, const char *type, const char *input_type, const char *data);
/* The submit event (SubmitEvent with submitter); false when cancelled. */
bool au_docscript_submit_event(AuDocScript *s, AuElement *form, AuElement *submitter);
/* Tests value against an HTML pattern attribute with the script engine's
 * RegExp: 1 match, 0 no match, -1 no runtime or a bad pattern. */
int au_docscript_pattern(AuDocScript *s, const char *pattern, const char *value);
/* Hover moved from one element to another: mouseout and mouseover. */
void au_docscript_hover(AuDocScript *s, AuElement *from, AuElement *to, int vx, int vy);
/* A link with a javascript: URL was activated. */
void au_docscript_javascript_url(AuDocScript *s, const char *url);
/* Runs timers, tasks and animation frames for up to budget_ms. */
bool au_docscript_run(AuDocScript *s, uint32_t budget_ms);
/* Evaluates src in the page (tools): the result as a string in out. */
bool au_docscript_eval(AuDocScript *s, const char *src, AuBuf *out);
/* DOMContentLoaded has fired. */
bool au_docscript_content_loaded(const AuDocScript *s);
/* The runtime's allocated bytes and limit (0, 0 before it starts). */
void au_docscript_heap(const AuDocScript *s, size_t *used, size_t *limit);
/* A line per script runtime, document and worker of the page (bytes,
 * peak, limit), and the peaks alone: the page's runtime and the largest
 * worker. For the top-level document's script. */
void au_docscript_memory_summary(const AuDocScript *s, AuBuf *out);
void au_docscript_peaks(const AuDocScript *s, size_t *page_peak, size_t *worker_peak);
/* The view's clock for the time counters (0 without one). */
uint32_t au_dv_perf_now(AuDocView *dv);
/* Milliseconds until script work is due (0: now; UINT32_MAX: none). */
uint32_t au_docscript_due(AuDocScript *s);
/* The viewport changed size or scrolled. */
void au_docscript_resized(AuDocScript *s);
void au_docscript_scrolled(AuDocScript *s);
/* The document cache: false when window has an unload or beforeunload
 * listener; pagehide or pageshow (persisted) when the document is hidden
 * or shown again; the script runtime's memory in bytes. NULL s is fine. */
bool au_docscript_can_persist(AuDocScript *s);
void au_docscript_page_transition(AuDocScript *s, bool show);
size_t au_docscript_memory(const AuDocScript *s);
/* Layout ran: intersection and resize observers. */
void au_docscript_layout_done(AuDocScript *s);
/* Frames (frames.c). on_stack: script of the page is running (nothing may
 * be freed). frames_changed: the document's frame list changed outside
 * script. flush_nav: hands a navigation script asked for to the embedder
 * now (true when there was one); for navigations requested through
 * another document's window. NULL s is fine for all three. */
bool au_docscript_on_stack(const AuDocScript *s);
void au_docscript_frames_changed(AuDocScript *s);
bool au_docscript_flush_nav(AuDocScript *s);

/* ---- script.c to docview.c ---- */
/* Starts an AU_RES_OTHER request. background requests (fetch, XHR) do
 * not hold the load event. Returns 0 when the URL cannot be fetched. */
uint32_t au_dv_request(AuDocView *dv, const char *url, bool background);
/* The same with a method and body (the loader's request_ex is needed). */
uint32_t au_dv_request_ex(AuDocView *dv, const AuResourceRequest *rq, bool background);
/* A script element's source, fetched with the CORS mode of its
 * crossorigin attribute (au_dv_cors_attr: 0 none, 1 anonymous, 2
 * use-credentials); the view fails the load when the CORS check does. */
uint32_t au_dv_request_cors(AuDocView *dv, const char *url, uint8_t cors);
uint8_t au_dv_cors_attr(const AuElement *el);
void au_dv_cancel(AuDocView *dv, uint32_t id);
/* Script changed the DOM: mark style and layout out of date. */
void au_dv_dom_changed(AuDocView *dv, AuNode *n, AuAtom attr, int kind);
/* The parser produced more of the document after a resume. */
void au_dv_parsed_more(AuDocView *dv);
/* Reports load_finished (and the load event) if nothing is pending. */
void au_dv_check_finished(AuDocView *dv);
/* End of a script turn: one repaint request for everything script
 * changed. */
void au_dv_turn_end(AuDocView *dv);
/* Style only (computed style reads) and style plus layout (geometry). */
void au_dv_update_style(AuDocView *dv);
const AuDocViewCallbacks *au_dv_callbacks(const AuDocView *dv);
void au_dv_scroll(const AuDocView *dv, int *x, int *y, int *vw, int *vh);
AuCssResolver *au_dv_resolver(const AuDocView *dv);
/* CSSOM: the sheet of a style or link element (AuJsHooks.sheet_text,
 * sheet_set_text and sheet_disable in src/js/js.h say what they do). */
int au_dv_sheet_text(AuDocView *dv, AuElement *el, AuBuf *text, AuBuf *url);
void au_dv_sheet_set_text(AuDocView *dv, AuElement *el, const char *text, size_t len);
void au_dv_sheet_disable(AuDocView *dv, AuElement *el, bool disabled);
/* Canvases (AuJsHooks.canvas_set and canvas_image in src/js/js.h): the
 * document's canvas set, made on first use, and the decoded pixels of an
 * image element the document loaded. */
struct AuCanvasSet *au_dv_canvas_set(AuDocView *dv);
bool au_dv_canvas_image(AuDocView *dv, AuElement *img, const AuSurface **px, int *w, int *h, char *url,
                        size_t url_cap);
/* The view's clock: the now_ms callback, or a counter. */
double au_dv_now(AuDocView *dv);
/* The user agent set with au_docview_set_user_agent, or NULL. */
const char *au_dv_user_agent(const AuDocView *dv);
/* forms.c: script focused el (NULL: blurred); script submitted form
 * (form.submit, or requestSubmit after its submit event); script changed
 * a control's value (repaint). */
void au_forms_script_focus(AuDocView *dv, AuElement *el);
void au_forms_script_submit(AuDocView *dv, AuElement *form, AuElement *submitter);
void au_forms_changed(AuDocView *dv, AuElement *el);

#endif
