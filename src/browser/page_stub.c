/* A stand-in for the page engine, implementing page.h until the real one
 * (CSS, layout, networking) arrives.
 *
 *   about:blank   an empty white page
 *   about:home    the start page, with links to the others
 *   about:scene   the paint test scene (scene.c)
 *   about:fonts   the registered font faces
 *   file:...      a local file: HTML is parsed with au_html_parse and its
 *                 <title> and text shown wrapped to the width; other files
 *                 are shown as plain text
 *   anything else a placeholder showing the URL, after a simulated load
 *                 of a little over a second, so the reload, stop and
 *                 progress states can be seen
 *
 * Text is laid out as blocks of words wrapped to the viewport width. */
#include "page.h"
#include "scene.h"
#include "url.h"

#include "../html/html.h"

#include <stdio.h>
#include <string.h>

#define SIM_LOAD_MS 1400

typedef enum { K_BLANK, K_HOME, K_SCENE, K_FONTS, K_TEXT, K_REMOTE } Kind;
typedef enum { F_SANS, F_SANS_BOLD, F_SERIF, F_MONO } Face;

typedef struct Block {
    char *text;
    Face face;
    float size;
    AuColor color;
    int link; /* index into links, or -1 */
    float space_after;
} Block;

typedef struct Run {
    size_t block;
    size_t off, len; /* within the block's text */
    float x, y;      /* baseline start, document coordinates */
    float w;
} Run;

typedef struct Link {
    char *url;
    AuIRect box; /* document coordinates, set by layout */
} Link;

struct AuPage {
    AuPageEnv env;
    AuPageCallbacks cb;
    char *url, *title;
    Kind kind;
    bool loading, failed;
    uint32_t load_start;
    float progress;
    int vw, vh;
    int content_w, content_h;
    Block *blocks;
    size_t nblocks;
    Run *runs;
    size_t nruns, runs_cap;
    Link *links;
    size_t nlinks;
    int hover, pressed;
};

/* ---- Helpers ---- */

static void set_str(char **f, const char *s)
{
    char *v = au_strndup(s, strlen(s));
    au_free(*f);
    *f = v;
}

static void clear_content(AuPage *p)
{
    for (size_t i = 0; i < p->nblocks; i++)
        au_free(p->blocks[i].text);
    au_free(p->blocks);
    p->blocks = NULL;
    p->nblocks = 0;
    for (size_t i = 0; i < p->nlinks; i++)
        au_free(p->links[i].url);
    au_free(p->links);
    p->links = NULL;
    p->nlinks = 0;
    p->nruns = 0;
    p->hover = p->pressed = -1;
}

static void add_block(AuPage *p, const char *text, size_t len, Face face, float size, AuColor color, float after,
                      const char *link)
{
    p->blocks = au_realloc(p->blocks, (p->nblocks + 1) * sizeof *p->blocks);
    Block *b = &p->blocks[p->nblocks++];
    b->text = au_strndup(text, len);
    b->face = face;
    b->size = size;
    b->color = color;
    b->space_after = after;
    b->link = -1;
    if (link) {
        p->links = au_realloc(p->links, (p->nlinks + 1) * sizeof *p->links);
        p->links[p->nlinks].url = au_strndup(link, strlen(link));
        memset(&p->links[p->nlinks].box, 0, sizeof(AuIRect));
        b->link = (int)p->nlinks++;
    }
}

static void para(AuPage *p, const char *s, Face f, float size, AuColor c, float after)
{
    add_block(p, s, strlen(s), f, size, c, after, NULL);
}

static AuFontFace *face_for(AuPage *p, Face f)
{
    const AuPrefs *pr = p->env.prefs;
    char fam[160];
    switch (f) {
    case F_SERIF: snprintf(fam, sizeof fam, "'%s', serif", pr ? pr->font_serif : "Times"); break;
    case F_MONO: snprintf(fam, sizeof fam, "'%s', monospace", pr ? pr->font_mono : "Courier"); break;
    default: snprintf(fam, sizeof fam, "'%s', sans-serif", pr ? pr->font_sans : "Helvetica"); break;
    }
    return au_fontset_match(p->env.fonts, fam, f == F_SANS_BOLD ? 700 : 400, false);
}

