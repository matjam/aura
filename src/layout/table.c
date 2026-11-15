/* CSS table layout: the slot grid (HTML "forming a table": colspan,
 * rowspan including rowspan=0, header and footer groups), the automatic
 * table layout with the CSS Tables 3 width distribution as Chrome
 * implements it, the fixed table layout, row heights with baseline
 * alignment and rowspan distribution, the separated and collapsed border
 * models (CSS 2.1 17.6), captions, and visibility: collapse.
 *
 * Where the specification leaves the behaviour open, this follows Chrome:
 * percentages of columns are capped so they add up to at most 100% in
 * column order; a percentage column inflates the max-content width of an
 * auto-width table (max-content / percentage); columns that only <col>
 * elements define do not count; a collapsed column takes its width and
 * the spacing before it away; rowspan and table height excess goes to the
 * rows in proportion to their heights (table height only to rows outside
 * the header and footer groups); collapsed borders size cells by half of
 * their own segments and the table by half of the widest outer segment.
 *
 * Cost: building the grid and every pass over it are linear in the number
 * of slots; spanning cells are sorted once per pass (n log n). Intrinsic
 * widths are cached per box (l_intrinsic), and the column constraints per
 * table with them, so nested tables are measured once per layout. */
#include "layout_int.h"

#include <stdlib.h>

#define MAX_COLS 10000
#define MAX_COLSPAN 1000
#define MAX_ROWSPAN 65534

enum { CF_FIXED = 1, CF_PCT = 2, CF_COLLAPSED = 4 };

/* ---- Small helpers ---- */

LTableBox *l_table_of(const LBox *b)
{
    for (LBox *p = b->parent; p; p = p->parent) {
        if (p->kind != LK_BLOCK)
            return NULL;
        if (p->inner == LI_TABLE)
            return (LTableBox *)p;
        if (p->inner != LI_TABLE_ROW && p->inner != LI_TABLE_ROW_GROUP && p->inner != LI_TABLE_COLUMN_GROUP)
            return NULL;
    }
    return NULL;
}

bool l_table_collapsed(const LTableBox *t)
{
    return t->k.b.style->inhr->border_collapse == AU_BORDER_COLLAPSE_COLLAPSE;
}

static float spacing_h(const LTableBox *t) { return l_table_collapsed(t) ? 0 : t->k.b.style->inhr->border_spacing[0]; }
static float spacing_v(const LTableBox *t) { return l_table_collapsed(t) ? 0 : t->k.b.style->inhr->border_spacing[1]; }

static bool is_quirks(const AuLayout *L) { return L->doc && L->doc->quirks == AU_QUIRKS_FULL; }

static long int_attr(const LBox *b, const char *name, long dflt, long lo, long hi)
{
    if (lb_anon(b))
        return dflt;
    AuAtom a = au_atom_lookup(name, strlen(name));
    const char *v = a ? l_attr(b, a) : NULL;
    if (!v)
        return dflt;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r' || *v == '\f')
        v++;
    if (*v < '0' || *v > '9')
        return dflt;
    long n = 0;
    while (*v >= '0' && *v <= '9' && n < 100000000)
        n = n * 10 + (*v++ - '0');
    return n < lo ? lo : n > hi ? hi : n;
}

static bool is_collapse_vis(const LBox *b)
{
    return b && !lb_anon(b) && b->style->inh->visibility == AU_VISIBILITY_COLLAPSE;
}

static int group_kind(const LBox *g)
{
    return au_display_inner(g->style->box->display);
}

/* ---- The slot grid ---- */

typedef struct GridBuild {
    LTableGrid *g;
    uint32_t *cover; /* per column: the row index below the last row an earlier cell covers */
    uint32_t cover_cap, ncols, nrows, ncells;
} GridBuild;

static void need_cover(GridBuild *G, uint32_t n)
{
    if (n <= G->cover_cap)
        return;
    uint32_t cap = G->cover_cap ? G->cover_cap : 16;
    while (cap < n)
        cap *= 2;
    G->cover = au_realloc(G->cover, cap * sizeof *G->cover);
    memset(G->cover + G->cover_cap, 0, (cap - G->cover_cap) * sizeof *G->cover);
    G->cover_cap = cap;
}

static bool is_row(const LBox *c) { return c->kind == LK_BLOCK && c->inner == LI_TABLE_ROW; }
static bool is_cell(const LBox *c) { return c->kind == LK_BLOCK && c->inner == LI_TABLE_CELL; }

/* Places the rows of one row group (or of a run of rows directly in the
 * table, when group is NULL) starting at row box first. */
static void place_section(GridBuild *G, LBox *first, LBlock *group, uint8_t rflags)
{
    LTableGrid *g = G->g;
    uint32_t n = 0;
    for (LBox *r = first; r; r = r->next) {
        if (is_row(r))
            n++;
        else if (!group && r->kind == LK_BLOCK && r->inner == LI_TABLE_ROW_GROUP)
            break;
    }
    uint32_t start = G->nrows, end = start + n;
    bool gcol = is_collapse_vis(group ? &group->b : NULL);
    for (LBox *r = first; r && G->nrows < end; r = r->next) {
        if (!is_row(r))
            continue;
        uint32_t ri = G->nrows++;
        LTableRow *row = &g->rows[ri];
        row->box = (LBlock *)r;
        row->group = group;
        row->first = G->ncells;
        row->flags = rflags | (gcol || is_collapse_vis(r) ? LTR_COLLAPSED : 0);
        uint32_t x = 0;
        for (LBox *c = r->first; c; c = c->next) {
            if (!is_cell(c))
                continue;
            LCellBox *cb = (LCellBox *)c;
            while (x < G->ncols && G->cover[x] > ri)
                x++;
            if (x >= MAX_COLS) {
                cb->colspan = 0; /* not in the grid: not laid out or painted */
                continue;
            }
            uint32_t cs = (uint32_t)int_attr(c, "colspan", 1, 1, MAX_COLSPAN);
            if (x + cs > MAX_COLS)
                cs = MAX_COLS - x;
            long rs = int_attr(c, "rowspan", 1, 0, MAX_ROWSPAN);
            uint32_t span = rs == 0 || (uint32_t)rs > end - ri ? end - ri : (uint32_t)rs;
            cb->row = ri;
            cb->rowspan = span;
            cb->col = (uint16_t)x;
            cb->colspan = (uint16_t)cs;
            need_cover(G, x + cs);
            for (uint32_t i = x; i < x + cs; i++)
                if (G->cover[i] < ri + span)
                    G->cover[i] = ri + span;
            if (x + cs > G->ncols)
                G->ncols = x + cs;
            g->cells[G->ncells++] = cb;
            x += cs;
        }
        row->n = G->ncells - row->first;
    }
}

