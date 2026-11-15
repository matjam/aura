/* Grid layout (CSS Grid Layout Level 1 and the auto-repeat, gap and
 * alignment parts of Level 2), horizontal writing mode, left to right.
 *
 * A layout runs in this order:
 *   1. The explicit grid: track lists (repeat(), auto-fill and auto-fit,
 *      line names) and named areas.
 *   2. Placement (section 8): each item's lines are resolved; items with
 *      definite lines are placed, then the auto-placement algorithm runs
 *      (sparse or dense, row or column flow) over an occupancy bitmap.
 *      Implicit tracks before the explicit grid shift every index so that
 *      tracks are numbered from 0.
 *   3. Track sizing (section 11) for columns, then for rows with each
 *      item's width known: initialize, resolve intrinsic sizes in span
 *      order, maximize, expand flexible tracks, stretch auto tracks.
 *   4. Content distribution (justify-content, align-content), then each
 *      item is laid out and aligned in its area.
 *
 * Axis 0 is columns (sizes are widths), axis 1 is rows.
 *
 * Limits. An axis has at most G_MAX_TRACKS tracks: lines beyond are
 * clamped to the last track, spans are cut, and repeat() counts stop
 * there, so a style sheet cannot make layout allocate without limit.
 *
 * Not done: subgrid (treated as none), baseline alignment (start),
 * direction: rtl and vertical writing modes, safe overflow alignment, and
 * painting in order-modified order. An item's min-content and max-content
 * heights are both its height at its used width. */
#include "layout_int.h"

#include <math.h>
#include <stdlib.h>

#define G_INF 1e30f
#define G_MAX_PX 1e7f
#define G_MAX_TRACKS 1000
#define G_WORDS ((G_MAX_TRACKS + 31) / 32)
#define G_MAX_NAMES 4096
#define G_MAX_AREAS 1024

/* Track sizing function kinds. */
enum { GK_PX, GK_AUTO, GK_MINC, GK_MAXC, GK_FR, GK_FIT };

typedef struct GTrack {
    const AuCssValue *def; /* the track size value; NULL for auto */
    float min_v, max_v;    /* px, flex factor or fit-content limit */
    float base, limit, plan;
    float pos;             /* start offset in the content box */
    uint8_t min_k, max_k;  /* GK_* */
    bool autofit;          /* made by repeat(auto-fit) */
    bool collapsed;        /* an empty auto-fit track: size 0, no gutter */
    bool sep;              /* a gutter follows this track */
    bool inf_grow;
} GTrack;

typedef struct GItem {
    LBlock *k;
    int32_t lo[2], hi[2]; /* tracks [lo, hi) per axis */
    int32_t span[2];
    bool def[2];          /* placed by definite lines */
    bool have[2];         /* contributions are computed */
    bool flexspan[2];     /* spans a flexible track */
    bool min_auto[2];     /* the minimum contribution is the automatic minimum */
    float cmin[2], cmax[2], cmm[2]; /* outer min-content, max-content and minimum contributions */
    float edge[2];        /* margins, borders and padding */
    float w;              /* used content width */
    int32_t order;
} GItem;

typedef struct GName {
    const char *s;
    uint32_t len;
    int32_t line;
} GName;

typedef struct GArea {
    const char *s;
    uint32_t len;
    int32_t lo[2], hi[2];
} GArea;

typedef struct GSort {
    float room;
    int32_t i;
} GSort;

typedef struct GOrd {
    int32_t key;
    uint32_t i;
} GOrd;

typedef struct Grid {
    AuLayout *L;
    LBlock *b;
    const AuStyleFlex *fs;
    /* the explicit grid */
    const AuCssValue **edef[2]; /* template track sizes */
    int32_t ntpl[2], edef_cap[2];
    int32_t af_lo[2], af_hi[2]; /* auto-fit tracks in the template */
    int32_t nexp[2];            /* explicit tracks (template or areas) */
    int32_t narea[2];
    GName *names[2];
    uint32_t nnames[2], names_cap[2];
    GArea *areas;
    uint32_t nareas;
    /* the implicit grid */
    int32_t off[2]; /* implicit tracks before the explicit grid */
    int32_t nt[2];
    GTrack *tr[2];
    float gap[2];
    bool stretch[2]; /* content distribution stretches auto tracks */
    bool pct_indef;  /* a percentage track met an indefinite size */
    GItem *it;
    uint32_t n;
    GOrd *byspan[2];
    /* scratch */
    GSort *sort;
    uint8_t *mark; /* G_MAX_TRACKS + 2 */
} Grid;

static int align_of(int v) { return v & 0xFF; }

static float gap_px(const AuLen *l, float basis)
{
    if (!au_len_is_definite_kind(l))
        return 0;
    if (l_len_pct(l) && basis < 0)
        return 0;
    float v = l_len(l, basis);
    if (!(v > 0))
        return 0;
    return v > G_MAX_PX ? G_MAX_PX : v;
}

/* ---- Values ---- */

static bool val_has_pct(const AuCssValue *v, int depth)
{
    if (v->type == AU_CSSV_PERCENT)
        return true;
    if (v->type == AU_CSSV_CALC) {
        if (depth >= 16)
            return true; /* too deep to tell: treat as a percentage */
        for (uint32_t i = 0; i < v->n; i++)
            if (val_has_pct(&v->u.items[i], depth + 1))
                return true;
    }
    return false;
}

/* A computed length-percentage: a px dimension, a percentage, or a calc
 * tree, which au_len_resolve evaluates (the blob header is only a
 * carrier for the node; nothing retains it). */
static float val_len(const AuCssValue *v, float basis)
{
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num * basis / 100;
    if (v->type == AU_CSSV_DIMENSION || v->type == AU_CSSV_NUMBER)
        return v->u.num;
    if (v->type == AU_CSSV_CALC) {
        AuCssBlob tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.refs = 1;
        tmp.value = *v;
        AuLen l;
        memset(&l, 0, sizeof l);
        l.kind = AU_LEN_CALC;
        l.u.calc = &tmp;
        return au_len_resolve(&l, basis < 0 ? 0 : basis);
    }
    return 0;
}

static float px_clamp(float v)
{
    if (!(v > 0))
        return 0; /* negative and NaN */
    return v > G_MAX_PX ? G_MAX_PX : v;
}

static void breadth(Grid *g, const AuCssValue *v, float basis, uint8_t *k, float *val)
{
    *val = 0;
    if (v->type == AU_CSSV_KEYWORD) {
        *k = v->kw == AU_K_min_content ? GK_MINC : v->kw == AU_K_max_content ? GK_MAXC : GK_AUTO;
        return;
    }
    if (v->type == AU_CSSV_DIMENSION && v->unit == AU_UNIT_FR) {
        *k = GK_FR;
        *val = px_clamp(v->u.num);
        return;
    }
    if (basis < 0 && val_has_pct(v, 0)) {
        g->pct_indef = true;
        *k = GK_AUTO;
        return;
    }
    *k = GK_PX;
    *val = px_clamp(val_len(v, basis));
}

/* The sizing functions of a track for a percentage basis (-1: indefinite,
 * percentages behave as auto). */
static void track_resolve(Grid *g, GTrack *t, float basis)
{
    const AuCssValue *d = t->def;
    t->min_k = t->max_k = GK_AUTO;
    t->min_v = t->max_v = 0;
    if (t->collapsed) {
        t->min_k = t->max_k = GK_PX;
        return;
    }
    if (!d)
        return;
    if (d->type == AU_CSSV_FUNCTION && d->kw == AU_K_minmax && d->n == 2) {
        breadth(g, &d->u.items[0], basis, &t->min_k, &t->min_v);
        breadth(g, &d->u.items[1], basis, &t->max_k, &t->max_v);
        if (t->min_k == GK_FR) {
            t->min_k = GK_AUTO;
            t->min_v = 0;
        }
        if (t->min_k == GK_PX && t->max_k == GK_PX && t->max_v < t->min_v)
            t->max_v = t->min_v;
    } else if (d->type == AU_CSSV_FUNCTION && d->kw == AU_K_fit_content && d->n == 1) {
        uint8_t k;
        float v;
        breadth(g, &d->u.items[0], basis, &k, &v);
        if (k == GK_PX) {
            t->max_k = GK_FIT;
            t->max_v = v;
        } else {
            t->max_k = GK_MAXC;
        }
    } else if (d->type == AU_CSSV_FUNCTION) {
        /* unknown function: auto */
    } else {
        breadth(g, d, basis, &t->max_k, &t->max_v);
        if (t->max_k != GK_FR) {
            t->min_k = t->max_k;
            t->min_v = t->max_v;
        }
    }
}

static bool is_line_names(const AuCssValue *v) { return v->type == AU_CSSV_FUNCTION && v->kw == 0 && v->unit == '['; }

static bool is_kw(const AuCssValue *v, int kw) { return v->type == AU_CSSV_KEYWORD && v->kw == kw; }

