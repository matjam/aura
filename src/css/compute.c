/* Computed values: from a declared (cascaded) value of a longhand to the
 * field in the element's style groups. Font-relative units resolve
 * against the element's font size (the parent's while computing
 * font-size), rem against the root, ex and ch through the metrics
 * callback, viewport units against the device. Percentages stay for
 * layout unless the property defines them against something known here
 * (font-size, line-height, vertical-align). */
#include "css_int.h"

#include <math.h>

static CssArena compute_arena;

/* ---- Lengths ---- */

static void need_metrics(CssComputeCtx *c)
{
    if (c->ex >= 0)
        return;
    float fs = c->style->inh->font_size;
    c->ex = fs * 0.5f;
    c->ch = fs * 0.5f;
    if (c->metrics)
        c->metrics(c->metrics_ctx, c->style, &c->ex, &c->ch);
}

static bool computing_font_size;

float css_length_px(CssComputeCtx *c, float v, int unit)
{
    float f;
    int canon;
    if (css_unit_canonical(unit, &f, &canon))
        return v * f;
    const AuCssDevice *d = c->dev;
    float em = computing_font_size ? c->font_size_for_em : c->style->inh->font_size;
    float rem = c->root ? c->root->inh->font_size : (computing_font_size ? 16 : c->style->inh->font_size);
    float vw = d->width / 100, vh = d->height / 100;
    bool horiz = c->style->inh->writing_mode == AU_WRITING_MODE_HORIZONTAL_TB;
    switch (unit) {
    case AU_UNIT_EM: return v * em;
    case AU_UNIT_REM: return v * rem;
    case AU_UNIT_EX:
    case AU_UNIT_CH:
        if (computing_font_size)
            return v * em * 0.5f;
        need_metrics(c);
        return v * (unit == AU_UNIT_EX ? c->ex : c->ch);
    case AU_UNIT_REX:
    case AU_UNIT_RCH:
        return v * rem * 0.5f;
    case AU_UNIT_CAP: return v * em * 0.7f;
    case AU_UNIT_RCAP: return v * rem * 0.7f;
    case AU_UNIT_IC: return v * em;
    case AU_UNIT_RIC: return v * rem;
    case AU_UNIT_LH:
    case AU_UNIT_RLH: {
        const AuStyle *s = unit == AU_UNIT_RLH && c->root ? c->root : c->style;
        float fs = unit == AU_UNIT_RLH ? rem : em;
        const AuLen *lh = &s->inh->line_height;
        if (computing_font_size || (unit == AU_UNIT_LH && c->flags & 1)) {
            /* line-height of the parent while computing font-size or line-height */
            lh = c->parent ? &c->parent->inh->line_height : &css_initial_style.inh->line_height;
            fs = c->parent ? c->parent->inh->font_size : 16;
        }
        float px = lh->kind == AU_LEN_PX ? lh->u.px : lh->kind == AU_LEN_NUMBER ? lh->u.num * fs : 1.2f * fs;
        return v * px;
    }
    case AU_UNIT_VW: case AU_UNIT_SVW: case AU_UNIT_LVW: case AU_UNIT_DVW: case AU_UNIT_CQW:
        c->flags |= 2;
        return v * vw;
    case AU_UNIT_VH: case AU_UNIT_SVH: case AU_UNIT_LVH: case AU_UNIT_DVH: case AU_UNIT_CQH:
        c->flags |= 2;
        return v * vh;
    case AU_UNIT_VI: case AU_UNIT_SVI: case AU_UNIT_LVI: case AU_UNIT_DVI: case AU_UNIT_CQI:
        c->flags |= 2;
        return v * (horiz ? vw : vh);
    case AU_UNIT_VB: case AU_UNIT_SVB: case AU_UNIT_LVB: case AU_UNIT_DVB: case AU_UNIT_CQB:
        c->flags |= 2;
        return v * (horiz ? vh : vw);
    case AU_UNIT_VMIN: case AU_UNIT_SVMIN: case AU_UNIT_LVMIN: case AU_UNIT_DVMIN: case AU_UNIT_CQMIN:
        c->flags |= 2;
        return v * (vw < vh ? vw : vh);
    case AU_UNIT_VMAX: case AU_UNIT_SVMAX: case AU_UNIT_LVMAX: case AU_UNIT_DVMAX: case AU_UNIT_CQMAX:
        c->flags |= 2;
        return v * (vw > vh ? vw : vh);
    default:
        return v;
    }
}

/* ---- Trees ---- */

