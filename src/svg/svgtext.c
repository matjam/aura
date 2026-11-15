/* SVG text: text and tspan on one line each chunk.
 *
 * The characters of a text element (its text nodes and those of its
 * tspan and a descendants) are collected with whitespace collapsed
 * (newlines and tabs become spaces, runs of spaces become one, leading
 * and trailing spaces go). The x, y, dx and dy lists of the text element
 * and each tspan then position characters from the element's first
 * character on, inner elements overriding outer ones. A character with
 * an absolute x or y starts a new chunk; text-anchor shifts each chunk
 * by its advance. Runs of characters that share an element and have no
 * positions of their own are shaped together with per-character font
 * fallback. Glyph outlines are filled and stroked like any shape, so
 * text takes gradients, strokes and transforms. */
#include "svg_int.h"

#define MAX_SPANS 128
#define MAX_RUN_BYTES (SVG_MAX_TEXT_CHARS * 4)

typedef struct TSpan {
    AuElement *el;
    SvgFrame f;
    uint32_t start, end; /* characters [start, end) */
    uint8_t baseline;    /* AU_DOMINANT_BASELINE_* (inherited from the text) */
} TSpan;

typedef struct TChar {
    uint32_t off;  /* byte offset in the text buffer */
    uint16_t span; /* innermost span */
    uint8_t len;
    float x, y, dx, dy; /* NAN: not given */
} TChar;

typedef struct TLayout {
    TSpan spans[MAX_SPANS];
    uint32_t nspans;
    TChar *chars;
    uint32_t nchars;
    char *text;
    uint32_t nbytes;
} TLayout;

static void tl_free(TLayout *t)
{
    au_free(t->chars);
    au_free(t->text);
}

static void add_text(TLayout *t, const char *s, size_t n, uint16_t span, bool *last_space)
{
    size_t i = 0;
    while (i < n && t->nchars < SVG_MAX_TEXT_CHARS) {
        size_t adv;
        uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
        if (!adv)
            adv = 1;
        bool sp = cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r';
        if (sp) {
            if (*last_space) {
                i += adv;
                continue;
            }
            *last_space = true;
        } else {
            *last_space = false;
        }
        TChar *c = &t->chars[t->nchars++];
        c->off = t->nbytes;
        c->span = span;
        c->x = c->y = c->dx = c->dy = NAN;
        if (sp) {
            t->text[t->nbytes++] = ' ';
            c->len = 1;
        } else {
            memcpy(t->text + t->nbytes, s + i, adv);
            t->nbytes += (uint32_t)adv;
            c->len = (uint8_t)adv;
        }
        i += adv;
    }
}

static void apply_list(TLayout *t, const AuElement *el, AuAtom a, const TSpan *sp, int axis, size_t field)
{
    const char *s = svg_attr(el, a);
    if (!s)
        return;
    SvgLen ls[256];
    size_t n = svg_parse_lengths(s, ls, 256);
    for (size_t i = 0; i < n && sp->start + i < sp->end; i++) {
        float ref = axis == 0 ? sp->f.vw : sp->f.vh;
        float v = svg_len_px(ls[i], sp->f.st.font_size > 0 ? sp->f.st.font_size : 16, ref);
        *(float *)((char *)&t->chars[sp->start + i] + field) = v;
    }
}