static uint32_t now(AuPage *p)
{
    return p->env.time_ms ? p->env.time_ms(p->env.ctx) : 0;
}

/* ---- Layout ---- */

static void add_run(AuPage *p, size_t block, size_t off, size_t len, float x, float y, float w)
{
    if (p->nruns == p->runs_cap) {
        p->runs_cap = p->runs_cap ? p->runs_cap * 2 : 64;
        p->runs = au_realloc(p->runs, p->runs_cap * sizeof *p->runs);
    }
    Run *r = &p->runs[p->nruns++];
    r->block = block;
    r->off = off;
    r->len = len;
    r->x = x;
    r->y = y;
    r->w = w;
}

static void layout(AuPage *p)
{
    p->nruns = 0;
    float margin = 16, maxw = (float)p->vw - 2 * margin;
    if (maxw < 80)
        maxw = 80;
    float y = margin;
    float widest = 0;
    for (size_t bi = 0; bi < p->nblocks; bi++) {
        Block *b = &p->blocks[bi];
        AuFontFace *f = face_for(p, b->face);
        float lh = b->size * 1.3f;
        float ascent = b->size;
        if (f) {
            AuFontMetrics m;
            au_font_metrics(f, b->size, &m);
            ascent = m.ascent;
            lh = m.ascent + m.descent + m.line_gap;
            if (lh < b->size * 1.15f)
                lh = b->size * 1.15f;
        }
        float space = f ? au_text_width(f, b->size, " ", 1) : b->size / 3;
        const char *t = b->text;
        size_t n = strlen(t), i = 0;
        float x = 0;
        size_t line_start = 0, line_end = 0;
        float line_w = 0;
        bool any = false;
        /* Greedy wrap: words separated by spaces; a newline forces a break. */
        while (i <= n) {
            size_t ws = i;
            while (i < n && t[i] != ' ' && t[i] != '\n')
                i++;
            size_t we = i;
            float w = f ? au_text_width(f, b->size, t + ws, we - ws) : (float)(we - ws) * b->size / 2;
            bool brk = any && x + space + w > maxw;
            if (brk) {
                add_run(p, bi, line_start, line_end - line_start, margin, y + ascent, line_w);
                if (margin + line_w > widest)
                    widest = margin + line_w;
                y += lh;
                any = false;
                x = 0;
            }
            if (!any) {
                line_start = ws;
                x = w;
                any = true;
            } else {
                x += space + w;
            }
            line_end = we;
            line_w = x;
            bool newline = i < n && t[i] == '\n';
            if (i >= n || newline) {
                add_run(p, bi, line_start, line_end - line_start, margin, y + ascent, line_w);
                if (margin + line_w > widest)
                    widest = margin + line_w;
                y += lh;
                any = false;
                x = 0;
                if (i >= n)
                    break;
            }
            i++; /* past the space or newline */
        }
        if (b->link >= 0) {
            /* The link box covers the block's lines. */
            Link *l = &p->links[b->link];
            float top = y, wmax = 0;
            for (size_t r = p->nruns; r-- > 0 && p->runs[r].block == bi;) {
                top = p->runs[r].y - ascent;
                if (p->runs[r].w > wmax)
                    wmax = p->runs[r].w;
            }
            l->box.x0 = (int)margin;
            l->box.y0 = (int)top;
            l->box.x1 = (int)(margin + wmax + 1);
            l->box.y1 = (int)(y + 0.5f);
        }
        y += b->space_after;
    }
    p->content_w = (int)(widest + margin);
    p->content_h = (int)(y + margin);
    if (p->kind == K_SCENE) {
        p->content_w = 600;
        p->content_h = AU_SCENE_HEIGHT;
    } else if (p->kind == K_FONTS) {
        p->content_w = 520;
        p->content_h = 40 + 22 * (int)au_fontset_count(p->env.fonts) + 20;
    }
}

