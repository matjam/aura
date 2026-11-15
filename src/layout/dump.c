/* A text dump of the box tree with geometry in document coordinates. */
#include "layout_int.h"

#include <stdio.h>

static void indent(AuBuf *o, int d)
{
    for (int i = 0; i < d; i++)
        au_buf_puts(o, "  ");
}

static void name_of(const LBox *b, AuBuf *o)
{
    if (b->kind == LK_TEXT) {
        au_buf_puts(o, "text");
        return;
    }
    static const char *const kinds[] = {"block", "inline", "text", "br", "wbr"};
    const char *k = kinds[b->kind];
    if (b->kind == LK_BLOCK) {
        if (b->flags & LF_INLINE_LEVEL)
            k = "inline-block";
        if (b->inner == LI_FLEX)
            k = b->flags & LF_INLINE_LEVEL ? "inline-flex" : "flex";
        else if (b->inner == LI_TABLE)
            k = b->flags & LF_INLINE_LEVEL ? "inline-table" : "table";
        else if (b->inner == LI_TABLE_COLUMN)
            k = "table-column";
        else if (b->inner == LI_TABLE_COLUMN_GROUP)
            k = "table-column-group";
        else if (b->inner == LI_TABLE_ROW)
            k = "table-row";
        else if (b->inner == LI_TABLE_ROW_GROUP)
            k = "table-row-group";
        else if (b->inner == LI_TABLE_CELL)
            k = "table-cell";
        else if (b->inner == LI_REPLACED)
            k = "replaced";
    }
    au_buf_puts(o, k);
    if (lb_anon(b) && !b->pseudo) {
        au_buf_puts(o, " (anonymous)");
        return;
    }
    if (b->node && au_is_element(b->node)) {
        AuElement *e = au_as_element(b->node);
        au_buf_printf(o, " <%s", au_atom_str(e->tag));
        if (e->id)
            au_buf_printf(o, " id=%s", au_atom_str(e->id));
        au_buf_putc(o, '>');
    }
    if (b->pseudo == AU_CSS_PSEUDO_BEFORE)
        au_buf_puts(o, "::before");
    else if (b->pseudo == AU_CSS_PSEUDO_AFTER)
        au_buf_puts(o, "::after");
    else if (b->pseudo == AU_CSS_PSEUDO_MARKER)
        au_buf_puts(o, "::marker");
}

static void flags_of(const LBox *b, AuBuf *o)
{
    static const struct {
        uint32_t f;
        const char *n;
    } fl[] = {{LF_FLOAT, "float"},     {LF_ABSPOS, "abspos"},   {LF_FIXED, "fixed"},     {LF_RELPOS, "relative"},
              {LF_STICKY, "sticky"},   {LF_BFC, "bfc"},         {LF_INLINES, "inlines"}, {LF_STACKING, "stacking"},
              {LF_CLIPS, "clips"},     {LF_SCROLLER, "scroll"}, {LF_HIDDEN, "hidden"},   {LF_TOO_DEEP, "too-deep"},
              {LF_FLEX_ITEM, "flex-item"}};
    bool any = false;
    for (size_t i = 0; i < sizeof fl / sizeof fl[0]; i++)
        if (b->flags & fl[i].f) {
            au_buf_puts(o, any ? "," : " [");
            au_buf_puts(o, fl[i].n);
            any = true;
        }
    if (any)
        au_buf_putc(o, ']');
}

static void quoted(AuBuf *o, const char *t, uint32_t n)
{
    au_buf_putc(o, '"');
    for (uint32_t i = 0; i < n && i < 60; i++) {
        char c = t[i];
        if (c == '\n')
            au_buf_puts(o, "\\n");
        else if (c == '"')
            au_buf_puts(o, "\\\"");
        else
            au_buf_putc(o, c);
    }
    if (n > 60)
        au_buf_puts(o, "...");
    au_buf_putc(o, '"');
}

static void num(AuBuf *o, float v)
{
    if (v == (float)au_f2i(v))
        au_buf_printf(o, "%d", au_f2i(v));
    else
        au_buf_printf(o, "%.2f", v);
}

