/* The page interface (page.h) on top of the document view (src/page).
 *
 *   about:blank, about:home, about:fonts
 *                 generated HTML, loaded through the engine inside
 *                 au_page_navigate
 *   about:scene   the paint test scene (scene.c), painted directly
 *   file:...      the file, read through AuPageEnv.read_file_url from
 *                 au_page_idle and fed to the parser in slices; HTML is
 *                 rendered, other text shown preformatted
 *   view-source:U the source of U as plain text
 *   http:, https: the fetcher AuPageEnv.fetcher returns for the scheme;
 *                 without one, a placeholder page after a simulated load
 *                 of a little over a second, so the reload, stop and
 *                 progress states can be seen
 *
 * Every byte from a file or a fetcher arrives from au_page_idle, never
 * from inside a request: the document view's loader only queues a Fetch.
 * Each idle call works for at most BUDGET_MS. Layout is not incremental,
 * so while a document loads it is laid out again only every
 * LAYOUT_INTERVAL_MS (more for pages whose layout is slow), and once at
 * the end. */
#include "page.h"
#include "scene.h"
#include "url.h"

#include "../layout/selection.h"
#include "../page/page.h"

#include <stdio.h>
#include <string.h>

#define SIM_LOAD_MS 1400
#define BUDGET_MS 40
#define CHUNK 8192
#define LAYOUT_INTERVAL_MS 300

typedef enum { K_DOC, K_SCENE } Kind;

typedef enum {
    MAIN_NONE, /* nothing to load (about: pages, or finished) */
    MAIN_FILE, /* a file: read and fed from idle */
    MAIN_FETCH, /* a fetcher delivers it */
    MAIN_SIM   /* the network placeholder's simulated load */
} MainSource;

typedef struct Fetch Fetch;
struct Fetch {
    AuPage *page;
    uint32_t id; /* the document view's resource id; 0 for the main document */
    bool main;
    char *url;
    /* A file: read on the first idle after the request. */
    bool is_file, read;
    char *data;
    size_t len, off;
    /* A fetcher's transfer. */
    const AuPageFetcher *fetcher;
    uint32_t fid;
    AuFetchSink sink;
    bool done, ok, cancelled;
};

struct AuPage {
    AuPageEnv env;
    AuPageCallbacks cb;
    AuDocView *dv;
    char *url, *title;
    Kind kind;
    uint32_t gen; /* bumped by every load, so idle work for an old load stops */

    /* The current load. */
    bool loading, failed;
    MainSource main;
    Fetch *main_fetch;
    bool source_mode; /* view-source: or a plain text file: show the bytes as text */
    bool plain;       /* the main document is plain text (decided on its first bytes) */
    bool begun;       /* the new document has been started in the view */
    bool main_done;
    uint32_t main_total; /* expected bytes of the main document, 0 if unknown */
    uint32_t load_start;
    float progress;

    Fetch **fetches; /* resources and the main document, in request order */
    size_t nfetches, fetches_cap;
    uint32_t next_id;
    uint32_t requested, completed;

    /* Layout. */
    bool in_idle, dirty;
    uint32_t last_layout;
    int vw, vh;
    int content_w, content_h;
    int scroll_x, scroll_y;

    /* Scrolling the page asks for once it can. */
    char *fragment;   /* fragment of the URL being loaded, or NULL */
    bool restore;     /* a back/forward or reload position */
    int restore_x, restore_y;

    /* A link activated inside au_docview_mouse, handled after it returns. */
    char *open_url;
    bool open_new_tab;

    float zoom; /* text zoom, 1 for none */

    AuBuf source; /* the main document's bytes as they arrived (Save Page As) */

    /* Text selection: anchor and focus hold a reference to their node. */
    AuTextPos anchor, focus;
    AuTextRange sel_base; /* the word or paragraph a double or triple click chose */
    int sel_mode;         /* 1 characters, 2 words, 3 paragraphs */
    bool selecting, sel_dragged;
    int down_x, down_y;

    AuPageStats stats;
};

/* ---- Helpers ---- */

static void set_str(char **f, const char *s)
{
    char *v = au_strndup(s ? s : "", s ? strlen(s) : 0);
    au_free(*f);
    *f = v;
}

static uint32_t now(AuPage *p)
{
    return p->env.time_ms ? p->env.time_ms(p->env.ctx) : 0;
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strlen(s) >= n && au_ieq(s, n, prefix, n);
}

static bool ends_with(const char *s, size_t n, const char *suffix)
{
    size_t m = strlen(suffix);
    return n >= m && au_ieq(s + n - m, m, suffix, m);
}

/* Length of url without its fragment. */
static size_t base_len(const char *url)
{
    const char *h = strchr(url, '#');
    return h ? (size_t)(h - url) : strlen(url);
}

static void html_escape(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&')
            au_buf_puts(out, "&amp;");
        else if (c == '<')
            au_buf_puts(out, "&lt;");
        else if (c == '>')
            au_buf_puts(out, "&gt;");
        else if (c == '"')
            au_buf_puts(out, "&quot;");
        else if (c == '\'')
            au_buf_puts(out, "&#39;");
        else
            au_buf_putc(out, c);
    }
}

static void html_escape_str(AuBuf *out, const char *s)
{
    html_escape(out, s, strlen(s));
}

static void invalidate_all(AuPage *p)
{
    if (p->cb.invalidate) {
        int w = p->content_w > p->vw ? p->content_w : p->vw;
        int h = p->content_h > p->vh ? p->content_h : p->vh;
        p->cb.invalidate(p->cb.ctx, p, (AuIRect){0, 0, w, h});
    }
}

static void set_title(AuPage *p, const char *t)
{
    if (!strcmp(p->title, t))
        return;
    set_str(&p->title, t);
    if (p->cb.title_changed)
        p->cb.title_changed(p->cb.ctx, p, p->title);
}

static void report_progress(AuPage *p)
{
    float main = p->main_done ? 1.0f : 0.0f;
    if (!p->main_done && p->main_total && p->main_fetch)
        main = (float)p->main_fetch->off / (float)p->main_total;
    float prog = (main + (float)p->completed) / (float)(1 + p->requested);
    if (prog > 1)
        prog = 1;
    prog = (float)(int)(prog * 20) / 20; /* visible steps only */
    if (prog != p->progress) {
        p->progress = prog;
        if (p->cb.load_progress)
            p->cb.load_progress(p->cb.ctx, p);
    }
}

/* ---- Fonts ---- */

static bool font_installed(AuFontSet *fs, const char *family)
{
    if (!fs || !family || !*family)
        return false;
    size_t n = strlen(family);
    for (size_t i = 0; i < au_fontset_count(fs); i++) {
        const char *f = au_fontset_entry(fs, i, NULL, NULL);
        if (f && strlen(f) == n && au_ieq(f, n, family, n))
            return true;
    }
    return false;
}