/* Collects the characters and spans of text element el. */
static bool collect(SvgCtx *x, AuElement *el, const SvgFrame *f, TLayout *t)
{
    memset(t, 0, sizeof *t);
    t->chars = au_alloc(SVG_MAX_TEXT_CHARS * sizeof *t->chars);
    t->text = au_alloc(MAX_RUN_BYTES + 1);
    /* An explicit walk: a stack of open spans. */
    uint16_t stack[MAX_SPANS];
    int sp = 0;
    bool last_space = true;
    const AuStyle *ts = au_element_style(el);
    TSpan *root = &t->spans[t->nspans++];
    root->el = el;
    root->f = *f;
    root->start = 0;
    root->baseline = ts ? ts->rare->dominant_baseline : 0;
    stack[sp++] = 0;
    AuNode *top = &el->node;
    AuNode *n = top->first_child;
    while (n && sp > 0) {
        bool descend = false;
        if (n->type == AU_NODE_TEXT) {
            const AuText *tx = (const AuText *)n;
            add_text(t, tx->data, tx->len, stack[sp - 1], &last_space);
        } else if (n->type == AU_NODE_ELEMENT) {
            AuElement *ce = au_as_element(n);
            const AuStyle *cs = au_element_style(ce);
            if (cs && cs->box->display != AU_DISPLAY_NONE && sp < MAX_SPANS && t->nspans < MAX_SPANS &&
                (svg_is_tag(ce, x->n->tspan) || svg_is_tag(ce, x->n->a))) {
                TSpan *ps = &t->spans[stack[sp - 1]];
                TSpan *s = &t->spans[t->nspans];
                s->el = ce;
                s->f = ps->f;
                svg_state(ce, &ps->f, ps->f.inst, &s->f.st);
                s->start = t->nchars;
                s->baseline = cs->rare->dominant_baseline ? cs->rare->dominant_baseline : ps->baseline;
                stack[sp++] = (uint16_t)t->nspans++;
                descend = n->first_child != NULL;
                if (!descend) {
                    s->end = t->nchars;
                    sp--;
                }
            }
        }
        if (descend) {
            n = n->first_child;
            continue;
        }
        /* Next node, closing spans as the walk leaves them. */
        while (n && n != top && !n->next) {
            n = n->parent;
            if (n && n != top && sp > 1) {
                t->spans[stack[sp - 1]].end = t->nchars;
                sp--;
            }
        }
        if (!n || n == top)
            break;
        n = n->next;
    }
    while (sp > 0)
        t->spans[stack[--sp]].end = t->nchars;
    /* Trailing space goes. */
    if (t->nchars && t->text[t->chars[t->nchars - 1].off] == ' ' && t->chars[t->nchars - 1].len == 1)
        t->nchars--;
    for (uint32_t i = 0; i < t->nspans; i++)
        if (t->spans[i].end > t->nchars)
            t->spans[i].end = t->nchars;
    if (!t->nchars)
        return false;
    for (uint32_t i = 0; i < t->nspans; i++) {
        TSpan *s = &t->spans[i];
        apply_list(t, s->el, x->n->x, s, 0, offsetof(TChar, x));
        apply_list(t, s->el, x->n->y, s, 1, offsetof(TChar, y));
        apply_list(t, s->el, x->n->dx, s, 0, offsetof(TChar, dx));
        apply_list(t, s->el, x->n->dy, s, 1, offsetof(TChar, dy));
    }
    return true;
}

static void font_style(const SvgState *st, AuFontStyle *fs, AuFontVariation *v, size_t *nv)
{
    memset(fs, 0, sizeof *fs);
    fs->weight = st->weight ? st->weight : 400;
    fs->italic = st->font_style != AU_FONT_STYLE_NORMAL;
    fs->stretch = st->stretch == 100 ? 0 : st->stretch;
    fs->size = st->opsz == AU_FONT_OPTICAL_SIZING_NONE ? 0 : st->font_size;
    *nv = 0;
    if (st->fvs && st->fvs->value.type == AU_CSSV_COMMA_LIST) {
        for (uint32_t i = 0; i < st->fvs->value.n && *nv < 16; i++) {
            const AuCssValue *it = &st->fvs->value.u.items[i];
            if (it->type != AU_CSSV_LIST || it->n != 2 || it->u.items[0].n != 4)
                continue;
            const char *tg = it->u.items[0].u.str;
            v[*nv].tag = (uint32_t)(uint8_t)tg[0] << 24 | (uint32_t)(uint8_t)tg[1] << 16 |
                         (uint32_t)(uint8_t)tg[2] << 8 | (uint8_t)tg[3];
            v[*nv].value = it->u.items[1].u.num;
            (*nv)++;
        }
    }
    fs->settings = v;
    fs->nsettings = *nv;
}

static AuFontList *span_fonts(const SvgCtx *x, const SvgState *st)
{
    if (!x->env || !x->env->fonts)
        return NULL;
    const char *names[16];
    size_t n = 0;
    if (st->family) {
        const AuCssValue *v = &st->family->value;
        uint32_t cnt = 1;
        const AuCssValue *it = v;
        if (v->type == AU_CSSV_COMMA_LIST || v->type == AU_CSSV_LIST)
            cnt = v->n, it = v->u.items;
        for (uint32_t i = 0; i < cnt && n < 16; i++) {
            if (it[i].type == AU_CSSV_IDENT || it[i].type == AU_CSSV_STRING)
                names[n++] = it[i].u.str;
            else if (it[i].type == AU_CSSV_KEYWORD)
                names[n++] = au_css_keyword_name((AuCssKeyword)it[i].kw);
        }
    }
    AuFontStyle fs;
    AuFontVariation v[16];
    size_t nv;
    font_style(st, &fs, v, &nv);
    return au_fontlist_new_style(x->env->fonts, names, n, &fs);
}

