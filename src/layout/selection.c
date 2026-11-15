/* Text selection, range text and find. See selection.h. */
#include "selection.h"

#include "layout_int.h"

/* ---- Order ---- */

static int node_depth(const AuNode *n)
{
    int d = 0;
    while (n->parent) {
        n = n->parent;
        d++;
    }
    return d;
}

int au_node_order(const AuNode *a, const AuNode *b)
{
    if (a == b)
        return 0;
    int da = node_depth(a), db = node_depth(b);
    const AuNode *x = a, *y = b;
    while (da > db) {
        x = x->parent;
        da--;
    }
    while (db > da) {
        y = y->parent;
        db--;
    }
    if (x == y)
        return a == x ? -1 : 1; /* one is the other's ancestor */
    while (x->parent != y->parent) {
        x = x->parent;
        y = y->parent;
    }
    if (!x->parent)
        return a < b ? -1 : 1; /* different trees: any stable order */
    for (const AuNode *s = x->next; s; s = s->next)
        if (s == y)
            return -1;
    return 1;
}

int au_text_pos_cmp(const AuTextPos *a, const AuTextPos *b)
{
    if (!a->node || !b->node)
        return (a->node != NULL) - (b->node != NULL);
    int o = au_node_order(a->node, b->node);
    if (o)
        return o;
    return a->offset < b->offset ? -1 : a->offset > b->offset ? 1 : 0;
}

AuTextRange au_text_range_make(AuTextPos a, AuTextPos b)
{
    AuTextRange r;
    if (au_text_pos_cmp(&a, &b) <= 0) {
        r.start = a;
        r.end = b;
    } else {
        r.start = b;
        r.end = a;
    }
    return r;
}

/* ---- Box tree walks ---- */

/* The next box in tree order after b within root, entering children
 * unless skip_children. */
static LBox *next_box(LBox *b, LBox *root, bool skip_children)
{
    if (!skip_children && b->first && !(b->flags & LF_TOO_DEEP))
        return b->first;
    while (b && b != root) {
        if (b->next)
            return b->next;
        b = b->parent;
    }
    return NULL;
}

/* The block whose lines hold text box t (its IFC root). */
static LBlock *ifc_root(LBox *t)
{
    for (LBox *p = t->parent; p; p = p->parent)
        if (p->kind == LK_BLOCK && (p->flags & LF_INLINES))
            return (LBlock *)p;
    return NULL;
}

/* The first text box of node n, or NULL. */
static LText *text_box(AuLayout *L, const AuNode *n)
{
    if (!L->root || !n || n->type != AU_NODE_TEXT)
        return NULL;
    for (LBox *b = L->root; b; b = next_box(b, L->root, false))
        if (b->kind == LK_TEXT && b->node == n)
            return (LText *)b;
    return NULL;
}

/* ---- Positions from points ---- */

/* The offset in fragment f (text box t) nearest to fx pixels from its
 * left. */
static uint32_t frag_offset_at(AuLayout *L, const LFrag *f, float fx)
{
    LText *t = (LText *)f->box;
    LFont *font = l_font(L, f->box->style);
    const char *s = t->text + f->start;
    uint32_t k = 0;
    float prev = 0;
    /* A click on a line of 90,000 characters without a break took
     * minutes without this (see l_prefix_seek). */
    k = l_prefix_seek(L, font, f->box->style, s, f->len, fx, &prev);
    while (k < f->len) {
        size_t adv;
        au_utf8_decode(s + k, f->len - k, &adv);
        float w = l_measure(L, font, f->box->style, s, k + (uint32_t)adv);
        if (w > fx) {
            if (fx - prev > (w - prev) / 2)
                k += (uint32_t)adv;
            return f->start + k;
        }
        prev = w;
        k += (uint32_t)adv;
    }
    return f->start + f->len;
}

