/* Inline formatting contexts: segmentation at break opportunities, line
 * breaking, line boxes with the strut, vertical-align, text-align
 * (justify included), text-indent, white-space modes, tabs, soft hyphens,
 * overflow-wrap and word-break, floats and absolutely positioned boxes
 * among inline content, text-overflow: ellipsis and -webkit-line-clamp.
 * Bidi is not implemented: all text is laid out left to right.
 *
 * The inline content of an IFC root is first flattened into segments: a
 * segment is a run of text up to and including a break opportunity (with
 * its trailing collapsible spaces measured separately), the start or end
 * edge of an inline box, an atomic inline, a forced break, a <wbr>, or an
 * out-of-flow box. Line breaking is greedy over segments; fragments are
 * then built per line. Scratch segments live on a stack in AuLayout so
 * that atomic inlines laid out during segmentation can reuse it. */
#include "layout_int.h"

enum { IT_TEXT, IT_OPEN, IT_CLOSE, IT_ATOMIC, IT_BR, IT_WBR, IT_FLOAT, IT_ABS };
enum {
    SF_BREAK = 1,        /* a break opportunity after this segment */
    SF_FORCED = 2,       /* a forced break after it */
    SF_SHY = 4,          /* ends at a soft hyphen */
    SF_BREAK_BEFORE = 8, /* a break opportunity before it */
    SF_TAB = 16,         /* a preserved tab */
    SF_CONTENT = 32,     /* makes the line non-empty */
    SF_SPLITTABLE = 64,  /* may break between any characters if it does not fit */
    SF_PRESERVED = 128,  /* trailing spaces are preserved (pre-wrap): they hang */
    SF_PLACED = SF_TAB   /* IT_FLOAT: already placed (a line that is retried lower must not place it again) */
};

struct LSeg {
    LBox *box;
    uint32_t start, end; /* text: bytes [start, end), trailing spaces included */
    uint16_t trail;      /* text: bytes of trailing spaces */
    uint8_t kind, flags;
    uint16_t depth;
    float w, trail_w, wmin;
};

#define INLINE_MAX_DEPTH 64

static uint32_t seg_push(AuLayout *L)
{
    if (L->nsegs == L->segs_cap) {
        L->segs_cap = L->segs_cap ? L->segs_cap * 2 : 256;
        L->segs = au_realloc(L->segs, L->segs_cap * sizeof *L->segs);
    }
    LSeg *s = &L->segs[L->nsegs];
    memset(s, 0, sizeof *s);
    return L->nsegs++;
}

static bool wraps(const AuStyle *s)
{
    return s->inh->text_wrap_mode == AU_TEXT_WRAP_MODE_WRAP;
}

static bool preserves_spaces(const AuStyle *s)
{
    int c = s->inh->white_space_collapse;
    return c == AU_WHITE_SPACE_COLLAPSE_PRESERVE || c == AU_WHITE_SPACE_COLLAPSE_BREAK_SPACES ||
           c == AU_WHITE_SPACE_COLLAPSE_PRESERVE_SPACES;
}

static bool preserves_breaks(const AuStyle *s)
{
    int c = s->inh->white_space_collapse;
    return c == AU_WHITE_SPACE_COLLAPSE_PRESERVE || c == AU_WHITE_SPACE_COLLAPSE_BREAK_SPACES ||
           c == AU_WHITE_SPACE_COLLAPSE_PRESERVE_BREAKS;
}

static bool is_alnum(uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c < 0x2000);
}

/* Horizontal edge (margin + border + padding) of an inline box side. */
static float inline_edge(const LBox *b, int side, float cb_w)
{
    const AuStyleSurround *s = b->style->surr;
    float m = s->margin[side].kind == AU_LEN_AUTO ? 0 : l_len(&s->margin[side], cb_w);
    return m + s->border_width[side] + l_max(0, l_len(&s->padding[side], cb_w));
}

static bool inline_has_edges(const LBox *b, float cb_w)
{
    const AuStyleSurround *s = b->style->surr;
    for (int i = 0; i < 4; i++)
        if (s->border_width[i] > 0 || l_len(&s->padding[i], cb_w) > 0 ||
            (s->margin[i].kind != AU_LEN_AUTO && l_len(&s->margin[i], cb_w) != 0 && (i == 1 || i == 3)))
            return true;
    return false;
}