void l_table_build_grid(AuLayout *L, LTableBox *t)
{
    LArena *A = &L->tree;
    LTableGrid *g = ar_alloc(A, sizeof *g);
    t->g = g;
    uint32_t nrows = 0, ncells = 0;
    LBox *head = NULL, *foot = NULL;
    for (LBox *c = t->k.b.first; c; c = c->next) {
        if (c->kind != LK_BLOCK)
            continue;
        if (c->inner == LI_TABLE_ROW_GROUP) {
            int k = group_kind(c);
            if (!lb_anon(c) && k == AU_DISPLAY_INNER_TABLE_HEADER_GROUP && !head)
                head = c;
            else if (!lb_anon(c) && k == AU_DISPLAY_INNER_TABLE_FOOTER_GROUP && !foot)
                foot = c;
            for (LBox *r = c->first; r; r = r->next)
                if (is_row(r)) {
                    nrows++;
                    for (LBox *x = r->first; x; x = x->next)
                        ncells += is_cell(x);
                }
        } else if (is_row(c)) {
            nrows++;
            for (LBox *x = c->first; x; x = x->next)
                ncells += is_cell(x);
        }
    }
    g->rows = nrows ? ar_alloc(A, nrows * sizeof *g->rows) : NULL;
    g->cells = ncells ? ar_alloc(A, ncells * sizeof *g->cells) : NULL;
    GridBuild G = {g, NULL, 0, 0, 0, 0};
    if (head)
        place_section(&G, head->first, (LBlock *)head, LTR_HEADFOOT);
    for (LBox *c = t->k.b.first; c; c = c->next) {
        if (c->kind != LK_BLOCK || c == head || c == foot)
            continue;
        if (c->inner == LI_TABLE_ROW_GROUP) {
            place_section(&G, c->first, (LBlock *)c, 0);
        } else if (is_row(c)) {
            /* place_section takes the rows up to the next row group; skip
             * the same run (captions and columns in it included). */
            place_section(&G, c, NULL, 0);
            while (c->next && !(c->next->kind == LK_BLOCK && c->next->inner == LI_TABLE_ROW_GROUP))
                c = c->next;
        }
    }
    if (foot)
        place_section(&G, foot->first, (LBlock *)foot, LTR_HEADFOOT);
    au_free(G.cover);
    g->nrows = G.nrows;
    g->ncells = G.ncells;
    g->ncols = G.ncols;
    g->cols = g->ncols ? ar_alloc(A, g->ncols * sizeof *g->cols) : NULL;
    g->colflags = g->ncols ? ar_alloc(A, g->ncols) : NULL;
    /* Columns and column groups name the grid's columns in order; those
     * beyond the last cell do not add columns (as in Chrome). */
    uint32_t ci = 0;
    for (LBox *c = t->k.b.first; c && ci < g->ncols; c = c->next) {
        if (c->kind != LK_BLOCK)
            continue;
        if (c->inner == LI_TABLE_COLUMN) {
            uint32_t span = (uint32_t)int_attr(c, "span", 1, 1, MAX_COLSPAN);
            for (uint32_t i = 0; i < span && ci < g->ncols; i++, ci++)
                g->cols[ci] = (LTableCol){(LBlock *)c, NULL};
        } else if (c->inner == LI_TABLE_COLUMN_GROUP) {
            if (c->first) {
                for (LBox *k = c->first; k && ci < g->ncols; k = k->next) {
                    uint32_t span = (uint32_t)int_attr(k, "span", 1, 1, MAX_COLSPAN);
                    for (uint32_t i = 0; i < span && ci < g->ncols; i++, ci++)
                        g->cols[ci] = (LTableCol){(LBlock *)k, (LBlock *)c};
                }
            } else {
                uint32_t span = (uint32_t)int_attr(c, "span", 1, 1, MAX_COLSPAN);
                for (uint32_t i = 0; i < span && ci < g->ncols; i++, ci++)
                    g->cols[ci] = (LTableCol){NULL, (LBlock *)c};
            }
        }
    }
    for (uint32_t i = 0; i < g->ncols; i++)
        if (is_collapse_vis(g->cols[i].col ? &g->cols[i].col->b : NULL) ||
            is_collapse_vis(g->cols[i].group ? &g->cols[i].group->b : NULL))
            g->colflags[i] |= LTR_COLLAPSED;
}

void l_table_free(LTableBox *t)
{
    au_free(t->cmin);
    au_free(t->hedge);
    au_free(t->vedge);
    t->cmin = t->cmax = t->cpct = t->colx = t->colw = t->rowy = t->rowh = NULL;
    t->cfl = NULL;
    t->hedge = t->vedge = NULL;
    t->ncap = 0;
    t->edges_ok = false;
}

/* The per-column and per-row arrays, in one allocation. */
static void ensure_arrays(LTableBox *t)
{
    if (t->ncap)
        return;
    uint32_t nc = t->g->ncols, nr = t->g->nrows;
    size_t n = 5 * (size_t)nc + 2 * (size_t)nr + 1;
    float *f = au_calloc(n * sizeof(float) + nc + 1);
    t->cmin = f;
    t->cmax = f + nc;
    t->cpct = f + 2 * nc;
    t->colx = f + 3 * nc;
    t->colw = f + 4 * nc;
    t->rowy = f + 5 * nc;
    t->rowh = f + 5 * nc + nr;
    t->cfl = (uint8_t *)(f + n);
    t->ncap = 1;
}

/* ---- Collapsed borders (CSS 2.1 17.6.2.1) ---- */

static int style_rank(int st)
{
    switch (st) {
    case AU_BORDER_STYLE_DOUBLE: return 8;
    case AU_BORDER_STYLE_SOLID: return 7;
    case AU_BORDER_STYLE_DASHED: return 6;
    case AU_BORDER_STYLE_DOTTED: return 5;
    case AU_BORDER_STYLE_RIDGE: return 4;
    case AU_BORDER_STYLE_OUTSET: return 3;
    case AU_BORDER_STYLE_GROOVE: return 2;
    case AU_BORDER_STYLE_INSET: return 1;
    default: return 0;
    }
}

/* Offers side i of box b to edge e. Boxes are offered in precedence
 * order (cells in row order first, then rows, row groups, columns, column
 * groups, the table), so on a full tie the earlier offer stays. */
static void offer(LTableEdge *e, const LBox *b, int i, uint8_t prec)
{
    if (!b || lb_anon(b) || e->style == AU_BORDER_STYLE_HIDDEN)
        return;
    const AuStyleSurround *s = b->style->surr;
    int st = s->border_style[i];
    if (st == AU_BORDER_STYLE_HIDDEN) {
        e->style = AU_BORDER_STYLE_HIDDEN;
        e->w = 0;
        e->prec = prec;
        return;
    }
    float w = s->border_width[i];
    if (st == AU_BORDER_STYLE_NONE || w <= 0)
        return;
    if (e->prec == 0 || w > e->w || (w == e->w && style_rank(st) > style_rank(e->style))) {
        e->w = w;
        e->style = (uint8_t)st;
        e->color = s->border_color[i];
        e->prec = prec;
    }
}

