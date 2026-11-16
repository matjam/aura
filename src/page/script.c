/* Script execution for the document view: the HTML Standard's script
 * processing model on top of src/js.
 *
 * - Parser-inserted classic scripts run when the parser reaches their end
 *   tag. An external one without async or defer blocks the parser: the
 *   parser pauses, the script loads through the view's loader
 *   (AU_RES_OTHER), runs, and parsing resumes (document.write inserts at
 *   the parser's insertion point meanwhile).
 * - defer scripts and non-async module scripts run in document order once
 *   parsing is done, then DOMContentLoaded fires; async scripts run as
 *   they arrive. All of them hold the load event, which fires once the
 *   view has nothing left to load.
 * - nomodule classic scripts are skipped (modules are supported); import
 *   maps are handed to src/js; scripts with an unknown type are ignored.
 * - The runtime starts on the first script, or at the end of parsing when
 *   an element has an event handler attribute (handlers compile lazily on
 *   first dispatch, in src/js) or a popovertarget attribute.
 * - Script DOM changes only mark style and layout out of date
 *   (au_dv_dom_changed); one repaint request goes out at the end of each
 *   script turn and layout runs at the next paint, or earlier when script
 *   reads geometry (element_rect) or computed style (style only).
 * - Navigation requested by script reaches the embedder through open_url
 *   at the end of the turn, never from inside script. */
#include "script.h"

#include "docview_int.h"

#include "../js/js.h"
#include "../net/cors.h"
#include "../layout/selection.h"

#include <stdio.h>
#include <string.h>

typedef struct PendingScript PendingScript;
struct PendingScript {
    AuElement *el; /* referenced */
    char *url;     /* absolute source URL (external), else NULL */
    char *text;    /* source once known */
    size_t len;
    uint32_t req;  /* loader request while loading */
    int kind;      /* AU_JS_SCRIPT_CLASSIC or AU_JS_SCRIPT_MODULE */
    bool ready, failed;
    bool bad_status; /* the response head had a status outside 200-299 */
    bool muted;      /* from another origin without CORS: its errors are muted */
    PendingScript *next;
};

struct AuDocScript {
    AuDocView *dv;
    AuDocument *doc;
    AuHtmlParser *parser;
    AuJs *js;
    bool js_failed;
    PendingScript *blocking;           /* the script the parser waits for */
    PendingScript *defer, *defer_tail; /* defer and module scripts, in order */
    PendingScript *async;              /* async classic scripts, any order */
    struct ScriptFetch {
        uint32_t id;      /* loader id */
        const char *type; /* content type guessed from the URL, or NULL */
        bool head_sent;   /* the loader reported the response head */
        AuJs *js;         /* who asked: the document's runtime or a worker's */
    } *fetches;           /* fetches made by script */
    uint32_t nfetches, fetches_cap;
    uint32_t modules;                  /* module scripts started, not evaluated */
    bool defer_module_running;         /* the head of the defer list is a module in flight */
    bool parse_done, content_loaded, loaded;
    bool resize_pending, scroll_pending;
    int depth;
    uint32_t perf_t0;    /* when the outermost entry began */
    char perf_label[72]; /* the script it ran, for the slow task list */
    char *nav_url;
    bool nav_new_tab;
};

/* ---- Helpers ---- */

static const char *attr(const AuElement *el, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr(el, a) : NULL;
}

static char *script_text(const AuElement *el, size_t *len)
{
    AuBuf b;
    au_buf_init(&b);
    for (const AuNode *c = el->node.first_child; c; c = c->next)
        if (c->type == AU_NODE_TEXT)
            au_buf_append(&b, ((const AuText *)c)->data, ((const AuText *)c)->len);
    au_buf_cstr(&b);
    return au_buf_take(&b, len);
}

static void free_pending(PendingScript *p)
{
    if (!p)
        return;
    au_node_unref(&p->el->node);
    au_free(p->url);
    au_free(p->text);
    au_free(p);
}

static void free_list(PendingScript *p)
{
    while (p) {
        PendingScript *n = p->next;
        free_pending(p);
        p = n;
    }
}

/* ---- Hooks for src/js ---- */

static double h_now(void *ctx)
{
    AuDocScript *s = ctx;
    return au_dv_now(s->dv);
}

static void h_console(void *ctx, AuJsLogLevel level, const char *text, size_t len)
{
    AuDocScript *s = ctx;
    const AuDocViewCallbacks *cb = au_dv_callbacks(s->dv);
    if (cb->console)
        cb->console(cb->ctx, s->dv, (int)level, text, len);
}

static void h_dom_changed(void *ctx, AuNode *node, AuAtom attr_name, int kind)
{
    AuDocScript *s = ctx;
    if (kind == AU_JS_CHANGE_VALUE) {
        if (au_is_element(node))
            au_forms_changed(s->dv, au_as_element(node));
        return;
    }
    /* A change in a subtree that is not in the document (a fragment or
     * element a script is still building, a template's content) changes
     * nothing on screen. Inserting or removing it reports its connected
     * parent. Without this, scripts that build detached trees restyled
     * and laid out the whole page for each attribute they set. */
    if (au_node_shadow_including_root(node) != &s->doc->node)
        return;
    au_dv_dom_changed(s->dv, node, attr_name, kind);
}

static void h_child_changed(void *ctx, AuNode *parent, AuNode *child, AuNode *prev, AuNode *next, bool inserted)
{
    AuDocScript *s = ctx;
    if (au_node_shadow_including_root(parent) != &s->doc->node)
        return; /* as h_dom_changed: a subtree not in the document */
    au_dv_child_changed(s->dv, parent, child, prev, next, inserted);
}

static bool h_random(void *ctx, void *buf, size_t len)
{
    AuDocScript *s = ctx;
    const AuDocViewCallbacks *cb = au_dv_callbacks(s->dv);
    return cb->random && cb->random(cb->ctx, s->dv, buf, len);
}

/* document.cookie. A document without a network URL of its own
 * (about:blank, srcdoc) has its parent's origin and so its cookies. */