AuCssValue *css_compute_tree(CssComputeCtx *c, CssArena *a, const AuCssValue *v)
{
    AuCssValue *r = css_new_value(a, v->type);
    *r = *v;
    switch (v->type) {
    case AU_CSSV_DIMENSION: {
        float f;
        int canon;
        if (au_unit_is_length(v->unit)) {
            r->u.num = css_length_px(c, v->u.num, v->unit);
            r->unit = AU_UNIT_PX;
        } else if (css_unit_canonical(v->unit, &f, &canon)) {
            r->u.num = v->u.num * f;
            r->unit = (uint8_t)canon;
        }
        r->n = 0;
        return r;
    }
    case AU_CSSV_NUMBER:
    case AU_CSSV_PERCENT:
        r->n = 0;
        return r;
    case AU_CSSV_IDENT:
    case AU_CSSV_STRING:
    case AU_CSSV_URL:
        r->u.str = css_arena_strndup(a, v->u.str, v->n);
        return r;
    case AU_CSSV_FUNCTION:
        if (v->kw == AU_K_attr && c->el && v->n >= 1) {
            /* attr(name [type]? [, fallback]?): the attribute as a string */
            uint32_t n;
            const AuCssValue *p = css_items(&v->u.items[0], &n);
            const char *name = p[0].type == AU_CSSV_IDENT ? p[0].u.str : au_css_keyword_name((AuCssKeyword)p[0].kw);
            size_t nlen = p[0].type == AU_CSSV_IDENT ? p[0].n : strlen(name);
            const char *val = css_attr_value(c->el, name, nlen);
            c->flags |= 4;
            if (!val && v->n == 2)
                return css_compute_tree(c, a, &v->u.items[1]);
            r = css_new_value(a, AU_CSSV_STRING);
            r->u.str = css_arena_strndup(a, val ? val : "", val ? strlen(val) : 0);
            r->n = val ? (uint32_t)strlen(val) : 0;
            return r;
        }
        /* fall through */
    case AU_CSSV_LIST:
    case AU_CSSV_COMMA_LIST:
    case AU_CSSV_CALC: {
        r->u.items = v->n ? css_arena_alloc(a, v->n * sizeof(AuCssValue)) : NULL;
        for (uint32_t i = 0; i < v->n; i++)
            r->u.items[i] = *css_compute_tree(c, a, &v->u.items[i]);
        if (v->type == AU_CSSV_CALC) {
            css_calc_simplify(a, r);
        }
        return r;
    }
    default:
        return r;
    }
}

bool css_compute_color(CssComputeCtx *c, const AuCssValue *v, AuColor *out)
{
    bool cur;
    (void)c;
    return css_value_to_color(v, out, &cur);
}

/* ---- Field access ---- */

static void *field(CssComputeCtx *c, uint16_t prop)
{
    const CssPropInfo *pi = &css_props[prop];
    return (char *)css_mut_group(c->style, pi->group) + pi->offset;
}

static void set_len(AuLen *dst, const AuLen *src)
{
    if (src->kind == AU_LEN_CALC)
        au_css_blob_retain(src->u.calc);
    if (dst->kind == AU_LEN_CALC)
        au_css_blob_release(dst->u.calc);
    *dst = *src;
}

static void set_blob(AuCssBlob **dst, AuCssBlob *src)
{
    au_css_blob_retain(src);
    au_css_blob_release(*dst);
    *dst = src;
}