static void compute_edges(LTableBox *t)
{
    LTableGrid *g = t->g;
    uint32_t nc = g->ncols, nr = g->nrows;
    size_t nh = (size_t)(nr + 1) * nc, nv = (size_t)nr * (nc + 1);
    au_free(t->hedge);
    au_free(t->vedge);
    t->hedge = nh ? au_calloc(nh * sizeof *t->hedge) : NULL;
    t->vedge = nv ? au_calloc(nv * sizeof *t->vedge) : NULL;
    LTableEdge *H = t->hedge, *V = t->vedge;
#define HE(r, c) H[(size_t)(r) * nc + (c)]
#define VE(r, c) V[(size_t)(r) * (nc + 1) + (c)]
    for (uint32_t i = 0; i < g->ncells; i++) {
        LCellBox *cb = g->cells[i];
        const LBox *b = &cb->k.b;
        for (uint32_t c = cb->col; c < (uint32_t)cb->col + cb->colspan; c++) {
            offer(&HE(cb->row, c), b, 0, 6);
            offer(&HE(cb->row + cb->rowspan, c), b, 2, 6);
        }
        for (uint32_t r = cb->row; r < cb->row + cb->rowspan; r++) {
            offer(&VE(r, cb->col), b, 3, 6);
            offer(&VE(r, cb->col + cb->colspan), b, 1, 6);
        }
    }
    for (uint32_t r = 0; r < nr && nc; r++) {
        const LBox *b = &g->rows[r].box->b;
        for (uint32_t c = 0; c < nc; c++) {
            offer(&HE(r, c), b, 0, 5);
            offer(&HE(r + 1, c), b, 2, 5);
        }
        offer(&VE(r, 0), b, 3, 5);
        offer(&VE(r, nc), b, 1, 5);
    }
    for (uint32_t r = 0; r < nr && nc;) {
        LBlock *grp = g->rows[r].group;
        uint32_t e = r + 1;
        while (e < nr && g->rows[e].group == grp)
            e++;
        if (grp) {
            for (uint32_t c = 0; c < nc; c++) {
                offer(&HE(r, c), &grp->b, 0, 4);
                offer(&HE(e, c), &grp->b, 2, 4);
            }
            for (uint32_t k = r; k < e; k++) {
                offer(&VE(k, 0), &grp->b, 3, 4);
                offer(&VE(k, nc), &grp->b, 1, 4);
            }
        }
        r = e;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t c = 0; c < nc;) {
            LBlock *cbx = pass == 0 ? g->cols[c].col : g->cols[c].group;
            uint32_t e = c + 1;
            while (e < nc && (pass == 0 ? g->cols[e].col : g->cols[e].group) == cbx)
                e++;
            if (cbx) {
                uint8_t prec = pass == 0 ? 3 : 2;
                for (uint32_t r = 0; r < nr; r++) {
                    offer(&VE(r, c), &cbx->b, 3, prec);
                    offer(&VE(r, e), &cbx->b, 1, prec);
                }
                for (uint32_t k = c; k < e; k++) {
                    offer(&HE(0, k), &cbx->b, 0, prec);
                    offer(&HE(nr, k), &cbx->b, 2, prec);
                }
            }
            c = e;
        }
    }
    const LBox *tb = &t->k.b;
    for (uint32_t c = 0; c < nc; c++) {
        offer(&HE(0, c), tb, 0, 1);
        offer(&HE(nr, c), tb, 2, 1);
    }
    for (uint32_t r = 0; r < nr; r++) {
        offer(&VE(r, 0), tb, 3, 1);
        offer(&VE(r, nc), tb, 1, 1);
    }
    float m[4] = {0, 0, 0, 0};
    for (uint32_t c = 0; c < nc; c++) {
        m[0] = l_max(m[0], HE(0, c).w);
        m[2] = l_max(m[2], HE(nr, c).w);
    }
    for (uint32_t r = 0; r < nr; r++) {
        m[3] = l_max(m[3], VE(r, 0).w);
        m[1] = l_max(m[1], VE(r, nc).w);
    }
    if (!nc || !nr) {
        /* No grid: the table's own borders. */
        for (int i = 0; i < 4; i++)
            m[i] = lb_anon(tb) || tb->style->surr->border_style[i] == AU_BORDER_STYLE_HIDDEN
                       ? 0
                       : tb->style->surr->border_width[i];
    }
    for (int i = 0; i < 4; i++)
        t->cbw[i] = m[i] / 2;
#undef HE
#undef VE
    t->edges_ok = true;
}

static void ensure_edges(LTableBox *t)
{
    if (!t->edges_ok && t->g)
        compute_edges(t);
}

/* Half the widest segment along each side of cell cb. */
static void cell_collapsed_bw(LTableBox *t, const LCellBox *cb, float *bw)
{
    LTableGrid *g = t->g;
    uint32_t nc = g->ncols;
    float m[4] = {0, 0, 0, 0};
    for (uint32_t c = cb->col; c < (uint32_t)cb->col + cb->colspan; c++) {
        m[0] = l_max(m[0], t->hedge[(size_t)cb->row * nc + c].w);
        m[2] = l_max(m[2], t->hedge[(size_t)(cb->row + cb->rowspan) * nc + c].w);
    }
    for (uint32_t r = cb->row; r < cb->row + cb->rowspan; r++) {
        m[3] = l_max(m[3], t->vedge[(size_t)r * (nc + 1) + cb->col].w);
        m[1] = l_max(m[1], t->vedge[(size_t)r * (nc + 1) + cb->col + cb->colspan].w);
    }
    for (int i = 0; i < 4; i++)
        bw[i] = m[i] / 2;
}

void l_table_box_edges(LBlock *b, float cb_w)
{
    memset(b->m, 0, sizeof b->m);
    memset(b->p, 0, sizeof b->p);
    memset(b->bw, 0, sizeof b->bw);
    int in = b->b.inner;
    if (in != LI_TABLE && in != LI_TABLE_CELL)
        return; /* rows, row groups and columns have no box edges of their own */
    if (!lb_anon(&b->b)) {
        const AuStyleSurround *s = b->b.style->surr;
        for (int i = 0; i < 4; i++) {
            if (in == LI_TABLE)
                b->m[i] = s->margin[i].kind == AU_LEN_AUTO ? 0 : l_len(&s->margin[i], cb_w);
            b->p[i] = l_max(0, l_len(&s->padding[i], cb_w));
            b->bw[i] = s->border_width[i];
        }
    }
    LTableBox *t = in == LI_TABLE ? (LTableBox *)b : l_table_of(&b->b);
    if (!t || !t->g || !l_table_collapsed(t))
        return;
    ensure_edges(t);
    if (in == LI_TABLE) {
        memset(b->p, 0, sizeof b->p); /* no padding in the collapsing model */
        memcpy(b->bw, t->cbw, sizeof b->bw);
    } else if (((LCellBox *)b)->colspan) {
        cell_collapsed_bw(t, (LCellBox *)b, b->bw);
    }
}

/* ---- Column widths ---- */

typedef struct CellCon {
    float mn, mx, pct;
    bool fixed, has_pct;
} CellCon;

static bool is_fixed_layout(const LTableBox *t)
{
    const LBox *b = &t->k.b;
    if (lb_anon(b))
        return false;
    const AuStyleBox *s = b->style->box;
    return s->table_layout == AU_TABLE_LAYOUT_FIXED && au_len_is_definite_kind(&s->width);
}

/* A width-like length as a border-box width of b, or -1 (auto, or a
 * percentage: reported in *pct). */
static float bb_width(const LBlock *b, const AuLen *l, float *pct, bool *has_pct)
{
    if (l->kind == AU_LEN_PCT) {
        if (pct) {
            *pct = l->u.pct;
            *has_pct = true;
        }
        return -1;
    }
    if (!au_len_is_definite_kind(l) || l_len_pct(l))
        return -1;
    float v = l_len(l, 0);
    if (b->b.style->box->box_sizing != AU_BOX_SIZING_BORDER_BOX)
        v += l_bp_h(b);
    return l_max(0, v);
}

/* The min-content, max-content, percentage and fixedness of a cell (CSS
 * Tables 3 "cell min/max", Chrome's ComputeCellInlineConstraint). */
