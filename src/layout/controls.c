/* Form field geometry and painting: the text of single-line inputs and
 * textareas (value or placeholder, password bullets, soft wrapping,
 * scrolling), the selection and the caret; hit positions for the editing
 * code in src/page; list box rows and range sliders.
 *
 * A field's text is laid out here, not in the box tree: typing changes
 * only the control's value (src/dom/control.h) and repaints the field. A
 * single-line field shows its text on one line scrolled sideways by
 * AuControl.scroll_x; a textarea wraps at its content width (at spaces,
 * else anywhere) and scrolls up by AuControl.scroll_y. */
#include "layout_int.h"

#include "../dom/control.h"

#include <stdlib.h>

#define FIELD_SELECTION AU_RGB(0xB3, 0xD7, 0xFF)

typedef struct FLine {
    uint32_t a, b; /* display offsets; b excludes a line break */
} FLine;

typedef struct Field {
    AuLayout *L;
    LBlock *b;
    AuElement *el;
    LFont *f;
    const AuStyle *s;
    float cx, cy, cw, ch; /* content box relative to the border box */
    float lh, asc;
    bool multi;
    /* the displayed text: the value, or bullets for a password */
    const char *t;
    size_t n;
    AuBuf own;
    bool bullets;
    const char *v; /* the value */
    size_t vn;
    FLine *lines;
    uint32_t nlines, lcap;
    AuControl *ctl;
} Field;

static LBlock *field_box(AuElement *el)
{
    LBox *b = el ? (LBox *)el->box : NULL;
    if (!b || b->kind != LK_BLOCK || (b->flags & LF_TOO_DEEP))
        return NULL;
    if (b->replaced != LR_TEXT_INPUT && b->replaced != LR_TEXTAREA)
        return NULL;
    return (LBlock *)b;
}

static uint32_t to_disp(const Field *F, uint32_t off)
{
    if (!F->bullets)
        return off > F->n ? (uint32_t)F->n : off;
    return (uint32_t)(3 * au_utf8_count(F->v, off < F->vn ? off : F->vn));
}

static uint32_t to_value(const Field *F, uint32_t d)
{
    if (!F->bullets)
        return d;
    size_t k = d / 3, i = 0;
    while (i < F->vn && k > 0) {
        i++;
        while (i < F->vn && ((unsigned char)F->v[i] & 0xC0) == 0x80)
            i++;
        k--;
    }
    return (uint32_t)i;
}

static float width_of(Field *F, uint32_t a, uint32_t b)
{
    return b > a ? l_measure(F->L, F->f, F->s, F->t + a, b - a) : 0;
}

static void add_line(Field *F, uint32_t a, uint32_t b)
{
    if (F->nlines == F->lcap) {
        F->lcap = F->lcap ? F->lcap * 2 : 8;
        F->lines = au_realloc(F->lines, F->lcap * sizeof *F->lines);
    }
    F->lines[F->nlines].a = a;
    F->lines[F->nlines].b = b;
    F->nlines++;
}

static uint32_t next_char(const char *t, size_t n, uint32_t i)
{
    i++;
    while (i < n && ((unsigned char)t[i] & 0xC0) == 0x80)
        i++;
    return i;
}

/* Soft wrap of one hard line [a, b) at width w. Each line costs about
 * its own length to find (l_prefix_seek): measuring the rest of the text
 * for every line, and every prefix up to each space, took 11 s to paint
 * a textarea of 80,000 characters. */
static void wrap_line(Field *F, uint32_t a, uint32_t b, float w)
{
    const char *t = F->t;
    while (a < b) {
        /* The longest prefix that fits. */
        float pw;
        uint32_t k = a + l_prefix_seek(F->L, F->f, F->s, t + a, b - a, w, &pw);
        while (k < b) {
            uint32_t e2 = next_char(t, b, k);
            if (width_of(F, a, e2) > w)
                break;
            k = e2;
        }
        if (k >= b) {
            add_line(F, a, b);
            return;
        }
        /* The longest prefix ending after a space, the text before the
         * space fitting. */
        uint32_t best = 0;
        for (uint32_t i = k + 1; i-- > a;) {
            if (t[i] == ' ') {
                best = i + 1;
                break;
            }
        }
        if (!best) /* no space fits: break between characters */
            best = k > a ? k : next_char(t, b, a);
        add_line(F, a, best);
        a = best;
    }
}