/* Splits one text box into segments. */
static void text_segments(AuLayout *L, LText *t, uint16_t depth)
{
    const AuStyle *s = t->b.style;
    LFont *f = l_font(L, s);
    bool wrap = wraps(s);
    bool pre_sp = preserves_spaces(s), pre_br = preserves_breaks(s);
    bool break_spaces = s->inh->white_space_collapse == AU_WHITE_SPACE_COLLAPSE_BREAK_SPACES;
    int wb = s->inhr->word_break, ow = s->inhr->overflow_wrap;
    bool break_all = wrap && (wb == AU_WORD_BREAK_BREAK_ALL || s->inhr->line_break == AU_LINE_BREAK_ANYWHERE);
    bool keep_all = wb == AU_WORD_BREAK_KEEP_ALL;
    bool splittable = wrap && (ow != AU_OVERFLOW_WRAP_NORMAL || wb == AU_WORD_BREAK_BREAK_WORD);
    const char *x = t->text;
    uint32_t n = t->len;
    uint32_t i = 0;
    uint32_t prev_cp = 0;
    while (i < n) {
        uint32_t si = seg_push(L);
        LSeg *sg = &L->segs[si];
        sg->box = &t->b;
        sg->kind = IT_TEXT;
        sg->depth = depth;
        sg->start = i;
        if (splittable)
            sg->flags |= SF_SPLITTABLE;
        uint32_t word_end = i;
        bool ended = false;
        /* Word part. */
        while (i < n) {
            size_t adv;
            uint32_t cp = au_utf8_decode(x + i, n - i, &adv);
            int bk = l_break_class(cp);
            if (bk == BK_SPACE && !break_spaces)
                break;
            if (bk == BK_NEWLINE && pre_br) {
                word_end = i;
                i += (uint32_t)adv;
                sg->flags |= SF_FORCED | SF_CONTENT;
                ended = true;
                break;
            }
            if (bk == BK_TAB && pre_sp) {
                if (i > sg->start) {
                    word_end = i;
                    ended = true;
                    break; /* the tab gets its own segment */
                }
                i += (uint32_t)adv;
                word_end = i;
                sg->flags |= SF_TAB | SF_CONTENT | (wrap ? SF_BREAK : 0);
                ended = true;
                break;
            }
            if (bk == BK_TAB)
                break;
            /* A break before an ideograph (not after an opening bracket). */
            if (wrap && !keep_all && bk == BK_IDEO && i > sg->start && l_break_class(prev_cp) != BK_OPEN) {
                word_end = i;
                sg->flags |= SF_BREAK;
                ended = true;
                break;
            }
            if (break_all && i > sg->start && bk != BK_CLOSE && l_break_class(prev_cp) != BK_OPEN) {
                word_end = i;
                sg->flags |= SF_BREAK;
                ended = true;
                break;
            }
            i += (uint32_t)adv;
            word_end = i;
            if (cp != 0x200B && cp != 0xAD)
                sg->flags |= SF_CONTENT;
            prev_cp = cp;
            if (!wrap)
                continue;
            if (bk == BK_SHY || bk == BK_ZWSP) {
                sg->flags |= SF_BREAK | (bk == BK_SHY ? SF_SHY : 0);
                ended = true;
                break;
            }
            if (bk == BK_SPACE && break_spaces) {
                sg->flags |= SF_BREAK;
                ended = true;
                break;
            }
            if (bk == BK_IDEO && !keep_all) {
                /* After an ideograph, unless closing punctuation follows. */
                if (i < n) {
                    size_t a2;
                    uint32_t nx = au_utf8_decode(x + i, n - i, &a2);
                    int nb = l_break_class(nx);
                    if (nb == BK_CLOSE || nb == BK_SPACE || nb == BK_NEWLINE)
                        continue;
                }
                sg->flags |= SF_BREAK;
                ended = true;
                break;
            }
            if (bk == BK_HYPHEN && i < n && is_alnum(prev_cp)) {
                size_t a2;
                uint32_t nx = au_utf8_decode(x + i, n - i, &a2);
                (void)a2;
                /* the character before the hyphen was alnum: check two back */
                if (is_alnum(nx)) {
                    sg->flags |= SF_BREAK;
                    ended = true;
                    break;
                }
            }
            if (bk == BK_HYPHEN)
                prev_cp = '-';
        }
        /* Trailing spaces (collapsible, or preserved in pre-wrap). */
        if (!ended || (sg->flags & (SF_FORCED | SF_TAB)) == 0) {
            uint32_t sp0 = i;
            while (i < n && x[i] == ' ' && !break_spaces)
                i++;
            if (i > sp0) {
                sg->trail = (uint16_t)(i - sp0 > 65535 ? 65535 : i - sp0);
                if (i - sp0 > 65535)
                    i = sp0 + 65535;
                if (wrap)
                    sg->flags |= SF_BREAK;
                if (pre_sp) {
                    sg->flags |= SF_CONTENT;
                    if (wrap)
                        sg->flags |= SF_PRESERVED;
                }
                word_end = sp0;
            }
        }
        sg->end = i;
        if (sg->flags & SF_TAB) {
            sg->w = 0; /* set at placement */
        } else {
            uint32_t wl = word_end - sg->start;
            /* A forced newline is not measured. */
            sg->w = wl ? l_measure(L, f, s, x + sg->start, wl) : 0;
            /* Trailing spaces stay separate in nowrap too (no break
             * opportunity there), so that they are removed at the end of
             * the line. */
            if (sg->trail)
                sg->trail_w = l_measure(L, f, s, x + word_end, sg->trail);
        }
        sg->wmin = sg->w;
        if (i == sg->start)
            i++; /* defensive: always progress */
    }
}

typedef struct InlineCtx {
    AuLayout *L;
    LBlock *root;
    float cw;
    bool measure; /* intrinsic sizing: atomics give min/max, nothing is laid out */
} InlineCtx;

/* Flattens the inline content of root into segments from L->segs[base]. */
static void build_segments(InlineCtx *C)
{
    AuLayout *L = C->L;
    LBox *b = C->root->b.first;
    uint16_t depth = 0;
    uint16_t real_depth = 0; /* inline nesting including transparent levels */
    while (b) {
        bool descend = false;
        switch (b->kind) {
        case LK_TEXT:
            text_segments(L, (LText *)b, depth);
            break;
        case LK_INLINE:
            if (depth < INLINE_MAX_DEPTH) {
                uint32_t si = seg_push(L);
                LSeg *s = &L->segs[si];
                s->box = b;
                s->kind = IT_OPEN;
                s->depth = depth;
                s->w = (b->flags & LF_SPLIT_HEAD) ? 0 : inline_edge(b, 3, C->measure ? 0 : C->cw);
                if (inline_has_edges(b, C->measure ? 0 : C->cw))
                    s->flags |= SF_CONTENT;
                s->wmin = s->w;
                depth++;
            }
            real_depth++;
            descend = true;
            break;
        case LK_BR: {
            uint32_t si = seg_push(L);
            L->segs[si].box = b;
            L->segs[si].kind = IT_BR;
            L->segs[si].depth = depth;
            L->segs[si].flags = SF_FORCED | SF_CONTENT;
            break;
        }
        case LK_WBR: {
            uint32_t si = seg_push(L);
            L->segs[si].box = b;
            L->segs[si].kind = IT_WBR;
            L->segs[si].depth = depth;
            if (wraps(b->style))
                L->segs[si].flags = SF_BREAK;
            break;
        }
        case LK_BLOCK: {
            LBlock *k = (LBlock *)b;
            uint32_t si = seg_push(L);
            LSeg *s = &L->segs[si];
            s->box = b;
            s->depth = depth;
            if (b->flags & LF_ABSPOS) {
                s->kind = IT_ABS;
            } else if (b->flags & LF_FLOAT) {
                s->kind = IT_FLOAT;
                if (C->measure) {
                    float mn, mx;
                    l_outer_intrinsic(L, k, -1, &mn, &mx);
                    s = &L->segs[si];
                    s->w = mx;
                    s->wmin = mn;
                }
            } else if (b->flags & LF_MARKER_OUT) {
                s->kind = IT_ABS; /* outside markers are placed by the list item */
                s->flags = 0;
                s->box = NULL;
            } else {
                s->kind = IT_ATOMIC;
                bool wrap = b->parent && wraps(b->parent->style);
                s->flags = SF_CONTENT | (wrap ? SF_BREAK | SF_BREAK_BEFORE : 0);
                if (C->measure) {
                    float mn, mx;
                    l_outer_intrinsic(L, k, -1, &mn, &mx);
                    s = &L->segs[si];
                    s->w = mx;
                    s->wmin = mn;
                } else {
                    l_layout_independent(L, k, C->cw, -1, C->cw, LS_FIT);
                    s = &L->segs[si];
                    s->w = k->w + k->m[1] + k->m[3];
                    s->wmin = s->w;
                }
            }
            break;
        }
        }
        if (descend && b->first)
            b = b->first;
        else {
            if (descend) {
                /* empty inline box: close it now */
                if (real_depth <= INLINE_MAX_DEPTH) {
                    uint32_t si = seg_push(L);
                    LSeg *s = &L->segs[si];
                    s->box = b;
                    s->kind = IT_CLOSE;
                    depth--;
                    s->depth = depth;
                    s->w = (b->flags & LF_SPLIT_TAIL) ? 0 : inline_edge(b, 1, C->measure ? 0 : C->cw);
                    s->wmin = s->w;
                    if (inline_has_edges(b, C->measure ? 0 : C->cw))
                        s->flags |= SF_CONTENT;
                }
                real_depth--;
            }
            while (b && !b->next) {
                b = b->parent;
                if (!b || b == &C->root->b) {
                    b = NULL;
                    break;
                }
                /* leaving an inline box */
                if (real_depth <= INLINE_MAX_DEPTH) {
                    uint32_t si = seg_push(L);
                    LSeg *s = &L->segs[si];
                    s->box = b;
                    s->kind = IT_CLOSE;
                    depth--;
                    s->depth = depth;
                    s->w = (b->flags & LF_SPLIT_TAIL) ? 0 : inline_edge(b, 1, C->measure ? 0 : C->cw);
                    s->wmin = s->w;
                    if (inline_has_edges(b, C->measure ? 0 : C->cw))
                        s->flags |= SF_CONTENT;
                }
                real_depth--;
            }
            if (b)
                b = b->next;
        }
    }
}

