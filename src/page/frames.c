/* Nested documents: iframe, frame, object and embed.
 *
 * Each frame element of a document has an AuFrame in the view of that
 * document (AuDocView.frames, in tree order). A frame that shows a
 * document owns an AuDocView for it, created only when there is content
 * to show or script asks for the frame's window: a page of empty iframes
 * costs one small record each.
 *
 * Loading. The frame's document is requested through the owner view as an
 * AU_RES_FRAME resource; a nested view's own resources go through the top
 * view's loader (docview.c, loader_start). The response head decides what
 * the element shows: an HTML or text document (a nested view), an image
 * (an iframe wraps it in a generated document; an object or embed becomes
 * an image box in layout), or nothing (an object then shows its fallback
 * content and gets an error event). X-Frame-Options and the CSP
 * frame-ancestors directive are checked against every ancestor document.
 *
 * Limits. At most AU_FRAMES_MAX nested documents per page and
 * AU_FRAMES_DEPTH levels; a frame whose URL is that of one of its
 * ancestors is refused; loading=lazy frames load when they come within a
 * viewport of the visible area or are painted.
 *
 * Lifetime. Script may remove or navigate a frame while script of that
 * frame, or of any document of the page, is running. Nothing is freed
 * then: a removed frame leaves its owner's list at once (its window is
 * gone for script) and waits in the top view's dead list; navigations and
 * load events wait in the frames. au_frames_idle does the work when no
 * script of the page is on the stack.
 *
 * Geometry. A nested view is laid out to the element's content box and
 * painted there by layout's paint_frame callback, with the frame's own
 * scroll offset (AuFrame.sx, sy) and scroll bars drawn here. Rectangles a
 * nested view invalidates are moved into the owner's coordinates and
 * clipped to the box. */
#include "docview_int.h"

#include "../url/url.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { FK_IFRAME, FK_FRAME, FK_OBJECT, FK_EMBED };

enum {
    FS_NONE,     /* nothing to load: a blank document */
    FS_WAIT,     /* requested; no data yet */
    FS_DOC,      /* a nested document is loading or loaded */
    FS_IMAGE,    /* object, embed: an image, shown by layout */
    FS_FALLBACK, /* object: its children; embed: an empty box */
    FS_BLOCKED,  /* refused by the response's framing rules, or failed */
    FS_LAZY,     /* loading=lazy: not near the viewport yet */
    FS_REFUSED   /* over a limit, or its own ancestor */
};

struct AuFrame {
    AuDocView *owner;
    AuElement *el; /* referenced */
    AuDocView *dv; /* the nested view, or NULL */
    uint8_t kind, state;
    char *key;      /* the attributes that decide what is loaded, as last seen */
    char *url;      /* the document's URL (absolute), or NULL */
    uint32_t req;   /* the owner's request for the document */
    int status;     /* from the response head */
    char *ctype;    /* Content-Type from the head, or NULL */
    char *final_url;
    bool got_head, begun; /* of the current load: the head arrived, the nested document was started */
    bool pre;       /* the document is plain text, shown in a <pre> */
    bool counted;   /* in the top view's frame_total */
    bool quiet;     /* a blank document made for script: no load event */
    bool load_due, error_due; /* events for the element, fired from au_frames_idle */
    bool restart;   /* look at the attributes again from au_frames_idle */
    bool dead;
    uint32_t sandbox;
    int sx, sy;     /* scroll offset */
    AuRect box;     /* the content box in the owner's document coordinates */
    float off_x, off_y; /* from the border box corner to the content box */
    bool has_box;
    /* a navigation waiting for au_frames_idle */
    char *nav_url, *nav_body, *nav_type;
    size_t nav_len;
    bool nav_post;
};

/* ---- Small helpers ---- */

static char *dup(const char *s)
{
    return au_strndup(s ? s : "", strlen(s ? s : ""));
}

static const char *attr(const AuElement *el, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr(el, a) : NULL;
}

static bool ieq(const char *a, const char *b)
{
    return a && au_ieq(a, strlen(a), b, strlen(b));
}

static bool starts_ci(const char *s, const char *p)
{
    size_t n = strlen(p);
    return s && strlen(s) >= n && au_ieq(s, n, p, n);
}

static size_t base_len(const char *url)
{
    return strcspn(url, "#");
}

static int frame_kind(const AuNode *n)
{
    if (n->type != AU_NODE_ELEMENT || ((const AuElement *)n)->ns != AU_NS_HTML)
        return -1;
    AuAtom t = ((const AuElement *)n)->tag;
    return t == AU_ATOM_iframe ? FK_IFRAME : t == AU_ATOM_frame ? FK_FRAME : t == AU_ATOM_object ? FK_OBJECT
           : t == AU_ATOM_embed ? FK_EMBED : -1;
}

static AuFrame *frame_of(AuDocView *dv, const AuElement *el)
{
    for (uint32_t i = 0; i < dv->nframes; i++)
        if (dv->frames[i]->el == el)
            return dv->frames[i];
    return NULL;
}

static int depth_of(const AuDocView *dv)
{
    int d = 0;
    for (; dv->parent; dv = dv->parent)
        d++;
    return d;
}

static void work(AuDocView *dv)
{
    au_dv_top(dv)->frames_work = true;
}

/* No script of the page is running: documents may be replaced or freed. */
static bool safe(AuDocView *top)
{
    return top->script_depth == 0 && !au_docscript_on_stack(top->script);
}

static void console(AuDocView *dv, const char *fmt, const char *a, const char *b)
{
    AuDocView *top = au_dv_top(dv);
    if (!top->cb.console)
        return;
    AuBuf m;
    au_buf_init(&m);
    au_buf_printf(&m, fmt, a ? a : "", b ? b : "");
    top->cb.console(top->cb.ctx, top, 2, m.data ? m.data : "", m.len);
    au_buf_free(&m);
}

/* scheme://host[:port] of an http, https or file URL, lower case, without
 * a default port; "" for anything else. */
static void origin_of(const char *url, char *out, size_t cap)
{
    out[0] = 0;
    if (!url)
        return;
    const char *p = strstr(url, "://");
    if (!p || (!starts_ci(url, "http:") && !starts_ci(url, "https:") && !starts_ci(url, "file:")))
        return;
    const char *h = p + 3;
    size_t hn = strcspn(h, "/?#");
    const char *at = memchr(h, '@', hn);
    if (at) {
        hn -= (size_t)(at + 1 - h);
        h = at + 1;
    }
    bool https = starts_ci(url, "https:");
    size_t n = (size_t)(p - url) + 3;
    if (n + hn + 1 > cap)
        return;
    memcpy(out, url, n);
    memcpy(out + n, h, hn);
    out[n + hn] = 0;
    for (char *c = out; *c; c++)
        if (*c >= 'A' && *c <= 'Z')
            *c = (char)(*c + 32);
    char *colon = strrchr(out + n, ':');
    if (colon && !strchr(colon, ']') && !strcmp(colon, https ? ":443" : ":80"))
        *colon = 0;
}

static bool same_origin_url(const char *a, const char *b)
{
    char oa[300], ob[300];
    origin_of(a, oa, sizeof oa);
    origin_of(b, ob, sizeof ob);
    return oa[0] && !strcmp(oa, ob);
}

/* ---- Sandbox ---- */

static uint32_t sandbox_flags(const AuElement *el)
{
    const char *s = attr(el, "sandbox");
    if (!s)
        return 0;
    uint32_t f = AU_SANDBOX_SCRIPTS | AU_SANDBOX_FORMS | AU_SANDBOX_ORIGIN | AU_SANDBOX_TOP_NAV | AU_SANDBOX_POPUPS;
    while (*s) {
        while (*s && au_is_space((unsigned char)*s))
            s++;
        const char *t = s;
        while (*s && !au_is_space((unsigned char)*s))
            s++;
        size_t n = (size_t)(s - t);
        if (au_ieq(t, n, "allow-scripts", 13))
            f &= ~(uint32_t)AU_SANDBOX_SCRIPTS;
        else if (au_ieq(t, n, "allow-forms", 11))
            f &= ~(uint32_t)AU_SANDBOX_FORMS;
        else if (au_ieq(t, n, "allow-same-origin", 17))
            f &= ~(uint32_t)AU_SANDBOX_ORIGIN;
        else if (au_ieq(t, n, "allow-top-navigation", 20))
            f &= ~(uint32_t)AU_SANDBOX_TOP_NAV;
        else if (au_ieq(t, n, "allow-popups", 12))
            f &= ~(uint32_t)AU_SANDBOX_POPUPS;
    }
    return f;
}