/* ---- The explicit grid ---- */

static void add_names(Grid *g, int ax, const AuCssValue *fn, int32_t line)
{
    for (uint32_t i = 0; i < fn->n; i++) {
        uint32_t m;
        const AuCssValue *it = l_items(&fn->u.items[i], &m);
        for (uint32_t j = 0; j < m; j++) {
            size_t len;
            const char *s = l_val_name(&it[j], &len);
            if (!s || !len || g->nnames[ax] >= G_MAX_NAMES)
                continue;
            if (g->nnames[ax] == g->names_cap[ax]) {
                g->names_cap[ax] = g->names_cap[ax] ? g->names_cap[ax] * 2 : 16;
                g->names[ax] = au_realloc(g->names[ax], g->names_cap[ax] * sizeof(GName));
            }
            GName *nm = &g->names[ax][g->nnames[ax]++];
            nm->s = s;
            nm->len = (uint32_t)len;
            nm->line = line;
        }
    }
}

static void push_def(Grid *g, int ax, const AuCssValue *def)
{
    if (g->ntpl[ax] >= G_MAX_TRACKS)
        return;
    if (g->ntpl[ax] == g->edef_cap[ax]) {
        g->edef_cap[ax] = g->edef_cap[ax] ? g->edef_cap[ax] * 2 : 16;
        g->edef[ax] = au_realloc(g->edef[ax], (size_t)g->edef_cap[ax] * sizeof(AuCssValue *));
    }
    g->edef[ax][g->ntpl[ax]++] = def;
}

/* The size a track counts for when fitting repeat(auto-fill): its max
 * sizing function if definite, else its min sizing function, else 0. */
static float track_estimate(Grid *g, const AuCssValue *def, float basis)
{
    GTrack t;
    memset(&t, 0, sizeof t);
    t.def = def;
    bool save = g->pct_indef;
    track_resolve(g, &t, basis);
    g->pct_indef = save;
    if (t.max_k == GK_PX)
        return t.max_v;
    if (t.min_k == GK_PX)
        return t.min_v;
    return 0;
}

/* Expands a track list. avail is the size auto-repeat fills (-1: none,
 * one repetition); at_least: avail is a minimum size, so the count is
 * rounded up. */
static void build_template(Grid *g, int ax, const AuCssBlob *blob, float avail, bool at_least)
{
    g->af_lo[ax] = g->af_hi[ax] = -1;
    if (!blob || blob->value.type == AU_CSSV_COMMA_LIST)
        return;
    uint32_t n;
    const AuCssValue *it = l_items(&blob->value, &n);
    if (n >= 1 && (is_kw(&it[0], AU_K_subgrid) || is_kw(&it[0], AU_K_none)))
        return;
    /* The auto-repeat count. */
    int32_t auto_n = 1;
    {
        float other = 0, group = 0;
        int32_t n_other = 0, n_group = 0;
        bool has_auto = false;
        for (uint32_t i = 0; i < n; i++) {
            const AuCssValue *v = &it[i];
            if (is_line_names(v))
                continue;
            if (v->type == AU_CSSV_FUNCTION && v->kw == AU_K_repeat && v->n == 2) {
                uint32_t m;
                const AuCssValue *sub = l_items(&v->u.items[1], &m);
                float sum = 0;
                int32_t cnt = 0;
                for (uint32_t j = 0; j < m; j++)
                    if (!is_line_names(&sub[j])) {
                        sum += track_estimate(g, &sub[j], avail);
                        cnt++;
                    }
                const AuCssValue *c = &v->u.items[0];
                if (c->type == AU_CSSV_NUMBER) {
                    float cf = c->u.num;
                    if (!(cf >= 1))
                        cf = 1;
                    if (cf > G_MAX_TRACKS)
                        cf = G_MAX_TRACKS;
                    other += cf * sum;
                    n_other += (int32_t)cf * cnt;
                    if (n_other > G_MAX_TRACKS)
                        n_other = G_MAX_TRACKS;
                } else {
                    has_auto = true;
                    group = sum;
                    n_group = cnt;
                }
            } else {
                other += track_estimate(g, v, avail);
                if (n_other < G_MAX_TRACKS)
                    n_other++;
            }
        }
        if (has_auto && n_group > 0 && avail >= 0) {
            float per = group + (float)n_group * g->gap[ax];
            float fixed = other + (float)(n_other - 1) * g->gap[ax];
            /* First repetition: n_other + n_group - 1 gutters in all. */
            if (per < 1)
                per = 1;
            float f = (avail - fixed) / per;
            f = at_least ? ceilf(f) : floorf(f + 0.0001f);
            float cap = (float)(G_MAX_TRACKS - n_other) / (float)n_group;
            if (f > cap)
                f = cap;
            if (!(f >= 1))
                f = 1;
            auto_n = (int32_t)f;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        const AuCssValue *v = &it[i];
        if (is_line_names(v)) {
            add_names(g, ax, v, g->ntpl[ax]);
            continue;
        }
        if (v->type == AU_CSSV_FUNCTION && v->kw == AU_K_repeat && v->n == 2) {
            const AuCssValue *c = &v->u.items[0];
            int32_t count = auto_n;
            bool fit = is_kw(c, AU_K_auto_fit);
            if (c->type == AU_CSSV_NUMBER) {
                float cf = c->u.num;
                count = !(cf >= 1) ? 1 : cf > G_MAX_TRACKS ? G_MAX_TRACKS : (int32_t)cf;
            }
            uint32_t m;
            const AuCssValue *sub = l_items(&v->u.items[1], &m);
            if (fit)
                g->af_lo[ax] = g->ntpl[ax];
            for (int32_t r = 0; r < count && g->ntpl[ax] < G_MAX_TRACKS; r++)
                for (uint32_t j = 0; j < m; j++) {
                    if (is_line_names(&sub[j]))
                        add_names(g, ax, &sub[j], g->ntpl[ax]);
                    else
                        push_def(g, ax, &sub[j]);
                }
            if (fit)
                g->af_hi[ax] = g->ntpl[ax];
            continue;
        }
        push_def(g, ax, v);
    }
}

/* Named areas from grid-template-areas (validated when parsed: every row
 * has the same number of cells and every area is one rectangle). */
static void build_areas(Grid *g, const AuCssBlob *blob)
{
    if (!blob || blob->value.type == AU_CSSV_COMMA_LIST)
        return;
    uint32_t n;
    const AuCssValue *rows = l_items(&blob->value, &n);
    if (n > G_MAX_TRACKS)
        n = G_MAX_TRACKS;
    int32_t *above = NULL; /* the area of the cell above, per column */
    int32_t above_cap = 0, cols = 0;
    g->areas = NULL;
    uint32_t cap = 0;
    for (uint32_t r = 0; r < n; r++) {
        if (rows[r].type != AU_CSSV_STRING)
            break;
        const char *s = rows[r].u.str;
        uint32_t len = rows[r].n, i = 0;
        int32_t c = 0, left = -1;
        while (i < len && c < G_MAX_TRACKS) {
            unsigned char ch = (unsigned char)s[i];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
                i++;
                continue;
            }
            uint32_t st = i;
            int32_t idx = -1;
            if (ch == '.') {
                while (i < len && s[i] == '.')
                    i++;
            } else {
                while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r' && s[i] != '\f' &&
                       s[i] != '.')
                    i++;
                uint32_t nl = i - st;
                int32_t up = c < above_cap && r > 0 ? above[c] : -1;
                if (left >= 0 && g->areas[left].len == nl && !memcmp(g->areas[left].s, s + st, nl)) {
                    idx = left;
                    if (g->areas[idx].hi[0] < c + 1)
                        g->areas[idx].hi[0] = c + 1;
                } else if (up >= 0 && g->areas[up].len == nl && !memcmp(g->areas[up].s, s + st, nl)) {
                    idx = up;
                    g->areas[idx].hi[1] = (int32_t)r + 1;
                } else if (g->nareas < G_MAX_AREAS) {
                    if (g->nareas == cap) {
                        cap = cap ? cap * 2 : 8;
                        g->areas = au_realloc(g->areas, cap * sizeof(GArea));
                    }
                    idx = (int32_t)g->nareas++;
                    GArea *a = &g->areas[idx];
                    a->s = s + st;
                    a->len = nl;
                    a->lo[0] = c;
                    a->hi[0] = c + 1;
                    a->lo[1] = (int32_t)r;
                    a->hi[1] = (int32_t)r + 1;
                }
            }
            if (c >= above_cap) {
                int32_t nc = above_cap ? above_cap * 2 : 16;
                above = au_realloc(above, (size_t)nc * sizeof *above);
                for (int32_t j = above_cap; j < nc; j++)
                    above[j] = -1;
                above_cap = nc;
            }
            above[c] = idx;
            left = idx;
            c++;
        }
        if (c > cols)
            cols = c;
        g->narea[1] = (int32_t)r + 1;
    }
    g->narea[0] = cols;
    au_free(above);
}