static const char *cookie_url(AuDocScript *s)
{
    AuDocView *dv = s->dv;
    for (;;) {
        const char *u = au_docview_url(dv);
        if (!dv->parent || (strncmp(u, "about:", 6) != 0 && strncmp(u, "data:", 5) != 0))
            return u;
        dv = dv->parent;
    }
}

static char *h_get_cookie(void *ctx, const char *url)
{
    AuDocScript *s = ctx;
    const AuDocViewCallbacks *cb = au_dv_callbacks(s->dv);
    (void)url;
    if (!cb->get_cookie)
        return NULL;
    return cb->get_cookie(cb->ctx, au_dv_top(s->dv), cookie_url(s), s->dv->parent != NULL);
}

static void h_set_cookie(void *ctx, const char *url, const char *value)
{
    AuDocScript *s = ctx;
    const AuDocViewCallbacks *cb = au_dv_callbacks(s->dv);
    (void)url;
    if (cb->set_cookie)
        cb->set_cookie(cb->ctx, au_dv_top(s->dv), cookie_url(s), s->dv->parent != NULL, value);
}

/* localStorage and sessionStorage: the embedder's service, told which
 * document asks (the view names the writer of a change) and, for a
 * nested document, the top-level document's URL, which decides whether
 * it is in a third-party context. */
static int h_storage(void *ctx, AuJsStorageCall *c)
{
    AuDocScript *s = ctx;
    const AuDocViewCallbacks *cb = au_dv_callbacks(s->dv);
    AuDocView *top = au_dv_top(s->dv);
    c->writer = s->dv;
    c->top_url = s->dv->parent ? au_docview_url(top) : NULL;
    return cb->storage ? cb->storage(cb->ctx, top, c) : AU_JS_STORAGE_DENIED;
}

static int h_sheet_text(void *ctx, AuElement *el, AuBuf *text, AuBuf *url)
{
    AuDocScript *s = ctx;
    return au_dv_sheet_text(s->dv, el, text, url);
}

static void h_sheet_set_text(void *ctx, AuElement *el, const char *text, size_t len)
{
    AuDocScript *s = ctx;
    au_dv_sheet_set_text(s->dv, el, text, len);
}

static void h_sheet_disable(void *ctx, AuElement *el, bool disabled)
{
    AuDocScript *s = ctx;
    au_dv_sheet_disable(s->dv, el, disabled);
}

static void h_focus_changed(void *ctx, AuElement *el)
{
    AuDocScript *s = ctx;
    au_forms_script_focus(s->dv, el);
}

static void h_submit_form(void *ctx, AuElement *form, AuElement *submitter)
{
    AuDocScript *s = ctx;
    au_forms_script_submit(s->dv, form, submitter);
}

static bool h_element_rect(void *ctx, AuElement *el, AuJsRect *out)
{
    AuDocScript *s = ctx;
    au_docview_update(s->dv); /* forced layout: script reads geometry */
    AuRect r;
    if (!au_layout_element_rect(au_docview_layout(s->dv), el, &r))
        return false;
    out->x = r.x;
    out->y = r.y;
    out->w = r.w;
    out->h = r.h;
    return true;
}

static int h_text_rects(void *ctx, AuNode *text, uint32_t start, uint32_t end, AuJsRect *out, int max)
{
    AuDocScript *s = ctx;
    au_docview_update(s->dv); /* forced layout: script reads geometry */
    AuLayout *L = au_docview_layout(s->dv);
    AuTextPos a, b;
    if (!au_layout_text_pos_from_dom(L, text, start, &a) || !au_layout_text_pos_from_dom(L, text, end, &b))
        return -1;
    AuTextRange r = au_text_range_make(a, b);
    AuRect rc[16];
    AuRect *buf = max > 16 ? au_alloc((size_t)max * sizeof *buf) : rc;
    int n = au_layout_range_rects(L, &r, buf, max);
    for (int i = 0; i < n && i < max; i++)
        out[i] = (AuJsRect){buf[i].x, buf[i].y, buf[i].w, buf[i].h};
    if (buf != rc)
        au_free(buf);
    return n;
}

static const void *h_computed_style(void *ctx, AuElement *el)
{
    AuDocScript *s = ctx;
    au_dv_update_style(s->dv);
    return au_element_style(el);
}

static void h_viewport(void *ctx, int *width, int *height, int *scroll_x, int *scroll_y, int *doc_w, int *doc_h)
{
    AuDocScript *s = ctx;
    au_dv_scroll(s->dv, scroll_x, scroll_y, width, height);
    if (doc_w && doc_h)
        au_docview_content_size(s->dv, doc_w, doc_h);
}

static void h_scroll_to(void *ctx, int x, int y)
{
    AuDocScript *s = ctx;
    const AuDocViewCallbacks *cb = au_dv_callbacks(s->dv);
    if (cb->scroll_to)
        cb->scroll_to(cb->ctx, s->dv, x, y);
}

static bool h_media_matches(void *ctx, const char *query, size_t len)
{
    AuDocScript *s = ctx;
    AuCssResolver *r = au_dv_resolver(s->dv);
    return r && au_css_media_matches(r, query, len);
}

static void request_nav(AuDocScript *s, const char *url, bool new_tab)
{
    au_free(s->nav_url);
    s->nav_url = au_strndup(url, strlen(url));
    s->nav_new_tab = new_tab;
    /* When another document's script asked (through this window), no
     * turn of this document ends: au_frames_idle hands it over. */
    au_dv_top(s->dv)->frames_work = true;
}

static void h_navigate(void *ctx, const char *url, bool replace)
{
    (void)replace; /* the browser's history decides; open_url has no replace flag */
    request_nav(ctx, url, false);
}

static void h_reload(void *ctx)
{
    AuDocScript *s = ctx;
    request_nav(s, au_docview_url(s->dv), false);
}

static bool h_open_window(void *ctx, const char *url, const char *target)
{
    AuDocScript *s = ctx;
    bool self = target && (!strcmp(target, "_self") || !strcmp(target, "_top") || !strcmp(target, "_parent"));
    request_nav(s, url, !self);
    return true;
}

