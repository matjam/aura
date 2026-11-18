/* The document view's state, shared by docview.c and forms.c (internal
 * to src/page). */
#ifndef AURA_PAGE_DOCVIEW_INT_H
#define AURA_PAGE_DOCVIEW_INT_H

#include "page.h"
#include "docimages.h"
#include "script.h"
#include "webfont.h"

/* A style sheet source: a <style>, a <link> or an @import. It is parsed
 * once, when its text is there; the parsed sheet stays until the source
 * goes. A source whose media query does not match the device is neither
 * fetched nor parsed until it matches (deferred). */
typedef struct SheetSrc SheetSrc;
struct SheetSrc {
    char *url;    /* base URL for the sheet (absolute) */
    char *media;  /* media attribute or @import media, NULL for all */
    char *text;   /* the text until it is parsed; the parsed sheet takes it (src_text) */
    size_t len;   /* the text's length, also once the sheet has it */
    uint32_t req; /* outstanding request, 0 if none */
    bool failed;
    bool deferred;     /* media does not match: not fetched (a <style>: not parsed) yet */
    AuCssSheet *sheet; /* parsed, or NULL; owned here, an import's by its parent's sheet */
    SheetSrc *parent;  /* the importing sheet, NULL for a <style> or <link> */
    uint32_t index;    /* which @import of the parent */
    uint32_t depth;    /* imports above this one */
    SheetSrc **imports; /* one per @import of the sheet, in order */
    uint32_t nimports;
    /* CSSOM (au_dv_sheet_*): the sheet parsed from the text script wrote
     * back after changing the sheet's rules (it holds that text), which
     * replaces sheet in the cascade (its @import rules are not loaded);
     * and sheet.disabled. */
    AuCssSheet *om_sheet;
    bool disabled;
    uint8_t cors; /* a link's crossorigin attribute: 0 none, 1 anonymous, 2 use-credentials */
    /* A <link>'s load (1) or error (2) event, due once the sheet and its
     * imports have arrived (au_docview_run_scripts fires it); sent: it
     * has been queued, so it is not queued twice. */
    uint8_t event;
    bool event_sent;
};

/* A <style> or <link> element in shadow-including tree order. */
typedef struct SheetSlot {
    AuElement *el; /* referenced */
    SheetSrc *src;
    AuNode *scope; /* the shadow root el is in (referenced), NULL in the document */
} SheetSlot;

/* A constructed style sheet (new CSSStyleSheet in script), by the id
 * script gave it, and the sheets a document or shadow root adopted. */
typedef struct ConstructedSheet {
    uint32_t id;
    AuCssSheet *sheet; /* parsed from the text script gave, or NULL */
    char *media;       /* NULL for all */
    bool disabled;
} ConstructedSheet;

typedef struct AdoptedSheets {
    AuNode *scope; /* the document or a shadow root (referenced) */
    uint32_t *ids;
    uint32_t n;
} AdoptedSheets;

typedef struct AuFrame AuFrame;

typedef struct Request {
    uint32_t id;
    AuResourceKind kind;
    AuFrame *frame;  /* AU_RES_FRAME: the frame whose document this is */
    SheetSrc *sheet; /* for style sheets */
    AuImageEntry *image; /* for images: bytes go straight to the image cache */
    bool font;           /* an @font-face source */
    AuBuf data;
    bool background; /* script fetch: does not hold the load event */
    /* Cross-origin reads (docview.c, "Resources of other origins"). */
    bool script;     /* a script's own request: src/js/fetch.c checks it */
    bool cross;      /* the URL is of another origin than the document */
    uint8_t cors;    /* fetched with CORS: 1 anonymous, 2 with credentials */
    char *img_url;   /* an image's URL, for au_docview_image_origin_clean */
    AuElement *preload; /* a <link rel=preload> (a reference): the bytes only warm the cache */
} Request;

/* A <link rel=preload> whose load or error event is due. */
typedef struct PreloadEvent {
    AuElement *el; /* a reference */
    bool failed;
} PreloadEvent;