bool au_layout_text_pos_at(AuLayout *L, float x, float y, float scroll_x, float scroll_y, AuTextPos *out)
{
    out->node = NULL;
    out->offset = 0;
    if (!L->root)
        return false;
    AuHitResult hr;
    if (au_layout_hit_test(L, x, y, scroll_x, scroll_y, &hr) && hr.node && hr.node->type == AU_NODE_TEXT) {
        out->node = hr.node;
        out->offset = (uint32_t)hr.text_offset;
        return true;
    }
    /* The nearest place on the nearest line, in document coordinates. */
    float dx = x + scroll_x, dy = y + scroll_y;
    float best_v = 1e30f, best_h = 1e30f;
    for (LBox *b = L->root; b; b = next_box(b, L->root, false)) {
        if (b->kind != LK_BLOCK || !(b->flags & LF_INLINES))
            continue;
        LBlock *k = (LBlock *)b;
        if (!k->nlines)
            continue;
        float ox, oy;
        l_box_origin(b, &ox, &oy);
        for (uint32_t li = 0; li < k->nlines; li++) {
            LLine *ln = &k->lines[li];
            float top = oy + ln->y, bot = top + ln->h;
            float vd = dy < top ? top - dy : dy >= bot ? dy - bot + 0.01f : 0;
            if (vd > best_v)
                continue;
            for (uint32_t i = ln->first; i < ln->first + ln->n; i++) {
                LFrag *f = &k->frags[i];
                if (f->kind != FR_TEXT || (f->flags & FRF_HIDDEN))
                    continue;
                float fl = ox + f->x, fr = fl + f->w;
                float hd = dx < fl ? fl - dx : dx > fr ? dx - fr : 0;
                /* Vertical distance first: a point beside a line picks
                 * that line. */
                if (vd < best_v || (vd == best_v && hd < best_h)) {
                    best_v = vd;
                    best_h = hd;
                    out->node = f->box->node;
                    out->offset = dx <= fl ? f->start : dx >= fr ? f->start + f->len : frag_offset_at(L, f, dx - fl);
                }
            }
        }
    }
    return out->node != NULL;
}

/* ---- Words and blocks ---- */

static bool word_char(uint32_t cp)
{
    if (cp < 0x80)
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_' ||
               cp == '\'';
    /* Spaces and general punctuation are not; other letters are. */
    if (cp == 0xA0 || (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xA1 && cp <= 0xBF))
        return false;
    return true;
}

/* The start of the UTF-8 character before offset i. */
static uint32_t prev_char(const char *s, uint32_t i)
{
    if (!i)
        return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    return i;
}

bool au_layout_word_at(AuLayout *L, const AuTextPos *p, AuTextRange *out)
{
    LText *t = text_box(L, p->node);
    if (!t || !t->len)
        return false;
    uint32_t o = p->offset > t->len ? t->len : p->offset;
    uint32_t a = o, b = o;
    size_t adv;
    bool here = o < t->len && word_char(au_utf8_decode(t->text + o, t->len - o, &adv));
    bool before = o > 0 && word_char(au_utf8_decode(t->text + prev_char(t->text, o), t->len - prev_char(t->text, o), &adv));
    if (here || before) {
        while (a > 0) {
            uint32_t q = prev_char(t->text, a);
            if (!word_char(au_utf8_decode(t->text + q, t->len - q, &adv)))
                break;
            a = q;
        }
        while (b < t->len) {
            uint32_t c = au_utf8_decode(t->text + b, t->len - b, &adv);
            if (!word_char(c))
                break;
            b += (uint32_t)adv;
        }
    } else if (o < t->len) {
        au_utf8_decode(t->text + o, t->len - o, &adv);
        b = o + (uint32_t)adv; /* the character itself: a space or a mark */
    } else {
        a = prev_char(t->text, o);
    }
    out->start.node = out->end.node = p->node;
    out->start.offset = a;
    out->end.offset = b;
    return true;
}

bool au_layout_block_at(AuLayout *L, const AuTextPos *p, AuTextRange *out)
{
    LText *t = text_box(L, p->node);
    LBlock *root = t ? ifc_root(&t->b) : NULL;
    if (!root)
        return false;
    bool any = false;
    for (uint32_t i = 0; i < root->nfrags; i++) {
        LFrag *f = &root->frags[i];
        if (f->kind != FR_TEXT)
            continue;
        AuTextPos s = {f->box->node, f->start}, e = {f->box->node, f->start + f->len};
        if (!any || au_text_pos_cmp(&s, &out->start) < 0)
            out->start = s;
        if (!any || au_text_pos_cmp(&e, &out->end) > 0)
            out->end = e;
        any = true;
    }
    return any;
}