/* The baseline shift for a dominant-baseline value, from the alphabetic
 * baseline (down positive). */
static float baseline_shift(uint8_t b, const AuFontMetrics *m)
{
    switch (b) {
    case AU_DOMINANT_BASELINE_MIDDLE: return m->x_height / 2;
    case AU_DOMINANT_BASELINE_CENTRAL: return (m->ascent - m->descent) / 2;
    case AU_DOMINANT_BASELINE_HANGING: return m->ascent * 0.8f;
    case AU_DOMINANT_BASELINE_MATHEMATICAL: return m->ascent * 0.5f;
    case AU_DOMINANT_BASELINE_TEXT_TOP:
    case AU_DOMINANT_BASELINE_TEXT_BEFORE_EDGE: return m->ascent;
    case AU_DOMINANT_BASELINE_IDEOGRAPHIC:
    case AU_DOMINANT_BASELINE_TEXT_BOTTOM:
    case AU_DOMINANT_BASELINE_TEXT_AFTER_EDGE: return -m->descent;
    default: return 0;
    }
}

/* One shaped run: its glyph outlines in user space go into the span's
 * path. */
typedef struct TRun {
    uint16_t span;
    AuPath path;
} TRun;

#define MAX_RUNS 512

/* Lays out the text: runs[] get outlines (user space), *nruns, and the
 * union bbox. */
static bool layout_text(SvgCtx *x, AuElement *el, const SvgFrame *f, TRun *runs, uint32_t *nruns, float bbox[4])
{
    *nruns = 0;
    x->text_spans = NULL;
    if (!x->env || !x->env->fonts)
        return false;
    /* On the heap: the spans hold frames, too large for a small stack. */
    TLayout *t = au_alloc(sizeof *t);
    if (!collect(x, el, f, t)) {
        tl_free(t);
        au_free(t);
        return false;
    }
    AuFontList **lists = au_calloc(MAX_SPANS * sizeof *lists);
    float pen_x = 0, pen_y = 0;
    uint32_t chunk_run = 0; /* first run of the current chunk */
    float chunk_x0 = 0;
    uint8_t chunk_anchor = AU_TEXT_ANCHOR_START;
    bool any = false;
    AuGlyphRun gr;
    au_glyph_run_init(&gr);
    uint32_t i = 0;
    while (i < t->nchars && *nruns < MAX_RUNS) {
        TChar *c0 = &t->chars[i];
        bool new_chunk = i == 0 || !isnan(c0->x) || !isnan(c0->y);
        if (new_chunk) {
            /* Close the previous chunk: shift it by its anchor. */
            if (i > 0 && chunk_anchor != AU_TEXT_ANCHOR_START) {
                float adv = pen_x - chunk_x0;
                float sh = chunk_anchor == AU_TEXT_ANCHOR_MIDDLE ? -adv / 2 : -adv;
                AuMatrix m = {1, 0, 0, 1, sh, 0};
                for (uint32_t r = chunk_run; r < *nruns; r++)
                    au_path_transform(&runs[r].path, &m);
            }
            if (!isnan(c0->x))
                pen_x = c0->x;
            if (!isnan(c0->y))
                pen_y = c0->y;
            chunk_run = *nruns;
            chunk_x0 = pen_x;
            chunk_anchor = t->spans[c0->span].f.st.anchor;
        }
        if (!isnan(c0->dx))
            pen_x += c0->dx;
        if (!isnan(c0->dy))
            pen_y += c0->dy;
        /* The run: following characters of the same span with no
         * positions. */
        uint32_t j = i + 1;
        while (j < t->nchars && t->chars[j].span == c0->span && isnan(t->chars[j].x) && isnan(t->chars[j].y) &&
               isnan(t->chars[j].dx) && isnan(t->chars[j].dy))
            j++;
        TSpan *sp = &t->spans[c0->span];
        if (!lists[c0->span])
            lists[c0->span] = span_fonts(x, &sp->f.st);
        AuFontList *fl = lists[c0->span];
        float size = sp->f.st.font_size;
        AuFontFace *pf = fl ? au_fontlist_primary(fl) : NULL;
        if (!fl || !pf || !(size > 0) || size > 10000) {
            i = j;
            continue;
        }
        AuFontMetrics m;
        au_font_metrics(pf, size, &m);
        float by = pen_y + baseline_shift(sp->baseline, &m);
        uint32_t off = c0->off, end = t->chars[j - 1].off + t->chars[j - 1].len;
        au_shape_text_list(fl, size, t->text + off, end - off, &gr);
        TRun *run = &runs[(*nruns)++];
        run->span = c0->span;
        au_path_init(&run->path);
        float ls = sp->f.st.letter_spacing;
        for (size_t g = 0; g < gr.n; g++) {
            const AuFontFace *face = gr.glyphs[g].face ? gr.glyphs[g].face : pf;
            float s = size / (face->units_per_em ? face->units_per_em : 1000);
            au_font_glyph_path(face, gr.glyphs[g].id, s, s, pen_x + gr.glyphs[g].x + ls * (float)g,
                               by + gr.glyphs[g].y, &run->path);
        }
        pen_x += gr.width + ls * (float)gr.n;
        any = true;
        i = j;
    }
    if (any && chunk_anchor != AU_TEXT_ANCHOR_START) {
        float adv = pen_x - chunk_x0;
        float sh = chunk_anchor == AU_TEXT_ANCHOR_MIDDLE ? -adv / 2 : -adv;
        AuMatrix m = {1, 0, 0, 1, sh, 0};
        for (uint32_t r = chunk_run; r < *nruns; r++)
            au_path_transform(&runs[r].path, &m);
    }
    au_glyph_run_free(&gr);
    for (uint32_t k = 0; k < t->nspans; k++)
        if (lists[k])
            au_fontlist_free(lists[k]);
    au_free(lists);
    /* The bbox: the union of outline bounds. */
    bool hb = false;
    for (uint32_t r = 0; r < *nruns; r++) {
        float rb[4];
        if (runs[r].path.nops && au_path_bounds(&runs[r].path, rb)) {
            if (!hb) {
                memcpy(bbox, rb, sizeof rb);
                hb = true;
            } else {
                bbox[0] = fminf(bbox[0], rb[0]);
                bbox[1] = fminf(bbox[1], rb[1]);
                bbox[2] = fmaxf(bbox[2], rb[2]);
                bbox[3] = fmaxf(bbox[3], rb[3]);
            }
        }
    }
    /* Spans' frames are needed by the caller: keep the states by
     * copying them into the runs' order (the caller re-derives frames). */
    x->text_spans = au_alloc(t->nspans * sizeof(SvgFrame));
    for (uint32_t k = 0; k < t->nspans; k++)
        x->text_spans[k] = t->spans[k].f;
    tl_free(t);
    au_free(t);
    return hb;
}