/* ---- Intrinsic sizes ---- */

void l_inline_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    uint32_t base = L->nsegs;
    InlineCtx C = {L, b, 0, true};
    build_segments(&C);
    const AuStyle *rs = b->b.style;
    float indent = rs->inhr->text_indent.kind == AU_LEN_PX ? rs->inhr->text_indent.u.px : 0;
    float mn = 0, mx = 0, run = indent, line = indent, floats = 0;
    float run_trail = 0; /* trailing spaces at the end of run */
    for (uint32_t i = base; i < L->nsegs; i++) {
        LSeg *s = &L->segs[i];
        if (s->kind == IT_ABS)
            continue;
        if (s->kind == IT_FLOAT) {
            mn = l_max(mn, s->wmin);
            floats += s->w;
            continue;
        }
        run_trail = 0;
        if (s->flags & SF_BREAK_BEFORE) {
            mn = l_max(mn, run);
            run = 0;
        }
        float w = s->w;
        if (s->flags & SF_TAB) {
            LFont *f = l_font(L, s->box->style);
            w = f->m.space_advance * 8;
        }
        if (s->kind == IT_TEXT && (s->flags & SF_SPLITTABLE) && s->box->style->inhr->overflow_wrap == AU_OVERFLOW_WRAP_ANYWHERE) {
            /* Any character may start a line: the widest character. */
            LText *t = (LText *)s->box;
            LFont *f = l_font(L, s->box->style);
            float widest = 0;
            uint32_t j = s->start, e = s->end - s->trail;
            while (j < e) {
                size_t adv;
                au_utf8_decode(t->text + j, e - j, &adv);
                widest = l_max(widest, l_measure(L, f, s->box->style, t->text + j, adv));
                j += (uint32_t)adv;
            }
            mn = l_max(mn, run + widest);
            run = 0;
        } else {
            run += s->kind == IT_ATOMIC ? s->wmin : w;
        }
        line += w;
        if (s->flags & SF_FORCED) {
            mn = l_max(mn, run);
            mx = l_max(mx, line);
            run = line = 0;
            continue;
        }
        if (s->flags & SF_BREAK) {
            mn = l_max(mn, run);
            run = 0;
            if (s->flags & SF_SHY)
                mn = l_max(mn, w);
        } else {
            /* No break after the spaces (nowrap): they join the run. */
            run += s->trail_w;
            run_trail = s->trail_w;
        }
        line += s->trail_w;
    }
    mn = l_max(mn, run - run_trail);
    mx = l_max(mx, line);
    /* Trailing spaces do not count at the end of the content. */
    if (L->nsegs > base) {
        LSeg *last = &L->segs[L->nsegs - 1];
        if (!(last->flags & SF_PRESERVED))
            mx -= last->trail_w;
    }
    mx += floats;
    L->nsegs = base;
    *min_c = l_max(0, mn);
    *max_c = l_max(*min_c, mx);
}

/* ---- Line building ---- */

typedef struct FragBuf {
    LFrag *f;
    uint32_t n, cap;
} FragBuf;

static LFrag *frag_push(FragBuf *fb)
{
    if (fb->n == fb->cap) {
        fb->cap = fb->cap ? fb->cap * 2 : 64;
        fb->f = au_realloc(fb->f, fb->cap * sizeof *fb->f);
    }
    LFrag *f = &fb->f[fb->n++];
    memset(f, 0, sizeof *f);
    return f;
}

typedef struct VCtx {
    LBox *box;
    float bo;       /* baseline offset from the root baseline, down positive */
    float a, d, lh; /* ascent, descent, line-height */
    float xh, fs;
    float rdx, rdy; /* relative offset, cumulative */
    int valign;
    uint32_t frag;  /* its fragment on this line */
} VCtx;

typedef struct LineState {
    InlineCtx *C;
    LFloats *fl;
    float bfc_x, bfc_y;   /* content-box origin in BFC coordinates */
    float cl, cr;         /* content edges (BFC x) */
    float y;              /* current line top, content coordinates */
    LFlowRef *flow;
    bool resolved;
    FragBuf fb;
    FragBuf lines_buf;    /* LLine storage, reinterpreted */
    LLine *lines;
    uint32_t nlines, lines_cap;
    LBox *open[INLINE_MAX_DEPTH + 1];
    int nopen;
    uint32_t deferred[64];
    int ndeferred;
    bool first_line;
} LineState;

static float hyphen_width(AuLayout *L, const LBox *b)
{
    LFont *f = l_font(L, b->style);
    return l_measure_raw(L, f, "-", 1);
}

static void resolve_flow(LineState *S)
{
    if (!S->resolved && S->flow) {
        S->bfc_y = l_flow_resolve(S->flow);
        S->resolved = true;
    }
    S->resolved = true;
}

static void line_band(LineState *S, float h, float *l, float *r)
{
    l_floats_avail(S->fl, S->bfc_y + S->y, h, S->cl, S->cr, l, r);
}