static bool field_open(Field *F, AuLayout *L, AuElement *el)
{
    memset(F, 0, sizeof *F);
    LBlock *b = field_box(el);
    if (!b)
        return false;
    F->L = L;
    F->b = b;
    F->el = el;
    F->s = b->b.style;
    F->f = l_font(L, F->s);
    F->cx = b->bw[3] + b->p[3];
    F->cy = b->bw[0] + b->p[0];
    F->cw = l_max(0, b->w - b->bw[1] - b->bw[3] - b->p[1] - b->p[3]);
    F->ch = l_max(0, b->h - b->bw[0] - b->bw[2] - b->p[0] - b->p[2]);
    F->lh = l_control_line(L, b, &F->asc);
    F->multi = b->b.replaced == LR_TEXTAREA;
    F->ctl = au_ctl(el);
    au_buf_init(&F->own);
    F->v = au_ctl_value(el, &F->vn);
    F->bullets = el->tag == AU_ATOM_input && au_input_type(el) == AU_INPUT_PASSWORD;
    if (F->bullets) {
        size_t k = au_utf8_count(F->v, F->vn);
        for (size_t i = 0; i < k && i < 100000; i++)
            au_buf_puts(&F->own, "\xE2\x80\xA2");
        F->t = F->own.data ? F->own.data : "";
        F->n = F->own.len;
    } else {
        F->t = F->v;
        F->n = F->vn;
    }
    if (!F->multi) {
        add_line(F, 0, (uint32_t)F->n);
    } else {
        uint32_t a = 0;
        for (uint32_t i = 0; i <= F->n; i++) {
            if (i == F->n || F->t[i] == '\n') {
                if (i == a)
                    add_line(F, a, a);
                else
                    wrap_line(F, a, i, l_max(F->cw - 2, 4));
                a = i + 1;
            }
        }
    }
    return true;
}

static void field_close(Field *F)
{
    au_buf_free(&F->own);
    au_free(F->lines);
}

/* The line holding display offset d (at a soft break, the later line). */
static uint32_t line_of(const Field *F, uint32_t d)
{
    uint32_t r = 0;
    for (uint32_t i = 0; i < F->nlines && F->lines[i].a <= d; i++)
        r = i;
    return r;
}

/* The text origin: where line 0 starts, relative to the border box. */
static void text_origin(const Field *F, float *x, float *y)
{
    *x = F->cx + 1 - F->ctl->scroll_x;
    if (F->multi)
        *y = F->cy - F->ctl->scroll_y;
    else
        *y = F->cy + (F->ch - F->lh) / 2;
}

/* The display offset nearest x within line li. */
static uint32_t offset_in_line(Field *F, uint32_t li, float x)
{
    const FLine *l = &F->lines[li];
    if (x <= 0)
        return l->a;
    float prev = 0;
    uint32_t i = l->a;
    while (i < l->b) {
        uint32_t j = next_char(F->t, l->b, i);
        float w = width_of(F, l->a, j);
        if (x < (prev + w) / 2)
            return i;
        prev = w;
        i = j;
    }
    return l->b;
}

void au_layout_set_focus(AuLayout *L, AuElement *el, bool caret_on)
{
    L->focus = el;
    L->caret_on = caret_on;
}

bool au_layout_field_offset_at(AuLayout *L, AuElement *el, float lx, float ly, uint32_t *off)
{
    Field F;
    if (!field_open(&F, L, el))
        return false;
    float ox, oy;
    text_origin(&F, &ox, &oy);
    int li = (int)floorf((ly - oy) / (F.lh > 0 ? F.lh : 1));
    if (li < 0)
        li = 0;
    if (li >= (int)F.nlines)
        li = (int)F.nlines - 1;
    if (li < 0)
        li = 0;
    uint32_t d = F.nlines ? offset_in_line(&F, (uint32_t)li, lx - ox) : 0;
    *off = to_value(&F, d);
    field_close(&F);
    return true;
}

/* Caret position (border-box relative) of value offset off. */
static void caret_xy(Field *F, uint32_t off, float *x, float *y)
{
    uint32_t d = to_disp(F, off);
    uint32_t li = line_of(F, d);
    float ox, oy;
    text_origin(F, &ox, &oy);
    *x = ox + width_of(F, F->lines[li].a, d < F->lines[li].a ? F->lines[li].a : d);
    *y = oy + li * F->lh;
}

bool au_layout_field_caret_rect(AuLayout *L, AuElement *el, uint32_t off, AuRect *out)
{
    Field F;
    if (!field_open(&F, L, el))
        return false;
    float x, y, bx, by;
    caret_xy(&F, off, &x, &y);
    l_box_origin(&F.b->b, &bx, &by);
    *out = (AuRect){bx + x - 1, by + y, 3, F.lh};
    field_close(&F);
    return true;
}