/* Computes a length-percentage (or keyword) into an AuLen. */
static bool compute_len(CssComputeCtx *c, const AuCssValue *v, AuLen *out, bool nonneg)
{
    memset(out, 0, sizeof *out);
    switch (v->type) {
    case AU_CSSV_KEYWORD:
        switch (v->kw) {
        case AU_K_auto_: out->kind = AU_LEN_AUTO; return true;
        case AU_K_none: out->kind = AU_LEN_NONE; return true;
        case AU_K_normal: out->kind = AU_LEN_NORMAL; return true;
        case AU_K_min_content: out->kind = AU_LEN_MIN_CONTENT; return true;
        case AU_K_max_content: out->kind = AU_LEN_MAX_CONTENT; return true;
        case AU_K_fit_content: out->kind = AU_LEN_FIT_CONTENT; return true;
        case AU_K_stretch: out->kind = AU_LEN_STRETCH; return true;
        case AU_K_content: out->kind = AU_LEN_CONTENT; return true;
        case AU_K_from_font: out->kind = AU_LEN_FROM_FONT; return true;
        default: return false;
        }
    case AU_CSSV_DIMENSION:
        out->kind = AU_LEN_PX;
        out->u.px = css_length_px(c, v->u.num, v->unit);
        if (nonneg && out->u.px < 0)
            out->u.px = 0;
        return true;
    case AU_CSSV_PERCENT:
        out->kind = AU_LEN_PCT;
        out->u.pct = v->u.num;
        if (nonneg && out->u.pct < 0)
            out->u.pct = 0;
        return true;
    case AU_CSSV_NUMBER:
        out->kind = AU_LEN_NUMBER;
        out->u.num = v->u.num;
        if (nonneg && out->u.num < 0)
            out->u.num = 0;
        return true;
    case AU_CSSV_CALC: {
        AuCssValue *t = css_compute_tree(c, &compute_arena, v);
        if (t->type != AU_CSSV_CALC)
            return compute_len(c, t, out, nonneg);
        if (css_value_type(t) == CT_NUMBER) {
            AuLen tmp = {{0}, AU_LEN_CALC};
            tmp.u.calc = css_blob_pack(t, NULL);
            out->kind = AU_LEN_NUMBER;
            out->u.num = au_len_resolve(&tmp, 0);
            au_css_blob_release(tmp.u.calc);
            if (nonneg && out->u.num < 0)
                out->u.num = 0;
            return true;
        }
        if (nonneg) {
            AuCssValue *mx = css_new_list(&compute_arena, AU_CSSV_CALC, 2);
            mx->unit = AU_CALC_MAX;
            mx->u.items[0] = *t;
            mx->u.items[1] = *css_new_num(&compute_arena, AU_CSSV_DIMENSION, 0, AU_UNIT_PX);
            t = mx;
        }
        out->kind = AU_LEN_CALC;
        out->u.calc = css_blob_pack(t, NULL);
        return true;
    }
    case AU_CSSV_FUNCTION:
        if (v->kw == AU_K_fit_content && v->n == 1) {
            AuLen arg;
            if (!compute_len(c, &v->u.items[0], &arg, true))
                return false;
            if (arg.kind == AU_LEN_PX) {
                out->kind = AU_LEN_FIT_CONTENT_PX;
                out->u.px = arg.u.px;
            } else {
                /* fit-content(<percentage>) is kept as plain fit-content */
                if (arg.kind == AU_LEN_CALC)
                    au_css_blob_release(arg.u.calc);
                out->kind = AU_LEN_FIT_CONTENT;
            }
            return true;
        }
        return false;
    default:
        return false;
    }
}

static float compute_number(CssComputeCtx *c, const AuCssValue *v)
{
    if (v->type == AU_CSSV_NUMBER)
        return v->u.num;
    if (v->type == AU_CSSV_PERCENT)
        return v->u.num / 100;
    if (v->type == AU_CSSV_CALC) {
        AuCssValue *t = css_compute_tree(c, &compute_arena, v);
        if (t->type == AU_CSSV_NUMBER)
            return t->u.num;
        if (t->type == AU_CSSV_PERCENT)
            return t->u.num / 100;
        AuLen l = {{0}, AU_LEN_CALC};
        AuCssBlob *b = css_blob_pack(t, NULL);
        l.u.calc = b;
        float r = au_len_resolve(&l, 100) / 100; /* a number-percentage mix: % of 1 */
        au_css_blob_release(b);
        return r;
    }
    return 0;
}

/* ---- Special properties ---- */

bool css_display_code(const AuCssValue *v, uint16_t *out);
bool css_align_code(uint16_t prop, const AuCssValue *v, uint16_t *out);

static float abs_font_size(uint16_t kw)
{
    switch (kw) {
    case AU_K_xx_small: return 9;
    case AU_K_x_small: return 10;
    case AU_K_small: return 13;
    case AU_K_medium: return 16;
    case AU_K_large: return 18;
    case AU_K_x_large: return 24;
    case AU_K_xx_large: return 32;
    case AU_K_xxx_large: return 48;
    default: return -1;
    }
}

static bool apply_font_size(CssComputeCtx *c, const AuCssValue *v)
{
    float parent = c->parent ? c->parent->inh->font_size : 16;
    float px;
    if (v->type == AU_CSSV_KEYWORD) {
        if (v->kw == AU_K_larger)
            px = parent * 1.2f;
        else if (v->kw == AU_K_smaller)
            px = parent / 1.2f;
        else if ((px = abs_font_size(v->kw)) < 0)
            return false;
    } else if (v->type == AU_CSSV_PERCENT) {
        px = parent * v->u.num / 100;
    } else {
        computing_font_size = true;
        c->font_size_for_em = parent;
        AuLen l;
        bool ok = compute_len(c, v, &l, true);
        computing_font_size = false;
        if (!ok)
            return false;
        if (l.kind == AU_LEN_PCT)
            px = parent * l.u.pct / 100;
        else if (l.kind == AU_LEN_CALC) {
            px = au_len_resolve(&l, parent);
            au_css_blob_release(l.u.calc);
        } else if (l.kind == AU_LEN_PX)
            px = l.u.px;
        else
            return false;
    }
    if (px < 0 || px != px)
        px = 0;
    AuStyleInh *inh = css_mut_group(c->style, G_INH);
    inh->font_size = px;
    c->ex = -1;
    return true;
}