static void free_runs(SvgCtx *x, TRun *runs, uint32_t n)
{
    for (uint32_t r = 0; r < n; r++)
        au_path_free(&runs[r].path);
    au_free(x->text_spans);
    x->text_spans = NULL;
}

void svg_text_paint(SvgCtx *x, AuElement *el, const SvgFrame *f, float opacity)
{
    TRun *runs = au_alloc(MAX_RUNS * sizeof *runs);
    uint32_t n;
    float b[4];
    if (layout_text(x, el, f, runs, &n, b)) {
        for (uint32_t r = 0; r < n; r++) {
            if (!runs[r].path.nops)
                continue;
            SvgFrame sf = x->text_spans[runs[r].span];
            sf.ctm = f->ctm;
            svg_paint_geometry(x, el, &sf, &runs[r].path, b, opacity, true);
        }
    }
    free_runs(x, runs, n);
    au_free(runs);
}

bool svg_text_bbox(SvgCtx *x, AuElement *el, const SvgFrame *f, float bbox[4])
{
    TRun *runs = au_alloc(MAX_RUNS * sizeof *runs);
    uint32_t n;
    bool ok = layout_text(x, el, f, runs, &n, bbox);
    free_runs(x, runs, n);
    au_free(runs);
    return ok;
}

void svg_text_outline(SvgCtx *x, AuElement *el, const SvgFrame *f, AuPath *out)
{
    TRun *runs = au_alloc(MAX_RUNS * sizeof *runs);
    uint32_t n;
    float b[4];
    if (layout_text(x, el, f, runs, &n, b))
        for (uint32_t r = 0; r < n; r++)
            svg_path_append(out, &runs[r].path);
    free_runs(x, runs, n);
    au_free(runs);
}