struct AuDocView {
    AuFontSet *fonts;
    /* The atom scope of the page (core.h): a top-level view makes one and
     * frees it with itself, so the atoms only its documents used go with
     * them; a nested view uses its top view's (nodes move between the
     * documents of a page). Each entry point enters it (DV_IN). */
    AuAtomScope atoms;
    bool own_atoms;
    AuWebFonts *webfonts; /* the document's @font-face faces, over fonts */
    AuLoader loader;
    AuLoaderRequestEx request_ex; /* au_docview_set_request_ex */
    AuDocViewCallbacks cb;
    AuHtmlParser *parser;
    AuDocument *doc;
    char *url, *base;
    char *title;
    char *user_css; /* the user style sheet, or NULL */
    AuCssFontPrefs font_prefs; /* au_docview_set_font_prefs */
    bool has_font_prefs;
    bool dark; /* prefers-color-scheme: dark (au_docview_set_dark) */
    AuCssResolver *resolver;
    AuLayout *layout;
    SheetSlot *slots;
    uint32_t nslots, slots_cap;
    ConstructedSheet *csheets;
    uint32_t ncsheets, csheets_cap;
    AdoptedSheets *adopted;
    uint32_t nadopted, adopted_cap;
    /* While every sheet change since the last rebuild_resolver was in a
     * shadow root, the roots (referenced; at most DV_MAX_DIRTY_SCOPES):
     * only their hosts' subtrees are restyled. */
#define DV_MAX_DIRTY_SCOPES 1024
    AuNode **dirty_scopes;
    uint32_t ndirty_scopes, dirty_scopes_cap;
    bool sheets_scoped_only;
    AuCssSheet *user_sheet; /* user_css parsed for this document, or NULL */
    uint32_t ndeferred;     /* sheet sources waiting for their media query to match */
    AuDocImages *imgs; /* docimages.c */
    Request *reqs;
    uint32_t nreqs, reqs_cap;
    char **tainted_imgs; /* URLs of images that are not CORS-same-origin */
    uint32_t ntainted;
    int vw, vh;
    int scroll_x, scroll_y;
    bool parsing;
    bool tree_dirty, sheets_dirty, restyle_needed, style_all, images_dirty;
    /* The differential check (au_docview_set_check): struct_changed says
     * children or text changed since the last style pass. */
    bool check, struct_changed;
    uint32_t check_failures;
    AuBuf check_msg;
    bool viewport_restyle; /* the viewport changed: restyle, keeping the box tree where styles allow */
    bool finished_reported;
    float last_w, last_h;
    AuElement *hover, *active;
    char *hover_link; /* the URL of the link under the pointer, from au_alloc; NULL for none (au_dv_clear_hover_link) */
    uint32_t scanned_nodes;
    size_t hidden_image_budget; /* the image budget to return to when shown again (au_docview_set_hidden); 0 while shown */
    bool link_events; /* a <link>'s load or error event is due (SheetSrc.event) */
    bool img_events;  /* an image finished or layout found a known one: <img> load events may be due */
    PreloadEvent *pl_ev; /* preload events due (fired with the link events) */
    uint32_t npl_ev, pl_ev_cap;
    uint32_t npreloads; /* preloads fetched for this document (capped) */
    uint32_t anim_epoch; /* the clock at the document's first animation frame (0: none yet) */
    /* A hidden view frees its layout (au_docview_set_hidden); the scroll
     * offsets of its scroll containers wait here for the next layout. */
    AuLayoutScroll *slim_scrolls;
    uint32_t nslim_scrolls;
    /* scripts (script.c) */
    AuDocScript *script; /* this document's, while scripting is on or script_window is set */
    bool scripting;      /* the setting, applied at begin */
    /* With scripting off: the document still gets a window for other
     * documents' script (a sandboxed frame without allow-scripts), but
     * none of its own script runs (AuJsSettings.scripts_off). */
    bool script_window;
    size_t script_mem, worker_mem; /* script memory limits for runtimes made from now on; 0: the engine's */
    char *user_agent;    /* navigator.userAgent for the next runtime; NULL: the script default */
    char *referrer;      /* document.referrer for the next runtime; NULL: "" */
    bool sheets_rescan;  /* script changed the tree: look for sheets again */
    bool sheets_packed;  /* every sheet's text has been packed (au_docview_pack_sheets) */
    /* When to pack them (au_docview_update): the count of late parses last
     * seen, and since when (or for how many passes, without a clock) it
     * has not moved. */
    uint32_t pack_seen, pack_since, pack_passes;
    bool pack_watching;
    bool layout_ran;     /* layout changed since observers last ran */
    bool in_key;         /* au_docview_key is running: it repaints at its end (au_dv_turn_end) */
    bool settle;         /* script changed it: au_docview_settle updates and repaints what changed */
    AuDocViewStats stats; /* style passes and styled elements; au_docview_stats adds the layout's counts */
    uint32_t dead_builds, dead_layouts; /* counts of layouts replaced by new_layout */
    uint32_t dead_subtree_builds, dead_subtree_boxes;
    uint32_t nbg;        /* background requests among reqs */
    double clock;        /* the fallback clock */
    /* forms (forms.c) */
    AuElement *focus;    /* the focused element (referenced), or NULL */
    bool focus_visible;  /* the focus came from the keyboard */
    bool caret_on;       /* the caret is shown (blink phase) */
    uint32_t blink_ms;   /* when the caret last changed phase */
    AuElement *drag;     /* a text field, list box or slider being dragged (referenced), or NULL */
    int drag_clicks;     /* 1 characters, 2 words, 3 lines */
    uint32_t drag_a, drag_b; /* the word or line the drag started on */
    float goal_x;        /* textarea: the x kept by up and down (-1: none) */
    AuElement *pressed;  /* a button held down (referenced), or NULL */
    bool submitting;     /* a submission is being made (no nesting) */
    /* nested documents (frames.c) */
    AuFrame **frames;    /* the frames of this document's elements, in tree order */
    uint32_t nframes, frames_cap;
    bool frames_dirty;   /* frame elements may have come, gone or changed */
    AuDocView *parent;   /* the view of the embedding document, NULL at the top */
    AuFrame *in_frame;   /* the frame this view is shown in (the parent's), NULL at the top */
    AuFrame *focus_frame; /* the frame that has the keyboard, or NULL */
    AuFrame *mouse_frame; /* the frame a mouse press went to, until the release */
    AuFrame *hover_frame; /* the frame the pointer is over */
    uint32_t sandbox;    /* AU_SANDBOX_* restrictions of this document */
    /* the top view only */
    struct FrameRoute {
        uint32_t id;
        AuDocView *dv;
    } *routes;           /* requests made for nested documents, by loader id */
    uint32_t nroutes, routes_cap;
    uint32_t frame_total; /* frames in the whole page (AU_FRAMES_MAX) */
    AuFrame **dead;      /* frames whose element left its document, freed at the next safe point */
    uint32_t ndead, dead_cap;
    int script_depth;    /* entries into script of any document of the page */
    uint32_t script_runs; /* counts those entries: it moved when script may have run (au_dv_script_runs) */
    bool frames_work;    /* something waits for au_frames_idle */
    bool in_frames_idle;
    uint32_t idle_docs;  /* documents frames began since au_frames_idle started (AU_FRAMES_IDLE_DOCS) */
    AuPerf perf;         /* time counters (perf.h), cleared by au_docview_begin */
    /* canvas elements (docview.c, "Canvases") */
    struct AuCanvasSet *canvases; /* the document's canvas bitmaps, made when script first asks */
    AuFontSet *cur_fonts;         /* the font set the layout draws with */
};