uint32_t au_layout_field_vmove(AuLayout *L, AuElement *el, uint32_t off, int lines, float *goal_x)
{
    Field F;
    if (!field_open(&F, L, el))
        return off;
    uint32_t r;
    if (!F.multi) {
        r = lines < 0 ? 0 : (uint32_t)F.vn;
    } else {
        uint32_t d = to_disp(&F, off);
        int li = (int)line_of(&F, d);
        float x = width_of(&F, F.lines[li].a, d);
        if (*goal_x >= 0)
            x = *goal_x;
        *goal_x = x;
        int to = li + lines;
        if (to < 0)
            r = 0;
        else if (to >= (int)F.nlines)
            r = (uint32_t)F.vn;
        else
            r = to_value(&F, offset_in_line(&F, (uint32_t)to, x));
    }
    field_close(&F);
    return r;
}

uint32_t au_layout_field_line_edge(AuLayout *L, AuElement *el, uint32_t off, int dir)
{
    Field F;
    if (!field_open(&F, L, el))
        return off;
    uint32_t li = line_of(&F, to_disp(&F, off));
    uint32_t r = to_value(&F, dir < 0 ? F.lines[li].a : F.lines[li].b);
    /* At a soft break the end is just before the next line's text: keep
     * the caret on this line by stopping before a trailing space. */
    if (dir > 0 && li + 1 < F.nlines && F.lines[li + 1].a == F.lines[li].b && r > 0 && F.v[r - 1] == ' ')
        r--;
    field_close(&F);
    return r;
}

bool au_layout_field_reveal(AuLayout *L, AuElement *el)
{
    Field F;
    if (!field_open(&F, L, el))
        return false;
    AuControl *c = F.ctl;
    float sx = c->scroll_x, sy = c->scroll_y;
    float x, y;
    caret_xy(&F, c->caret, &x, &y);
    if (!F.multi) {
        float left = F.cx, right = F.cx + F.cw - 2;
        if (x > right)
            c->scroll_x += x - right + F.cw / 3;
        else if (x < left)
            c->scroll_x -= left - x + F.cw / 3;
        float total = width_of(&F, 0, (uint32_t)F.n);
        float maxs = l_max(0, total + 3 - F.cw);
        if (c->scroll_x > maxs)
            c->scroll_x = maxs;
        if (c->scroll_x < 0)
            c->scroll_x = 0;
    } else {
        c->scroll_x = 0;
        if (y < F.cy)
            c->scroll_y -= F.cy - y;
        else if (y + F.lh > F.cy + F.ch)
            c->scroll_y += y + F.lh - (F.cy + F.ch);
        float maxs = l_max(0, F.nlines * F.lh - F.ch);
        if (c->scroll_y > maxs)
            c->scroll_y = maxs;
        if (c->scroll_y < 0)
            c->scroll_y = 0;
    }
    bool moved = sx != c->scroll_x || sy != c->scroll_y;
    field_close(&F);
    return moved;
}

bool au_layout_field_scroll(AuLayout *L, AuElement *el, float dy)
{
    Field F;
    if (!field_open(&F, L, el))
        return false;
    bool moved = false;
    if (F.multi) {
        float maxs = l_max(0, F.nlines * F.lh - F.ch);
        float s = l_max(0, l_min(maxs, F.ctl->scroll_y + dy));
        moved = s != F.ctl->scroll_y;
        F.ctl->scroll_y = s;
    }
    field_close(&F);
    return moved;
}