/* The preferences' families become the generic families (when they are
 * installed); the default sizes, the minimum size and the tab's text zoom
 * go to the resolver's font preferences. */
static void apply_font_prefs(AuPage *p)
{
    const AuPrefs *pr = p->env.prefs;
    AuCssFontPrefs fp;
    au_css_font_prefs_default(&fp);
    if (pr) {
        if (font_installed(p->env.fonts, pr->font_serif))
            au_fontset_set_generic(p->env.fonts, AU_GENERIC_SERIF, pr->font_serif);
        if (font_installed(p->env.fonts, pr->font_sans))
            au_fontset_set_generic(p->env.fonts, AU_GENERIC_SANS_SERIF, pr->font_sans);
        if (font_installed(p->env.fonts, pr->font_mono))
            au_fontset_set_generic(p->env.fonts, AU_GENERIC_MONOSPACE, pr->font_mono);
        if (pr->font_size > 0)
            fp.default_size = (float)pr->font_size;
        if (pr->font_size_mono > 0)
            fp.default_mono_size = (float)pr->font_size_mono;
        fp.min_size = pr->font_size_min > 0 ? (float)pr->font_size_min : 0;
    }
    fp.text_zoom = p->zoom > 0 ? p->zoom : 1;
    au_docview_set_font_prefs(p->dv, &fp);
    /* The initial family: the reader's serif face, as browsers default. */
    au_docview_set_user_css(p->dv, "html { font-family: serif; }\n");
}

/* ---- Image sizes ----
 * There are no image decoders yet. The intrinsic size from the file
 * header lets layout reserve the right space; a placeholder is painted. */

static uint32_t be32(const uint8_t *b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static uint32_t be16(const uint8_t *b) { return ((uint32_t)b[0] << 8) | b[1]; }
static uint32_t le16(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8); }

static bool sniff_image(void *ctx, const void *data, size_t len, int *w, int *h, AuSurface **pixels)
{
    (void)ctx;
    const uint8_t *d = data;
    *pixels = NULL;
    if (len >= 24 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        *w = (int)be32(d + 16);
        *h = (int)be32(d + 20);
        return true;
    }
    if (len >= 10 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) {
        *w = (int)le16(d + 6);
        *h = (int)le16(d + 8);
        return true;
    }
    if (len >= 26 && d[0] == 'B' && d[1] == 'M') {
        *w = (int)(d[18] | (d[19] << 8) | (d[20] << 16));
        int32_t hh = (int32_t)((uint32_t)d[22] | ((uint32_t)d[23] << 8) | ((uint32_t)d[24] << 16) | ((uint32_t)d[25] << 24));
        *h = hh < 0 ? (int)-hh : (int)hh;
        return true;
    }
    if (len >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < len) {
            if (d[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t m = d[i + 1];
            if (m == 0xD8 || m == 0x01 || m == 0xFF || (m >= 0xD0 && m <= 0xD7)) {
                i += m == 0xFF ? 1 : 2;
                continue;
            }
            size_t seg = be16(d + i + 2);
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = (int)be16(d + i + 5);
                *w = (int)be16(d + i + 7);
                return true;
            }
            if (seg < 2)
                return false;
            i += 2 + seg;
        }
        return false;
    }
    return false;
}

/* ---- Fetches ---- */

static void fetch_free(Fetch *f)
{
    au_free(f->url);
    au_free(f->data);
    au_free(f);
}

static Fetch *fetch_new(AuPage *p, const char *url, bool main)
{
    Fetch *f = au_calloc(sizeof *f);
    f->page = p;
    f->url = au_strndup(url, base_len(url)); /* a fragment is never fetched */
    f->main = main;
    if (p->nfetches == p->fetches_cap) {
        p->fetches_cap = p->fetches_cap ? p->fetches_cap * 2 : 16;
        p->fetches = au_realloc(p->fetches, p->fetches_cap * sizeof *p->fetches);
    }
    p->fetches[p->nfetches++] = f;
    return f;
}

static void fetch_cancel(Fetch *f)
{
    if (f->done || f->cancelled)
        return;
    f->cancelled = true;
    if (f->fetcher && f->fetcher->cancel)
        f->fetcher->cancel(f->fetcher->ctx, f->fid);
}

/* Drops finished and cancelled fetches. */
static void compact_fetches(AuPage *p)
{
    size_t k = 0;
    for (size_t i = 0; i < p->nfetches; i++) {
        Fetch *f = p->fetches[i];
        if (f->done || f->cancelled) {
            if (f == p->main_fetch)
                p->main_fetch = NULL;
            fetch_free(f);
        } else {
            p->fetches[k++] = f;
        }
    }
    p->nfetches = k;
}

static void main_data(AuPage *p, const char *bytes, size_t len);
static void main_done(AuPage *p, bool ok, const AuFetchInfo *info);

static void sink_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    Fetch *f = ctx;
    (void)id;
    if (f->done || f->cancelled)
        return;
    f->off += len;
    if (f->main)
        main_data(f->page, bytes, len);
    else
        au_docview_resource_data(f->page->dv, f->id, bytes, len);
    f->page->stats.bytes += (uint32_t)len;
}

static void sink_done(void *ctx, uint32_t id, bool ok, const AuFetchInfo *info)
{
    Fetch *f = ctx;
    (void)id;
    if (f->done || f->cancelled)
        return;
    f->done = true;
    f->ok = ok;
    AuPage *p = f->page;
    if (f->main) {
        main_done(p, ok, info);
    } else {
        p->completed++;
        au_docview_resource_done(p->dv, f->id, ok);
        report_progress(p);
    }
}