/* ---- Callbacks of a nested view (ctx is the AuFrame) ---- */

static AuIRect frame_rect_in_owner(const AuFrame *f, AuIRect r)
{
    AuIRect o = {(int)f->box.x + r.x0 - f->sx - 1, (int)f->box.y + r.y0 - f->sy - 1, (int)f->box.x + r.x1 - f->sx + 1,
                 (int)f->box.y + r.y1 - f->sy + 1};
    int bx0 = (int)f->box.x - 1, by0 = (int)f->box.y - 1;
    int bx1 = (int)(f->box.x + f->box.w) + 2, by1 = (int)(f->box.y + f->box.h) + 2;
    if (o.x0 < bx0)
        o.x0 = bx0;
    if (o.y0 < by0)
        o.y0 = by0;
    if (o.x1 > bx1)
        o.x1 = bx1;
    if (o.y1 > by1)
        o.y1 = by1;
    return o;
}

static void invalidate_frame(AuFrame *f)
{
    AuDocView *o = f->owner;
    if (!f->has_box || !o->cb.invalidate)
        return;
    AuIRect r = {(int)f->box.x - 1, (int)f->box.y - 1, (int)(f->box.x + f->box.w) + 2, (int)(f->box.y + f->box.h) + 2};
    o->cb.invalidate(o->cb.ctx, o, r);
}

static void fc_invalidate(void *ctx, AuDocView *dv, AuIRect r)
{
    (void)dv;
    AuFrame *f = ctx;
    AuDocView *o = f->owner;
    if (f->dead || !f->has_box || !o->cb.invalidate)
        return;
    AuIRect t = frame_rect_in_owner(f, r);
    if (t.x1 > t.x0 && t.y1 > t.y0)
        o->cb.invalidate(o->cb.ctx, o, t);
}

static void clamp_scroll(AuFrame *f)
{
    if (!f->dv)
        return;
    float w, h;
    au_layout_document_size(au_docview_layout(f->dv), &w, &h);
    int mx = (int)(w + 0.99f) - f->dv->vw, my = (int)(h + 0.99f) - f->dv->vh;
    if (f->sx > mx)
        f->sx = mx;
    if (f->sy > my)
        f->sy = my;
    if (f->sx < 0)
        f->sx = 0;
    if (f->sy < 0)
        f->sy = 0;
}

static void fc_size_changed(void *ctx, AuDocView *dv)
{
    (void)dv;
    AuFrame *f = ctx;
    if (f->dead)
        return;
    int ox = f->sx, oy = f->sy;
    clamp_scroll(f);
    if (ox != f->sx || oy != f->sy)
        au_docview_set_scroll(f->dv, f->sx, f->sy);
    invalidate_frame(f); /* the scroll bars */
}

static void fc_status(void *ctx, AuDocView *dv, const char *text)
{
    (void)dv;
    AuFrame *f = ctx;
    AuDocView *top = au_dv_top(f->owner);
    if (!f->dead && top->cb.status_text)
        top->cb.status_text(top->cb.ctx, top, text);
}

static void fc_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    AuFrame *f = ctx;
    if (f->dead)
        return;
    AuDocView *to = NULL;
    int where = au_frames_route(dv, NULL, new_tab, false, &to);
    if (where == 1) {
        au_frames_navigate(to, url, NULL);
    } else if (where == 3) {
        AuDocView *top = au_dv_top(f->owner);
        if (top->cb.open_url)
            top->cb.open_url(top->cb.ctx, top, url, true);
    }
}

static void fc_load_finished(void *ctx, AuDocView *dv)
{
    (void)dv;
    AuFrame *f = ctx;
    if (f->quiet || f->dead)
        return;
    f->load_due = true;
    work(f->owner);
}

static double fc_now(void *ctx, AuDocView *dv)
{
    (void)dv;
    AuFrame *f = ctx;
    return au_dv_now(au_dv_top(f->owner));
}

static void fc_scroll_to(void *ctx, AuDocView *dv, int x, int y)
{
    AuFrame *f = ctx;
    if (f->dead)
        return;
    au_docview_update(dv);
    f->sx = x;
    f->sy = y;
    clamp_scroll(f);
    au_docview_set_scroll(dv, f->sx, f->sy);
    invalidate_frame(f);
}

static void fc_console(void *ctx, AuDocView *dv, int level, const char *text, size_t len)
{
    (void)dv;
    AuFrame *f = ctx;
    AuDocView *top = au_dv_top(f->owner);
    if (top->cb.console)
        top->cb.console(top->cb.ctx, top, level, text, len);
}

static bool fc_random(void *ctx, AuDocView *dv, void *buf, size_t len)
{
    (void)dv;
    AuFrame *f = ctx;
    AuDocView *top = au_dv_top(f->owner);
    return top->cb.random && top->cb.random(top->cb.ctx, top, buf, len);
}

static int fc_popup(void *ctx, AuDocView *dv, const AuPopupItem *items, int n, int selected, AuIRect at)
{
    AuFrame *f = ctx;
    AuDocView *top = au_dv_top(f->owner);
    if (f->dead || !top->cb.popup)
        return -2;
    return top->cb.popup(top->cb.ctx, top, items, n, selected, au_docview_rect_to_top(dv, at));
}

static bool fc_choose_file(void *ctx, AuDocView *dv, bool multiple, AuControlFile **files, uint32_t *n)
{
    (void)dv;
    AuFrame *f = ctx;
    AuDocView *top = au_dv_top(f->owner);
    return !f->dead && top->cb.choose_file && top->cb.choose_file(top->cb.ctx, top, multiple, files, n);
}

static bool fc_confirm(void *ctx, AuDocView *dv, const char *message)
{
    (void)dv;
    AuFrame *f = ctx;
    AuDocView *top = au_dv_top(f->owner);
    return !top->cb.confirm || top->cb.confirm(top->cb.ctx, top, message);
}

static void fc_message(void *ctx, AuDocView *dv, const char *text)
{
    (void)dv;
    AuFrame *f = ctx;
    AuDocView *top = au_dv_top(f->owner);
    if (top->cb.message)
        top->cb.message(top->cb.ctx, top, text);
}

/* ---- The nested view ---- */

static bool count_frame(AuFrame *f)
{
    AuDocView *top = au_dv_top(f->owner);
    if (f->counted)
        return true;
    if (top->frame_total >= AU_FRAMES_MAX || depth_of(f->owner) + 1 > AU_FRAMES_DEPTH)
        return false;
    f->counted = true;
    top->frame_total++;
    return true;
}

static AuDocView *frame_view(AuFrame *f)
{
    if (f->dv)
        return f->dv;
    if (!count_frame(f))
        return NULL;
    AuDocView *o = f->owner, *top = au_dv_top(o);
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = f;
    cb.invalidate = fc_invalidate;
    cb.size_changed = fc_size_changed;
    cb.status_text = fc_status;
    cb.open_url = fc_open;
    cb.load_finished = fc_load_finished;
    cb.now_ms = fc_now;
    cb.scroll_to = fc_scroll_to;
    cb.console = fc_console;
    if (top->cb.random)
        cb.random = fc_random;
    cb.coro = top->cb.coro;
    cb.popup = fc_popup;
    cb.choose_file = fc_choose_file;
    cb.confirm = fc_confirm;
    cb.message = fc_message;
    AuDocView *c = au_docview_new(o->fonts, NULL, &cb);
    c->parent = o;
    c->in_frame = f;
    c->sandbox = f->sandbox | o->sandbox;
    c->scripting = o->scripting && !(c->sandbox & AU_SANDBOX_SCRIPTS);
    if (o->user_css)
        au_docview_set_user_css(c, o->user_css);
    if (o->has_font_prefs)
        au_docview_set_font_prefs(c, &o->font_prefs);
    if (f->has_box && f->box.w >= 1 && f->box.h >= 1)
        au_docview_set_viewport(c, (int)f->box.w, (int)f->box.h);
    else
        au_docview_set_viewport(c, 300, 150);
    f->dv = c;
    return c;
}