static bool apply_font_weight(CssComputeCtx *c, const AuCssValue *v)
{
    float pw = c->parent ? c->parent->inh->font_weight : 400;
    float w;
    if (v->type == AU_CSSV_KEYWORD) {
        switch (v->kw) {
        case AU_K_normal: w = 400; break;
        case AU_K_bold: w = 700; break;
        case AU_K_bolder: w = pw < 350 ? 400 : pw < 550 ? 700 : pw < 900 ? 900 : pw; break;
        case AU_K_lighter: w = pw < 100 ? pw : pw < 550 ? 100 : pw < 750 ? 400 : 700; break;
        default: return false;
        }
    } else {
        w = compute_number(c, v);
    }
    if (w < 1)
        w = 1;
    if (w > 1000)
        w = 1000;
    ((AuStyleInh *)css_mut_group(c->style, G_INH))->font_weight = (uint16_t)(w + 0.5f);
    return true;
}

static bool apply_line_height(CssComputeCtx *c, const AuCssValue *v)
{
    AuLen l;
    float fs = c->style->inh->font_size;
    if (css_is_kw(v, AU_K_normal)) {
        l.kind = AU_LEN_NORMAL;
        l.u.px = 0;
    } else if (v->type == AU_CSSV_PERCENT) {
        l.kind = AU_LEN_PX;
        l.u.px = fs * v->u.num / 100;
    } else {
        c->flags |= 1; /* lh units refer to the parent here */
        bool ok = compute_len(c, v, &l, true);
        c->flags &= (uint8_t)~1;
        if (!ok)
            return false;
        if (l.kind == AU_LEN_PCT) {
            l.kind = AU_LEN_PX;
            l.u.px = fs * l.u.pct / 100;
        } else if (l.kind == AU_LEN_CALC) {
            float px = au_len_resolve(&l, fs);
            au_css_blob_release(l.u.calc);
            l.kind = AU_LEN_PX;
            l.u.px = px < 0 ? 0 : px;
        }
    }
    set_len(&((AuStyleInh *)css_mut_group(c->style, G_INH))->line_height, &l);
    return true;
}

static float stretch_pct(uint16_t kw)
{
    switch (kw) {
    case AU_K_ultra_condensed: return 50;
    case AU_K_extra_condensed: return 62.5f;
    case AU_K_condensed: return 75;
    case AU_K_semi_condensed: return 87.5f;
    case AU_K_normal: return 100;
    case AU_K_semi_expanded: return 112.5f;
    case AU_K_expanded: return 125;
    case AU_K_extra_expanded: return 150;
    case AU_K_ultra_expanded: return 200;
    default: return -1;
    }
}

static float border_width_px(CssComputeCtx *c, const AuCssValue *v)
{
    float px;
    if (v->type == AU_CSSV_KEYWORD)
        px = v->kw == AU_K_thin ? 1 : v->kw == AU_K_thick ? 5 : 3;
    else {
        AuLen l;
        if (!compute_len(c, v, &l, true))
            return 0;
        px = l.kind == AU_LEN_PX ? l.u.px : 0;
        if (l.kind == AU_LEN_CALC)
            au_css_blob_release(l.u.calc);
    }
    /* snap as a border width (1 device pixel per CSS px here) */
    if (px > 0 && px < 1)
        px = 1;
    else if (px >= 1)
        px = floorf(px);
    return px;
}

static uint8_t bits_of(const AuCssValue *v, uint16_t prop)
{
    uint32_t n;
    const AuCssValue *it = css_items(v, &n);
    uint8_t b = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t k = it[i].kw;
        if (prop == AU_PROP_contain) {
            switch (k) {
            case AU_K_strict: b |= AU_CONTAIN_SIZE | AU_CONTAIN_LAYOUT | AU_CONTAIN_PAINT | AU_CONTAIN_STYLE; break;
            case AU_K_content: b |= AU_CONTAIN_LAYOUT | AU_CONTAIN_PAINT | AU_CONTAIN_STYLE; break;
            case AU_K_size: b |= AU_CONTAIN_SIZE; break;
            case AU_K_inline_size: b |= AU_CONTAIN_INLINE_SIZE; break;
            case AU_K_layout: b |= AU_CONTAIN_LAYOUT; break;
            case AU_K_style: b |= AU_CONTAIN_STYLE; break;
            case AU_K_paint: b |= AU_CONTAIN_PAINT; break;
            default: break;
            }
        } else if (prop == AU_PROP_text_decoration_line) {
            switch (k) {
            case AU_K_underline: b |= AU_TDL_UNDERLINE; break;
            case AU_K_overline: b |= AU_TDL_OVERLINE; break;
            case AU_K_line_through: b |= AU_TDL_LINE_THROUGH; break;
            case AU_K_blink: b |= AU_TDL_BLINK; break;
            case AU_K_spelling_error: b |= AU_TDL_SPELLING_ERROR; break;
            case AU_K_grammar_error: b |= AU_TDL_GRAMMAR_ERROR; break;
            default: break;
            }
        } else { /* grid-auto-flow */
            if (k == AU_K_column)
                b |= AU_GRID_FLOW_COLUMN;
            else if (k == AU_K_dense)
                b |= AU_GRID_FLOW_DENSE;
        }
    }
    return b;
}

