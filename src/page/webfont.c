/* @font-face loading: rules from style sheets, demand from the styled
 * document, fetches through the document's loader, faces registered in a
 * font set layered over the platform set. See webfont.h. */
#include "webfont.h"

#include "page.h"

#include <string.h>

#define MAX_RULES 128
#define MAX_SRC 8
#define MAX_RANGES 64
#define MAX_LOADED 48                /* faces a document may load */
#define MAX_FONT_BYTES (8u << 20)    /* a decoded font */

enum { R_IDLE, R_LOADING, R_LOADED, R_FAILED };

typedef struct Src {
    char *url;   /* absolute, or NULL */
    char *local; /* a local() name, or NULL */
} Src;

typedef struct Rule {
    char family[64];
    int wmin, wmax;
    bool italic;
    uint32_t ranges[MAX_RANGES * 2];
    size_t nranges; /* 0: every code point */
    Src src[MAX_SRC];
    int nsrc, next;
    uint32_t req;
    uint8_t state;
    uint64_t key;
} Rule;

struct AuWebFonts {
    AuFontSet *platform, *set;
    Rule *rules;
    size_t n, cap;
    size_t loaded;
    bool changed;
};

AuWebFonts *au_webfonts_new(AuFontSet *platform)
{
    AuWebFonts *w = au_calloc(sizeof *w);
    w->platform = platform;
    w->set = au_fontset_new_layered(platform);
    return w;
}

void au_webfonts_free(AuWebFonts *w)
{
    if (!w)
        return;
    for (size_t i = 0; i < w->n; i++)
        for (int k = 0; k < w->rules[i].nsrc; k++) {
            au_free(w->rules[i].src[k].url);
            au_free(w->rules[i].src[k].local);
        }
    au_free(w->rules);
    au_fontset_free(w->set);
    au_free(w);
}

AuFontSet *au_webfonts_set(AuWebFonts *w)
{
    return w->set;
}

bool au_webfonts_take_changed(AuWebFonts *w)
{
    bool c = w->changed;
    w->changed = false;
    return c;
}

void au_webfonts_counts(const AuWebFonts *w, size_t *rules, size_t *loaded)
{
    *rules = w->n;
    *loaded = w->loaded;
}

bool au_webfonts_owns(const AuWebFonts *w, uint32_t id)
{
    for (size_t i = 0; i < w->n; i++)
        if (w->rules[i].state == R_LOADING && w->rules[i].req == id)
            return true;
    return false;
}

/* ---- unicode-range, read from the sheet's source ----
 * The CSS parser keeps unknown descriptors as generic values, which do not
 * preserve <urange> tokens, so the ranges come from the text: the n-th
 * @font-face block of the source is the parser's n-th rule. */

typedef struct RawFace {
    const char *v; /* unicode-range value, or NULL */
    size_t n;
} RawFace;

/* Past a comment or string at i, else i. */
static size_t skip_cs(const char *t, size_t n, size_t i)
{
    if (i + 1 < n && t[i] == '/' && t[i + 1] == '*') {
        for (i += 2; i + 1 < n; i++)
            if (t[i] == '*' && t[i + 1] == '/')
                return i + 2;
        return n;
    }
    if (t[i] == '"' || t[i] == '\'') {
        char q = t[i++];
        while (i < n && t[i] != q && t[i] != '\n')
            i += t[i] == '\\' ? 2 : 1;
        return i < n ? i + 1 : n;
    }
    return i;
}