/* Replaces the nested document with a generated one. */
static void frame_show_html(AuFrame *f, const char *url, const char *html)
{
    AuDocView *c = frame_view(f);
    if (!c)
        return;
    f->sx = f->sy = 0;
    au_docview_begin(c, url);
    au_docview_feed(c, html, strlen(html));
    au_docview_finish(c);
    invalidate_frame(f);
}

static void owner_relayout(AuFrame *f)
{
    /* what the element shows changed kind (document, image, fallback):
     * its box is built again */
    au_dv_tree_dirty(f->owner);
}

static void drop_request(AuFrame *f)
{
    if (f->req) {
        uint32_t id = f->req;
        f->req = 0;
        au_dv_cancel(f->owner, id);
    }
}

static void drop_view(AuFrame *f)
{
    if (!f->dv)
        return;
    AuDocView *c = f->dv;
    AuDocView *o = f->owner;
    f->dv = NULL;
    if (o->focus_frame == f)
        o->focus_frame = NULL;
    au_docview_free(c);
}

static void set_state(AuFrame *f, int st)
{
    bool was_box = f->state == FS_DOC || f->state == FS_WAIT || f->state == FS_NONE || f->state == FS_BLOCKED ||
                   f->state == FS_LAZY || f->state == FS_REFUSED;
    bool is_box = st == FS_DOC || st == FS_WAIT || st == FS_NONE || st == FS_BLOCKED || st == FS_LAZY ||
                  st == FS_REFUSED;
    f->state = (uint8_t)st;
    if ((f->kind == FK_OBJECT || f->kind == FK_EMBED) && (was_box != is_box || st == FS_IMAGE || st == FS_FALLBACK))
        owner_relayout(f);
}

/* The load failed or was refused: an iframe shows nothing and still gets
 * its load event; an object shows its fallback and gets an error event. */
static void frame_failed(AuFrame *f, int st)
{
    drop_request(f);
    if (f->kind == FK_OBJECT || f->kind == FK_EMBED) {
        if (safe(au_dv_top(f->owner)))
            drop_view(f);
        set_state(f, f->kind == FK_OBJECT ? FS_FALLBACK : st);
        f->error_due = true;
    } else {
        set_state(f, st);
        if (f->dv && safe(au_dv_top(f->owner)))
            frame_show_html(f, "about:blank", "");
        else
            f->load_due = true;
    }
    work(f->owner);
    invalidate_frame(f);
}

/* ---- Starting a load ---- */

static bool is_image_url(const char *url)
{
    static const char *const ext[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico", ".svg", ".apng"};
    if (starts_ci(url, "data:"))
        return starts_ci(url + 5, "image/");
    size_t n = strcspn(url, "?#");
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        size_t k = strlen(ext[i]);
        if (n >= k && au_ieq(url + n - k, k, ext[i], k))
            return true;
    }
    return false;
}

static bool is_ancestor_url(AuFrame *f, const char *url)
{
    size_t n = base_len(url);
    for (AuDocView *a = f->owner; a; a = a->parent)
        if (strlen(a->url) >= n && base_len(a->url) == n && !strncmp(a->url, url, n))
            return true;
    return false;
}

static void frame_fetch(AuFrame *f, const char *url, bool post, const char *body, size_t body_len,
                        const char *body_type)
{
    drop_request(f);
    au_free(f->url);
    f->url = dup(url);
    au_free(f->ctype);
    au_free(f->final_url);
    f->ctype = f->final_url = NULL;
    f->got_head = f->begun = f->pre = false;
    f->status = 0;
    f->load_due = f->error_due = false;
    if (!count_frame(f) || is_ancestor_url(f, url)) {
        console(f->owner, f->counted ? "Frame not loaded: %s is one of its own ancestors%s"
                                     : "Frame not loaded: the page has too many frames, or they nest too deeply (%s)%s",
                url, "");
        set_state(f, FS_REFUSED);
        f->load_due = true; /* the element still gets its load event */
        work(f->owner);
        return;
    }
    AuResourceRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.kind = AU_RES_FRAME;
    rq.document_url = f->owner->url;
    if (post) {
        rq.method = "POST";
        rq.body = body ? body : "";
        rq.body_len = body_len;
        rq.body_type = body_type;
    }
    set_state(f, FS_WAIT);
    f->req = au_dv_start_request(f->owner, &rq, f);
    if (!f->req)
        frame_failed(f, FS_BLOCKED);
}

static bool near_viewport(const AuFrame *f)
{
    const AuDocView *o = f->owner;
    if (!f->has_box)
        return false;
    if (o->parent)
        return true; /* inside a frame that is itself loaded */
    float top = (float)o->scroll_y - (float)o->vh, bottom = (float)o->scroll_y + 2.0f * (float)o->vh;
    return f->box.y + f->box.h >= top && f->box.y <= bottom;
}

/* Reads the element's attributes and loads what they name. */
static void frame_start(AuFrame *f, bool lazy_ok)
{
    AuDocView *o = f->owner, *top = au_dv_top(o);
    AuElement *el = f->el;
    if (f->dv && !safe(top)) {
        f->restart = true; /* a document is replaced: when no script runs */
        work(o);
        return;
    }
    f->restart = false;
    if (f->kind == FK_IFRAME)
        f->sandbox = sandbox_flags(el);
    const char *src = attr(el, f->kind == FK_OBJECT ? "data" : "src");
    const char *srcdoc = f->kind == FK_IFRAME ? attr(el, "srcdoc") : NULL;
    const char *type = f->kind == FK_OBJECT || f->kind == FK_EMBED ? attr(el, "type") : NULL;
    char abs[2048];
    abs[0] = 0;
    if (src && *src && !au_url_resolve(o->base, src, abs, sizeof abs))
        abs[0] = 0;
    f->load_due = f->error_due = false;
    if (srcdoc) {
        drop_request(f);
        if (!count_frame(f)) {
            set_state(f, FS_REFUSED);
            return;
        }
        au_free(f->url);
        f->url = dup("about:srcdoc");
        set_state(f, FS_DOC);
        f->begun = true;
        frame_show_html(f, "about:srcdoc", srcdoc);
        /* relative URLs in a srcdoc document resolve against the parent's */
        if (f->dv) {
            au_free(f->dv->base);
            f->dv->base = dup(o->base);
        }
        return;
    }
    if (f->kind == FK_OBJECT || f->kind == FK_EMBED) {
        if (!abs[0]) {
            drop_request(f);
            drop_view(f);
            set_state(f, FS_FALLBACK);
            return;
        }
        bool typed = type && *type;
        if ((typed && starts_ci(type, "image/")) || (!typed && is_image_url(abs))) {
            drop_request(f);
            drop_view(f);
            set_state(f, FS_IMAGE);
            return;
        }
        /* a data: URL names its own type */
        if (!typed && starts_ci(abs, "data:") && !starts_ci(abs + 5, "text/html") && !starts_ci(abs + 5, "text/plain") &&
            abs[5] != ',' && abs[5] != ';')
            type = abs + 5, typed = true;
        if (typed && !starts_ci(type, "text/html") && !starts_ci(type, "application/xhtml+xml") &&
            !starts_ci(type, "text/plain")) {
            drop_request(f);
            drop_view(f);
            set_state(f, FS_FALLBACK); /* a plugin type */
            f->error_due = f->kind == FK_OBJECT;
            work(o);
            return;
        }
    }
    if (!abs[0] || starts_ci(abs, "about:blank") || starts_ci(abs, "javascript:")) {
        drop_request(f);
        au_free(f->url);
        f->url = NULL;
        set_state(f, FS_NONE);
        if (f->dv)
            frame_show_html(f, "about:blank", ""); /* its load event follows */
        else
            f->load_due = true;
        work(o);
        return;
    }
    const char *loading = f->kind == FK_IFRAME ? attr(el, "loading") : NULL;
    if (lazy_ok && ieq(loading, "lazy") && !near_viewport(f)) {
        au_free(f->url);
        f->url = dup(abs);
        set_state(f, FS_LAZY);
        return;
    }
    frame_fetch(f, abs, false, NULL, 0, NULL);
}