/* ---- Content builders ---- */

static const AuColor ink = AU_RGB(20, 20, 20), grey = AU_RGB(90, 90, 90), link_blue = AU_RGB(0, 0, 204);

static void build_home(AuPage *p)
{
    para(p, "Aura", F_SANS_BOLD, 30, AU_RGB(32, 60, 120), 4);
    para(p,
         "A web browser for Mac OS 9. The page engine is still being built, so this version shows built-in pages, "
         "local files as plain text, and a placeholder for network pages.",
         F_SANS, 14, grey, 14);
    para(p, "Built-in pages", F_SANS_BOLD, 16, ink, 6);
    add_block(p, "Paint and font test scene", 25, F_SANS, 14, link_blue, 4, "about:scene");
    add_block(p, "Installed fonts", 15, F_SANS, 14, link_blue, 4, "about:fonts");
    add_block(p, "Blank page", 10, F_SANS, 14, link_blue, 14, "about:blank");
    para(p, "Placeholders for network pages", F_SANS_BOLD, 16, ink, 6);
    add_block(p, "http://example.com/", 19, F_SANS, 14, link_blue, 4, "http://example.com/");
    add_block(p, "http://www.macintoshrepository.org/", 35, F_SANS, 14, link_blue, 4,
              "http://www.macintoshrepository.org/");
    add_block(p, "https://www.w3.org/TR/css-2023/", 31, F_SANS, 14, link_blue, 14, "https://www.w3.org/TR/css-2023/");
    para(p, "Command-click a link to open it in a new tab.", F_SANS, 12, grey, 0);
}

static void build_remote(AuPage *p, bool done)
{
    char host[256];
    au_url_host(p->url, host, sizeof host);
    para(p, host[0] ? host : p->url, F_SANS_BOLD, 24, ink, 6);
    add_block(p, p->url, strlen(p->url), F_MONO, 13, grey, 12, NULL);
    if (!done) {
        para(p, "Loading...", F_SANS, 14, grey, 0);
    } else if (p->failed) {
        para(p, "Loading stopped.", F_SANS, 14, grey, 0);
    } else {
        para(p,
             "Aura does not load network pages yet. This placeholder stands in for the page while the networking "
             "and layout engines are being written. The title, history, bookmarks, back and forward, reload and "
             "stop all work with it.",
             F_SERIF, 16, ink, 10);
        add_block(p, "Back to the start page", 22, F_SANS, 14, link_blue, 0, "about:home");
    }
}

/* Collapses whitespace in s into out, adding a space between pieces. */
static void append_collapsed(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (au_is_space((unsigned char)c)) {
            if (out->len && out->data[out->len - 1] != ' ' && out->data[out->len - 1] != '\n')
                au_buf_putc(out, ' ');
        } else {
            au_buf_putc(out, c);
        }
    }
}

static bool block_level(AuAtom t)
{
    static const AuAtom tags[] = {AU_ATOM_p,       AU_ATOM_div,     AU_ATOM_h1,      AU_ATOM_h2,      AU_ATOM_h3,
                                  AU_ATOM_h4,      AU_ATOM_h5,      AU_ATOM_h6,      AU_ATOM_li,      AU_ATOM_br,
                                  AU_ATOM_tr,      AU_ATOM_pre,     AU_ATOM_blockquote, AU_ATOM_dt,   AU_ATOM_hr,
                                  AU_ATOM_section, AU_ATOM_article, AU_ATOM_header,  AU_ATOM_footer,  AU_ATOM_nav,
                                  AU_ATOM_main,    AU_ATOM_ul,      AU_ATOM_ol,      AU_ATOM_table,   AU_ATOM_dl};
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; i++)
        if (tags[i] == t)
            return true;
    return false;
}