static bool ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           (unsigned char)c >= 0x80;
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static size_t scan_raw(const char *t, size_t n, RawFace *out, size_t max)
{
    size_t i = 0, count = 0;
    while (i < n) {
        size_t j = skip_cs(t, n, i);
        if (j != i) {
            i = j;
            continue;
        }
        if (!(t[i] == '@' && n - i >= 10 && au_ieq(t + i + 1, 9, "font-face", 9) && (i + 10 >= n || !ident_char(t[i + 10])))) {
            i++;
            continue;
        }
        i += 10;
        while (i < n && t[i] != '{' && t[i] != ';' && t[i] != '}') {
            j = skip_cs(t, n, i);
            i = j != i ? j : i + 1;
        }
        if (i >= n || t[i] != '{')
            continue;
        i++;
        RawFace rf = {NULL, 0};
        int depth = 0;
        bool at_decl = true;
        while (i < n) {
            j = skip_cs(t, n, i);
            if (j != i) {
                i = j;
                continue;
            }
            char c = t[i];
            if (c == '{' || c == '(' || c == '[') {
                depth++;
            } else if (c == '}' || c == ')' || c == ']') {
                if (depth == 0 && c == '}')
                    break;
                if (depth)
                    depth--;
            } else if (depth == 0 && c == ';') {
                at_decl = true;
                i++;
                continue;
            } else if (depth == 0 && at_decl && !is_ws(c)) {
                at_decl = false;
                if (n - i > 13 && au_ieq(t + i, 13, "unicode-range", 13) && !ident_char(t[i + 13])) {
                    size_t k = i + 13;
                    while (k < n && is_ws(t[k]))
                        k++;
                    if (k < n && t[k] == ':') {
                        k++;
                        size_t vs = k;
                        while (k < n && t[k] != ';' && t[k] != '}') {
                            size_t m = skip_cs(t, n, k);
                            k = m != k ? m : k + 1;
                        }
                        rf.v = t + vs;
                        rf.n = k - vs;
                        i = k;
                        continue;
                    }
                }
            }
            i++;
        }
        if (count < max)
            out[count] = rf;
        count++;
        if (i < n)
            i++;
    }
    return count;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parses "U+0-7F, U+4??" into pairs. Returns the pair count, or -1 when
 * the descriptor is invalid (and so ignored). */
static int parse_ranges(const char *v, size_t n, uint32_t *out, int max_pairs)
{
    int np = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && (is_ws(v[i]) || v[i] == ','))
            i++;
        if (i >= n)
            break;
        if (n - i < 3 || (v[i] != 'u' && v[i] != 'U') || v[i + 1] != '+')
            return -1;
        i += 2;
        uint32_t lo = 0, hi = 0;
        int digits = 0, wild = 0;
        while (i < n && digits < 6 && (hexv(v[i]) >= 0 || v[i] == '?')) {
            if (v[i] == '?') {
                wild++;
                lo = lo << 4;
                hi = (hi << 4) | 15;
            } else {
                if (wild)
                    return -1;
                lo = (lo << 4) | (uint32_t)hexv(v[i]);
                hi = (hi << 4) | (uint32_t)hexv(v[i]);
            }
            digits++;
            i++;
        }
        if (!digits)
            return -1;
        if (!wild && i < n && v[i] == '-') {
            i++;
            hi = 0;
            int d2 = 0;
            while (i < n && d2 < 6 && hexv(v[i]) >= 0) {
                hi = (hi << 4) | (uint32_t)hexv(v[i]);
                d2++;
                i++;
            }
            if (!d2)
                return -1;
        }
        while (i < n && is_ws(v[i]))
            i++;
        if (i < n && v[i] != ',')
            return -1;
        if (lo > 0x10FFFF || lo > hi)
            return -1;
        if (hi > 0x10FFFF)
            hi = 0x10FFFF;
        if (np < max_pairs) {
            out[np * 2] = lo;
            out[np * 2 + 1] = hi;
            np++;
        }
    }
    return np ? np : -1;
}

/* ---- Descriptors ---- */

static const AuCssValue *items(const AuCssValue *v, uint32_t *n)
{
    if (v->type == AU_CSSV_LIST || v->type == AU_CSSV_COMMA_LIST) {
        *n = v->n;
        return v->u.items;
    }
    *n = 1;
    return v;
}