static const AuPageFetcher *fetcher_for(AuPage *p, const char *url)
{
    size_t n = au_url_scheme_len(url, strlen(url));
    if (!n || n > 15 || !p->env.fetcher)
        return NULL;
    char scheme[16];
    for (size_t i = 0; i < n; i++) {
        char c = url[i];
        scheme[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    scheme[n] = 0;
    return p->env.fetcher(p->env.ctx, scheme);
}

/* Starts fetching url from a file or a fetcher. Returns NULL when no
 * source can supply it. Nothing is delivered before the next idle. */
static Fetch *start_fetch(AuPage *p, const char *url, bool main)
{
    if (au_url_scheme_is(url, "file")) {
        Fetch *f = fetch_new(p, url, main);
        f->is_file = true;
        return f;
    }
    const AuPageFetcher *fe = fetcher_for(p, url);
    if (!fe || !fe->start)
        return NULL;
    Fetch *f = fetch_new(p, url, main);
    f->fetcher = fe;
    f->sink.ctx = f;
    f->sink.data = sink_data;
    f->sink.done = sink_done;
    f->fid = fe->start(fe->ctx, f->url, &f->sink);
    if (!f->fid) {
        f->cancelled = true; /* removed at the next compaction */
        return NULL;
    }
    return f;
}

/* The document view's loader. */
static uint32_t loader_request(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    AuPage *p = ctx;
    (void)dv, (void)kind;
    if (p->nfetches > 2000)
        return 0;
    Fetch *f = start_fetch(p, url, false);
    if (!f)
        return 0;
    f->id = ++p->next_id;
    p->requested++;
    p->stats.resources++;
    return f->id;
}

static void loader_cancel(void *ctx, uint32_t id)
{
    AuPage *p = ctx;
    for (size_t i = 0; i < p->nfetches; i++)
        if (!p->fetches[i]->main && p->fetches[i]->id == id)
            fetch_cancel(p->fetches[i]);
}

/* ---- Document view callbacks ---- */

static void dv_title(void *ctx, AuDocView *dv, const char *title)
{
    AuPage *p = ctx;
    (void)dv;
    if (p->kind == K_DOC)
        set_title(p, title);
}

static void dv_invalidate(void *ctx, AuDocView *dv, AuIRect r)
{
    AuPage *p = ctx;
    (void)dv;
    if (p->in_idle) {
        p->dirty = true; /* laid out and repainted at the end of the slice */
        return;
    }
    if (p->kind == K_DOC && p->cb.invalidate)
        p->cb.invalidate(p->cb.ctx, p, r);
}

static void dv_size(void *ctx, AuDocView *dv)
{
    AuPage *p = ctx;
    float w, h;
    au_layout_document_size(au_docview_layout(dv), &w, &h);
    int iw = (int)(w + 0.99f), ih = (int)(h + 0.99f);
    if (iw == p->content_w && ih == p->content_h)
        return;
    p->content_w = iw;
    p->content_h = ih;
    if (p->kind == K_DOC && p->cb.content_size_changed)
        p->cb.content_size_changed(p->cb.ctx, p);
}

static void dv_status(void *ctx, AuDocView *dv, const char *text)
{
    AuPage *p = ctx;
    (void)dv;
    if (p->cb.status_text)
        p->cb.status_text(p->cb.ctx, p, text);
}

static void dv_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    AuPage *p = ctx;
    (void)dv;
    set_str(&p->open_url, url);
    p->open_new_tab = new_tab;
}

/* ---- Pending scroll ---- */

static void request_scroll(AuPage *p, int x, int y)
{
    if (p->cb.scroll_to)
        p->cb.scroll_to(p->cb.ctx, p, x, y);
}

/* Applies a restore position once the document is tall enough, and a
 * fragment once its element exists; final at the end of the load. */
static void apply_pending_scroll(AuPage *p, bool final)
{
    if (p->restore && (final || p->restore_y + p->vh <= p->content_h)) {
        p->restore = false;
        au_free(p->fragment);
        p->fragment = NULL;
        request_scroll(p, p->restore_x, p->restore_y);
    }
    if (p->fragment && p->begun) {
        int y = au_docview_fragment_y(p->dv, p->fragment);
        if (y >= 0)
            request_scroll(p, 0, y);
        if (final) {
            au_free(p->fragment);
            p->fragment = NULL;
        }
    }
}

/* ---- Layout ---- */

static void run_layout(AuPage *p)
{
    uint32_t t0 = now(p);
    au_docview_update(p->dv);
    uint32_t dt = now(p) - t0;
    p->stats.layout_ms = dt;
    p->stats.layout_total_ms += dt;
    p->stats.layouts++;
    p->last_layout = now(p);
    p->dirty = false;
}

/* Lays out and repaints after loading work, throttled while loading. */
static void maybe_layout(AuPage *p, bool force)
{
    if (!p->begun || !p->dirty || p->kind != K_DOC)
        return;
    uint32_t interval = LAYOUT_INTERVAL_MS;
    if (p->stats.layout_ms * 4 > interval)
        interval = p->stats.layout_ms * 4;
    if (!force && p->stats.layouts > 0 && now(p) - p->last_layout < interval)
        return;
    run_layout(p);
    invalidate_all(p);
    apply_pending_scroll(p, false);
}

static void finish(AuPage *p, bool ok)
{
    p->loading = false;
    p->progress = 1;
    p->main = MAIN_NONE;
    p->stats.load_ms = now(p) - p->load_start;
    if (!p->stats.load_ms)
        p->stats.load_ms = 1;
    if (p->cb.load_finished)
        p->cb.load_finished(p->cb.ctx, p, ok);
}

/* Ends the load when the main document and every resource are in. */
static void check_finished(AuPage *p)
{
    if (!p->loading || !p->main_done || !p->begun || au_docview_loading(p->dv))
        return;
    p->dirty = true;
    maybe_layout(p, true);
    apply_pending_scroll(p, true);
    finish(p, !p->failed);
}

/* ---- Selection ----
 * The page keeps the anchor (where the selection started) and the focus
 * (where it ends, following the pointer); the layout paints the range
 * between them. Both positions hold a reference to their text node, so
 * the layout's unreferenced copy stays valid until the selection is
 * cleared, which happens before the document is replaced. */

#define DEFAULT_HIGHLIGHT 0xFFB3D7FFu /* light blue, as the Mac's default highlight */

static void set_pos(AuTextPos *dst, AuTextPos src)
{
    if (src.node)
        au_node_ref(src.node);
    if (dst->node)
        au_node_unref(dst->node);
    *dst = src;
}

static void invalidate_view(AuPage *p)
{
    if (p->cb.invalidate && p->kind == K_DOC)
        p->cb.invalidate(p->cb.ctx, p, (AuIRect){p->scroll_x, p->scroll_y, p->scroll_x + p->vw, p->scroll_y + p->vh});
}

static AuTextRange sel_range(const AuPage *p)
{
    return au_text_range_make(p->anchor, p->focus);
}

/* Hands the current selection to the layout and repaints. */
static void sel_apply(AuPage *p)
{
    AuTextRange r = sel_range(p);
    uint32_t col = p->env.highlight ? p->env.highlight : DEFAULT_HIGHLIGHT;
    au_layout_set_selection(au_docview_layout(p->dv), au_text_range_empty(&r) ? NULL : &r, col);
    invalidate_view(p);
}

static void sel_set(AuPage *p, AuTextPos anchor, AuTextPos focus)
{
    set_pos(&p->anchor, anchor);
    set_pos(&p->focus, focus);
    sel_apply(p);
}

void au_page_clear_selection(AuPage *p)
{
    bool had = p->anchor.node || p->focus.node;
    au_layout_set_selection(au_docview_layout(p->dv), NULL, 0);
    AuTextPos none = {NULL, 0};
    set_pos(&p->anchor, none);
    set_pos(&p->focus, none);
    p->sel_base.start = p->sel_base.end = none;
    p->selecting = false;
    if (had)
        invalidate_view(p);
}

bool au_page_has_selection(const AuPage *p)
{
    AuTextRange r = sel_range(p);
    return p->kind == K_DOC && !au_text_range_empty(&r);
}

bool au_page_selection_text(AuPage *p, AuBuf *out)
{
    if (!au_page_has_selection(p))
        return false;
    au_docview_update(p->dv);
    AuTextRange r = sel_range(p);
    size_t before = out->len;
    au_layout_range_text(au_docview_layout(p->dv), &r, out);
    return out->len > before;
}

void au_page_select_all(AuPage *p)
{
    if (p->kind != K_DOC || !p->begun)
        return;
    au_docview_update(p->dv);
    AuTextRange all;
    if (au_layout_all_text(au_docview_layout(p->dv), &all))
        sel_set(p, all.start, all.end);
}

/* Scrolls so document rectangle r is in view, if it is not. */
static void reveal(AuPage *p, AuRect r)
{
    int x = p->scroll_x, y = p->scroll_y;
    if (r.y < (float)y || r.y + r.h > (float)(y + p->vh))
        y = (int)r.y - p->vh / 3;
    if (r.x < (float)x || r.x + r.w > (float)(x + p->vw))
        x = (int)r.x - 20;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x != p->scroll_x || y != p->scroll_y)
        request_scroll(p, x, y);
}