static void place_float_seg(LineState *S, LBlock *f, float y)
{
    InlineCtx *C = S->C;
    LBlock *g = l_geom_parent(&f->b);
    /* The geometry parent of an inline float is the IFC root. */
    (void)g;
    float ox = S->bfc_x - C->root->bw[3] - C->root->p[3];
    float oy = S->bfc_y - C->root->bw[0] - C->root->p[0];
    C->L->depth++;
    l_place_float(C->L, S->fl, f, C->cw, -1, S->cl, S->cr, S->bfc_y + y, ox, oy);
    C->L->depth--;
}

static float tab_width(AuLayout *L, const LSeg *s, float x)
{
    const AuStyle *st = s->box->style;
    LFont *f = l_font(L, st);
    const AuLen *ts = &st->inhr->tab_size;
    float stop = ts->kind == AU_LEN_PX ? ts->u.px
                                       : (ts->kind == AU_LEN_NUMBER ? ts->u.num : 8) *
                                             (f->m.space_advance + st->inhr->letter_spacing + st->inhr->word_spacing);
    if (stop <= 0)
        return 0;
    float next = ceilf((x + 0.01f) / stop) * stop;
    if (next - x < f->m.space_advance * 0.5f)
        next += stop;
    return next - x;
}

static int text_align_of(const AuStyle *s, bool last)
{
    int a = s->inh->text_align;
    if (last && a == AU_TEXT_ALIGN_JUSTIFY) {
        int al = s->inhr->text_align_last;
        switch (al) {
        case AU_TEXT_ALIGN_LAST_JUSTIFY: return AU_TEXT_ALIGN_JUSTIFY;
        case AU_TEXT_ALIGN_LAST_CENTER: return AU_TEXT_ALIGN_CENTER;
        case AU_TEXT_ALIGN_LAST_RIGHT:
        case AU_TEXT_ALIGN_LAST_END: return AU_TEXT_ALIGN_RIGHT;
        default: return AU_TEXT_ALIGN_LEFT;
        }
    }
    switch (a) {
    case AU_TEXT_ALIGN_RIGHT:
    case AU_TEXT_ALIGN_END:
    case AU_TEXT_ALIGN_WEBKIT_RIGHT: return AU_TEXT_ALIGN_RIGHT;
    case AU_TEXT_ALIGN_CENTER:
    case AU_TEXT_ALIGN_WEBKIT_CENTER: return AU_TEXT_ALIGN_CENTER;
    case AU_TEXT_ALIGN_JUSTIFY: return AU_TEXT_ALIGN_JUSTIFY;
    default: return AU_TEXT_ALIGN_LEFT;
    }
}

static void vmetrics(AuLayout *L, LBox *b, VCtx *v)
{
    LFont *f = l_font(L, b->style);
    v->box = b;
    v->a = f->m.ascent;
    v->d = f->m.descent;
    v->lh = l_line_height(b->style, f);
    v->xh = f->m.x_height;
    v->fs = b->style->inh->font_size;
}

static uint32_t count_spaces(const LText *t, uint32_t start, uint32_t len)
{
    uint32_t n = 0;
    for (uint32_t i = start; i < start + len; i++)
        if (t->text[i] == ' ' || ((uint8_t)t->text[i] == 0xC2 && i + 1 < start + len && (uint8_t)t->text[i + 1] == 0xA0))
            n++;
    return n;
}

/* Builds the fragments of segments [a, e] as one line at S->y with the
 * content edges [l, r] (BFC x). Returns the line height. */