/* A family or local() name: a string, or identifiers joined by spaces. */
static bool value_name(const AuCssValue *v, char *out, size_t cap)
{
    out[0] = 0;
    if (v->type == AU_CSSV_STRING || v->type == AU_CSSV_IDENT) {
        if (!v->u.str || strlen(v->u.str) >= cap)
            return false;
        strcpy(out, v->u.str);
        return out[0] != 0;
    }
    if (v->type != AU_CSSV_LIST)
        return false;
    size_t len = 0;
    for (uint32_t i = 0; i < v->n; i++) {
        const AuCssValue *it = &v->u.items[i];
        if (it->type != AU_CSSV_IDENT || !it->u.str)
            return false;
        size_t k = strlen(it->u.str);
        if (len + k + 2 > cap)
            return false;
        if (len)
            out[len++] = ' ';
        memcpy(out + len, it->u.str, k);
        len += k;
        out[len] = 0;
    }
    return len > 0;
}

static int weight_value(const AuCssValue *v)
{
    if (v->type == AU_CSSV_KEYWORD)
        return v->kw == AU_K_bold ? 700 : v->kw == AU_K_normal ? 400 : -1;
    if (v->type == AU_CSSV_NUMBER && v->u.num >= 1 && v->u.num <= 1000)
        return (int)(v->u.num + 0.5f);
    return -1;
}

static bool format_ok(const AuCssValue *fn)
{
    static const char *const ok[] = {"woff2", "woff", "truetype", "opentype", "collection", "woff2-variations",
                                     "woff-variations", "truetype-variations", "opentype-variations"};
    for (uint32_t i = 0; i < fn->n; i++) {
        uint32_t m;
        const AuCssValue *a = items(&fn->u.items[i], &m);
        for (uint32_t k = 0; k < m; k++) {
            if ((a[k].type != AU_CSSV_STRING && a[k].type != AU_CSSV_IDENT) || !a[k].u.str)
                continue;
            for (size_t f = 0; f < sizeof ok / sizeof ok[0]; f++)
                if (au_ieq(a[k].u.str, strlen(a[k].u.str), ok[f], strlen(ok[f])))
                    return true;
        }
    }
    return false;
}

static bool tech_ok(const AuCssValue *fn)
{
    static const char *const ok[] = {"variations", "color-colrv0", "color-colrv1", "features-opentype"};
    for (uint32_t i = 0; i < fn->n; i++) {
        uint32_t m;
        const AuCssValue *a = items(&fn->u.items[i], &m);
        for (uint32_t k = 0; k < m; k++) {
            bool known = false;
            if ((a[k].type == AU_CSSV_IDENT || a[k].type == AU_CSSV_STRING) && a[k].u.str)
                for (size_t f = 0; f < sizeof ok / sizeof ok[0]; f++)
                    known |= au_ieq(a[k].u.str, strlen(a[k].u.str), ok[f], strlen(ok[f]));
            if (!known)
                return false;
        }
    }
    return true;
}

