/* The page interface (page.h) on top of the document view (src/page).
 *
 *   about:blank, about:home, about:fonts, about:licences
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

#include "bookmarks.h"
#include "credits.h"
#include "history.h"
#include "netlog.h"
#include "scriptmem.h"
#include "../codec/codec.h"
#include "../image/cache.h"
#include "../layout/selection.h"
#include "../js/storage_call.h"
#include "../net/auth.h" /* au_base64_encode */
#include "../net/dataurl.h"
#include "../net/policy.h"
#include "../net/web_storage.h"
#include "../page/page.h"

#include <stdio.h>
#include <string.h>

#define SIM_LOAD_MS 1400
#define BUDGET_MS 40
#define CHUNK 8192
#define LAYOUT_INTERVAL_MS 300
#define SCRIPT_BUDGET_MS 20 /* script time per idle call, on top of BUDGET_MS */

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
    AuFetchKind kind;
    bool done, ok, cancelled;
    bool script;   /* a script's own request (fetch, XMLHttpRequest, a worker) */
    char *doc_url; /* the nested document that asked (a frame's resources), NULL for the page's own */
    char *mime;    /* a data: URL's type, reported as the response head before its bytes */
    uint32_t log; /* its record in the page's request log (netlog.h) */
};

/* The main document's bytes as they arrived, for Page Source, View Source
 * and Save Page As ("The page source"). They are read rarely, so once
 * the load is over they are held compressed: plain is then empty until
 * something asks for the bytes, and empty again a while after. */
typedef struct Source {
    AuBuf plain;   /* the bytes; empty while only z holds them */
    AuBuf z;       /* raw deflate of the bytes, or empty (loading, small, or not compressible) */
    size_t len;    /* the length of the bytes when z holds them */
    uint32_t used; /* when plain was last asked for (the page's clock) */
    bool tried;    /* compression was tried and did not pay */
    uint32_t pack_ms; /* how long compressing took */
} Source;

/* A document that is not shown: the view and what the page knows about
 * its document (see "Kept documents"). */
typedef struct Kept {
    uint32_t entry; /* the browser's back/forward entry */
    AuDocView *dv;  /* NULL while the document is still the one shown (AuPage.leaving) */
    char *url, *title, *content_type;
    AuFetchCert *cert;
    AuSecurity security;
    bool have_security, mixed, legacy_kex;
    int status;
    AuFetchReferrerPolicy rpolicy;
    bool image_doc, plain, source_mode;
    Source src;
    AuNetLog netlog; /* the document's request log goes with it */
    int content_w, content_h;
    AuPageStats stats;
    uint32_t stamp; /* when it was hidden */
    size_t bytes;   /* estimated memory */
} Kept;

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

    Source src; /* the main document's bytes as they arrived (Save Page As) */

    /* Network documents. */
    AuFetchCacheMode cache_mode;    /* of the current load */
    bool allow_http;                /* HTTPS-first: the user accepted http for the next load */
    char *link_referrer, *link_target; /* a link this page opened: its referrer, for that navigation */
    char *referrer;                 /* the current load's referrer (NULL: typed or bookmarked) */
    AuFetchReferrerPolicy rpolicy;  /* the document's Referrer-Policy */
    AuSecurity security;            /* from the main response */
    bool have_security, mixed, legacy_kex;
    AuFetchCert *cert;              /* the main document's certificate, or NULL */
    int status;                     /* the main response's status */
    char *content_type;             /* the main response's type, "" before one */
    bool internal;                  /* the document is an internal page Aura generated (error, interim) */
    bool image_doc;                 /* the document is an image: shown with a generated <img> */
    bool discard;                   /* the document is of a type Aura cannot show: its bytes are dropped */
    char last_status[160];          /* the status text last reported (load_progress) */
    uint32_t storage_ns;            /* the tab's sessionStorage namespace (au_page_set_storage_ns) */
    AuBuf *preset_source;           /* au_page_navigate_source: the bytes to show */
    bool interim;                   /* an internal page shown while the real one is awaited */
    uint32_t interim_ms;
    unsigned interim_bits;
    /* What was shown before this load, for a load that turned out to be a
     * download. */
    char *prev_url, *prev_title;
    bool prev_begun;
    AuSecurity prev_security;
    bool prev_have_security, prev_mixed, prev_legacy_kex;
    AuFetchCert *prev_cert;

    /* Text selection: anchor, focus and sel_base hold a reference to their nodes. */
    AuTextPos anchor, focus;
    AuDocView *sel_dv;    /* the nested view the selection is in; NULL: the page's own (sel_view) */
    AuDocument *sel_doc;  /* the document sel_dv showed then (referenced) */
    AuTextRange sel_base; /* the word or paragraph a double or triple click chose */
    int sel_mode;         /* 1 characters, 2 words, 3 paragraphs */
    bool selecting, sel_dragged;
    int down_x, down_y;

    AuPageStats stats;

    /* Forms: a POST submission waiting for au_page_mouse or au_page_key to
     * return (sub_*), and the body of the current load when it is a POST
     * (post_*; kept for a reload). */
    char *sub_url, *sub_type;
    AuBuf sub_body;
    bool sub_new_tab;
    bool post;
    AuBuf post_body;
    char *post_type;
    uint32_t blink_due;
    bool ctl_press; /* the button went down on a form control: no text selection */

    /* The document cache. */
    uint32_t next_entry; /* the entry the next load is for (au_page_set_entry) */
    uint32_t doc_entry;  /* the entry of the current load (the shown document once begun) */
    uint32_t prev_entry; /* the entry shown before this load */
    Kept *leaving;       /* the shown document, to be kept when the load under way replaces it */
    Kept **kept;         /* hidden documents, oldest first */
    size_t nkept;
    bool no_store;       /* the main response said Cache-Control: no-store */
    bool no_keep;        /* the document shown was a POST result (set until the next load starts) */
    bool discarded;      /* the document was freed to save memory (au_page_discard); cleared by the next load */

    /* Script console: the last AU_PAGE_CONSOLE_LINES lines, oldest first
     * from console_start. */
    char *console[AU_PAGE_CONSOLE_LINES];
    uint8_t console_level[AU_PAGE_CONSOLE_LINES];
    size_t console_start, console_count;
    uint32_t console_total; /* lines ever added, so a view sees new ones */
    uint32_t console_current; /* lines added since the last navigation began */
    /* Script ran out of memory since the last navigation began: the
     * notice waiting for au_page_take_script_oom, and whether this page
     * has had one (one notice per page shown). */
    AuScriptOom oom;
    bool oom_due, oom_told;

    AuNetLog netlog; /* the requests since the last navigation (netlog.h) */
};

/* Milliseconds since the navigation, for the request log. */
static uint32_t log_ms(AuPage *p)
{
    return p->env.time_ms ? p->env.time_ms(p->env.ctx) - p->load_start : 0;
}

static void log_begin(AuPage *p, Fetch *f, const char *method)
{
    f->log = au_netlog_begin(&p->netlog, f->url, method, f->kind, log_ms(p));
    if (f->is_file)
        au_netlog_source(&p->netlog, f->log, AU_NETREC_FROM_FILE);
    else if (au_url_scheme_is(f->url, "data"))
        au_netlog_source(&p->netlog, f->log, AU_NETREC_FROM_DATA);
}

/* ---- The page source ----
 * An article's HTML is about 1 MB and deflates to a sixth of that.
 * source_pack compresses it (level 1: 0.2 s for a megabyte in the guest
 * would be too long at level 2 for bytes that may never be read) and
 * source_open gives the bytes back, inflating them when needed.
 * au_page_idle packs once the load is over and drops the plain copy
 * SOURCE_OPEN_MS after it was last asked for. */
#define SOURCE_PACK_MIN 8192
#define SOURCE_OPEN_MS 10000

static void source_free(Source *s)
{
    au_buf_free(&s->plain);
    au_buf_free(&s->z);
    memset(s, 0, sizeof *s);
}

/* Moves src's bytes to dst (freeing what dst held). */
static void source_move(Source *dst, Source *src)
{
    source_free(dst);
    *dst = *src;
    memset(src, 0, sizeof *src);
}

/* Bytes of memory held. */
static size_t source_held(const Source *s)
{
    return s->plain.len + s->z.len;
}

static size_t source_len(const Source *s)
{
    return s->z.len ? s->len : s->plain.len;
}

/* The bytes as one buffer (inflated when only the compressed form is
 * held); false when there are none. */
static bool source_open(Source *s, uint32_t now_ms)
{
    s->used = now_ms;
    if (s->plain.len)
        return true;
    if (!s->z.len)
        return false;
    uint8_t *out = NULL;
    size_t n = 0;
    if (!au_inflate_all(AU_INFLATE_RAW, s->z.data, s->z.len, s->len, &out, &n) || n != s->len) {
        au_free(out);
        return false; /* cannot happen: the stream is our own */
    }
    au_buf_free(&s->plain);
    s->plain.data = (char *)out;
    s->plain.len = s->plain.cap = n;
    return true;
}

static void source_append(Source *s, const void *bytes, size_t len)
{
    if (s->z.len) {
        /* More bytes after packing (not expected): back to the plain form. */
        source_open(s, 0);
        au_buf_free(&s->z);
        s->len = 0;
    }
    s->tried = false;
    au_buf_append(&s->plain, bytes, len);
}

/* Compresses the bytes and frees the plain copy, when that saves at
 * least an eighth. */
static void source_pack(Source *s, AuPage *p);

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