bool au_layout_all_text(AuLayout *L, AuTextRange *out)
{
    LText *first = NULL, *last = NULL;
    if (!L->root)
        return false;
    for (LBox *b = L->root; b; b = next_box(b, L->root, false))
        if (b->kind == LK_TEXT && ((LText *)b)->len) {
            if (!first)
                first = (LText *)b;
            last = (LText *)b;
        }
    if (!first)
        return false;
    out->start.node = first->b.node;
    out->start.offset = 0;
    out->end.node = last->b.node;
    out->end.offset = last->len;
    return true;
}

/* ---- Range text ----
 * One walk over the box tree; text boxes in the range give their slice,
 * blocks give line breaks and table cells tabs. segment() also reports
 * where each slice landed in the output, for find. */

typedef struct Seg {
    size_t at;        /* offset in the output */
    AuNode *node;
    uint32_t from, len; /* slice of the node's processed text */
} Seg;

typedef struct Walk {
    AuBuf *out;
    Seg *segs;
    size_t nsegs, cap;
    bool collect; /* record segments */
    int pending;  /* 0 none, 1 tab, 2 line break */
} Walk;

static void want_break(Walk *w, int kind)
{
    if (kind > w->pending)
        w->pending = kind;
}

static void put_text(Walk *w, AuNode *node, const char *s, uint32_t from, uint32_t to)
{
    if (to <= from)
        return;
    if (w->pending && w->out->len) {
        char last = w->out->data[w->out->len - 1];
        if (w->pending == 2 && last != '\n')
            au_buf_putc(w->out, '\n');
        else if (w->pending == 1 && last != '\n' && last != '\t')
            au_buf_putc(w->out, '\t');
    }
    w->pending = 0;
    if (w->collect) {
        if (w->nsegs == w->cap) {
            w->cap = w->cap ? w->cap * 2 : 64;
            w->segs = au_realloc(w->segs, w->cap * sizeof *w->segs);
        }
        Seg *g = &w->segs[w->nsegs++];
        g->at = w->out->len;
        g->node = node;
        g->from = from;
        g->len = to - from;
    }
    au_buf_append(w->out, s + from, to - from);
}

static bool block_level(const LBox *b)
{
    return b->kind == LK_BLOCK && !(b->flags & LF_INLINE_LEVEL);
}

/* Walks r (NULL: everything) into w. */
static void walk_range(AuLayout *L, const AuTextRange *r, Walk *w)
{
    if (!L->root)
        return;
    bool started = r == NULL;
    LBox *b = L->root;
    bool descend = true;
    while (b) {
        if (descend) {
            if (block_level(b))
                want_break(w, b->inner == LI_TABLE_CELL ? 1 : 2);
            if (b->inner == LI_TABLE_ROW)
                want_break(w, 2);
            if (b->kind == LK_BR && started) {
                au_buf_putc(w->out, '\n'); /* each <br> is a line, even an empty one */
                w->pending = 0;
            }
            if (b->kind == LK_TEXT && !(b->flags & LF_TOO_DEEP)) {
                LText *t = (LText *)b;
                uint32_t lo = 0, hi = t->len;
                bool in = true;
                if (r) {
                    if (!started) {
                        int o = au_node_order(b->node, r->start.node);
                        if (o < 0)
                            in = false;
                        else {
                            started = true;
                            if (o == 0)
                                lo = r->start.offset;
                        }
                    }
                    if (in) {
                        int o = au_node_order(b->node, r->end.node);
                        if (o > 0)
                            return;
                        if (o == 0)
                            hi = r->end.offset < t->len ? r->end.offset : t->len;
                    }
                }
                if (in)
                    put_text(w, b->node, t->text, lo, hi);
                if (r && b->node == r->end.node)
                    return;
            }
            if (b->first && !(b->flags & LF_TOO_DEEP)) {
                b = b->first;
                continue;
            }
        }
        if (block_level(b))
            want_break(w, b->inner == LI_TABLE_CELL ? 1 : 2);
        if (b == L->root)
            break;
        if (b->next) {
            b = b->next;
            descend = true;
        } else {
            b = b->parent;
            descend = false;
        }
    }
}

void au_layout_range_text(AuLayout *L, const AuTextRange *r, AuBuf *out)
{
    if (!r || au_text_range_empty(r))
        return;
    Walk w;
    memset(&w, 0, sizeof w);
    w.out = out;
    walk_range(L, r, &w);
}