/* Dialogs have no interface in the view yet: they go to the console and
 * take their default answers. */
static void dialog_log(AuDocScript *s, const char *what, const char *message)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_printf(&b, "%s: %s", what, message ? message : "");
    h_console(s, AU_JS_INFO, b.data, b.len);
    au_buf_free(&b);
}

static void h_alert(void *ctx, const char *origin, const char *message)
{
    (void)origin;
    dialog_log(ctx, "alert", message);
}

static bool h_confirm(void *ctx, const char *origin, const char *message)
{
    (void)origin;
    dialog_log(ctx, "confirm", message);
    return false;
}

static char *h_prompt(void *ctx, const char *origin, const char *message, const char *def)
{
    (void)origin;
    (void)def;
    dialog_log(ctx, "prompt", message);
    return NULL;
}

/* ---- Frames ---- */

static bool ensure_js(AuDocScript *s);

static struct AuCanvasSet *h_canvas_set(void *ctx)
{
    AuDocScript *s = ctx;
    return au_dv_canvas_set(s->dv);
}

static bool h_image_origin_clean(void *ctx, const char *url)
{
    AuDocScript *s = ctx;
    return au_docview_image_origin_clean(s->dv, url);
}

static bool h_canvas_image(void *ctx, AuElement *img, const struct AuSurface **px, int *w, int *h, char *url,
                           size_t url_cap)
{
    AuDocScript *s = ctx;
    return au_dv_canvas_image(s->dv, img, px, w, h, url, url_cap);
}

static void h_frames_changed(void *ctx)
{
    AuDocScript *s = ctx;
    s->dv->frames_dirty = true;
    au_frames_sync(s->dv);
}

static int h_frame_count(void *ctx)
{
    AuDocScript *s = ctx;
    return au_frames_window_count(s->dv);
}

static AuElement *h_frame_at(void *ctx, int i)
{
    AuDocScript *s = ctx;
    return au_frames_window_at(s->dv, i);
}

/* The script context of the document nested in el, started on demand (a
 * frame whose document has no script of its own still has a window). */
static AuJs *h_frame_js(void *ctx, AuElement *el)
{
    AuDocScript *s = ctx;
    AuDocView *c = au_frames_view_of(s->dv, el);
    if (!c || !c->script || !ensure_js(c->script))
        return NULL;
    return c->script->js;
}

static AuJs *h_parent_js(void *ctx)
{
    AuDocScript *s = ctx;
    AuDocView *p = s->dv->parent;
    if (!p || !p->script || !ensure_js(p->script))
        return NULL;
    return p->script->js;
}

static AuElement *h_frame_element(void *ctx)
{
    AuDocScript *s = ctx;
    return au_frames_element(s->dv);
}

/* How long the workers of a document run in one au_docscript_run, after
 * the document's own tasks. */
#define WORKER_BUDGET_MS 12

/* ---- Fetches made by script (fetch, XHR, module and script loads) ---- */

/* The loader reports no headers: the type comes from the URL's suffix. */
static const char *type_of(const char *url)
{
    static const char *const map[] = {".js", "text/javascript", ".mjs", "text/javascript", ".json",
                                      "application/json", ".html", "text/html", ".htm", "text/html",
                                      ".css", "text/css", ".txt", "text/plain", ".svg", "image/svg+xml",
                                      ".xml", "application/xml"};
    size_t n = strcspn(url, "?#");
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i += 2) {
        size_t k = strlen(map[i]);
        if (n >= k && au_ieq(url + n - k, k, map[i], k))
            return map[i + 1];
    }
    return NULL;
}

static uint32_t f_start(void *ctx, AuJs *js, const AuFetchRequest *req)
{
    AuDocScript *s = ctx;
    /* One request of a script's fetch (src/js/fetch.c follows redirects
     * and applies CORS). A loader that takes a request with its context
     * (au_docview_set_request_ex) sends it as described; without one only
     * a plain GET can go out. */
    bool get = !strcmp(req->method, "GET");
    uint32_t id;
    bool bg = req->dest == AU_FETCH_DEST_EMPTY || req->dest == AU_FETCH_DEST_WORKER;
    if (au_dv_top(s->dv)->request_ex) {
        AuResourceRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = req->url;
        rq.kind = AU_RES_OTHER;
        rq.document_url = au_docview_url(s->dv);
        rq.method = req->method;
        rq.body = req->body;
        rq.body_len = req->body_len;
        for (size_t i = 0; i < req->nheaders; i++)
            if (au_ieq(req->headers[2 * i], strlen(req->headers[2 * i]), "content-type", 12))
                rq.body_type = req->headers[2 * i + 1];
        rq.headers = req->headers;
        rq.nheaders = req->nheaders;
        rq.script_mode = req->mode == AU_FETCH_NO_CORS ? 2 : req->mode == AU_FETCH_SAME_ORIGIN_MODE ? 3 : 1;
        rq.cache = req->cache == AU_FETCH_CACHE_ONLY_IF_CACHED ? AU_FETCH_CACHE_FORCE : req->cache;
        rq.omit_credentials = req->credentials != AU_FETCH_INCLUDE;
        rq.manual_redirect = req->manual_redirect;
        rq.referrer = req->referrer;
        rq.referrer_policy = req->referrer_policy;
        id = au_dv_request_ex(s->dv, &rq, bg);
    } else {
        if (!get || req->body_len)
            return 0;
        id = au_dv_request(s->dv, req->url, bg);
    }
    if (!id)
        return 0;
    if (s->nfetches == s->fetches_cap) {
        s->fetches_cap = s->fetches_cap ? s->fetches_cap * 2 : 8;
        s->fetches = au_realloc(s->fetches, s->fetches_cap * sizeof *s->fetches);
    }
    s->fetches[s->nfetches].id = id;
    s->fetches[s->nfetches].type = type_of(req->url);
    s->fetches[s->nfetches].head_sent = false;
    s->fetches[s->nfetches].js = js;
    s->nfetches++;
    return id;
}