void l_dump(AuLayout *L, AuBuf *o)
{
    au_buf_printf(o, "viewport %dx%d document %dx%d\n", (int)L->vw, (int)L->vh, au_f2i(L->doc_w), au_f2i(L->doc_h));
    LBox *b = L->root;
    int d = 0;
    while (b) {
        if (b->kind != LK_TEXT || 1) {
            indent(o, d);
            name_of(b, o);
            if (b->kind == LK_BLOCK) {
                LBlock *k = (LBlock *)b;
                float x, y;
                l_box_origin(b, &x, &y);
                au_buf_putc(o, ' ');
                num(o, x);
                au_buf_putc(o, ',');
                num(o, y);
                au_buf_putc(o, ' ');
                num(o, k->w);
                au_buf_putc(o, 'x');
                num(o, k->h);
                static const char *const en[3] = {"m", "b", "p"};
                const float *ev[3] = {k->m, k->bw, k->p};
                for (int e = 0; e < 3; e++)
                    if (ev[e][0] || ev[e][1] || ev[e][2] || ev[e][3]) {
                        au_buf_printf(o, " %s=", en[e]);
                        for (int s = 0; s < 4; s++) {
                            if (s)
                                au_buf_putc(o, ',');
                            num(o, ev[e][s]);
                        }
                    }
                if (b->flags & LF_CLIPS) {
                    au_buf_puts(o, " extent=");
                    num(o, k->sw);
                    au_buf_putc(o, 'x');
                    num(o, k->sh);
                }
            } else if (b->kind == LK_TEXT) {
                au_buf_putc(o, ' ');
                quoted(o, ((LText *)b)->text, ((LText *)b)->len);
            }
            flags_of(b, o);
            au_buf_putc(o, '\n');
            if (b->kind == LK_BLOCK && (b->flags & LF_INLINES)) {
                LBlock *k = (LBlock *)b;
                float gx, gy;
                l_box_origin(b, &gx, &gy);
                for (uint32_t i = 0; i < k->nlines; i++) {
                    LLine *ln = &k->lines[i];
                    indent(o, d + 1);
                    au_buf_printf(o, "line %u: ", i);
                    num(o, gx + ln->x);
                    au_buf_putc(o, ',');
                    num(o, gy + ln->y);
                    au_buf_putc(o, ' ');
                    num(o, ln->w);
                    au_buf_putc(o, 'x');
                    num(o, ln->h);
                    au_buf_puts(o, " baseline ");
                    num(o, ln->baseline);
                    au_buf_putc(o, '\n');
                    for (uint32_t j = ln->first; j < ln->first + ln->n; j++) {
                        LFrag *f = &k->frags[j];
                        indent(o, d + 2);
                        static const char *const fk[] = {"text", "inline", "atomic", "br"};
                        au_buf_puts(o, fk[f->kind]);
                        if (f->kind == FR_TEXT) {
                            au_buf_putc(o, ' ');
                            quoted(o, ((LText *)f->box)->text + f->start, f->len);
                        } else {
                            au_buf_putc(o, ' ');
                            name_of(f->box, o);
                        }
                        au_buf_putc(o, ' ');
                        num(o, gx + f->x);
                        au_buf_putc(o, ',');
                        num(o, gy + f->y);
                        au_buf_putc(o, ' ');
                        num(o, f->w);
                        au_buf_putc(o, 'x');
                        num(o, f->h);
                        if (f->flags & FRF_ELLIPSIS)
                            au_buf_puts(o, " ellipsis");
                        if (f->flags & FRF_HYPHEN)
                            au_buf_puts(o, " hyphen");
                        if (f->flags & FRF_HIDDEN)
                            au_buf_puts(o, " hidden");
                        au_buf_putc(o, '\n');
                    }
                }
            }
        }
        if (b->first && b->kind != LK_TEXT && !(b->flags & LF_TOO_DEEP)) {
            b = b->first;
            d++;
        } else {
            while (b && !b->next) {
                b = b->parent;
                d--;
            }
            if (b)
                b = b->next;
        }
    }
}

void au_layout_dump(AuLayout *L, AuBuf *out)
{
    l_dump(L, out);
}