static AuCssBlob *tree_blob(CssComputeCtx *c, const AuCssValue *v, AuCssBlob *declared)
{
    if (declared && !css_value_needs_compute(v))
        return au_css_blob_retain(declared);
    AuCssValue *t = css_compute_tree(c, &compute_arena, v);
    return css_blob_pack(t, declared ? declared->base_url : NULL);
}

static bool apply_special(CssComputeCtx *c, uint16_t prop, const AuCssValue *v, AuCssBlob *blob)
{
    switch (prop) {
    case AU_PROP_font_size:
        return apply_font_size(c, v);
    case AU_PROP_font_weight:
        return apply_font_weight(c, v);
    case AU_PROP_font_style: {
        uint8_t s = v->kw == AU_K_italic ? AU_FONT_STYLE_ITALIC : v->kw == AU_K_oblique ? AU_FONT_STYLE_OBLIQUE
                                                                                     : AU_FONT_STYLE_NORMAL;
        ((AuStyleInh *)css_mut_group(c->style, G_INH))->font_style = s;
        return true;
    }
    case AU_PROP_font_stretch: {
        float p = v->type == AU_CSSV_KEYWORD ? stretch_pct(v->kw) : compute_number(c, v) * 100;
        if (p < 0)
            p = v->type == AU_CSSV_KEYWORD ? 100 : 0;
        ((AuStyleInh *)css_mut_group(c->style, G_INH))->font_stretch = p;
        return true;
    }
    case AU_PROP_line_height:
        return apply_line_height(c, v);
    case AU_PROP_border_spacing: {
        uint32_t n;
        const AuCssValue *it = css_items(v, &n);
        AuStyleInhRare *r = css_mut_group(c->style, G_INHR);
        for (uint32_t i = 0; i < 2; i++) {
            AuLen l;
            if (!compute_len(c, &it[n > 1 ? i : 0], &l, true))
                return false;
            r->border_spacing[i] = l.kind == AU_LEN_PX ? l.u.px : 0;
            if (l.kind == AU_LEN_CALC)
                au_css_blob_release(l.u.calc);
        }
        return true;
    }
    case AU_PROP_cursor: {
        uint32_t n;
        const AuCssValue *it = css_items(v, &n);
        if (v->type != AU_CSSV_COMMA_LIST) {
            n = 1;
            it = v;
        }
        int idx = css_prop_kw_index(prop, it[n - 1].kw);
        AuStyleInhRare *r = css_mut_group(c->style, G_INHR);
        r->cursor = (uint8_t)(idx < 0 ? 0 : idx);
        AuCssBlob *imgs = NULL;
        if (n > 1) {
            AuCssValue *l = css_new_list(&compute_arena, AU_CSSV_COMMA_LIST, n - 1);
            for (uint32_t i = 0; i + 1 < n; i++)
                l->u.items[i] = *css_compute_tree(c, &compute_arena, &it[i]);
            imgs = css_blob_pack(l, blob ? blob->base_url : NULL);
        }
        set_blob(&r->cursor_images, imgs);
        au_css_blob_release(imgs);
        return true;
    }
    case AU_PROP_display: {
        uint16_t d;
        if (!css_display_code(v, &d))
            return false;
        AuStyleBox *b = css_mut_group(c->style, G_BOX);
        b->display = d;
        b->display_specified = d;
        return true;
    }
    case AU_PROP_vertical_align: {
        AuStyleBox *b = css_mut_group(c->style, G_BOX);
        if (v->type == AU_CSSV_KEYWORD) {
            static const uint16_t kws[] = {AU_K_baseline, AU_K_sub, AU_K_super, AU_K_text_top, AU_K_text_bottom,
                                           AU_K_middle, AU_K_top, AU_K_bottom};
            for (uint8_t i = 0; i < CSS_ARRAY_LEN(kws); i++)
                if (kws[i] == v->kw) {
                    b->vertical_align = i;
                    set_len(&b->vertical_align_len, &(AuLen){.kind = AU_LEN_PX});
                    return true;
                }
            return false;
        }
        AuLen l;
        if (!compute_len(c, v, &l, false))
            return false;
        if (l.kind == AU_LEN_PCT || l.kind == AU_LEN_CALC) {
            /* percentages refer to the line-height */
            const AuLen *lh = &c->style->inh->line_height;
            float fs = c->style->inh->font_size;
            float basis = lh->kind == AU_LEN_PX ? lh->u.px : lh->kind == AU_LEN_NUMBER ? lh->u.num * fs : 1.2f * fs;
            float px = au_len_resolve(&l, basis);
            if (l.kind == AU_LEN_CALC)
                au_css_blob_release(l.u.calc);
            l.kind = AU_LEN_PX;
            l.u.px = px;
        }
        b->vertical_align = AU_VALIGN_LENGTH;
        set_len(&b->vertical_align_len, &l);
        return true;
    }
    case AU_PROP_aspect_ratio: {
        uint32_t n;
        const AuCssValue *it = css_items(v, &n);
        AuStyleBox *b = css_mut_group(c->style, G_BOX);
        b->aspect_ratio_auto = 0;
        b->aspect_ratio = 0;
        float num = 0, den = 1;
        bool have = false;
        for (uint32_t i = 0; i < n; i++) {
            if (css_is_kw(&it[i], AU_K_auto_)) {
                b->aspect_ratio_auto = 1;
            } else if (it[i].type == AU_CSSV_DELIM) {
                if (i + 1 < n)
                    den = compute_number(c, &it[++i]);
            } else {
                num = compute_number(c, &it[i]);
                have = true;
            }
        }
        if (have && num > 0 && den > 0)
            b->aspect_ratio = num / den;
        if (!have)
            b->aspect_ratio_auto = 1;
        return true;
    }
    case AU_PROP_border_top_width:
    case AU_PROP_border_right_width:
    case AU_PROP_border_bottom_width:
    case AU_PROP_border_left_width:
        *(float *)field(c, prop) = border_width_px(c, v);
        return true;
    case AU_PROP_outline_width:
        ((AuStyleRare *)css_mut_group(c->style, G_RARE))->outline_width = border_width_px(c, v);
        return true;
    case AU_PROP_border_top_left_radius:
    case AU_PROP_border_top_right_radius:
    case AU_PROP_border_bottom_right_radius:
    case AU_PROP_border_bottom_left_radius: {
        uint32_t n;
        const AuCssValue *it = css_items(v, &n);
        AuLen *dst = field(c, prop);
        for (uint32_t i = 0; i < 2; i++) {
            AuLen l;
            if (!compute_len(c, &it[n > 1 ? i : 0], &l, true))
                return false;
            set_len(&dst[i], &l);
            if (l.kind == AU_LEN_CALC)
                au_css_blob_release(l.u.calc);
        }
        return true;
    }
    case AU_PROP_justify_content:
    case AU_PROP_align_content:
    case AU_PROP_justify_items:
    case AU_PROP_align_items:
    case AU_PROP_justify_self:
    case AU_PROP_align_self: {
        uint16_t code;
        if (!css_align_code(prop, v, &code))
            return false;
        *(uint16_t *)field(c, prop) = code;
        return true;
    }
    case AU_PROP_grid_auto_flow:
    case AU_PROP_contain:
    case AU_PROP_text_decoration_line:
        *(uint8_t *)field(c, prop) = bits_of(v, prop);
        return true;
    default:
        return false;
    }
}