static struct ScriptFetch *find_fetch(AuDocScript *s, uint32_t id)
{
    for (uint32_t i = 0; i < s->nfetches; i++)
        if (s->fetches[i].id == id)
            return &s->fetches[i];
    return NULL;
}

/* Removes a script fetch; returns false if id is not one. */
static bool take_fetch(AuDocScript *s, uint32_t id, const char **type, bool *head_sent, AuJs **js)
{
    struct ScriptFetch *f = find_fetch(s, id);
    if (!f)
        return false;
    if (js)
        *js = f->js;
    if (type)
        *type = f->type;
    if (head_sent)
        *head_sent = f->head_sent;
    *f = s->fetches[--s->nfetches];
    return true;
}

static void f_abort(void *ctx, uint32_t id)
{
    AuDocScript *s = ctx;
    if (take_fetch(s, id, NULL, NULL, NULL))
        au_dv_cancel(s->dv, id);
}

static void f_pause(void *ctx, uint32_t id, bool paused)
{
    AuDocScript *s = ctx;
    if (find_fetch(s, id))
        au_dv_pause(s->dv, id, paused);
}

/* A script element's kind from its type and language attributes, as the
 * standard decides it (src/js has the same rule; this one needs no
 * runtime, so a page whose only scripts are data blocks starts none). */
static int script_kind(const AuElement *el)
{
    static const char *const mimes[] = {"application/ecmascript", "application/javascript",
        "application/x-ecmascript", "application/x-javascript", "text/ecmascript", "text/javascript",
        "text/javascript1.0", "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
        "text/javascript1.4", "text/javascript1.5", "text/jscript", "text/livescript", "text/x-ecmascript",
        "text/x-javascript"};
    const char *t = attr(el, "type");
    char buf[64];
    if (!t || !*t) {
        const char *lang = attr(el, "language");
        if (t || !lang || !*lang)
            return AU_JS_SCRIPT_CLASSIC;
        if (strlen(lang) > 40)
            return AU_JS_SCRIPT_NONE;
        strcpy(buf, "text/");
        strcat(buf, lang);
        t = buf;
    }
    while (au_is_space((unsigned char)*t))
        t++;
    size_t n = strlen(t);
    while (n && au_is_space((unsigned char)t[n - 1]))
        n--;
    for (size_t i = 0; i < sizeof mimes / sizeof mimes[0]; i++)
        if (au_ieq(t, n, mimes[i], strlen(mimes[i])))
            return AU_JS_SCRIPT_CLASSIC;
    if (au_ieq(t, n, "module", 6))
        return AU_JS_SCRIPT_MODULE;
    if (au_ieq(t, n, "importmap", 9))
        return AU_JS_SCRIPT_IMPORTMAP;
    return AU_JS_SCRIPT_NONE;
}

/* ---- The runtime ---- */

static bool ensure_js(AuDocScript *s)
{
    if (s->js || s->js_failed)
        return s->js != NULL;
    AuJsSettings set;
    au_js_settings_default(&set);
    if (au_dv_user_agent(s->dv))
        set.user_agent = au_dv_user_agent(s->dv);
    set.referrer = au_dv_referrer(s->dv);
    /* The limits the embedder set for the page (au_docview_set_script_memory);
     * a nested document's context lives in the page's runtime. */
    {
        AuDocView *topv = au_dv_top(s->dv);
        if (topv->script_mem)
            set.memory_limit = topv->script_mem;
        if (topv->worker_mem)
            set.worker_memory_limit = topv->worker_mem;
    }
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.ctx = s;
    h.now_ms = h_now;
    h.console = h_console;
    h.dom_changed = h_dom_changed;
    h.child_changed = h_child_changed;
    h.element_rect = h_element_rect;
    h.text_rects = h_text_rects;
    h.computed_style = h_computed_style;
    h.viewport = h_viewport;
    h.scroll_to = h_scroll_to;
    h.media_matches = h_media_matches;
    h.navigate = h_navigate;
    h.reload = h_reload;
    h.open_window = h_open_window;
    h.alert = h_alert;
    h.confirm = h_confirm;
    h.prompt = h_prompt;
    h.focus_changed = h_focus_changed;
    h.submit_form = h_submit_form;
    if (au_dv_callbacks(s->dv)->random)
        h.random = h_random;
    h.coro = au_dv_callbacks(s->dv)->coro;
    h.get_cookie = h_get_cookie;
    h.set_cookie = h_set_cookie;
    if (au_dv_callbacks(s->dv)->storage)
        h.storage = h_storage;
    h.sheet_text = h_sheet_text;
    h.sheet_set_text = h_sheet_set_text;
    h.sheet_disable = h_sheet_disable;
    h.fetcher.ctx = s;
    h.fetcher.start = f_start;
    h.fetcher.abort = f_abort;
    h.fetcher.pause = f_pause;
    h.frames_changed = h_frames_changed;
    h.frame_count = h_frame_count;
    h.frame_at = h_frame_at;
    h.frame_js = h_frame_js;
    h.parent_js = h_parent_js;
    h.frame_element = h_frame_element;
    h.canvas_set = h_canvas_set;
    h.canvas_image = h_canvas_image;
    h.image_origin_clean = h_image_origin_clean;
    AuDocView *dv = s->dv;
    if (dv->parent) {
        /* A nested document: a context in the page's runtime, which the
         * top-level document owns and so starts first. */
        AuDocView *top = au_dv_top(dv);
        if (!top->script || !ensure_js(top->script)) {
            s->js_failed = true;
            return false;
        }
        const char *url = au_docview_url(dv);
        char origin[512];
        set.sandbox_origin = (dv->sandbox & AU_SANDBOX_ORIGIN) != 0;
        set.no_top_navigation = (dv->sandbox & AU_SANDBOX_TOP_NAV) != 0;
        if (!strncmp(url, "about:", 6) && dv->parent->script && dv->parent->script->js) {
            /* about:blank and srcdoc documents have their parent's origin */
            snprintf(origin, sizeof origin, "%s", au_js_origin(dv->parent->script->js));
            set.origin = origin;
        }
        s->js = au_js_new_child(top->script->js, s->doc, url, &set, &h);
    } else {
        s->js = au_js_new(s->doc, au_docview_url(s->dv), &set, &h);
    }
    if (!s->js) {
        s->js_failed = true;
        return false;
    }
    if (s->parser && !au_html_parser_done(s->parser))
        au_js_set_parser(s->js, s->parser);
    if (dv->nframes)
        au_js_frames_changed(s->js); /* frames the parser made before the runtime started */
    return true;
}