static void parse_src(Rule *r, const AuCssValue *v, const char *base)
{
    uint32_t n;
    const AuCssValue *it = items(v, &n);
    if (v->type != AU_CSSV_COMMA_LIST) {
        n = 1;
        it = v;
    }
    for (uint32_t i = 0; i < n && r->nsrc < MAX_SRC; i++) {
        const AuCssValue *e = &it[i];
        char name[96];
        if (e->type == AU_CSSV_FUNCTION && e->kw == AU_K_local) {
            if (e->n == 1 && value_name(&e->u.items[0], name, sizeof name))
                r->src[r->nsrc++].local = au_strndup(name, strlen(name));
            continue;
        }
        const AuCssValue *url = NULL;
        bool ok = true;
        if (e->type == AU_CSSV_URL) {
            url = e;
        } else if (e->type == AU_CSSV_LIST && e->n >= 1 && e->u.items[0].type == AU_CSSV_URL) {
            url = &e->u.items[0];
            for (uint32_t k = 1; k < e->n; k++) {
                const AuCssValue *f = &e->u.items[k];
                if (f->type == AU_CSSV_FUNCTION && f->kw == AU_K_format)
                    ok &= format_ok(f);
                else if (f->type == AU_CSSV_FUNCTION && f->kw == AU_K_tech)
                    ok &= tech_ok(f);
                else
                    ok = false;
            }
        }
        char *abs = url && ok && url->u.str ? au_url_resolve_dup(base ? base : "", url->u.str) : NULL;
        if (!abs)
            continue;
        r->src[r->nsrc++].url = abs;
    }
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static uint64_t rule_key(const Rule *r)
{
    uint64_t h = 14695981039346656037ull;
    char lower[64];
    size_t fl = strlen(r->family);
    for (size_t i = 0; i < fl; i++)
        lower[i] = (char)au_lower((unsigned char)r->family[i]);
    h = fnv(h, lower, fl);
    int d[3] = {r->wmin, r->wmax, r->italic};
    h = fnv(h, d, sizeof d);
    h = fnv(h, r->ranges, r->nranges * 2 * sizeof r->ranges[0]);
    for (int i = 0; i < r->nsrc; i++) {
        const char *s = r->src[i].url ? r->src[i].url : r->src[i].local;
        h = fnv(h, r->src[i].url ? "u" : "l", 1);
        h = fnv(h, s, strlen(s) + 1);
    }
    return h;
}

void au_webfonts_add_sheet(AuWebFonts *w, const AuCssSheet *sh, const char *text, size_t len)
{
    size_t nf = au_css_sheet_font_face_count(sh);
    if (!nf)
        return;
    RawFace raw[MAX_RULES];
    size_t nraw = text ? scan_raw(text, len, raw, MAX_RULES) : 0;
    bool use_raw = nraw == nf;
    for (size_t i = 0; i < nf && w->n < MAX_RULES; i++) {
        const AuCssFontFace *f = au_css_sheet_font_face(sh, i);
        const AuCssValue *fam = au_css_font_face_descriptor(f, "font-family");
        const AuCssValue *src = au_css_font_face_descriptor(f, "src");
        if (!fam || !src)
            continue;
        Rule r;
        memset(&r, 0, sizeof r);
        if (!value_name(fam, r.family, sizeof r.family))
            continue;
        r.wmin = r.wmax = 400;
        const AuCssValue *wt = au_css_font_face_descriptor(f, "font-weight");
        if (wt) {
            uint32_t m;
            const AuCssValue *a = items(wt, &m);
            int lo = weight_value(&a[0]), hi = m > 1 ? weight_value(&a[1]) : lo;
            if (lo > 0 && hi > 0 && m <= 2) {
                r.wmin = lo < hi ? lo : hi;
                r.wmax = lo < hi ? hi : lo;
            }
        }
        const AuCssValue *st = au_css_font_face_descriptor(f, "font-style");
        if (st) {
            uint32_t m;
            const AuCssValue *a = items(st, &m);
            r.italic = a[0].type == AU_CSSV_KEYWORD && (a[0].kw == AU_K_italic || a[0].kw == AU_K_oblique);
        }
        if (use_raw && raw[i].v) {
            int np = parse_ranges(raw[i].v, raw[i].n, r.ranges, MAX_RANGES);
            r.nranges = np > 0 ? (size_t)np : 0;
        }
        parse_src(&r, src, au_css_font_face_base_url(f));
        if (!r.nsrc)
            continue;
        r.key = rule_key(&r);
        bool known = false;
        for (size_t k = 0; k < w->n && !known; k++)
            known = w->rules[k].key == r.key;
        if (known) {
            for (int k = 0; k < r.nsrc; k++) {
                au_free(r.src[k].url);
                au_free(r.src[k].local);
            }
            continue;
        }
        if (w->n == w->cap) {
            w->cap = w->cap ? w->cap * 2 : 8;
            w->rules = au_realloc(w->rules, w->cap * sizeof *w->rules);
        }
        w->rules[w->n++] = r;
    }
}

/* ---- Loading ---- */

static bool add_face(AuWebFonts *w, Rule *r, uint8_t *data, size_t len)
{
    if (!au_fontset_add_web(w->set, data, len, r->family, r->wmin, r->wmax, r->italic, r->nranges ? r->ranges : NULL,
                            r->nranges))
        return false; /* data freed */
    r->state = R_LOADED;
    w->loaded++;
    w->changed = true;
    return true;
}

/* A local() face: a platform face whose full name (or family) is name. */
static bool try_local(AuWebFonts *w, Rule *r, const char *name)
{
    const char *nm = name;
    AuFontFace *f = au_fontset_match_list(w->platform, &nm, 1, r->wmin, r->italic);
    size_t n = strlen(name);
    if (!f || !(au_ieq(f->full_name, strlen(f->full_name), name, n) || au_ieq(f->family, strlen(f->family), name, n)))
        return false;
    if (au_font_face_count(f->data, f->len) != 1 || f->len > MAX_FONT_BYTES)
        return false;
    uint8_t *copy = au_alloc(f->len);
    memcpy(copy, f->data, f->len);
    return add_face(w, r, copy, f->len);
}

static void start(AuWebFonts *w, Rule *r, AuWebFontFetch fetch, void *ctx)
{
    while (r->next < r->nsrc) {
        Src *s = &r->src[r->next++];
        if (s->local) {
            if (try_local(w, r, s->local))
                return;
            continue;
        }
        if (w->loaded >= MAX_LOADED || !fetch)
            break;
        uint32_t id = fetch(ctx, s->url);
        if (id) {
            r->req = id;
            r->state = R_LOADING;
            return;
        }
    }
    r->state = R_FAILED;
}

void au_webfonts_done(AuWebFonts *w, uint32_t id, const void *data, size_t len, bool ok, AuWebFontFetch fetch,
                      void *ctx)
{
    for (size_t i = 0; i < w->n; i++) {
        Rule *r = &w->rules[i];
        if (r->state != R_LOADING || r->req != id)
            continue;
        r->req = 0;
        uint8_t *out;
        size_t olen;
        if (ok && data && w->loaded < MAX_LOADED &&
            au_font_decode(data, len, MAX_FONT_BYTES, &out, &olen) == AU_FONT_OK && add_face(w, r, out, olen))
            return;
        start(w, r, fetch, ctx);
        return;
    }
}

/* CSS font matching distance of a rule's weight range from a weight:
 * within the range is 0; for weights above 500 heavier faces come first,
 * otherwise lighter ones. */
static int weight_distance(const Rule *r, int wt)
{
    if (wt >= r->wmin && wt <= r->wmax)
        return 0;
    if (r->wmin > wt)
        return wt > 500 ? r->wmin - wt : 1000 + r->wmin - wt;
    return wt > 500 ? 1000 + wt - r->wmax : wt - r->wmax;
}

static bool in_ranges(const Rule *r, uint32_t cp)
{
    if (!r->nranges)
        return true;
    for (size_t i = 0; i < r->nranges; i++)
        if (cp >= r->ranges[i * 2] && cp <= r->ranges[i * 2 + 1])
            return true;
    return false;
}

static bool text_in_range(const Rule *r, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t adv;
        uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
        i += adv ? adv : 1;
        if (cp > 0x20 && in_ranges(r, cp))
            return true;
    }
    return false;
}