bool au_layout_range_rect(AuLayout *L, const AuTextRange *r, AuRect *out)
{
    if (!L->root || !r || !r->start.node)
        return false;
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (LBox *b = L->root; b; b = next_box(b, L->root, false)) {
        if (b->kind != LK_BLOCK || !(b->flags & LF_INLINES))
            continue;
        LBlock *k = (LBlock *)b;
        float ox = 0, oy = 0;
        bool have_origin = false;
        for (uint32_t i = 0; i < k->nfrags; i++) {
            LFrag *f = &k->frags[i];
            if (f->kind != FR_TEXT)
                continue;
            AuTextPos s = {f->box->node, f->start}, e = {f->box->node, f->start + f->len};
            if (au_text_pos_cmp(&e, &r->start) <= 0 || au_text_pos_cmp(&s, &r->end) >= 0)
                continue;
            if (!have_origin) {
                l_box_origin(b, &ox, &oy);
                have_origin = true;
            }
            float fx0 = ox + f->x, fy0 = oy + f->y, fx1 = fx0 + f->w, fy1 = fy0 + f->h;
            if (!any) {
                x0 = fx0, y0 = fy0, x1 = fx1, y1 = fy1;
                any = true;
            } else {
                x0 = l_min(x0, fx0), y0 = l_min(y0, fy0), x1 = l_max(x1, fx1), y1 = l_max(y1, fy1);
            }
        }
    }
    if (any)
        *out = (AuRect){x0, y0, x1 - x0, y1 - y0};
    return any;
}

/* ---- The painted selection ---- */

static float frag_width(AuLayout *L, const LFrag *f, LFont *font, const char *s, uint32_t n);

int au_layout_range_rects(AuLayout *L, const AuTextRange *r, AuRect *out, int max)
{
    if (!L->root || !r || !r->start.node)
        return 0;
    int n = 0;
    for (LBox *b = L->root; b; b = next_box(b, L->root, false)) {
        if (b->kind != LK_BLOCK || !(b->flags & LF_INLINES))
            continue;
        LBlock *k = (LBlock *)b;
        float ox = 0, oy = 0;
        bool have_origin = false;
        for (uint32_t i = 0; i < k->nfrags; i++) {
            LFrag *f = &k->frags[i];
            if (f->kind != FR_TEXT || !f->box->node)
                continue;
            int a = au_node_order(f->box->node, r->start.node), e = au_node_order(f->box->node, r->end.node);
            if (a < 0 || e > 0)
                continue;
            uint32_t lo = a == 0 ? r->start.offset : 0, hi = e == 0 ? r->end.offset : UINT32_MAX;
            uint32_t fs = f->start, fe = f->start + f->len;
            if (lo < fs)
                lo = fs;
            if (hi > fe)
                hi = fe;
            /* An empty range still has a place: a caret at its offset. */
            bool caret = a == 0 && e == 0 && r->start.offset == r->end.offset;
            if (lo > hi || (lo == hi && !caret))
                continue;
            if (caret && (r->start.offset < fs || r->start.offset > fe))
                continue;
            if (!have_origin) {
                l_box_origin(b, &ox, &oy);
                have_origin = true;
            }
            LFont *font = l_font(L, f->box->style);
            const char *s = ((LText *)f->box)->text + fs;
            float x0 = frag_width(L, f, font, s, lo - fs);
            float x1 = hi == fe ? f->w : frag_width(L, f, font, s, hi - fs);
            if (n < max)
                out[n] = (AuRect){ox + f->x + x0, oy + f->y, l_max(0, x1 - x0), f->h};
            n++;
            if (caret)
                return n; /* the first fragment that holds it */
        }
    }
    return n;
}

/* The source text of a text box and the map from its bytes to the
 * box's processed text. The map (len + 1 entries) is allocated; false
 * when n is not a text node with a box. */
static bool source_map(AuLayout *L, const AuNode *n, LText **box, uint32_t **map)
{
    LText *t = text_box(L, n);
    if (!t)
        return false;
    const AuText *src = au_as_text((AuNode *)n);
    uint32_t *m = au_alloc(((size_t)src->len + 1) * sizeof *m);
    AuBuf out;
    au_buf_init(&out);
    /* The box does not record whether the content before it ended in a
     * collapsible space: the processing that gives the box's text does. */
    bool ok = false;
    for (int first = 0; first < 2 && !ok; first++) {
        bool last_space = first == 1;
        l_text_process(t->b.style, src->data, src->len, &last_space, &out, m);
        ok = out.len == t->len && (t->len == 0 || !memcmp(out.data, t->text, t->len));
    }
    au_buf_free(&out);
    if (!ok) {
        /* The box is out of date (the node's data changed since the tree
         * was built). */
        au_free(m);
        return false;
    }
    *box = t;
    *map = m;
    return true;
}