static void enter(AuDocScript *s)
{
    au_dv_top(s->dv)->script_depth++;
    au_dv_top(s->dv)->script_runs++;
    if (s->depth++ == 0) { /* the outermost entry is timed (perf.h) */
        s->perf_t0 = au_dv_perf_now(s->dv);
        s->perf_label[0] = 0;
        /* Frames the parser has made since the last update are known to
         * script from its first statement (window.frames, parent.frames). */
        if (au_dv_top(s->dv)->script_depth == 1)
            au_frames_sync_all(au_dv_top(s->dv));
    }
    if (s->js && strcmp(au_docview_base_url(s->dv), au_docview_url(s->dv)) != 0)
        au_js_set_base_url(s->js, au_docview_base_url(s->dv));
}

/* Ends an entry from the view. At the outermost level, asks for one
 * repaint and hands a requested navigation to the embedder; s must not be
 * used after that (the embedder may start a new document). */
static void leave(AuDocScript *s)
{
    au_dv_top(s->dv)->script_depth--;
    if (--s->depth > 0)
        return;
    if (s->js) {
        uint32_t t1 = au_dv_perf_now(s->dv);
        au_perf_add(au_docview_perf(s->dv), AU_PERF_SCRIPT, t1 - s->perf_t0, s->perf_label, t1);
    }
    au_dv_turn_end(s->dv);
    /* A module may have finished (load event) */
    s->depth++;
    au_dv_check_finished(s->dv);
    s->depth--;
    if (s->nav_url) {
        char *url = s->nav_url;
        bool new_tab = s->nav_new_tab;
        AuDocView *dv = s->dv;
        s->nav_url = NULL;
        const AuDocViewCallbacks *cb = au_dv_callbacks(dv);
        if (cb->open_url)
            cb->open_url(cb->ctx, dv, url, new_tab);
        au_free(url);
    }
}

static void fire_simple(AuDocScript *s, AuElement *el, const char *type)
{
    if (s->js)
        au_js_fire(s->js, &el->node, type, false, false);
}

/* Runs a classic script's source. Called inside enter/leave. */
static void run_classic(AuDocScript *s, PendingScript *p)
{
    if (!ensure_js(s))
        return;
    /* Frame elements the parser has inserted since the frame list was
     * last brought up to date (that happens between parser chunks, and
     * the parser's insertions do not mark the list): a script right
     * after an iframe must see its window (frames.length, window[0], a
     * frame by name, contentWindow), which the standard gives an initial
     * about:blank document at once. The check walks the document, so it
     * is made only while the parser runs. */
    if (s->parser && !au_html_parser_done(s->parser)) {
        s->dv->frames_dirty = true;
        au_frames_sync(s->dv);
    }
    if (p->failed) {
        fire_simple(s, p->el, "error");
        return;
    }
    const char *url = p->url ? p->url : au_docview_url(s->dv);
    snprintf(s->perf_label, sizeof s->perf_label, "%s", url);
    if (p->muted && p->url)
        au_js_mute_script(s->js, p->url);
    /* The runtime takes the text and frees it once it is compiled. */
    size_t len = p->text ? p->len : 0;
    char *text = p->text ? p->text : au_strndup("", 0);
    p->text = NULL;
    au_js_run_classic_take(s->js, p->el, text, len, url, 1);
    if (p->url)
        fire_simple(s, p->el, "load");
}

static void run_deferred(AuDocScript *s);

static void module_done(void *ctx, AuElement *el, bool ok)
{
    (void)el;
    (void)ok;
    AuDocScript *s = ctx;
    if (s->modules)
        s->modules--;
    if (s->defer_module_running && s->defer && s->defer->el == el) {
        s->defer_module_running = false;
        PendingScript *p = s->defer;
        s->defer = p->next;
        if (!s->defer)
            s->defer_tail = NULL;
        free_pending(p);
        run_deferred(s);
    }
}

static void start_module(AuDocScript *s, PendingScript *p)
{
    if (!ensure_js(s))
        return;
    s->modules++;
    if (p->url)
        au_js_run_module(s->js, p->el, NULL, 0, p->url, module_done, s);
    else
        au_js_run_module(s->js, p->el, p->text ? p->text : "", p->len, au_docview_url(s->dv), module_done, s);
}

/* Runs the deferred list in order while its head is ready; then
 * DOMContentLoaded. Called inside enter/leave. */
static void run_deferred(AuDocScript *s)
{
    while (s->defer && !s->defer_module_running) {
        PendingScript *p = s->defer;
        if (p->kind == AU_JS_SCRIPT_MODULE) {
            s->defer_module_running = true;
            start_module(s, p);
            if (!s->js) { /* no runtime: drop it */
                s->defer_module_running = false;
                s->defer = p->next;
                free_pending(p);
            }
            continue;
        }
        if (!p->ready)
            return;
        s->defer = p->next;
        run_classic(s, p);
        free_pending(p);
    }
    if (!s->defer)
        s->defer_tail = NULL;
    if (s->parse_done && !s->defer && !s->content_loaded) {
        s->content_loaded = true;
        if (s->js)
            au_js_set_ready_state(s->js, "contentloaded");
    }
}

static PendingScript *new_pending(AuElement *el, int kind)
{
    PendingScript *p = au_calloc(sizeof *p);
    p->el = el;
    au_node_ref(&el->node);
    p->kind = kind;
    return p;
}