/* ---- Applying a value ---- */

static bool blob_none_is_null(uint16_t prop) { return strcmp(css_props[prop].initial, "none") == 0; }

bool css_compute_apply(CssComputeCtx *c, uint16_t prop, const AuCssValue *v, AuCssBlob *blob)
{
    const CssPropInfo *pi = &css_props[prop];
    css_arena_reset(&compute_arena);
    if (!v)
        return false;
    switch (pi->type) {
    case T_KW: {
        if (v->type != AU_CSSV_KEYWORD)
            return false;
        int idx = css_prop_kw_index(prop, v->kw);
        if (idx < 0)
            return false;
        *(uint8_t *)field(c, prop) = (uint8_t)idx;
        return true;
    }
    case T_LEN: {
        AuLen l;
        if (!compute_len(c, v, &l, (pi->flags & F_NONNEG) != 0))
            return false;
        set_len(field(c, prop), &l);
        if (l.kind == AU_LEN_CALC)
            au_css_blob_release(l.u.calc);
        return true;
    }
    case T_PX: {
        float px = 0;
        if (v->type != AU_CSSV_KEYWORD) {
            AuLen l;
            if (!compute_len(c, v, &l, (pi->flags & F_NONNEG) != 0))
                return false;
            px = l.kind == AU_LEN_PX ? l.u.px : 0;
            if (l.kind == AU_LEN_CALC) {
                px = au_len_resolve(&l, 0);
                au_css_blob_release(l.u.calc);
            }
        }
        *(float *)field(c, prop) = px;
        return true;
    }
    case T_COLOR: {
        AuColor col;
        if (!css_compute_color(c, v, &col))
            return false;
        if (prop == AU_PROP_color && col == AU_COLOR_CURRENT)
            col = c->parent ? c->parent->inh->color : AU_COLOR_RGBA(0, 0, 0, 255);
        *(AuColor *)field(c, prop) = col;
        return true;
    }
    case T_NUM: {
        float n = compute_number(c, v);
        if (prop == AU_PROP_opacity)
            n = n < 0 ? 0 : n > 1 ? 1 : n;
        else if ((pi->flags & F_NONNEG) && n < 0)
            n = 0;
        *(float *)field(c, prop) = n;
        return true;
    }
    case T_INT: {
        int32_t iv;
        if (v->type == AU_CSSV_KEYWORD)
            iv = INT32_MIN;
        else {
            float f = compute_number(c, v);
            iv = (int32_t)(f < 0 ? f - 0.5f : f + 0.5f);
            if ((pi->flags & F_NONNEG) && iv < 1)
                iv = 1;
        }
        *(int32_t *)field(c, prop) = iv;
        return true;
    }
    case T_BLOB: {
        AuCssBlob *b = NULL;
        if (!(v->type == AU_CSSV_KEYWORD && v->kw == AU_K_none && blob_none_is_null(prop)))
            b = tree_blob(c, v, blob);
        set_blob(field(c, prop), b);
        au_css_blob_release(b);
        return true;
    }
    case T_SPECIAL:
        return apply_special(c, prop, v, blob);
    default:
        return false;
    }
}