#define MAX_CAND 32

/* The rules the computed font of a text node could use: for each family
 * of its list, the best descriptor match among that family's rules (all
 * rules with that descriptor, which differ by unicode-range). */
static size_t candidates(AuWebFonts *w, const AuStyleInh *in, uint16_t *cand)
{
    size_t nc = 0;
    if (!in->font_family)
        return 0;
    uint32_t nf;
    const AuCssValue *fl = items(&in->font_family->value, &nf);
    bool italic = in->font_style != AU_FONT_STYLE_NORMAL;
    for (uint32_t i = 0; i < nf && i < 16; i++) {
        if ((fl[i].type != AU_CSSV_STRING && fl[i].type != AU_CSSV_IDENT) || !fl[i].u.str)
            continue;
        const char *name = fl[i].u.str;
        size_t len = strlen(name);
        int best = -1, best_d = 0;
        for (size_t k = 0; k < w->n; k++) {
            const Rule *r = &w->rules[k];
            if (!au_ieq(r->family, strlen(r->family), name, len))
                continue;
            int d = weight_distance(r, in->font_weight) + (r->italic != italic ? 10000 : 0);
            if (best < 0 || d < best_d) {
                best = (int)k;
                best_d = d;
            }
        }
        if (best < 0)
            continue;
        const Rule *b = &w->rules[best];
        for (size_t k = 0; k < w->n && nc < MAX_CAND; k++) {
            const Rule *r = &w->rules[k];
            if (r->state == R_IDLE && r->wmin == b->wmin && r->wmax == b->wmax && r->italic == b->italic &&
                au_ieq(r->family, strlen(r->family), name, len))
                cand[nc++] = (uint16_t)k;
        }
    }
    return nc;
}