static char *frame_key(const AuFrame *f)
{
    AuBuf b;
    au_buf_init(&b);
    const char *src = attr(f->el, f->kind == FK_OBJECT ? "data" : "src");
    const char *srcdoc = f->kind == FK_IFRAME ? attr(f->el, "srcdoc") : NULL;
    const char *type = f->kind == FK_OBJECT || f->kind == FK_EMBED ? attr(f->el, "type") : NULL;
    au_buf_puts(&b, src ? src : "");
    au_buf_putc(&b, 1);
    if (srcdoc) {
        au_buf_putc(&b, 'D');
        au_buf_puts(&b, srcdoc);
    }
    au_buf_putc(&b, 1);
    au_buf_puts(&b, type ? type : "");
    au_buf_cstr(&b);
    return au_buf_take(&b, NULL);
}

/* ---- The response ---- */

static const char *header(const char *const *headers, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (headers[2 * i] && ieq(headers[2 * i], name))
            return headers[2 * i + 1];
    return NULL;
}

/* One source expression of frame-ancestors against an ancestor's URL. */
static bool source_matches(const char *src, size_t n, const char *self_url, const char *anc_url)
{
    if (n == 6 && au_ieq(src, n, "'self'", 6))
        return same_origin_url(self_url, anc_url);
    if (n == 1 && src[0] == '*')
        return starts_ci(anc_url, "http:") || starts_ci(anc_url, "https:");
    char ao[300];
    origin_of(anc_url, ao, sizeof ao);
    if (!ao[0])
        return false;
    const char *as = strstr(ao, "://");
    size_t scheme_n = (size_t)(as - ao);
    const char *ahost = as + 3;
    /* scheme-source: "https:" */
    if (n >= 2 && src[n - 1] == ':' && !memchr(src, '/', n))
        return scheme_n == n - 1 && au_ieq(src, n - 1, ao, scheme_n);
    /* host-source: [scheme://]host[:port][/path], host may start "*." */
    const char *h = src;
    size_t hn = n;
    const char *sep = NULL;
    for (size_t i = 0; i + 2 < n; i++)
        if (src[i] == ':' && src[i + 1] == '/' && src[i + 2] == '/') {
            sep = src + i;
            break;
        }
    if (sep) {
        size_t sn = (size_t)(sep - src);
        if (!(sn == scheme_n && au_ieq(src, sn, ao, scheme_n)) &&
            !(sn == 4 && au_ieq(src, 4, "http", 4) && scheme_n == 5)) /* http: also allows https: */
            return false;
        h = sep + 3;
        hn = n - (size_t)(h - src);
    }
    const char *slash = memchr(h, '/', hn);
    if (slash)
        hn = (size_t)(slash - h);
    char want[300], have[300];
    if (hn == 0 || hn >= sizeof want)
        return false;
    memcpy(want, h, hn);
    want[hn] = 0;
    snprintf(have, sizeof have, "%s", ahost);
    char *wp = strrchr(want, ':'), *hp = strrchr(have, ':');
    if (wp && strchr(wp, ']'))
        wp = NULL;
    if (hp && strchr(hp, ']'))
        hp = NULL;
    if (wp) {
        const char *port = hp ? hp + 1 : scheme_n == 5 ? "443" : "80";
        if (strcmp(wp + 1, "*") != 0 && strcmp(wp + 1, port) != 0)
            return false;
        *wp = 0;
    }
    if (hp)
        *hp = 0;
    if (want[0] == '*' && want[1] == '.') {
        size_t wl = strlen(want + 1), hl = strlen(have);
        return hl > wl && au_ieq(have + hl - wl, wl, want + 1, wl);
    }
    return ieq(want, have);
}

/* Whether the response lets url be shown in f: X-Frame-Options (DENY,
 * SAMEORIGIN) and the CSP frame-ancestors directive ('none', 'self',
 * scheme and host sources), each checked against every ancestor. When
 * frame-ancestors is present X-Frame-Options is ignored, as the CSP
 * standard says. Other CSP directives are not enforced. */
static bool framing_allowed(AuFrame *f, const char *url, const char *xfo, const char *csp)
{
    const char *fa = NULL;
    size_t fan = 0;
    bool has_fa = false;
    /* every policy in the header (comma-separated) must allow it */
    for (const char *p = csp; p && *p;) {
        size_t pn = strcspn(p, ",");
        for (const char *d = p; d < p + pn;) {
            size_t dn = 0;
            while (d + dn < p + pn && d[dn] != ';')
                dn++;
            const char *t = d;
            size_t tn = dn;
            while (tn && au_is_space((unsigned char)*t)) {
                t++;
                tn--;
            }
            if (tn >= 15 && au_ieq(t, 15, "frame-ancestors", 15) && (tn == 15 || au_is_space((unsigned char)t[15]))) {
                has_fa = true;
                fa = t + 15;
                fan = tn - 15;
                for (AuDocView *a = f->owner; a; a = a->parent) {
                    bool ok = false;
                    for (size_t i = 0; i < fan && !ok;) {
                        while (i < fan && au_is_space((unsigned char)fa[i]))
                            i++;
                        size_t s = i;
                        while (i < fan && !au_is_space((unsigned char)fa[i]))
                            i++;
                        if (i > s && !(i - s == 6 && au_ieq(fa + s, 6, "'none'", 6)))
                            ok = source_matches(fa + s, i - s, url, a->url);
                    }
                    if (!ok)
                        return false;
                }
            }
            d += dn + (d + dn < p + pn ? 1 : 0);
        }
        p += pn + (p[pn] ? 1 : 0);
    }
    if (has_fa || !xfo)
        return true;
    while (au_is_space((unsigned char)*xfo))
        xfo++;
    size_t n = strcspn(xfo, " \t,;");
    if (au_ieq(xfo, n, "deny", 4))
        return false;
    if (au_ieq(xfo, n, "sameorigin", 10))
        for (AuDocView *a = f->owner; a; a = a->parent)
            if (!same_origin_url(url, a->url))
                return false;
    return true;
}

void au_frames_head(AuDocView *dv, AuFrame *f, int status, const char *final_url, const char *const *headers,
                    size_t nheaders)
{
    (void)dv;
    if (f->dead || f->got_head)
        return;
    f->got_head = true;
    f->status = status;
    const char *ct = header(headers, nheaders, "content-type");
    f->ctype = ct ? dup(ct) : NULL;
    f->final_url = final_url && *final_url ? dup(final_url) : NULL;
    const char *url = f->final_url ? f->final_url : f->url;
    if (!framing_allowed(f, url, header(headers, nheaders, "x-frame-options"),
                         header(headers, nheaders, "content-security-policy"))) {
        console(f->owner, "Frame not shown: %s does not allow framing by this page%s", url, "");
        frame_failed(f, FS_BLOCKED);
        return;
    }
    if (f->final_url && is_ancestor_url(f, f->final_url)) {
        console(f->owner, "Frame not loaded: %s is one of its own ancestors%s", url, "");
        frame_failed(f, FS_REFUSED);
        return;
    }
    if (f->kind == FK_OBJECT && status >= 400)
        frame_failed(f, FS_FALLBACK);
}