/* ---- frames.c: nested documents ----
 * An AuFrame belongs to the view whose document holds its element. */
enum {
    AU_SANDBOX_SCRIPTS = 1,    /* scripts do not run */
    AU_SANDBOX_FORMS = 2,      /* forms are not submitted */
    AU_SANDBOX_ORIGIN = 4,     /* the document has an origin of its own */
    AU_SANDBOX_TOP_NAV = 8,    /* may navigate only itself and its descendants */
    AU_SANDBOX_POPUPS = 16     /* may not open new tabs */
};
#define AU_FRAMES_MAX 64      /* nested documents per top-level page */
#define AU_FRAMES_DEPTH 8     /* nesting depth */
#define AU_FRAMES_IDLE_DOCS 4 /* documents (about:blank, srcdoc) one au_frames_idle call begins */

AuDocView *au_dv_top(AuDocView *dv);
/* A nested view takes its top view's atom scope (it frees its own). */
void au_dv_use_scope_of(AuDocView *dv, const AuDocView *top);
/* Brings the frame list in step with the document's elements, starts the
 * loads of new frames and notes the ones whose element is gone. Safe
 * while script runs: nothing is freed here. */
void au_frames_sync(AuDocView *dv);
void au_frames_sync_all(AuDocView *dv);
/* The document is going: frees its frames (never while script runs). */
void au_frames_clear(AuDocView *dv);
/* After a layout: the frames' boxes, viewports and lazy loads. */
void au_frames_layout(AuDocView *dv);
/* Deferred work (removed frames, navigations, load events), done when no
 * script of the page runs. Call on any view; it works on the top. */