static float finish_line(LineState *S, uint32_t a, uint32_t e, float l, float r, float indent, bool last_line,
                         bool ellipsis_here)
{
    InlineCtx *C = S->C;
    AuLayout *L = C->L;
    LBlock *root = C->root;
    const AuStyle *rs = root->b.style;
    float content_x0 = root->bw[3] + root->p[3];
    float content_y0 = root->bw[0] + root->p[0];
    float x0 = l - S->cl; /* line start relative to the content left */
    uint32_t first = S->fb.n;
    float x = indent;
    /* Inline boxes open from the previous line continue. */
    int line_depth = 0;
    for (int i = 0; i < S->nopen; i++) {
        LFrag *f = frag_push(&S->fb);
        f->box = S->open[i];
        f->kind = FR_INLINE;
        f->x = x;
        f->depth = (uint16_t)i;
        line_depth = i + 1;
    }
    (void)line_depth;
    /* Trailing spaces at the end of the line do not count. */
    uint32_t last_text = UINT32_MAX;
    for (uint32_t i = e + 1; i-- > a;) {
        LSeg *s = &L->segs[i];
        if (s->kind == IT_TEXT || s->kind == IT_ATOMIC || s->kind == IT_BR) {
            last_text = i;
            break;
        }
    }
    bool line_start = true;
    for (uint32_t i = a; i <= e; i++) {
        LSeg *s = &L->segs[i];
        switch (s->kind) {
        case IT_OPEN: {
            if (S->nopen < INLINE_MAX_DEPTH)
                S->open[S->nopen++] = s->box;
            LFrag *f = frag_push(&S->fb);
            f->box = s->box;
            f->kind = FR_INLINE;
            f->flags = (s->box->flags & LF_SPLIT_HEAD) ? 0 : FRF_START;
            const AuStyleSurround *sr = s->box->style->surr;
            float ml = sr->margin[3].kind == AU_LEN_AUTO ? 0 : l_len(&sr->margin[3], C->cw);
            f->x = x + ((s->box->flags & LF_SPLIT_HEAD) ? 0 : ml);
            f->depth = (uint16_t)(S->nopen - 1);
            x += s->w;
            break;
        }
        case IT_CLOSE: {
            /* Find its open fragment on this line. */
            for (uint32_t j = S->fb.n; j-- > first;) {
                LFrag *f = &S->fb.f[j];
                if (f->kind == FR_INLINE && f->box == s->box && f->h != -1) {
                    const AuStyleSurround *sr = s->box->style->surr;
                    float mr = sr->margin[1].kind == AU_LEN_AUTO ? 0 : l_len(&sr->margin[1], C->cw);
                    float edge = (s->box->flags & LF_SPLIT_TAIL) ? 0 : s->w - mr;
                    f->w = l_max(0, x + edge - f->x);
                    if (!(s->box->flags & LF_SPLIT_TAIL))
                        f->flags |= FRF_END;
                    f->h = -1; /* closed marker */
                    break;
                }
            }
            if (S->nopen)
                S->nopen--;
            x += s->w;
            break;
        }
        case IT_TEXT: {
            LText *t = (LText *)s->box;
            uint32_t len = s->end - s->start;
            float w = s->w;
            if (s->flags & SF_TAB)
                w = tab_width(L, s, x);
            bool at_end = i == last_text;
            bool trim_trail = at_end && !(s->flags & SF_PRESERVED);
            uint32_t lead_skip = 0;
            if (line_start && s->w == 0 && s->trail && !(s->flags & SF_PRESERVED) && !preserves_spaces(s->box->style)) {
                /* Collapsible spaces at the start of the line are removed. */
                lead_skip = s->trail;
            }
            if (s->flags & SF_FORCED) {
                /* exclude the newline */
                if (len && t->text[s->start + len - 1] == '\n')
                    len--;
            }
            uint32_t vis = len - (trim_trail ? s->trail : 0);
            if (lead_skip) {
                vis = 0;
            }
            LFrag *prev = S->fb.n > first ? &S->fb.f[S->fb.n - 1] : NULL;
            if (vis || (s->flags & SF_CONTENT)) {
                if (prev && prev->kind == FR_TEXT && prev->box == s->box && prev->start + prev->len == s->start &&
                    !(prev->flags & FRF_HYPHEN)) {
                    prev->len += vis;
                    prev->w += w + (trim_trail || lead_skip ? 0 : s->trail_w);
                } else if (vis) {
                    LFrag *f = frag_push(&S->fb);
                    f->box = s->box;
                    f->kind = FR_TEXT;
                    f->x = x;
                    f->start = s->start;
                    f->len = vis;
                    f->w = w + (trim_trail ? 0 : s->trail_w);
                    f->depth = (uint16_t)S->nopen;
                }
            }
            if (i == e && (s->flags & SF_SHY)) {
                LFrag *f = S->fb.n > first ? &S->fb.f[S->fb.n - 1] : NULL;
                if (f && f->kind == FR_TEXT && f->box == s->box) {
                    f->flags |= FRF_HYPHEN;
                    f->w += hyphen_width(L, s->box);
                    x += hyphen_width(L, s->box);
                }
            }
            x += w + (trim_trail || lead_skip ? 0 : s->trail_w);
            if (vis)
                line_start = false;
            break;
        }
        case IT_ATOMIC: {
            LFrag *f = frag_push(&S->fb);
            f->box = s->box;
            f->kind = FR_ATOMIC;
            f->x = x;
            f->w = s->w;
            f->depth = (uint16_t)S->nopen;
            x += s->w;
            line_start = false;
            break;
        }
        case IT_BR: {
            LFrag *f = frag_push(&S->fb);
            f->box = s->box;
            f->kind = FR_BR;
            f->x = x;
            f->depth = (uint16_t)S->nopen;
            line_start = false;
            break;
        }
        default:
            break;
        }
    }
    /* Still-open inline boxes end at the line end without their end edge. */
    for (uint32_t j = first; j < S->fb.n; j++) {
        LFrag *f = &S->fb.f[j];
        if (f->kind == FR_INLINE) {
            if (f->h == -1)
                f->h = 0;
            else
                f->w = l_max(0, x - f->x);
        }
    }
    float line_w = x;
    uint32_t nfr = S->fb.n - first;

    /* text-overflow: ellipsis / line-clamp. */
    float avail = r - l;
    bool clip_ellipsis = rs->rare->text_overflow == AU_TEXT_OVERFLOW_ELLIPSIS &&
                         (root->b.flags & LF_CLIPS) && line_w > avail + 0.01f;
    if (clip_ellipsis || ellipsis_here) {
        LFont *rf = l_font(L, rs);
        float ew = l_measure_raw(L, rf, "\xE2\x80\xA6", 3);
        float limit = avail - ew;
        bool cut = false;
        for (uint32_t j = first; j < S->fb.n; j++) {
            LFrag *f = &S->fb.f[j];
            if (cut) {
                f->flags |= FRF_HIDDEN;
                continue;
            }
            if (f->kind == FR_INLINE || f->x + f->w <= limit + 0.01f)
                continue;
            if (f->kind == FR_TEXT) {
                LText *t = (LText *)f->box;
                LFont *ff = l_font(L, f->box->style);
                uint32_t k = 0;
                float w = 0;
                while (k < f->len) {
                    size_t adv;
                    au_utf8_decode(t->text + f->start + k, f->len - k, &adv);
                    float nw = l_measure(L, ff, f->box->style, t->text + f->start, k + (uint32_t)adv);
                    if (f->x + nw > limit + 0.01f)
                        break;
                    w = nw;
                    k += (uint32_t)adv;
                }
                f->len = k;
                f->w = w;
                f->flags &= ~FRF_HYPHEN;
                f->flags |= FRF_ELLIPSIS;
            } else {
                f->flags |= FRF_HIDDEN;
                for (uint32_t q = j; q-- > first;)
                    if (S->fb.f[q].kind == FR_TEXT && !(S->fb.f[q].flags & FRF_HIDDEN)) {
                        S->fb.f[q].flags |= FRF_ELLIPSIS;
                        break;
                    }
            }
            cut = true;
        }
        if (!cut && ellipsis_here) {
            for (uint32_t q = S->fb.n; q-- > first;)
                if (S->fb.f[q].kind == FR_TEXT) {
                    S->fb.f[q].flags |= FRF_ELLIPSIS;
                    break;
                }
        }
        if (clip_ellipsis)
            line_w = l_min(line_w, avail);
    }

    /* Vertical alignment. */
    VCtx stack[INLINE_MAX_DEPTH + 2];
    int sp = 0;
    vmetrics(L, &root->b, &stack[0]);
    stack[0].bo = 0;
    stack[0].rdx = stack[0].rdy = 0;
    stack[0].valign = AU_VALIGN_BASELINE;
    float half = (stack[0].lh - (stack[0].a + stack[0].d)) / 2;
    float top = -(stack[0].a + half), bottom = stack[0].d + half;
    bool any_top_bottom = false;
    float tb_max = 0;
    /* per-fragment temporary: baseline offset (from root baseline) and top/bottom flags */
    float *bo = au_alloc((nfr + 1) * sizeof(float) * 2);
    float *ext = bo + nfr + 1;
    for (uint32_t j = 0; j < nfr; j++) {
        LFrag *f = &S->fb.f[first + j];
        if (sp > f->depth)
            sp = f->depth;
        VCtx *P = &stack[sp];
        if (f->kind == FR_INLINE) {
            if (sp + 1 > INLINE_MAX_DEPTH)
                continue;
            VCtx *v = &stack[sp + 1];
            vmetrics(L, f->box, v);
            const AuStyleBox *bx = f->box->style->box;
            float hl = (v->lh - (v->a + v->d)) / 2;
            v->valign = bx->vertical_align;
            v->rdx = P->rdx;
            v->rdy = P->rdy;
            if (f->box->flags & LF_RELPOS) {
                LBlock tmp;
                memset(&tmp, 0, sizeof tmp);
                tmp.b = *f->box;
                l_apply_relative(&tmp, C->cw, -1);
                v->rdx += tmp.x;
                v->rdy += tmp.y;
            }
            switch (bx->vertical_align) {
            case AU_VALIGN_SUB: v->bo = P->bo + P->fs / 5 + 1; break;
            case AU_VALIGN_SUPER: v->bo = P->bo - P->fs / 3 - 1; break;
            case AU_VALIGN_TEXT_TOP: v->bo = P->bo - P->a + v->a + hl; break;
            case AU_VALIGN_TEXT_BOTTOM: v->bo = P->bo + P->d - v->d - hl; break;
            case AU_VALIGN_MIDDLE: v->bo = P->bo - P->xh / 2 - (v->d - v->a) / 2; break;
            case AU_VALIGN_LENGTH: v->bo = P->bo - l_len(&bx->vertical_align_len, v->lh); break;
            case AU_VALIGN_TOP:
            case AU_VALIGN_BOTTOM:
                v->bo = 0;
                any_top_bottom = true;
                tb_max = l_max(tb_max, v->lh);
                break;
            default: v->bo = P->bo; break;
            }
            if (P->valign == AU_VALIGN_TOP || P->valign == AU_VALIGN_BOTTOM) {
                /* nested in a top/bottom aligned box: aligned with it */
                v->valign = P->valign;
            }
            v->frag = first + j;
            sp++;
            bo[j] = v->bo;
            ext[j] = v->rdy;
            f->x += v->rdx;
            if (v->valign != AU_VALIGN_TOP && v->valign != AU_VALIGN_BOTTOM) {
                top = l_min(top, v->bo - v->a - hl);
                bottom = l_max(bottom, v->bo + v->d + hl);
            }
            continue;
        }
        /* text, atomic, br: belong to P */
        bo[j] = P->bo;
        f->x += P->rdx;
        if (f->kind == FR_ATOMIC) {
            LBlock *k = (LBlock *)f->box;
            float mh = k->h + k->m[0] + k->m[2];
            bool use_bottom = isnan(k->last_baseline) || (k->b.flags & LF_CLIPS) || k->b.inner == LI_REPLACED;
            float bl = use_bottom ? mh : k->m[0] + k->last_baseline;
            if (k->b.inner == LI_FLEX && !isnan(k->baseline))
                bl = k->m[0] + k->baseline;
            int va = k->b.style->box->vertical_align;
            if (P->valign == AU_VALIGN_TOP || P->valign == AU_VALIGN_BOTTOM)
                va = AU_VALIGN_BASELINE;
            float tp;
            switch (va) {
            case AU_VALIGN_SUB: tp = P->bo + P->fs / 5 + 1 - bl; break;
            case AU_VALIGN_SUPER: tp = P->bo - P->fs / 3 - 1 - bl; break;
            case AU_VALIGN_TEXT_TOP: tp = P->bo - P->a; break;
            case AU_VALIGN_TEXT_BOTTOM: tp = P->bo + P->d - mh; break;
            case AU_VALIGN_MIDDLE: tp = P->bo - P->xh / 2 - mh / 2; break;
            case AU_VALIGN_LENGTH:
                tp = P->bo - l_len(&k->b.style->box->vertical_align_len, l_line_height(k->b.style, l_font(L, k->b.style))) -
                     bl;
                break;
            case AU_VALIGN_TOP:
            case AU_VALIGN_BOTTOM:
                any_top_bottom = true;
                tb_max = l_max(tb_max, mh);
                tp = 0;
                break;
            default: tp = P->bo - bl; break;
            }
            bo[j] = tp; /* for atomics: margin-box top relative to the root baseline */
            ext[j] = (float)va;
            if (va != AU_VALIGN_TOP && va != AU_VALIGN_BOTTOM) {
                top = l_min(top, tp);
                bottom = l_max(bottom, tp + mh);
            }
        }
    }
    float lh = bottom - top;
    if (any_top_bottom && tb_max > lh) {
        bottom += tb_max - lh;
        lh = tb_max;
    }
    float baseline = -top;
    /* Positions. */
    sp = 0;
    for (uint32_t j = 0; j < nfr; j++) {
        LFrag *f = &S->fb.f[first + j];
        while (sp > f->depth)
            sp--;
        if (f->kind == FR_INLINE) {
            if (sp + 1 > INLINE_MAX_DEPTH)
                continue;
            VCtx *v = &stack[sp + 1];
            vmetrics(L, f->box, v);
            v->bo = bo[j];
            v->rdy = ext[j];
            int va = f->box->style->box->vertical_align;
            float hl = (v->lh - (v->a + v->d)) / 2;
            if (va == AU_VALIGN_TOP)
                v->bo = top + v->a + hl;
            else if (va == AU_VALIGN_BOTTOM)
                v->bo = bottom - v->d - hl;
            const AuStyleSurround *sr = f->box->style->surr;
            float bt = sr->border_width[0] + l_max(0, l_len(&sr->padding[0], C->cw));
            float bb = sr->border_width[2] + l_max(0, l_len(&sr->padding[2], C->cw));
            f->y = S->y + baseline + v->bo - v->a - bt + v->rdy;
            f->h = v->a + v->d + bt + bb;
            f->baseline = v->a + bt;
            sp++;
            continue;
        }
        VCtx *P = &stack[sp];
        if (f->kind == FR_ATOMIC) {
            LBlock *k = (LBlock *)f->box;
            float mh = k->h + k->m[0] + k->m[2];
            float tp = bo[j];
            int va = (int)ext[j];
            if (va == AU_VALIGN_TOP)
                tp = top;
            else if (va == AU_VALIGN_BOTTOM)
                tp = bottom - mh;
            f->y = S->y + baseline + tp + P->rdy;
            f->h = mh;
            f->baseline = baseline + P->bo - tp;
            k->x = content_x0 + x0 + f->x + k->m[3];
            k->y = content_y0 + f->y + k->m[0];
            continue;
        }
        float pbo = P->bo;
        f->y = S->y + baseline + pbo - P->a;
        f->h = P->a + P->d;
        f->baseline = P->a;
        if (f->kind == FR_TEXT || f->kind == FR_BR)
            f->y += P->rdy;
    }
    au_free(bo);

    /* Horizontal alignment. */
    int ta = text_align_of(rs, last_line);
    float slack = avail - line_w;
    float shift = 0;
    if (ta == AU_TEXT_ALIGN_RIGHT)
        shift = slack;
    else if (ta == AU_TEXT_ALIGN_CENTER)
        shift = slack / 2;
    if (shift < 0)
        shift = 0;
    float extra = 0;
    if (ta == AU_TEXT_ALIGN_JUSTIFY && slack > 0) {
        uint32_t spaces = 0;
        for (uint32_t j = first; j < S->fb.n; j++)
            if (S->fb.f[j].kind == FR_TEXT)
                spaces += count_spaces((LText *)S->fb.f[j].box, S->fb.f[j].start, S->fb.f[j].len);
        if (spaces)
            extra = slack / spaces;
    }
    float cum = 0;
    for (uint32_t j = first; j < S->fb.n; j++) {
        LFrag *f = &S->fb.f[j];
        f->x += x0 + shift + cum;
        if (f->kind == FR_TEXT && extra > 0) {
            uint32_t sp2 = count_spaces((LText *)f->box, f->start, f->len);
            f->spacing = extra;
            f->w += sp2 * extra;
            cum += sp2 * extra;
        }
        if (f->kind == FR_ATOMIC) {
            LBlock *k = (LBlock *)f->box;
            k->x += shift + cum;
        }
    }
    if (extra > 0) {
        /* Inline boxes grow by the space added inside them. */
        for (uint32_t j = first; j < S->fb.n; j++) {
            LFrag *f = &S->fb.f[j];
            if (f->kind != FR_INLINE)
                continue;
            float end = f->x;
            for (uint32_t q = j + 1; q < S->fb.n && S->fb.f[q].depth > f->depth; q++)
                end = l_max(end, S->fb.f[q].x + S->fb.f[q].w);
            if (end > f->x + f->w)
                f->w = end - f->x;
        }
    }
    /* Fragments are stored relative to the IFC root's border box. */
    for (uint32_t j = first; j < S->fb.n; j++) {
        LFrag *f = &S->fb.f[j];
        f->x += content_x0;
        f->y += content_y0;
    }
    if (S->nlines == S->lines_cap) {
        S->lines_cap = S->lines_cap ? S->lines_cap * 2 : 16;
        S->lines = au_realloc(S->lines, S->lines_cap * sizeof *S->lines);
    }
    LLine *ln = &S->lines[S->nlines++];
    ln->y = S->y + content_y0;
    ln->h = lh;
    ln->baseline = baseline;
    ln->x = content_x0 + x0 + shift;
    ln->w = line_w + cum;
    ln->first = first;
    ln->n = S->fb.n - first;
    return lh;
}