static void html_escape(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '<')
            au_buf_puts(out, "&lt;");
        else if (c == '&')
            au_buf_puts(out, "&amp;");
        else if (c == 0)
            au_buf_puts(out, "\xEF\xBF\xBD");
        else
            au_buf_putc(out, c);
    }
}

/* What the bytes are: 0 HTML, 1 text, 2 image, 3 something else. */
static int content_class(const AuFrame *f, const char *data, size_t len)
{
    const char *ct = f->ctype;
    const char *url = f->final_url ? f->final_url : f->url;
    if (ct && *ct) {
        if (starts_ci(ct, "text/html") || starts_ci(ct, "application/xhtml+xml"))
            return 0;
        if (starts_ci(ct, "image/"))
            return 2;
        if (starts_ci(ct, "text/") || starts_ci(ct, "application/json") || starts_ci(ct, "application/xml") ||
            starts_ci(ct, "application/javascript"))
            return 1;
        return 3;
    }
    /* no head (a file, or a loader that reports none): the URL, then the bytes */
    if (is_image_url(url))
        return 2;
    const unsigned char *b = (const unsigned char *)data;
    if (len >= 4 && ((b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') ||
                     (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) || !memcmp(b, "GIF8", 4) ||
                     (len >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4))))
        return 2;
    size_t n = strcspn(url, "?#");
    if (n >= 4 && (au_ieq(url + n - 4, 4, ".txt", 4) || au_ieq(url + n - 4, 4, ".css", 4)))
        return 1;
    if (n >= 3 && au_ieq(url + n - 3, 3, ".js", 3))
        return 1;
    return 0;
}

/* The first bytes (or the end of an empty response): decides what the
 * element shows and starts the nested document. False when the load was
 * turned into something else (an image, the fallback). */
static bool frame_begin(AuFrame *f, const char *data, size_t len)
{
    const char *url = f->final_url ? f->final_url : f->url;
    int cls = content_class(f, data, len);
    bool embed = f->kind == FK_OBJECT || f->kind == FK_EMBED;
    if (cls == 3 || (embed && cls == 2)) {
        /* An object's image is laid out as an image box (layout fetches
         * it by URL; the HTTP cache has it). Another type has no viewer. */
        drop_request(f);
        if (embed && cls == 2) {
            set_state(f, FS_IMAGE);
            f->load_due = true;
            work(f->owner);
        } else {
            frame_failed(f, FS_BLOCKED);
        }
        return false;
    }
    if (cls == 2) {
        drop_request(f);
        AuBuf h;
        au_buf_init(&h);
        au_buf_puts(&h, "<!DOCTYPE html><body style=\"margin:0\"><img src=\"");
        html_escape(&h, url, strlen(url));
        au_buf_puts(&h, "\">");
        set_state(f, FS_DOC);
        f->begun = true;
        frame_show_html(f, url, au_buf_cstr(&h));
        au_buf_free(&h);
        return false;
    }
    AuDocView *c = frame_view(f);
    if (!c) {
        frame_failed(f, FS_REFUSED);
        return false;
    }
    set_state(f, FS_DOC);
    f->begun = true;
    f->pre = cls == 1;
    f->sx = f->sy = 0;
    au_docview_begin(c, url);
    if (f->pre) {
        static const char pre[] = "<!DOCTYPE html><body><pre style=\"white-space:pre-wrap\">";
        au_docview_feed(c, pre, sizeof pre - 1);
    }
    invalidate_frame(f);
    return true;
}

static void frame_feed(AuFrame *f, const char *data, size_t len)
{
    if (!f->pre) {
        au_docview_feed(f->dv, data, len);
        return;
    }
    AuBuf e;
    au_buf_init(&e);
    html_escape(&e, data, len);
    au_docview_feed(f->dv, e.data ? e.data : "", e.len);
    au_buf_free(&e);
}

void au_frames_data(AuDocView *dv, AuFrame *f, const void *data, size_t len)
{
    (void)dv;
    if (f->dead || !f->req || !len)
        return;
    if (!f->begun && !frame_begin(f, data, len))
        return;
    if (f->dv && f->begun && f->req)
        frame_feed(f, data, len);
}

void au_frames_done(AuDocView *dv, AuFrame *f, bool ok)
{
    (void)dv;
    if (f->dead || !f->req)
        return;
    f->req = 0; /* the owner dropped the request */
    if (!f->begun) {
        if (!ok) {
            frame_failed(f, FS_BLOCKED);
            return;
        }
        if (!frame_begin(f, "", 0))
            return;
    }
    if (f->dv)
        au_docview_finish(f->dv);
}

/* ---- The frame list ---- */

static void frame_free(AuFrame *f)
{
    AuDocView *o = f->owner, *top = au_dv_top(o);
    drop_request(f);
    if (o->focus_frame == f)
        o->focus_frame = NULL;
    if (o->mouse_frame == f)
        o->mouse_frame = NULL;
    if (o->hover_frame == f)
        o->hover_frame = NULL;
    if (f->dv) {
        AuDocView *c = f->dv;
        f->dv = NULL;
        au_docview_free(c);
    }
    if (f->counted && top->frame_total)
        top->frame_total--;
    au_node_unref(&f->el->node);
    au_free(f->key);
    au_free(f->url);
    au_free(f->ctype);
    au_free(f->final_url);
    au_free(f->nav_url);
    au_free(f->nav_body);
    au_free(f->nav_type);
    au_free(f);
}

void au_frames_clear(AuDocView *dv)
{
    AuDocView *top = au_dv_top(dv);
    while (dv->nframes) {
        AuFrame *f = dv->frames[--dv->nframes];
        frame_free(f);
    }
    /* Removed frames of this document that still wait. Each is taken
     * off the list before it is freed: freeing it clears its own nested
     * view, which edits the same list. */
    for (;;) {
        AuFrame *f = NULL;
        for (uint32_t i = 0; i < top->ndead && !f; i++)
            if (top->dead[i]->owner == dv) {
                f = top->dead[i];
                top->dead[i] = top->dead[--top->ndead];
            }
        if (!f)
            break;
        frame_free(f);
    }
    dv->focus_frame = dv->mouse_frame = dv->hover_frame = NULL;
    dv->frames_dirty = false;
}

static void frame_kill(AuFrame *f)
{
    AuDocView *o = f->owner, *top = au_dv_top(o);
    f->dead = true;
    if (o->focus_frame == f)
        o->focus_frame = NULL;
    if (o->mouse_frame == f)
        o->mouse_frame = NULL;
    if (o->hover_frame == f)
        o->hover_frame = NULL;
    if (top->ndead == top->dead_cap) {
        top->dead_cap = top->dead_cap ? top->dead_cap * 2 : 8;
        top->dead = au_realloc(top->dead, top->dead_cap * sizeof *top->dead);
    }
    top->dead[top->ndead++] = f;
    top->frames_work = true;
}