bool au_page_find(AuPage *p, const char *text, bool forward, bool case_sensitive)
{
    if (p->kind != K_DOC || !p->begun || !text || !*text)
        return false;
    au_docview_update(p->dv);
    AuLayout *L = au_docview_layout(p->dv);
    AuTextRange cur = sel_range(p), m;
    AuTextPos from = {NULL, 0};
    if (cur.start.node)
        from = forward ? cur.end : cur.start;
    if (!au_layout_find(L, text, &from, forward, !case_sensitive, &m))
        return false;
    sel_set(p, m.start, m.end);
    AuRect r;
    if (au_layout_range_rect(L, &m, &r))
        reveal(p, r);
    return true;
}

/* The text position at document point (x, y). */
static bool pos_at(AuPage *p, int x, int y, AuTextPos *out)
{
    au_docview_update(p->dv);
    return au_layout_text_pos_at(au_docview_layout(p->dv), (float)(x - p->scroll_x), (float)(y - p->scroll_y),
                                 (float)p->scroll_x, (float)p->scroll_y, out);
}

/* Extends a word or paragraph selection to the unit at q. */
static void extend_by_unit(AuPage *p, AuTextPos q)
{
    AuLayout *L = au_docview_layout(p->dv);
    AuTextRange u;
    bool ok = p->sel_mode == 2 ? au_layout_word_at(L, &q, &u) : au_layout_block_at(L, &q, &u);
    if (!ok)
        u.start = u.end = q;
    if (au_text_pos_cmp(&q, &p->sel_base.start) < 0)
        sel_set(p, p->sel_base.end, u.start);
    else
        sel_set(p, p->sel_base.start, au_text_pos_cmp(&u.end, &p->sel_base.end) > 0 ? u.end : p->sel_base.end);
}

/* Selection by mouse: returns true when the event was a selection
 * gesture that should not activate a link. */
static bool sel_mouse(AuPage *p, const AuEvent *ev)
{
    AuTextPos q;
    switch (ev->type) {
    case AU_EVENT_MOUSE_DOWN: {
        bool have = pos_at(p, ev->x, ev->y, &q);
        p->selecting = true;
        p->sel_dragged = false;
        p->down_x = ev->x;
        p->down_y = ev->y;
        p->sel_mode = ev->clicks >= 3 ? 3 : ev->clicks == 2 ? 2 : 1;
        if (!have) {
            au_page_clear_selection(p);
            p->selecting = true;
            return false;
        }
        AuLayout *L = au_docview_layout(p->dv);
        if ((ev->mods & AU_MOD_SHIFT) && p->anchor.node) {
            p->sel_mode = 1;
            sel_set(p, p->anchor, q);
            return true;
        }
        AuTextRange u;
        if (p->sel_mode > 1 && (p->sel_mode == 2 ? au_layout_word_at(L, &q, &u) : au_layout_block_at(L, &q, &u))) {
            /* sel_base holds no references: its nodes belong to the
             * document, which outlives it (it is cleared first). */
            p->sel_base = u;
            sel_set(p, u.start, u.end);
            return true;
        }
        p->sel_mode = 1;
        sel_set(p, q, q);
        return false;
    }
    case AU_EVENT_MOUSE_MOVE:
        if (!p->selecting)
            return false;
        if (!p->sel_dragged) {
            int dx = ev->x - p->down_x, dy = ev->y - p->down_y;
            if (dx * dx + dy * dy < 16)
                return false;
            p->sel_dragged = true;
        }
        if (!pos_at(p, ev->x, ev->y, &q))
            return true;
        if (p->sel_mode > 1 && p->sel_base.start.node)
            extend_by_unit(p, q);
        else if (p->anchor.node)
            sel_set(p, p->anchor, q);
        return true;
    case AU_EVENT_MOUSE_UP: {
        bool gesture = p->selecting && (p->sel_dragged || p->sel_mode > 1 || (ev->mods & AU_MOD_SHIFT));
        p->selecting = false;
        return gesture;
    }
    default:
        return false;
    }
}

/* ---- Documents ---- */

/* Starts the new document in the view (the old one stays visible until
 * then). */
static void begin_doc(AuPage *p, const char *url)
{
    au_page_clear_selection(p);
    if (au_docview_hover_link(p->dv)[0] && p->cb.status_text)
        p->cb.status_text(p->cb.ctx, p, "");
    p->kind = K_DOC;
    au_docview_begin(p->dv, url);
    au_docview_set_viewport(p->dv, p->vw, p->vh);
    au_docview_set_scroll(p->dv, 0, 0);
    p->begun = true;
    p->dirty = true;
}

/* Loads a whole generated document now. */
static void load_html_now(AuPage *p, const char *html, size_t len, const char *title)
{
    begin_doc(p, p->url);
    au_buf_append(&p->source, html, len);
    au_docview_feed(p->dv, html, len);
    au_docview_finish(p->dv);
    if (title)
        set_title(p, title);
    p->main_done = true;
    p->stats.bytes += (uint32_t)len;
    check_finished(p);
}