static void source_pack(Source *s, AuPage *p)
{
    if (s->z.len || s->tried || s->plain.len < SOURCE_PACK_MIN)
        return;
    uint32_t t0 = now(p);
    AuBuf z;
    au_buf_init(&z);
    au_deflate_all(AU_DEFLATE_RAW, 1, s->plain.data, s->plain.len, &z);
    s->pack_ms = now(p) - t0;
    if (z.len > s->plain.len - s->plain.len / 8) {
        au_buf_free(&z);
        s->tried = true;
        return;
    }
    if (z.cap > z.len + 1) {
        z.data = au_realloc(z.data, z.len + 1);
        z.cap = z.len + 1;
    }
    s->z = z;
    s->len = s->plain.len;
    au_buf_free(&s->plain);
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
    /* Decoded images get the memory cache size as their budget, scaled
     * up on 64-bit builds (AU_IMAGE_BUDGET_SCALE). */
    au_docview_set_image_budget(p->dv, pr ? (size_t)pr->memory_cache_kb * 1024 * AU_IMAGE_BUDGET_SCALE : 0);
    /* The colour scheme (prefers-color-scheme), from the browser. */
    au_docview_set_dark(p->dv, p->env.dark && p->env.dark(p->env.ctx));
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
    au_free(f->doc_url);
    au_free(f->mime);
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
    au_netlog_end(&f->page->netlog, f->log, AU_NETREC_CANCELLED, NULL, log_ms(f->page));
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
static void hide_leaving(AuPage *p);
static void kept_drop(AuPage *p, uint32_t entry);
static void kept_trim(AuPage *p);
static void new_view(AuPage *p);
static void main_response(AuPage *p, const AuFetchResponse *r);
static bool main_download(AuPage *p, const AuFetchResponse *r, uint32_t handoff);

static void sink_response(void *ctx, uint32_t id, const AuFetchResponse *r)
{
    Fetch *f = ctx;
    (void)id;
    if (f->done || f->cancelled)
        return;
    au_netlog_response(&f->page->netlog, f->log, r->status, r->content_type, r->from_cache, log_ms(f->page));
    au_netlog_set_url(&f->page->netlog, f->log, r->url);
    if (f->main) {
        main_response(f->page, r);
        return;
    }
    if (f->page->have_security && f->page->security == AU_SECURITY_SECURE && r->security != AU_SECURITY_SECURE &&
        r->security != AU_SECURITY_NONE)
        f->page->mixed = true; /* an insecure or doubtful resource in a secure page */
    /* Script sees the status and these headers (the fetcher passes no
     * others yet); a frame's document is checked against the framing
     * headers (src/page/frames.c). */
    if (r->headers) {
        /* The fetcher passes every header. The computed type (after
         * sniffing) replaces the server's for everything but a script's
         * own request, which reads the header as sent. */
        const char **hv = au_alloc((r->nheaders + 1) * 2 * sizeof *hv);
        size_t n = 0;
        bool typed = false;
        for (size_t i = 0; i < r->nheaders; i++) {
            bool ct = au_ieq(r->headers[2 * i], strlen(r->headers[2 * i]), "content-type", 12);
            if (ct && typed)
                continue;
            hv[2 * n] = r->headers[2 * i];
            hv[2 * n + 1] = r->headers[2 * i + 1];
            if (ct) {
                typed = true;
                if (!f->script && r->content_type && r->content_type[0])
                    hv[2 * n + 1] = r->content_type;
            }
            n++;
        }
        if (!typed && !f->script && r->content_type && r->content_type[0]) {
            hv[2 * n] = "content-type";
            hv[2 * n + 1] = r->content_type;
            n++;
        }
        au_docview_resource_head(f->page->dv, f->id, r->status ? r->status : 200,
                                 r->status_text ? r->status_text : "", r->url, hv, n);
        au_free(hv);
        return;
    }
    AuBuf len;
    au_buf_init(&len);
    if (r->content_length >= 0)
        au_put_i64(&len, r->content_length);
    const char *hdrs[8];
    size_t n = 0;
    if (f->kind == AU_FETCH_FRAME && r->x_frame_options && r->x_frame_options[0]) {
        hdrs[2 * n] = "x-frame-options";
        hdrs[2 * n + 1] = r->x_frame_options;
        n++;
    }
    if (f->kind == AU_FETCH_FRAME && r->csp && r->csp[0]) {
        hdrs[2 * n] = "content-security-policy";
        hdrs[2 * n + 1] = r->csp;
        n++;
    }
    if (r->content_type && r->content_type[0]) {
        hdrs[2 * n] = "content-type";
        hdrs[2 * n + 1] = r->content_type;
        n++;
    }
    if (len.len) {
        hdrs[2 * n] = "content-length";
        hdrs[2 * n + 1] = au_buf_cstr(&len);
        n++;
    }
    au_docview_resource_head(f->page->dv, f->id, r->status ? r->status : 200, "", r->url, hdrs, n);
    au_buf_free(&len);
}

static bool sink_download(void *ctx, uint32_t id, const AuFetchResponse *r, uint32_t handoff)
{
    Fetch *f = ctx;
    (void)id;
    if (f->done || f->cancelled || !f->main)
        return false;
    return main_download(f->page, r, handoff);
}

static void sink_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    Fetch *f = ctx;
    (void)id;
    if (f->done || f->cancelled)
        return;
    f->off += len;
    au_netlog_data(&f->page->netlog, f->log, len);
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
    au_netlog_end(&p->netlog, f->log, ok ? AU_NETREC_DONE : AU_NETREC_FAILED, info, log_ms(p));
    if (f->main) {
        main_done(p, ok, info);
    } else {
        if (info && info->mixed && p->have_security)
            p->mixed = true;
        if (info && info->blocked_reason) {
            p->stats.blocked++;
            if (p->cb.load_progress) /* the status bar shows the count */
                p->cb.load_progress(p->cb.ctx, p);
        }
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
 * source can supply it. Nothing is delivered before the next idle. The
 * main document's request carries the navigation's context; a resource's
 * names the document as its referrer and top-level site. */
static Fetch *start_fetch(AuPage *p, const char *url, bool main, AuFetchKind kind, const AuResourceRequest *rr)
{
    /* The document that asks: the page, or a nested document (rr). */
    const char *doc_url = rr && rr->document_url && rr->document_url[0] ? rr->document_url : p->url;
    if (au_url_scheme_is(url, "file")) {
        /* a network page reads no local files, in frames either */
        if (!main && !au_url_scheme_is(doc_url, "file") && !au_url_scheme_is(p->url, "file") &&
            kind == AU_FETCH_FRAME)
            return NULL;
        Fetch *f = fetch_new(p, url, main);
        f->is_file = true;
        f->kind = kind;
        log_begin(p, f, NULL);
        return f;
    }
    if (!main && au_url_scheme_is(url, "data")) {
        /* A data: URL (a script, a frame's document): decoded here and
         * delivered from idle like a file that has been read. */
        AuDataUrl d;
        au_data_url_init(&d);
        if (!au_data_url_decode(url, &d)) {
            au_data_url_free(&d);
            return NULL;
        }
        Fetch *f = fetch_new(p, "data:", main);
        f->is_file = f->read = true;
        f->kind = kind;
        f->mime = au_strndup(au_buf_cstr(&d.mime), d.mime.len);
        f->data = au_buf_take(&d.body, &f->len);
        au_data_url_free(&d);
        log_begin(p, f, NULL);
        return f;
    }
    const AuPageFetcher *fe = fetcher_for(p, url);
    if (!fe || !fe->start)
        return NULL;
    Fetch *f = fetch_new(p, url, main);
    f->fetcher = fe;
    f->kind = kind;
    if (!main && doc_url != p->url)
        f->doc_url = au_strndup(doc_url, strlen(doc_url));
    f->sink.ctx = f;
    f->sink.response = sink_response;
    f->sink.data = sink_data;
    f->sink.done = sink_done;
    f->sink.download = main ? sink_download : NULL;
    AuFetchRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = f->url;
    rq.kind = kind;
    if (main && p->post) {
        rq.method = "POST";
        rq.body = p->post_body.data ? p->post_body.data : "";
        rq.body_len = p->post_body.len;
        rq.body_type = p->post_type;
    }
    if (main) {
        rq.document_url = p->referrer;
        rq.user_activated = true;
        rq.cache = p->cache_mode;
        rq.allow_http_fallback = p->allow_http;
        p->allow_http = false;
    } else {
        /* HTTPS-first upgrades navigations only. A page that was loaded
         * over http (the user accepted that) loads its http subresources
         * as written: upgrading them and dropping the ones whose https
         * fails left such pages without their style sheets and images.
         * Hosts known to HSTS are still upgraded. */
        rq.allow_http_fallback = true;
        /* The requesting document is the referrer and decides mixed
         * content (an http frame in an https page is blocked); the tab's
         * document is the top-level site for cookies and blocklists. */
        rq.document_url = f->doc_url ? f->doc_url : p->url;
        rq.top_level_url = p->url;
        if (rr && rr->method && strcmp(rr->method, "GET") != 0) {
            rq.method = rr->method;
            if (rr->body || !strcmp(rr->method, "POST")) {
                rq.body = rr->body ? rr->body : "";
                rq.body_len = rr->body_len;
                rq.body_type = rr->body_type;
            }
        }
        rq.referrer_policy = p->rpolicy;
        rq.cache = p->cache_mode == AU_FETCH_CACHE_BYPASS || p->cache_mode == AU_FETCH_CACHE_HISTORY
                       ? p->cache_mode
                       : AU_FETCH_CACHE_NORMAL;
        if (rr) {
            /* What a script's request (or a CORS resource) asks for. */
            rq.headers = rr->headers;
            rq.nheaders = rr->nheaders;
            rq.script_mode = rr->script_mode;
            rq.manual_redirect = rr->manual_redirect;
            rq.omit_credentials = rr->omit_credentials;
            if (rr->cache == 1)
                rq.no_store = true;
            else if (rr->cache == 2)
                rq.cache = AU_FETCH_CACHE_BYPASS;
            else if (rr->cache == 3)
                rq.cache = AU_FETCH_CACHE_REVALIDATE;
            else if (rr->cache == 4)
                rq.cache = AU_FETCH_CACHE_HISTORY;
            if (rr->referrer && !rr->referrer[0])
                rq.no_referrer = true;
            else if (rr->referrer)
                rq.document_url = rr->referrer;
            if (rr->referrer_policy)
                rq.referrer_policy = (AuFetchReferrerPolicy)rr->referrer_policy;
        }
    }
    log_begin(p, f, rq.method);
    f->fid = fe->start(fe->ctx, &rq, &f->sink);
    if (!f->fid) {
        au_netlog_end(&p->netlog, f->log, AU_NETREC_FAILED, NULL, log_ms(p));
        f->cancelled = true; /* removed at the next compaction */
        return NULL;
    }
    return f;
}

/* The document view's loader. Nested documents ask through the page's
 * view too (dv is always the top-level view); rq names the document. */
static uint32_t loader_request_ex(void *ctx, AuDocView *dv, const AuResourceRequest *rq)
{
    AuPage *p = ctx;
    const char *url = rq->url;
    AuResourceKind kind = rq->kind;
    if (dv != p->dv)
        return 0; /* a hidden document fetches nothing */
    if (p->nfetches > 2000)
        return 0;
    AuFetchKind fk = kind == AU_RES_STYLESHEET ? AU_FETCH_STYLESHEET
                     : kind == AU_RES_IMAGE    ? AU_FETCH_IMAGE
                     : kind == AU_RES_FONT     ? AU_FETCH_FONT
                     : kind == AU_RES_FRAME    ? AU_FETCH_FRAME
                                               : AU_FETCH_OTHER;
    Fetch *f = start_fetch(p, url, false, fk, rq);
    if (!f)
        return 0;
    f->script = rq->script_mode != 0 && !rq->cors_resource;
    f->id = ++p->next_id;
    p->requested++;
    p->stats.resources++;
    return f->id;
}

static uint32_t loader_request(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    AuResourceRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.kind = kind;
    return loader_request_ex(ctx, dv, &rq);
}

static void loader_cancel(void *ctx, uint32_t id)
{
    AuPage *p = ctx;
    for (size_t i = 0; i < p->nfetches; i++)
        if (!p->fetches[i]->main && p->fetches[i]->id == id)
            fetch_cancel(p->fetches[i]);
}

/* Backpressure from a script's response stream: the fetcher stops
 * reading the transfer (AuPageFetcher.pause). */
static void loader_pause(void *ctx, uint32_t id, bool paused)
{
    AuPage *p = ctx;
    for (size_t i = 0; i < p->nfetches; i++) {
        Fetch *f = p->fetches[i];
        if (!f->main && f->id == id && !f->done && !f->cancelled && f->fetcher && f->fetcher->pause)
            f->fetcher->pause(f->fetcher->ctx, f->fid, paused);
    }
}

/* ---- Document view callbacks ---- */

static void dv_title(void *ctx, AuDocView *dv, const char *title)
{
    AuPage *p = ctx;
    if (dv != p->dv)
        return; /* a hidden document */
    if (p->kind == K_DOC)
        set_title(p, title);
}

static void dv_invalidate(void *ctx, AuDocView *dv, AuIRect r)
{
    AuPage *p = ctx;
    if (dv != p->dv)
        return; /* a hidden document */
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
    if (dv != p->dv)
        return; /* a hidden document */
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
    if (dv != p->dv)
        return; /* a hidden document */
    if (p->cb.status_text)
        p->cb.status_text(p->cb.ctx, p, text);
}

static void dv_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    AuPage *p = ctx;
    if (dv != p->dv)
        return; /* a hidden document */
    set_str(&p->open_url, url);
    p->open_new_tab = new_tab;
}

/* ---- Forms ---- */

typedef char au_edit_cmds_match[((int)AU_PAGE_EDIT_UNDO == (int)AU_EDITCMD_UNDO &&
                                 (int)AU_PAGE_EDIT_CUT == (int)AU_EDITCMD_CUT &&
                                 (int)AU_PAGE_EDIT_COPY == (int)AU_EDITCMD_COPY &&
                                 (int)AU_PAGE_EDIT_PASTE == (int)AU_EDITCMD_PASTE &&
                                 (int)AU_PAGE_EDIT_CLEAR == (int)AU_EDITCMD_CLEAR &&
                                 (int)AU_PAGE_EDIT_SELECT_ALL == (int)AU_EDITCMD_SELECT_ALL)
                                    ? 1
                                    : -1];

static bool follow_open_url(AuPage *p);
static void start_load(AuPage *p, const char *url, bool entry, int rx, int ry, bool force);

static void clear_post(AuPage *p)
{
    if (p->post)
        p->no_keep = true; /* the document shown is a POST result: never kept */
    p->post = false;
    au_buf_free(&p->post_body);
    au_free(p->post_type);
    p->post_type = NULL;
}

/* A submission: GET goes the way of a link; POST waits in sub_* until the
 * input handler returns (follow_open_url). */
static void dv_submit(void *ctx, AuDocView *dv, const AuFormSubmission *s)
{
    AuPage *p = ctx;
    if (dv != p->dv)
        return; /* a hidden document */
    if (!s->method || strcmp(s->method, "POST") != 0) {
        set_str(&p->open_url, s->url);
        p->open_new_tab = s->new_tab;
        return;
    }
    au_free(p->open_url);
    p->open_url = NULL;
    set_str(&p->sub_url, s->url);
    set_str(&p->sub_type, s->body_type ? s->body_type : "application/x-www-form-urlencoded");
    au_buf_free(&p->sub_body);
    au_buf_append(&p->sub_body, s->body ? s->body : "", s->body_len);
    p->sub_new_tab = s->new_tab;
}

static int dv_popup(void *ctx, AuDocView *dv, const AuPopupItem *items, int n, int selected, AuIRect at)
{
    AuPage *p = ctx;
    (void)dv;
    return p->cb.popup ? p->cb.popup(p->cb.ctx, p, items, n, selected, at) : -2;
}

static bool dv_choose_file(void *ctx, AuDocView *dv, bool multiple, AuControlFile **files, uint32_t *n)
{
    AuPage *p = ctx;
    (void)dv;
    return p->cb.choose_file && p->cb.choose_file(p->cb.ctx, p, multiple, files, n);
}

static bool dv_confirm(void *ctx, AuDocView *dv, const char *message)
{
    AuPage *p = ctx;
    (void)dv;
    return !p->cb.confirm || p->cb.confirm(p->cb.ctx, p, message);
}

static void dv_message(void *ctx, AuDocView *dv, const char *text)
{
    AuPage *p = ctx;
    if (dv != p->dv)
        return; /* a hidden document */
    if (p->cb.status_text)
        p->cb.status_text(p->cb.ctx, p, text);
}

/* Hands a POST submission to the browser (a history entry or a new tab);
 * without the callback the page loads it itself. */
static bool follow_submit(AuPage *p)
{
    if (!p->sub_url)
        return false;
    char *u = p->sub_url, *ty = p->sub_type;
    AuBuf body = p->sub_body;
    p->sub_url = p->sub_type = NULL;
    au_buf_init(&p->sub_body);
    if (!p->internal && (au_url_scheme_is(p->url, "http") || au_url_scheme_is(p->url, "https"))) {
        set_str(&p->link_referrer, p->url);
        set_str(&p->link_target, u);
    }
    if (p->cb.submit_post)
        p->cb.submit_post(p->cb.ctx, p, u, body.data ? body.data : "", body.len, ty, p->sub_new_tab);
    else
        au_page_navigate_post(p, u, body.data ? body.data : "", body.len, ty);
    au_buf_free(&body);
    au_free(u);
    au_free(ty);
    return true;
}

void au_page_navigate_post(AuPage *p, const char *url, const void *body, size_t len, const char *type)
{
    char *u = au_strndup(url, strlen(url)); /* url may be p->url */
    clear_post(p); /* notes whether the document shown was a POST result */
    p->post = true;
    au_buf_append(&p->post_body, body, len);
    p->post_type = au_strndup(type ? type : "", type ? strlen(type) : 0);
    p->cache_mode = AU_FETCH_CACHE_BYPASS;
    start_load(p, u, false, 0, 0, true);
    au_free(u);
}

bool au_page_is_post(const AuPage *p)
{
    return p->post;
}

/* The view that has the keyboard: the page's, or a nested document's. */
static AuDocView *key_view(const AuPage *p)
{
    return au_docview_focus_view(p->dv);
}

bool au_page_editing(const AuPage *p)
{
    return p->kind == K_DOC && p->begun && au_docview_editing(key_view(p));
}

bool au_page_can_edit(const AuPage *p, int cmd)
{
    return au_page_editing(p) && au_docview_can_edit(key_view(p), cmd);
}

bool au_page_edit(AuPage *p, int cmd, const char *text, size_t len, AuBuf *out)
{
    if (!au_page_editing(p))
        return false;
    bool ok = au_docview_edit(key_view(p), cmd, text, len, out);
    follow_open_url(p);
    return ok;
}

bool au_page_text_field_at(AuPage *p, int x, int y)
{
    AuHitResult r;
    if (p->kind != K_DOC || !p->begun)
        return false;
    AuDocView *v = au_docview_view_at(p->dv, &x, &y);
    if (!au_docview_hit_test(v, x, y, &r))
        return false;
    for (AuElement *e = r.element; e; e = au_is_element(e->node.parent) ? au_as_element(e->node.parent) : NULL)
        if (au_ctl_is_text_field(e))
            return true;
    return false;
}

bool au_page_control_rect(AuPage *p, const char *name, AuIRect *r)
{
    if (p->kind != K_DOC || !p->begun || !au_docview_document(p->dv))
        return false;
    AuDocument *d = au_docview_document(p->dv);
    au_docview_update(p->dv);
    AuAtom an = au_atom_lookup("name", 4);
    for (AuNode *c = au_node_next(&d->node, &d->node); c; c = au_node_next(c, &d->node)) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        AuElement *el = au_as_element(c);
        const char *v = au_element_get_attr(el, an);
        AuRect b;
        if (v && !strcmp(v, name) && au_layout_element_rect(au_docview_layout(p->dv), el, &b) && b.w > 0) {
            *r = (AuIRect){(int)b.x, (int)b.y, (int)(b.x + b.w), (int)(b.y + b.h)};
            return true;
        }
    }
    return false;
}

void au_page_blur(AuPage *p)
{
    if (p->kind == K_DOC && p->begun) {
        AuDocView *v = key_view(p);
        au_docview_set_focus(v, NULL);
    }
}

/* ---- Pending scroll ---- */

static void request_scroll(AuPage *p, int x, int y)
{
    if (p->cb.scroll_to)
        p->cb.scroll_to(p->cb.ctx, p, x, y);
}

/* ---- Script callbacks ---- */

static double dv_now(void *ctx, AuDocView *dv)
{
    (void)dv;
    return now(ctx);
}

/* crypto.getRandomValues: bytes from the platform's entropy source, or
 * false (the call throws) while it is not ready. */
static bool dv_random(void *ctx, AuDocView *dv, void *buf, size_t len)
{
    AuPage *p = ctx;
    (void)dv;
    return p->env.random && p->env.random(p->env.ctx, buf, len);
}

static void dv_scroll_to(void *ctx, AuDocView *dv, int x, int y)
{
    if (dv == ((AuPage *)ctx)->dv)
        request_scroll(ctx, x, y);
}

/* document.cookie: the scheme's fetcher owns the cookie jar. A view that
 * is not the page's current one (a kept document running pagehide)
 * reads nothing and sets nothing. */