void au_frames_sync(AuDocView *dv)
{
    if (!dv->frames_dirty || !dv->doc)
        return;
    dv->frames_dirty = false;
    AuFrame **old = dv->frames;
    uint32_t nold = dv->nframes;
    AuFrame **list = NULL;
    uint32_t n = 0, cap = 0;
    bool changed = false;
    /* The document's frame elements in tree order. */
    uint32_t left = nold; /* old records not met yet */
    for (AuNode *c = au_node_next(&dv->doc->node, &dv->doc->node); c; c = au_node_next(c, &dv->doc->node)) {
        /* Every old record is placed and no new one may be made: the rest
         * of the document changes nothing. Without this each sync looked
         * every further frame element up in the old list, and a script
         * that kept inserting iframes (one sync per insertion) slowed
         * down so fast that its time limit was never checked. */
        if (left == 0 && n >= 4 * AU_FRAMES_MAX)
            break;
        int kind = frame_kind(c);
        if (kind < 0)
            continue;
        AuElement *el = au_as_element(c);
        AuFrame *f = NULL;
        for (uint32_t i = 0; left && i < nold; i++)
            if (old[i] && old[i]->el == el) {
                f = old[i];
                old[i] = NULL;
                left--;
                if (i != n)
                    changed = true;
                break;
            }
        if (!f) {
            if (n >= 4 * AU_FRAMES_MAX)
                continue; /* records for elements, most of them without a document */
            f = au_calloc(sizeof *f);
            f->owner = dv;
            f->el = el;
            au_node_ref(&el->node);
            f->kind = (uint8_t)kind;
            changed = true;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            list = au_realloc(list, cap * sizeof *list);
        }
        list[n++] = f;
    }
    for (uint32_t i = 0; i < nold; i++)
        if (old[i]) {
            frame_kill(old[i]);
            changed = true;
        }
    au_free(old);
    dv->frames = list;
    dv->nframes = n;
    dv->frames_cap = cap;
    /* New frames and frames whose attributes changed load. */
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        char *key = frame_key(f);
        if (!f->key || strcmp(key, f->key) != 0) {
            au_free(f->key);
            f->key = key;
            frame_start(f, true);
        } else {
            au_free(key);
        }
    }
    if (changed && dv->script)
        au_docscript_frames_changed(dv->script);
}

/* ---- Deferred work ---- */

static void fire_frame_event(AuFrame *f, const char *type)
{
    AuDocView *o = f->owner;
    if (o->script && au_docscript_running(o->script)) {
        AuElement *el = f->el;
        au_node_ref(&el->node);
        au_docscript_fire(o->script, el, type, false, false);
        au_node_unref(&el->node);
    }
}

static void do_navigation(AuFrame *f)
{
    char *url = f->nav_url, *body = f->nav_body, *type = f->nav_type;
    size_t len = f->nav_len;
    bool post = f->nav_post;
    f->nav_url = f->nav_body = f->nav_type = NULL;
    AuDocView *c = f->dv;
    if (starts_ci(url, "javascript:")) {
        if (c && c->script)
            au_docscript_javascript_url(c->script, url);
    } else if (!post && c && c->doc && f->state == FS_DOC && !f->req && strchr(url, '#') &&
               base_len(url) == base_len(c->url) && !strncmp(url, c->url, base_len(url))) {
        /* the same document: scroll to the fragment */
        int y = au_docview_fragment_y(c, strchr(url, '#'));
        if (y >= 0) {
            f->sy = y;
            clamp_scroll(f);
            au_docview_set_scroll(c, f->sx, f->sy);
            invalidate_frame(f);
        }
    } else if (starts_ci(url, "about:blank")) {
        drop_request(f);
        set_state(f, FS_NONE);
        frame_show_html(f, "about:blank", "");
    } else {
        frame_fetch(f, url, post, body, len, type);
    }
    au_free(url);
    au_free(body);
    au_free(type);
}

/* Does one piece of waiting work in dv's subtree; false when none is
 * left. One piece at a time: each may run script that changes the tree. */
static bool idle_step(AuDocView *dv)
{
    if (dv->script && au_docscript_flush_nav(dv->script))
        return true;
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        if (f->restart) {
            frame_start(f, false);
            return true;
        }
        if (f->nav_url) {
            do_navigation(f);
            return true;
        }
        if (f->error_due) {
            f->error_due = false;
            fire_frame_event(f, "error");
            au_dv_check_finished(dv);
            return true;
        }
        if (f->load_due && !f->req && !(f->dv && au_docview_loading(f->dv))) {
            f->load_due = false;
            fire_frame_event(f, "load");
            au_dv_check_finished(dv);
            return true;
        }
        if (f->dv && idle_step(f->dv))
            return true;
    }
    return false;
}

void au_frames_idle(AuDocView *dv)
{
    AuDocView *top = au_dv_top(dv);
    if (!top->frames_work || top->in_frames_idle || !safe(top))
        return;
    top->in_frames_idle = true;
    top->frames_work = false;
    for (int guard = 0; guard < 512; guard++) {
        while (top->ndead) {
            AuFrame *f = top->dead[--top->ndead];
            frame_free(f);
        }
        if (!idle_step(top) && !top->ndead)
            break;
        if (guard == 511)
            top->frames_work = true; /* more next time */
    }
    top->in_frames_idle = false;
}

bool au_frames_loading(const AuDocView *dv)
{
    for (uint32_t i = 0; i < dv->nframes; i++) {
        const AuFrame *f = dv->frames[i];
        if (f->req || f->load_due || f->error_due || f->restart || f->nav_url ||
            (f->dv && !f->quiet && au_docview_loading(f->dv)))
            return true;
    }
    return false;
}

/* ---- Navigation ---- */

static AuDocView *find_named(AuDocView *dv, const char *name)
{
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        if (f->kind == FK_IFRAME || f->kind == FK_FRAME) {
            const char *n = attr(f->el, "name");
            if (n && !strcmp(n, name)) {
                AuDocView *v = au_frames_view_of(dv, f->el);
                if (v)
                    return v;
            }
        }
        AuDocView *r = f->dv ? find_named(f->dv, name) : NULL;
        if (r)
            return r;
    }
    return NULL;
}

int au_frames_route(AuDocView *dv, const char *target, bool new_tab, bool named_new, AuDocView **out)
{
    AuDocView *top = au_dv_top(dv);
    bool no_popups = (dv->sandbox & AU_SANDBOX_POPUPS) != 0;
    if (new_tab || ieq(target, "_blank"))
        return no_popups ? 0 : 3;
    AuDocView *t = dv;
    if (!target || !*target || ieq(target, "_self")) {
        t = dv;
    } else if (ieq(target, "_parent")) {
        t = dv->parent ? dv->parent : dv;
    } else if (ieq(target, "_top")) {
        t = top;
    } else {
        AuDocView *named = find_named(top, target);
        if (named)
            t = named;
        else if (named_new)
            return no_popups ? 0 : 3;
    }
    if ((dv->sandbox & AU_SANDBOX_TOP_NAV) && t != dv) {
        bool inside = false;
        for (AuDocView *a = t; a; a = a->parent)
            if (a == dv)
                inside = true;
        if (!inside)
            return 0;
    }
    if (!t->parent)
        return 2;
    *out = t;
    return 1;
}

void au_frames_navigate(AuDocView *dv, const char *url, const AuFormSubmission *s)
{
    AuFrame *f = dv->in_frame;
    if (!f || f->dead)
        return;
    au_free(f->nav_url);
    au_free(f->nav_body);
    au_free(f->nav_type);
    f->nav_url = dup(url);
    f->nav_body = f->nav_type = NULL;
    f->nav_len = 0;
    f->nav_post = s && s->method && !strcmp(s->method, "POST");
    if (f->nav_post) {
        f->nav_body = au_alloc(s->body_len + 1);
        memcpy(f->nav_body, s->body ? s->body : "", s->body_len);
        f->nav_len = s->body_len;
        f->nav_type = s->body_type ? dup(s->body_type) : NULL;
    }
    work(dv);
}

/* ---- Script ---- */

static bool is_window(const AuFrame *f)
{
    if (f->state == FS_REFUSED)
        return false;
    if (f->kind == FK_IFRAME || f->kind == FK_FRAME)
        return true;
    return f->state == FS_DOC && f->dv;
}

int au_frames_window_count(AuDocView *dv)
{
    au_frames_sync(dv);
    int n = 0;
    for (uint32_t i = 0; i < dv->nframes; i++)
        n += is_window(dv->frames[i]);
    return n;
}

AuElement *au_frames_window_at(AuDocView *dv, int idx)
{
    au_frames_sync(dv);
    for (uint32_t i = 0; i < dv->nframes; i++)
        if (is_window(dv->frames[i]) && idx-- == 0)
            return dv->frames[i]->el;
    return NULL;
}