static void page_head(AuBuf *b, const char *title)
{
    au_buf_puts(b, "<!DOCTYPE html><html><head><meta charset=utf-8><title>");
    html_escape_str(b, title);
    au_buf_puts(b, "</title><style>"
                   "body { font-family: sans-serif; margin: 24px 32px; color: #222; line-height: 1.35; }"
                   "h1 { font-size: 28px; color: #203c78; margin: 0 0 6px; }"
                   "h2 { font-size: 17px; margin: 22px 0 6px; }"
                   "p { margin: 0 0 10px; }"
                   ".note { color: #5a5a5a; font-size: 13px; }"
                   ".url { font-family: monospace; color: #5a5a5a; font-size: 13px; }"
                   "ul { margin: 0; padding-left: 22px; } li { margin: 3px 0; }"
                   "a { color: #0000cc; }"
                   "</style></head><body>");
}

static void build_home(AuBuf *b)
{
    page_head(b, "Aura");
    au_buf_puts(b, "<h1>Aura</h1>"
                   "<p class=note>A web browser for Mac OS 9. This version renders local HTML files with its own "
                   "engine (HTML, CSS, layout and painting). Networking, images and scripts are still to come.</p>"
                   "<h2>Open a page</h2>"
                   "<p>Choose File &gt; Open File to open an HTML or text file, drop one on the Aura icon, or "
                   "type a path or a <span class=url>file:</span> URL in the address field.</p>"
                   "<h2>Built-in pages</h2><ul>"
                   "<li><a href='about:scene'>Paint and font test scene</a></li>"
                   "<li><a href='about:fonts'>Installed fonts</a></li>"
                   "<li><a href='about:blank'>Blank page</a></li></ul>"
                   "<h2>Placeholders for network pages</h2><ul>"
                   "<li><a href='http://example.com/'>http://example.com/</a></li>"
                   "<li><a href='http://www.macintoshrepository.org/'>http://www.macintoshrepository.org/</a></li>"
                   "<li><a href='https://www.w3.org/TR/css-2023/'>https://www.w3.org/TR/css-2023/</a></li></ul>"
                   "<p class=note style='margin-top:18px'>Command-click a link to open it in a new tab.</p>"
                   "</body></html>");
}

static void css_string(AuBuf *b, const char *s)
{
    au_buf_putc(b, '\'');
    for (; *s; s++) {
        if (*s == '\'' || *s == '"' || *s == '\\' || *s == '<' || *s == '>' || *s == '&')
            continue;
        au_buf_putc(b, *s);
    }
    au_buf_putc(b, '\'');
}

static void build_fonts(AuPage *p, AuBuf *b)
{
    page_head(b, "Fonts");
    size_t n = au_fontset_count(p->env.fonts), bundled = 0;
    for (size_t i = 0; i < n; i++)
        bundled += (au_fontset_entry_flags(p->env.fonts, i) & AU_FONT_BUNDLED) != 0;
    AuFontSetStats st;
    au_fontset_stats(p->env.fonts, &st);
    au_buf_printf(b,
                  "<h1>Fonts</h1><p class=note>%u font faces registered: %u from the system, %u bundled with Aura "
                  "(its Fonts folder). %u loaded, %u KB of font data.</p>"
                  "<table style='border-spacing:0 2px'>",
                  (unsigned)n, (unsigned)(n - bundled), (unsigned)bundled, (unsigned)st.loaded,
                  (unsigned)(st.loaded_bytes / 1024));
    for (size_t i = 0; i < n; i++) {
        int weight;
        bool italic;
        const char *fam = au_fontset_entry(p->env.fonts, i, &weight, &italic);
        if (!fam)
            continue;
        bool own = (au_fontset_entry_flags(p->env.fonts, i) & AU_FONT_BUNDLED) != 0;
        au_buf_puts(b, "<tr><td class=note style='padding-right:16px'>");
        html_escape_str(b, fam);
        au_buf_printf(b, " %d%s%s</td><td style=\"font-size:15px;font-weight:%d;font-style:%s;font-family:", weight,
                      italic ? " italic" : "", own ? " (bundled)" : "", weight, italic ? "italic" : "normal");
        css_string(b, fam);
        au_buf_puts(b, "\">The quick brown fox 0123</td></tr>");
    }
    au_buf_puts(b, "</table></body></html>");
}

static void build_remote(AuPage *p, AuBuf *b, bool stopped)
{
    char host[256];
    au_url_host(p->url, host, sizeof host);
    page_head(b, host[0] ? host : p->url);
    au_buf_puts(b, "<h1>");
    html_escape_str(b, host[0] ? host : p->url);
    au_buf_puts(b, "</h1><p class=url>");
    html_escape_str(b, p->url);
    au_buf_puts(b, "</p>");
    if (stopped)
        au_buf_puts(b, "<p>Loading stopped.</p>");
    else
        au_buf_puts(b, "<p style='font-family:serif;font-size:16px'>Aura does not load network pages yet. This "
                       "placeholder stands in for the page while the networking code is being written. The "
                       "title, history, bookmarks, back and forward, reload and stop all work with it.</p>"
                       "<p><a href='about:home'>Back to the start page</a></p>");
    au_buf_puts(b, "</body></html>");
}

static void build_error(AuPage *p, AuBuf *b, const char *title, const char *what)
{
    page_head(b, title);
    au_buf_puts(b, "<h1>");
    html_escape_str(b, what);
    au_buf_puts(b, "</h1><p class=url>");
    html_escape_str(b, p->url);
    au_buf_puts(b, "</p></body></html>");
}

/* The file name of a URL, percent-decoded, for a plain text page's title. */
static void url_file_name(const char *url, AuBuf *out)
{
    size_t q = strcspn(url, "?#");
    const char *s = url;
    for (size_t i = 0; i < q; i++)
        if (url[i] == '/' || url[i] == ':')
            s = url + i + 1;
    au_url_percent_decode(out, s, (size_t)(url + q - s));
}

/* Plain text: the bytes inside <pre>, escaped, CR LF and CR as LF. */
static void feed_plain(AuPage *p, const char *bytes, size_t len)
{
    AuBuf b;
    au_buf_init(&b);
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (c == '\r') {
            au_buf_putc(&b, '\n');
            if (i + 1 < len && bytes[i + 1] == '\n')
                i++;
        } else if (c == 0) {
            continue;
        } else if (c == '&' || c == '<' || c == '>') {
            html_escape(&b, &c, 1);
        } else {
            au_buf_putc(&b, c);
        }
    }
    if (b.len)
        au_docview_feed(p->dv, b.data, b.len);
    au_buf_free(&b);
}