static char *dv_get_cookie(void *ctx, AuDocView *dv, const char *url, bool nested)
{
    AuPage *p = ctx;
    const AuPageFetcher *fe = dv == p->dv ? fetcher_for(p, url) : NULL;
    if (!fe || !fe->get_cookie)
        return NULL;
    return fe->get_cookie(fe->ctx, url, nested ? p->url : NULL);
}

static void dv_set_cookie(void *ctx, AuDocView *dv, const char *url, bool nested, const char *value)
{
    AuPage *p = ctx;
    const AuPageFetcher *fe = dv == p->dv ? fetcher_for(p, url) : NULL;
    if (fe && fe->set_cookie)
        fe->set_cookie(fe->ctx, url, nested ? p->url : NULL, value);
}

/* localStorage and sessionStorage: the application's service
 * (net/web_storage.h) decides the document's area from its origin and,
 * for a nested document, the top-level URL; sessionStorage is the tab's
 * namespace. A document kept for Back may still write (its pagehide
 * handler saves state): the areas belong to the origin and the tab, not
 * to the document. */
static int dv_storage(void *ctx, AuDocView *dv, AuJsStorageCall *c)
{
    AuPage *p = ctx;
    AuWebStorage *ws = p->env.storage(p->env.ctx);
    (void)dv;
    return ws ? au_web_storage_call(ws, p->storage_ns, c) : AU_JS_STORAGE_DENIED;
}

void au_page_set_storage_ns(AuPage *p, uint32_t ns)
{
    p->storage_ns = ns;
}

void au_page_storage_event(AuPage *p, const AuStorageChange *c)
{
    if (!p || !p->dv || (c->kind == AU_STORAGE_SESSION && c->ns != p->storage_ns))
        return;
    AuDocStorageEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = c->kind == AU_STORAGE_SESSION ? AU_JS_STORAGE_SESSION : AU_JS_STORAGE_LOCAL;
    ev.storage_key = c->storage_key;
    ev.writer = c->writer;
    ev.key = c->key;
    ev.key_len = c->key_len;
    ev.old_value = c->old_value;
    ev.old_len = c->old_len;
    ev.new_value = c->new_value;
    ev.new_len = c->new_len;
    ev.url = c->url;
    au_docview_storage_event(p->dv, &ev);
}

static void dv_console(void *ctx, AuDocView *dv, int level, const char *text, size_t len)
{
    AuPage *p = ctx;
    (void)dv;
    size_t i = (p->console_start + p->console_count) % AU_PAGE_CONSOLE_LINES;
    if (p->console_count == AU_PAGE_CONSOLE_LINES) {
        au_free(p->console[i]);
        p->console_start = (p->console_start + 1) % AU_PAGE_CONSOLE_LINES;
    } else {
        p->console_count++;
    }
    p->console[i] = au_strndup(text, len < 1024 ? len : 1024);
    p->console_level[i] = (uint8_t)level;
    p->console_total++;
    p->console_current++;
}

static void dv_script_oom(void *ctx, AuDocView *dv, bool worker, bool machine, uint32_t limit_kb)
{
    AuPage *p = ctx;
    (void)dv;
    if (p->oom_due || p->oom_told)
        return; /* the first one of this page says it */
    p->oom.worker = worker;
    p->oom.machine = machine;
    p->oom.limit_kb = limit_kb;
    p->oom_due = true;
}

bool au_page_take_script_oom(AuPage *p, AuScriptOom *out)
{
    if (!p || !p->oom_due)
        return false;
    p->oom_due = false;
    p->oom_told = true;
    *out = p->oom;
    return true;
}

static void internal_action(AuPage *p, const char *what);