AuDocView *au_frames_view_of(AuDocView *dv, AuElement *el)
{
    au_frames_sync(dv);
    AuFrame *f = frame_of(dv, el);
    if (!f || !is_window(f))
        return NULL;
    if (!f->dv) {
        /* A blank document until the frame's own arrives. */
        AuDocView *c = frame_view(f);
        if (!c)
            return NULL;
        f->quiet = true;
        au_docview_begin(c, "about:blank");
        au_docview_finish(c);
        f->quiet = false;
    }
    return f->dv;
}

AuElement *au_frames_element(const AuDocView *dv)
{
    return dv->in_frame && !dv->in_frame->dead ? dv->in_frame->el : NULL;
}

/* ---- Layout and painting ---- */

int au_frames_embed_kind(AuDocView *dv, AuElement *el)
{
    AuFrame *f = frame_of(dv, el);
    if (!f)
        return 0;
    switch (f->state) {
    case FS_IMAGE:
        return 0;
    case FS_FALLBACK:
        return 2;
    default:
        return 1;
    }
}

void au_frames_layout(AuDocView *dv)
{
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        AuRect cr, br;
        if (!au_layout_element_content_rect(dv->layout, f->el, &cr) ||
            !au_layout_element_rect(dv->layout, f->el, &br)) {
            f->has_box = false;
            continue;
        }
        f->has_box = true;
        f->box = cr;
        f->off_x = cr.x - br.x;
        f->off_y = cr.y - br.y;
        if (f->dv && cr.w >= 1 && cr.h >= 1)
            au_docview_set_viewport(f->dv, (int)cr.w, (int)cr.h);
        if (f->state == FS_LAZY && near_viewport(f) && f->url) {
            char *url = f->url;
            f->url = NULL;
            frame_fetch(f, url, false, NULL, 0, NULL);
            au_free(url);
        }
    }
}

static bool wants_bars(const AuFrame *f)
{
    const char *s = attr(f->el, "scrolling");
    return !(s && (ieq(s, "no") || ieq(s, "off")));
}

void au_frames_paint(AuDocView *dv, AuElement *el, AuCanvas *c, AuRect content)
{
    AuFrame *f = frame_of(dv, el);
    if (!f)
        return;
    if (f->state == FS_LAZY && f->url) { /* it is being shown: load it */
        char *url = f->url;
        f->url = NULL;
        frame_fetch(f, url, false, NULL, 0, NULL);
        au_free(url);
    }
    if (!f->dv || !f->dv->doc)
        return;
    /* whole pixels: the nested document paints as a page does */
    AuRect r = {floorf(content.x + 0.5f), floorf(content.y + 0.5f), floorf(content.w), floorf(content.h)};
    if (r.w < 1 || r.h < 1)
        return;
    au_docview_set_viewport(f->dv, (int)r.w, (int)r.h);
    int cw, ch;
    au_docview_content_size(f->dv, &cw, &ch);
    clamp_scroll(f);
    au_docview_paint(f->dv, c, r, f->sx, f->sy);
    if (!wants_bars(f))
        return;
    /* Scroll bars, drawn as the page's overflow scrollers are. */
    AuRadii rad = {{3, 3, 3, 3}, {3, 3, 3, 3}};
    au_canvas_clip_push(c, r);
    if (ch > (int)r.h && r.h > 8) {
        float th = r.h * r.h / (float)ch;
        if (th < 16)
            th = 16;
        float ty = r.y + (r.h - th) * ((float)f->sy / (float)(ch - (int)r.h));
        au_fill_rect(c, (AuRect){r.x + r.w - 8, r.y, 8, r.h}, AU_RGBA(0, 0, 0, 20));
        au_fill_rounded_rect(c, (AuRect){r.x + r.w - 7, ty + 1, 6, th - 2}, &rad, AU_RGBA(0, 0, 0, 110));
    }
    if (cw > (int)r.w && r.w > 8) {
        float tw = r.w * r.w / (float)cw;
        if (tw < 16)
            tw = 16;
        float tx = r.x + (r.w - tw) * ((float)f->sx / (float)(cw - (int)r.w));
        au_fill_rect(c, (AuRect){r.x, r.y + r.h - 8, r.w, 8}, AU_RGBA(0, 0, 0, 20));
        au_fill_rounded_rect(c, (AuRect){tx + 1, r.y + r.h - 7, tw - 2, 6}, &rad, AU_RGBA(0, 0, 0, 110));
    }
    au_canvas_clip_pop(c);
}

/* ---- Input ---- */

/* The frame with a nested view under document point (x, y) of dv, and
 * the point in the nested document's coordinates. */
static AuFrame *frame_at(AuDocView *dv, int x, int y, int *cx, int *cy)
{
    if (!dv->nframes || !dv->doc)
        return NULL;
    AuHitResult r;
    if (!au_docview_hit_test(dv, x, y, &r) || !r.element)
        return NULL;
    AuFrame *f = frame_of(dv, r.element);
    if (!f || !f->dv || !f->dv->doc || !f->has_box)
        return NULL;
    float lx = r.local_x - f->off_x, ly = r.local_y - f->off_y;
    if (lx < 0 || ly < 0 || lx >= f->box.w || ly >= f->box.h)
        return NULL; /* on the border or padding */
    *cx = (int)lx + f->sx;
    *cy = (int)ly + f->sy;
    return f;
}

static void leave_frame(AuDocView *dv)
{
    AuFrame *f = dv->hover_frame;
    dv->hover_frame = NULL;
    if (f && f->dv)
        au_dv_mouse_leave(f->dv);
}

void au_frames_blur(AuDocView *dv)
{
    AuFrame *f = dv->focus_frame;
    dv->focus_frame = NULL;
    if (f && f->dv) {
        au_frames_blur(f->dv);
        if (f->dv->focus)
            au_docview_set_focus(f->dv, NULL);
    }
}

bool au_frames_mouse(AuDocView *dv, const AuEvent *ev, bool *used)
{
    *used = false;
    if (!dv->nframes && !dv->mouse_frame && !dv->hover_frame)
        return false;
    int cx = 0, cy = 0;
    AuFrame *f = dv->mouse_frame;
    if (f && (!f->dv || f->dead))
        f = dv->mouse_frame = NULL;
    if (f) {
        /* a press that began in the frame: it gets the drag and the release */
        cx = ev->x - (int)f->box.x + f->sx;
        cy = ev->y - (int)f->box.y + f->sy;
    } else {
        f = frame_at(dv, ev->x, ev->y, &cx, &cy);
    }
    if (!f) {
        if (dv->hover_frame)
            leave_frame(dv);
        if (ev->type == AU_EVENT_MOUSE_DOWN && dv->focus_frame)
            au_frames_blur(dv);
        return false;
    }
    if (dv->hover_frame != f) {
        leave_frame(dv);
        dv->hover_frame = f;
        au_dv_set_hover(dv, f->el);
        if (dv->hover_link[0]) {
            dv->hover_link[0] = 0;
            if (dv->cb.status_text)
                dv->cb.status_text(dv->cb.ctx, dv, "");
        }
    }
    if (ev->type == AU_EVENT_MOUSE_DOWN) {
        dv->mouse_frame = f;
        if (dv->focus_frame != f) {
            au_frames_blur(dv);
            if (dv->focus)
                au_docview_set_focus(dv, NULL);
            if (!f->dead && f->dv)
                dv->focus_frame = f;
        }
    } else if (ev->type == AU_EVENT_MOUSE_UP) {
        dv->mouse_frame = NULL;
    }
    if (f->dead || !f->dv)
        return true;
    AuEvent e = *ev;
    e.x = cx;
    e.y = cy;
    au_docview_set_scroll(f->dv, f->sx, f->sy);
    *used = au_docview_mouse(f->dv, &e);
    if (dv->doc && dv->restyle_needed) {
        au_docview_update(dv);
        if (au_layout_needs_paint(dv->layout))
            au_dv_invalidate_all(dv);
    }
    return true;
}