static bool looks_like_html(const char *bytes, size_t len)
{
    size_t i = 0;
    if (len >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        i = 3;
    while (i < len && au_is_space((unsigned char)bytes[i]))
        i++;
    return i < len && bytes[i] == '<';
}

/* The first bytes of the main document: start the new document. */
static void start_main_doc(AuPage *p, const char *bytes, size_t len, const char *content_type)
{
    const char *u = p->url;
    if (starts_with_ci(u, "view-source:"))
        u += 12;
    size_t q = strcspn(u, "?#");
    if (p->source_mode) {
        p->plain = true;
    } else if (content_type) {
        p->plain = starts_with_ci(content_type, "text/plain");
    } else if (ends_with(u, q, ".html") || ends_with(u, q, ".htm") || ends_with(u, q, ".xhtml") ||
               ends_with(u, q, ".shtml")) {
        p->plain = false;
    } else if (ends_with(u, q, ".txt") || ends_with(u, q, ".text")) {
        p->plain = true;
    } else {
        p->plain = !looks_like_html(bytes, len);
    }
    begin_doc(p, p->url);
    if (p->plain) {
        AuBuf name, b;
        au_buf_init(&name);
        au_buf_init(&b);
        if (p->source_mode)
            au_buf_puts(&name, "Source of ");
        url_file_name(u, &name);
        au_buf_puts(&b, "<!DOCTYPE html><html><head><title>");
        html_escape(&b, name.data ? name.data : "", name.len);
        au_buf_puts(&b, "</title></head><body style='margin:8px'><pre style='white-space:pre-wrap;margin:0'>");
        au_docview_feed(p->dv, b.data, b.len);
        au_buf_free(&name);
        au_buf_free(&b);
    }
}

static void main_data(AuPage *p, const char *bytes, size_t len)
{
    if (!p->begun)
        start_main_doc(p, bytes, len, NULL);
    au_buf_append(&p->source, bytes, len);
    if (p->plain)
        feed_plain(p, bytes, len);
    else
        au_docview_feed(p->dv, bytes, len);
    p->dirty = true;
    if (!p->in_idle)
        maybe_layout(p, false);
}

static void show_error(AuPage *p, const char *title, const char *what)
{
    AuBuf b;
    au_buf_init(&b);
    build_error(p, &b, title, what);
    p->failed = true;
    load_html_now(p, b.data, b.len, title);
    au_buf_free(&b);
}

static void main_done(AuPage *p, bool ok, const AuFetchInfo *info)
{
    if (info && info->url && strcmp(info->url, p->url) != 0) {
        set_str(&p->url, info->url);
        if (p->cb.url_changed)
            p->cb.url_changed(p->cb.ctx, p, p->url);
    }
    if (!ok && (!p->main_fetch || p->main_fetch->off == 0)) {
        p->main_done = true;
        show_error(p, au_url_scheme_is(p->url, "file") ? "File not found" : "Cannot load page",
                   au_url_scheme_is(p->url, "file") ? "Cannot open this file" : "Cannot load this page");
        return;
    }
    if (!p->begun)
        start_main_doc(p, "", 0, info ? info->content_type : NULL);
    if (p->plain)
        au_docview_feed(p->dv, "</pre></body></html>", 20);
    au_docview_finish(p->dv);
    if (!ok)
        p->failed = true;
    p->main_done = true;
    p->dirty = true;
    report_progress(p);
}

/* ---- Loading ---- */

static void cancel_all(AuPage *p)
{
    for (size_t i = 0; i < p->nfetches; i++)
        fetch_cancel(p->fetches[i]);
    p->main_fetch = NULL;
    p->main = MAIN_NONE;
}

static bool is_same_document(AuPage *p, const char *url, bool entry)
{
    if (p->kind != K_DOC || !p->begun || p->failed)
        return false;
    if (au_url_scheme_is(url, "about"))
        return false;
    size_t a = base_len(url), b = base_len(p->url);
    if (a != b || memcmp(url, p->url, a) != 0)
        return false;
    return entry || url[a] == '#';
}

static void start_load(AuPage *p, const char *url, bool entry, int rx, int ry, bool force)
{
    if (!force && is_same_document(p, url, entry)) {
        set_str(&p->url, url);
        au_free(p->fragment);
        p->fragment = NULL;
        p->restore = false;
        /* The new entry (and its history item) gets the document's title. */
        if (p->title[0] && p->cb.title_changed)
            p->cb.title_changed(p->cb.ctx, p, p->title);
        if (entry) {
            request_scroll(p, rx, ry);
        } else {
            int y = au_docview_fragment_y(p->dv, url + base_len(url) + 1);
            if (y >= 0)
                request_scroll(p, 0, y);
        }
        return;
    }
    cancel_all(p);
    compact_fetches(p);
    au_page_clear_selection(p);
    au_buf_free(&p->source);
    p->gen++;
    char *u = au_strndup(url, strlen(url)); /* url may be p->url */
    au_free(p->url);
    p->url = u;
    p->failed = false;
    p->begun = false;
    p->main_done = false;
    p->main_total = 0;
    p->source_mode = false;
    p->plain = false;
    p->requested = p->completed = 0;
    p->loading = true;
    p->progress = 0;
    p->load_start = now(p);
    memset(&p->stats, 0, sizeof p->stats);
    au_free(p->fragment);
    p->fragment = NULL;
    const char *hash = strchr(u, '#');
    if (hash && hash[1])
        p->fragment = au_strndup(hash + 1, strlen(hash + 1));
    p->restore = entry;
    p->restore_x = rx;
    p->restore_y = ry;
    set_title(p, "");
    apply_font_prefs(p);
    if (p->cb.load_started)
        p->cb.load_started(p->cb.ctx, p);
    if (p->cb.url_changed)
        p->cb.url_changed(p->cb.ctx, p, p->url);

    if (au_url_scheme_is(u, "about")) {
        const char *what = u + 6;
        size_t wn = strcspn(what, "?#");
        AuBuf b;
        au_buf_init(&b);
        if (wn == 5 && !memcmp(what, "scene", 5)) {
            if (au_docview_hover_link(p->dv)[0] && p->cb.status_text)
                p->cb.status_text(p->cb.ctx, p, "");
            au_docview_load(p->dv, "", 0, "about:blank");
            p->kind = K_SCENE;
            p->begun = true;
            p->dirty = false;
            p->main_done = true;
            p->content_w = 600;
            p->content_h = AU_SCENE_HEIGHT;
            if (p->cb.content_size_changed)
                p->cb.content_size_changed(p->cb.ctx, p);
            invalidate_all(p);
            set_title(p, "Test Scene");
            apply_pending_scroll(p, true);
            finish(p, true);
        } else if (wn == 4 && !memcmp(what, "home", 4)) {
            build_home(&b);
            load_html_now(p, b.data, b.len, NULL);
        } else if (wn == 5 && !memcmp(what, "fonts", 5)) {
            build_fonts(p, &b);
            load_html_now(p, b.data, b.len, NULL);
        } else if (wn == 5 && !memcmp(what, "blank", 5)) {
            au_buf_puts(&b, "<!DOCTYPE html><html><head></head><body></body></html>");
            load_html_now(p, b.data, b.len, NULL);
        } else {
            show_error(p, "Unknown page", "Unknown about: page");
        }
        au_buf_free(&b);
        return;
    }
    const char *target = u;
    if (starts_with_ci(u, "view-source:")) {
        p->source_mode = true;
        target = u + 12;
    }
    if (au_url_scheme_is(target, "file") || fetcher_for(p, target)) {
        p->main_fetch = start_fetch(p, target, true);
        if (p->main_fetch) {
            p->main = p->main_fetch->is_file ? MAIN_FILE : MAIN_FETCH;
            return;
        }
        show_error(p, "Cannot load page", "Cannot load this page");
        return;
    }
    if (!p->source_mode && (au_url_scheme_is(u, "http") || au_url_scheme_is(u, "https"))) {
        p->main = MAIN_SIM; /* au_page_idle finishes it */
        return;
    }
    show_error(p, "Cannot open", "Aura cannot open this kind of address");
}

void au_page_navigate(AuPage *p, const char *url)
{
    start_load(p, url, false, 0, 0, false);
}

void au_page_navigate_entry(AuPage *p, const char *url, int scroll_x, int scroll_y)
{
    start_load(p, url, true, scroll_x, scroll_y, false);
}

void au_page_reload(AuPage *p)
{
    start_load(p, p->url, true, p->scroll_x, p->scroll_y, true);
}

void au_page_stop(AuPage *p)
{
    if (!p->loading)
        return;
    if (p->main == MAIN_SIM) {
        AuBuf b;
        au_buf_init(&b);
        build_remote(p, &b, true);
        p->failed = true;
        begin_doc(p, p->url);
        au_docview_feed(p->dv, b.data, b.len);
        au_docview_finish(p->dv);
        au_buf_free(&b);
        p->main_done = true;
    } else {
        /* Keep what has arrived: end the parse, drop outstanding resources. */
        for (size_t i = 0; i < p->nfetches; i++) {
            Fetch *f = p->fetches[i];
            if (f->done || f->cancelled)
                continue;
            fetch_cancel(f);
            if (!f->main)
                au_docview_resource_done(p->dv, f->id, false);
        }
        if (p->begun && !p->main_done) {
            if (p->plain)
                au_docview_feed(p->dv, "</pre>", 6);
            au_docview_finish(p->dv);
        }
        p->main_done = true;
        p->main_fetch = NULL;
    }
    p->failed = true;
    compact_fetches(p);
    if (p->begun) {
        p->dirty = true;
        maybe_layout(p, true);
    }
    finish(p, false);
}

/* ---- Interface ---- */

AuPage *au_page_new(const AuPageEnv *env, const AuPageCallbacks *cb)
{
    AuPage *p = au_calloc(sizeof *p);
    p->env = *env;
    p->cb = *cb;
    p->url = au_strndup("about:blank", 11);
    p->title = au_strndup("", 0);
    p->kind = K_DOC;
    p->vw = 640;
    p->vh = 480;
    p->content_w = p->vw;
    p->content_h = p->vh;
    p->zoom = 1;
    AuLoader loader = {p, loader_request, loader_cancel, sniff_image};
    AuDocViewCallbacks dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.ctx = p;
    dcb.title_changed = dv_title;
    dcb.invalidate = dv_invalidate;
    dcb.size_changed = dv_size;
    dcb.status_text = dv_status;
    dcb.open_url = dv_open;
    p->dv = au_docview_new(env->fonts, &loader, &dcb);
    au_docview_set_viewport(p->dv, p->vw, p->vh);
    apply_font_prefs(p);
    return p;
}

void au_page_free(AuPage *p)
{
    if (!p)
        return;
    cancel_all(p);
    compact_fetches(p);
    au_page_clear_selection(p);
    au_buf_free(&p->source);
    au_docview_free(p->dv); /* cancels nothing more: the fetches are gone */
    au_free(p->fetches);
    au_free(p->url);
    au_free(p->title);
    au_free(p->fragment);
    au_free(p->open_url);
    au_free(p);
}

bool au_page_source(const AuPage *p, const char **data, size_t *len)
{
    if (p->kind != K_DOC || !p->main_done || p->failed || !p->source.len)
        return false;
    *data = p->source.data;
    *len = p->source.len;
    return true;
}

const char *au_page_url(const AuPage *p) { return p->url; }
const char *au_page_title(const AuPage *p) { return p->title; }
bool au_page_loading(const AuPage *p) { return p->loading; }
float au_page_progress(const AuPage *p) { return p->progress; }

AuSecurity au_page_security(const AuPage *p)
{
    const char *u = p->url;
    if (starts_with_ci(u, "view-source:"))
        u += 12;
    /* Nothing is connected to TLS yet, so HTTPS is not claimed secure. */
    if (au_url_scheme_is(u, "http"))
        return AU_SECURITY_NOT_SECURE;
    return AU_SECURITY_NONE;
}

void au_page_set_viewport(AuPage *p, int width, int height)
{
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (width == p->vw && height == p->vh)
        return;
    p->vw = width;
    p->vh = height;
    au_docview_set_viewport(p->dv, width, height);
    if (p->kind == K_DOC && p->begun) {
        /* Lay out now, so the scroll range is right before the next paint. */
        run_layout(p);
        invalidate_all(p);
    }
}

void au_page_content_size(const AuPage *p, int *width, int *height)
{
    *width = p->content_w;
    *height = p->content_h;
}

void au_page_set_scroll(AuPage *p, int x, int y)
{
    p->scroll_x = x;
    p->scroll_y = y;
    au_docview_set_scroll(p->dv, x, y);
}

void au_page_prefs_changed(AuPage *p)
{
    apply_font_prefs(p);
    if (p->kind == K_DOC && p->begun) {
        run_layout(p);
        invalidate_all(p);
    }
}

void au_page_set_zoom(AuPage *p, float zoom)
{
    if (!(zoom >= AU_PAGE_ZOOM_MIN))
        zoom = AU_PAGE_ZOOM_MIN;
    if (zoom > AU_PAGE_ZOOM_MAX)
        zoom = AU_PAGE_ZOOM_MAX;
    if (zoom == p->zoom)
        return;
    p->zoom = zoom;
    au_page_prefs_changed(p);
}

float au_page_zoom(const AuPage *p) { return p->zoom; }

void au_page_paint(AuPage *p, AuCanvas *c, AuRect view, int scroll_x, int scroll_y)
{
    uint32_t t0 = now(p);
    au_canvas_clip_push(c, view);
    if (p->kind == K_SCENE) {
        au_fill_rect(c, view, AU_RGB(255, 255, 255));
        au_canvas_translate(c, view.x - (float)scroll_x, view.y);
        au_scene_draw(c, p->env.fonts, (int)view.w + scroll_x, (int)view.h, scroll_y);
        au_canvas_translate(c, (float)scroll_x - view.x, -view.y);
    } else if (!p->begun && !au_docview_document(p->dv)) {
        au_fill_rect(c, view, AU_RGB(255, 255, 255));
    } else {
        bool was_idle = p->in_idle;
        p->in_idle = false;
        au_docview_paint(p->dv, c, view, scroll_x, scroll_y);
        p->in_idle = was_idle;
        if (p->dirty && !p->loading)
            p->dirty = false; /* paint brought the layout up to date */
    }
    au_canvas_clip_pop(c);
    p->scroll_x = scroll_x;
    p->scroll_y = scroll_y;
    p->stats.paint_ms = now(p) - t0;
    if (!p->stats.first_paint_ms && p->begun) {
        p->stats.first_paint_ms = now(p) - p->load_start;
        if (!p->stats.first_paint_ms)
            p->stats.first_paint_ms = 1;
    }
}

bool au_page_mouse(AuPage *p, const AuEvent *ev)
{
    if (p->kind != K_DOC || !p->begun)
        return false;
    bool gesture = sel_mouse(p, ev);
    AuEvent e = *ev;
    if (gesture && ev->type == AU_EVENT_MOUSE_UP)
        e.x = e.y = -100000; /* end the press without activating a link */
    bool used = au_docview_mouse(p->dv, &e) || gesture;
    if (p->open_url) {
        char *u = p->open_url;
        p->open_url = NULL;
        if (p->cb.open_url)
            p->cb.open_url(p->cb.ctx, p, u, p->open_new_tab);
        au_free(u);
        used = true;
    }
    return used;
}

static void put_resolved(AuPage *p, const AuElement *el, const char *name, AuBuf *out)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    const char *v = a ? au_element_get_attr(el, a) : NULL;
    char abs[2048];
    if (v && *v && au_url_resolve(au_docview_base_url(p->dv), v, abs, sizeof abs))
        au_buf_puts(out, abs);
}

