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
/* A resource of kind AU_RES_OTHER finished. Returns true if it was the
 * script side's request. */
bool au_docscript_resource_done(AuDocScript *s, uint32_t id, const void *data, size_t len, bool ok);
/* Scripts still hold the load event (a pending or deferred script, or
 * DOMContentLoaded not yet fired). */
bool au_docscript_busy(const AuDocScript *s);
/* Everything has loaded: readyState complete and the load event. */
void au_docscript_loaded(AuDocScript *s);
/* Mouse events at target (NULL: none). type is "mousedown", "mouseup",
 * "mousemove" or "click". Returns false when a listener cancelled it. */
bool au_docscript_mouse(AuDocScript *s, const AuEvent *ev, AuElement *target, const char *type, int vx, int vy);
/* Hover moved from one element to another: mouseout and mouseover. */
void au_docscript_hover(AuDocScript *s, AuElement *from, AuElement *to, int vx, int vy);
/* A link with a javascript: URL was activated. */
void au_docscript_javascript_url(AuDocScript *s, const char *url);
/* Runs timers, tasks and animation frames for up to budget_ms. */
bool au_docscript_run(AuDocScript *s, uint32_t budget_ms);
/* The viewport changed size or scrolled. */
void au_docscript_resized(AuDocScript *s);
void au_docscript_scrolled(AuDocScript *s);
/* Layout ran: intersection and resize observers. */
void au_docscript_layout_done(AuDocScript *s);

/* ---- script.c to docview.c ---- */
/* Starts an AU_RES_OTHER request. background requests (fetch, XHR) do
 * not hold the load event. Returns 0 when the URL cannot be fetched. */
uint32_t au_dv_request(AuDocView *dv, const char *url, bool background);
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
/* The view's clock: the now_ms callback, or a counter. */
double au_dv_now(AuDocView *dv);

#endif