static bool ends_with(const char *s, size_t len, const char *suf, size_t sl)
{
    return len > sl && !memcmp(s + len - sl, suf, sl);
}

/* Marks in g->mark the explicit lines (0 .. nexp) named s followed by suf
 * (suf may be empty): names from the track lists, and the implicit
 * NAME-start and NAME-end lines of named areas. Returns how many. */
static int lines_named(Grid *g, int ax, const char *s, size_t len, const char *suf)
{
    size_t sl = strlen(suf);
    int32_t ne = g->nexp[ax];
    memset(g->mark, 0, (size_t)ne + 1);
    for (uint32_t i = 0; i < g->nnames[ax]; i++) {
        const GName *nm = &g->names[ax][i];
        if (nm->len == len + sl && !memcmp(nm->s, s, len) && !memcmp(nm->s + len, suf, sl) && nm->line <= ne)
            g->mark[nm->line] = 1;
    }
    for (uint32_t i = 0; i < g->nareas; i++) {
        const GArea *a = &g->areas[i];
        int which = -1; /* 0 start, 1 end */
        if (sl) {
            if (a->len == len && !memcmp(a->s, s, len))
                which = !strcmp(suf, "-start") ? 0 : 1;
        } else if (ends_with(s, len, "-start", 6) && a->len == len - 6 && !memcmp(a->s, s, a->len)) {
            which = 0;
        } else if (ends_with(s, len, "-end", 4) && a->len == len - 4 && !memcmp(a->s, s, a->len)) {
            which = 1;
        }
        if (which < 0)
            continue;
        int32_t line = which ? a->hi[ax] : a->lo[ax];
        if (line <= ne)
            g->mark[line] = 1;
    }
    int cnt = 0;
    for (int32_t i = 0; i <= ne; i++)
        cnt += g->mark[i];
    return cnt;
}

/* ---- Line resolution (section 8.3) ----
 * Lines are numbered from 0 at the start of the explicit grid; implicit
 * lines before it are negative. */

enum { GP_AUTO, GP_LINE, GP_SPAN };
typedef struct GSide {
    uint8_t kind;
    int32_t v; /* line, or span count */
    const char *name; /* span to a named line */
    size_t nlen;
} GSide;

static int32_t clamp_line(int32_t v)
{
    return v < -G_MAX_TRACKS ? -G_MAX_TRACKS : v > 2 * G_MAX_TRACKS ? 2 * G_MAX_TRACKS : v;
}

static void side_parse(Grid *g, int ax, const AuCssBlob *blob, bool end, GSide *o)
{
    o->kind = GP_AUTO;
    o->v = 0;
    o->name = NULL;
    o->nlen = 0;
    if (!blob || blob->value.type == AU_CSSV_COMMA_LIST)
        return;
    uint32_t n;
    const AuCssValue *it = l_items(&blob->value, &n);
    bool span = false, num = false;
    int32_t iv = 0;
    const char *nm = NULL;
    size_t nl = 0;
    for (uint32_t i = 0; i < n && i < 3; i++) {
        if (is_kw(&it[i], AU_K_auto_))
            return;
        if (is_kw(&it[i], AU_K_span)) {
            span = true;
        } else if (it[i].type == AU_CSSV_NUMBER) {
            float f = it[i].u.num;
            num = true;
            iv = f > G_MAX_TRACKS + 1 ? G_MAX_TRACKS + 1 : f < -G_MAX_TRACKS - 1 ? -G_MAX_TRACKS - 1 : (int32_t)f;
        } else {
            nm = l_val_name(&it[i], &nl);
        }
    }
    if (span) {
        o->kind = GP_SPAN;
        o->v = num && iv > 1 ? iv : 1;
        if (o->v > G_MAX_TRACKS)
            o->v = G_MAX_TRACKS;
        o->name = nm;
        o->nlen = nl;
        return;
    }
    if (!nm && (!num || iv == 0))
        return;
    int32_t ne = g->nexp[ax], line;
    if (nm && !num) {
        /* A named area's edge, else the first line of that name. */
        if (lines_named(g, ax, nm, nl, end ? "-end" : "-start") > 0) {
            for (line = 0; !g->mark[line]; line++) {
            }
            o->kind = GP_LINE;
            o->v = line;
            return;
        }
        num = true;
        iv = 1;
    }
    if (nm) {
        int cnt = lines_named(g, ax, nm, nl, "");
        if (iv > 0) {
            if (cnt >= iv) {
                int seen = 0;
                for (line = 0; line <= ne; line++)
                    if (g->mark[line] && ++seen == iv)
                        break;
            } else {
                /* Implicit lines after the grid all have the name. */
                line = ne + (iv - cnt);
            }
        } else {
            int k = -iv;
            if (cnt >= k) {
                int seen = 0;
                for (line = ne; line >= 0; line--)
                    if (g->mark[line] && ++seen == k)
                        break;
            } else {
                line = -(k - cnt);
            }
        }
    } else {
        line = iv > 0 ? iv - 1 : ne + 1 + iv;
    }
    o->kind = GP_LINE;
    o->v = clamp_line(line);
}

/* The line a span reaches from definite line `from`, searching forward
 * (dir 1) or backward (dir -1). */
static int32_t span_to(Grid *g, int ax, int32_t from, const GSide *sp, int dir)
{
    if (!sp->name)
        return from + dir * sp->v;
    int32_t ne = g->nexp[ax];
    lines_named(g, ax, sp->name, sp->nlen, "");
    int found = 0;
    if (dir > 0) {
        for (int32_t l = from + 1 < 0 ? 0 : from + 1; l <= ne; l++)
            if (g->mark[l] && ++found == sp->v)
                return l;
        return (from > ne ? from : ne) + (sp->v - found);
    }
    for (int32_t l = from - 1 > ne ? ne : from - 1; l >= 0; l--)
        if (g->mark[l] && ++found == sp->v)
            return l;
    return (from < 0 ? from : 0) - (sp->v - found);
}

/* Resolves one axis of a placement: a definite range [lo, hi) in explicit
 * line numbers (returns true), or a span for auto-placement. */
static bool resolve_lines(Grid *g, int ax, const AuCssBlob *bs, const AuCssBlob *be, int32_t *lo, int32_t *hi,
                          int32_t *span)
{
    GSide s, e;
    side_parse(g, ax, bs, false, &s);
    side_parse(g, ax, be, true, &e);
    if (s.kind == GP_SPAN && e.kind == GP_SPAN)
        e.kind = GP_AUTO;
    if (s.kind == GP_LINE && e.kind == GP_LINE) {
        *lo = s.v;
        *hi = e.v;
        if (*lo > *hi) {
            int32_t t = *lo;
            *lo = *hi;
            *hi = t;
        }
        if (*lo == *hi)
            *hi = *lo + 1;
    } else if (s.kind == GP_LINE) {
        *lo = s.v;
        *hi = e.kind == GP_SPAN ? span_to(g, ax, s.v, &e, 1) : s.v + 1;
    } else if (e.kind == GP_LINE) {
        *hi = e.v;
        *lo = s.kind == GP_SPAN ? span_to(g, ax, e.v, &s, -1) : e.v - 1;
    } else {
        const GSide *sp = s.kind == GP_SPAN ? &s : e.kind == GP_SPAN ? &e : NULL;
        *span = sp && !sp->name ? sp->v : 1;
        return false;
    }
    *lo = clamp_line(*lo);
    *hi = clamp_line(*hi);
    if (*hi <= *lo)
        *hi = *lo + 1;
    if (*hi - *lo > G_MAX_TRACKS)
        *hi = *lo + G_MAX_TRACKS;
    *span = *hi - *lo;
    return true;
}

/* ---- Placement (section 8.5) ---- */

/* Occupied cells: one row of G_WORDS words per major-axis track. */
typedef struct GOcc {
    uint32_t *bits;
    int32_t rows;
} GOcc;

static bool occ_free(const GOcc *o, int32_t m0, int32_t m1, int32_t n0, int32_t n1)
{
    if (m1 > o->rows)
        m1 = o->rows;
    int32_t w0 = n0 >> 5, w1 = (n1 - 1) >> 5;
    uint32_t first = 0xFFFFFFFFu << (n0 & 31), last = 0xFFFFFFFFu >> (31 - ((n1 - 1) & 31));
    for (int32_t m = m0; m < m1; m++) {
        const uint32_t *row = o->bits + (size_t)m * G_WORDS;
        for (int32_t w = w0; w <= w1; w++) {
            uint32_t mask = 0xFFFFFFFFu;
            if (w == w0)
                mask &= first;
            if (w == w1)
                mask &= last;
            if (row[w] & mask)
                return false;
        }
    }
    return true;
}