void au_page_hit_info(AuPage *p, int x, int y, AuBuf *link, AuBuf *image)
{
    if (p->kind != K_DOC || !p->begun)
        return;
    AuHitResult r;
    if (!au_docview_hit_test(p->dv, x, y, &r))
        return;
    if (link && r.link)
        put_resolved(p, r.link, "href", link);
    if (image && r.element && r.element->tag == AU_ATOM_img)
        put_resolved(p, r.element, "src", image);
}

bool au_page_key(AuPage *p, const AuEvent *ev)
{
    (void)p;
    (void)ev;
    return false; /* scrolling keys are the browser's */
}

bool au_page_scroll(AuPage *p, int x, int y, int dx, int dy)
{
    if (p->kind != K_DOC || !p->begun)
        return false;
    return au_docview_scroll_at(p->dv, x, y, dx, dy);
}

/* Reads and delivers files, a slice at a time. Returns false when there
 * is nothing left to do now. */
static bool file_step(AuPage *p)
{
    Fetch *f = NULL;
    /* The main document first, then resources in request order. */
    if (p->main_fetch && p->main_fetch->is_file && !p->main_fetch->done && !p->main_fetch->cancelled)
        f = p->main_fetch;
    for (size_t i = 0; !f && i < p->nfetches; i++) {
        Fetch *x = p->fetches[i];
        if (x->is_file && !x->done && !x->cancelled)
            f = x;
    }
    if (!f)
        return false;
    if (!f->read) {
        f->read = true;
        bool ok = p->env.read_file_url && p->env.read_file_url(p->env.ctx, f->url, &f->data, &f->len);
        if (!ok) {
            sink_done(f, 0, false, NULL);
            return true;
        }
        if (f->main)
            p->main_total = (uint32_t)f->len;
    }
    size_t n = f->len - f->off;
    if (f->main && n > CHUNK)
        n = CHUNK;
    if (n)
        sink_data(f, 0, f->data + f->off, n);
    if (f->off >= f->len && !f->done && !f->cancelled) {
        au_free(f->data);
        f->data = NULL;
        sink_done(f, 0, true, NULL);
    } else if (f->main) {
        report_progress(p);
    }
    return true;
}