void l_paint_field(AuLayout *L, AuCanvas *c, LBlock *b, AuRect cb, AuRect pb, uint32_t color, uint32_t ph_color)
{
    AuElement *el = b->b.node && au_is_element(b->b.node) ? au_as_element(b->b.node) : NULL;
    Field F;
    if (!el || !field_open(&F, L, el))
        return;
    /* Clipped to the content box across (a textarea to its padding box),
     * the padding box down. */
    AuRect clip = F.multi ? pb : (AuRect){cb.x, pb.y, cb.w, pb.h};
    au_canvas_clip_push(c, clip);
    float bx = cb.x - F.cx, by = cb.y - F.cy; /* border-box origin, canvas */
    float ox, oy;
    text_origin(&F, &ox, &oy);
    bool focused = L->focus == el;
    AuControl *ctl = F.ctl;
    if (F.n == 0) {
        const char *ph = l_attr(&b->b, au_atom_lookup("placeholder", 11));
        if (ph && *ph) {
            size_t pn = strlen(ph);
            float y = by + oy;
            const char *p = ph;
            while (pn) {
                size_t k = 0;
                while (k < pn && p[k] != '\n' && p[k] != '\r')
                    k++;
                l_draw_text(L, c, F.f, F.s, p, k, bx + F.cx + 1, y + F.asc, ph_color, 0);
                if (!F.multi)
                    break;
                y += F.lh;
                p += k;
                pn -= k;
                if (pn) {
                    p++;
                    pn--;
                }
            }
        }
    }
    uint32_t sa = 0, se = 0;
    if (focused && ctl->anchor != ctl->caret) {
        uint32_t a = ctl->anchor < ctl->caret ? ctl->anchor : ctl->caret;
        uint32_t e = ctl->anchor < ctl->caret ? ctl->caret : ctl->anchor;
        sa = to_disp(&F, a);
        se = to_disp(&F, e);
    }
    uint32_t selc = L->sel.color ? L->sel.color : FIELD_SELECTION;
    for (uint32_t i = 0; i < F.nlines; i++) {
        float y = by + oy + i * F.lh;
        if (y + F.lh < clip.y || y > clip.y + clip.h)
            continue;
        const FLine *l = &F.lines[i];
        if (se > sa && se > l->a && sa <= l->b) {
            uint32_t a = sa > l->a ? sa : l->a, e = se < l->b ? se : l->b;
            float x0 = bx + ox + width_of(&F, l->a, a), x1 = bx + ox + width_of(&F, l->a, e);
            if (se > l->b && F.multi)
                x1 += F.f->m.space_advance / 2; /* the line break is selected */
            if (x1 > x0)
                au_fill_rect(c, (AuRect){x0, y, x1 - x0, F.lh}, selc);
        }
        if (l->b > l->a)
            l_draw_text(L, c, F.f, F.s, F.t + l->a, l->b - l->a, bx + ox, y + F.asc, color, 0);
    }
    if (focused && L->caret_on && ctl->anchor == ctl->caret) {
        float x, y;
        caret_xy(&F, ctl->caret, &x, &y);
        au_fill_rect(c, (AuRect){roundf(bx + x), roundf(by + y), 1, roundf(F.lh)}, color);
    }
    au_canvas_clip_pop(c);
    field_close(&F);
}

/* ---- List boxes and sliders ---- */

int au_layout_listbox_row_at(AuLayout *L, AuElement *el, float lx, float ly)
{
    LBox *bx = el ? (LBox *)el->box : NULL;
    if (!bx || bx->kind != LK_BLOCK || bx->replaced != LR_LISTBOX)
        return -1;
    LBlock *b = (LBlock *)bx;
    (void)lx;
    float rh = l_listbox_row(L, b);
    float y = ly - b->bw[0] - b->p[0];
    if (y < 0 || rh <= 0)
        return -1;
    AuControl *c = au_ctl_peek(el);
    int top = c ? (int)c->scroll_y : 0;
    int row = (int)(y / rh) + top;
    /* count the rows */
    int n = 0;
    const char *t = b->rep && b->rep->label ? b->rep->label : "";
    if (*t) {
        n = 1;
        for (; *t; t++)
            n += *t == '\n';
    }
    return row < n ? row : -1;
}

int au_layout_listbox_visible(AuLayout *L, AuElement *el)
{
    LBox *bx = el ? (LBox *)el->box : NULL;
    if (!bx || bx->kind != LK_BLOCK || bx->replaced != LR_LISTBOX)
        return 4;
    LBlock *b = (LBlock *)bx;
    float rh = l_listbox_row(L, b);
    float h = b->h - b->bw[0] - b->bw[2] - b->p[0] - b->p[2];
    int n = rh > 0 ? (int)(h / rh) : 4;
    return n > 0 ? n : 1;
}

float au_layout_range_frac_at(AuLayout *L, AuElement *el, float lx)
{
    (void)L;
    LBox *bx = el ? (LBox *)el->box : NULL;
    if (!bx || bx->kind != LK_BLOCK || bx->replaced != LR_RANGE)
        return -1;
    LBlock *b = (LBlock *)bx;
    float cx = b->bw[3] + b->p[3];
    float cw = b->w - b->bw[1] - b->bw[3] - b->p[1] - b->p[3];
    float tw = 11;
    if (cw <= tw)
        return 0;
    float f = (lx - cx - tw / 2) / (cw - tw);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}