void au_frames_idle(AuDocView *dv);
/* The response of a frame's document request. */
void au_frames_head(AuDocView *dv, AuFrame *f, int status, const char *final_url, const char *const *headers,
                    size_t nheaders);
void au_frames_data(AuDocView *dv, AuFrame *f, const void *data, size_t len);
void au_frames_done(AuDocView *dv, AuFrame *f, bool ok);
/* A frame of dv is loading its document (holds dv's load event). */
bool au_frames_loading(const AuDocView *dv);
/* Input. Each returns true when a frame took the event (*used then says
 * what the nested view returned). */
bool au_frames_mouse(AuDocView *dv, const AuEvent *ev, bool *used);
bool au_frames_key(AuDocView *dv, const AuEvent *ev, bool *used);
bool au_frames_scroll(AuDocView *dv, int x, int y, int dx, int dy);
/* The keyboard focus moved to an element of dv: its frames lose it. */
void au_frames_blur(AuDocView *dv);
/* Layout callbacks. */
int au_frames_embed_kind(AuDocView *dv, AuElement *el);
void au_frames_paint(AuDocView *dv, AuElement *el, AuCanvas *c, AuRect content);
/* Where a navigation from dv goes for a link or form target (NULL or ""
 * for none): 0 refused (sandbox), 1 the nested view *out, 2 the tab, 3 a
 * new tab. named_new: a name that no frame has opens a new tab (forms)
 * instead of the tab (links). */
int au_frames_route(AuDocView *dv, const char *target, bool new_tab, bool named_new, AuDocView **out);
/* Navigates the frame dv is shown in (s NULL: GET url). */
void au_frames_navigate(AuDocView *dv, const char *url, const AuFormSubmission *s);
/* Script: the frames that are windows, and the view of one (created on
 * demand; NULL when it has none). */
int au_frames_window_count(AuDocView *dv);
AuElement *au_frames_window_at(AuDocView *dv, int i);
AuDocView *au_frames_view_of(AuDocView *dv, AuElement *el);
AuElement *au_frames_element(const AuDocView *dv);
/* Sums and state over the frames, for the view's own functions. */
bool au_frames_can_persist(AuDocView *dv);
bool au_frames_has_state(AuDocView *dv);
void au_frames_set_hidden(AuDocView *dv, bool hidden);
void au_frames_storage_event(AuDocView *dv, const AuDocStorageEvent *ev);
size_t au_frames_memory(AuDocView *dv);
bool au_frames_run_scripts(AuDocView *dv, uint32_t budget_ms);
uint32_t au_frames_scripts_due(AuDocView *dv);
uint32_t au_frames_animate(AuDocView *dv, uint32_t now_ms);
/* Settles the nested documents whose script changed them
 * (au_docview_settle): each repaints only what changed. */
void au_frames_settle(AuDocView *dv);
bool au_frames_animating(const AuDocView *dv);
void au_frames_settings_changed(AuDocView *dv);

/* docview.c, for frames.c */
/* Starts a request for dv (a nested view's goes through the top view's
 * loader); f names the frame for AU_RES_FRAME. 0 when it cannot start. */
uint32_t au_dv_start_request(AuDocView *dv, const AuResourceRequest *rq, AuFrame *f);
void au_dv_drop_routes(AuDocView *top, AuDocView *dv);
/* The document's URL changed without a load (pushState, a history move
 * within it); the base URL follows unless a <base> set it. */