/* ---- inherit and initial ---- */

static void copy_prop(AuStyle *dst, const AuStyle *src, uint16_t prop)
{
    const CssPropInfo *pi = &css_props[prop];
    int g = pi->group;
    const char *sg = css_style_group(src, g);
    if (css_style_group(dst, g) == sg)
        return; /* same group: already equal */
    char *dg = css_mut_group(dst, g);
    const char *s = sg + pi->offset;
    char *d = dg + pi->offset;
    size_t size = 0;
    switch (pi->type) {
    case T_KW: size = 1; break;
    case T_LEN: set_len((AuLen *)d, (const AuLen *)s); return;
    case T_PX: case T_COLOR: case T_NUM: case T_INT: size = 4; break;
    case T_BLOB: set_blob((AuCssBlob **)d, *(AuCssBlob *const *)s); return;
    case T_SPECIAL:
        switch (prop) {
        case AU_PROP_font_size: case AU_PROP_font_stretch: size = 4; break;
        case AU_PROP_font_weight: size = 2; break;
        case AU_PROP_font_style: size = 1; break;
        case AU_PROP_line_height: set_len((AuLen *)d, (const AuLen *)s); return;
        case AU_PROP_border_spacing: size = 8; break;
        case AU_PROP_cursor: {
            const AuStyleInhRare *a = (const AuStyleInhRare *)sg;
            AuStyleInhRare *b = (AuStyleInhRare *)dg;
            b->cursor = a->cursor;
            set_blob(&b->cursor_images, a->cursor_images);
            return;
        }
        case AU_PROP_display: {
            const AuStyleBox *a = (const AuStyleBox *)sg;
            AuStyleBox *b = (AuStyleBox *)dg;
            /* inherit takes the parent's computed (blockified) value */
            b->display = a->display;
            b->display_specified = a->display;
            return;
        }
        case AU_PROP_vertical_align: {
            const AuStyleBox *a = (const AuStyleBox *)sg;
            AuStyleBox *b = (AuStyleBox *)dg;
            b->vertical_align = a->vertical_align;
            set_len(&b->vertical_align_len, &a->vertical_align_len);
            return;
        }
        case AU_PROP_aspect_ratio: {
            const AuStyleBox *a = (const AuStyleBox *)sg;
            AuStyleBox *b = (AuStyleBox *)dg;
            b->aspect_ratio = a->aspect_ratio;
            b->aspect_ratio_auto = a->aspect_ratio_auto;
            return;
        }
        case AU_PROP_border_top_width: case AU_PROP_border_right_width: case AU_PROP_border_bottom_width:
        case AU_PROP_border_left_width: case AU_PROP_outline_width:
            size = 4;
            break;
        case AU_PROP_border_top_left_radius: case AU_PROP_border_top_right_radius:
        case AU_PROP_border_bottom_right_radius: case AU_PROP_border_bottom_left_radius:
            set_len((AuLen *)d, (const AuLen *)s);
            set_len((AuLen *)d + 1, (const AuLen *)s + 1);
            return;
        case AU_PROP_justify_content: case AU_PROP_align_content: case AU_PROP_justify_items:
        case AU_PROP_align_items: case AU_PROP_justify_self: case AU_PROP_align_self:
            size = 2;
            break;
        default:
            size = 1;
            break;
        }
        break;
    default:
        return;
    }
    memcpy(d, s, size);
}