static void occ_set(GOcc *o, int32_t m0, int32_t m1, int32_t n0, int32_t n1)
{
    if (m1 > o->rows) {
        int32_t nr = o->rows ? o->rows : 8;
        while (nr < m1)
            nr *= 2;
        o->bits = au_realloc(o->bits, (size_t)nr * G_WORDS * sizeof(uint32_t));
        memset(o->bits + (size_t)o->rows * G_WORDS, 0, (size_t)(nr - o->rows) * G_WORDS * sizeof(uint32_t));
        o->rows = nr;
    }
    int32_t w0 = n0 >> 5, w1 = (n1 - 1) >> 5;
    uint32_t first = 0xFFFFFFFFu << (n0 & 31), last = 0xFFFFFFFFu >> (31 - ((n1 - 1) & 31));
    for (int32_t m = m0; m < m1; m++) {
        uint32_t *row = o->bits + (size_t)m * G_WORDS;
        for (int32_t w = w0; w <= w1; w++) {
            uint32_t mask = 0xFFFFFFFFu;
            if (w == w0)
                mask &= first;
            if (w == w1)
                mask &= last;
            row[w] |= mask;
        }
    }
}

static void place_items(Grid *g)
{
    bool colflow = (g->fs->grid_auto_flow & AU_GRID_FLOW_COLUMN) != 0;
    bool dense = (g->fs->grid_auto_flow & AU_GRID_FLOW_DENSE) != 0;
    int maj = colflow ? 0 : 1, mn = 1 - maj;
    int32_t cnt[2];
    /* Implicit tracks before the explicit grid shift everything. */
    for (int ax = 0; ax < 2; ax++) {
        int32_t lo = 0, hi = g->nexp[ax];
        for (uint32_t i = 0; i < g->n; i++) {
            GItem *it = &g->it[i];
            if (!it->def[ax])
                continue;
            if (it->lo[ax] < lo)
                lo = it->lo[ax];
            if (it->hi[ax] > hi)
                hi = it->hi[ax];
        }
        int32_t off = -lo;
        if (off > G_MAX_TRACKS - 1)
            off = G_MAX_TRACKS - 1;
        g->off[ax] = off;
        for (uint32_t i = 0; i < g->n; i++) {
            GItem *it = &g->it[i];
            if (it->span[ax] > G_MAX_TRACKS)
                it->span[ax] = G_MAX_TRACKS;
            if (!it->def[ax])
                continue;
            int32_t a = it->lo[ax] + off, b = it->hi[ax] + off;
            if (a < 0)
                a = 0;
            if (a > G_MAX_TRACKS - 1)
                a = G_MAX_TRACKS - 1;
            if (b > G_MAX_TRACKS)
                b = G_MAX_TRACKS;
            if (b <= a)
                b = a + 1;
            it->lo[ax] = a;
            it->hi[ax] = b;
            it->span[ax] = b - a;
        }
        cnt[ax] = hi + off;
        if (cnt[ax] > G_MAX_TRACKS)
            cnt[ax] = G_MAX_TRACKS;
    }
    GOcc o = {NULL, 0};
    /* Items with both positions. */
    for (uint32_t i = 0; i < g->n; i++) {
        GItem *it = &g->it[i];
        if (it->def[0] && it->def[1])
            occ_set(&o, it->lo[maj], it->hi[maj], it->lo[mn], it->hi[mn]);
    }
    /* Items locked to a major-axis position. */
    int32_t *cursor = NULL;
    for (uint32_t i = 0; i < g->n; i++) {
        GItem *it = &g->it[i];
        if (!it->def[maj] || it->def[mn])
            continue;
        int32_t sp = it->span[mn], p = 0;
        if (!dense) {
            if (!cursor)
                cursor = au_calloc((size_t)G_MAX_TRACKS * sizeof *cursor);
            for (int32_t r = it->lo[maj]; r < it->hi[maj]; r++)
                if (cursor[r] > p)
                    p = cursor[r];
        }
        int32_t lim = G_MAX_TRACKS - sp;
        while (p < lim && !occ_free(&o, it->lo[maj], it->hi[maj], p, p + sp))
            p++;
        if (p > lim)
            p = lim;
        it->lo[mn] = p;
        it->hi[mn] = p + sp;
        occ_set(&o, it->lo[maj], it->hi[maj], p, p + sp);
        if (!dense)
            for (int32_t r = it->lo[maj]; r < it->hi[maj]; r++)
                cursor[r] = p + sp;
        if (p + sp > cnt[mn])
            cnt[mn] = p + sp;
    }
    au_free(cursor);
    /* The minor axis holds the widest auto-placed item. */
    for (uint32_t i = 0; i < g->n; i++) {
        GItem *it = &g->it[i];
        if (!it->def[maj] && !it->def[mn] && it->span[mn] > cnt[mn])
            cnt[mn] = it->span[mn];
    }
    if (cnt[mn] < 1)
        cnt[mn] = 1;
    int32_t W = cnt[mn];
    /* The rest, at the cursor. hm, hn: in dense packing, every cell
     * before it is occupied. */
    int32_t cm = 0, cn = 0, hm = 0, hn = 0;
    for (uint32_t i = 0; i < g->n; i++) {
        GItem *it = &g->it[i];
        if (it->def[maj])
            continue;
        int32_t sm = it->span[maj], mlim = G_MAX_TRACKS - sm;
        if (it->def[mn]) {
            if (dense)
                cm = 0;
            else if (it->lo[mn] < cn)
                cm++;
            cn = it->lo[mn];
            while (cm < mlim && !occ_free(&o, cm, cm + sm, it->lo[mn], it->hi[mn]))
                cm++;
            if (cm > mlim)
                cm = mlim;
        } else {
            int32_t sn = it->span[mn];
            if (dense) {
                while (hm < o.rows && !occ_free(&o, hm, hm + 1, hn, hn + 1)) {
                    if (++hn >= W) {
                        hn = 0;
                        hm++;
                    }
                }
                cm = hm;
                cn = hn;
            }
            for (;;) {
                if (cn + sn > W) {
                    cm++;
                    cn = 0;
                }
                if (cm >= mlim) {
                    cm = mlim;
                    break;
                }
                if (occ_free(&o, cm, cm + sm, cn, cn + sn))
                    break;
                cn++;
            }
            it->lo[mn] = cn;
            it->hi[mn] = cn + sn;
        }
        it->lo[maj] = cm;
        it->hi[maj] = cm + sm;
        occ_set(&o, cm, cm + sm, it->lo[mn], it->hi[mn]);
        if (cm + sm > cnt[maj])
            cnt[maj] = cm + sm;
    }
    au_free(o.bits);
    g->nt[0] = cnt[0];
    g->nt[1] = cnt[1];
}

static int ord_cmp(const void *a, const void *b)
{
    const GOrd *x = a, *y = b;
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i ? 1 : 0;
}

/* Tracks of an axis from the template and grid-auto-columns/rows. */
static void make_tracks(Grid *g, int ax)
{
    int32_t n = g->nt[ax];
    const AuCssBlob *ab = ax == 0 ? g->fs->grid_auto_columns : g->fs->grid_auto_rows;
    uint32_t m = 0;
    const AuCssValue *pat = NULL;
    if (ab && ab->value.type != AU_CSSV_COMMA_LIST)
        pat = l_items(&ab->value, &m);
    GTrack *t = au_calloc((size_t)(n ? n : 1) * sizeof *t);
    g->tr[ax] = t;
    for (int32_t i = 0; i < n; i++) {
        int32_t e = i - g->off[ax];
        if (e >= 0 && e < g->ntpl[ax]) {
            t[i].def = g->edef[ax][e];
            t[i].autofit = e >= g->af_lo[ax] && e < g->af_hi[ax];
        } else if (m) {
            int32_t k = e >= 0 ? (e - g->ntpl[ax]) % (int32_t)m : ((e % (int32_t)m) + (int32_t)m) % (int32_t)m;
            t[i].def = &pat[k];
        }
    }
    /* Empty auto-fit tracks collapse. */
    if (g->af_hi[ax] > g->af_lo[ax]) {
        for (int32_t i = 0; i < n; i++)
            t[i].collapsed = t[i].autofit;
        for (uint32_t i = 0; i < g->n; i++) {
            const GItem *it = &g->it[i];
            for (int32_t j = it->lo[ax]; j < it->hi[ax] && j < n; j++)
                t[j].collapsed = false;
        }
    }
    bool later = false;
    for (int32_t i = n - 1; i >= 0; i--) {
        t[i].sep = !t[i].collapsed && later;
        if (!t[i].collapsed)
            later = true;
    }
    /* Items by span. */
    g->byspan[ax] = au_alloc((g->n ? g->n : 1) * sizeof(GOrd));
    for (uint32_t i = 0; i < g->n; i++) {
        g->byspan[ax][i].key = g->it[i].hi[ax] - g->it[i].lo[ax];
        g->byspan[ax][i].i = i;
    }
    qsort(g->byspan[ax], g->n, sizeof(GOrd), ord_cmp);
}