/* Hands a navigation from a link or script to the browser. */
static bool follow_open_url(AuPage *p)
{
    if (follow_submit(p))
        return true;
    if (!p->open_url)
        return false;
    char *u = p->open_url;
    p->open_url = NULL;
    if (starts_with_ci(u, "aura-action:")) {
        /* Only Aura's own pages may ask for these. */
        if (p->internal)
            internal_action(p, u + 12);
    } else {
        /* The navigation this link causes has this page as referrer. */
        if (!p->open_new_tab && !p->internal &&
            (au_url_scheme_is(p->url, "http") || au_url_scheme_is(p->url, "https"))) {
            set_str(&p->link_referrer, p->url);
            set_str(&p->link_target, u);
        }
        if (p->cb.open_url)
            p->cb.open_url(p->cb.ctx, p, u, p->open_new_tab);
    }
    au_free(u);
    return true;
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
    au_netlog_mark(&p->netlog, p->netlog.dcl_ms ? 0 : p->stats.load_ms, p->stats.load_ms);
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

/* The view the selection is in: the page's, or a nested document's
 * (sel_dv, with the document it showed when the selection was made). A
 * frame that was removed or navigated since takes its selection with
 * it: the positions are dropped without touching its layout. */
static AuDocView *sel_view(AuPage *p)
{
    if (!p->sel_dv)
        return p->dv;
    if (au_docview_contains(p->dv, p->sel_dv) && au_docview_document(p->sel_dv) == p->sel_doc)
        return p->sel_dv;
    AuTextPos none = {NULL, 0};
    set_pos(&p->anchor, none);
    set_pos(&p->focus, none);
    set_pos(&p->sel_base.start, none);
    set_pos(&p->sel_base.end, none);
    p->selecting = false;
    if (p->sel_doc)
        au_node_unref(&p->sel_doc->node);
    p->sel_doc = NULL;
    p->sel_dv = NULL;
    return p->dv;
}

/* Moves the selection's home to view v (the page's own: NULL). */
static void sel_set_view(AuPage *p, AuDocView *v)
{
    if (v == p->dv)
        v = NULL;
    if (v == p->sel_dv && (!v || au_docview_document(v) == p->sel_doc))
        return;
    au_page_clear_selection(p);
    if (p->sel_doc)
        au_node_unref(&p->sel_doc->node);
    p->sel_dv = v;
    p->sel_doc = v ? au_docview_document(v) : NULL;
    if (p->sel_doc)
        au_node_ref(&p->sel_doc->node);
}

/* Hands the current selection to the layout and repaints. */
static void sel_apply(AuPage *p)
{
    AuTextRange r = sel_range(p);
    uint32_t col = p->env.highlight ? p->env.highlight : DEFAULT_HIGHLIGHT;
    au_layout_set_selection(au_docview_layout(sel_view(p)), au_text_range_empty(&r) ? NULL : &r, col);
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
    au_layout_set_selection(au_docview_layout(sel_view(p)), NULL, 0);
    AuTextPos none = {NULL, 0};
    set_pos(&p->anchor, none);
    set_pos(&p->focus, none);
    set_pos(&p->sel_base.start, none);
    set_pos(&p->sel_base.end, none);
    p->selecting = false;
    if (p->sel_doc)
        au_node_unref(&p->sel_doc->node);
    p->sel_doc = NULL;
    p->sel_dv = NULL; /* the next selection starts in the page's own document */
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
    AuDocView *v = sel_view(p);
    if (!au_page_has_selection(p))
        return false;
    au_docview_update(v);
    AuTextRange r = sel_range(p);
    size_t before = out->len;
    au_layout_range_text(au_docview_layout(v), &r, out);
    return out->len > before;
}

void au_page_select_all(AuPage *p)
{
    if (p->kind != K_DOC || !p->begun)
        return;
    /* in the document that has the keyboard */
    AuDocView *v = key_view(p);
    sel_set_view(p, v);
    au_docview_update(v);
    AuTextRange all;
    if (au_layout_all_text(au_docview_layout(v), &all))
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
    /* Find searches the document the selection is in (a nested document
     * after a click in it), the page's own otherwise. */
    AuDocView *v = sel_view(p);
    au_docview_update(v);
    AuLayout *L = au_docview_layout(v);
    AuTextRange cur = sel_range(p), m;
    AuTextPos from = {NULL, 0};
    if (cur.start.node)
        from = forward ? cur.end : cur.start;
    if (!au_layout_find(L, text, &from, forward, !case_sensitive, &m))
        return false;
    sel_set(p, m.start, m.end);
    AuRect r;
    if (v == p->dv && au_layout_range_rect(L, &m, &r))
        reveal(p, r);
    return true;
}

/* The text position at document point (x, y) of the page, in the view
 * the selection is in. */
static bool pos_at(AuPage *p, int x, int y, AuTextPos *out)
{
    AuDocView *v = sel_view(p);
    int sx = p->scroll_x, sy = p->scroll_y;
    if (v != p->dv) {
        au_docview_point_from_top(v, &x, &y);
        au_docview_scroll_pos(v, &sx, &sy);
    }
    au_docview_update(v);
    return au_layout_text_pos_at(au_docview_layout(v), (float)(x - sx), (float)(y - sy), (float)sx, (float)sy, out);
}

/* Extends a word or paragraph selection to the unit at q. */
static void extend_by_unit(AuPage *p, AuTextPos q)
{
    AuLayout *L = au_docview_layout(sel_view(p));
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
        /* the selection lives in the document under the pointer */
        int vx = ev->x, vy = ev->y;
        AuDocView *home = au_docview_view_at(p->dv, &vx, &vy);
        sel_set_view(p, home);
        bool have = pos_at(p, ev->x, ev->y, &q);
        p->selecting = true;
        p->sel_dragged = false;
        p->down_x = ev->x;
        p->down_y = ev->y;
        p->sel_mode = ev->clicks >= 3 ? 3 : ev->clicks == 2 ? 2 : 1;
        if (!have) {
            au_page_clear_selection(p);
            sel_set_view(p, home);
            p->selecting = true;
            return false;
        }
        AuLayout *L = au_docview_layout(sel_view(p));
        if ((ev->mods & AU_MOD_SHIFT) && p->anchor.node) {
            p->sel_mode = 1;
            sel_set(p, p->anchor, q);
            return true;
        }
        AuTextRange u;
        if (p->sel_mode > 1 && (p->sel_mode == 2 ? au_layout_word_at(L, &q, &u) : au_layout_block_at(L, &q, &u))) {
            /* sel_base holds references as the anchor and focus do:
             * script may remove and free a text node while the document
             * stays, and the next drag compares positions with it. */
            set_pos(&p->sel_base.start, u.start);
            set_pos(&p->sel_base.end, u.end);
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

/* ---- Kept documents ----
 * The document cache for Back and Forward (page.h). A document that the
 * tab leaves moves, with its view, into p->kept, and the page gets a new
 * view for the next document. The old document stays on screen until the
 * new one begins, so the move happens in two steps: start_load records
 * what the page knows about the shown document in p->leaving, and the
 * first begin_doc of the load (hide_leaving) moves the view. A load that
 * never begins (a download, a stop) leaves the document where it is.
 *
 * Nothing calls a hidden view, so its timers, frames and animations
 * wait; the view's callbacks and loader ignore a view that is not p->dv,
 * so script that runs in pagehide cannot change the page or fetch. */

static void new_view(AuPage *p)
{
    AuLoader loader = {p, loader_request, loader_cancel, sniff_image, loader_pause};
    AuDocViewCallbacks dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.ctx = p;
    dcb.title_changed = dv_title;
    dcb.invalidate = dv_invalidate;
    dcb.size_changed = dv_size;
    dcb.status_text = dv_status;
    dcb.open_url = dv_open;
    dcb.now_ms = dv_now;
    dcb.scroll_to = dv_scroll_to;
    dcb.console = dv_console;
    dcb.script_oom = dv_script_oom;
    dcb.random = p->env.random ? dv_random : NULL;
    dcb.coro = p->env.coro;
    dcb.get_cookie = dv_get_cookie;
    dcb.set_cookie = dv_set_cookie;
    dcb.storage = p->env.storage ? dv_storage : NULL;
    dcb.submit = dv_submit;
    dcb.popup = dv_popup;
    dcb.choose_file = dv_choose_file;
    dcb.confirm = dv_confirm;
    dcb.message = dv_message;
    p->dv = au_docview_new(p->env.fonts, &loader, &dcb);
    au_docview_set_request_ex(p->dv, loader_request_ex);
    au_docview_set_viewport(p->dv, p->vw, p->vh);
    au_docview_set_clock(p->dv, p->env.time_ms, p->env.ctx);
    apply_font_prefs(p);
}

static void kept_free(Kept *k)
{
    if (k->dv)
        au_docview_free(k->dv);
    au_free(k->url);
    au_free(k->title);
    au_free(k->content_type);
    au_free(k->cert);
    source_free(&k->src);
    au_netlog_free(&k->netlog);
    au_free(k);
}

/* Gives the page the request log held in from (a kept document's): the
 * page's own goes, and views see a change. */
static void netlog_take(AuPage *p, AuNetLog *from)
{
    uint32_t gen = p->netlog.generation + from->generation + 1;
    au_netlog_free(&p->netlog);
    p->netlog = *from;
    p->netlog.generation = gen;
    au_netlog_init(from);
}

static size_t keep_max(const AuPage *p)
{
    return p->env.prefs ? (size_t)p->env.prefs->document_cache_pages : 3;
}

static size_t keep_budget(const AuPage *p)
{
    return p->env.prefs ? (size_t)p->env.prefs->document_cache_kb * 1024 : (size_t)24 << 20;
}

/* Removes kept[i] from the list first, then frees it: freeing can run
 * out of memory, and the low-memory path drops kept documents. */
static void kept_remove(AuPage *p, size_t i)
{
    Kept *k = p->kept[i];
    memmove(&p->kept[i], &p->kept[i + 1], (p->nkept - i - 1) * sizeof *p->kept);
    p->nkept--;
    kept_free(k);
}

static void kept_drop(AuPage *p, uint32_t entry)
{
    if (!entry)
        return;
    for (size_t i = p->nkept; i-- > 0;)
        if (p->kept[i]->entry == entry)
            kept_remove(p, i);
}

/* Applies the limits: the count per page, and no document over the budget. */
static void kept_trim(AuPage *p)
{
    size_t max = keep_max(p), budget = keep_budget(p);
    for (size_t i = p->nkept; i-- > 0;)
        if (p->kept[i]->bytes > budget)
            kept_remove(p, i);
    while (p->nkept > max)
        kept_remove(p, 0);
}

/* Whether the document shown may be kept when the tab leaves it. */
static bool can_keep(AuPage *p)
{
    if (!p->doc_entry || !keep_max(p))
        return false;
    if (p->kind != K_DOC || !p->begun || p->loading || p->failed || p->internal || p->interim || p->no_keep ||
        p->no_store)
        return false;
    if (p->nfetches) /* transfers under way (a script's fetch, a late image) */
        return false;
    return au_docview_can_persist(p->dv);
}

/* Records what the page knows about the shown document, before a load
 * overwrites it. The view follows in hide_leaving. */
static Kept *kept_capture(AuPage *p)
{
    Kept *k = au_calloc(sizeof *k);
    k->entry = p->doc_entry;
    k->url = au_strndup(p->url, strlen(p->url));
    k->title = au_strndup(p->title, strlen(p->title));
    k->content_type = au_strndup(p->content_type ? p->content_type : "", p->content_type ? strlen(p->content_type) : 0);
    if (p->cert) {
        k->cert = au_alloc(sizeof *k->cert);
        *k->cert = *p->cert;
    }
    k->security = p->security;
    k->have_security = p->have_security;
    k->mixed = p->mixed;
    k->legacy_kex = p->legacy_kex;
    k->status = p->status;
    k->rpolicy = p->rpolicy;
    k->image_doc = p->image_doc;
    k->plain = p->plain;
    k->source_mode = p->source_mode;
    source_move(&k->src, &p->src);
    k->netlog = p->netlog;
    au_netlog_init(&p->netlog);
    k->content_w = p->content_w;
    k->content_h = p->content_h;
    k->stats = p->stats;
    return k;
}

/* Hides view dv, the document of k, and adds it to the kept list. p->dv
 * is already another view. */
static void kept_add(AuPage *p, Kept *k, AuDocView *dv)
{
    k->dv = dv;
    au_docview_set_hidden(dv, true); /* pagehide */
    k->stamp = now(p);
    source_pack(&k->src, p); /* left before the page's idle got to it */
    au_buf_free(k->src.z.len ? &k->src.plain : &k->src.z);
    k->bytes = au_docview_memory_estimate(dv) + source_held(&k->src);
    kept_drop(p, k->entry);
    p->kept = au_realloc(p->kept, (p->nkept + 1) * sizeof *p->kept);
    p->kept[p->nkept++] = k;
    kept_trim(p);
}

/* The load under way begins its document: the one shown until now moves
 * aside with its view, and the page gets a new view. */
static void hide_leaving(AuPage *p)
{
    Kept *k = p->leaving;
    if (!k)
        return;
    p->leaving = NULL;
    AuDocView *old = p->dv;
    new_view(p);
    kept_add(p, k, old);
}

/* A back/forward navigation to an entry whose document is kept: shows it
 * again without loading. keep says whether the document shown now may be
 * kept in turn. Returns false when there is none (the caller loads). */
static bool show_kept(AuPage *p, const char *url, int rx, int ry, bool keep)
{
    uint32_t entry = p->next_entry;
    if (!entry)
        return false;
    Kept *k = NULL;
    bool in_place = false;
    if (p->leaving && p->leaving->entry == entry) {
        k = p->leaving; /* still on screen: the load that was to replace it never began */
        in_place = true;
    } else {
        for (size_t i = 0; i < p->nkept; i++)
            if (p->kept[i]->entry == entry)
                k = p->kept[i];
    }
    if (!k)
        return false;
    size_t a = base_len(url);
    if (a != base_len(k->url) || memcmp(url, k->url, a) != 0) {
        /* The entry names another address now: load it. */
        if (in_place) {
            kept_free(k);
            p->leaving = NULL;
        } else {
            kept_drop(p, entry);
        }
        return false;
    }
    uint32_t t0 = now(p), hide_ms = 0;
    if (in_place) {
        p->leaving = NULL;
    } else {
        for (size_t i = 0; i < p->nkept; i++) {
            if (p->kept[i] == k) {
                memmove(&p->kept[i], &p->kept[i + 1], (p->nkept - i - 1) * sizeof *p->kept);
                p->nkept--;
                break;
            }
        }
        /* The document shown now is kept or freed, and k's view shown. */
        Kept *cur = p->leaving;
        p->leaving = NULL;
        if (!cur && keep)
            cur = kept_capture(p);
        if (au_docview_hover_link(p->dv)[0] && p->cb.status_text)
            p->cb.status_text(p->cb.ctx, p, "");
        AuDocView *old = p->dv;
        p->dv = k->dv;
        k->dv = NULL;
        if (cur)
            kept_add(p, cur, old);
        else
            au_docview_free(old);
        hide_ms = now(p) - t0;
    }
    /* The page's state becomes the kept document's. */
    set_str(&p->url, url);
    au_free(p->cert);
    p->cert = k->cert;
    k->cert = NULL;
    au_free(p->prev_cert);
    p->prev_cert = NULL;
    p->security = k->security;
    p->have_security = k->have_security;
    p->mixed = k->mixed;
    p->legacy_kex = k->legacy_kex;
    p->status = k->status;
    set_str(&p->content_type, k->content_type);
    p->rpolicy = k->rpolicy;
    p->image_doc = k->image_doc;
    p->plain = k->plain;
    p->source_mode = k->source_mode;
    source_move(&p->src, &k->src);
    netlog_take(p, &k->netlog);
    /* The timings now describe the restore: load_ms the whole of it,
     * first_paint_ms up to the next paint. */
    p->stats = k->stats;
    p->stats.restored++;
    p->stats.hide_ms = hide_ms;
    p->stats.first_paint_ms = 0;
    p->kind = K_DOC;
    p->internal = p->interim = p->failed = p->discard = false;
    p->no_store = false;
    p->begun = true;
    p->main_done = true;
    p->main = MAIN_NONE;
    p->main_total = 0;
    p->requested = p->completed = 0;
    p->last_status[0] = 0;
    p->dirty = false;
    p->restore = false;
    au_free(p->fragment);
    p->fragment = NULL;
    au_free(p->referrer);
    p->referrer = NULL;
    au_free(p->link_target);
    au_free(p->link_referrer);
    p->link_target = p->link_referrer = NULL;
    p->doc_entry = entry;
    p->next_entry = 0;
    p->loading = true;
    p->progress = 0;
    p->load_start = t0;
    if (p->cb.load_started)
        p->cb.load_started(p->cb.ctx, p);
    if (p->cb.url_changed)
        p->cb.url_changed(p->cb.ctx, p, p->url);
    set_str(&p->title, k->title);
    if (p->cb.title_changed)
        p->cb.title_changed(p->cb.ctx, p, p->title);
    /* The window may have been resized and the zoom or fonts changed
     * while the document was hidden. */
    au_docview_set_viewport(p->dv, p->vw, p->vh);
    apply_font_prefs(p);
    p->content_w = k->content_w;
    p->content_h = k->content_h;
    /* A kept document freed its layout: this update builds it again,
     * before the first paint. */
    uint32_t t1 = now(p);
    au_docview_update(p->dv);
    p->stats.rebuild_ms = now(p) - t1;
    float w, h;
    au_layout_document_size(au_docview_layout(p->dv), &w, &h);
    p->content_w = (int)(w + 0.99f);
    p->content_h = (int)(h + 0.99f);
    if (p->cb.content_size_changed)
        p->cb.content_size_changed(p->cb.ctx, p);
    kept_free(k);
    request_scroll(p, rx, ry);
    if (!in_place)
        au_docview_set_hidden(p->dv, false); /* pageshow */
    invalidate_all(p);
    finish(p, true);
    return true;
}

void au_page_set_entry(AuPage *p, uint32_t entry)
{
    p->next_entry = entry;
}

void au_page_retain_entries(AuPage *p, const uint32_t *ids, size_t n)
{
    for (size_t i = p->nkept; i-- > 0;) {
        bool live = false;
        for (size_t k = 0; k < n && !live; k++)
            live = ids[k] == p->kept[i]->entry;
        if (!live)
            kept_remove(p, i);
    }
}

size_t au_page_kept_count(const AuPage *p)
{
    return p->nkept;
}

size_t au_page_kept_bytes(const AuPage *p)
{
    size_t n = 0;
    for (size_t i = 0; i < p->nkept; i++)
        n += p->kept[i]->bytes;
    return n;
}

size_t au_page_memory_estimate(const AuPage *p)
{
    return (p->dv ? au_docview_memory_estimate(p->dv) : 0) + source_held(&p->src);
}

bool au_page_kept_oldest(const AuPage *p, uint32_t *stamp)
{
    if (!p->nkept)
        return false;
    *stamp = p->kept[0]->stamp;
    return true;
}

bool au_page_evict_oldest(AuPage *p)
{
    if (!p->nkept)
        return false;
    kept_remove(p, 0);
    return true;
}

size_t au_page_drop_kept(AuPage *p)
{
    size_t n = 0;
    while (p->nkept) {
        kept_remove(p, p->nkept - 1);
        n++;
    }
    return n;
}

/* ---- Discarding the document (page.h) ---- */

static void cancel_all(AuPage *p);

bool au_page_can_discard(AuPage *p)
{
    if (p->discarded || p->kind != K_DOC || !p->begun || p->loading || p->failed || p->internal || p->interim)
        return false;
    if (p->in_idle || p->leaving || p->open_url || p->sub_url || p->post || p->no_keep)
        return false;
    compact_fetches(p);
    if (p->nfetches)
        return false;
    return au_docview_can_persist(p->dv) && !au_docview_has_state(p->dv);
}

/* The page's state for a document that is not there. */
static void set_discarded(AuPage *p)
{
    p->discarded = true;
    p->begun = false;
    p->loading = p->failed = p->internal = p->interim = p->discard = false;
    p->main = MAIN_NONE;
    p->main_done = false;
    p->dirty = false;
    p->restore = false;
    p->progress = 0;
    p->requested = p->completed = 0;
    p->last_status[0] = 0;
    p->content_w = p->vw;
    p->content_h = p->vh;
    memset(&p->stats, 0, sizeof p->stats);
}

void au_page_discard(AuPage *p)
{
    if (p->discarded)
        return;
    cancel_all(p);
    compact_fetches(p);
    au_page_clear_selection(p);
    if (p->leaving)
        kept_free(p->leaving);
    p->leaving = NULL;
    /* The page is in its discarded state before anything is freed:
     * freeing can run out of memory, and the low-memory path discards. */
    AuDocView *old = p->dv;
    new_view(p);
    set_discarded(p);
    au_page_drop_kept(p);
    au_docview_free(old);
    source_free(&p->src);
    uint32_t gen = p->netlog.generation + 1;
    au_netlog_free(&p->netlog);
    au_netlog_init(&p->netlog);
    p->netlog.generation = gen;
    au_page_console_clear(p);
    au_free(p->fragment);
    p->fragment = NULL;
    p->gen++;
}

bool au_page_discarded(const AuPage *p)
{
    return p->discarded;
}

void au_page_set_discarded(AuPage *p, const char *url, const char *title)
{
    if (p->begun || p->loading)
        return;
    set_str(&p->url, url);
    set_str(&p->title, title ? title : "");
    set_discarded(p);
}

/* ---- Documents ---- */

/* Starts the new document in the view (the old one stays visible until
 * then). */
static void begin_doc(AuPage *p, const char *url)
{
    au_page_clear_selection(p);
    if (au_docview_hover_link(p->dv)[0] && p->cb.status_text)
        p->cb.status_text(p->cb.ctx, p, "");
    hide_leaving(p); /* the document shown until now is kept, in a view of its own */
    p->kind = K_DOC;
    p->internal = false;
    p->interim = false;
    p->discarded = false;
    au_docview_set_scripting(p->dv, !p->env.prefs || p->env.prefs->javascript);
    {
        /* Script memory limits for this document's runtime and workers:
         * the preferences, or what Automatic gives with the memory free
         * now. A change of preference applies from the next load. */
        uint32_t page_kb, worker_kb;
        au_script_memory_limits(p->env.prefs ? p->env.prefs->script_memory_kb : 0,
                                p->env.prefs ? p->env.prefs->worker_memory_kb : 0,
                                p->env.memory_free_kb ? p->env.memory_free_kb(p->env.ctx) : 0, &page_kb, &worker_kb);
        au_docview_set_script_memory(p->dv, (size_t)page_kb * 1024, (size_t)worker_kb * 1024);
    }
    au_docview_set_user_agent(p->dv, au_prefs_user_agent(p->env.prefs));
    {
        /* document.referrer: what the network layer sent as Referer for
         * this navigation (the default policy cuts a cross-origin
         * referrer to its origin and sends none from https to http). */
        AuBuf ref;
        AuUrl target;
        au_buf_init(&ref);
        if (p->referrer && !p->internal && au_url_parse(&target, url, NULL)) {
            if (!au_referrer_compute(p->referrer, &target, AU_RP_DEFAULT, &ref))
                au_buf_clear(&ref);
            au_url_free(&target);
        }
        au_docview_set_referrer(p->dv, ref.len ? au_buf_cstr(&ref) : NULL);
        au_buf_free(&ref);
    }
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
    source_append(&p->src, html, len);
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
                   "h3 { font-size: 14px; margin: 14px 0 4px; }"
                   "p { margin: 0 0 10px; }"
                   ".note { color: #5a5a5a; font-size: 13px; }"
                   ".url { font-family: monospace; color: #5a5a5a; font-size: 13px; }"
                   "ul { margin: 0; padding-left: 22px; } li { margin: 3px 0; }"
                   "a { color: #0000cc; }"
                   "</style></head><body>");
}

/* ---- The start page (about:home) ---- */

/* #embed is C23; the compilers Aura uses (Clang, GCC 15 or later) take it
 * in C99 mode with a warning. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
static const unsigned char home_icon_png[] = {
#embed "home.png"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* Sites that work well on an old Mac: light pages, little script. */
static const struct {
    const char *name, *url, *what;
} home_sites[] = {
    {"Wikipedia", "https://en.wikipedia.org/", "The free encyclopedia"},
    {"Hacker News", "https://news.ycombinator.com/", "Technology news"},
    {"CNN Lite", "https://lite.cnn.com/", "News, text only"},
    {"NPR Text", "https://text.npr.org/", "News, text only"},
    {"68k.news", "http://68k.news/", "Headlines for vintage computers"},
    {"FrogFind", "http://frogfind.com/", "Search that simplifies pages"},
    {"Macintosh Repository", "https://www.macintoshrepository.org/", "Classic Mac software"},
    {"Mac OS 9 Lives", "https://macos9lives.com/", "The Mac OS 9 community"},
};

static const char *const home_tips[] = {
    "Command-L puts you in the address field; type words there to search.",
    "Command-click a link to open it in a new tab.",
    "Command-T opens a new tab, and Command-W closes it.",
    "Content blocking in Preferences keeps trackers and ads from loading, which makes pages faster.",
    "Develop &gt; Performance shows where a slow page spends its time.",
    "Help &gt; Report a Problem writes up a bug report you can paste into GitHub.",
    "Choose File &gt; Open File to open an HTML or text file, or drop one on the Aura icon.",
    "Develop &gt; Console shows a page's script errors.",
};

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Appends s percent-decoded ('+' as a space). */
static void url_decode_into(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi, lo;
        if (s[i] == '+') {
            au_buf_putc(out, ' ');
        } else if (s[i] == '%' && i + 2 < n && (hi = hex_digit(s[i + 1])) >= 0 && (lo = hex_digit(s[i + 2])) >= 0) {
            au_buf_putc(out, (char)(hi * 16 + lo));
            i += 2;
        } else {
            au_buf_putc(out, s[i]);
        }
    }
}

/* The search form for a template such as
 * "https://html.duckduckgo.com/html/?q=%s": the form posts to the URL
 * before '?' by GET, the parameter whose value is "%s" becomes the text
 * field and the others hidden fields. Returns false, writing nothing,
 * when no query parameter holds exactly "%s". */
static bool home_search_form(AuBuf *b, const char *tpl)
{
    const char *q = tpl ? strchr(tpl, '?') : NULL;
    if (!q)
        return false;
    size_t end = strcspn(q + 1, "#");
    const char *field = NULL;
    size_t field_len = 0;
    for (const char *s = q + 1; s < q + 1 + end;) {
        size_t n = strcspn(s, "&#");
        const char *eq = memchr(s, '=', n);
        if (eq && (size_t)(s + n - eq - 1) == 2 && !memcmp(eq + 1, "%s", 2)) {
            field = s;
            field_len = (size_t)(eq - s);
        }
        s += n + (s[n] == '&');
    }
    if (!field || !field_len)
        return false;
    AuBuf v;
    au_buf_init(&v);
    au_buf_puts(b, "<form class=search method=get action='");
    html_escape(b, tpl, (size_t)(q - tpl));
    au_buf_puts(b, "'><input type=search size=40 placeholder='Search the web' name='");
    au_buf_clear(&v);
    url_decode_into(&v, field, field_len);
    html_escape_str(b, au_buf_cstr(&v));
    au_buf_puts(b, "'>");
    for (const char *s = q + 1; s < q + 1 + end;) {
        size_t n = strcspn(s, "&#");
        const char *eq = memchr(s, '=', n);
        if (s != field && n) {
            au_buf_puts(b, "<input type=hidden name='");
            au_buf_clear(&v);
            url_decode_into(&v, s, eq ? (size_t)(eq - s) : n);
            html_escape_str(b, au_buf_cstr(&v));
            au_buf_puts(b, "' value='");
            au_buf_clear(&v);
            if (eq)
                url_decode_into(&v, eq + 1, (size_t)(s + n - eq - 1));
            html_escape_str(b, au_buf_cstr(&v));
            au_buf_puts(b, "'>");
        }
        s += n + (s[n] == '&');
    }
    au_buf_puts(b, " <input type=submit value='Search'></form>");
    au_buf_free(&v);
    return true;
}

/* The host of an http or https URL without "www.", for a tile's second
 * line. */
static void home_host(AuBuf *b, const char *url)
{
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
        html_escape_str(b, url);
        return;
    }
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    if (!strncmp(s, "www.", 4))
        s += 4;
    html_escape(b, s, strcspn(s, "/?#:"));
}