static void cell_constraint(AuLayout *L, LCellBox *cb, CellCon *c)
{
    LBlock *k = &cb->k;
    l_resolve_box_edges(k, 0);
    float mn, mx;
    l_intrinsic(L, k, &mn, &mx);
    float bp = l_bp_h(k);
    c->mn = mn + bp;
    c->mx = mx + bp;
    c->pct = 0;
    c->fixed = c->has_pct = false;
    if (lb_anon(&k->b))
        return;
    const AuStyleBox *s = k->b.style->box;
    float w = bb_width(k, &s->width, &c->pct, &c->has_pct);
    float mnw = bb_width(k, &s->min_width, NULL, NULL), mxw = bb_width(k, &s->max_width, NULL, NULL);
    if (mnw >= 0)
        c->mn = l_max(c->mn, mnw);
    /* The table cell nowrap minimum width quirk. */
    if (w >= 0 && is_quirks(L)) {
        AuAtom a = au_atom_lookup("nowrap", 6);
        if (a && l_attr(&k->b, a))
            c->mn = l_max(c->mn, w);
    }
    float content_max = w >= 0 ? w : c->mx;
    if (mxw >= 0) {
        content_max = l_min(content_max, mxw);
        c->mn = l_min(c->mn, mxw);
    }
    c->mx = l_max(c->mn, content_max);
    c->fixed = w >= 0;
}

static void encompass(LTableBox *t, uint32_t i, const CellCon *c)
{
    float *mn = &t->cmin[i], *mx = &t->cmax[i];
    if (t->cfl[i] & CF_FIXED) {
        if (c->fixed)
            *mx = l_max(*mx, c->mx);
        *mn = l_max(*mn, c->mn);
        *mx = l_max(*mx, *mn);
    } else if (c->fixed) {
        *mn = l_max(*mn, c->mn);
        *mx = l_max(c->mx, *mn);
        t->cfl[i] |= CF_FIXED;
    } else {
        *mn = l_max(*mn, c->mn);
        *mx = l_max(*mx, c->mx);
    }
    if (c->has_pct) {
        t->cfl[i] |= CF_PCT;
        t->cpct[i] = l_max(t->cpct[i], c->pct);
    }
}

/* Distributes target over columns [0, n) of arrays mn, mx, pct, fl (CSS
 * Tables 3 "distributing width to columns", following Chrome's
 * DistributeInlineSizeToComputedInlineSizeAuto). Percentages resolve
 * against pct_base. constrained: the target comes from a specified width,
 * so fixed columns may grow before percentage columns. */
static void distribute(uint32_t n, const float *mn, const float *mx, const float *pct, const uint8_t *fl,
                       float target, float pct_base, bool constrained, float *out)
{
    enum { G_MIN, G_PCT, G_SPEC, G_MAX, G_ABOVE };
    float guess[4] = {0, 0, 0, 0}, inc[4] = {0, 0, 0, 0};
    float tot_pct = 0, tot_auto_max = 0, tot_fixed_max = 0;
    uint32_t np = 0, nf = 0, na = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (fl[i] & CF_COLLAPSED)
            continue;
        if (fl[i] & CF_PCT) {
            float rp = l_max(mn[i], pct[i] * pct_base / 100);
            np++;
            tot_pct += pct[i];
            guess[G_MIN] += mn[i];
            guess[G_PCT] += rp;
            guess[G_SPEC] += rp;
            guess[G_MAX] += rp;
            inc[G_PCT] += rp - mn[i];
        } else if (fl[i] & CF_FIXED) {
            nf++;
            tot_fixed_max += mx[i];
            guess[G_MIN] += mn[i];
            guess[G_PCT] += mn[i];
            guess[G_SPEC] += mx[i];
            guess[G_MAX] += mx[i];
            inc[G_SPEC] += mx[i] - mn[i];
        } else {
            na++;
            tot_auto_max += mx[i];
            guess[G_MIN] += mn[i];
            guess[G_PCT] += mn[i];
            guess[G_SPEC] += mn[i];
            guess[G_MAX] += mx[i];
            inc[G_MAX] += mx[i] - mn[i];
        }
    }
    target = l_max(target, guess[G_MIN]);
    int start = G_ABOVE;
    for (int i = G_MIN; i < G_ABOVE; i++)
        if (guess[i] >= target) {
            start = i;
            break;
        }
    for (uint32_t i = 0; i < n; i++) {
        if (fl[i] & CF_COLLAPSED) {
            out[i] = 0;
            continue;
        }
        bool p = (fl[i] & CF_PCT) != 0, f = !p && (fl[i] & CF_FIXED);
        float rp = p ? l_max(mn[i], pct[i] * pct_base / 100) : 0;
        float w = mn[i];
        switch (start) {
        case G_MIN: break;
        case G_PCT: {
            float d = target - guess[G_MIN];
            if (p)
                w = mn[i] + (inc[G_PCT] > 0 ? d * (rp - mn[i]) / inc[G_PCT] : d / np);
            break;
        }
        case G_SPEC: {
            float d = target - guess[G_PCT];
            if (p)
                w = rp;
            else if (f)
                w = mn[i] + (inc[G_SPEC] > 0 ? d * (mx[i] - mn[i]) / inc[G_SPEC] : d / nf);
            break;
        }
        case G_MAX: {
            float d = target - guess[G_SPEC];
            bool exact = fabsf(target - guess[G_MAX]) < 0.001f;
            if (p)
                w = rp;
            else if (f)
                w = mx[i];
            else
                w = exact ? mx[i] : mn[i] + (inc[G_MAX] > 0 ? d * (mx[i] - mn[i]) / inc[G_MAX] : d / na);
            break;
        }
        default: {
            float d = target - guess[G_MAX];
            w = p ? rp : mx[i];
            if (na) {
                if (!p && !f)
                    w += tot_auto_max > 0 ? d * mx[i] / tot_auto_max : d / na;
            } else if (nf && (constrained || !np)) {
                if (f)
                    w += tot_fixed_max > 0 ? d * mx[i] / tot_fixed_max : d / nf;
            } else if (np) {
                if (p)
                    w += tot_pct > 0 ? d * pct[i] / tot_pct : d / np;
            }
            break;
        }
        }
        out[i] = w;
    }
}

typedef struct Span {
    uint32_t cell, span;
} Span;

static int span_cmp(const void *a, const void *b)
{
    const Span *x = a, *y = b;
    if (x->span != y->span)
        return x->span < y->span ? -1 : 1;
    return x->cell < y->cell ? -1 : x->cell > y->cell;
}

/* A spanning cell's constraint spread over its columns (Chrome's
 * DistributeColspanCellToColumnsAuto). */
static void spread_colspan(LTableBox *t, const LCellBox *cb, const CellCon *c, float hs, float *tmp)
{
    uint32_t c0 = cb->col, n = cb->colspan, vis = 0;
    for (uint32_t i = c0; i < c0 + n; i++)
        vis += !(t->cfl[i] & CF_COLLAPSED);
    float inner = vis > 1 ? hs * (vis - 1) : 0;
    float mn = l_max(0, c->mn - inner), mx = l_max(0, c->mx - inner);
    if (c->has_pct) {
        float cols_pct = 0, nonpct_max = 0;
        uint32_t nnp = 0;
        for (uint32_t i = c0; i < c0 + n; i++) {
            if (t->cfl[i] & CF_COLLAPSED)
                continue;
            if (t->cfl[i] & CF_PCT) {
                cols_pct += t->cpct[i];
            } else {
                nnp++;
                nonpct_max += t->cmax[i];
            }
        }
        float surplus = c->pct - cols_pct;
        if (surplus > 0 && nnp) {
            for (uint32_t i = c0; i < c0 + n; i++) {
                if ((t->cfl[i] & (CF_PCT | CF_COLLAPSED)))
                    continue;
                t->cpct[i] = nonpct_max > 0 ? surplus * t->cmax[i] / nonpct_max : surplus / nnp;
                t->cfl[i] |= CF_PCT;
            }
        }
    }
    distribute(n, t->cmin + c0, t->cmax + c0, t->cpct + c0, t->cfl + c0, mn, 0, c->fixed, tmp);
    for (uint32_t i = 0; i < n; i++)
        t->cmin[c0 + i] = l_max(t->cmin[c0 + i], tmp[i]);
    distribute(n, t->cmin + c0, t->cmax + c0, t->cpct + c0, t->cfl + c0, mx, 0, c->fixed, tmp);
    for (uint32_t i = 0; i < n; i++)
        t->cmax[c0 + i] = l_max(l_max(t->cmin[c0 + i], t->cmax[c0 + i]), tmp[i]);
}