static void grid_free(Grid *g)
{
    for (int ax = 0; ax < 2; ax++) {
        au_free(g->edef[ax]);
        au_free(g->names[ax]);
        au_free(g->tr[ax]);
        au_free(g->byspan[ax]);
    }
    au_free(g->areas);
    au_free(g->it);
    au_free(g->sort);
    au_free(g->mark);
}

/* Builds the grid of b: templates, items in order-modified document
 * order, placement and tracks. aw and ah are the sizes auto-repeat fills
 * (-1: none). g->gap must be set. */
static void grid_build(Grid *g, float aw, bool w_least, float ah, bool h_least)
{
    const AuStyleFlex *fs = g->fs;
    LBlock *b = g->b;
    g->mark = au_alloc(G_MAX_TRACKS + 2);
    build_areas(g, fs->grid_template_areas);
    build_template(g, 0, fs->grid_template_columns, aw, w_least);
    build_template(g, 1, fs->grid_template_rows, ah, h_least);
    for (int ax = 0; ax < 2; ax++)
        g->nexp[ax] = g->ntpl[ax] > g->narea[ax] ? g->ntpl[ax] : g->narea[ax];

    uint32_t n = 0;
    for (LBox *x = b->b.first; x; x = x->next)
        if (x->kind == LK_BLOCK && !(x->flags & LF_ABSPOS))
            n++;
    g->n = n;
    g->it = au_calloc((n ? n : 1) * sizeof(GItem));
    if (n) {
        GOrd *ord = au_alloc(n * sizeof *ord);
        uint32_t i = 0;
        bool sorted = true;
        for (LBox *x = b->b.first; x; x = x->next)
            if (x->kind == LK_BLOCK && !(x->flags & LF_ABSPOS)) {
                g->it[i].k = (LBlock *)x;
                g->it[i].order = lb_anon(x) ? 0 : x->style->flex->order;
                ord[i].key = g->it[i].order;
                ord[i].i = i;
                if (i && ord[i].key < ord[i - 1].key)
                    sorted = false;
                i++;
            }
        if (!sorted) {
            qsort(ord, n, sizeof *ord, ord_cmp);
            GItem *tmp = au_alloc(n * sizeof *tmp);
            for (i = 0; i < n; i++)
                tmp[i] = g->it[ord[i].i];
            au_free(g->it);
            g->it = tmp;
        }
        au_free(ord);
    }
    for (uint32_t i = 0; i < n; i++) {
        GItem *it = &g->it[i];
        it->span[0] = it->span[1] = 1;
        if (lb_anon(&it->k->b))
            continue;
        const AuStyleFlex *kf = it->k->b.style->flex;
        it->def[0] = resolve_lines(g, 0, kf->grid_column_start, kf->grid_column_end, &it->lo[0], &it->hi[0], &it->span[0]);
        it->def[1] = resolve_lines(g, 1, kf->grid_row_start, kf->grid_row_end, &it->lo[1], &it->hi[1], &it->span[1]);
    }
    place_items(g);
    int32_t most = g->nt[0] > g->nt[1] ? g->nt[0] : g->nt[1];
    g->sort = au_alloc((size_t)(most ? most : 1) * sizeof(GSort));
    make_tracks(g, 0);
    make_tracks(g, 1);
    int jc = align_of(fs->justify_content), ac = align_of(fs->align_content);
    g->stretch[0] = jc == AU_ALIGN_NORMAL || jc == AU_ALIGN_STRETCH;
    g->stretch[1] = ac == AU_ALIGN_NORMAL || ac == AU_ALIGN_STRETCH;
}

/* ---- Item contributions ---- */

static bool min_is_auto(const LBlock *k, int ax)
{
    if (lb_anon(&k->b))
        return true;
    const AuStyleBox *s = k->b.style->box;
    return (ax == 0 ? s->min_width.kind : s->min_height.kind) == AU_LEN_AUTO;
}

/* The preferred size is auto or depends on the containing block. */
static bool pref_is_auto(const LBlock *k, int ax)
{
    if (lb_anon(&k->b))
        return true;
    const AuLen *l = ax == 0 ? &k->b.style->box->width : &k->b.style->box->height;
    return l->kind == AU_LEN_AUTO || l_len_pct(l);
}

/* Computes the item's contributions on an axis, once per layout of the
 * container. Rows need it->w. */
static void item_contrib(Grid *g, GItem *it, int ax)
{
    if (it->have[ax])
        return;
    it->have[ax] = true;
    AuLayout *L = g->L;
    LBlock *k = it->k;
    bool clips = (k->b.flags & LF_CLIPS) != 0;
    if (ax == 0) {
        float mn, mx;
        l_outer_intrinsic(L, k, -1, &mn, &mx);
        it->cmin[0] = mn;
        it->cmax[0] = mx;
        it->edge[0] = l_edges_h(k);
        it->cmm[0] = mn;
        if (pref_is_auto(k, 0)) {
            if (min_is_auto(k, 0)) {
                it->min_auto[0] = true;
                if (clips)
                    it->cmm[0] = it->edge[0];
            } else {
                it->cmm[0] = l_clamp_w(k, 0, -1) + it->edge[0];
            }
        }
    } else {
        float mv = k->m[0] + k->m[2];
        float h = l_spec_height(k, -1);
        if (h >= 0)
            h = l_clamp_h(k, h, -1);
        float bh = l_measure_content(L, k, it->w, h, -1);
        it->cmin[1] = it->cmax[1] = bh + mv;
        it->edge[1] = l_bp_v(k) + mv;
        it->cmm[1] = it->cmin[1];
        if (pref_is_auto(k, 1)) {
            if (min_is_auto(k, 1)) {
                it->min_auto[1] = true;
                if (clips)
                    it->cmm[1] = it->edge[1];
            } else {
                it->cmm[1] = l_clamp_h(k, 0, -1) + it->edge[1];
            }
        }
    }
}

/* ---- Track sizing (section 11) ---- */

static float gutters(const Grid *g, int ax, int32_t lo, int32_t hi)
{
    const GTrack *t = g->tr[ax];
    float s = 0;
    for (int32_t i = lo; i + 1 < hi; i++)
        if (t[i].sep)
            s += g->gap[ax];
    return s;
}

/* The sum of the spanned tracks' fixed max sizes with their gutters, or
 * -1 when a track's max sizing function is not fixed. fit: a fit-content
 * limit counts as fixed. */
static float fixed_max(const Grid *g, int ax, const GItem *it, bool fit)
{
    const GTrack *t = g->tr[ax];
    float s = gutters(g, ax, it->lo[ax], it->hi[ax]);
    for (int32_t i = it->lo[ax]; i < it->hi[ax]; i++) {
        if (t[i].max_k != GK_PX && !(fit && t[i].max_k == GK_FIT))
            return -1;
        s += t[i].max_v;
    }
    return s;
}

static bool affects(const GTrack *t, int ph, int mode)
{
    switch (ph) {
    case 0: return t->min_k == GK_AUTO || t->min_k == GK_MINC || t->min_k == GK_MAXC;
    case 1: return t->min_k == GK_MINC || t->min_k == GK_MAXC;
    case 2: return t->min_k == GK_MAXC || (mode == 2 && t->min_k == GK_AUTO);
    case 3: return t->max_k == GK_AUTO || t->max_k == GK_MINC || t->max_k == GK_MAXC || t->max_k == GK_FIT;
    default: return t->max_k == GK_AUTO || t->max_k == GK_MAXC || t->max_k == GK_FIT;
    }
}

/* The contribution an item makes in a phase. mode: 0 layout, 1 sizing
 * under a min-content constraint, 2 under a max-content constraint. */
static float phase_contrib(const Grid *g, int ax, const GItem *it, int ph, int mode)
{
    float fm, v;
    switch (ph) {
    case 0: {
        float m = it->cmm[ax];
        if (it->min_auto[ax]) {
            /* The automatic minimum is zero across flexible tracks, and
             * no more than fixed tracks hold. */
            if (it->flexspan[ax] && it->hi[ax] - it->lo[ax] > 1) {
                m = it->edge[ax];
            } else {
                fm = fixed_max(g, ax, it, false);
                if (fm >= 0 && m > fm)
                    m = l_max(fm, it->edge[ax]);
            }
        }
        if (!mode)
            return m;
        v = it->cmin[ax];
        fm = fixed_max(g, ax, it, true);
        if (fm >= 0 && v > fm)
            v = fm;
        return l_max(v, m);
    }
    case 2:
        v = it->cmax[ax];
        if (mode == 2) {
            fm = fixed_max(g, ax, it, true);
            if (fm >= 0 && v > fm)
                v = l_max(fm, it->cmm[ax]);
        }
        return v;
    case 1:
    case 3:
        return it->cmin[ax];
    default:
        return it->cmax[ax];
    }
}

static int sort_cmp(const void *a, const void *b)
{
    const GSort *x = a, *y = b;
    if (x->room != y->room)
        return x->room < y->room ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i ? 1 : 0;
}