static void flush_para(AuPage *p, AuBuf *b, Face f, float size)
{
    while (b->len && b->data[b->len - 1] == ' ')
        b->len--;
    if (b->len) {
        add_block(p, b->data, b->len, f, size, ink, size * 0.6f, NULL);
        au_buf_clear(b);
    }
}

static void build_html(AuPage *p, const char *data, size_t len)
{
    AuHtmlOptions opts;
    memset(&opts, 0, sizeof opts);
    AuDocument *doc = au_html_parse(data, len, &opts);
    AuNode *root = &doc->node;
    AuBuf title, text;
    au_buf_init(&title);
    au_buf_init(&text);
    for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
        if (au_is_html_element(n, AU_ATOM_title)) {
            for (AuNode *c = n->first_child; c; c = c->next)
                if (c->type == AU_NODE_TEXT)
                    append_collapsed(&title, au_as_text(c)->data, au_as_text(c)->len);
            break;
        }
    }
    while (title.len && title.data[title.len - 1] == ' ')
        title.len--;
    set_str(&p->title, au_buf_cstr(&title));
    if (title.len)
        para(p, title.data, F_SANS_BOLD, 22, ink, 10);

    AuElement *body = au_document_body(doc);
    AuNode *broot = body ? &body->node : root;
    Face face = F_SERIF;
    float size = 16;
    for (AuNode *n = broot->first_child; n;) {
        if (au_is_element(n)) {
            AuElement *el = au_as_element(n);
            AuAtom t = el->tag;
            if (t == AU_ATOM_script || t == AU_ATOM_style || t == AU_ATOM_template || t == AU_ATOM_head ||
                t == AU_ATOM_title || t == AU_ATOM_noscript) {
                n = au_node_next_skip(n, broot);
                continue;
            }
            if (block_level(t)) {
                flush_para(p, &text, face, size);
                bool heading = t == AU_ATOM_h1 || t == AU_ATOM_h2 || t == AU_ATOM_h3 || t == AU_ATOM_h4;
                if (heading || t == AU_ATOM_pre) {
                    /* Gather the element's text as its own block. */
                    for (AuNode *c = n->first_child; c; c = au_node_next(c, n))
                        if (c->type == AU_NODE_TEXT) {
                            if (t == AU_ATOM_pre)
                                au_buf_append(&text, au_as_text(c)->data, au_as_text(c)->len);
                            else
                                append_collapsed(&text, au_as_text(c)->data, au_as_text(c)->len);
                        }
                    if (t == AU_ATOM_pre)
                        flush_para(p, &text, F_MONO, 13);
                    else
                        flush_para(p, &text, F_SANS_BOLD, t == AU_ATOM_h1 ? 24 : t == AU_ATOM_h2 ? 20 : 17);
                    n = au_node_next_skip(n, broot);
                    continue;
                }
                if (t == AU_ATOM_li)
                    au_buf_puts(&text, "\xE2\x80\xA2 ");
            }
        } else if (n->type == AU_NODE_TEXT) {
            append_collapsed(&text, au_as_text(n)->data, au_as_text(n)->len);
        }
        n = au_node_next(n, broot);
    }
    flush_para(p, &text, face, size);
    if (p->nblocks == 0)
        para(p, "(This document has no text.)", F_SANS, 13, grey, 0);
    au_buf_free(&title);
    au_buf_free(&text);
    au_node_unref(root);
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && au_ieq(s + n - m, m, suffix, m);
}