/* Starts loading an external script; false if it cannot be fetched. */
static bool fetch_script(AuDocScript *s, PendingScript *p, const char *src)
{
    char *abs = *src ? au_url_resolve_dup(au_docview_base_url(s->dv), src) : NULL;
    if (!abs)
        return false;
    p->url = abs;
    if (p->kind == AU_JS_SCRIPT_MODULE)
        return true; /* src/js fetches modules itself */
    /* Until the response says otherwise (a loader that reports no head),
     * the URL decides whether the script's errors are muted. */
    uint8_t cors = au_dv_cors_attr(p->el);
    p->muted = !cors && (!strncmp(abs, "http:", 5) || !strncmp(abs, "https:", 6)) &&
               !au_cors_same_origin(au_docview_url(s->dv), abs);
    p->req = au_dv_request_cors(s->dv, abs, cors);
    return p->req != 0;
}

/* ---- docview.c entry points ---- */

AuDocScript *au_docscript_new(AuDocView *dv)
{
    AuDocScript *s = au_calloc(sizeof *s);
    s->dv = dv;
    return s;
}

void au_docscript_set_parser(AuDocScript *s, AuHtmlParser *p, AuDocument *doc)
{
    s->parser = p;
    s->doc = doc;
}

void au_docscript_free(AuDocScript *s)
{
    if (!s)
        return;
    if (s->js) {
        s->depth++; /* no turn end or navigation from here */
        au_js_unload(s->js);
        au_js_free(s->js);
    }
    free_pending(s->blocking);
    free_list(s->defer);
    free_list(s->async);
    au_free(s->fetches);
    au_free(s->nav_url);
    au_free(s);
}

void au_docscript_script_ready(void *ctx, AuHtmlParser *parser, AuElement *el)
{
    AuDocScript *s = ctx;
    int kind = script_kind(el);
    if (kind == AU_JS_SCRIPT_NONE || !ensure_js(s))
        return;
    const char *src = attr(el, "src");
    enter(s);
    if (kind == AU_JS_SCRIPT_IMPORTMAP) {
        size_t len;
        char *text = script_text(el, &len);
        au_js_add_import_map(s->js, text, len);
        au_free(text);
    } else if (kind == AU_JS_SCRIPT_CLASSIC && attr(el, "nomodule")) {
        /* skipped: this browser runs modules */
    } else if (kind == AU_JS_SCRIPT_MODULE) {
        PendingScript *p = new_pending(el, kind);
        if (src && !fetch_script(s, p, src)) {
            fire_simple(s, el, "error");
            free_pending(p);
        } else {
            if (!src)
                p->text = script_text(el, &p->len);
            p->ready = true;
            if (attr(el, "async")) {
                start_module(s, p);
                free_pending(p);
            } else {
                if (s->defer_tail)
                    s->defer_tail->next = p;
                else
                    s->defer = p;
                s->defer_tail = p;
            }
        }
    } else if (!src) {
        PendingScript p;
        memset(&p, 0, sizeof p);
        p.el = el;
        p.text = script_text(el, &p.len);
        run_classic(s, &p);
        au_free(p.text);
    } else {
        PendingScript *p = new_pending(el, kind);
        if (!fetch_script(s, p, src)) {
            /* the error event is a task in the standard */
            p->failed = true;
            run_classic(s, p);
            free_pending(p);
        } else if (attr(el, "async")) {
            p->next = s->async;
            s->async = p;
        } else if (attr(el, "defer")) {
            if (s->defer_tail)
                s->defer_tail->next = p;
            else
                s->defer = p;
            s->defer_tail = p;
        } else {
            /* parser-blocking */
            s->blocking = p;
            au_html_parser_pause(parser);
        }
    }
    leave(s);
}

void au_docscript_parse_done(AuDocScript *s)
{
    if (s->parse_done)
        return;
    s->parse_done = true;
    if (!s->js) {
        /* Start the runtime only when an event handler attribute needs
         * it, or a popovertarget button (popovers live in the bindings). */
        bool handlers = false;
        for (AuNode *n = &s->doc->node; n && !handlers; n = au_node_next(n, &s->doc->node)) {
            if (n->type != AU_NODE_ELEMENT)
                continue;
            const AuElement *el = au_as_element(n);
            for (uint32_t i = 0; i < el->nattrs && !handlers; i++) {
                const char *name = au_atom_str(el->attrs[i].name);
                handlers = ((name[0] == 'o' || name[0] == 'O') && (name[1] == 'n' || name[1] == 'N')) ||
                           !strcmp(name, "popovertarget");
            }
        }
        if (!handlers) {
            s->content_loaded = true;
            return;
        }
        if (!ensure_js(s))
            return;
    }
    au_js_set_parser(s->js, NULL);
    enter(s);
    au_js_set_ready_state(s->js, "interactive");
    run_deferred(s);
    leave(s);
}

void au_docscript_resource_head(AuDocScript *s, uint32_t id, int status, const char *status_text,
                                const char *final_url, const char *const *headers, size_t nheaders, bool muted)
{
    struct ScriptFetch *f = find_fetch(s, id);
    if (f) {
        f->head_sent = true;
        if (s->js) {
            /* f->js: the document's runtime, or the worker that asked */
            AuJs *to = f->js;
            enter(s);
            au_js_fetch_head(to, id, status, status_text ? status_text : "", headers, nheaders, final_url);
            leave(s);
        }
        return;
    }
    /* A script element's source: an error status fails the load. */
    bool bad = status < 200 || status > 299;
    if (s->blocking && s->blocking->req == id) {
        s->blocking->bad_status = bad;
        s->blocking->muted = muted;
    }
    for (PendingScript *q = s->defer; q; q = q->next)
        if (q->req == id) {
            q->bad_status = bad;
            q->muted = muted;
        }
    for (PendingScript *q = s->async; q; q = q->next)
        if (q->req == id) {
            q->bad_status = bad;
            q->muted = muted;
        }
}

