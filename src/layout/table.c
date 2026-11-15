/* PLACEHOLDER table layout, until real CSS table layout (anonymous table
 * objects, the automatic and fixed table layout algorithms, rowspan,
 * border-collapse, baseline alignment of cells) replaces it.
 *
 * What it does: rows stack vertically; each row's cells sit side by side
 * in columns whose widths come from the cells' min-content and
 * max-content widths (a simplified automatic layout: columns get their
 * max-content width if it fits, otherwise min-content plus a share of the
 * rest); colspan spans columns; rowspan is ignored; border-spacing
 * separates cells; cells are as tall as their row and align their content
 * top, middle or bottom; captions go above. Table boxes that are not
 * inside a table (a stray row or row group) are laid out the same way.
 * This keeps the table-based sites (Hacker News, old forums) readable. */
#include "layout_int.h"

#include <stdlib.h>

#define MAX_COLS 256

typedef struct TRow {
    LBlock *row;   /* the row box, or NULL for cells directly in the table */
    LBlock *group; /* the row group holding it, or NULL */
    LBox *first;   /* first cell (or the only block, for a stray block) */
    bool stray;    /* a non-row child: laid out across the whole table */
} TRow;

typedef struct TTable {
    TRow *rows;
    uint32_t n, cap;
    int ncols;
    float colmin[MAX_COLS], colmax[MAX_COLS], colpct[MAX_COLS], colw[MAX_COLS];
} TTable;

static void add_row(TTable *T, LBlock *row, LBlock *group, LBox *first, bool stray)
{
    if (T->n == T->cap) {
        T->cap = T->cap ? T->cap * 2 : 16;
        T->rows = au_realloc(T->rows, T->cap * sizeof *T->rows);
    }
    T->rows[T->n++] = (TRow){row, group, first, stray};
}

static bool is_cell(const LBox *c)
{
    return c->kind == LK_BLOCK && c->inner == LI_TABLE_CELL && !(c->flags & (LF_ABSPOS | LF_FLOAT));
}

static bool is_caption(const LBox *c)
{
    return c->kind == LK_BLOCK && !lb_anon(c) && au_display_inner(c->style->box->display) == AU_DISPLAY_INNER_TABLE_CAPTION;
}

static void gather_rows_of(TTable *T, LBlock *container, LBlock *group)
{
    LBox *c = container->b.first;
    while (c) {
        if (c->kind != LK_BLOCK || (c->flags & (LF_ABSPOS | LF_FLOAT)) || is_caption(c)) {
            c = c->next;
            continue;
        }
        if (c->inner == LI_TABLE_ROW) {
            add_row(T, (LBlock *)c, group, c->first, false);
            c = c->next;
        } else if (c->inner == LI_TABLE_ROW_GROUP && !group) {
            gather_rows_of(T, (LBlock *)c, (LBlock *)c);
            c = c->next;
        } else if (is_cell(c)) {
            add_row(T, NULL, group, c, false);
            while (c && (is_cell(c) || c->kind != LK_BLOCK))
                c = c->next;
        } else {
            add_row(T, NULL, group, c, true);
            c = c->next;
        }
    }
}

static void gather(TTable *T, LBlock *b)
{
    memset(T, 0, sizeof *T);
    if (b->b.inner == LI_TABLE_ROW)
        add_row(T, b, NULL, b->b.first, false);
    else
        gather_rows_of(T, b, b->b.inner == LI_TABLE_ROW_GROUP ? b : NULL);
}

static int colspan(const LBox *c)
{
    AuAtom a = au_atom_lookup("colspan", 7);
    const char *v = a ? l_attr(c, a) : NULL;
    int n = v ? atoi(v) : 1;
    return n < 1 ? 1 : n > 1000 ? 1000 : n;
}

/* Iterates the cells of a row: for a row box its cell children, for an
 * implicit row the run of cells starting at first. */
static LBox *next_cell(const TRow *r, LBox *c)
{
    for (c = c ? c->next : r->first; c; c = c->next) {
        if (is_cell(c))
            return c;
        if (!r->row && c->kind == LK_BLOCK && !(c->flags & (LF_ABSPOS | LF_FLOAT)))
            return NULL;
    }
    return NULL;
}

static float spacing_h(const LBlock *t)
{
    if (lb_anon(&t->b) || t->b.inner != LI_TABLE)
        return 0;
    if (t->b.style->inhr->border_collapse == AU_BORDER_COLLAPSE_COLLAPSE)
        return 0;
    return t->b.style->inhr->border_spacing[0];
}

static float spacing_v(const LBlock *t)
{
    if (lb_anon(&t->b) || t->b.inner != LI_TABLE)
        return 0;
    if (t->b.style->inhr->border_collapse == AU_BORDER_COLLAPSE_COLLAPSE)
        return 0;
    return t->b.style->inhr->border_spacing[1];
}