static void build_file(AuPage *p)
{
    char *data = NULL;
    size_t len = 0;
    if (!p->env.read_file_url || !p->env.read_file_url(p->env.ctx, p->url, &data, &len)) {
        p->failed = true;
        set_str(&p->title, "File not found");
        para(p, "Cannot open this file", F_SANS_BOLD, 22, ink, 8);
        add_block(p, p->url, strlen(p->url), F_MONO, 13, grey, 0, NULL);
        return;
    }
    size_t q = strcspn(p->url, "?#");
    char *path = au_strndup(p->url, q);
    bool html = ends_with(path, ".html") || ends_with(path, ".htm") || ends_with(path, ".xhtml");
    if (!html && !ends_with(path, ".txt")) {
        /* Sniff: text starting with '<' is treated as HTML. */
        size_t i = 0;
        while (i < len && au_is_space((unsigned char)data[i]))
            i++;
        html = i < len && data[i] == '<';
    }
    if (html) {
        build_html(p, data, len);
    } else {
        const char *slash = strrchr(path, '/');
        AuBuf name;
        au_buf_init(&name);
        au_url_percent_decode(&name, slash ? slash + 1 : path, strlen(slash ? slash + 1 : path));
        set_str(&p->title, au_buf_cstr(&name));
        au_buf_free(&name);
        /* Plain text: one block, line breaks kept, CR LF and CR as LF. */
        AuBuf t;
        au_buf_init(&t);
        for (size_t i = 0; i < len; i++) {
            if (data[i] == '\r') {
                au_buf_putc(&t, '\n');
                if (i + 1 < len && data[i + 1] == '\n')
                    i++;
            } else if (data[i] != 0) {
                au_buf_putc(&t, data[i]);
            }
        }
        add_block(p, t.data ? t.data : "", t.len, F_MONO, 13, ink, 0, NULL);
        au_buf_free(&t);
    }
    au_free(path);
    au_free(data);
}

/* ---- Interface ---- */

AuPage *au_page_new(const AuPageEnv *env, const AuPageCallbacks *cb)
{
    AuPage *p = au_calloc(sizeof *p);
    p->env = *env;
    p->cb = *cb;
    p->url = au_strndup("about:blank", 11);
    p->title = au_strndup("", 0);
    p->kind = K_BLANK;
    p->hover = p->pressed = -1;
    p->vw = 640;
    p->vh = 480;
    return p;
}

void au_page_free(AuPage *p)
{
    if (!p)
        return;
    clear_content(p);
    au_free(p->runs);
    au_free(p->url);
    au_free(p->title);
    au_free(p);
}

static void invalidate_all(AuPage *p)
{
    if (p->cb.invalidate) {
        AuIRect r = {0, 0, p->content_w > p->vw ? p->content_w : p->vw, p->content_h > p->vh ? p->content_h : p->vh};
        p->cb.invalidate(p->cb.ctx, p, r);
    }
}

static void relayout(AuPage *p)
{
    layout(p);
    if (p->cb.content_size_changed)
        p->cb.content_size_changed(p->cb.ctx, p);
    invalidate_all(p);
}

static void finish(AuPage *p, bool ok)
{
    p->loading = false;
    p->progress = 1;
    if (p->cb.load_finished)
        p->cb.load_finished(p->cb.ctx, p, ok);
}

static void load(AuPage *p)
{
    clear_content(p);
    p->failed = false;
    set_str(&p->title, "");
    const char *u = p->url;
    p->loading = true;
    p->progress = 0;
    p->load_start = now(p);
    if (p->cb.load_started)
        p->cb.load_started(p->cb.ctx, p);
    if (au_url_scheme_is(u, "about")) {
        const char *what = u + 6;
        if (!strcmp(what, "home")) {
            p->kind = K_HOME;
            set_str(&p->title, "Aura");
            build_home(p);
        } else if (!strcmp(what, "scene")) {
            p->kind = K_SCENE;
            set_str(&p->title, "Test Scene");
        } else if (!strcmp(what, "fonts")) {
            p->kind = K_FONTS;
            set_str(&p->title, "Fonts");
        } else {
            p->kind = K_BLANK;
            if (strcmp(what, "blank") != 0) {
                p->kind = K_TEXT;
                set_str(&p->title, "Unknown page");
                para(p, "Unknown about: page", F_SANS_BOLD, 22, ink, 8);
                add_block(p, u, strlen(u), F_MONO, 13, grey, 0, NULL);
            }
        }
    } else if (au_url_scheme_is(u, "file")) {
        p->kind = K_TEXT;
        build_file(p);
    } else {
        p->kind = K_REMOTE;
        build_remote(p, false);
        relayout(p);
        return; /* au_page_idle finishes the simulated load */
    }
    relayout(p);
    if (p->cb.title_changed)
        p->cb.title_changed(p->cb.ctx, p, p->title);
    finish(p, !p->failed);
}