/* A tile: a link with a title line and a note line. The title is cut to
 * 40 bytes at a character boundary. */
static void home_tile(AuBuf *b, const char *url, const char *title, const char *note, bool note_is_host)
{
    au_buf_puts(b, "<a class=tile href='");
    html_escape_str(b, url);
    au_buf_puts(b, "'><b>");
    size_t n = strlen(title);
    bool cut = n > 40;
    if (cut) {
        n = 40;
        while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80)
            n--;
    }
    html_escape(b, title, n);
    if (cut)
        au_buf_puts(b, "&#8230;");
    au_buf_puts(b, "</b><span>");
    if (note_is_host)
        home_host(b, note);
    else
        html_escape_str(b, note);
    au_buf_puts(b, "</span></a>");
}

static bool is_web_url(const char *u)
{
    return !strncmp(u, "http://", 7) || !strncmp(u, "https://", 8);
}

static void build_home(AuPage *p, AuBuf *b)
{
    au_buf_puts(b, "<!DOCTYPE html><html><head><meta charset=utf-8><title>Aura</title><style>"
                   "body { font-family: sans-serif; margin: 0; color: #222; background: #f4f1ea; }"
                   ".page { max-width: 760px; margin: 0 auto; padding: 28px 24px 20px; }"
                   ".head { display: flex; align-items: center; margin-bottom: 18px; }"
                   ".head img { width: 72px; height: 72px; margin-right: 16px; }"
                   "h1 { font-size: 34px; color: #203c78; margin: 0; }"
                   ".head p { margin: 2px 0 0; color: #5a5a5a; font-size: 14px; }"
                   ".search { margin: 0 0 22px; }"
                   ".search input[type=search] { font-size: 15px; padding: 5px 7px; width: 70%; }"
                   ".search input[type=submit] { font-size: 14px; padding: 4px 12px; }"
                   "h2 { font-size: 15px; color: #6b5a3a; margin: 20px 0 8px; text-transform: uppercase; "
                   "letter-spacing: 1px; }"
                   ".tiles { display: flex; flex-wrap: wrap; margin: 0 -5px; }"
                   ".tile { display: block; width: 146px; min-height: 34px; margin: 5px; padding: 9px 11px; "
                   "background: #fff; "
                   "border: 1px solid #d8d0bf; border-radius: 6px; text-decoration: none; color: #222; }"
                   ".tile:hover { border-color: #203c78; background: #f8faff; }"
                   ".tile b { display: block; font-size: 14px; color: #203c78; overflow: hidden; }"
                   ".tile span { display: block; font-size: 12px; color: #6a6a6a; margin-top: 2px; overflow: hidden; }"
                   ".tip { margin-top: 22px; padding: 10px 14px; background: #fff8dc; border: 1px solid #e6d9a8; "
                   "border-radius: 6px; font-size: 13px; }"
                   ".foot { margin-top: 26px; font-size: 12px; color: #777; }"
                   ".foot a { color: #555; }"
                   "</style></head><body><div class=page><div class=head><img alt='' src='data:image/png;base64,");
    au_base64_encode(home_icon_png, sizeof home_icon_png, b);
    au_buf_puts(b, "'><div><h1>Aura</h1><p>The modern web, on your Mac.</p></div></div>");

    const AuPrefs *pr = p->env.prefs;
    home_search_form(b, pr ? pr->search_template : NULL);

    /* Most visited: web pages visited at least twice, most visits first. */
    const AuHistory *h = p->env.history ? p->env.history(p->env.ctx) : NULL;
    if (h && h->n) {
        enum { kTop = 8 };
        const AuHistoryEntry *top[kTop];
        int ntop = 0;
        for (size_t i = 0; i < h->n; i++) {
            const AuHistoryEntry *e = &h->e[i];
            if (e->visits < 2 || !is_web_url(e->url))
                continue;
            int at = ntop;
            while (at > 0 && top[at - 1]->visits < e->visits)
                at--;
            if (at >= kTop)
                continue;
            int last = ntop < kTop ? ntop : kTop - 1;
            for (int k = last; k > at; k--)
                top[k] = top[k - 1];
            top[at] = e;
            if (ntop < kTop)
                ntop++;
        }
        if (ntop) {
            au_buf_puts(b, "<h2>Most visited</h2><div class=tiles>");
            for (int i = 0; i < ntop; i++)
                home_tile(b, top[i]->url, top[i]->title && *top[i]->title ? top[i]->title : top[i]->url,
                          top[i]->url, true);
            au_buf_puts(b, "</div>");
        }
    }

    /* Bookmarks: the first links in the bookmark tree. */
    const AuBookmarks *bm = p->env.bookmarks ? p->env.bookmarks(p->env.ctx) : NULL;
    if (bm) {
        int shown = 0;
        for (const AuBookmark *k = au_bookmark_next(&bm->root, &bm->root, false); k && shown < 12;
             k = au_bookmark_next(k, &bm->root, false)) {
            if (k->folder || !k->url || !*k->url)
                continue;
            if (!shown)
                au_buf_puts(b, "<h2>Bookmarks</h2><div class=tiles>");
            home_tile(b, k->url, k->title && *k->title ? k->title : k->url, k->url, true);
            shown++;
        }
        if (shown)
            au_buf_puts(b, "</div>");
    }

    au_buf_puts(b, "<h2>Sites that work well</h2><div class=tiles>");
    for (size_t i = 0; i < sizeof home_sites / sizeof home_sites[0]; i++)
        home_tile(b, home_sites[i].url, home_sites[i].name, home_sites[i].what, false);
    au_buf_puts(b, "</div>");

    size_t ntips = sizeof home_tips / sizeof home_tips[0];
    uint32_t now = p->env.time_ms ? p->env.time_ms(p->env.ctx) : 0;
    au_buf_puts(b, "<div class=tip><b>Tip:</b> ");
    au_buf_puts(b, home_tips[(now / 1000u) % ntips]);
    au_buf_puts(b, "</div>");

    au_buf_puts(b, "<p class=foot>Aura " AU_VERSION " by " AU_AUTHOR ". "
                   "<a href='" AU_HOMEPAGE "'>Source and releases on GitHub</a> &middot; "
                   "<a href='about:licences'>Licences</a> &middot; "
                   "<a href='about:fonts'>Fonts</a> &middot; "
                   "<a href='about:scene'>Test scene</a></p>"
                   "</div></body></html>");
}

static void credit_rows(AuBuf *b, AuCreditKind kind)
{
    for (size_t i = 0; i < au_ncredits; i++) {
        const AuCredit *c = &au_credits[i];
        if (c->kind != kind)
            continue;
        au_buf_puts(b, "<h3>");
        html_escape_str(b, c->name);
        au_buf_puts(b, "</h3><p>");
        html_escape_str(b, c->what);
        au_buf_puts(b, ".</p><p class=note>");
        for (const char *s = c->copyright; *s;) {
            size_t n = strcspn(s, "\n");
            char line[256];
            snprintf(line, sizeof line, "%.*s", (int)n, s);
            html_escape_str(b, line);
            s += n;
            if (*s == '\n') {
                au_buf_puts(b, "<br>");
                s++;
            }
        }
        au_buf_puts(b, "</p><p>Licence: ");
        html_escape_str(b, c->licence);
        if (c->file) {
            au_buf_puts(b, ". The full text is in <span class=url>");
            html_escape_str(b, c->file);
            au_buf_puts(b, "</span> beside the Aura application.</p>");
        } else {
            au_buf_puts(b, ".</p>");
        }
        if (c->text) {
            au_buf_puts(b, "<p class=note>");
            for (const char *s = c->text; *s;) {
                size_t n = strcspn(s, "\n");
                AuBuf line;
                au_buf_init(&line);
                au_buf_append(&line, s, n);
                html_escape_str(b, au_buf_cstr(&line));
                au_buf_free(&line);
                s += n;
                if (*s == '\n') {
                    au_buf_puts(b, "<br>");
                    s++;
                }
            }
            au_buf_puts(b, "</p>");
        }
    }
}

static void build_licences(AuBuf *b)
{
    page_head(b, "Licences");
    au_buf_puts(b, "<h1>Licences</h1><p class=note>Aura " AU_VERSION ". " AU_TAGLINE
                   " Aura's own code is written for Aura. These parts come from others.</p>"
                   "<h2>Fonts</h2>");
    credit_rows(b, AU_CREDIT_FONT);
    au_buf_puts(b, "<h2>Libraries</h2>");
    size_t libs = 0;
    for (size_t i = 0; i < au_ncredits; i++)
        libs += au_credits[i].kind == AU_CREDIT_LIBRARY;
    if (libs)
        credit_rows(b, AU_CREDIT_LIBRARY);
    else
        au_buf_puts(b, "<p>This build contains no third-party libraries.</p>");
    au_buf_puts(b, "<h2>Data</h2>");
    credit_rows(b, AU_CREDIT_DATA);
    au_buf_puts(b, "</body></html>");
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
    if (!content_type && p->content_type && p->content_type[0])
        content_type = p->content_type;
    if (p->source_mode) {
        p->plain = true;
    } else if (content_type) {
        /* HTML is rendered; every other type shown is text. */
        p->plain = !starts_with_ci(content_type, "text/html") && !starts_with_ci(content_type, "application/xhtml");
    } else if (ends_with(u, q, ".html") || ends_with(u, q, ".htm") || ends_with(u, q, ".xhtml") ||
               ends_with(u, q, ".shtml")) {
        p->plain = false;
    } else if (ends_with(u, q, ".txt") || ends_with(u, q, ".text")) {
        p->plain = true;
    } else {
        p->plain = !looks_like_html(bytes, len);
    }
    if (p->interim)
        set_title(p, "");
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
    if (p->image_doc || p->discard)
        return; /* the <img> the page made fetches it; or nothing to show */
    if (p->prev_cert) {
        /* The new document is showing, not a download: the previous
         * one's certificate is no longer needed. */
        au_free(p->prev_cert);
        p->prev_cert = NULL;
    }
    if (!p->begun || p->interim)
        start_main_doc(p, bytes, len, NULL);
    source_append(&p->src, bytes, len);
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
    p->internal = true;
    au_buf_free(&b);
}

/* ---- Network error pages ----
 * Internal pages the engine renders in place of the document. Their links
 * to aura-action: URLs (continue past a certificate error, use plain
 * http, try again) work only on these pages (p->internal), never on a web
 * page. */

static const char *const month_names[12] = {"January", "February", "March",     "April",   "May",      "June",
                                            "July",    "August",   "September", "October", "November", "December"};

/* "14 November 2026, 10:00 UTC". */
static void put_date(AuBuf *b, int64_t t)
{
    if (!t) {
        au_buf_puts(b, "unknown");
        return;
    }
    int64_t day = au_day_of(t);
    int y, m, d, wd;
    au_date_of_day(day, &y, &m, &d, &wd);
    int64_t secs = t - day * 86400;
    au_buf_printf(b, "%d %s %d, %02d:%02d UTC", d, month_names[(m - 1) % 12], y, (int)(secs / 3600),
                  (int)(secs / 60 % 60));
}

static void put_fingerprint(AuBuf *b, const uint8_t *h)
{
    for (int i = 0; i < 32; i++) {
        au_buf_printf(b, "%02X", h[i]);
        if (i == 15)
            au_buf_puts(b, "<br>");
        else if (i < 31)
            au_buf_putc(b, ':');
    }
}

static void put_cert_entry(AuBuf *b, const AuFetchCertEntry *e)
{
    au_buf_puts(b, "<table class=cert><tr><th>Issued to</th><td>");
    html_escape_str(b, e->subject);
    au_buf_puts(b, "</td></tr><tr><th>Issued by</th><td>");
    html_escape_str(b, e->issuer);
    au_buf_puts(b, "</td></tr><tr><th>Valid from</th><td>");
    put_date(b, e->not_before);
    au_buf_puts(b, "</td></tr><tr><th>Valid until</th><td>");
    put_date(b, e->not_after);
    au_buf_puts(b, "</td></tr>");
    if (e->san[0]) {
        au_buf_puts(b, "<tr><th>Names</th><td>");
        html_escape_str(b, e->san);
        au_buf_puts(b, "</td></tr>");
    }
    au_buf_puts(b, "<tr><th>SHA-256</th><td class=url>");
    put_fingerprint(b, e->sha256);
    au_buf_puts(b, "</td></tr></table>");
}

static const char cert_css[] = "<style>table.cert { border-collapse: collapse; margin: 6px 0 12px; font-size: 13px }"
                               "table.cert th { text-align: left; padding: 2px 12px 2px 0; color: #5a5a5a;"
                               " font-weight: normal; vertical-align: top; white-space: nowrap }"
                               "table.cert td { padding: 2px 0 }"
                               ".action { margin: 18px 0 }"
                               ".action a { padding: 3px 10px; border: 1px solid #888; background: #eee;"
                               " color: #000; text-decoration: none }</style>";

static void cert_chain_html(AuBuf *b, const AuFetchCert *c)
{
    int n = c->count < AU_FETCH_CERT_CHAIN ? c->count : AU_FETCH_CERT_CHAIN;
    for (int i = 0; i < n; i++) {
        au_buf_printf(b, "<h3>%s</h3>", i == 0 ? "The site's certificate" : "Issuer's certificate");
        put_cert_entry(b, &c->chain[i]);
    }
    if (c->count > n)
        au_buf_printf(b, "<p class=note>%d more certificates are not shown.</p>", c->count - n);
}