static void columns(AuLayout *L, LBlock *b, TTable *T)
{
    T->ncols = 0;
    for (int i = 0; i < MAX_COLS; i++)
        T->colmin[i] = T->colmax[i] = T->colpct[i] = 0;
    /* Single-span cells first, then spanning cells spread their excess. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t r = 0; r < T->n; r++) {
            if (T->rows[r].stray)
                continue;
            int col = 0;
            for (LBox *c = next_cell(&T->rows[r], NULL); c && col < MAX_COLS; c = next_cell(&T->rows[r], c)) {
                int span = colspan(c);
                if (col + span > MAX_COLS)
                    span = MAX_COLS - col;
                if ((span == 1) != (pass == 0)) {
                    col += span;
                    continue;
                }
                LBlock *k = (LBlock *)c;
                float mn, mx;
                l_outer_intrinsic(L, k, -1, &mn, &mx);
                const AuLen *w = &k->b.style->box->width;
                if (w->kind == AU_LEN_PCT && span == 1)
                    T->colpct[col] = l_max(T->colpct[col], w->u.pct);
                if (span == 1) {
                    T->colmin[col] = l_max(T->colmin[col], mn);
                    T->colmax[col] = l_max(T->colmax[col], mx);
                } else {
                    float smn = 0, smx = 0;
                    for (int i = col; i < col + span; i++) {
                        smn += T->colmin[i];
                        smx += T->colmax[i];
                    }
                    float sp = spacing_h(b) * (span - 1);
                    if (mn > smn + sp)
                        for (int i = col; i < col + span; i++)
                            T->colmin[i] += (mn - smn - sp) / span;
                    if (mx > smx + sp)
                        for (int i = col; i < col + span; i++)
                            T->colmax[i] += (mx - smx - sp) / span;
                }
                col += span;
            }
            if (col > T->ncols)
                T->ncols = col;
        }
    }
    for (int i = 0; i < T->ncols; i++)
        if (T->colmax[i] < T->colmin[i])
            T->colmax[i] = T->colmin[i];
}

static float stray_width(AuLayout *L, TTable *T, float *mn_out)
{
    float mn = 0, mx = 0;
    for (uint32_t r = 0; r < T->n; r++)
        if (T->rows[r].stray && T->rows[r].first->kind == LK_BLOCK) {
            float a, b;
            l_outer_intrinsic(L, (LBlock *)T->rows[r].first, -1, &a, &b);
            mn = l_max(mn, a);
            mx = l_max(mx, b);
        }
    *mn_out = mn;
    return mx;
}

void l_table_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    TTable *T = au_alloc(sizeof *T);
    gather(T, b);
    columns(L, b, T);
    float sp = spacing_h(b);
    float mn = sp * (T->ncols + 1), mx = mn;
    for (int i = 0; i < T->ncols; i++) {
        mn += T->colmin[i];
        mx += T->colmax[i];
    }
    float smn;
    float smx = stray_width(L, T, &smn);
    for (LBox *c = b->b.first; c; c = c->next)
        if (is_caption(c)) {
            float a, bb;
            l_outer_intrinsic(L, (LBlock *)c, -1, &a, &bb);
            smn = l_max(smn, a);
        }
    *min_c = l_max(mn, smn);
    *max_c = l_max(mx, l_max(smx, *min_c));
    au_free(T->rows);
    au_free(T);
}

#define shift_content l_shift_content

float l_table_layout(AuLayout *L, LBlock *b, const LConstraints *cons)
{
    TTable *T = au_alloc(sizeof *T);
    gather(T, b);
    columns(L, b, T);
    float W = cons->avail_w;
    float sh = spacing_h(b), sv = spacing_v(b);
    int nc = T->ncols;
    float avail = W - sh * (nc + 1);
    float summin = 0, summax = 0;
    for (int i = 0; i < nc; i++) {
        summin += T->colmin[i];
        summax += T->colmax[i];
    }
    /* Percentage columns first. */
    float used_pct = 0;
    for (int i = 0; i < nc; i++)
        T->colw[i] = -1;
    for (int i = 0; i < nc; i++)
        if (T->colpct[i] > 0) {
            T->colw[i] = l_max(T->colmin[i], avail * T->colpct[i] / 100);
            used_pct += T->colw[i];
        }
    float rest = avail - used_pct, rmin = 0, rmax = 0;
    int nrest = 0;
    for (int i = 0; i < nc; i++)
        if (T->colw[i] < 0) {
            rmin += T->colmin[i];
            rmax += T->colmax[i];
            nrest++;
        }
    for (int i = 0; i < nc; i++) {
        if (T->colw[i] >= 0)
            continue;
        if (rest >= rmax) {
            float extra = rest - rmax;
            T->colw[i] = T->colmax[i] + (rmax > 0 ? extra * T->colmax[i] / rmax : extra / nrest);
        } else if (rest > rmin && rmax > rmin) {
            T->colw[i] = T->colmin[i] + (T->colmax[i] - T->colmin[i]) * (rest - rmin) / (rmax - rmin);
        } else {
            T->colw[i] = T->colmin[i];
        }
    }
    (void)summin;
    (void)summax;
    float content_x0 = b->bw[3] + b->p[3], content_y0 = b->bw[0] + b->p[0];
    float y = 0;
    /* Captions on top. */
    for (LBox *c = b->b.first; c; c = c->next)
        if (is_caption(c)) {
            LBlock *k = (LBlock *)c;
            l_layout_independent(L, k, W, -1, W, LS_STRETCH);
            k->x = content_x0 + k->m[3];
            k->y = content_y0 + y + k->m[0];
            y += k->h + k->m[0] + k->m[2];
        }
    y += sv;
    LBlock *cur_group = NULL;
    float group_y = 0;
    for (uint32_t r = 0; r < T->n; r++) {
        TRow *row = &T->rows[r];
        if (row->group != cur_group) {
            if (cur_group && cur_group != b) {
                cur_group->h = y - group_y - sv;
                l_finish_overflow(cur_group);
            }
            cur_group = row->group;
            if (cur_group && cur_group != b) {
                l_resolve_box_edges(cur_group, W);
                cur_group->x = content_x0;
                cur_group->y = content_y0 + y;
                cur_group->w = W;
                group_y = y;
            }
        }
        /* Coordinates of this row's geometry parent, relative to the table content box. */
        float px = 0, py = 0;
        if (cur_group && cur_group != b) {
            py = group_y;
        }
        if (row->stray) {
            LBlock *k = (LBlock *)row->first;
            if (row->first->kind == LK_BLOCK) {
                l_layout_independent(L, k, W, -1, W - 2 * sh, LS_STRETCH);
                k->x = (cur_group && cur_group != b ? 0 : content_x0) + sh + k->m[3];
                k->y = (cur_group && cur_group != b ? y - group_y : content_y0 + y) + k->m[0];
                y += k->h + k->m[0] + k->m[2] + sv;
            }
            continue;
        }
        float rowh = 0;
        if (row->row) {
            l_resolve_box_edges(row->row, W);
            float sh2 = l_spec_height(row->row, -1);
            if (sh2 > 0)
                rowh = sh2;
        }
        int col = 0;
        float x = sh;
        for (LBox *c = next_cell(row, NULL); c && col < MAX_COLS; c = next_cell(row, c)) {
            int span = colspan(c);
            if (col + span > nc)
                span = nc - col > 0 ? nc - col : 1;
            float w = 0;
            for (int i = col; i < col + span && i < nc; i++)
                w += T->colw[i];
            w += sh * (span - 1);
            LBlock *k = (LBlock *)c;
            l_resolve_box_edges(k, W);
            float cw = l_max(0, w - l_bp_h(k));
            float h = l_spec_height(k, -1);
            l_layout_content(L, k, cw, -1, -1);
            /* k->h stays the content height, so a cell made taller by its
             * height property still aligns its content below. */
            rowh = l_max(rowh, h >= 0 ? l_max(k->h, h + l_bp_v(k)) : k->h);
            k->x = x;
            col += span;
            x += w + sh;
        }
        /* Second pass: heights and vertical alignment. */
        for (LBox *c = next_cell(row, NULL); c; c = next_cell(row, c)) {
            LBlock *k = (LBlock *)c;
            float extra = rowh - k->h;
            int va = k->b.style->box->vertical_align;
            if (extra > 0) {
                if (va == AU_VALIGN_MIDDLE)
                    shift_content(k, extra / 2);
                else if (va == AU_VALIGN_BOTTOM)
                    shift_content(k, extra);
                k->h = rowh;
                l_finish_overflow(k);
            }
            if (row->row) {
                k->y = 0;
            } else {
                k->x += cur_group && cur_group != b ? 0 : content_x0;
                k->y = (cur_group && cur_group != b ? y - group_y : content_y0 + y);
            }
        }
        if (row->row) {
            LBlock *rb = row->row;
            rb->x = (cur_group && cur_group != b ? 0 : content_x0) + px;
            rb->y = (cur_group && cur_group != b ? y - py : content_y0 + y);
            rb->w = W;
            rb->h = rowh;
            rb->baseline = NAN;
            l_finish_overflow(rb);
            /* cells were positioned relative to the table content: make them row-relative */
        }
        y += rowh + sv;
    }
    if (cur_group && cur_group != b) {
        cur_group->h = y - group_y - sv;
        l_finish_overflow(cur_group);
    }
    if (T->n == 0)
        y = 0;
    b->baseline = NAN;
    /* The first row's baseline: the top of its first cell's content. */
    if (T->n && !T->rows[0].stray) {
        LBox *c = next_cell(&T->rows[0], NULL);
        if (c && !isnan(((LBlock *)c)->baseline)) {
            float ox = 0, oy = 0;
            (void)ox;
            if (T->rows[0].row)
                oy = T->rows[0].row->y;
            if (T->rows[0].group && T->rows[0].group != b && T->rows[0].row)
                oy += T->rows[0].group->y;
            b->baseline = oy + ((LBlock *)c)->y + ((LBlock *)c)->baseline;
        }
    }
    b->last_baseline = b->baseline;
    au_free(T->rows);
    au_free(T);
    return y;
}