/* Column constraints from <col> and <colgroup> widths. */
static void column_element_constraints(LTableBox *t, bool fixed)
{
    LTableGrid *g = t->g;
    for (uint32_t i = 0; i < g->ncols; i++) {
        LBlock *src[2] = {g->cols[i].col, g->cols[i].group};
        for (int k = 0; k < 2; k++) {
            if (!src[k] || lb_anon(&src[k]->b))
                continue;
            const AuLen *w = &src[k]->b.style->box->width;
            if (w->kind == AU_LEN_AUTO)
                continue;
            if (w->kind == AU_LEN_PCT) {
                t->cfl[i] |= CF_PCT;
                t->cpct[i] = w->u.pct;
            } else if (au_len_is_definite_kind(w) && !l_len_pct(w)) {
                float v = l_max(0, l_len(w, 0));
                t->cmax[i] = v;
                if (fixed)
                    t->cmin[i] = v;
                t->cfl[i] |= CF_FIXED;
            }
            break; /* a column's own width wins over its group's */
        }
    }
}

/* Fills cmin, cmax, cpct and cfl. */
static void column_constraints(AuLayout *L, LTableBox *t)
{
    LTableGrid *g = t->g;
    uint32_t nc = g->ncols;
    ensure_arrays(t);
    memset(t->cmin, 0, nc * sizeof(float));
    memset(t->cmax, 0, nc * sizeof(float));
    memset(t->cpct, 0, nc * sizeof(float));
    memset(t->cfl, 0, nc);
    for (uint32_t i = 0; i < nc; i++)
        if (g->colflags[i] & LTR_COLLAPSED)
            t->cfl[i] |= CF_COLLAPSED;
    bool fixed = is_fixed_layout(t);
    t->fixed = fixed;
    column_element_constraints(t, fixed);
    float hs = spacing_h(t);
    if (fixed) {
        /* The first row decides; contents are not measured. */
        if (!g->nrows)
            return;
        const LTableRow *r = &g->rows[0];
        for (uint32_t j = 0; j < r->n; j++) {
            LCellBox *cb = g->cells[r->first + j];
            LBlock *k = &cb->k;
            if (lb_anon(&k->b))
                continue;
            l_resolve_box_edges(k, 0);
            float pct = 0;
            bool has_pct = false;
            float w = bb_width(k, &k->b.style->box->width, &pct, &has_pct);
            uint32_t n = cb->colspan;
            for (uint32_t i = cb->col; i < (uint32_t)cb->col + n; i++) {
                if (t->cfl[i] & (CF_FIXED | CF_PCT))
                    continue;
                if (w >= 0) {
                    t->cmin[i] = t->cmax[i] = l_max(0, (w - hs * (n - 1)) / n);
                    t->cfl[i] |= CF_FIXED;
                } else if (has_pct) {
                    t->cpct[i] = pct / n;
                    t->cfl[i] |= CF_PCT;
                }
            }
        }
        return;
    }
    Span *spans = NULL;
    uint32_t nspans = 0, maxspan = 0;
    for (uint32_t i = 0; i < g->ncells; i++) {
        LCellBox *cb = g->cells[i];
        if (cb->colspan > 1) {
            if (!spans)
                spans = au_alloc(g->ncells * sizeof *spans);
            spans[nspans++] = (Span){i, cb->colspan};
            if (cb->colspan > maxspan)
                maxspan = cb->colspan;
            continue;
        }
        CellCon c;
        cell_constraint(L, cb, &c);
        encompass(t, cb->col, &c);
    }
    if (nspans) {
        qsort(spans, nspans, sizeof *spans, span_cmp);
        float *tmp = au_alloc(maxspan * sizeof *tmp);
        for (uint32_t s = 0; s < nspans; s++) {
            LCellBox *cb = g->cells[spans[s].cell];
            CellCon c;
            cell_constraint(L, cb, &c);
            spread_colspan(t, cb, &c, hs, tmp);
        }
        au_free(tmp);
        au_free(spans);
    }
    /* Percentages add up to at most 100%, in column order. */
    float sum = 0;
    for (uint32_t i = 0; i < nc; i++) {
        if (!(t->cfl[i] & CF_PCT) || (t->cfl[i] & CF_COLLAPSED))
            continue;
        if (sum + t->cpct[i] > 100)
            t->cpct[i] = l_max(0, 100 - sum);
        sum += t->cpct[i];
    }
}

static uint32_t visible_cols(const LTableBox *t)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < t->g->ncols; i++)
        n += !(t->g->colflags[i] & LTR_COLLAPSED);
    return n;
}

void l_table_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    *min_c = *max_c = 0;
    if (b->b.inner != LI_TABLE || !((LTableBox *)b)->g)
        return;
    LTableBox *t = (LTableBox *)b;
    LTableGrid *g = t->g;
    t->edges_ok = false; /* styles may have changed since the last pass */
    l_resolve_box_edges(b, 0);
    column_constraints(L, t);
    uint32_t nc = g->ncols, vis = visible_cols(t);
    float sp = vis ? spacing_h(t) * (vis + 1) : 0;
    float mn = 0, mx = 0;
    if (t->fixed) {
        for (uint32_t i = 0; i < nc; i++)
            if ((t->cfl[i] & CF_FIXED) && !(t->cfl[i] & CF_COLLAPSED))
                mn += t->cmax[i];
        mx = mn;
    } else {
        float pct_max = 0, nonpct = 0, pct_sum = 0;
        for (uint32_t i = 0; i < nc; i++) {
            if (t->cfl[i] & CF_COLLAPSED)
                continue;
            mn += t->cmin[i];
            mx += t->cmax[i];
            if (t->cfl[i] & CF_PCT) {
                pct_sum += t->cpct[i];
                if (t->cpct[i] > 0 && t->cmax[i] > 0)
                    pct_max = l_max(pct_max, t->cmax[i] * 100 / t->cpct[i]);
            } else {
                nonpct += t->cmax[i];
            }
        }
        /* Percentage columns widen an auto table so their share fits. */
        mx = l_max(mx, pct_max);
        if (pct_sum > 0 && pct_sum < 100 && nonpct > 0)
            mx = l_max(mx, nonpct * 100 / (100 - pct_sum));
    }
    mn += sp;
    mx += sp;
    /* The table is at least as wide as its captions' min-content. */
    for (LBox *c = b->b.first; c; c = c->next)
        if (l_is_caption(c)) {
            float a, z;
            l_outer_intrinsic(L, (LBlock *)c, -1, &a, &z);
            mn = l_max(mn, a - l_bp_h(b));
        }
    *min_c = mn;
    *max_c = l_max(mx, mn);
}