void au_dv_set_url(AuDocView *dv, const char *url);
void au_dv_mouse_leave(AuDocView *dv);
void au_dv_set_hover(AuDocView *dv, AuElement *el);
void au_dv_clear_hover_link(AuDocView *dv);
void au_dv_tree_dirty(AuDocView *dv);
/* The constructed sheet with id, or NULL. */
ConstructedSheet *au_dv_constructed(AuDocView *dv, uint32_t id);


/* ---- Pointers across script ----
 * Any call that can run script (an event, focus or blur, input or change,
 * click activation, a submission, a javascript: URL, timers, the load
 * event, an embedder callback that shows a menu or a dialog) can remove a
 * node from the document, free it, move it to another document, replace
 * the document or rebuild the layout. Code that uses an element after
 * such a call must hold a reference across it (au_node_ref before,
 * au_node_unref after), ask au_dv_alive before acting on the element
 * again, and look boxes and hit results up again instead of keeping them.
 * docs/conventions.md states the rule. */

/* True while el is connected to the document dv shows. el must be a
 * valid pointer: one the caller holds a reference to. */
static inline bool au_dv_alive(const AuDocView *dv, const AuElement *el)
{
    return dv->doc && el && el->node.doc == dv->doc && au_node_is_connected(&el->node);
}

/* A count of entries into script of any document of the page. When it
 * has not moved across a call, no script ran in it. */
static inline uint32_t au_dv_script_runs(AuDocView *dv)
{
    while (dv->parent)
        dv = dv->parent;
    return dv->script_runs;
}

/* forms.c */
/* Forgets the focus and drag state (the document is going). */
void au_forms_clear(AuDocView *dv);
void au_forms_autofocus(AuDocView *dv);
/* Mouse handling for form controls; returns true when the event was
 * used by a control (no link or selection then). Called before the
 * view's own handling, after script mouse events. */
bool au_forms_mouse(AuDocView *dv, const AuEvent *ev, const AuHitResult *r);
/* The activation behaviour of a click on target without script (with
 * script, the bindings run it). */
void au_forms_activate(AuDocView *dv, AuElement *target, int x, int y);
void au_dv_invalidate_element(AuDocView *dv, AuElement *el);
/* Invalidates a document rectangle that belongs to el's box (twice for a
 * fixed element, as au_dv_invalidate_element does). */
void au_dv_invalidate_element_rect(AuDocView *dv, AuElement *el, AuIRect r);
void au_dv_invalidate_all(AuDocView *dv);
/* A style sheet of the document (a <link>, or an @import of a <link> or
 * <style>) is still loading: deferred and module scripts wait for it (the
 * HTML Standard's script-blocking style sheets), so they read geometry
 * with the page's styles. */
bool au_dv_sheets_block_scripts(const AuDocView *dv);
/* Invalidates what the layout's updates changed since the last call
 * (au_layout_take_damage). */
void au_dv_invalidate_damage(AuDocView *dv);

/* An entry point of the view: what it interns belongs to the page's atom
 * scope. DV_ENTRY(ret, name, params, args) defines the public function
 * as a call of name##_in, the body that follows, inside the scope. */
#define DV_IN(dv) AuAtomBits dv_scope_prev_ = au_atom_scope_enter((dv)->atoms)
#define DV_OUT() au_atom_scope_leave(dv_scope_prev_)
#define DV_ENTRY(ret, name, params, args)                                                                              \
    static ret name##_in params;                                                                                       \
    ret au_docview_##name params                                                                                       \
    {                                                                                                                  \
        DV_IN(dv);                                                                                                     \
        ret r_ = name##_in args;                                                                                       \
        DV_OUT();                                                                                                      \
        return r_;                                                                                                     \
    }                                                                                                                  \
    static ret name##_in params
#define DV_ENTRY_VOID(name, params, args)                                                                              \
    static void name##_in params;                                                                                      \
    void au_docview_##name params                                                                                      \
    {                                                                                                                  \
        DV_IN(dv);                                                                                                     \
        name##_in args;                                                                                                \
        DV_OUT();                                                                                                      \
    }                                                                                                                  \
    static void name##_in params

#endif