/* ---- Line breaking ---- */

static void split_seg(AuLayout *L, uint32_t i, uint32_t at_byte, float w_head)
{
    seg_push(L);
    memmove(&L->segs[i + 2], &L->segs[i + 1], (L->nsegs - i - 2) * sizeof *L->segs);
    LSeg *h = &L->segs[i], *t = &L->segs[i + 1];
    *t = *h;
    LText *tx = (LText *)h->box;
    LFont *f = l_font(L, h->box->style);
    h->end = at_byte;
    h->trail = 0;
    h->trail_w = 0;
    h->w = w_head;
    h->flags = (h->flags & (SF_CONTENT | SF_SPLITTABLE)) | SF_BREAK;
    t->start = at_byte;
    uint32_t wl = t->end - t->trail - t->start;
    t->w = l_measure(L, f, t->box->style, tx->text + t->start, wl);
    t->wmin = t->w;
}

/* The longest prefix of text segment i that fits in room (at least one
 * character). Returns the byte offset and its width. */
static uint32_t fit_prefix(AuLayout *L, LSeg *s, float room, float *w_out)
{
    LText *t = (LText *)s->box;
    LFont *f = l_font(L, s->box->style);
    uint32_t e = s->end - s->trail;
    uint32_t k = s->start;
    float w = 0;
    bool first = true;
    while (k < e) {
        size_t adv;
        au_utf8_decode(t->text + k, e - k, &adv);
        float nw = l_measure(L, f, s->box->style, t->text + s->start, k + adv - s->start);
        if (!first && nw > room)
            break;
        w = nw;
        k += (uint32_t)adv;
        first = false;
    }
    *w_out = w;
    return k;
}