/* Fixed layout distribution (Chrome's DistributeInlineSizeToComputedInlineSizeFixed). */
static void distribute_fixed(LTableBox *t, float target, float *out)
{
    uint32_t nc = t->g->ncols, na = 0, nf = 0, np = 0;
    float total = 0, tf = 0, tp = 0;
    for (uint32_t i = 0; i < nc; i++) {
        uint8_t f = t->cfl[i];
        if (f & CF_COLLAPSED)
            out[i] = 0;
        else if (f & CF_FIXED) {
            out[i] = t->cmax[i];
            nf++;
            tf += t->cmax[i];
        } else if (f & CF_PCT) {
            out[i] = t->cpct[i] * target / 100;
            np++;
            tp += t->cpct[i];
        } else {
            out[i] = 0;
            na++;
        }
        total += out[i];
    }
    float d = target - total;
    if (d <= 0)
        return;
    for (uint32_t i = 0; i < nc; i++) {
        uint8_t f = t->cfl[i];
        if (f & CF_COLLAPSED)
            continue;
        if (na) {
            if (!(f & (CF_FIXED | CF_PCT)))
                out[i] += d / na;
        } else if (nf) {
            if (f & CF_FIXED)
                out[i] += tf > 0 ? d * t->cmax[i] / tf : d / nf;
        } else if (np && (f & CF_PCT)) {
            out[i] += tp > 0 ? d * t->cpct[i] / tp : d / np;
        }
    }
}

/* ---- Heights ---- */

static bool valign_baseline(const LBlock *k, int *va)
{
    int v = lb_anon(&k->b) ? AU_VALIGN_BASELINE : k->b.style->box->vertical_align;
    *va = v;
    return v != AU_VALIGN_TOP && v != AU_VALIGN_MIDDLE && v != AU_VALIGN_BOTTOM;
}

/* A specified height as a border-box height (quirks mode: the table cell
 * height box sizing quirk), or -1. pct_base: the table's definite content
 * height or -1. */
static float spec_bb_height(AuLayout *L, const LBlock *k, float pct_base, bool cell)
{
    if (lb_anon(&k->b))
        return -1;
    const AuLen *h = &k->b.style->box->height;
    if (!au_len_is_definite_kind(h))
        return -1;
    if (l_len_pct(h) && pct_base < 0)
        return -1;
    float v = l_len(h, pct_base);
    if (cell && !is_quirks(L) && k->b.style->box->box_sizing != AU_BOX_SIZING_BORDER_BOX)
        v += l_bp_v(k);
    return l_max(0, v);
}

/* The baseline of a laid-out cell from its border-box top, before any
 * alignment shift: its first line or row, else its content-box bottom,
 * where the content box is as tall as the cell's specified height when
 * that is more than its content (CSS 2.1 17.5.3; Blink and Gecko agree:
 * an empty cell with height: 1em has its baseline 1em down). */
static float cell_baseline(AuLayout *L, const LCellBox *cb, float pct_base)
{
    const LBlock *k = &cb->k;
    if (!isnan(k->baseline))
        return k->baseline;
    float content = cb->nat_h, sh = spec_bb_height(L, k, pct_base, true);
    if (sh >= 0 && sh - l_bp_v(k) > content)
        content = sh - l_bp_v(k);
    return k->bw[0] + k->p[0] + content;
}

/* Adds extra to rows [r0, r0 + n): to the rows without a specified height
 * (con[r] 0) in proportion to their heights, or equally when they have
 * none; when every row has one, to all of them. Collapsed rows and, when
 * body_only, header and footer group rows take nothing. Returns false if
 * no row could take it. */
static bool grow_rows(LTableBox *t, const uint8_t *con, uint32_t r0, uint32_t n, float extra, bool body_only)
{
    const LTableRow *rows = t->g->rows;
    for (int pass = 0; pass < 2; pass++) {
        float total = 0;
        uint32_t cnt = 0;
        for (uint32_t r = r0; r < r0 + n; r++) {
            if ((rows[r].flags & LTR_COLLAPSED) || (body_only && (rows[r].flags & LTR_HEADFOOT)) ||
                (pass == 0 && con[r]))
                continue;
            total += t->rowh[r];
            cnt++;
        }
        if (!cnt)
            continue;
        for (uint32_t r = r0; r < r0 + n; r++) {
            if ((rows[r].flags & LTR_COLLAPSED) || (body_only && (rows[r].flags & LTR_HEADFOOT)) ||
                (pass == 0 && con[r]))
                continue;
            t->rowh[r] += total > 0 ? extra * t->rowh[r] / total : extra / cnt;
        }
        return true;
    }
    return false;
}

static float span_height(const LTableBox *t, uint32_t r0, uint32_t n, float vs)
{
    float h = 0;
    uint32_t vis = 0;
    for (uint32_t r = r0; r < r0 + n; r++)
        if (!(t->g->rows[r].flags & LTR_COLLAPSED)) {
            h += t->rowh[r];
            vis++;
        }
    return h + (vis > 1 ? vs * (vis - 1) : 0);
}

/* ---- Layout ---- */

static float layout_captions(AuLayout *L, LTableBox *t, float wb, float y, int side)
{
    for (LBox *c = t->k.b.first; c; c = c->next) {
        if (!l_is_caption(c) || (c->flags & LF_ABSPOS) || c->style->inhr->caption_side != side)
            continue;
        LBlock *k = (LBlock *)c;
        l_layout_independent(L, k, wb, -1, wb, LS_STRETCH);
        k->x = k->m[3];
        k->y = y + k->m[0];
        y += k->h + k->m[0] + k->m[2];
    }
    return y;
}

/* After a measuring layout that used cached table heights, the boxes under
 * root may hold geometry from other constraints: none may be reused. */
static void forget_subtree(LBox *root)
{
    LBox *b = root;
    while (b) {
        b->flags &= ~LF_HAS_LAYOUT;
        if (b->first && b->kind != LK_TEXT)
            b = b->first;
        else {
            while (b && b != root && !b->next)
                b = b->parent;
            b = b && b != root ? b->next : NULL;
        }
    }
}

/* Does a percentage height inside cell cb resolve against its final
 * height? When the cell, its row or the table has a specified height. A
 * percentage on the cell or row counts only when the table's height is
 * definite (as in Chrome); this also keeps nested tables with percentage
 * heights from laying out every level twice per pass of the level above. */
static bool cell_height_definite(const LTableBox *t, const LCellBox *cb, float target)
{
    if (target >= 0)
        return true;
    const LBox *k = &cb->k.b, *r = &t->g->rows[cb->row].box->b;
    if (!lb_anon(k) && (k->style->box->height.kind == AU_LEN_PX || k->style->box->height.kind == AU_LEN_FIT_CONTENT_PX))
        return true;
    if (!lb_anon(r) && (r->style->box->height.kind == AU_LEN_PX || r->style->box->height.kind == AU_LEN_FIT_CONTENT_PX))
        return true;
    return false;
}