void au_page_idle(AuPage *p)
{
    if (p->main == MAIN_SIM && p->loading) {
        uint32_t t = now(p) - p->load_start;
        float prog = (float)t / SIM_LOAD_MS;
        if (prog >= 1) {
            AuBuf b;
            au_buf_init(&b);
            build_remote(p, &b, false);
            p->main = MAIN_NONE;
            char host[256];
            au_url_host(p->url, host, sizeof host);
            load_html_now(p, b.data, b.len, host[0] ? host : p->url);
            au_buf_free(&b);
            return;
        }
        prog = (float)(int)(prog * 20) / 20;
        if (prog != p->progress) {
            p->progress = prog;
            if (p->cb.load_progress)
                p->cb.load_progress(p->cb.ctx, p);
        }
        return;
    }
    if (!p->nfetches && !p->dirty)
        return;
    uint32_t gen = p->gen;
    /* Poll each fetcher with transfers outstanding once. */
    const AuPageFetcher *polled[8];
    size_t npolled = 0;
    p->in_idle = true;
    for (size_t i = 0; i < p->nfetches && gen == p->gen; i++) {
        const AuPageFetcher *fe = p->fetches[i]->fetcher;
        if (!fe || !fe->poll || p->fetches[i]->done || p->fetches[i]->cancelled)
            continue;
        bool seen = false;
        for (size_t k = 0; k < npolled; k++)
            seen |= polled[k] == fe;
        if (seen)
            continue;
        if (npolled < 8)
            polled[npolled++] = fe;
        fe->poll(fe->ctx);
    }
    uint32_t t0 = now(p);
    while (gen == p->gen && file_step(p)) {
        if (now(p) - t0 >= BUDGET_MS)
            break;
    }
    p->in_idle = false;
    if (gen != p->gen)
        return; /* a callback started another load */
    compact_fetches(p);
    maybe_layout(p, false);
    check_finished(p);
}

bool au_page_busy(const AuPage *p)
{
    return p->loading || p->nfetches > 0 || (p->dirty && p->begun);
}

void au_page_stats(const AuPage *p, AuPageStats *s)
{
    *s = p->stats;
    AuLayoutStats ls;
    au_layout_stats(au_docview_layout(p->dv), &ls);
    s->boxes = ls.boxes;
}