void css_compute_inherit(CssComputeCtx *c, uint16_t prop)
{
    const AuStyle *src = c->parent ? c->parent : &css_initial_style;
    copy_prop(c->style, src, prop);
    if (prop == AU_PROP_font_size)
        c->ex = -1;
}

void css_compute_initial(CssComputeCtx *c, uint16_t prop)
{
    if (prop == AU_PROP_color) {
        ((AuStyleInh *)css_mut_group(c->style, G_INH))->color = css_initial_style.inh->color;
        return;
    }
    copy_prop(c->style, &css_initial_style, prop);
    if (prop == AU_PROP_font_size)
        c->ex = -1;
}

/* ---- Fixups ---- */

static uint16_t blockify(uint16_t d)
{
    int inner = au_display_inner(d);
    bool li = au_display_list_item(d);
    if (inner == AU_DISPLAY_INNER_NONE)
        return d;
    if (inner == AU_DISPLAY_INNER_CONTENTS)
        return d;
    if (inner >= AU_DISPLAY_INNER_TABLE_ROW_GROUP && inner <= AU_DISPLAY_INNER_RUBY_TEXT_CONTAINER)
        return AU_DISPLAY_BLOCK;
    if (inner == AU_DISPLAY_INNER_RUBY)
        return AU_DISPLAY_BLOCK;
    if (au_display_outer(d) == AU_DISPLAY_OUTER_INLINE && inner == AU_DISPLAY_INNER_FLOW && !li)
        return AU_DISPLAY_BLOCK;
    return AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, inner, li);
}

void css_compute_finish(CssComputeCtx *c, uint16_t parent_display)
{
    AuStyle *s = c->style;
    const AuStyleBox *b = s->box;
    uint16_t d = b->display_specified;
    bool out_of_flow = b->position == AU_POSITION_ABSOLUTE || b->position == AU_POSITION_FIXED;
    int pinner = au_display_inner(parent_display);
    bool flex_item = c->parent && (pinner == AU_DISPLAY_INNER_FLEX || pinner == AU_DISPLAY_INNER_GRID ||
                                   pinner == AU_DISPLAY_INNER_WEBKIT_BOX);
    uint16_t nd = d;
    if (c->is_root) {
        if (au_display_inner(d) == AU_DISPLAY_INNER_CONTENTS)
            nd = AU_DISPLAY_BLOCK;
        else
            nd = blockify(d);
    } else if (out_of_flow || b->float_ != AU_FLOAT_NONE || flex_item) {
        nd = blockify(d);
    }
    if (nd != b->display || (out_of_flow && b->float_ != AU_FLOAT_NONE)) {
        AuStyleBox *mb = css_mut_group(s, G_BOX);
        mb->display = nd;
        if (out_of_flow)
            mb->float_ = AU_FLOAT_NONE;
    }
    /* overflow: visible/clip pair with scrollable values */
    b = s->box;
    int ox = b->overflow_x, oy = b->overflow_y;
    bool vx = ox == AU_OVERFLOW_VISIBLE || ox == AU_OVERFLOW_CLIP, vy = oy == AU_OVERFLOW_VISIBLE || oy == AU_OVERFLOW_CLIP;
    if (vx != vy) {
        AuStyleBox *mb = css_mut_group(s, G_BOX);
        if (vx)
            mb->overflow_x = (uint8_t)(ox == AU_OVERFLOW_VISIBLE ? AU_OVERFLOW_AUTO : AU_OVERFLOW_HIDDEN);
        else
            mb->overflow_y = (uint8_t)(oy == AU_OVERFLOW_VISIBLE ? AU_OVERFLOW_AUTO : AU_OVERFLOW_HIDDEN);
    }
    /* border widths are 0 with style none or hidden */
    const AuStyleSurround *sr = s->surr;
    for (int k = 0; k < 4; k++) {
        if ((sr->border_style[k] == AU_BORDER_STYLE_NONE || sr->border_style[k] == AU_BORDER_STYLE_HIDDEN) &&
            sr->border_width[k] != 0) {
            AuStyleSurround *m = css_mut_group(s, G_SURR);
            m->border_width[k] = 0;
            sr = m;
        }
    }
    if (s->rare->outline_style == AU_OUTLINE_STYLE_NONE && s->rare->outline_width != 0)
        ((AuStyleRare *)css_mut_group(s, G_RARE))->outline_width = 0;
}