void au_page_navigate(AuPage *p, const char *url)
{
    set_str(&p->url, url);
    load(p);
}

void au_page_reload(AuPage *p)
{
    load(p);
}

void au_page_stop(AuPage *p)
{
    if (!p->loading)
        return;
    if (p->kind == K_REMOTE) {
        p->failed = true;
        clear_content(p);
        build_remote(p, true);
        relayout(p);
    }
    finish(p, false);
}

const char *au_page_url(const AuPage *p) { return p->url; }
const char *au_page_title(const AuPage *p) { return p->title; }
bool au_page_loading(const AuPage *p) { return p->loading; }
float au_page_progress(const AuPage *p) { return p->progress; }

AuSecurity au_page_security(const AuPage *p)
{
    /* Nothing is connected to TLS yet, so HTTPS is not claimed secure. */
    if (au_url_scheme_is(p->url, "http"))
        return AU_SECURITY_NOT_SECURE;
    return AU_SECURITY_NONE;
}

void au_page_set_viewport(AuPage *p, int width, int height)
{
    if (width == p->vw && height == p->vh)
        return;
    bool rewrap = width != p->vw;
    p->vw = width;
    p->vh = height;
    if (rewrap) {
        layout(p);
        if (p->cb.content_size_changed)
            p->cb.content_size_changed(p->cb.ctx, p);
    }
}

void au_page_content_size(const AuPage *p, int *width, int *height)
{
    *width = p->content_w;
    *height = p->content_h;
}

static void paint_fonts(AuPage *p, AuCanvas *c, float w)
{
    AuFontSet *fonts = p->env.fonts;
    AuFontFace *ui = au_fontset_match(fonts, "system-ui", 400, false);
    char line[200];
    snprintf(line, sizeof line, "%u font faces registered", (unsigned)au_fontset_count(fonts));
    if (ui)
        au_draw_text(c, ui, 13, 12, 24, line, strlen(line), AU_RGB(0, 0, 0));
    for (size_t i = 0; i < au_fontset_count(fonts); i++) {
        int weight;
        bool italic;
        const char *fam = au_fontset_entry(fonts, i, &weight, &italic);
        float y = 52 + 22 * (float)i;
        AuIRect clip = au_canvas_clip(c);
        float dx, dy;
        au_canvas_get_translation(c, &dx, &dy);
        if (y + dy < (float)clip.y0 - 4 || y - 20 + dy > (float)clip.y1)
            continue; /* off screen: do not load the face */
        AuFontFace *f = au_fontset_match(fonts, fam, weight, italic);
        snprintf(line, sizeof line, "%s %d%s", fam, weight, italic ? " italic" : "");
        if (ui)
            au_draw_text(c, ui, 12, 12, y, line, strlen(line), AU_RGB(80, 80, 80));
        if (f)
            au_draw_text(c, f, 15, 230, y, "The quick brown fox 0123", 24, AU_RGB(0, 0, 0));
    }
    (void)w;
}