bool au_docscript_resource_data(AuDocScript *s, uint32_t id, const void *data, size_t len)
{
    struct ScriptFetch *f = find_fetch(s, id);
    if (!f)
        return false;
    if (!s->js || !len)
        return true;
    AuJs *to = f->js;
    /* No enter() here: these calls only queue tasks for script (or C data
     * for a worker), so no script runs inside the loader's callback, and
     * the turn bookkeeping of enter() and leave() is not wanted per piece. */
    if (!f->head_sent) {
        /* PLACEHOLDER HEAD, as in au_docscript_resource_done: a loader
         * that reports no head gets a 200 with a type from the URL. The
         * call may fail the fetch, which cancels the request (and so
         * removes f). */
        const char *hdrs[2] = {"content-type", f->type};
        f->head_sent = true;
        au_js_fetch_head(to, id, 200, "OK", hdrs, f->type ? 1 : 0, NULL);
        if (!find_fetch(s, id))
            return true;
    }
    au_js_fetch_data(to, id, data, len);
    return true;
}

bool au_docscript_resource_done(AuDocScript *s, uint32_t id, AuBuf *body, bool ok)
{
    const void *data = body->data;
    size_t len = body->len;
    const char *type = NULL;
    bool head_sent = false;
    AuJs *to = NULL;
    if (take_fetch(s, id, &type, &head_sent, &to)) {
        if (!s->js)
            return true;
        enter(s);
        if (ok) {
            /* PLACEHOLDER HEAD: a loader that never calls
             * au_docview_resource_head gives no status or headers, so a
             * delivered body is reported as a 200 with a type guessed
             * from the URL. */
            const char *hdrs[2] = {"content-type", type};
            if (!head_sent)
                au_js_fetch_head(to, id, 200, "OK", hdrs, type ? 1 : 0, NULL);
            if (len)
                au_js_fetch_data(to, id, data, len);
        }
        au_js_fetch_done(to, id, ok);
        leave(s);
        return true;
    }
    PendingScript *p = NULL;
    int where = 0;
    if (s->blocking && s->blocking->req == id) {
        p = s->blocking;
        where = 1;
    }
    for (PendingScript *q = s->defer; !p && q; q = q->next)
        if (q->req == id) {
            p = q;
            where = 2;
        }
    PendingScript **pp = &s->async;
    for (; !p && *pp; pp = &(*pp)->next)
        if ((*pp)->req == id) {
            p = *pp;
            where = 3;
            break;
        }
    if (!p)
        return false;
    p->req = 0;
    p->ready = true;
    p->failed = !ok || p->bad_status;
    ok = !p->failed;
    if (ok) {
        /* the response's buffer becomes the script's text: no second copy */
        p->text = au_buf_take(body, &p->len);
        if (!p->text) {
            p->text = au_strndup("", 0);
            p->len = 0;
        }
    }
    enter(s);
    if (where == 1) {
        s->blocking = NULL;
        au_html_parser_begin_script(s->parser);
        run_classic(s, p);
        au_html_parser_end_script(s->parser);
        free_pending(p);
        /* Parsing resumes; it may meet the next blocking script. */
        au_html_parser_resume(s->parser);
        au_dv_parsed_more(s->dv);
    } else if (where == 2) {
        if (s->parse_done)
            run_deferred(s);
    } else {
        *pp = p->next;
        run_classic(s, p);
        free_pending(p);
    }
    leave(s);
    return true;
}

bool au_docscript_busy(const AuDocScript *s)
{
    return s->blocking || s->defer || s->async || s->modules || !s->content_loaded;
}

void au_docscript_loaded(AuDocScript *s)
{
    if (s->loaded)
        return;
    s->loaded = true;
    if (!s->js)
        return;
    enter(s);
    au_js_set_ready_state(s->js, "complete");
    leave(s);
}

bool au_docscript_running(const AuDocScript *s)
{
    return s && s->js;
}

bool au_docscript_mouse(AuDocScript *s, const AuEvent *ev, AuElement *target, const char *type, int vx, int vy)
{
    if (!s->js || !target)
        return true;
    enter(s);
    int detail = ev->clicks > 0 ? ev->clicks : 1;
    if (!strcmp(type, "mousemove"))
        detail = 0;
    bool ok = au_js_fire_mouse(s->js, target, type, vx, vy, ev->button, ev->mods & 15, detail, NULL);
    leave(s);
    return ok;
}

bool au_docscript_fire(AuDocScript *s, AuElement *el, const char *type, bool bubbles, bool cancelable)
{
    if (!s->js || !el)
        return true;
    enter(s);
    bool ok = au_js_fire(s->js, &el->node, type, bubbles, cancelable);
    leave(s);
    return ok;
}

void au_docscript_focus(AuDocScript *s, AuElement *el)
{
    if (!s->js)
        return;
    enter(s);
    au_js_focus(s->js, el);
    leave(s);
}

bool au_docscript_key(AuDocScript *s, AuElement *target, const char *type, const char *key, const char *code,
                      uint32_t mods)
{
    if (!s->js)
        return true;
    enter(s);
    bool ok = au_js_fire_key(s->js, target, type, key, code, mods & 15, false);
    leave(s);
    return ok;
}

bool au_docscript_input(AuDocScript *s, AuElement *el, const char *type, const char *input_type, const char *data)
{
    if (!s->js || !el)
        return true;
    enter(s);
    bool ok = au_js_fire_input(s->js, el, type, input_type, data);
    leave(s);
    return ok;
}

bool au_docscript_submit_event(AuDocScript *s, AuElement *form, AuElement *submitter)
{
    if (!s->js)
        return true;
    enter(s);
    bool ok = au_js_fire_submit(s->js, form, submitter);
    leave(s);
    return ok;
}

int au_docscript_pattern(AuDocScript *s, const char *pattern, const char *value)
{
    if (!s->js)
        return -1;
    return au_js_pattern_test(s->js, pattern, value);
}

bool au_docscript_close_request(AuDocScript *s)
{
    if (!s->js)
        return false;
    enter(s);
    bool r = au_js_close_request(s->js);
    leave(s);
    return r;
}