bool au_page_certificate_html(const AuPage *p, AuBuf *b)
{
    const AuFetchCert *c = p->cert;
    if (!c)
        return false;
    page_head(b, "Certificate");
    au_buf_puts(b, cert_css);
    au_buf_puts(b, "<h1>Certificate for ");
    html_escape_str(b, c->host);
    au_buf_puts(b, "</h1>");
    if (c->overridden) {
        au_buf_puts(b, "<p><b>Not valid:</b> ");
        html_escape_str(b, c->reason);
        au_buf_puts(b, ". You chose to continue to this site anyway, for this session.</p>");
    } else if (c->error) {
        au_buf_puts(b, "<p><b>Not valid:</b> ");
        html_escape_str(b, c->reason);
        au_buf_puts(b, ".</p>");
    } else {
        au_buf_puts(b, "<p>The connection is encrypted and the certificate is valid: it was issued for this site "
                       "by an authority Aura trusts.</p>");
    }
    if (c->protocol[0]) {
        au_buf_puts(b, "<table class=cert><tr><th>Protocol</th><td>");
        html_escape_str(b, c->protocol);
        au_buf_puts(b, "</td></tr><tr><th>Cipher suite</th><td>");
        html_escape_str(b, c->cipher[0] ? c->cipher : "unknown");
        if (!strncmp(c->cipher, "RSA-", 4))
            au_buf_puts(b, " (RSA key exchange: no forward secrecy)");
        au_buf_puts(b, "</td></tr><tr><th>Checked</th><td>");
        put_date(b, c->checked_at);
        au_buf_puts(b, "</td></tr></table>");
    }
    if (c->count == 0)
        au_buf_puts(b, "<p class=note>The server sent no certificates on this connection.</p>");
    cert_chain_html(b, c);
    au_buf_puts(b, "</body></html>");
    return true;
}

static const char *status_reason(int st)
{
    switch (st) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 410: return "Gone";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return st >= 500 ? "Server Error" : "Error";
    }
}

/* Shows an internal page built in b (the head and title are b's), as the
 * failed load's document. */
static void show_internal(AuPage *p, AuBuf *b, const char *title, bool failed)
{
    p->failed = failed;
    load_html_now(p, b->data, b->len, title);
    p->internal = true;
}

static void error_head(AuPage *p, AuBuf *b, const char *title, const char *heading)
{
    page_head(b, title);
    au_buf_puts(b, cert_css);
    au_buf_puts(b, "<h1>");
    html_escape_str(b, heading);
    au_buf_puts(b, "</h1><p class=url>");
    html_escape_str(b, p->url);
    au_buf_puts(b, "</p>");
}

static const char plain_http_note[] = "On the plain HTTP site, what you send and receive can be read and changed by "
                                      "others on the network.";

/* HTTPS-first tried https for an http address, and it failed. Two cases
 * read differently: the site has no HTTPS (nothing answered securely),
 * or it has HTTPS and that failed (a certificate or TLS problem), which
 * the page says, with the certificate when there is one. */
static void show_https_fallback(AuPage *p, AuBuf *b, const AuFetchInfo *info, const char *host, const char **title)
{
    AuFetchError cause = info ? info->cause : AU_FETCH_OK;
    const AuFetchCert *c = info ? info->cert : NULL;
    const char *http_link = "<p class=action><a href='aura-action:http'>Continue to the HTTP Site</a></p>";
    if (cause == AU_FETCH_ERR_CERT || cause == AU_FETCH_ERR_CLOCK || cause == AU_FETCH_ERR_TLS) {
        *title = "Secure connection failed";
        error_head(p, b, *title, "This site's secure connection failed");
        au_buf_puts(b, "<p><b>");
        html_escape_str(b, host);
        if (cause == AU_FETCH_ERR_CLOCK) {
            au_buf_puts(b, "</b> has an HTTPS site, but Aura cannot check its certificate because the Mac's clock "
                           "says <b>");
            put_date(b, c ? c->checked_at : 0);
            au_buf_puts(b, "</b>, which is earlier than the certificates Aura knows about were issued. Set the date "
                           "and time in the Date &amp; Time control panel, then try again.</p>");
        } else if (cause == AU_FETCH_ERR_CERT) {
            au_buf_puts(b, "</b> has an HTTPS site, but its certificate is not valid: ");
            html_escape_str(b, c && c->reason[0] ? c->reason : "the certificate is not valid");
            au_buf_puts(b, ". Aura cannot confirm that the secure site is the real one.</p>");
        } else {
            au_buf_puts(b, "</b> has an HTTPS site, but the secure connection to it failed: ");
            html_escape_str(b, info && info->message && info->message[0] ? info->message : "the TLS handshake failed");
            au_buf_puts(b, ". The server may need a protocol or cipher Aura does not offer.</p>");
        }
        au_buf_printf(b, "<p>%s</p>", plain_http_note);
        au_buf_puts(b, http_link);
        if (cause == AU_FETCH_ERR_CERT && c && c->count > 0 && !c->hsts && !c->clock_wrong && p->main_fetch &&
            p->main_fetch->fetcher && p->main_fetch->fetcher->allow_cert)
            au_buf_puts(b, "<p class=action><a href='aura-action:continue'>Continue to the HTTPS Site Anyway</a></p>"
                           "<p class=note>Continuing trusts this certificate for this site until you quit Aura. "
                           "The address field then shows that the site is not secure.</p>");
        if (cause == AU_FETCH_ERR_CERT && c)
            cert_chain_html(b, c);
        return;
    }
    *title = "HTTPS not available";
    error_head(p, b, *title, "This site does not support HTTPS");
    au_buf_puts(b, "<p><b>");
    html_escape_str(b, host);
    au_buf_puts(b, cause == AU_FETCH_ERR_TIMEOUT ? "</b> did not answer on the secure (HTTPS) port in time. "
                   : cause == AU_FETCH_ERR_RESET ? "</b> closed the secure (HTTPS) connection without answering. "
                                                 : "</b> does not accept secure (HTTPS) connections. ");
    au_buf_printf(b, "%s</p>", plain_http_note);
    au_buf_puts(b, http_link);
}

static void show_net_error(AuPage *p, const AuFetchInfo *info)
{
    char host[256];
    au_url_host(p->url, host, sizeof host);
    AuBuf b;
    au_buf_init(&b);
    AuFetchError e = info ? info->error : AU_FETCH_ERR_CONNECT;
    const char *title = "Cannot load page";
    const char *retry = "<p class=action><a href='aura-action:retry'>Try Again</a></p>";
    switch (e) {
    case AU_FETCH_ERR_DNS:
        title = "Server not found";
        error_head(p, &b, title, "Aura cannot find this server");
        au_buf_puts(&b, "<p>No server named <b>");
        html_escape_str(&b, host);
        au_buf_puts(&b, "</b> was found. Check the address for typing errors. If it is right, check that the "
                        "Mac is connected to the internet (the TCP/IP control panel).</p>");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_CONNECT:
        title = "Cannot connect";
        error_head(p, &b, title, "Aura cannot connect to this server");
        au_buf_puts(&b, "<p>The server <b>");
        html_escape_str(&b, host);
        au_buf_puts(&b, "</b> refused the connection or cannot be reached. It may be down, or a firewall or "
                        "proxy may be in the way.</p>");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_TIMEOUT:
        title = "Connection timed out";
        error_head(p, &b, title, "The connection timed out");
        au_buf_puts(&b, "<p>The server <b>");
        html_escape_str(&b, host);
        au_buf_puts(&b, "</b> took too long to respond. It may be busy or unreachable.</p>");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_RESET:
        title = "Connection reset";
        error_head(p, &b, title, "The connection was reset");
        au_buf_puts(&b, "<p>The connection to the server broke while the page was loading.</p>");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_NO_NETWORK:
        title = "No network";
        error_head(p, &b, title, "No network connection");
        au_buf_puts(&b, "<p>");
        html_escape_str(&b, info && info->message && info->message[0] ? info->message
                                                                     : "Open Transport TCP/IP is not available.");
        au_buf_puts(&b, "</p><p>Check the TCP/IP control panel, then try again.</p>");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_TLS:
        title = "Secure connection failed";
        error_head(p, &b, title, "Aura cannot make a secure connection");
        au_buf_puts(&b, "<p>The secure connection to <b>");
        html_escape_str(&b, host);
        au_buf_puts(&b, "</b> failed. The server may need a protocol or cipher Aura does not offer (Aura speaks "
                        "TLS 1.3 and TLS 1.2 with AES-GCM or ChaCha20-Poly1305).</p>");
        if (p->env.prefs && !p->env.prefs->legacy_tls)
            au_buf_puts(&b, "<p>Old servers that offer no forward secrecy are refused while "
                            "<b>Allow old servers without forward secrecy</b> is off in Preferences, under "
                            "Privacy.</p>");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_CLOCK: {
        title = "Date is wrong";
        error_head(p, &b, title, "Your Mac's date looks wrong");
        au_buf_puts(&b, "<p>Aura cannot check this site's certificate because the Mac's clock says <b>");
        put_date(&b, info && info->cert ? info->cert->checked_at : 0);
        au_buf_puts(&b, "</b>, which is earlier than the certificates Aura knows about were issued.</p>"
                        "<p>Set the date and time in the Date &amp; Time control panel, then try again.</p>");
        au_buf_puts(&b, retry);
        break;
    }
    case AU_FETCH_ERR_CERT: {
        const AuFetchCert *c = info ? info->cert : NULL;
        title = "Certificate problem";
        error_head(p, &b, title, "This site's certificate is not valid");
        au_buf_puts(&b, "<p>Aura cannot confirm that this is the real <b>");
        html_escape_str(&b, host);
        au_buf_puts(&b, "</b>: ");
        html_escape_str(&b, c && c->reason[0] ? c->reason : "the certificate is not valid");
        au_buf_puts(&b, ". Someone may be trying to read or change what you send and receive.</p>");
        if (c)
            cert_chain_html(&b, c);
        if (c && c->hsts) {
            au_buf_puts(&b, "<p><b>This site must always be secure</b> (it uses HTTP Strict Transport Security), so "
                            "Aura offers no way past this error.</p>");
        } else if (c && c->count > 0 && p->main_fetch && p->main_fetch->fetcher &&
                   p->main_fetch->fetcher->allow_cert) {
            au_buf_puts(&b, "<p class=action><a href='aura-action:continue'>Continue Anyway</a></p>"
                            "<p class=note>Continuing trusts this certificate for this site until you quit Aura. "
                            "The address field then shows that the site is not secure.</p>");
        }
        break;
    }
    case AU_FETCH_ERR_HTTPS_FALLBACK:
        show_https_fallback(p, &b, info, host, &title);
        break;
    case AU_FETCH_ERR_REDIRECTS:
        title = "Too many redirects";
        error_head(p, &b, title, "This page redirects too many times");
        au_buf_puts(&b, "<p>The server answered with a redirect to another address more than 20 times in a row, "
                        "so Aura stopped following them.</p>");
        if (info && info->url && info->url[0]) {
            au_buf_puts(&b, "<p>The last address it sent Aura to was <b>");
            html_escape_str(&b, info->url);
            au_buf_puts(&b, "</b>.</p>");
        }
        au_buf_puts(&b, "<p>The usual causes are a site that needs a cookie Aura's settings block, or a site "
                        "that sends its secure address to its plain one and back. Develop &gt; Network lists "
                        "each redirect with the address it came from.</p>");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_PROTOCOL:
    case AU_FETCH_ERR_DECODE:
        title = "Bad response";
        error_head(p, &b, title, "The server sent a response Aura cannot read");
        au_buf_puts(&b, retry);
        break;
    case AU_FETCH_ERR_TOO_LARGE:
        title = "Too large";
        error_head(p, &b, title, "The response is too large");
        break;
    case AU_FETCH_ERR_PROXY:
        title = "Proxy error";
        error_head(p, &b, title, "The proxy server refused the request");
        au_buf_puts(&b, "<p>Check the proxy settings in Preferences &gt; Network.</p>");
        break;
    case AU_FETCH_ERR_BLOCKED:
        if (info && info->blocked_reason) {
            /* A blocklist names this site ("blocked: LIST"). Nothing was
             * sent to it. */
            const char *list = info->blocked_reason;
            if (!strncmp(list, "blocked: ", 9))
                list += 9;
            title = "Site blocked";
            error_head(p, &b, title, "Aura blocked this site");
            au_buf_puts(&b, "<p><b>");
            html_escape_str(&b, host);
            au_buf_puts(&b, "</b> is on a blocklist you use (");
            html_escape_str(&b, list);
            au_buf_puts(&b, "), so Aura did not connect to it. Lists name sites that serve advertising or "
                            "tracking, or that are reported as harmful.</p>"
                            "<p class=action><a href='aura-action:back'>Go Back</a></p>"
                            "<p class=action><a href='aura-action:unblock'>Visit Anyway (this session)</a></p>"
                            "<p class=note>Visiting anyway opens this site until you quit Aura. To allow it for "
                            "good, add it under Preferences &gt; Blocking.</p>");
            break;
        }
        title = "Blocked";
        error_head(p, &b, title, "Aura blocked this page");
        au_buf_puts(&b, "<p>The request was refused by Aura's security rules (for example, plain HTTP to a site that "
                        "must always be secure).</p>");
        break;
    default:
        error_head(p, &b, title, "Aura cannot load this page");
        au_buf_puts(&b, retry);
        break;
    }
    au_buf_puts(&b, "</body></html>");
    show_internal(p, &b, title, true);
    au_buf_free(&b);
}

static void show_http_error(AuPage *p)
{
    AuBuf b;
    au_buf_init(&b);
    char heading[96];
    snprintf(heading, sizeof heading, "%d %s", p->status, status_reason(p->status));
    error_head(p, &b, heading, heading);
    au_buf_puts(&b, p->status >= 500 ? "<p>The server had a problem and sent no page.</p>"
                                     : "<p>The server refused the request and sent no page.</p>");
    au_buf_puts(&b, "</body></html>");
    show_internal(p, &b, heading, true);
    au_buf_free(&b);
}

static void show_unsupported(AuPage *p)
{
    AuBuf b;
    au_buf_init(&b);
    error_head(p, &b, "Cannot show file", "Aura cannot show this kind of file");
    au_buf_puts(&b, "<p>The file is of type <b>");
    html_escape_str(&b, p->content_type && p->content_type[0] ? p->content_type : "unknown");
    au_buf_puts(&b, "</b>. To keep it, choose Save Link As in the link's contextual menu.</p></body></html>");
    show_internal(p, &b, "Cannot show file", true);
    au_buf_free(&b);
}

/* Types a document may be shown as: HTML, text and images. */
static bool displayable_type(const char *t)
{
    return !t[0] || starts_with_ci(t, "text/") || starts_with_ci(t, "image/") ||
           starts_with_ci(t, "application/xhtml+xml") || starts_with_ci(t, "application/xml") ||
           starts_with_ci(t, "application/json") || starts_with_ci(t, "application/javascript");
}

static AuFetchReferrerPolicy parse_referrer_policy(const char *v, AuFetchReferrerPolicy cur)
{
    static const char *const names[] = {"",       "no-referrer",          "no-referrer-when-downgrade",
                                        "same-origin", "origin",          "strict-origin",
                                        "origin-when-cross-origin", "strict-origin-when-cross-origin",
                                        "unsafe-url"};
    /* The last known token of a comma-separated list wins. */
    while (*v) {
        while (*v == ' ' || *v == ',' || *v == '\t')
            v++;
        size_t n = strcspn(v, ", \t");
        for (int i = 1; i < 9; i++)
            if (strlen(names[i]) == n && au_ieq(v, n, names[i], n))
                cur = (AuFetchReferrerPolicy)i;
        v += n;
    }
    return cur;
}

/* The fetch's final URL becomes the page's (after a redirect: the
 * address field and the history entry take it, once). Fetch URLs carry
 * no fragment: compare without it, and keep the fragment the user asked
 * for unless the final URL names its own, as the Fetch standard does. */