float l_table_layout(AuLayout *L, LBlock *b, const LConstraints *cons)
{
    b->baseline = b->last_baseline = NAN;
    if (b->b.inner != LI_TABLE || !((LTableBox *)b)->g)
        return 0;
    LTableBox *t = (LTableBox *)b;
    LTableGrid *g = t->g;
    /* Inside a cell's measuring layout, a table laid out again in this
     * layout generation with the same constraints answers with its height
     * and leaves its parts as they are; the cell is then laid out again
     * for real (see stale). Without this, tables nested in cells that are
     * laid out twice take time exponential in the nesting depth. */
    if (L->measuring) {
        for (int i = 0; i < 2; i++) {
            const LTableMeasure *m = &t->mc[i];
            if (m->gen == L->gen && m->w == cons->avail_w && m->h == cons->avail_h && m->cbh == cons->cb_h) {
                b->baseline = b->last_baseline = m->baseline;
                L->measure_hits++;
                return m->res;
            }
        }
    }
    float imn, imx;
    l_intrinsic(L, b, &imn, &imx); /* column constraints (cached) */
    ensure_arrays(t);
    uint32_t nc = g->ncols, nr = g->nrows;
    float W = cons->avail_w;
    float hs = spacing_h(t), vs = spacing_v(t);
    uint32_t visc = visible_cols(t), visr = 0;
    for (uint32_t r = 0; r < nr; r++)
        visr += !(g->rows[r].flags & LTR_COLLAPSED);

    /* Column widths and positions (table border-box coordinates). */
    float assign = l_max(0, W - (visc ? hs * (visc + 1) : 0));
    if (t->fixed)
        distribute_fixed(t, assign, t->colw);
    else
        distribute(nc, t->cmin, t->cmax, t->cpct, t->cfl, assign, assign,
                   !lb_anon(&b->b) && b->b.style->box->width.kind != AU_LEN_AUTO, t->colw);
    float x0 = b->bw[3] + b->p[3], x = x0;
    for (uint32_t i = 0; i < nc; i++) {
        if (t->cfl[i] & CF_COLLAPSED) {
            t->colw[i] = 0;
            t->colx[i] = x;
            continue;
        }
        x += hs;
        t->colx[i] = x;
        x += t->colw[i];
    }

    /* Captions above. */
    float wb = W + l_bp_h(b);
    float cap_top = layout_captions(L, t, wb, 0, AU_CAPTION_SIDE_TOP);
    float gy0 = cap_top + b->bw[0] + b->p[0];

    /* Cells at their column widths. */
    float target = cons->avail_h;
    if (target < 0) {
        float mh = l_clamp_h(b, 0, -1); /* min-height */
        target = mh > 0 ? l_max(0, mh) : -1;
    }
    for (uint32_t i = 0; i < g->ncells; i++) {
        LCellBox *cb = g->cells[i];
        LBlock *k = &cb->k;
        l_resolve_box_edges(k, W);
        uint32_t last = cb->col + cb->colspan - 1;
        float cw = t->colx[last] + t->colw[last] - t->colx[cb->col];
        if (cb->shift != 0) {
            l_shift_content(k, -cb->shift);
            cb->shift = 0;
        }
        k->h = cb->nat_h + l_bp_v(k);
        float base = -1;
        if (!lb_anon(&k->b)) {
            float sh = spec_bb_height(L, k, -1, true);
            if (sh >= 0)
                base = l_max(0, sh - l_bp_v(k));
        }
        uint32_t hits = L->measure_hits;
        L->measuring++;
        l_layout_content(L, k, l_max(0, cw - l_bp_h(k)), base, -1);
        L->measuring--;
        cb->base1 = base;
        cb->stale = L->measure_hits != hits;
        if (cb->stale)
            forget_subtree(&k->b);
        cb->nat_h = k->h - l_bp_v(k);
        k->w = l_max(cw, l_bp_h(k));
    }

    /* Row heights. */
    float *asc = au_alloc((nr ? nr : 1) * (2 * sizeof(float) + 1)), *desc = asc + nr;
    uint8_t *con = (uint8_t *)(desc + nr); /* the row has a specified height */
    for (uint32_t r = 0; r < nr; r++) {
        asc[r] = -1;
        desc[r] = 0;
        float sh = spec_bb_height(L, g->rows[r].box, target, false);
        t->rowh[r] = sh > 0 ? sh : 0;
        con[r] = sh >= 0;
    }
    Span *spans = NULL;
    uint32_t nspans = 0;
    for (uint32_t i = 0; i < g->ncells; i++) {
        LCellBox *cb = g->cells[i];
        LBlock *k = &cb->k;
        float h = cb->nat_h + l_bp_v(k);
        float sh = spec_bb_height(L, k, target, true);
        if (sh > h)
            h = sh;
        if (sh >= 0 && cb->rowspan == 1)
            con[cb->row] = 1;
        int va;
        if (valign_baseline(k, &va)) {
            float bl = cell_baseline(L, cb, target);
            asc[cb->row] = l_max(asc[cb->row], bl);
            if (cb->rowspan == 1)
                desc[cb->row] = l_max(desc[cb->row], h - bl);
        } else if (cb->rowspan == 1) {
            t->rowh[cb->row] = l_max(t->rowh[cb->row], h);
        }
        if (cb->rowspan > 1) {
            if (!spans)
                spans = au_alloc(g->ncells * sizeof *spans);
            spans[nspans++] = (Span){i, cb->rowspan};
        }
    }
    for (uint32_t r = 0; r < nr; r++)
        if (asc[r] >= 0)
            t->rowh[r] = l_max(t->rowh[r], asc[r] + desc[r]);
    if (nspans) {
        qsort(spans, nspans, sizeof *spans, span_cmp);
        for (uint32_t s = 0; s < nspans; s++) {
            LCellBox *cb = g->cells[spans[s].cell];
            LBlock *k = &cb->k;
            float h = cb->nat_h + l_bp_v(k);
            float sh = spec_bb_height(L, k, target, true);
            if (sh > h)
                h = sh;
            int va;
            if (valign_baseline(k, &va))
                h += asc[cb->row] - cell_baseline(L, cb, target);
            float have = span_height(t, cb->row, cb->rowspan, vs);
            if (h > have + 0.001f)
                grow_rows(t, con, cb->row, cb->rowspan, h - have, false);
        }
        au_free(spans);
    }
    for (uint32_t r = 0; r < nr; r++)
        if (g->rows[r].flags & LTR_COLLAPSED)
            t->rowh[r] = 0;
    /* A taller table gives the excess to its body rows. */
    float content_h = span_height(t, 0, nr, vs) + (visr ? 2 * vs : 0);
    if (target > content_h + 0.001f && nr) {
        if (!grow_rows(t, con, 0, nr, target - content_h, true))
            grow_rows(t, con, 0, nr, target - content_h, false);
    }
    float y = gy0;
    for (uint32_t r = 0; r < nr; r++) {
        if (g->rows[r].flags & LTR_COLLAPSED) {
            t->rowy[r] = y;
            continue;
        }
        y += vs;
        t->rowy[r] = y;
        y += t->rowh[r];
    }
    float grid_content = visr ? y + vs - gy0 : 0;
    if (target > grid_content)
        grid_content = target;

    /* Row groups and rows. A group spans the table's content box less the
     * outer spacing; a row spans its group. */
    float sx = x0 + (visc ? hs : 0), sw = l_max(0, W - (visc ? 2 * hs : 0));
    for (LBox *c = b->b.first; c; c = c->next) {
        if (c->kind == LK_BLOCK && c->inner == LI_TABLE_ROW_GROUP) {
            LBlock *k = (LBlock *)c;
            k->x = sx;
            k->y = gy0;
            k->w = sw;
            k->h = 0;
        }
    }
    for (uint32_t r = 0; r < nr;) {
        LBlock *grp = g->rows[r].group;
        uint32_t e = r + 1;
        while (e < nr && g->rows[e].group == grp)
            e++;
        if (grp) {
            grp->y = t->rowy[r];
            grp->h = t->rowy[e - 1] + t->rowh[e - 1] - t->rowy[r];
        }
        for (uint32_t k = r; k < e; k++) {
            LBlock *rb = g->rows[k].box;
            rb->x = grp ? 0 : sx;
            rb->y = grp ? t->rowy[k] - grp->y : t->rowy[k];
            rb->w = sw;
            rb->h = t->rowh[k];
            rb->baseline = rb->last_baseline = NAN;
        }
        r = e;
    }

    /* Cells: final size, percentage heights, vertical alignment. */
    for (uint32_t i = 0; i < g->ncells; i++) {
        LCellBox *cb = g->cells[i];
        LBlock *k = &cb->k;
        const LTableRow *row = &g->rows[cb->row];
        LBlock *rb = row->box;
        float rax = row->group ? row->group->x + rb->x : rb->x;
        uint32_t last = cb->col + cb->colspan - 1;
        float cw = t->colx[last] + t->colw[last] - t->colx[cb->col];
        float h = span_height(t, cb->row, cb->rowspan, vs);
        float bpv = l_bp_v(k), inner = l_max(0, h - bpv);
        bool definite = cell_height_definite(t, cb, target);
        if ((definite && fabsf(inner - cb->nat_h) > 0.001f) || cb->stale) {
            l_layout_content(L, k, l_max(0, cw - l_bp_h(k)), definite ? inner : cb->base1, -1);
            cb->nat_h = k->h - bpv;
            cb->stale = false;
        }
        k->x = t->colx[cb->col] - rax;
        k->y = 0;
        k->w = l_max(cw, l_bp_h(k));
        int va;
        float shift = 0;
        if (valign_baseline(k, &va))
            shift = asc[cb->row] - cell_baseline(L, cb, target);
        else if (va == AU_VALIGN_MIDDLE)
            shift = (inner - cb->nat_h) / 2;
        else if (va == AU_VALIGN_BOTTOM)
            shift = inner - cb->nat_h;
        if (shift < 0 && va != AU_VALIGN_BASELINE)
            shift = 0;
        if (shift != 0) {
            l_shift_content(k, shift);
            cb->shift = shift;
        }
        k->h = l_max(h, bpv);
        l_finish_overflow(k);
        if (cb->row == 0 || isnan(rb->baseline))
            rb->baseline = rb->last_baseline = asc[cb->row] >= 0 ? asc[cb->row] : NAN;
    }
    for (uint32_t r = 0; r < nr; r++)
        l_finish_overflow(g->rows[r].box);
    for (LBox *c = b->b.first; c; c = c->next)
        if (c->kind == LK_BLOCK && c->inner == LI_TABLE_ROW_GROUP)
            l_finish_overflow((LBlock *)c);

    /* Columns and column groups: their boxes span the rows (for
     * backgrounds and hit tests by element). */
    float ry0 = nr ? t->rowy[0] : gy0, ry1 = nr ? t->rowy[nr - 1] + t->rowh[nr - 1] : gy0;
    for (LBox *c = b->b.first; c; c = c->next) {
        if (c->kind != LK_BLOCK || (c->inner != LI_TABLE_COLUMN && c->inner != LI_TABLE_COLUMN_GROUP))
            continue;
        LBlock *k = (LBlock *)c;
        k->x = k->y = k->w = k->h = 0;
        for (LBox *cc = c->first; cc; cc = cc->next) {
            if (cc->kind != LK_BLOCK)
                continue; /* the box tree leaves only columns here */
            LBlock *kk = (LBlock *)cc;
            kk->x = kk->y = kk->w = kk->h = 0;
        }
    }
    for (uint32_t i = 0; i < nc;) {
        uint32_t e = i + 1;
        LBlock *col = g->cols[i].col;
        while (e < nc && g->cols[e].col == col)
            e++;
        if (col) {
            col->x = t->colx[i];
            col->y = ry0;
            col->w = t->colx[e - 1] + t->colw[e - 1] - t->colx[i];
            col->h = ry1 - ry0;
            if (g->cols[i].group) {
                col->x -= t->colx[0];
                col->y = 0;
            }
            col->ov = (LArea){0, 0, col->w, col->h};
        }
        i = e;
    }
    for (uint32_t i = 0; i < nc;) {
        uint32_t e = i + 1;
        LBlock *grp = g->cols[i].group;
        while (e < nc && g->cols[e].group == grp)
            e++;
        if (grp) {
            grp->x = t->colx[i];
            grp->y = ry0;
            grp->w = t->colx[e - 1] + t->colw[e - 1] - t->colx[i];
            grp->h = ry1 - ry0;
            grp->ov = (LArea){0, 0, grp->w, grp->h};
            for (uint32_t k = i; k < e; k++)
                if (g->cols[k].col && g->cols[k].group == grp)
                    g->cols[k].col->x = t->colx[k] - t->colx[i];
        }
        i = e;
    }

    /* Absolutely positioned children of the table and its parts. */
    for (LBox *c = b->b.first; c; c = c->next) {
        if (c->kind != LK_BLOCK)
            continue;
        if (c->flags & LF_ABSPOS) {
            l_resolve_box_edges((LBlock *)c, W);
            l_queue_abspos(L, (LBlock *)c, x0, gy0);
            continue;
        }
        if (c->inner != LI_TABLE_ROW_GROUP && c->inner != LI_TABLE_ROW)
            continue;
        for (LBox *d = c->first; d; d = d->next) {
            if (d->kind == LK_BLOCK && (d->flags & LF_ABSPOS)) {
                l_resolve_box_edges((LBlock *)d, W);
                l_queue_abspos(L, (LBlock *)d, 0, 0);
            } else if (c->inner == LI_TABLE_ROW_GROUP && d->kind == LK_BLOCK && d->inner == LI_TABLE_ROW) {
                for (LBox *e = d->first; e; e = e->next)
                    if (e->kind == LK_BLOCK && (e->flags & LF_ABSPOS)) {
                        l_resolve_box_edges((LBlock *)e, W);
                        l_queue_abspos(L, (LBlock *)e, 0, 0);
                    }
            }
        }
    }

    /* The table's baseline: its first row's. */
    for (uint32_t r = 0; r < nr; r++) {
        if (g->rows[r].flags & LTR_COLLAPSED)
            continue;
        b->baseline = t->rowy[r] + (asc[r] >= 0 ? asc[r] : t->rowh[r]);
        break;
    }
    b->last_baseline = b->baseline;
    au_free(asc);

    float gh = grid_content + l_bp_v(b);
    t->grid_y = cap_top;
    t->grid_h = gh;
    float cap_bot = layout_captions(L, t, wb, cap_top + gh, AU_CAPTION_SIDE_BOTTOM) - (cap_top + gh);
    float res = cap_top + grid_content + cap_bot;
    int slot = t->mc_next;
    for (int i = 0; i < 2; i++)
        if (t->mc[i].gen == L->gen && t->mc[i].w == cons->avail_w && t->mc[i].h == cons->avail_h &&
            t->mc[i].cbh == cons->cb_h)
            slot = i;
    t->mc[slot] = (LTableMeasure){cons->avail_w, cons->avail_h, cons->cb_h, res, b->baseline, L->gen};
    if (slot == t->mc_next)
        t->mc_next ^= 1;
    return res;
}

bool l_cell_empty(const LBlock *k)
{
    if (k->nfrags)
        return false;
    for (const LBox *c = k->b.first; c; c = c->next)
        if (c->kind == LK_BLOCK && !(c->flags & LF_ABSPOS))
            return false;
    return true;
}