/* Shares space among the n tracks in g->sort, none past its room; the
 * shares are left in room. Returns what did not fit. */
static float water_fill(Grid *g, int32_t n, float space)
{
    if (n > 1)
        qsort(g->sort, (size_t)n, sizeof(GSort), sort_cmp);
    float rem = space;
    for (int32_t j = 0; j < n; j++) {
        float share = rem / (float)(n - j);
        float give = g->sort[j].room < share ? g->sort[j].room : share;
        g->sort[j].room = give;
        rem -= give;
    }
    return rem;
}

/* Distributes an item's extra space to the tracks it spans (11.5.1),
 * into their planned increases. flexpass: only flexible tracks, by flex
 * factor. */
static void dist_item(Grid *g, int ax, const GItem *it, int ph, int mode, bool flexpass)
{
    GTrack *t = g->tr[ax];
    bool lim_phase = ph >= 3;
    int32_t lo = it->lo[ax], hi = it->hi[ax], n = 0;
    float used = gutters(g, ax, lo, hi), fsum = 0;
    for (int32_t i = lo; i < hi; i++) {
        used += lim_phase && t[i].limit < G_INF ? t[i].limit : t[i].base;
        if (t[i].collapsed || !affects(&t[i], ph, mode) || (flexpass && t[i].max_k != GK_FR))
            continue;
        float room;
        if (!lim_phase) {
            float lim = t[i].limit;
            if (t[i].max_k == GK_FIT && t[i].max_v < lim)
                lim = l_max(t[i].max_v, t[i].base);
            room = l_max(0, lim - t[i].base);
        } else if (t[i].limit < G_INF && !t[i].inf_grow) {
            room = 0;
        } else if (t[i].max_k == GK_FIT) {
            room = l_max(0, t[i].max_v - (t[i].limit < G_INF ? t[i].limit : t[i].base));
        } else {
            room = G_INF;
        }
        g->sort[n].room = room;
        g->sort[n].i = i;
        n++;
        fsum += t[i].max_v;
    }
    float space = phase_contrib(g, ax, it, ph, mode) - used;
    if (!(space > 0) || !n)
        return;
    if (flexpass) {
        for (int32_t j = 0; j < n; j++) {
            GTrack *x = &t[g->sort[j].i];
            float inc = fsum > 0 ? space * x->max_v / fsum : space / (float)n;
            if (inc > x->plan)
                x->plan = inc;
        }
        return;
    }
    float rem = water_fill(g, n, space);
    if (rem > 0.0005f) {
        /* Beyond the limits. */
        int32_t nb = 0;
        for (int pass = 0; pass < 2 && !nb; pass++) {
            for (int32_t j = 0; j < n; j++) {
                const GTrack *x = &t[g->sort[j].i];
                bool ok;
                if (pass == 1)
                    ok = !lim_phase;
                else if (lim_phase)
                    ok = x->max_k != GK_FIT;
                else if (ph == 2)
                    ok = x->max_k == GK_MAXC || x->max_k == GK_AUTO || x->max_k == GK_FIT;
                else
                    ok = x->max_k != GK_PX && x->max_k != GK_FR;
                if (ok)
                    nb++;
            }
            if (nb) {
                float add = rem / (float)nb;
                for (int32_t j = 0; j < n; j++) {
                    const GTrack *x = &t[g->sort[j].i];
                    bool ok;
                    if (pass == 1)
                        ok = !lim_phase;
                    else if (lim_phase)
                        ok = x->max_k != GK_FIT;
                    else if (ph == 2)
                        ok = x->max_k == GK_MAXC || x->max_k == GK_AUTO || x->max_k == GK_FIT;
                    else
                        ok = x->max_k != GK_PX && x->max_k != GK_FR;
                    if (ok)
                        g->sort[j].room += add;
                }
            }
        }
    }
    for (int32_t j = 0; j < n; j++) {
        GTrack *x = &t[g->sort[j].i];
        if (g->sort[j].room > x->plan)
            x->plan = g->sort[j].room;
    }
}

static void apply_plans(Grid *g, int ax, int ph)
{
    GTrack *t = g->tr[ax];
    for (int32_t i = 0; i < g->nt[ax]; i++) {
        if (t[i].plan <= 0)
            continue;
        if (ph < 3) {
            t[i].base += t[i].plan;
        } else if (t[i].limit >= G_INF) {
            t[i].limit = t[i].base + t[i].plan;
            if (ph == 3)
                t[i].inf_grow = true;
        } else {
            t[i].limit += t[i].plan;
        }
        t[i].plan = 0;
    }
}

/* The size of an fr for tracks [lo, hi) filling space (11.7.1). */
static float find_fr(Grid *g, int ax, int32_t lo, int32_t hi, float space)
{
    const GTrack *t = g->tr[ax];
    uint8_t *inflex = g->mark;
    memset(inflex + lo, 0, (size_t)(hi - lo));
    float hyp = 0;
    for (int pass = 0; pass < 16; pass++) {
        float left = space - gutters(g, ax, lo, hi), fs = 0;
        for (int32_t i = lo; i < hi; i++) {
            if (t[i].max_k == GK_FR && !inflex[i])
                fs += t[i].max_v;
            else
                left -= t[i].base;
        }
        if (fs < 1)
            fs = 1;
        hyp = left / fs;
        bool again = false;
        for (int32_t i = lo; i < hi; i++)
            if (t[i].max_k == GK_FR && !inflex[i] && hyp * t[i].max_v < t[i].base) {
                inflex[i] = 1;
                again = true;
            }
        if (!again)
            break;
    }
    return hyp > 0 ? hyp : 0;
}

static float tracks_total(const Grid *g, int ax)
{
    const GTrack *t = g->tr[ax];
    float s = 0;
    for (int32_t i = 0; i < g->nt[ax]; i++)
        s += t[i].base + (t[i].sep ? g->gap[ax] : 0);
    return s;
}

/* Sizes the tracks of an axis. avail: the container's content-box size,
 * or -1. mode: 0 layout, 1 min-content, 2 max-content. lim_min, lim_max:
 * the container's min and max size when avail is indefinite. basis: what
 * percentages resolve against (-1: they behave as auto). Returns the sum
 * of the tracks and gutters. */