static bool frame_scroll_by(AuFrame *f, int dx, int dy)
{
    if (!f->dv)
        return false;
    int ox = f->sx, oy = f->sy;
    au_docview_update(f->dv);
    f->sx += dx;
    f->sy += dy;
    clamp_scroll(f);
    if (ox == f->sx && oy == f->sy)
        return false;
    au_docview_set_scroll(f->dv, f->sx, f->sy);
    invalidate_frame(f);
    return true;
}

bool au_frames_key(AuDocView *dv, const AuEvent *ev, bool *used)
{
    AuFrame *f = dv->focus_frame;
    if (!f || f->dead || !f->dv || !f->dv->doc)
        return false;
    *used = au_docview_key(f->dv, ev);
    if (*used || ev->type != AU_EVENT_KEY_DOWN || (ev->mods & AU_MOD_COMMAND) || f->dead || !f->dv)
        return true;
    /* Keys the nested document did not use scroll the frame; at its end
     * they go on to the page. */
    int page = f->dv->vh - 40 > 20 ? f->dv->vh - 40 : 20;
    switch (ev->key) {
    case AU_KEY_DOWN: *used = frame_scroll_by(f, 0, 40); break;
    case AU_KEY_UP: *used = frame_scroll_by(f, 0, -40); break;
    case AU_KEY_LEFT: *used = frame_scroll_by(f, -40, 0); break;
    case AU_KEY_RIGHT: *used = frame_scroll_by(f, 40, 0); break;
    case AU_KEY_PAGE_DOWN: *used = frame_scroll_by(f, 0, page); break;
    case AU_KEY_PAGE_UP: *used = frame_scroll_by(f, 0, -page); break;
    case AU_KEY_HOME: *used = frame_scroll_by(f, 0, -1000000); break;
    case AU_KEY_END: *used = frame_scroll_by(f, 0, 1000000); break;
    default:
        if (ev->ch == ' ')
            *used = frame_scroll_by(f, 0, (ev->mods & AU_MOD_SHIFT) ? -page : page);
        break;
    }
    return true;
}

bool au_frames_scroll(AuDocView *dv, int x, int y, int dx, int dy)
{
    int cx, cy;
    AuFrame *f = frame_at(dv, x, y, &cx, &cy);
    if (!f)
        return false;
    au_docview_set_scroll(f->dv, f->sx, f->sy);
    if (au_docview_scroll_at(f->dv, cx, cy, dx, dy))
        return true;
    return wants_bars(f) && frame_scroll_by(f, dx, dy);
}

/* ---- The public queries (page.h) ---- */

AuDocView *au_docview_view_at(AuDocView *dv, int *x, int *y)
{
    for (int depth = 0; depth <= AU_FRAMES_DEPTH; depth++) {
        int cx, cy;
        au_docview_update(dv);
        AuFrame *f = frame_at(dv, *x, *y, &cx, &cy);
        if (!f)
            break;
        dv = f->dv;
        *x = cx;
        *y = cy;
    }
    return dv;
}

AuDocView *au_docview_focus_view(AuDocView *dv)
{
    for (int depth = 0; depth <= AU_FRAMES_DEPTH; depth++) {
        AuFrame *f = dv->focus_frame;
        if (!f || f->dead || !f->dv || !f->dv->doc)
            break;
        dv = f->dv;
    }
    return dv;
}

AuDocView *au_docview_parent(const AuDocView *dv)
{
    return dv->parent;
}

AuIRect au_docview_rect_to_top(const AuDocView *dv, AuIRect r)
{
    for (; dv->parent; dv = dv->parent) {
        const AuFrame *f = dv->in_frame;
        if (!f || !f->has_box)
            break;
        r = frame_rect_in_owner(f, r);
    }
    return r;
}

bool au_docview_contains(AuDocView *dv, const AuDocView *v)
{
    if (dv == v)
        return true;
    for (uint32_t i = 0; i < dv->nframes; i++)
        if (dv->frames[i]->dv && au_docview_contains(dv->frames[i]->dv, v))
            return true;
    return false;
}

void au_docview_point_from_top(const AuDocView *dv, int *x, int *y)
{
    const AuFrame *chain[AU_FRAMES_DEPTH + 1];
    int n = 0;
    for (; dv->parent && dv->in_frame && n <= AU_FRAMES_DEPTH; dv = dv->parent)
        chain[n++] = dv->in_frame;
    while (n--) {
        *x = *x - (int)chain[n]->box.x + chain[n]->sx;
        *y = *y - (int)chain[n]->box.y + chain[n]->sy;
    }
}

void au_docview_scroll_pos(const AuDocView *dv, int *x, int *y)
{
    *x = dv->in_frame ? dv->in_frame->sx : dv->scroll_x;
    *y = dv->in_frame ? dv->in_frame->sy : dv->scroll_y;
}

uint32_t au_docview_frame_count(const AuDocView *dv)
{
    while (dv->parent)
        dv = dv->parent;
    return dv->frame_total;
}

/* ---- Sums over the frames ---- */

bool au_frames_can_persist(AuDocView *dv)
{
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        if (f->req || f->nav_url || f->restart)
            return false;
        if (f->dv && f->dv->doc && !au_docview_can_persist(f->dv))
            return false;
    }
    return au_dv_top(dv)->ndead == 0 || dv->parent;
}

/* Brings the frame list of dv and of every nested view up to date. Script
 * calls it before it runs, so that window.frames, window[0] and
 * window["name"] name the frames the parser has made so far: a frame's
 * script reads parent.frames["sibling"] as soon as it starts. The depth
 * is bounded by the nesting limit. */
void au_frames_sync_all(AuDocView *dv)
{
    au_frames_sync(dv);
    for (uint32_t i = 0; i < dv->nframes; i++)
        if (dv->frames[i]->dv && dv->frames[i]->dv->doc)
            au_frames_sync_all(dv->frames[i]->dv);
}

void au_frames_set_hidden(AuDocView *dv, bool hidden)
{
    for (uint32_t i = 0; i < dv->nframes; i++)
        if (dv->frames[i]->dv && dv->frames[i]->dv->doc)
            au_docview_set_hidden(dv->frames[i]->dv, hidden);
}

size_t au_frames_memory(AuDocView *dv)
{
    size_t n = 0;
    for (uint32_t i = 0; i < dv->nframes; i++) {
        n += sizeof(AuFrame);
        if (dv->frames[i]->dv)
            n += au_docview_memory_estimate(dv->frames[i]->dv);
    }
    return n;
}

bool au_frames_run_scripts(AuDocView *dv, uint32_t budget_ms)
{
    bool ran = false;
    /* The list may change while script runs: read it afresh each time. */
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        if (f->dv && f->dv->doc && au_docview_run_scripts(f->dv, budget_ms))
            ran = true;
    }
    return ran;
}

uint32_t au_frames_scripts_due(AuDocView *dv)
{
    uint32_t due = UINT32_MAX;
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        if (!f->dv || !f->dv->doc)
            continue;
        uint32_t d = au_docview_scripts_due(f->dv);
        if (d < due)
            due = d;
    }
    return due;
}

uint32_t au_frames_animate(AuDocView *dv, uint32_t now_ms)
{
    uint32_t next = UINT32_MAX;
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuFrame *f = dv->frames[i];
        if (!f->dv || !f->dv->doc)
            continue;
        uint32_t d = au_docview_animate(f->dv, now_ms);
        if (d < next)
            next = d;
    }
    return next;
}

bool au_frames_animating(const AuDocView *dv)
{
    for (uint32_t i = 0; i < dv->nframes; i++)
        if (dv->frames[i]->dv && au_docview_animating(dv->frames[i]->dv))
            return true;
    return false;
}

void au_frames_settings_changed(AuDocView *dv)
{
    for (uint32_t i = 0; i < dv->nframes; i++) {
        AuDocView *c = dv->frames[i]->dv;
        if (!c)
            continue;
        au_docview_set_user_css(c, dv->user_css);
        if (dv->has_font_prefs)
            au_docview_set_font_prefs(c, &dv->font_prefs);
    }
}