void au_page_paint(AuPage *p, AuCanvas *c, AuRect view, int scroll_x, int scroll_y)
{
    au_canvas_clip_push(c, view);
    au_fill_rect(c, view, AU_RGB(255, 255, 255));
    au_canvas_translate(c, view.x, view.y);
    if (p->kind == K_SCENE) {
        au_canvas_translate(c, (float)-scroll_x, 0);
        au_scene_draw(c, p->env.fonts, (int)view.w + scroll_x, (int)view.h, scroll_y);
        au_canvas_translate(c, (float)scroll_x, 0);
    } else {
        au_canvas_translate(c, (float)-scroll_x, (float)-scroll_y);
        if (p->kind == K_FONTS) {
            paint_fonts(p, c, view.w);
        } else {
            float top = (float)scroll_y - 40, bottom = (float)scroll_y + view.h + 40;
            for (size_t i = 0; i < p->nruns; i++) {
                Run *r = &p->runs[i];
                if (r->y < top || r->y > bottom)
                    continue;
                Block *b = &p->blocks[r->block];
                AuFontFace *f = face_for(p, b->face);
                if (!f)
                    continue;
                au_draw_text(c, f, b->size, r->x, r->y, b->text + r->off, r->len, b->color);
                if (b->link >= 0) {
                    AuColor uc = b->link == p->hover ? AU_RGB(204, 0, 0) : b->color;
                    au_fill_rect(c, (AuRect){r->x, r->y + 2, r->w, 1}, uc);
                }
            }
        }
        au_canvas_translate(c, (float)scroll_x, (float)scroll_y);
    }
    au_canvas_translate(c, -view.x, -view.y);
    au_canvas_clip_pop(c);
}

static int link_at(AuPage *p, int x, int y)
{
    for (size_t i = 0; i < p->nlinks; i++) {
        AuIRect b = p->links[i].box;
        if (x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1)
            return (int)i;
    }
    return -1;
}

static void set_hover(AuPage *p, int h)
{
    if (h == p->hover)
        return;
    if (p->hover >= 0 && p->cb.invalidate)
        p->cb.invalidate(p->cb.ctx, p, p->links[p->hover].box);
    p->hover = h;
    if (h >= 0 && p->cb.invalidate)
        p->cb.invalidate(p->cb.ctx, p, p->links[h].box);
    if (p->cb.status_text)
        p->cb.status_text(p->cb.ctx, p, h >= 0 ? p->links[h].url : "");
}

bool au_page_mouse(AuPage *p, const AuEvent *ev)
{
    int l = link_at(p, ev->x, ev->y);
    switch (ev->type) {
    case AU_EVENT_MOUSE_MOVE:
        set_hover(p, l);
        return l >= 0;
    case AU_EVENT_MOUSE_DOWN:
        p->pressed = l;
        return l >= 0;
    case AU_EVENT_MOUSE_UP: {
        int pressed = p->pressed;
        p->pressed = -1;
        if (l >= 0 && l == pressed && p->cb.open_url) {
            /* Copy: open_url may navigate this page, freeing the links. */
            char *u = au_strndup(p->links[l].url, strlen(p->links[l].url));
            p->cb.open_url(p->cb.ctx, p, u, (ev->mods & AU_MOD_COMMAND) != 0);
            au_free(u);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool au_page_key(AuPage *p, const AuEvent *ev)
{
    (void)p;
    (void)ev;
    return false;
}

void au_page_idle(AuPage *p)
{
    if (!p->loading || p->kind != K_REMOTE)
        return;
    uint32_t t = now(p) - p->load_start;
    float prog = (float)t / SIM_LOAD_MS;
    if (prog >= 1) {
        clear_content(p);
        char host[256];
        au_url_host(p->url, host, sizeof host);
        set_str(&p->title, host[0] ? host : p->url);
        build_remote(p, true);
        relayout(p);
        if (p->cb.title_changed)
            p->cb.title_changed(p->cb.ctx, p, p->title);
        finish(p, true);
        return;
    }
    /* Progress in visible steps, so the status bar repaints a few times a
     * second rather than on every idle event. */
    prog = (float)(int)(prog * 20) / 20;
    if (prog != p->progress) {
        p->progress = prog;
        if (p->cb.load_progress)
            p->cb.load_progress(p->cb.ctx, p);
    }
}

bool au_page_busy(const AuPage *p)
{
    return p->loading;
}