static float size_axis(Grid *g, int ax, float avail, int mode, float lim_min, float lim_max, float basis)
{
    GTrack *t = g->tr[ax];
    int32_t n = g->nt[ax];
    bool any_flex = false, any_intr = false;
    for (int32_t i = 0; i < n; i++) {
        track_resolve(g, &t[i], basis);
        t[i].base = t[i].min_k == GK_PX ? t[i].min_v : 0;
        t[i].limit = t[i].max_k == GK_PX ? t[i].max_v : G_INF;
        if (t[i].limit < t[i].base)
            t[i].limit = t[i].base;
        t[i].plan = 0;
        t[i].inf_grow = false;
        if (t[i].max_k == GK_FR)
            any_flex = true;
        if (t[i].min_k != GK_PX || (t[i].max_k != GK_PX && t[i].max_k != GK_FR))
            any_intr = true;
    }
    /* Which items matter: those spanning an intrinsic or flexible track. */
    for (uint32_t i = 0; i < g->n; i++) {
        GItem *it = &g->it[i];
        bool flex = false, intr = false;
        for (int32_t j = it->lo[ax]; j < it->hi[ax]; j++) {
            if (t[j].max_k == GK_FR)
                flex = true;
            if (t[j].min_k != GK_PX || (t[j].max_k != GK_PX && t[j].max_k != GK_FR))
                intr = true;
        }
        it->flexspan[ax] = flex;
        if (intr || (flex && avail < 0))
            item_contrib(g, it, ax);
    }
    if (any_intr) {
        const GOrd *ord = g->byspan[ax];
        /* Items that span no flexible track, by span. */
        for (uint32_t a = 0; a < g->n;) {
            uint32_t e = a;
            while (e < g->n && ord[e].key == ord[a].key)
                e++;
            for (int ph = 0; ph < 5; ph++) {
                for (uint32_t j = a; j < e; j++) {
                    const GItem *it = &g->it[ord[j].i];
                    if (!it->flexspan[ax] && it->have[ax])
                        dist_item(g, ax, it, ph, mode, false);
                }
                apply_plans(g, ax, ph);
                if (ph == 2)
                    for (int32_t i = 0; i < n; i++)
                        if (t[i].limit < t[i].base)
                            t[i].limit = t[i].base;
            }
            for (int32_t i = 0; i < n; i++)
                t[i].inf_grow = false;
            a = e;
        }
        /* Items that span a flexible track: base sizes of the flexible
         * tracks only. */
        if (any_flex)
            for (uint32_t a = 0; a < g->n;) {
                uint32_t e = a;
                while (e < g->n && ord[e].key == ord[a].key)
                    e++;
                for (int ph = 0; ph < 3; ph++) {
                    for (uint32_t j = a; j < e; j++) {
                        const GItem *it = &g->it[ord[j].i];
                        if (it->flexspan[ax] && it->have[ax])
                            dist_item(g, ax, it, ph, mode, true);
                    }
                    apply_plans(g, ax, ph);
                }
                a = e;
            }
    }
    for (int32_t i = 0; i < n; i++)
        if (t[i].limit >= G_INF || t[i].limit < t[i].base)
            t[i].limit = t[i].base;
    /* Maximize. */
    if (mode != 1) {
        float target = avail >= 0 ? avail : lim_max;
        if (target >= G_INF / 2) {
            for (int32_t i = 0; i < n; i++)
                t[i].base = t[i].limit;
        } else {
            float free = target - tracks_total(g, ax);
            if (free > 0) {
                for (int32_t i = 0; i < n; i++) {
                    g->sort[i].room = t[i].limit - t[i].base;
                    g->sort[i].i = i;
                }
                water_fill(g, n, free);
                for (int32_t i = 0; i < n; i++)
                    t[g->sort[i].i].base += g->sort[i].room;
            }
        }
    }
    /* Flexible tracks. */
    if (any_flex) {
        float fr = 0;
        if (mode == 1) {
            fr = 0;
        } else if (avail >= 0) {
            fr = find_fr(g, ax, 0, n, avail);
        } else {
            for (int32_t i = 0; i < n; i++)
                if (t[i].max_k == GK_FR)
                    fr = l_max(fr, t[i].max_v > 1 ? t[i].base / t[i].max_v : t[i].base);
            for (uint32_t i = 0; i < g->n; i++) {
                const GItem *it = &g->it[i];
                if (it->flexspan[ax] && it->have[ax])
                    fr = l_max(fr, find_fr(g, ax, it->lo[ax], it->hi[ax], it->cmax[ax]));
            }
            float tot = 0;
            for (int32_t i = 0; i < n; i++) {
                float s = t[i].base;
                if (t[i].max_k == GK_FR && fr * t[i].max_v > s)
                    s = fr * t[i].max_v;
                tot += s + (t[i].sep ? g->gap[ax] : 0);
            }
            if (tot < lim_min)
                fr = find_fr(g, ax, 0, n, lim_min);
            else if (tot > lim_max)
                fr = find_fr(g, ax, 0, n, lim_max);
        }
        for (int32_t i = 0; i < n; i++)
            if (t[i].max_k == GK_FR && fr * t[i].max_v > t[i].base)
                t[i].base = fr * t[i].max_v;
    }
    /* Stretch auto tracks. */
    if (g->stretch[ax] && mode == 0) {
        float space = avail >= 0 ? avail : lim_min;
        float free = space - tracks_total(g, ax);
        int32_t cnt = 0;
        for (int32_t i = 0; i < n; i++)
            if (t[i].max_k == GK_AUTO && !t[i].collapsed)
                cnt++;
        if (free > 0 && cnt)
            for (int32_t i = 0; i < n; i++)
                if (t[i].max_k == GK_AUTO && !t[i].collapsed)
                    t[i].base += free / (float)cnt;
    }
    return tracks_total(g, ax);
}

/* ---- Alignment ---- */

/* Offset before the first of n tracks and extra space between tracks. */
static void distribute(int mode, float free, int32_t n, float *lead, float *between)
{
    *lead = 0;
    *between = 0;
    switch (mode) {
    case AU_ALIGN_END:
    case AU_ALIGN_FLEX_END:
    case AU_ALIGN_RIGHT:
    case AU_ALIGN_SELF_END:
        *lead = free;
        break;
    case AU_ALIGN_CENTER:
        *lead = free / 2;
        break;
    case AU_ALIGN_SPACE_BETWEEN:
        if (n > 1 && free > 0)
            *between = free / (float)(n - 1);
        break;
    case AU_ALIGN_SPACE_AROUND:
        if (free > 0 && n > 0) {
            *between = free / (float)n;
            *lead = *between / 2;
        }
        break;
    case AU_ALIGN_SPACE_EVENLY:
        if (free > 0) {
            *between = free / (float)(n + 1);
            *lead = *between;
        }
        break;
    default:
        break;
    }
}

/* Sets the track positions of an axis in a content box of size inner. */
static void position_axis(Grid *g, int ax, float inner, int align)
{
    GTrack *t = g->tr[ax];
    int32_t n = g->nt[ax], vis = 0;
    for (int32_t i = 0; i < n; i++)
        if (!t[i].collapsed)
            vis++;
    float lead, between;
    distribute(align_of(align), inner - tracks_total(g, ax), vis, &lead, &between);
    float pos = lead;
    for (int32_t i = 0; i < n; i++) {
        t[i].pos = pos;
        pos += t[i].base;
        if (t[i].sep)
            pos += g->gap[ax] + between;
    }
}

static void area_of(const Grid *g, int ax, const GItem *it, float *start, float *size)
{
    const GTrack *t = g->tr[ax];
    int32_t lo = it->lo[ax], hi = it->hi[ax];
    *start = t[lo].pos;
    *size = l_max(0, t[hi - 1].pos + t[hi - 1].base - t[lo].pos);
}

enum { GA_START, GA_END, GA_CENTER, GA_STRETCH };

/* justify-self (ax 0) or align-self (ax 1) of an item. */
static int self_align(const Grid *g, const LBlock *k, int ax)
{
    const AuStyleFlex *kf = lb_anon(&k->b) ? NULL : k->b.style->flex;
    int v = kf ? align_of(ax == 0 ? kf->justify_self : kf->align_self) : AU_ALIGN_AUTO;
    if (v == AU_ALIGN_AUTO)
        v = align_of(ax == 0 ? g->fs->justify_items : g->fs->align_items);
    if (v == AU_ALIGN_AUTO || v == AU_ALIGN_LEGACY_ONLY)
        v = AU_ALIGN_NORMAL;
    if (v == AU_ALIGN_NORMAL) {
        /* Images keep their size, and a box with an aspect ratio takes
         * its height from it (6.2: sized as a block-level box would be);
         * other boxes, form controls among them, fill the area. */
        float r;
        int rk = k->b.replaced;
        bool keeps = (k->b.inner == LI_REPLACED && (rk == LR_IMAGE || rk == LR_SVG || rk == LR_CHECKBOX ||
                                                    rk == LR_RADIO || rk == LR_OTHER)) ||
                     (ax == 1 && l_aspect_ratio(k, &r));
        return keeps ? GA_START : GA_STRETCH;
    }
    switch (v) {
    case AU_ALIGN_STRETCH: return GA_STRETCH;
    case AU_ALIGN_CENTER: return GA_CENTER;
    case AU_ALIGN_END:
    case AU_ALIGN_FLEX_END:
    case AU_ALIGN_SELF_END:
    case AU_ALIGN_RIGHT: return GA_END;
    default: return GA_START; /* start, baseline */
    }
}

static bool margin_auto(const LBlock *k, int edge)
{
    return !lb_anon(&k->b) && k->b.style->surr->margin[edge].kind == AU_LEN_AUTO;
}

static bool size_auto(const LBlock *k, int ax)
{
    if (lb_anon(&k->b))
        return true;
    return (ax == 0 ? k->b.style->box->width.kind : k->b.style->box->height.kind) == AU_LEN_AUTO;
}

/* The item's used content width in an area aw wide. */
static void item_width(Grid *g, GItem *it, float aw)
{
    LBlock *k = it->k;
    l_resolve_box_edges(k, aw);
    bool stretch = self_align(g, k, 0) == GA_STRETCH && size_auto(k, 0) && !margin_auto(k, 1) && !margin_auto(k, 3);
    if (stretch) {
        float w = l_clamp_w(k, l_max(0, aw - k->m[1] - k->m[3] - l_bp_h(k)), aw);
        if (k->b.inner == LI_TABLE) {
            float mn, mx;
            l_intrinsic(g->L, k, &mn, &mx);
            w = l_max(w, mn);
        }
        it->w = w;
    } else {
        it->w = l_used_width(g->L, k, aw, aw, LS_FIT);
    }
}

/* ---- Absolutely positioned children ----
 * A child whose containing block is the grid container and that names
 * grid lines is positioned against the area between them; an auto line is
 * the container's padding edge. The areas wait in L->grid_abs until the
 * container's size is final (l_grid_abspos_area). */