bool au_layout_text_pos_from_dom(AuLayout *L, AuNode *text, uint32_t utf16_offset, AuTextPos *out)
{
    LText *t;
    uint32_t *map;
    if (!source_map(L, text, &t, &map))
        return false;
    const AuText *src = au_as_text(text);
    size_t i = 0;
    uint32_t units = 0;
    while (i < src->len && units < utf16_offset) {
        size_t adv;
        uint32_t c = au_utf8_decode(src->data + i, src->len - i, &adv);
        uint32_t w = c >= 0x10000 ? 2 : 1;
        if (units + w > utf16_offset)
            break; /* between the halves of a surrogate pair: its start */
        units += w;
        i += adv;
    }
    if (i > src->len)
        i = src->len;
    out->node = text;
    out->offset = map[i];
    au_free(map);
    return true;
}

bool au_layout_text_pos_to_dom(AuLayout *L, const AuTextPos *p, uint32_t *utf16_offset)
{
    LText *t;
    uint32_t *map;
    if (!p || !p->node || !source_map(L, p->node, &t, &map))
        return false;
    const AuText *src = au_as_text(p->node);
    /* The first source character that comes out at or after the offset. */
    size_t i = 0;
    uint32_t units = 0;
    while (i < src->len && map[i] < p->offset) {
        size_t adv;
        uint32_t c = au_utf8_decode(src->data + i, src->len - i, &adv);
        units += c >= 0x10000 ? 2 : 1;
        i += adv;
    }
    au_free(map);
    *utf16_offset = units;
    return true;
}

void au_layout_set_selection(AuLayout *L, const AuTextRange *r, uint32_t argb)
{
    if (!r || au_text_range_empty(r)) {
        if (L->sel.on)
            L->need_paint = true;
        memset(&L->sel, 0, sizeof L->sel);
        return;
    }
    L->sel.n0 = r->start.node;
    L->sel.o0 = r->start.offset;
    L->sel.n1 = r->end.node;
    L->sel.o1 = r->end.offset;
    L->sel.color = argb;
    L->sel.on = true;
    L->need_paint = true;
}

bool au_layout_get_selection(const AuLayout *L, AuTextRange *out)
{
    if (!L->sel.on)
        return false;
    out->start.node = L->sel.n0;
    out->start.offset = L->sel.o0;
    out->end.node = L->sel.n1;
    out->end.offset = L->sel.o1;
    return true;
}

/* Width of the first n bytes of fragment text s, with its justification
 * spacing. */
static float frag_width(AuLayout *L, const LFrag *f, LFont *font, const char *s, uint32_t n)
{
    float w = n ? l_measure(L, font, f->box->style, s, n) : 0;
    if (f->spacing)
        for (uint32_t i = 0; i < n; i++)
            if (s[i] == ' ')
                w += f->spacing;
    return w;
}

void l_select_paint_frag(AuLayout *L, AuCanvas *c, const LFrag *f, LFont *font, float x, float y)
{
    AuNode *n = f->box->node;
    if (!n)
        return;
    int a = au_node_order(n, L->sel.n0);
    if (a < 0)
        return;
    int b = au_node_order(n, L->sel.n1);
    if (b > 0)
        return;
    uint32_t lo = a == 0 ? L->sel.o0 : 0, hi = b == 0 ? L->sel.o1 : UINT32_MAX;
    uint32_t fs = f->start, fe = f->start + f->len;
    if (lo < fs)
        lo = fs;
    if (hi > fe)
        hi = fe;
    if (lo >= hi)
        return;
    const char *s = ((LText *)f->box)->text + fs;
    float x0 = frag_width(L, f, font, s, lo - fs);
    float x1 = hi == fe ? f->w : frag_width(L, f, font, s, hi - fs);
    if (x1 <= x0)
        return;
    au_fill_rect(c, (AuRect){x + x0, y, x1 - x0, f->h}, L->sel.color);
}

/* ---- Find ---- */