/* Text that generated content may show where the sheet does not spell
 * it out: counters, attr(), quotes. */
static const char GENERATED_SAMPLE[] = "0123456789Aa\"\xE2\x80\x9C";

/* Starts the faces that the content of a ::before or ::after style
 * shows: icon fonts are often used only there. */
static void scan_pseudo(AuWebFonts *w, const AuStyle *ps, AuWebFontFetch fetch, void *ctx)
{
    if (!ps || !ps->rare || !ps->rare->content || ps->box->display == AU_DISPLAY_NONE)
        return;
    uint16_t cand[MAX_CAND];
    size_t nc = candidates(w, ps->inh, cand);
    if (!nc)
        return;
    uint32_t n;
    const AuCssValue *it = items(&ps->rare->content->value, &n);
    for (uint32_t k = 0; k < n; k++) {
        const char *s;
        size_t len;
        if (it[k].type == AU_CSSV_STRING && it[k].u.str) {
            s = it[k].u.str;
            len = strlen(s);
        } else if (it[k].type == AU_CSSV_FUNCTION || it[k].type == AU_CSSV_KEYWORD) {
            if (it[k].type == AU_CSSV_KEYWORD && (it[k].kw == AU_K_none || it[k].kw == AU_K_normal))
                return;
            s = GENERATED_SAMPLE;
            len = sizeof GENERATED_SAMPLE - 1;
        } else {
            continue; /* url(): an image */
        }
        for (size_t i = 0; i < nc; i++) {
            Rule *r = &w->rules[cand[i]];
            if (r->state == R_IDLE && text_in_range(r, s, len))
                start(w, r, fetch, ctx);
        }
    }
}

void au_webfonts_scan(AuWebFonts *w, AuDocument *doc, AuWebFontFetch fetch, void *ctx)
{
    bool idle = false;
    for (size_t i = 0; i < w->n && !idle; i++)
        idle = w->rules[i].state == R_IDLE;
    if (!idle || !doc)
        return;
    const AuStyleInh *last = NULL;
    uint16_t cand[MAX_CAND];
    size_t nc = 0;
    for (AuNode *c = au_node_next(&doc->node, &doc->node); c; c = au_node_next(c, &doc->node)) {
        if (c->type == AU_NODE_ELEMENT) {
            const AuStyle *es = au_element_style(au_as_element(c));
            if (es && es->pseudo_bits) {
                scan_pseudo(w, au_style_pseudo(es, AU_CSS_PSEUDO_BEFORE), fetch, ctx);
                scan_pseudo(w, au_style_pseudo(es, AU_CSS_PSEUDO_AFTER), fetch, ctx);
            }
            continue;
        }
        if (c->type != AU_NODE_TEXT || !c->parent || !au_is_element(c->parent))
            continue;
        const AuStyle *st = au_element_style(au_as_element(c->parent));
        if (!st || !st->inh)
            continue;
        if (st->inh != last) {
            last = st->inh;
            nc = candidates(w, st->inh, cand);
        }
        const AuText *t = au_as_text(c);
        for (size_t i = 0; i < nc; i++) {
            Rule *r = &w->rules[cand[i]];
            if (r->state == R_IDLE && text_in_range(r, t->data, t->len))
                start(w, r, fetch, ctx);
        }
    }
}