float l_inline_layout(AuLayout *L, LBlock *b, float content_w, LFloats *fl, float bfc_x, float bfc_y, float cb_h,
                      LFlowRef *flow)
{
    (void)cb_h;
    l_box_free_layout(&b->b);
    uint32_t base = L->nsegs;
    InlineCtx C = {L, b, content_w, false};
    LFloats own;
    memset(&own, 0, sizeof own);
    if (!fl)
        fl = &own;
    build_segments(&C);
    LineState S;
    memset(&S, 0, sizeof S);
    S.C = &C;
    S.fl = fl;
    S.bfc_x = bfc_x;
    S.bfc_y = bfc_y;
    S.cl = bfc_x;
    S.cr = bfc_x + content_w;
    S.flow = flow;
    S.y = 0;
    S.first_line = true;
    const AuStyle *rs = b->b.style;
    LFont *rf = l_font(L, rs);
    float strut = l_line_height(rs, rf);
    if (strut <= 0)
        strut = rf->m.ascent + rf->m.descent;
    float indent = l_len(&rs->inhr->text_indent, content_w);
    int clamp = (b->b.flags & LF_LINE_CLAMP) ? rs->rare->line_clamp : 0;
    uint32_t i = base;
    int pending_clear = AU_CLEAR_NONE;
    uint32_t retries = 0; /* lines moved below floats: bounded */
    while (i < L->nsegs) {
        if (clamp && (int)S.nlines >= clamp)
            break;
        if (pending_clear != AU_CLEAR_NONE) {
            float cy = l_floats_clear_y(fl, pending_clear) - S.bfc_y;
            if (cy > S.y)
                S.y = cy;
            pending_clear = AU_CLEAR_NONE;
        }
        float l, r;
        line_band(&S, strut, &l, &r);
        float avail = r - l;
        float x = S.first_line ? indent : 0;
        uint32_t j = i;
        int32_t opp = -1;     /* last segment after which we may break */
        bool has_content = false;
        bool forced = false;
        bool at_start = true; /* no visible content yet */
        uint32_t end = i;
        bool retry = false;
        S.ndeferred = 0;
        while (j < L->nsegs) {
            LSeg *s = &L->segs[j];
            if (s->kind == IT_FLOAT && (s->flags & SF_PLACED)) {
                end = j;
                j++;
                continue;
            }
            if (s->kind == IT_FLOAT) {
                LBlock *f = (LBlock *)s->box;
                /* Lay it out to know its width; place now if it fits. */
                l_resolve_box_edges(f, content_w);
                float fw;
                {
                    float mn, mx;
                    l_outer_intrinsic(L, f, content_w, &mn, &mx);
                    float sw = l_spec_width(f, content_w);
                    fw = sw >= 0 ? sw + l_edges_h(f) : l_min(l_max(mn, avail), mx);
                }
                if (x + fw <= avail + 0.01f || !has_content) {
                    /* Laying the float out can grow L->segs: s is stale
                     * after this call. */
                    place_float_seg(&S, f, S.y);
                    L->segs[j].flags |= SF_PLACED;
                    line_band(&S, strut, &l, &r);
                    avail = r - l;
                } else if (S.ndeferred < 64) {
                    S.deferred[S.ndeferred++] = j;
                } else {
                    place_float_seg(&S, f, S.y);
                    L->segs[j].flags |= SF_PLACED;
                }
                end = j;
                j++;
                continue;
            }
            if (s->kind == IT_ABS) {
                /* Queued once; a later scan of the same segment (the line
                 * broke before it, or is retried lower) only moves its
                 * static position. */
                if (s->box && (s->flags & SF_PLACED)) {
                    LBlock *k = (LBlock *)s->box;
                    k->sx = b->bw[3] + b->p[3] + (l - S.cl) + x;
                    k->sy = b->bw[0] + b->p[0] + S.y;
                } else if (s->box) {
                    s->flags |= SF_PLACED;
                    LBlock *k = (LBlock *)s->box;
                    float sx = b->bw[3] + b->p[3] + (l - S.cl) + x;
                    float sy = b->bw[0] + b->p[0] + S.y;
                    l_queue_abspos(L, k, sx, sy);
                }
                end = j;
                j++;
                continue;
            }
            float w = s->w;
            if (s->flags & SF_TAB)
                w = tab_width(L, s, x);
            if ((s->flags & SF_BREAK_BEFORE) && j > i && has_content)
                opp = (int32_t)j - 1;
            bool leading_space = at_start && s->kind == IT_TEXT && s->w == 0 && !(s->flags & SF_PRESERVED) &&
                                 !preserves_spaces(s->box->style);
            if (!leading_space && x + w > avail + 0.01f && has_content && opp >= (int32_t)i) {
                end = (uint32_t)opp;
                break;
            }
            if (!leading_space && x + w > avail + 0.01f && (s->flags & SF_SPLITTABLE) && s->kind == IT_TEXT &&
                (s->end - s->trail) > s->start) {
                float room = avail - x;
                float hw;
                uint32_t at = fit_prefix(L, s, room, &hw);
                if (at < s->end - s->trail && (hw <= room || !has_content)) {
                    split_seg(L, j, at, hw);
                    end = j;
                    has_content = true;
                    break;
                }
            }
            if (!leading_space && x + w > avail + 0.01f && !has_content && (l > S.cl + 0.01f || r < S.cr - 0.01f)) {
                /* Nothing fits beside the floats: move below the next one. */
                float nb = l_floats_next_bottom(fl, S.bfc_y + S.y);
                if (isfinite(nb) && retries < 10000) {
                    /* Always move down: with large coordinates or very
                     * thin floats, nb - bfc_y can round back to S.y. */
                    float ny = nb - S.bfc_y;
                    S.y = ny > S.y + 0.001f ? ny : S.y + 0.01f;
                    retries++;
                    retry = true;
                    break;
                }
            }
            if (s->flags & SF_CONTENT)
                has_content = true;
            if (!leading_space) {
                x += w;
                if ((s->kind == IT_TEXT && s->w > 0) || s->kind == IT_ATOMIC || s->kind == IT_BR ||
                    (s->kind == IT_TEXT && (s->flags & SF_PRESERVED)))
                    at_start = false;
            }
            end = j;
            if (s->flags & SF_FORCED) {
                forced = true;
                if (s->kind == IT_BR) {
                    int cl = s->box->style->box->clear;
                    if (cl != AU_CLEAR_NONE)
                        pending_clear = cl;
                }
                break;
            }
            if (s->flags & SF_BREAK)
                opp = (int32_t)j;
            if (!leading_space)
                x += s->trail_w;
            j++;
        }
        if (retry)
            continue;
        if (j >= L->nsegs)
            end = L->nsegs - 1;
        /* Close edges right after the break stay on this line. */
        while (end + 1 < L->nsegs && L->segs[end + 1].kind == IT_CLOSE)
            end++;
        /* Was there content? */
        bool content = false;
        for (uint32_t q = i; q <= end && !content; q++)
            if (L->segs[q].flags & SF_CONTENT)
                content = true;
        bool last_line = end + 1 >= L->nsegs || forced;
        if (content) {
            resolve_flow(&S);
            line_band(&S, strut, &l, &r);
            bool clamp_here = clamp && (int)S.nlines + 1 == clamp && end + 1 < L->nsegs;
            float h = finish_line(&S, i, end, l, r, S.first_line ? indent : 0, last_line, clamp_here);
            S.y += h;
            S.first_line = false;
        } else {
            /* Boxes opened or closed on an empty line still nest. */
            for (uint32_t q = i; q <= end; q++) {
                if (L->segs[q].kind == IT_OPEN && S.nopen < INLINE_MAX_DEPTH)
                    S.open[S.nopen++] = L->segs[q].box;
                else if (L->segs[q].kind == IT_CLOSE && S.nopen)
                    S.nopen--;
            }
        }
        for (int q = 0; q < S.ndeferred; q++) {
            place_float_seg(&S, (LBlock *)L->segs[S.deferred[q]].box, S.y);
            L->segs[S.deferred[q]].flags |= SF_PLACED;
        }
        i = end + 1;
    }
    /* Floats and abspos after the clamp point are still placed. */
    for (; i < L->nsegs; i++) {
        LSeg *s = &L->segs[i];
        if (s->kind == IT_FLOAT) {
            if (!(s->flags & SF_PLACED))
                place_float_seg(&S, (LBlock *)s->box, S.y);
        } else if (s->kind == IT_ABS && s->box && !(s->flags & SF_PLACED))
            l_queue_abspos(L, (LBlock *)s->box, b->bw[3] + b->p[3], b->bw[0] + b->p[0] + S.y);
        else if (s->kind == IT_ATOMIC && clamp) {
            /* hidden by the clamp: give it a position anyway */
            LBlock *k = (LBlock *)s->box;
            k->x = b->bw[3] + b->p[3];
            k->y = b->bw[0] + b->p[0] + S.y;
            k->b.flags |= LF_HIDDEN;
        }
    }
    L->nsegs = base;
    b->frags = S.fb.f;
    b->nfrags = S.fb.n;
    b->lines = S.lines;
    b->nlines = S.nlines;
    l_floats_free(&own);
    return S.y;
}