void au_docscript_light_dismiss(AuDocScript *s, AuElement *target, bool up)
{
    if (!s->js)
        return;
    enter(s);
    au_js_light_dismiss(s->js, target, up);
    leave(s);
}

void au_docscript_hover(AuDocScript *s, AuElement *from, AuElement *to, int vx, int vy)
{
    if (!s->js)
        return;
    /* Both are held: a mouseout listener may remove either, and from is
     * the related target of the second event. */
    if (from)
        au_node_ref(&from->node);
    if (to)
        au_node_ref(&to->node);
    enter(s);
    if (from && au_node_is_connected(&from->node))
        au_js_fire_mouse(s->js, from, "mouseout", vx, vy, 0, 0, 0, to);
    if (to && s->js)
        au_js_fire_mouse(s->js, to, "mouseover", vx, vy, 0, 0, 0, from);
    leave(s);
    if (from)
        au_node_unref(&from->node);
    if (to)
        au_node_unref(&to->node);
}

void au_docscript_javascript_url(AuDocScript *s, const char *url)
{
    if (!ensure_js(s))
        return;
    enter(s);
    au_js_run_javascript_url(s->js, url);
    leave(s);
}

bool au_docscript_run(AuDocScript *s, uint32_t budget_ms)
{
    if (!s->js)
        return false;
    bool ran = false;
    enter(s);
    if (s->resize_pending) {
        s->resize_pending = false;
        au_js_resized(s->js);
        ran = true;
    }
    if (s->scroll_pending) {
        s->scroll_pending = false;
        au_js_scrolled(s->js);
        ran = true;
    }
    if (au_js_run_tasks(s->js, budget_ms))
        ran = true;
    if (au_js_wants_frame(s->js)) {
        au_js_run_frame(s->js);
        ran = true;
    }
    /* The document's workers, a slice each in turn. */
    if (au_js_run_workers(s->js, WORKER_BUDGET_MS))
        ran = true;
    leave(s);
    return ran;
}

bool au_docscript_content_loaded(const AuDocScript *s)
{
    return s->content_loaded;
}

void au_docscript_heap(const AuDocScript *s, size_t *used, size_t *limit)
{
    *used = *limit = 0;
    if (!s->js)
        return;
    AuJsStats st;
    au_js_stats(s->js, &st);
    *used = st.memory_used;
    *limit = st.memory_limit;
}

void au_docscript_memory_summary(const AuDocScript *s, AuBuf *out)
{
    if (s && s->js)
        au_js_memory_summary(s->js, out);
}

void au_docscript_peaks(const AuDocScript *s, size_t *page_peak, size_t *worker_peak)
{
    *page_peak = *worker_peak = 0;
    if (!s || !s->js)
        return;
    AuJsStats st;
    au_js_stats(s->js, &st);
    *page_peak = st.memory_peak;
    *worker_peak = st.worker_peak;
}

bool au_docscript_eval(AuDocScript *s, const char *src, AuBuf *out)
{
    /* The console may be the first script a page without scripts runs. */
    if (!s->js && !ensure_js(s))
        return false;
    enter(s);
    bool ok = au_js_eval(s->js, src, strlen(src), out);
    leave(s);
    return ok;
}

uint32_t au_docscript_due(AuDocScript *s)
{
    if (!s->js)
        return UINT32_MAX;
    if (s->resize_pending || s->scroll_pending || au_js_workers_busy(s->js))
        return 0;
    uint32_t due = au_js_wants_frame(s->js) ? 16 : UINT32_MAX;
    double t = au_js_next_timer(s->js);
    if (t >= 0) {
        double d = t - au_dv_now(s->dv);
        uint32_t ms = d <= 0 ? 0 : d >= 86400000.0 ? 86400000u : (uint32_t)d;
        if (ms < due)
            due = ms;
    }
    return due;
}

void au_docscript_resized(AuDocScript *s)
{
    s->resize_pending = s->js != NULL;
}

void au_docscript_scrolled(AuDocScript *s)
{
    s->scroll_pending = s->js != NULL;
}

bool au_docscript_on_stack(const AuDocScript *s)
{
    return s && s->js && au_js_running(s->js);
}

void au_docscript_frames_changed(AuDocScript *s)
{
    if (s && s->js)
        au_js_frames_changed(s->js);
}

bool au_docscript_flush_nav(AuDocScript *s)
{
    if (!s || !s->nav_url || s->depth > 0)
        return false;
    char *url = s->nav_url;
    bool new_tab = s->nav_new_tab;
    AuDocView *dv = s->dv;
    s->nav_url = NULL;
    const AuDocViewCallbacks *cb = au_dv_callbacks(dv);
    if (cb->open_url)
        cb->open_url(cb->ctx, dv, url, new_tab);
    au_free(url);
    return true;
}

bool au_docscript_can_persist(AuDocScript *s)
{
    return !s || !s->js || !au_js_has_unload_handler(s->js);
}

int au_docscript_workers(const AuDocScript *s)
{
    return s && s->js ? au_js_worker_count(s->js) : 0;
}

void au_docscript_page_transition(AuDocScript *s, bool show)
{
    if (!s || !s->js)
        return;
    au_js_page_transition(s->js, show);
    if (!show)
        au_js_gc(s->js); /* a hidden document holds no garbage */
}

void au_docscript_storage_event(AuDocScript *s, const AuDocStorageEvent *ev)
{
    if (!s || !s->js)
        return;
    au_js_storage_event(s->js, ev->kind, ev->storage_key, ev->key, ev->key_len, ev->old_value, ev->old_len,
                        ev->new_value, ev->new_len, ev->url);
}

size_t au_docscript_memory(const AuDocScript *s)
{
    if (!s || !s->js)
        return 0;
    AuJsStats st;
    au_js_stats(s->js, &st);
    return st.memory_used;
}

void au_docscript_layout_done(AuDocScript *s)
{
    if (!s->js)
        return;
    enter(s);
    au_js_layout_done(s->js);
    leave(s);
}