static void abs_area(Grid *g, LBlock *k, float ox, float oy)
{
    AuLayout *L = g->L;
    const AuStyleFlex *kf = k->b.style->flex;
    const AuCssBlob *pl[2][2] = {{kf->grid_column_start, kf->grid_column_end}, {kf->grid_row_start, kf->grid_row_end}};
    LGridAbs a;
    memset(&a, 0, sizeof a);
    a.box = &k->b;
    bool any = false;
    for (int ax = 0; ax < 2; ax++) {
        GSide s, e;
        side_parse(g, ax, pl[ax][0], false, &s);
        side_parse(g, ax, pl[ax][1], true, &e);
        const GTrack *t = g->tr[ax];
        int32_t n = g->nt[ax];
        float org = ax == 0 ? ox : oy;
        for (int side = 0; side < 2; side++) {
            const GSide *p = side ? &e : &s;
            int32_t line = p->v + g->off[ax];
            if (p->kind != GP_LINE || line < 0 || line > n || n == 0)
                continue; /* auto: the padding edge */
            float pos;
            if (side == 0)
                pos = line < n ? t[line].pos : t[n - 1].pos + t[n - 1].base;
            else
                pos = line > 0 ? t[line - 1].pos + t[line - 1].base : t[0].pos;
            a.edge[ax * 2 + side] = org + pos;
            a.set |= (uint8_t)(1u << (ax * 2 + side));
            any = true;
        }
        /* Lines in the wrong order: the start line wins. */
        if ((a.set >> (ax * 2) & 3) == 3 && a.edge[ax * 2 + 1] < a.edge[ax * 2])
            a.edge[ax * 2 + 1] = a.edge[ax * 2];
    }
    for (uint32_t i = 0; i < L->ngrid_abs; i++)
        if (L->grid_abs[i].box == &k->b) {
            if (any)
                L->grid_abs[i] = a;
            else
                L->grid_abs[i] = L->grid_abs[--L->ngrid_abs];
            return;
        }
    if (!any)
        return;
    if (L->ngrid_abs == L->grid_abs_cap) {
        L->grid_abs_cap = L->grid_abs_cap ? L->grid_abs_cap * 2 : 8;
        L->grid_abs = au_realloc(L->grid_abs, L->grid_abs_cap * sizeof *L->grid_abs);
    }
    L->grid_abs[L->ngrid_abs++] = a;
}

void l_grid_abspos_area(AuLayout *L, LBlock *b, LArea *r)
{
    for (uint32_t i = 0; i < L->ngrid_abs; i++) {
        if (L->grid_abs[i].box != &b->b)
            continue;
        LGridAbs a = L->grid_abs[i];
        L->grid_abs[i] = L->grid_abs[--L->ngrid_abs];
        float x0 = a.set & 1 ? a.edge[0] : r->x, x1 = a.set & 2 ? a.edge[1] : r->x + r->w;
        float y0 = a.set & 4 ? a.edge[2] : r->y, y1 = a.set & 8 ? a.edge[3] : r->y + r->h;
        r->x = x0;
        r->y = y0;
        r->w = l_max(0, x1 - x0);
        r->h = l_max(0, y1 - y0);
        return;
    }
}

/* ---- Entry points ---- */

static void grid_init(Grid *g, AuLayout *L, LBlock *b)
{
    memset(g, 0, sizeof *g);
    g->L = L;
    g->b = b;
    g->fs = b->b.style->flex;
}

float l_grid_layout(AuLayout *L, LBlock *b, const LConstraints *c)
{
    Grid g;
    grid_init(&g, L, b);
    const AuStyleFlex *fs = g.fs;
    float cw = c->avail_w, ch = c->avail_h;
    float ox = b->bw[3] + b->p[3], oy = b->bw[0] + b->p[0];
    float hmin = l_clamp_h(b, 0, -1), hmax = l_clamp_h(b, G_INF, -1);
    g.gap[0] = gap_px(&fs->column_gap, cw);
    g.gap[1] = gap_px(&fs->row_gap, ch);
    /* The height repeat(auto-fill) rows fill: the height, else the
     * max-height, else at least the min-height. */
    float ah = ch;
    bool h_least = false;
    if (ah < 0) {
        if (hmax < G_INF / 2) {
            ah = hmax;
        } else if (hmin > 0) {
            ah = hmin;
            h_least = true;
        }
    }
    grid_build(&g, cw, false, ah, h_least);

    for (LBox *x = b->b.first; x; x = x->next)
        if (x->kind == LK_BLOCK && (x->flags & LF_ABSPOS)) {
            l_resolve_box_edges((LBlock *)x, cw);
            l_queue_abspos(L, (LBlock *)x, ox, oy);
        }
    b->baseline = b->last_baseline = NAN;

    /* Columns, then each item's width. */
    size_axis(&g, 0, cw, 0, cw, cw, cw);
    position_axis(&g, 0, cw, fs->justify_content);
    for (uint32_t i = 0; i < g.n; i++) {
        float ax, aw;
        area_of(&g, 0, &g.it[i], &ax, &aw);
        item_width(&g, &g.it[i], aw);
    }

    /* Rows. With an indefinite height, percentage rows and gaps behave as
     * auto while the height is found, then resolve against it. */
    g.pct_indef = false;
    float total = size_axis(&g, 1, ch, 0, ch >= 0 ? ch : hmin, ch >= 0 ? ch : hmax, ch);
    float inner_h = ch >= 0 ? ch : l_clamp_h(b, total, -1);
    if (ch < 0 && (g.pct_indef || l_len_pct(&fs->row_gap))) {
        g.gap[1] = gap_px(&fs->row_gap, inner_h);
        size_axis(&g, 1, inner_h, 0, inner_h, inner_h, inner_h);
    }
    position_axis(&g, 1, inner_h, fs->align_content);

    /* Items. */
    const GItem *first = NULL, *last = NULL;
    for (uint32_t i = 0; i < g.n; i++) {
        GItem *it = &g.it[i];
        LBlock *k = it->k;
        float ax, aw, ay, ahh;
        area_of(&g, 0, it, &ax, &aw);
        area_of(&g, 1, it, &ay, &ahh);
        int ja = self_align(&g, k, 0), aa = self_align(&g, k, 1);
        bool mt = margin_auto(k, 0), mr = margin_auto(k, 1), mb = margin_auto(k, 2), ml = margin_auto(k, 3);
        float w = it->w;
        float h = l_spec_height(k, ahh);
        if (h >= 0)
            h = l_clamp_h(k, h, ahh);
        bool stretch_v = aa == GA_STRETCH && size_auto(k, 1) && !mt && !mb;
        if (stretch_v)
            h = l_clamp_h(k, l_max(0, ahh - k->m[0] - k->m[2] - l_bp_v(k)), ahh);
        l_layout_content(L, k, w, h, ahh);
        float fx = aw - (k->w + k->m[1] + k->m[3]), fy = ahh - (k->h + k->m[0] + k->m[2]);
        float dx = 0, dy = 0;
        if (ml || mr) {
            if (fx > 0)
                dx = ml && mr ? fx / 2 : ml ? fx : 0;
        } else if (ja == GA_END) {
            dx = fx;
        } else if (ja == GA_CENTER) {
            dx = fx / 2;
        }
        if (mt || mb) {
            if (fy > 0)
                dy = mt && mb ? fy / 2 : mt ? fy : 0;
        } else if (aa == GA_END) {
            dy = fy;
        } else if (aa == GA_CENTER) {
            dy = fy / 2;
        }
        k->x = ox + ax + k->m[3] + dx;
        k->y = oy + ay + k->m[0] + dy;
        if (!first || it->lo[1] < first->lo[1] || (it->lo[1] == first->lo[1] && it->lo[0] < first->lo[0]))
            first = it;
        if (!last || it->hi[1] > last->hi[1] || (it->hi[1] == last->hi[1] && it->lo[0] < last->lo[0]))
            last = it;
    }
    if (first) {
        const LBlock *k = first->k;
        b->baseline = k->y + (isnan(k->baseline) ? k->h : k->baseline);
        k = last->k;
        b->last_baseline = k->y + (isnan(k->last_baseline) ? k->h : k->last_baseline);
    }

    for (LBox *x = b->b.first; x; x = x->next)
        if (x->kind == LK_BLOCK && (x->flags & LF_ABSPOS) && ((LBlock *)x)->cb == &b->b)
            abs_area(&g, (LBlock *)x, ox, oy);

    grid_free(&g);
    return inner_h;
}

/* Intrinsic widths: the columns sized under a min-content and under a
 * max-content constraint. Percentage tracks and gaps behave as auto. */
void l_grid_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    Grid g;
    grid_init(&g, L, b);
    /* The width repeat(auto-fill) columns fill: a definite width, else
     * the max-width, else at least the min-width, else one repetition. */
    float aw = l_spec_width(b, -1);
    bool least = false;
    if (aw < 0) {
        float mx = l_clamp_w(b, G_INF, -1), mn = l_clamp_w(b, 0, -1);
        if (mx < G_INF / 2) {
            aw = mx;
        } else if (mn > 0) {
            aw = mn;
            least = true;
        }
    }
    g.gap[0] = gap_px(&g.fs->column_gap, -1);
    g.gap[1] = 0;
    grid_build(&g, aw, least, -1, false);
    *min_c = size_axis(&g, 0, -1, 1, 0, G_INF, -1);
    *max_c = size_axis(&g, 0, -1, 2, 0, G_INF, -1);
    grid_free(&g);
}