/* Simple case folding that keeps the UTF-8 length: ASCII, Latin-1,
 * Greek and Cyrillic capitals to small letters. */
static uint32_t fold(uint32_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        /* Latin Extended-A pairs alternate capital, small (with two runs
         * shifted by one). */
        bool odd_caps = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (odd_caps ? (c & 1) : !(c & 1))
            return c + 1;
    }
    return c;
}

static void fold_buf(char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        size_t adv;
        uint32_t c = au_utf8_decode(s + i, n - i, &adv);
        uint32_t f = fold(c);
        if (f != c) {
            char tmp[4];
            int k = au_utf8_encode(f, tmp);
            if ((size_t)k == adv)
                memcpy(s + i, tmp, adv);
        }
        i += adv;
    }
}

/* The position at output offset at (the end of a match when is_end). */
static AuTextPos pos_of(const Walk *w, size_t at, bool is_end)
{
    AuTextPos p = {NULL, 0};
    size_t lo = 0, hi = w->nsegs;
    /* The last segment starting at or before at (for an end: before). */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        bool ok = is_end ? w->segs[mid].at < at : w->segs[mid].at <= at;
        if (ok)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return w->nsegs ? (AuTextPos){w->segs[0].node, w->segs[0].from} : p;
    const Seg *g = &w->segs[lo - 1];
    size_t d = at - g->at;
    if (d > g->len)
        d = g->len; /* in a break between segments */
    p.node = g->node;
    p.offset = g->from + (uint32_t)d;
    return p;
}

/* The output offset of position p (first offset at or after it). */
static size_t offset_of(const Walk *w, const AuTextPos *p, AuBuf *out)
{
    for (size_t i = 0; i < w->nsegs; i++) {
        const Seg *g = &w->segs[i];
        AuTextPos e = {g->node, g->from + g->len};
        if (au_text_pos_cmp(&e, p) < 0)
            continue;
        AuTextPos s = {g->node, g->from};
        if (au_text_pos_cmp(p, &s) <= 0)
            return g->at;
        return g->at + (p->offset - g->from);
    }
    return out->len;
}

bool au_layout_find(AuLayout *L, const char *needle, const AuTextPos *from, bool forward, bool case_insensitive,
                    AuTextRange *out)
{
    size_t nn = needle ? strlen(needle) : 0;
    if (!nn || !L->root)
        return false;
    AuBuf text, pat;
    au_buf_init(&text);
    au_buf_init(&pat);
    Walk w;
    memset(&w, 0, sizeof w);
    w.out = &text;
    w.collect = true;
    walk_range(L, NULL, &w);
    au_buf_append(&pat, needle, nn);
    if (case_insensitive) {
        fold_buf(text.data ? text.data : (char *)"", text.len);
        fold_buf(pat.data, pat.len);
    }
    bool found = false;
    if (text.len >= nn) {
        size_t start = from && from->node ? offset_of(&w, from, &text) : (forward ? 0 : text.len);
        size_t last = text.len - nn;
        size_t hit = (size_t)-1;
        if (forward) {
            for (size_t pass = 0; pass < 2 && hit == (size_t)-1; pass++) {
                size_t a = pass ? 0 : start, b = pass ? (start < last + 1 ? start : last + 1) : last + 1;
                for (size_t i = a; i < b; i++)
                    if (text.data[i] == pat.data[0] && !memcmp(text.data + i, pat.data, nn)) {
                        hit = i;
                        break;
                    }
            }
        } else {
            for (size_t pass = 0; pass < 2 && hit == (size_t)-1; pass++) {
                /* Matches ending at or before start, then (wrapping) the rest. */
                size_t top = pass ? last + 1 : (start >= nn ? start - nn + 1 : 0);
                size_t bottom = pass ? (start >= nn ? start - nn + 1 : 0) : 0;
                for (size_t i = top; i-- > bottom;)
                    if (text.data[i] == pat.data[0] && !memcmp(text.data + i, pat.data, nn)) {
                        hit = i;
                        break;
                    }
            }
        }
        if (hit != (size_t)-1) {
            out->start = pos_of(&w, hit, false);
            out->end = pos_of(&w, hit + nn, true);
            found = out->start.node && out->end.node;
        }
    }
    au_free(w.segs);
    au_buf_free(&text);
    au_buf_free(&pat);
    return found;
}