static void adopt_url(AuPage *p, const char *final)
{
    if (!final || !final[0])
        return;
    const char *hash = strchr(p->url, '#');
    size_t base = hash ? (size_t)(hash - p->url) : strlen(p->url);
    if ((strlen(final) == base && !strncmp(final, p->url, base)) || !strcmp(final, p->url))
        return;
    AuBuf nu;
    au_buf_init(&nu);
    au_buf_puts(&nu, final);
    if (hash && !strchr(final, '#'))
        au_buf_puts(&nu, hash);
    set_str(&p->url, au_buf_cstr(&nu));
    au_buf_free(&nu);
    if (p->cb.url_changed)
        p->cb.url_changed(p->cb.ctx, p, p->url);
}

static void main_response(AuPage *p, const AuFetchResponse *r)
{
    adopt_url(p, r->url);
    p->status = r->status;
    p->no_store = r->no_store;
    p->stats.document_cached = r->from_cache;
    set_str(&p->content_type, r->content_type);
    p->security = r->security;
    p->have_security = true;
    p->mixed = false;
    au_free(p->cert);
    p->cert = NULL;
    if (r->cert) {
        p->cert = au_alloc(sizeof *p->cert);
        *p->cert = *r->cert;
    }
    /* A document that came over RSA key exchange (an old server with no
     * forward secrecy) gets the "not fully secure" lock mixed content gets. */
    p->legacy_kex = r->cert && !strncmp(r->cert->cipher, "RSA-", 4);
    if (r->referrer_policy && r->referrer_policy[0])
        p->rpolicy = parse_referrer_policy(r->referrer_policy, p->rpolicy);
    if (!displayable_type(r->content_type) && !p->source_mode) {
        p->discard = true; /* main_data drops it; main_done says so */
        return;
    }
    if (starts_with_ci(r->content_type, "image/") && !p->source_mode) {
        AuBuf b, name;
        au_buf_init(&b);
        au_buf_init(&name);
        url_file_name(p->url, &name);
        au_buf_puts(&b, "<!DOCTYPE html><html><head><title>");
        html_escape(&b, name.data ? name.data : "", name.len);
        au_buf_puts(&b, "</title></head><body style='margin:0'><img style='display:block' src=\"");
        html_escape_str(&b, p->url);
        au_buf_puts(&b, "\">");
        begin_doc(p, p->url);
        au_docview_feed(p->dv, b.data, b.len);
        p->image_doc = true;
        au_buf_free(&b);
        au_buf_free(&name);
    }
}

static bool main_download(AuPage *p, const AuFetchResponse *r, uint32_t handoff)
{
    if (!p->cb.download || p->source_mode)
        return false;
    char *url = au_strndup(p->url, strlen(p->url));
    /* Back to what was shown before, then hand over (the browser drops
     * the history entry this load made). */
    set_str(&p->url, p->prev_url ? p->prev_url : "about:blank");
    set_title(p, p->prev_title ? p->prev_title : "");
    if (!p->begun)
        p->begun = p->prev_begun;
    /* The document still shown keeps its padlock and certificate. */
    p->security = p->prev_security;
    p->have_security = p->prev_have_security;
    p->mixed = p->prev_mixed;
    p->legacy_kex = p->prev_legacy_kex;
    au_free(p->cert);
    p->cert = p->prev_cert;
    p->prev_cert = NULL;
    p->main_done = true;
    /* The document shown stays: it is not kept aside after all, and gets
     * its entry and its source back. */
    p->doc_entry = p->prev_entry;
    if (p->main_fetch)
        au_netlog_end(&p->netlog, p->main_fetch->log, AU_NETREC_DONE, NULL, log_ms(p));
    if (p->leaving) {
        source_move(&p->src, &p->leaving->src);
        netlog_take(p, &p->leaving->netlog); /* the download's own record goes with its log */
        p->no_store = false;
        kept_free(p->leaving);
        p->leaving = NULL;
    }
    if (p->main_fetch) {
        p->main_fetch->done = true; /* no more calls from the fetcher */
        p->main_fetch = NULL;
    }
    bool took = p->cb.download(p->cb.ctx, p, r, handoff);
    au_free(url);
    if (p->cb.url_changed)
        p->cb.url_changed(p->cb.ctx, p, p->url);
    finish(p, true);
    return took;
}

static void main_done(AuPage *p, bool ok, const AuFetchInfo *info)
{
    /* The final URL, except after HTTPS-first's failed upgrade, where the
     * address stays the one the user asked for. */
    if (info && info->url && info->error != AU_FETCH_ERR_HTTPS_FALLBACK)
        adopt_url(p, info->url);
    if (info && info->cert && !p->cert) {
        p->cert = au_alloc(sizeof *p->cert);
        *p->cert = *info->cert;
        p->security = AU_SECURITY_CERT_ERROR;
        p->have_security = true;
    }
    bool is_file = au_url_scheme_is(p->url, "file");
    if (!ok && (!p->main_fetch || p->main_fetch->off == 0)) {
        p->main_done = true;
        if (is_file || !info)
            show_error(p, is_file ? "File not found" : "Cannot load page",
                       is_file ? "Cannot open this file" : "Cannot load this page");
        else
            show_net_error(p, info);
        return;
    }
    if (p->discard) {
        p->main_done = true;
        show_unsupported(p);
        return;
    }
    if (ok && p->status >= 400 && p->main_fetch && p->main_fetch->off == 0) {
        p->main_done = true;
        show_http_error(p);
        return;
    }
    if (!p->begun || p->interim)
        start_main_doc(p, "", 0, info ? info->content_type : NULL);
    if (p->plain)
        au_docview_feed(p->dv, "</pre></body></html>", 20);
    au_docview_finish(p->dv);
    if (!ok)
        p->failed = true;
    p->main_done = true;
    p->dirty = true;
    /* The source is complete: give back the room its buffer grew beyond
     * its length (up to as much again). */
    if (p->src.plain.cap > p->src.plain.len + 1 && p->src.plain.len) {
        p->src.plain.data = au_realloc(p->src.plain.data, p->src.plain.len + 1);
        p->src.plain.cap = p->src.plain.len + 1;
    }
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
        /* The document now belongs to the new entry too. */
        if (p->next_entry) {
            kept_drop(p, p->next_entry);
            p->doc_entry = p->next_entry;
        }
        p->next_entry = 0;
        return;
    }
    /* Whether the document shown may be kept, judged before its fetches
     * are cancelled. */
    compact_fetches(p);
    p->discarded = false; /* a discarded page loads like one that never held a document */
    bool keep = !p->leaving && p->next_entry != p->doc_entry && can_keep(p);
    p->no_keep = false;
    cancel_all(p);
    compact_fetches(p);
    au_page_clear_selection(p);
    p->gen++;
    if (entry && !force && show_kept(p, url, rx, ry, keep))
        return;
    /* While an earlier load has not begun, the document shown is still
     * the one that load set aside. */
    if (!p->leaving && keep)
        p->leaving = kept_capture(p);
    kept_drop(p, p->next_entry); /* the entry is loaded afresh */
    if (p->begun)
        p->prev_entry = p->doc_entry; /* the entry of the document on screen */
    p->doc_entry = p->next_entry;
    p->next_entry = 0;
    p->no_store = false;
    char *u = au_strndup(url, strlen(url)); /* url may be p->url */
    /* What is shown now, in case this load turns out to be a download. */
    au_free(p->prev_url);
    au_free(p->prev_title);
    p->prev_url = p->url;
    p->prev_title = au_strndup(p->title, strlen(p->title));
    p->prev_begun = p->begun;
    p->prev_security = p->security;
    p->prev_have_security = p->have_security;
    p->prev_mixed = p->mixed;
    p->prev_legacy_kex = p->legacy_kex;
    au_free(p->prev_cert);
    p->prev_cert = p->cert;
    p->cert = NULL;
    source_free(&p->src);
    p->url = u;
    /* The referrer: this page's own address when one of its links asked
     * for the navigation; none when the user typed or chose it. */
    au_free(p->referrer);
    p->referrer = NULL;
    if (p->link_target && !strcmp(p->link_target, u) && p->link_referrer)
        p->referrer = au_strndup(p->link_referrer, strlen(p->link_referrer));
    au_free(p->link_target);
    au_free(p->link_referrer);
    p->link_target = p->link_referrer = NULL;
    p->rpolicy = AU_FETCH_RP_DEFAULT;
    p->have_security = p->mixed = false;
    p->status = 0;
    set_str(&p->content_type, "");
    p->image_doc = p->discard = false;
    p->last_status[0] = 0;
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
    au_netlog_clear(&p->netlog);
    p->console_current = 0; /* the console keeps the earlier lines; the Console window marks where this page's begin */
    p->oom_due = p->oom_told = false;
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
            hide_leaving(p);
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
            build_home(p, &b);
            load_html_now(p, b.data, b.len, NULL);
        } else if (wn == 5 && !memcmp(what, "fonts", 5)) {
            build_fonts(p, &b);
            load_html_now(p, b.data, b.len, NULL);
        } else if (wn == 8 && (!memcmp(what, "licences", 8) || !memcmp(what, "licenses", 8))) {
            build_licences(&b);
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
    if (p->preset_source) {
        /* View Source of a page whose bytes are at hand. */
        AuBuf src = *p->preset_source;
        au_free(p->preset_source);
        p->preset_source = NULL;
        p->main_done = false;
        start_main_doc(p, src.data ? src.data : "", src.len, NULL);
        source_append(&p->src, src.data ? src.data : "", src.len);
        feed_plain(p, src.data ? src.data : "", src.len);
        au_buf_free(&src);
        au_docview_feed(p->dv, "</pre></body></html>", 20);
        au_docview_finish(p->dv);
        p->main_done = true;
        check_finished(p);
        return;
    }
    if (au_url_scheme_is(target, "file") || fetcher_for(p, target)) {
        p->main_fetch = start_fetch(p, target, true, AU_FETCH_DOCUMENT, NULL);
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
    p->cache_mode = AU_FETCH_CACHE_NORMAL;
    if (p->post) {
        clear_post(p);
        start_load(p, url, false, 0, 0, true); /* a POST result is no base for a fragment */
        return;
    }
    start_load(p, url, false, 0, 0, false);
}

void au_page_navigate_entry(AuPage *p, const char *url, int scroll_x, int scroll_y)
{
    p->cache_mode = AU_FETCH_CACHE_HISTORY;
    clear_post(p);
    start_load(p, url, true, scroll_x, scroll_y, false);
}

void au_page_reload(AuPage *p)
{
    p->next_entry = p->doc_entry; /* the same entry: its document is replaced, not kept */
    p->cache_mode = AU_FETCH_CACHE_REVALIDATE;
    start_load(p, p->url, true, p->scroll_x, p->scroll_y, true);
}

void au_page_reload_bypass(AuPage *p)
{
    p->next_entry = p->doc_entry;
    p->cache_mode = AU_FETCH_CACHE_BYPASS;
    start_load(p, p->url, true, p->scroll_x, p->scroll_y, true);
}

void au_page_navigate_source(AuPage *p, const char *url, const char *data, size_t len)
{
    au_free(p->preset_source);
    p->preset_source = au_alloc(sizeof *p->preset_source);
    au_buf_init(p->preset_source);
    au_buf_append(p->preset_source, data, len);
    p->cache_mode = AU_FETCH_CACHE_NORMAL;
    clear_post(p);
    start_load(p, url, false, 0, 0, true);
}

void au_page_load_internal(AuPage *p, const char *url, const char *html, size_t len)
{
    p->cache_mode = AU_FETCH_CACHE_NORMAL;
    clear_post(p);
    start_load(p, "about:blank", false, 0, 0, true);
    set_str(&p->url, url);
    if (p->cb.url_changed)
        p->cb.url_changed(p->cb.ctx, p, p->url);
    p->loading = true;
    load_html_now(p, html, len, NULL);
    p->internal = true;
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
    au_netlog_init(&p->netlog);
    new_view(p);
    return p;
}

void au_page_free(AuPage *p)
{
    if (!p)
        return;
    cancel_all(p);
    compact_fetches(p);
    au_page_clear_selection(p);
    source_free(&p->src);
    if (p->leaving)
        kept_free(p->leaving);
    p->leaving = NULL;
    au_page_drop_kept(p);
    au_free(p->kept);
    au_docview_free(p->dv);
    /* Freeing the documents can start a fetch (YouTube's page left one,
     * Fetch and URL, behind each time it closed): cancel and free those
     * too. */
    cancel_all(p);
    compact_fetches(p);
    au_free(p->fetches);
    au_free(p->url);
    au_free(p->title);
    au_free(p->fragment);
    au_free(p->open_url);
    au_free(p->sub_url);
    au_free(p->sub_type);
    au_buf_free(&p->sub_body);
    clear_post(p);
    au_free(p->link_referrer);
    au_free(p->link_target);
    au_free(p->referrer);
    au_free(p->cert);
    au_free(p->content_type);
    au_free(p->prev_url);
    au_free(p->prev_title);
    au_free(p->prev_cert);
    for (size_t i = 0; i < p->console_count; i++)
        au_free(p->console[(p->console_start + i) % AU_PAGE_CONSOLE_LINES]);
    au_netlog_free(&p->netlog);
    au_free(p);
}

/* ---- Developer tools ---- */

const AuNetLog *au_page_netlog(const AuPage *p)
{
    return &p->netlog;
}

AuPerf *au_page_perf(AuPage *p)
{
    return au_docview_perf(p->dv);
}

void au_page_script_memory(const AuPage *p, size_t *used, size_t *limit)
{
    au_docview_script_memory(p->dv, used, limit);
}

void au_page_script_summary(const AuPage *p, AuBuf *out)
{
    au_docview_script_summary(p->dv, out);
}

void au_page_script_peaks(const AuPage *p, size_t *page_peak, size_t *worker_peak)
{
    au_docview_script_peaks(p->dv, page_peak, worker_peak);
}

uint32_t au_page_console_total(const AuPage *p)
{
    return p->console_total;
}

void au_page_console_add(AuPage *p, int level, const char *text)
{
    dv_console(p, p->dv, level, text, strlen(text));
}

void au_page_console_clear(AuPage *p)
{
    for (size_t i = 0; i < p->console_count; i++)
        au_free(p->console[(p->console_start + i) % AU_PAGE_CONSOLE_LINES]);
    p->console_start = p->console_count = 0;
    p->console_current = 0;
    p->console_total++;
}

bool au_page_eval(AuPage *p, const char *src, AuBuf *out)
{
    if (p->kind != K_DOC || !p->begun) {
        au_buf_puts(out, "This page has no document to run script in");
        return false;
    }
    if (!au_docview_scripting(p->dv)) {
        au_buf_puts(out, "JavaScript is turned off (Preferences), or was when this page loaded");
        return false;
    }
    uint32_t gen = p->gen;
    bool ok = au_docview_eval(p->dv, src, out);
    if (gen == p->gen)
        follow_open_url(p); /* the expression may have navigated */
    return ok;
}

bool au_page_update_stats(const AuPage *p, struct AuDocViewStats *s)
{
    if (!p->dv)
        return false;
    au_docview_stats(p->dv, s);
    return true;
}

size_t au_page_console_count(const AuPage *p)
{
    return p->console_count;
}

size_t au_page_console_current(const AuPage *p)
{
    return p->console_current < p->console_count ? p->console_current : p->console_count;
}

const char *au_page_console_line(const AuPage *p, size_t i, int *level)
{
    if (i >= p->console_count)
        return NULL;
    size_t k = (p->console_start + i) % AU_PAGE_CONSOLE_LINES;
    if (level)
        *level = p->console_level[k];
    return p->console[k];
}

bool au_page_has_source(const AuPage *p)
{
    return p->kind == K_DOC && p->main_done && !p->failed && source_len(&p->src) > 0;
}

bool au_page_source(const AuPage *p, const char **data, size_t *len)
{
    if (p->kind != K_DOC || !p->main_done || p->failed || !source_len(&p->src))
        return false;
    /* The bytes may be held compressed: the plain copy made here is the
     * page's own cache, dropped again by au_page_idle. */
    AuPage *m = (AuPage *)p;
    if (!source_open(&m->src, now(m)))
        return false;
    *data = p->src.plain.data;
    *len = p->src.plain.len;
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
    if (p->have_security) {
        if (p->security == AU_SECURITY_SECURE && (p->mixed || p->legacy_kex))
            return AU_SECURITY_MIXED;
        return p->security;
    }
    /* Before the response: http is never secure; https says nothing yet. */
    if (au_url_scheme_is(u, "http"))
        return AU_SECURITY_NOT_SECURE;
    return AU_SECURITY_NONE;
}

const AuFetchCert *au_page_cert(const AuPage *p)
{
    return p->cert;
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
    kept_trim(p);
    apply_font_prefs(p);
    if (p->kind == K_DOC && p->begun) {
        run_layout(p);
        invalidate_all(p);
    }
}

void au_page_set_dark(AuPage *p, bool dark)
{
    if (au_docview_dark(p->dv) == dark)
        return;
    au_docview_set_dark(p->dv, dark);
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

/* A link on an internal page: continue past a certificate error (for
 * this host and certificate, this session), accept plain http after
 * HTTPS-first failed, or try again. */
static void internal_action(AuPage *p, const char *what)
{
    if (!strcmp(what, "continue")) {
        const AuPageFetcher *fe = fetcher_for(p, p->url);
        if (!p->cert || p->cert->hsts || p->cert->clock_wrong || p->cert->count < 1 || !fe || !fe->allow_cert ||
            !fe->allow_cert(fe->ctx, p->cert->host, p->cert->chain[0].sha256))
            return;
        au_page_reload(p);
    } else if (!strcmp(what, "http")) {
        p->allow_http = true;
        au_page_reload(p);
    } else if (!strcmp(what, "retry")) {
        au_page_reload(p);
    } else if (!strcmp(what, "unblock")) {
        const AuPageFetcher *fe = fetcher_for(p, p->url);
        if (!fe || !fe->allow_blocked)
            return;
        fe->allow_blocked(fe->ctx, p->url);
        au_page_reload(p);
    } else if (!strcmp(what, "back")) {
        if (p->cb.go_back)
            p->cb.go_back(p->cb.ctx, p);
    }
}

/* Updates the layout for what script turns changed and repaints only what
 * moved or changed (au_docview_settle). Runs outside fetch polling, where
 * an invalidation would mark the whole page dirty. */
static void settle(AuPage *p)
{
    if (p->kind == K_DOC && p->begun && !p->in_idle)
        au_docview_settle(p->dv);
}

bool au_page_mouse(AuPage *p, const AuEvent *ev)
{
    if (p->kind != K_DOC || !p->begun)
        return false;
    if (ev->type == AU_EVENT_MOUSE_DOWN) {
        int vx = ev->x, vy = ev->y;
        AuDocView *v = au_docview_view_at(p->dv, &vx, &vy);
        p->ctl_press = au_docview_control_at(v, vx, vy);
        if (p->ctl_press)
            au_page_clear_selection(p);
    }
    bool gesture = !p->ctl_press && sel_mouse(p, ev);
    if (ev->type == AU_EVENT_MOUSE_UP)
        p->ctl_press = false;
    AuEvent e = *ev;
    if (gesture && ev->type == AU_EVENT_MOUSE_UP)
        e.x = e.y = -100000; /* end the press without activating a link */
    bool used = au_docview_mouse(p->dv, &e) || gesture;
    if (follow_open_url(p))
        return true;
    settle(p);
    return used;
}

static void put_resolved(AuDocView *dv, const AuElement *el, const char *name, AuBuf *out)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    const char *v = a ? au_element_get_attr(el, a) : NULL;
    char *abs = v && *v ? au_url_resolve_dup(au_docview_base_url(dv), v) : NULL;
    if (abs)
        au_buf_puts(out, abs);
    au_free(abs);
}

void au_page_hit_info(AuPage *p, int x, int y, AuBuf *link, AuBuf *image)
{
    if (p->kind != K_DOC || !p->begun)
        return;
    AuHitResult r;
    /* in the nested document under the point, with its base URL */
    AuDocView *v = au_docview_view_at(p->dv, &x, &y);
    if (!au_docview_hit_test(v, x, y, &r))
        return;
    if (link && r.link)
        put_resolved(v, r.link, "href", link);
    if (image && r.element && r.element->tag == AU_ATOM_img)
        put_resolved(v, r.element, "src", image);
}

/* Keys go to the focused element (fields, controls, Tab between them, and
 * script listeners); what the page does not use (scrolling keys outside a
 * field) is the browser's. */
bool au_page_key(AuPage *p, const AuEvent *ev)
{
    if (p->kind != K_DOC || !p->begun)
        return false;
    bool used = au_docview_key(p->dv, ev);
    if (follow_open_url(p))
        return true;
    settle(p);
    return used;
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
    if (f->mime && !f->main && !f->off) {
        const char *hdrs[2] = {"content-type", f->mime};
        au_docview_resource_head(p->dv, f->id, 200, "", NULL, hdrs, 1);
        if (f->done || f->cancelled)
            return true;
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

/* The main document's fetch status, while it has not begun. */
static bool main_status(const AuPage *p, AuFetchStatus *st)
{
    const Fetch *f = p->main_fetch;
    if (!p->loading || !f || f->done || f->cancelled || !f->fetcher || !f->fetcher->status)
        return false;
    return f->fetcher->status(f->fetcher->ctx, f->fid, st);
}

void au_page_status_text(const AuPage *p, char *out, size_t cap)
{
    out[0] = 0;
    if (!p->loading || !cap)
        return;
    char host[256];
    au_url_host(p->url, host, sizeof host);
    AuFetchStatus st;
    if ((!p->begun || p->interim) && main_status(p, &st)) {
        switch (st.phase) {
        case AU_FETCH_PHASE_ENTROPY:
            snprintf(out, cap, "Gathering randomness for a secure connection (move the mouse to help)");
            return;
        case AU_FETCH_PHASE_RESOLVING: snprintf(out, cap, "Looking up %s", host); return;
        case AU_FETCH_PHASE_CONNECTING: snprintf(out, cap, "Connecting to %s", host); return;
        case AU_FETCH_PHASE_SECURING: snprintf(out, cap, "Making a secure connection to %s", host); return;
        case AU_FETCH_PHASE_SENDING:
        case AU_FETCH_PHASE_WAITING: snprintf(out, cap, "Waiting for %s", host); return;
        case AU_FETCH_PHASE_RECEIVING: snprintf(out, cap, "Receiving the page from %s", host); return;
        default: break;
        }
    }
    if (p->requested > 0 && p->completed < p->requested) {
        snprintf(out, cap, "Loading resources: %u of %u done", (unsigned)p->completed, (unsigned)p->requested);
        return;
    }
    snprintf(out, cap, "Loading %s", host[0] ? host : p->url);
}

/* While https waits for the random source, the page says so, with the
 * pool's progress (refreshed twice a second). */
static void update_interim(AuPage *p)
{
    AuFetchStatus st;
    if ((p->begun && !p->interim) || !main_status(p, &st) || st.phase != AU_FETCH_PHASE_ENTROPY ||
        !st.entropy_needed)
        return;
    uint32_t t = now(p);
    if (p->interim && t - p->interim_ms < 500 && st.entropy_bits == p->interim_bits)
        return;
    if (!p->interim && t - p->load_start < 300)
        return; /* a short wait needs no page */
    unsigned pct = st.entropy_bits >= st.entropy_needed ? 100 : st.entropy_bits * 100 / st.entropy_needed;
    AuBuf b;
    au_buf_init(&b);
    page_head(&b, "Preparing a secure connection");
    au_buf_puts(&b, "<h1>Preparing a secure connection</h1><p class=url>");
    html_escape_str(&b, p->url);
    au_buf_printf(&b,
                  "</p><p>Aura is gathering randomness for the encryption keys of secure connections. This "
                  "happens once, the first time Aura runs on this Mac; later runs start from a saved seed.</p>"
                  "<p><b>Moving the mouse or typing makes it faster.</b></p>"
                  "<div style='width:300px;height:14px;border:1px solid #666;background:#fff'>"
                  "<div style='width:%upx;height:14px;background:#203c78'></div></div>"
                  "<p class=note>%u%% (%u of %u bits)</p></body></html>",
                  pct * 3, pct, st.entropy_bits < st.entropy_needed ? st.entropy_bits : st.entropy_needed,
                  st.entropy_needed);
    begin_doc(p, p->url);
    au_docview_feed(p->dv, b.data, b.len);
    au_docview_finish(p->dv);
    p->interim = true;
    p->internal = true;
    p->interim_ms = t;
    p->interim_bits = st.entropy_bits;
    p->dirty = true;
    set_title(p, "Preparing a secure connection");
    au_buf_free(&b);
}

static void report_status(AuPage *p)
{
    char s[sizeof p->last_status];
    au_page_status_text(p, s, sizeof s);
    if (strcmp(s, p->last_status)) {
        snprintf(p->last_status, sizeof p->last_status, "%s", s);
        if (p->cb.load_progress)
            p->cb.load_progress(p->cb.ctx, p);
    }
}

void au_page_idle(AuPage *p)
{
    /* The page source goes cold: compressed once the load is over, and
     * its inflated copy dropped a while after the last reader. */
    if (p->src.plain.len && !p->loading && p->main_done) {
        if (p->src.z.len) {
            if (now(p) - p->src.used >= SOURCE_OPEN_MS)
                au_buf_free(&p->src.plain);
        } else if (!p->src.tried && now(p) - p->src.used >= SOURCE_OPEN_MS / 10) {
            source_pack(&p->src, p);
            p->src.tried = true;
        }
    }
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
    if (p->kind == K_DOC && p->begun) {
        au_docview_animate(p->dv, now(p)); /* animated images */
        uint32_t t = now(p);
        if (au_docview_editing(p->dv) && (int32_t)(t - p->blink_due) >= 0)
            p->blink_due = t + au_docview_blink(key_view(p), t); /* the caret */
    }
    if (p->kind == K_DOC && p->begun) {
        uint32_t gen0 = p->gen;
        au_docview_run_scripts(p->dv, SCRIPT_BUDGET_MS); /* timers, tasks, frames */
        if (follow_open_url(p) || gen0 != p->gen)
            return; /* script navigated */
        settle(p);
    }
    /* A load whose fetches are all in may still wait for a frame's
     * document (a srcdoc frame, a frame's load event): check_finished
     * below ends it. */
    if (!p->nfetches && !p->dirty && !(p->loading && p->main_done && p->begun))
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
    if (follow_open_url(p))
        return; /* a script navigated while the document loaded */
    update_interim(p);
    maybe_layout(p, false);
    settle(p); /* script that ran in fetch callbacks */
    if (!p->netlog.dcl_ms && p->main_done && p->begun && !p->internal && au_docview_content_loaded(p->dv))
        au_netlog_mark(&p->netlog, log_ms(p) ? log_ms(p) : 1, 0);
    check_finished(p);
    report_status(p);
}

bool au_page_busy(const AuPage *p)
{
    return p->loading || p->nfetches > 0 || (p->dirty && p->begun) || (p->begun && au_docview_animating(p->dv)) ||
           (p->begun && au_docview_scripts_due(p->dv) < 300) || (p->begun && au_docview_settle_pending(p->dv));
}

void au_page_stats(const AuPage *p, AuPageStats *s)
{
    *s = p->stats;
    AuLayoutStats ls;
    au_layout_stats(au_docview_layout(p->dv), &ls);
    s->boxes = ls.boxes;
    AuImageCacheStats is;
    au_docview_image_stats(p->dv, &is);
    s->image_decodes = is.decodes;
    s->image_evictions = is.evictions;
    s->image_kb = (uint32_t)(is.decoded_bytes / 1024);
    s->image_budget_kb = (uint32_t)(is.budget / 1024);
    s->source_bytes = (uint32_t)source_len(&p->src);
    s->source_held = (uint32_t)source_held(&p->src);
    s->source_pack_ms = p->src.pack_ms;
}

/* ---- Scrolling by moving pixels ---- */

int au_page_scroll_damage(AuPage *p, int ox, int oy, int nx, int ny, AuIRect *out, int max)
{
    if (p->kind != K_DOC || p->dirty || !au_docview_document(p->dv))
        return -1;
    return au_docview_scroll_damage(p->dv, ox, oy, nx, ny, out, max);
}

/* ---- The site icon ---- */

/* How well a link's sizes attribute suits a 16 pixel icon: the smallest
 * size of at least 16 is best, then "any" or no attribute, then the
 * largest below 16. */
static int icon_sizes_score(const char *sizes)
{
    if (!sizes || !*sizes)
        return 500;
    int best = 0;
    const char *s = sizes;
    while (*s) {
        while (au_is_space((unsigned char)*s))
            s++;
        if (!*s)
            break;
        int w = 0, sc;
        const char *t = s;
        while (au_is_digit((unsigned char)*t) && w < 100000)
            w = w * 10 + (*t++ - '0');
        if (t == s)
            sc = 500; /* "any" */
        else if (w >= 16)
            sc = 2000 - (w > 1000 ? 1000 : w);
        else
            sc = w;
        if (sc > best)
            best = sc;
        while (*s && !au_is_space((unsigned char)*s))
            s++;
    }
    return best;
}

static bool rel_has_icon(const char *rel)
{
    while (*rel) {
        while (au_is_space((unsigned char)*rel))
            rel++;
        const char *e = rel;
        while (*e && !au_is_space((unsigned char)*e))
            e++;
        if (au_ieq(rel, (size_t)(e - rel), "icon", 4))
            return true;
        rel = e;
    }
    return false;
}

char *au_page_icon_url(AuPage *p)
{
    if (p->kind != K_DOC || !p->begun || p->failed || p->internal || !p->url || !au_docview_document(p->dv))
        return NULL;
    if (!au_url_scheme_is(p->url, "http") && !au_url_scheme_is(p->url, "https"))
        return NULL;
    AuDocument *d = au_docview_document(p->dv);
    AuAtom a_rel = au_atom_cstr("rel"), a_href = au_atom_cstr("href"), a_sizes = au_atom_cstr("sizes");
    const char *best = NULL;
    int best_score = -1;
    for (AuNode *c = au_node_next(&d->node, &d->node); c; c = au_node_next(c, &d->node)) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        AuElement *el = au_as_element(c);
        if (el->tag == AU_ATOM_body)
            break; /* icons are named in the head */
        if (el->tag != AU_ATOM_link)
            continue;
        const char *rel = au_element_get_attr(el, a_rel), *href = au_element_get_attr(el, a_href);
        if (!rel || !href || !*href || !rel_has_icon(rel))
            continue;
        int sc = icon_sizes_score(au_element_get_attr(el, a_sizes));
        if (sc > best_score) {
            best_score = sc;
            best = href;
        }
    }
    char *abs = best ? au_url_resolve_dup(au_docview_base_url(p->dv), best) : NULL;
    if (abs && (au_url_scheme_is(abs, "http") || au_url_scheme_is(abs, "https")))
        return abs;
    au_free(abs);
    return au_url_resolve_dup(p->url, "/favicon.ico");
}
