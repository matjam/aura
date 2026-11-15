/* Property value validation: from the generic value tree of a
 * declaration to the tree stored for each longhand, or rejection. Each
 * pp_<name> function implements the grammar of the properties that use it
 * in props.def. Values out of range inside calc() are kept (they clamp at
 * computed-value time); a folded calc leaf has n == 1. */
#include "css_int.h"

/* ---- Common predicates ---- */

enum { P_PCT = 1, P_NONNEG = 2, P_QUIRK = 4, P_NUM = 8, P_QUIRKS_ON = 16 };

static bool from_calc(const AuCssValue *v) { return v->n != 0; }

static bool calc_type_ok(const AuCssValue *v, unsigned want)
{
    unsigned t = css_value_type(v);
    if (t & CT_INVALID)
        return false;
    return t != 0 ? (t & ~want) == 0 : (want & 0x4000) != 0;
}
#define WANT_NUMBER 0x4000

/* Length or length-percentage, normalized in place (unitless 0 and quirks
 * numbers become px). */
static bool norm_lp(AuCssValue *v, unsigned f)
{
    if (!v)
        return false;
    switch (v->type) {
    case AU_CSSV_DIMENSION:
        if (!au_unit_is_length(v->unit))
            return false;
        return !(f & P_NONNEG) || v->u.num >= 0 || from_calc(v);
    case AU_CSSV_PERCENT:
        if (!(f & P_PCT))
            return false;
        return !(f & P_NONNEG) || v->u.num >= 0 || from_calc(v);
    case AU_CSSV_NUMBER:
        if (f & P_NUM)
            return !(f & P_NONNEG) || v->u.num >= 0 || from_calc(v);
        if (from_calc(v))
            return false;
        if (v->u.num == 0 || ((f & P_QUIRK) && (f & P_QUIRKS_ON))) {
            if ((f & P_NONNEG) && v->u.num < 0)
                return false;
            v->type = AU_CSSV_DIMENSION;
            v->unit = AU_UNIT_PX;
            return true;
        }
        return false;
    case AU_CSSV_CALC:
        return calc_type_ok(v, CT_LENGTH | ((f & P_PCT) ? CT_PERCENT : 0) | ((f & P_NUM) ? WANT_NUMBER : 0));
    default:
        return false;
    }
}

static bool is_number(const AuCssValue *v, bool nonneg)
{
    if (v->type == AU_CSSV_NUMBER)
        return !nonneg || v->u.num >= 0 || from_calc(v);
    if (v->type == AU_CSSV_CALC)
        return calc_type_ok(v, WANT_NUMBER);
    return false;
}

static bool is_integer(const AuCssValue *v)
{
    if (v->type == AU_CSSV_NUMBER)
        return v->unit == 1 || from_calc(v);
    if (v->type == AU_CSSV_CALC)
        return calc_type_ok(v, WANT_NUMBER);
    return false;
}

static bool is_dim_type(const AuCssValue *v, unsigned ct, bool nonneg)
{
    if (v->type == AU_CSSV_DIMENSION)
        return (css_value_type(v) == ct) && (!nonneg || v->u.num >= 0 || from_calc(v));
    if (v->type == AU_CSSV_CALC)
        return calc_type_ok(v, ct);
    return false;
}

static bool is_angle(const AuCssValue *v)
{
    if (v->type == AU_CSSV_NUMBER && v->u.num == 0 && !from_calc(v))
        return true;
    return is_dim_type(v, CT_ANGLE, false);
}

static bool is_time(const AuCssValue *v, bool nonneg) { return is_dim_type(v, CT_TIME, nonneg); }

static bool is_kw_in(const AuCssValue *v, const uint16_t *kws, size_t n)
{
    if (v->type != AU_CSSV_KEYWORD)
        return false;
    for (size_t i = 0; i < n; i++)
        if (v->kw == kws[i])
            return true;
    return false;
}
#define KW_IN(v, ...) is_kw_in((v), (const uint16_t[]){__VA_ARGS__}, sizeof((const uint16_t[]){__VA_ARGS__}) / 2)

static bool is_wide_kw(int kw)
{
    return kw == AU_K_initial || kw == AU_K_inherit || kw == AU_K_unset || kw == AU_K_revert ||
           kw == AU_K_revert_layer || kw == AU_K_default_;
}

/* A <custom-ident>: an identifier that is not a CSS-wide keyword. Known
 * keywords are accepted too (their text is the ident). */
static bool is_custom_ident(const AuCssValue *v)
{
    if (v->type == AU_CSSV_IDENT)
        return true;
    return v->type == AU_CSSV_KEYWORD && !is_wide_kw(v->kw) && v->kw != AU_K_none;
}

/* Converts a color-denoting value to a COLOR node in place. */
static bool norm_color(AuCssValue *v)
{
    AuColor c;
    bool cur;
    if (!css_value_to_color(v, &c, &cur))
        return false;
    memset(v, 0, sizeof *v);
    v->type = AU_CSSV_COLOR;
    v->u.rgba = c;
    return true;
}

static AuCssValue *as_list(CssParseCtx *cx, AuCssValue *v, int type)
{
    if (v->type == type)
        return v;
    AuCssValue *l = css_new_list(cx->arena, type, 1);
    l->u.items[0] = *v;
    return l;
}

/* Applies fn to each item of a comma list (a single value is a list of
 * one); returns the list or NULL. */
typedef bool (*ItemFn)(CssParseCtx *cx, uint16_t prop, AuCssValue *item);
static AuCssValue *each_item(CssParseCtx *cx, uint16_t prop, AuCssValue *v, ItemFn fn)
{
    AuCssValue *l = as_list(cx, v, AU_CSSV_COMMA_LIST);
    for (uint32_t i = 0; i < l->n; i++)
        if (!fn(cx, prop, &l->u.items[i]))
            return NULL;
    return l;
}

static unsigned lp_flags(CssParseCtx *cx, uint16_t prop)
{
    const CssPropInfo *pi = &css_props[prop];
    return ((pi->flags & F_PCT) ? P_PCT : 0) | ((pi->flags & F_NONNEG) ? P_NONNEG : 0) |
           ((pi->flags & F_QUIRK) ? P_QUIRK : 0) | (cx->quirks ? P_QUIRKS_ON : 0);
}

/* ---- Generic longhand validation ---- */

static bool size_keyword(AuCssValue *v, unsigned mask)
{
    if (v->type == AU_CSSV_KEYWORD) {
        switch (v->kw) {
        case AU_K_auto_: return (mask & LK_AUTO) != 0;
        case AU_K_none: return (mask & LK_NONE) != 0;
        case AU_K_normal: return (mask & LK_NORMAL) != 0;
        case AU_K_content: return (mask & LK_CONTENT) != 0;
        case AU_K_from_font: return (mask & LK_FROM_FONT) != 0;
        case AU_K_min_content: case AU_K_max_content: case AU_K_fit_content: case AU_K_stretch:
            return (mask & LK_SIZE) != 0;
        case AU_K__webkit_fill_available: case AU_K__moz_available:
            if (!(mask & LK_SIZE))
                return false;
            v->kw = AU_K_stretch;
            return true;
        case AU_K__webkit_min_content: case AU_K__moz_min_content:
            v->kw = AU_K_min_content;
            return (mask & LK_SIZE) != 0;
        case AU_K__webkit_max_content: case AU_K__moz_max_content:
            v->kw = AU_K_max_content;
            return (mask & LK_SIZE) != 0;
        case AU_K__webkit_fit_content: case AU_K__moz_fit_content:
            v->kw = AU_K_fit_content;
            return (mask & LK_SIZE) != 0;
        default:
            return false;
        }
    }
    if (v->type == AU_CSSV_FUNCTION && v->kw == AU_K_fit_content && (mask & LK_SIZE))
        return v->n == 1 && norm_lp(&v->u.items[0], P_PCT | P_NONNEG);
    return false;
}

AuCssValue *css_validate_longhand(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    const CssPropInfo *pi = &css_props[prop];
    if (!v)
        return NULL;
    switch (pi->type) {
    case T_KW:
        return v->type == AU_CSSV_KEYWORD && css_prop_kw_index(prop, v->kw) >= 0 ? v : NULL;
    case T_LEN: {
        if (size_keyword(v, pi->extra))
            return v;
        unsigned f = lp_flags(cx, prop) | ((pi->extra & LK_NUMBER) ? P_NUM : 0);
        return norm_lp(v, f) ? v : NULL;
    }
    case T_PX: {
        if (v->type == AU_CSSV_KEYWORD)
            return ((pi->extra & LK_NORMAL) && v->kw == AU_K_normal) ? v : NULL;
        return norm_lp(v, lp_flags(cx, prop) & ~(unsigned)P_PCT) ? v : NULL;
    }
    case T_COLOR:
        return norm_color(v) ? v : NULL;
    case T_NUM:
        if (v->type == AU_CSSV_PERCENT && (pi->flags & F_PCT))
            return (v->u.num >= 0 || from_calc(v) || !(pi->flags & F_NONNEG)) ? v : NULL;
        if (v->type == AU_CSSV_CALC && (pi->flags & F_PCT) && calc_type_ok(v, CT_PERCENT | WANT_NUMBER))
            return v;
        return is_number(v, (pi->flags & F_NONNEG) != 0) ? v : NULL;
    case T_INT:
        if (v->type == AU_CSSV_KEYWORD)
            return ((v->kw == AU_K_auto_ && (pi->extra & LK_AUTO)) || (v->kw == AU_K_none && (pi->extra & LK_NONE)))
                       ? v : NULL;
        if (!is_integer(v))
            return NULL;
        if ((pi->flags & F_NONNEG) && v->type == AU_CSSV_NUMBER && v->u.num < 1 && !from_calc(v))
            return NULL; /* -webkit-line-clamp: positive integers */
        return v;
    case T_BLOB:
    case T_SPECIAL:
        return pi->parse(cx, prop, v);
    default:
        return NULL;
    }
}

/* ---- Fonts ---- */

AuCssValue *pp_font_size(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    if (KW_IN(v, AU_K_xx_small, AU_K_x_small, AU_K_small, AU_K_medium, AU_K_large, AU_K_x_large, AU_K_xx_large,
              AU_K_xxx_large, AU_K_larger, AU_K_smaller))
        return v;
    return norm_lp(v, lp_flags(cx, prop)) ? v : NULL;
}

AuCssValue *pp_font_weight(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (KW_IN(v, AU_K_normal, AU_K_bold, AU_K_bolder, AU_K_lighter))
        return v;
    if (v->type == AU_CSSV_NUMBER)
        return (from_calc(v) || (v->u.num >= 1 && v->u.num <= 1000)) ? v : NULL;
    return is_number(v, false) ? v : NULL;
}

AuCssValue *pp_font_style(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    if (KW_IN(v, AU_K_normal, AU_K_italic, AU_K_oblique))
        return v;
    if (v->type == AU_CSSV_LIST && v->n == 2 && css_is_kw(&v->u.items[0], AU_K_oblique) && is_angle(&v->u.items[1]))
        return css_new_kw(cx->arena, AU_K_oblique); /* the angle is not used */
    return NULL;
}

AuCssValue *pp_font_stretch(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (KW_IN(v, AU_K_normal, AU_K_ultra_condensed, AU_K_extra_condensed, AU_K_condensed, AU_K_semi_condensed,
              AU_K_semi_expanded, AU_K_expanded, AU_K_extra_expanded, AU_K_ultra_expanded))
        return v;
    if (v->type == AU_CSSV_PERCENT)
        return (v->u.num >= 0 || from_calc(v)) ? v : NULL;
    if (v->type == AU_CSSV_CALC && calc_type_ok(v, CT_PERCENT))
        return v;
    return NULL;
}

static bool is_generic_family(int kw)
{
    switch (kw) {
    case AU_K_serif: case AU_K_sans_serif: case AU_K_monospace: case AU_K_cursive: case AU_K_fantasy:
    case AU_K_system_ui: case AU_K_ui_serif: case AU_K_ui_sans_serif: case AU_K_ui_monospace:
    case AU_K_ui_rounded: case AU_K_math: case AU_K_emoji: case AU_K_fangsong:
        return true;
    default:
        return false;
    }
}

static bool family_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    if (it->type == AU_CSSV_STRING)
        return true;
    if (it->type == AU_CSSV_KEYWORD && is_generic_family(it->kw))
        return true;
    uint32_t n;
    const AuCssValue *p = css_items(it, &n);
    if (it->type == AU_CSSV_COMMA_LIST)
        return false;
    /* a sequence of identifiers: joined with single spaces */
    AuBuf b;
    au_buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (p[i].type == AU_CSSV_KEYWORD) {
            if (is_wide_kw(p[i].kw) && n == 1) {
                au_buf_free(&b);
                return false;
            }
            if (i)
                au_buf_putc(&b, ' ');
            au_buf_puts(&b, au_css_keyword_name((AuCssKeyword)p[i].kw));
        } else if (p[i].type == AU_CSSV_IDENT) {
            if (i)
                au_buf_putc(&b, ' ');
            au_buf_append(&b, p[i].u.str, p[i].n);
        } else {
            au_buf_free(&b);
            return false;
        }
    }
    AuCssValue *s = css_new_value(cx->arena, AU_CSSV_IDENT);
    s->u.str = css_arena_strndup(cx->arena, b.data ? b.data : "", b.len);
    s->n = (uint32_t)b.len;
    au_buf_free(&b);
    *it = *s;
    return true;
}

AuCssValue *pp_font_family(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    return each_item(cx, prop, v, family_item);
}

AuCssValue *pp_line_height(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_normal))
        return v;
    return norm_lp(v, P_PCT | P_NONNEG | P_NUM) ? v : NULL;
}

/* ---- Text, lists ---- */

AuCssValue *pp_list_style_type(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (v->type == AU_CSSV_STRING || css_is_kw(v, AU_K_none) || is_custom_ident(v))
        return v;
    return NULL;
}

AuCssValue *pp_quotes(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_auto_) || css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    const AuCssValue *it = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 2 || n % 2)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        if (it[i].type != AU_CSSV_STRING)
            return NULL;
    return v;
}

/* Shadow: box: [x y blur spread color inset]; text: [x y blur color]. */
static AuCssValue *parse_shadow(CssParseCtx *cx, AuCssValue *it, bool box)
{
    uint32_t n;
    const AuCssValue *p = css_items(it, &n);
    if (it->type == AU_CSSV_COMMA_LIST)
        return NULL;
    AuCssValue *lens[4] = {0};
    int nl = 0;
    AuCssValue color = {0};
    bool has_color = false, inset = false;
    bool lens_done = false;
    for (uint32_t i = 0; i < n; i++) {
        AuCssValue c = p[i];
        if (box && css_is_kw(&c, AU_K_inset)) {
            if (inset)
                return NULL;
            inset = true;
            if (nl)
                lens_done = true;
            continue;
        }
        if (norm_lp(&c, nl == 2 ? P_NONNEG : 0) && !(nl == 3 && !box) && nl < 4 && !lens_done) {
            lens[nl] = css_new_value(cx->arena, 0);
            *lens[nl] = c;
            nl++;
            continue;
        }
        if (nl == 2 && c.type == AU_CSSV_DIMENSION && c.u.num < 0)
            return NULL; /* negative blur */
        if (!has_color && norm_color(&c)) {
            color = c;
            has_color = true;
            if (nl)
                lens_done = true;
            continue;
        }
        return NULL;
    }
    if (nl < 2)
        return NULL;
    AuCssValue *r = css_new_list(cx->arena, AU_CSSV_LIST, box ? 6 : 4);
    for (int k = 0; k < (box ? 4 : 3); k++) {
        if (k < nl)
            r->u.items[k] = *lens[k];
        else
            r->u.items[k] = *css_new_num(cx->arena, AU_CSSV_DIMENSION, 0, AU_UNIT_PX);
    }
    AuCssValue *col = &r->u.items[box ? 4 : 3];
    col->type = AU_CSSV_COLOR;
    col->u.rgba = has_color ? color.u.rgba : AU_COLOR_CURRENT;
    if (box)
        r->u.items[5] = *css_new_kw(cx->arena, inset ? AU_K_inset : AU_K_none);
    return r;
}

static bool text_shadow_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    AuCssValue *s = parse_shadow(cx, it, false);
    if (!s)
        return false;
    *it = *s;
    return true;
}

static bool box_shadow_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    AuCssValue *s = parse_shadow(cx, it, true);
    if (!s)
        return false;
    *it = *s;
    return true;
}

AuCssValue *pp_text_shadow(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    if (css_is_kw(v, AU_K_none))
        return v;
    return each_item(cx, prop, v, text_shadow_item);
}

AuCssValue *pp_box_shadow(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    if (css_is_kw(v, AU_K_none))
        return v;
    return each_item(cx, prop, v, box_shadow_item);
}

AuCssValue *pp_border_spacing(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    uint32_t n;
    AuCssValue *it = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 2)
        return NULL;
    unsigned f = P_NONNEG | P_QUIRK | (cx->quirks ? P_QUIRKS_ON : 0);
    for (uint32_t i = 0; i < n; i++)
        if (!norm_lp(&it[i], f))
            return NULL;
    if (n == 1) {
        AuCssValue *l = css_new_list(cx->arena, AU_CSSV_LIST, 2);
        l->u.items[0] = *v;
        l->u.items[1] = *v;
        return l;
    }
    return v;
}

/* ---- Images ---- */

static bool color_stop_list(CssParseCtx *cx, AuCssValue *fn, uint32_t first, bool angular)
{
    (void)cx;
    uint32_t ncolors = 0;
    for (uint32_t i = first; i < fn->n; i++) {
        AuCssValue *it = &fn->u.items[i];
        uint32_t n;
        AuCssValue *p = (AuCssValue *)css_items(it, &n);
        uint32_t ncol = 0, npos = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (norm_color(&p[k])) {
                ncol++;
                continue;
            }
            if (angular ? (is_angle(&p[k]) || p[k].type == AU_CSSV_PERCENT) : norm_lp(&p[k], P_PCT)) {
                npos++;
                continue;
            }
            return false;
        }
        if (ncol > 1 || npos > 2 || (ncol == 0 && npos != 1))
            return false;
        ncolors += ncol;
    }
    return ncolors >= 1;
}

static bool position_kws(const AuCssValue *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        AuCssValue c = p[i];
        if (!KW_IN(&c, AU_K_left, AU_K_right, AU_K_top, AU_K_bottom, AU_K_center) && !norm_lp(&c, P_PCT))
            return false;
    }
    return n >= 1 && n <= 4;
}

static bool valid_image(CssParseCtx *cx, AuCssValue *v);

static bool valid_gradient(CssParseCtx *cx, AuCssValue *fn)
{
    if (fn->n < 1)
        return false;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(&fn->u.items[0], &n);
    switch (fn->kw) {
    case AU_K_linear_gradient:
    case AU_K_repeating_linear_gradient:
    case AU_K__webkit_linear_gradient:
    case AU_K__webkit_repeating_linear_gradient: {
        bool legacy = fn->kw == AU_K__webkit_linear_gradient || fn->kw == AU_K__webkit_repeating_linear_gradient;
        bool has_dir = false;
        if (n == 1 && is_angle(&p[0]))
            has_dir = true;
        else if (!legacy && n >= 2 && css_is_kw(&p[0], AU_K_to)) {
            for (uint32_t i = 1; i < n; i++)
                if (!KW_IN(&p[i], AU_K_left, AU_K_right, AU_K_top, AU_K_bottom))
                    return false;
            has_dir = n <= 3;
            if (!has_dir)
                return false;
        } else if (legacy && n >= 1 && KW_IN(&p[0], AU_K_left, AU_K_right, AU_K_top, AU_K_bottom)) {
            has_dir = true;
        }
        return color_stop_list(cx, fn, has_dir ? 1 : 0, false);
    }
    case AU_K_radial_gradient:
    case AU_K_repeating_radial_gradient:
    case AU_K__webkit_radial_gradient:
    case AU_K__webkit_repeating_radial_gradient: {
        /* [shape || size] [at position]? is accepted when the first
         * argument has no color in it */
        bool has_cfg = false;
        AuColor c;
        bool cur;
        bool any_color = false;
        for (uint32_t i = 0; i < n; i++)
            if (css_value_to_color(&p[i], &c, &cur))
                any_color = true;
        if (!any_color) {
            for (uint32_t i = 0; i < n; i++) {
                AuCssValue t = p[i];
                if (KW_IN(&t, AU_K_circle, AU_K_ellipse, AU_K_closest_side, AU_K_closest_corner,
                          AU_K_farthest_side, AU_K_farthest_corner, AU_K_at, AU_K_left, AU_K_right, AU_K_top,
                          AU_K_bottom, AU_K_center, AU_K_contain, AU_K_cover))
                    continue;
                if (norm_lp(&p[i], P_PCT))
                    continue;
                return false;
            }
            has_cfg = true;
        }
        return color_stop_list(cx, fn, has_cfg ? 1 : 0, false);
    }
    case AU_K_conic_gradient:
    case AU_K_repeating_conic_gradient: {
        bool has_cfg = false;
        if (n >= 1 && (css_is_kw(&p[0], AU_K_from) || css_is_kw(&p[0], AU_K_at))) {
            for (uint32_t i = 0; i < n; i++) {
                if (css_is_kw(&p[i], AU_K_from) || css_is_kw(&p[i], AU_K_at) || is_angle(&p[i]))
                    continue;
                if (KW_IN(&p[i], AU_K_left, AU_K_right, AU_K_top, AU_K_bottom, AU_K_center) || norm_lp(&p[i], P_PCT))
                    continue;
                return false;
            }
            has_cfg = true;
        }
        return color_stop_list(cx, fn, has_cfg ? 1 : 0, true);
    }
    default:
        return false;
    }
    (void)position_kws;
}

static bool valid_image(CssParseCtx *cx, AuCssValue *v)
{
    if (v->type == AU_CSSV_URL)
        return true;
    if (v->type != AU_CSSV_FUNCTION)
        return false;
    switch (v->kw) {
    case AU_K_image_set:
    case AU_K__webkit_image_set:
        for (uint32_t i = 0; i < v->n; i++) {
            uint32_t n;
            AuCssValue *p = (AuCssValue *)css_items(&v->u.items[i], &n);
            if (n < 1 || (p[0].type != AU_CSSV_URL && p[0].type != AU_CSSV_STRING && !valid_image(cx, &p[0])))
                return false;
            for (uint32_t k = 1; k < n; k++)
                if (!is_dim_type(&p[k], CT_RES, true) && !(p[k].type == AU_CSSV_FUNCTION && p[k].kw == AU_K_type))
                    return false;
        }
        return v->n >= 1;
    case AU_K_cross_fade:
    case AU_K_image:
    case AU_K_element:
        return true;
    default:
        return valid_gradient(cx, v);
    }
}

AuCssValue *pp_image_or_none(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    return valid_image(cx, v) ? v : NULL;
}

static bool bg_image_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    return css_is_kw(it, AU_K_none) || valid_image(cx, it);
}

AuCssValue *pp_bg_image(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_image_item); }

/* ---- Cursor ---- */

AuCssValue *pp_cursor(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    AuCssValue *l = as_list(cx, v, AU_CSSV_COMMA_LIST);
    for (uint32_t i = 0; i + 1 < l->n; i++) {
        uint32_t n;
        AuCssValue *p = (AuCssValue *)css_items(&l->u.items[i], &n);
        if (n != 1 && n != 3)
            return NULL;
        if (!valid_image(cx, &p[0]))
            return NULL;
        if (n == 3 && (!is_number(&p[1], false) || !is_number(&p[2], false)))
            return NULL;
    }
    const AuCssValue *last = &l->u.items[l->n - 1];
    if (last->type != AU_CSSV_KEYWORD || css_prop_kw_index(prop, last->kw) < 0)
        return NULL;
    return v;
}

/* ---- Display ---- */

/* Maps a display value (keyword or two/three keywords) to its code. */
bool css_display_code(const AuCssValue *v, uint16_t *out);
bool css_display_code(const AuCssValue *v, uint16_t *out)
{
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 3)
        return false;
    if (n == 1) {
        if (p[0].type != AU_CSSV_KEYWORD)
            return false;
        switch (p[0].kw) {
        case AU_K_none: *out = AU_DISPLAY_NONE; return true;
        case AU_K_contents: *out = AU_DISPLAY_CONTENTS; return true;
        case AU_K_block: *out = AU_DISPLAY_BLOCK; return true;
        case AU_K_inline_: *out = AU_DISPLAY_INLINE; return true;
        case AU_K_run_in: *out = AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_RUN_IN, AU_DISPLAY_INNER_FLOW, 0); return true;
        case AU_K_flow: *out = AU_DISPLAY_BLOCK; return true;
        case AU_K_flow_root: *out = AU_DISPLAY_FLOW_ROOT; return true;
        case AU_K_table: *out = AU_DISPLAY_TABLE; return true;
        case AU_K_flex: case AU_K__webkit_flex: *out = AU_DISPLAY_FLEX; return true;
        case AU_K_grid: *out = AU_DISPLAY_GRID; return true;
        case AU_K_ruby: *out = AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_RUBY, 0); return true;
        case AU_K_list_item: *out = AU_DISPLAY_LIST_ITEM; return true;
        case AU_K_inline_block: *out = AU_DISPLAY_INLINE_BLOCK; return true;
        case AU_K_inline_table: *out = AU_DISPLAY_INLINE_TABLE; return true;
        case AU_K_inline_flex: case AU_K__webkit_inline_flex: *out = AU_DISPLAY_INLINE_FLEX; return true;
        case AU_K_inline_grid: *out = AU_DISPLAY_INLINE_GRID; return true;
        case AU_K_inline_list_item:
            *out = AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_FLOW, 1);
            return true;
        case AU_K__webkit_box: case AU_K__moz_box:
            *out = AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, AU_DISPLAY_INNER_WEBKIT_BOX, 0);
            return true;
        case AU_K__webkit_inline_box: case AU_K__moz_inline_box:
            *out = AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_WEBKIT_BOX, 0);
            return true;
#define INT_(k, inner) case AU_K_##k: *out = AU_DISPLAY_MAKE(0, AU_DISPLAY_INNER_##inner, 0); return true;
        INT_(table_row_group, TABLE_ROW_GROUP)
        INT_(table_header_group, TABLE_HEADER_GROUP)
        INT_(table_footer_group, TABLE_FOOTER_GROUP)
        INT_(table_row, TABLE_ROW)
        INT_(table_cell, TABLE_CELL)
        INT_(table_column_group, TABLE_COLUMN_GROUP)
        INT_(table_column, TABLE_COLUMN)
        INT_(table_caption, TABLE_CAPTION)
        INT_(ruby_base, RUBY_BASE)
        INT_(ruby_text, RUBY_TEXT)
        INT_(ruby_base_container, RUBY_BASE_CONTAINER)
        INT_(ruby_text_container, RUBY_TEXT_CONTAINER)
#undef INT_
        default: return false;
        }
    }
    /* multi-keyword: [outer || inner] or [outer? flow|flow-root && list-item] */
    int outer = -1, inner = -1;
    bool li = false;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i].type != AU_CSSV_KEYWORD)
            return false;
        int o = -1, in = -1;
        switch (p[i].kw) {
        case AU_K_block: o = AU_DISPLAY_OUTER_BLOCK; break;
        case AU_K_inline_: o = AU_DISPLAY_OUTER_INLINE; break;
        case AU_K_run_in: o = AU_DISPLAY_OUTER_RUN_IN; break;
        case AU_K_flow: in = AU_DISPLAY_INNER_FLOW; break;
        case AU_K_flow_root: in = AU_DISPLAY_INNER_FLOW_ROOT; break;
        case AU_K_table: in = AU_DISPLAY_INNER_TABLE; break;
        case AU_K_flex: in = AU_DISPLAY_INNER_FLEX; break;
        case AU_K_grid: in = AU_DISPLAY_INNER_GRID; break;
        case AU_K_ruby: in = AU_DISPLAY_INNER_RUBY; break;
        case AU_K_list_item:
            if (li)
                return false;
            li = true;
            continue;
        default: return false;
        }
        if (o >= 0) {
            if (outer >= 0)
                return false;
            outer = o;
        } else {
            if (inner >= 0)
                return false;
            inner = in;
        }
    }
    if (li && inner >= 0 && inner != AU_DISPLAY_INNER_FLOW && inner != AU_DISPLAY_INNER_FLOW_ROOT)
        return false;
    if (outer < 0)
        outer = inner == AU_DISPLAY_INNER_RUBY ? AU_DISPLAY_OUTER_INLINE : AU_DISPLAY_OUTER_BLOCK;
    if (inner < 0)
        inner = AU_DISPLAY_INNER_FLOW;
    *out = AU_DISPLAY_MAKE(outer, inner, li);
    return true;
}

AuCssValue *pp_display(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    uint16_t d;
    return css_display_code(v, &d) ? v : NULL;
}

/* ---- Box ---- */

AuCssValue *pp_vertical_align(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    if (KW_IN(v, AU_K_baseline, AU_K_sub, AU_K_super, AU_K_text_top, AU_K_text_bottom, AU_K_middle, AU_K_top,
              AU_K_bottom))
        return v;
    return norm_lp(v, P_PCT | (cx->quirks ? P_QUIRKS_ON | P_QUIRK : 0)) ? v : NULL;
    (void)prop;
}

AuCssValue *pp_aspect_ratio(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    uint32_t i = 0;
    bool aut = false, ratio = false;
    while (i < n) {
        if (css_is_kw(&p[i], AU_K_auto_) && !aut) {
            aut = true;
            i++;
            continue;
        }
        if (!ratio && is_number(&p[i], true)) {
            ratio = true;
            i++;
            if (i + 1 < n + 1 && i < n && p[i].type == AU_CSSV_DELIM && p[i].unit == '/') {
                if (i + 1 >= n || !is_number(&p[i + 1], true))
                    return NULL;
                i += 2;
            }
            continue;
        }
        return NULL;
    }
    return (aut || ratio) ? v : NULL;
}

AuCssValue *pp_border_width(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    if (KW_IN(v, AU_K_thin, AU_K_medium, AU_K_thick))
        return v;
    return norm_lp(v, lp_flags(cx, prop) & ~(unsigned)P_PCT) ? v : NULL;
}

AuCssValue *pp_radius(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    uint32_t n;
    AuCssValue *it = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 2)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        if (!norm_lp(&it[i], P_PCT | P_NONNEG))
            return NULL;
    (void)cx;
    return v; /* one value stands for both radii */
}

/* ---- Background layers ---- */

/* One axis of a background position: center | edge [offset]? | offset. */
static bool bg_pos_axis(CssParseCtx *cx, AuCssValue *it, bool horiz)
{
    (void)cx;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(it, &n);
    if (it->type == AU_CSSV_COMMA_LIST)
        return false;
    if (n == 1) {
        if (css_is_kw(&p[0], AU_K_center))
            return true;
        if (horiz ? KW_IN(&p[0], AU_K_left, AU_K_right, AU_K_x_start, AU_K_x_end)
                  : KW_IN(&p[0], AU_K_top, AU_K_bottom, AU_K_y_start, AU_K_y_end))
            return true;
        return norm_lp(&p[0], P_PCT);
    }
    if (n == 2)
        return (horiz ? KW_IN(&p[0], AU_K_left, AU_K_right, AU_K_x_start, AU_K_x_end)
                      : KW_IN(&p[0], AU_K_top, AU_K_bottom, AU_K_y_start, AU_K_y_end)) &&
               norm_lp(&p[1], P_PCT);
    return false;
}

static bool bg_pos_x_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    return bg_pos_axis(cx, it, true);
}

static bool bg_pos_y_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    return bg_pos_axis(cx, it, false);
}

AuCssValue *pp_bg_pos_x(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_pos_x_item); }
AuCssValue *pp_bg_pos_y(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_pos_y_item); }

static bool bg_size_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    if (css_is_kw(it, AU_K_cover) || css_is_kw(it, AU_K_contain))
        return true;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(it, &n);
    if (it->type == AU_CSSV_COMMA_LIST || n < 1 || n > 2)
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (!css_is_kw(&p[i], AU_K_auto_) && !norm_lp(&p[i], P_PCT | P_NONNEG))
            return false;
    if (n == 1) {
        AuCssValue *l = css_new_list(cx->arena, AU_CSSV_LIST, 2);
        l->u.items[0] = *it;
        l->u.items[1] = *css_new_kw(cx->arena, AU_K_auto_);
        *it = *l;
    }
    return true;
}

AuCssValue *pp_bg_size(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_size_item); }

static bool bg_repeat_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)prop;
    uint32_t n;
    const AuCssValue *p = css_items(it, &n);
    uint16_t x, y;
    if (it->type == AU_CSSV_COMMA_LIST || n < 1 || n > 2)
        return false;
    if (n == 1 && css_is_kw(&p[0], AU_K_repeat_x)) {
        x = AU_K_repeat;
        y = AU_K_no_repeat;
    } else if (n == 1 && css_is_kw(&p[0], AU_K_repeat_y)) {
        x = AU_K_no_repeat;
        y = AU_K_repeat;
    } else {
        for (uint32_t i = 0; i < n; i++)
            if (!KW_IN(&p[i], AU_K_repeat, AU_K_space, AU_K_round, AU_K_no_repeat))
                return false;
        x = p[0].kw;
        y = n == 2 ? p[1].kw : x;
    }
    AuCssValue *l = css_new_list(cx->arena, AU_CSSV_LIST, 2);
    l->u.items[0] = *css_new_kw(cx->arena, x);
    l->u.items[1] = *css_new_kw(cx->arena, y);
    *it = *l;
    return true;
}

AuCssValue *pp_bg_repeat(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_repeat_item); }

static bool bg_attach_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return KW_IN(it, AU_K_scroll, AU_K_fixed, AU_K_local);
}

static bool bg_clip_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return KW_IN(it, AU_K_border_box, AU_K_padding_box, AU_K_content_box, AU_K_text);
}

static bool bg_origin_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return KW_IN(it, AU_K_border_box, AU_K_padding_box, AU_K_content_box);
}

AuCssValue *pp_bg_attachment(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_attach_item); }
AuCssValue *pp_bg_clip(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_clip_item); }
AuCssValue *pp_bg_origin(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, bg_origin_item); }

/* ---- Alignment ---- */

/* Computes the code of an alignment value for prop; false if invalid. */
bool css_align_code(uint16_t prop, const AuCssValue *v, uint16_t *out);
bool css_align_code(uint16_t prop, const AuCssValue *v, uint16_t *out)
{
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 2)
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (p[i].type != AU_CSSV_KEYWORD)
            return false;
    bool content = prop == AU_PROP_justify_content || prop == AU_PROP_align_content;
    bool self = prop == AU_PROP_justify_self || prop == AU_PROP_align_self;
    bool justify = prop == AU_PROP_justify_content || prop == AU_PROP_justify_items || prop == AU_PROP_justify_self;
    bool items = prop == AU_PROP_justify_items || prop == AU_PROP_align_items;
    uint16_t a = p[0].kw, b = n == 2 ? p[1].kw : 0;
    if (n == 1) {
        switch (a) {
        case AU_K_auto_: if (!self) return false; *out = AU_ALIGN_AUTO; return true;
        case AU_K_normal: *out = AU_ALIGN_NORMAL; return true;
        case AU_K_stretch: *out = AU_ALIGN_STRETCH; return true;
        case AU_K_baseline: *out = AU_ALIGN_BASELINE; return true;
        case AU_K_legacy: if (prop != AU_PROP_justify_items) return false; *out = AU_ALIGN_LEGACY_ONLY; return true;
        case AU_K_space_between: if (!content) return false; *out = AU_ALIGN_SPACE_BETWEEN; return true;
        case AU_K_space_around: if (!content) return false; *out = AU_ALIGN_SPACE_AROUND; return true;
        case AU_K_space_evenly: if (!content) return false; *out = AU_ALIGN_SPACE_EVENLY; return true;
        case AU_K_center: *out = AU_ALIGN_CENTER; return true;
        case AU_K_start: *out = AU_ALIGN_START; return true;
        case AU_K_end: *out = AU_ALIGN_END; return true;
        case AU_K_flex_start: *out = AU_ALIGN_FLEX_START; return true;
        case AU_K_flex_end: *out = AU_ALIGN_FLEX_END; return true;
        case AU_K_self_start: if (content) return false; *out = AU_ALIGN_SELF_START; return true;
        case AU_K_self_end: if (content) return false; *out = AU_ALIGN_SELF_END; return true;
        case AU_K_left: if (!justify) return false; *out = AU_ALIGN_LEFT; return true;
        case AU_K_right: if (!justify) return false; *out = AU_ALIGN_RIGHT; return true;
        default: return false;
        }
    }
    if (a == AU_K_first && b == AU_K_baseline) {
        *out = AU_ALIGN_BASELINE;
        return true;
    }
    if (a == AU_K_last && b == AU_K_baseline) {
        *out = AU_ALIGN_LAST_BASELINE;
        return true;
    }
    if (prop == AU_PROP_justify_items && (a == AU_K_legacy || b == AU_K_legacy)) {
        uint16_t o = a == AU_K_legacy ? b : a;
        uint16_t c = o == AU_K_left ? AU_ALIGN_LEFT : o == AU_K_right ? AU_ALIGN_RIGHT
                     : o == AU_K_center ? AU_ALIGN_CENTER : 0;
        if (!c)
            return false;
        *out = (uint16_t)(c | AU_ALIGN_LEGACY);
        return true;
    }
    if (a == AU_K_safe || a == AU_K_unsafe) {
        uint16_t c;
        if (!css_align_code(prop, &p[1], &c))
            return false;
        if (c != AU_ALIGN_CENTER && c != AU_ALIGN_START && c != AU_ALIGN_END && c != AU_ALIGN_FLEX_START &&
            c != AU_ALIGN_FLEX_END && c != AU_ALIGN_SELF_START && c != AU_ALIGN_SELF_END && c != AU_ALIGN_LEFT &&
            c != AU_ALIGN_RIGHT)
            return false;
        *out = (uint16_t)(c | (a == AU_K_safe ? AU_ALIGN_SAFE : AU_ALIGN_UNSAFE));
        return true;
    }
    (void)items;
    return false;
}

AuCssValue *pp_align(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    uint16_t c;
    return css_align_code(prop, v, &c) ? v : NULL;
}

/* ---- Grid ---- */

static bool line_names(const AuCssValue *v) { return v->type == AU_CSSV_FUNCTION && v->kw == 0 && v->unit == '['; }

/* <track-breadth> (inflexible if !flex), fixed only if fixed. */
static bool track_breadth(AuCssValue *v, bool flex)
{
    if (KW_IN(v, AU_K_min_content, AU_K_max_content, AU_K_auto_))
        return true;
    if (flex && v->type == AU_CSSV_DIMENSION && v->unit == AU_UNIT_FR)
        return v->u.num >= 0 || from_calc(v);
    return norm_lp(v, P_PCT | P_NONNEG);
}

static bool track_size(AuCssValue *v)
{
    if (track_breadth(v, true))
        return true;
    if (v->type == AU_CSSV_FUNCTION && v->kw == AU_K_minmax && v->n == 2)
        return track_breadth(&v->u.items[0], false) && track_breadth(&v->u.items[1], true) &&
               !(v->u.items[0].type == AU_CSSV_DIMENSION && v->u.items[0].unit == AU_UNIT_FR);
    if (v->type == AU_CSSV_FUNCTION && v->kw == AU_K_fit_content && v->n == 1)
        return norm_lp(&v->u.items[0], P_PCT | P_NONNEG);
    return false;
}

static bool track_seq(AuCssValue *items, uint32_t n, bool in_repeat, bool *auto_repeat)
{
    uint32_t ntracks = 0;
    for (uint32_t i = 0; i < n; i++) {
        AuCssValue *it = &items[i];
        if (line_names(it))
            continue;
        if (it->type == AU_CSSV_FUNCTION && it->kw == AU_K_repeat) {
            if (in_repeat || it->n != 2)
                return false;
            AuCssValue *cnt = &it->u.items[0];
            if (KW_IN(cnt, AU_K_auto_fill, AU_K_auto_fit)) {
                if (*auto_repeat)
                    return false;
                *auto_repeat = true;
            } else if (!(cnt->type == AU_CSSV_NUMBER && cnt->unit == 1 && cnt->u.num >= 1)) {
                return false;
            }
            uint32_t m;
            AuCssValue *sub = (AuCssValue *)css_items(&it->u.items[1], &m);
            bool dummy = false;
            if (!track_seq(sub, m, true, &dummy))
                return false;
            ntracks++;
            continue;
        }
        if (!track_size(it))
            return false;
        ntracks++;
    }
    return ntracks > 0;
}

AuCssValue *pp_track_list(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    AuCssValue *it = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    if (n >= 1 && css_is_kw(&it[0], AU_K_subgrid)) {
        for (uint32_t i = 1; i < n; i++)
            if (!line_names(&it[i]))
                return NULL;
        return v;
    }
    bool ar = false;
    return track_seq(it, n, false, &ar) ? v : NULL;
}

AuCssValue *pp_track_sizes(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    uint32_t n;
    AuCssValue *it = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        if (!track_size(&it[i]))
            return NULL;
    return v;
}

/* Validates grid-template-areas strings: each row has the same number of
 * cells and every named area is a rectangle. */
bool css_grid_areas_valid(const AuCssValue *v);
bool css_grid_areas_valid(const AuCssValue *v)
{
    uint32_t n;
    const AuCssValue *rows = css_items(v, &n);
    if (n == 0 || n > 1000)
        return false;
    /* tokenized cells: names as (start, len) in each row */
    uint32_t cols = 0;
    typedef struct { const char *s; uint32_t len; } Cell;
    Cell *cells = NULL;
    uint32_t ncells = 0, cap = 0;
    bool ok = true;
    for (uint32_t r = 0; r < n && ok; r++) {
        if (rows[r].type != AU_CSSV_STRING) {
            ok = false;
            break;
        }
        const char *s = rows[r].u.str;
        uint32_t len = rows[r].n, i = 0, c = 0;
        while (i < len) {
            unsigned char ch = (unsigned char)s[i];
            if (au_is_space(ch)) {
                i++;
                continue;
            }
            uint32_t st = i;
            if (ch == '.') {
                while (i < len && s[i] == '.')
                    i++;
                Cell *ce = CSS_PUSH(cells, ncells, cap);
                ce->s = ".";
                ce->len = 1;
            } else if (au_is_alpha(ch) || au_is_digit(ch) || ch == '-' || ch == '_' || ch >= 0x80) {
                while (i < len && !au_is_space((unsigned char)s[i]) && s[i] != '.')
                    i++;
                Cell *ce = CSS_PUSH(cells, ncells, cap);
                ce->s = s + st;
                ce->len = i - st;
            } else {
                ok = false;
                break;
            }
            c++;
        }
        if (r == 0)
            cols = c;
        else if (c != cols)
            ok = false;
        if (c == 0)
            ok = false;
    }
    /* rectangles: for each name, its cells form a full rectangle */
    for (uint32_t k = 0; ok && k < ncells; k++) {
        if (cells[k].s[0] == '.')
            continue;
        uint32_t r0 = n, r1 = 0, c0 = cols, c1 = 0, count = 0;
        bool seen_before = false;
        for (uint32_t j = 0; j < ncells; j++) {
            if (cells[j].len == cells[k].len && memcmp(cells[j].s, cells[k].s, cells[k].len) == 0) {
                if (j < k)
                    seen_before = true;
                uint32_t r = j / cols, c = j % cols;
                if (r < r0) r0 = r;
                if (r > r1) r1 = r;
                if (c < c0) c0 = c;
                if (c > c1) c1 = c;
                count++;
            }
        }
        if (seen_before)
            continue;
        if (count != (r1 - r0 + 1) * (c1 - c0 + 1))
            ok = false;
    }
    au_free(cells);
    return ok;
}

AuCssValue *pp_grid_areas(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    return css_grid_areas_valid(v) ? v : NULL;
}

AuCssValue *pp_grid_auto_flow(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    uint32_t n;
    const AuCssValue *it = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 2)
        return NULL;
    bool dir = false, dense = false;
    for (uint32_t i = 0; i < n; i++) {
        if (KW_IN(&it[i], AU_K_row, AU_K_column) && !dir)
            dir = true;
        else if (css_is_kw(&it[i], AU_K_dense) && !dense)
            dense = true;
        else
            return NULL;
    }
    return v;
}

AuCssValue *pp_grid_line(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_auto_))
        return v;
    uint32_t n;
    const AuCssValue *it = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 3)
        return NULL;
    bool span = false, num = false, name = false;
    for (uint32_t i = 0; i < n; i++) {
        if (css_is_kw(&it[i], AU_K_span) && !span) {
            if (i != 0 && i != n - 1)
                return NULL;
            span = true;
        } else if (it[i].type == AU_CSSV_NUMBER && it[i].unit == 1 && !num) {
            if (it[i].u.num == 0)
                return NULL;
            num = true;
        } else if (is_custom_ident(&it[i]) && !css_is_kw(&it[i], AU_K_auto_) && !css_is_kw(&it[i], AU_K_span) &&
                   !name) {
            name = true;
        } else {
            return NULL;
        }
    }
    if (span && !num && !name)
        return NULL;
    if (span && num) {
        for (uint32_t i = 0; i < n; i++)
            if (it[i].type == AU_CSSV_NUMBER && it[i].u.num < 0)
                return NULL;
    }
    return v;
}

/* ---- Transforms ---- */

static bool lp_or(AuCssValue *v, bool pct) { return norm_lp(v, pct ? P_PCT : 0); }

static bool num_or_pct(const AuCssValue *v)
{
    return is_number(v, false) || v->type == AU_CSSV_PERCENT || (v->type == AU_CSSV_CALC && calc_type_ok(v, CT_PERCENT));
}

static bool transform_fn(AuCssValue *f)
{
    if (f->type != AU_CSSV_FUNCTION)
        return false;
    AuCssValue *a = f->u.items;
    uint32_t n = f->n;
    /* every argument must be a single value */
    for (uint32_t i = 0; i < n; i++)
        if (a[i].type == AU_CSSV_LIST)
            return false;
    switch (f->kw) {
    case AU_K_matrix:
        if (n != 6) return false;
        for (uint32_t i = 0; i < n; i++) if (!is_number(&a[i], false)) return false;
        return true;
    case AU_K_matrix3d:
        if (n != 16) return false;
        for (uint32_t i = 0; i < n; i++) if (!is_number(&a[i], false)) return false;
        return true;
    case AU_K_translate:
        return (n == 1 || n == 2) && lp_or(&a[0], true) && (n == 1 || lp_or(&a[1], true));
    case AU_K_translatex:
    case AU_K_translatey:
        return n == 1 && lp_or(&a[0], true);
    case AU_K_translatez:
        return n == 1 && lp_or(&a[0], false);
    case AU_K_translate3d:
        return n == 3 && lp_or(&a[0], true) && lp_or(&a[1], true) && lp_or(&a[2], false);
    case AU_K_scale:
        return (n == 1 || n == 2) && num_or_pct(&a[0]) && (n == 1 || num_or_pct(&a[1]));
    case AU_K_scalex:
    case AU_K_scaley:
    case AU_K_scalez:
        return n == 1 && num_or_pct(&a[0]);
    case AU_K_scale3d:
        return n == 3 && num_or_pct(&a[0]) && num_or_pct(&a[1]) && num_or_pct(&a[2]);
    case AU_K_rotate:
    case AU_K_rotatex:
    case AU_K_rotatey:
    case AU_K_rotatez:
        return n == 1 && is_angle(&a[0]);
    case AU_K_rotate3d:
        return n == 4 && is_number(&a[0], false) && is_number(&a[1], false) && is_number(&a[2], false) &&
               is_angle(&a[3]);
    case AU_K_skew:
        return (n == 1 || n == 2) && is_angle(&a[0]) && (n == 1 || is_angle(&a[1]));
    case AU_K_skewx:
    case AU_K_skewy:
        return n == 1 && is_angle(&a[0]);
    case AU_K_perspective:
        return n == 1 && (css_is_kw(&a[0], AU_K_none) || norm_lp(&a[0], P_NONNEG));
    default:
        return false;
    }
}

AuCssValue *pp_transform(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    AuCssValue *it = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        if (!transform_fn(&it[i]))
            return NULL;
    return v;
}

/* Normalizes a 1-3 value position with keywords into [x y] (and z):
 * keywords become percentages, edge offsets become calc(). */
static AuCssValue *pct(CssArena *a, float v) { return css_new_num(a, AU_CSSV_PERCENT, v, 0); }

static AuCssValue *edge_offset(CssArena *a, uint16_t kw, const AuCssValue *off)
{
    bool far = kw == AU_K_right || kw == AU_K_bottom;
    if (!off)
        return pct(a, kw == AU_K_center ? 50 : far ? 100 : 0);
    if (!far)
        return css_value_copy(a, off);
    /* calc(100% - off) */
    AuCssValue *s = css_new_list(a, AU_CSSV_CALC, 2);
    s->unit = AU_CALC_SUM;
    s->u.items[0] = *pct(a, 100);
    AuCssValue *ng = css_new_list(a, AU_CSSV_CALC, 1);
    ng->unit = AU_CALC_NEGATE;
    ng->u.items[0] = *off;
    s->u.items[1] = *ng;
    css_calc_simplify(a, s);
    return s;
}

static bool is_hkw(const AuCssValue *v) { return KW_IN(v, AU_K_left, AU_K_right); }
static bool is_vkw(const AuCssValue *v) { return KW_IN(v, AU_K_top, AU_K_bottom); }

/* <position> (1 to 4 values) into a LIST of two. */
AuCssValue *css_parse_position(CssParseCtx *cx, AuCssValue *v);
AuCssValue *css_parse_position(CssParseCtx *cx, AuCssValue *v)
{
    CssArena *a = cx->arena;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 4)
        return NULL;
    AuCssValue *x = NULL, *y = NULL;
    if (n == 1) {
        if (css_is_kw(&p[0], AU_K_center)) {
            x = pct(a, 50);
            y = pct(a, 50);
        } else if (is_hkw(&p[0])) {
            x = edge_offset(a, p[0].kw, NULL);
            y = pct(a, 50);
        } else if (is_vkw(&p[0])) {
            x = pct(a, 50);
            y = edge_offset(a, p[0].kw, NULL);
        } else if (norm_lp(&p[0], P_PCT)) {
            x = &p[0];
            y = pct(a, 50);
        } else {
            return NULL;
        }
    } else if (n == 2) {
        AuCssValue *A = &p[0], *B = &p[1];
        bool al = norm_lp(A, P_PCT), bl = norm_lp(B, P_PCT);
        if ((al || is_hkw(A) || css_is_kw(A, AU_K_center)) && (bl || is_vkw(B) || css_is_kw(B, AU_K_center))) {
            x = al ? A : edge_offset(a, A->kw, NULL);
            y = bl ? B : edge_offset(a, B->kw, NULL);
        } else if ((is_vkw(A) || css_is_kw(A, AU_K_center)) && (is_hkw(B) || css_is_kw(B, AU_K_center))) {
            x = edge_offset(a, B->kw, NULL);
            y = edge_offset(a, A->kw, NULL);
        } else {
            return NULL;
        }
    } else {
        /* 3 or 4 values: keyword offset pairs */
        uint16_t hk = 0, vk = 0;
        AuCssValue *ho = NULL, *vo = NULL;
        uint32_t i = 0;
        while (i < n) {
            if (p[i].type != AU_CSSV_KEYWORD)
                return NULL;
            uint16_t k = p[i].kw;
            AuCssValue *off = NULL;
            if (i + 1 < n && p[i + 1].type != AU_CSSV_KEYWORD) {
                if (k == AU_K_center || !norm_lp(&p[i + 1], P_PCT))
                    return NULL;
                off = &p[i + 1];
            }
            if (is_hkw(&p[i]) && !hk) {
                hk = k;
                ho = off;
            } else if (is_vkw(&p[i]) && !vk) {
                vk = k;
                vo = off;
            } else if (k == AU_K_center && !hk) {
                hk = k;
            } else if (k == AU_K_center && !vk) {
                vk = k;
            } else {
                return NULL;
            }
            i += off ? 2 : 1;
        }
        if (!hk || !vk)
            return NULL;
        x = edge_offset(a, hk, ho);
        y = edge_offset(a, vk, vo);
    }
    AuCssValue *l = css_new_list(a, AU_CSSV_LIST, 2);
    l->u.items[0] = *x;
    l->u.items[1] = *y;
    return l;
}

AuCssValue *pp_position(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    return css_parse_position(cx, v);
}

AuCssValue *pp_transform_origin(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n < 1 || n > 3)
        return NULL;
    AuCssValue *z = NULL;
    AuCssValue *xy = NULL;
    if (n == 3) {
        AuCssValue zz = p[2];
        if (!norm_lp(&zz, 0))
            return NULL;
        AuCssValue *two = css_new_list(cx->arena, AU_CSSV_LIST, 2);
        two->u.items[0] = p[0];
        two->u.items[1] = p[1];
        xy = css_parse_position(cx, two);
        z = css_new_value(cx->arena, 0);
        *z = zz;
    } else {
        xy = css_parse_position(cx, v);
    }
    if (!xy)
        return NULL;
    AuCssValue *l = css_new_list(cx->arena, AU_CSSV_LIST, 3);
    l->u.items[0] = xy->u.items[0];
    l->u.items[1] = xy->u.items[1];
    l->u.items[2] = z ? *z : *css_new_num(cx->arena, AU_CSSV_DIMENSION, 0, AU_UNIT_PX);
    return l;
}

AuCssValue *pp_translate(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n > 3)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        if (!norm_lp(&p[i], i < 2 ? P_PCT : 0))
            return NULL;
    return v;
}

AuCssValue *pp_rotate(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    if (n == 1)
        return is_angle(&p[0]) ? v : NULL;
    if (n == 2)
        return ((KW_IN(&p[0], AU_K_x, AU_K_y, AU_K_z) && is_angle(&p[1])) ||
                (KW_IN(&p[1], AU_K_x, AU_K_y, AU_K_z) && is_angle(&p[0]))) ? v : NULL;
    if (n == 4)
        return is_number(&p[0], false) && is_number(&p[1], false) && is_number(&p[2], false) && is_angle(&p[3])
                   ? v : NULL;
    return NULL;
}

AuCssValue *pp_scale(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n > 3)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        if (!num_or_pct(&p[i]))
            return NULL;
    return v;
}

/* ---- Transitions and animations ---- */

static bool transition_property_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return css_is_kw(it, AU_K_all) || is_custom_ident(it);
}

AuCssValue *pp_transition_property(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    if (css_is_kw(v, AU_K_none))
        return v;
    return each_item(cx, prop, v, transition_property_item);
}

static bool time_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    return is_time(it, (css_props[prop].flags & F_NONNEG) != 0) ||
           (prop == AU_PROP_animation_duration && css_is_kw(it, AU_K_auto_));
}

AuCssValue *pp_time_list(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, time_item); }

bool css_is_easing(const AuCssValue *it);
bool css_is_easing(const AuCssValue *it)
{
    if (KW_IN(it, AU_K_linear, AU_K_ease, AU_K_ease_in, AU_K_ease_out, AU_K_ease_in_out, AU_K_step_start,
              AU_K_step_end))
        return true;
    if (it->type != AU_CSSV_FUNCTION)
        return false;
    if (it->kw == AU_K_cubic_bezier) {
        if (it->n != 4)
            return false;
        for (uint32_t i = 0; i < 4; i++)
            if (!is_number(&it->u.items[i], false))
                return false;
        for (uint32_t i = 0; i < 4; i += 2) {
            const AuCssValue *x = &it->u.items[i];
            if (x->type == AU_CSSV_NUMBER && (x->u.num < 0 || x->u.num > 1))
                return false;
        }
        return true;
    }
    if (it->kw == AU_K_steps) {
        if (it->n < 1 || it->n > 2 || !is_integer(&it->u.items[0]))
            return false;
        if (it->u.items[0].type == AU_CSSV_NUMBER && it->u.items[0].u.num < 1)
            return false;
        if (it->n == 2 && !KW_IN(&it->u.items[1], AU_K_jump_start, AU_K_jump_end, AU_K_jump_none, AU_K_jump_both,
                                 AU_K_start, AU_K_end))
            return false;
        if (it->n == 2 && css_is_kw(&it->u.items[1], AU_K_jump_none) && it->u.items[0].type == AU_CSSV_NUMBER &&
            it->u.items[0].u.num < 2)
            return false;
        return true;
    }
    if (it->kw == AU_K_linear) {
        for (uint32_t i = 0; i < it->n; i++) {
            uint32_t n;
            const AuCssValue *p = css_items(&it->u.items[i], &n);
            if (n < 1 || n > 3 || !is_number(&p[0], false))
                return false;
            for (uint32_t k = 1; k < n; k++)
                if (p[k].type != AU_CSSV_PERCENT)
                    return false;
        }
        return it->n >= 1;
    }
    return false;
}

static bool easing_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return css_is_easing(it);
}

AuCssValue *pp_easing_list(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, easing_item); }

static bool behavior_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return KW_IN(it, AU_K_normal, AU_K_allow_discrete);
}

AuCssValue *pp_transition_behavior(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    return each_item(cx, prop, v, behavior_item);
}

static bool anim_name_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return css_is_kw(it, AU_K_none) || it->type == AU_CSSV_STRING || is_custom_ident(it);
}

AuCssValue *pp_animation_name(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, anim_name_item); }

static bool iter_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return css_is_kw(it, AU_K_infinite) || is_number(it, true);
}

AuCssValue *pp_iteration_count(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, iter_item); }

static bool dir_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return KW_IN(it, AU_K_normal, AU_K_reverse, AU_K_alternate, AU_K_alternate_reverse);
}

AuCssValue *pp_animation_direction(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, dir_item); }

static bool fill_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return KW_IN(it, AU_K_none, AU_K_forwards, AU_K_backwards, AU_K_both);
}

AuCssValue *pp_animation_fill_mode(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, fill_item); }

static bool play_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return KW_IN(it, AU_K_running, AU_K_paused);
}

AuCssValue *pp_animation_play_state(CssParseCtx *cx, uint16_t prop, AuCssValue *v) { return each_item(cx, prop, v, play_item); }

/* ---- Filters ---- */

static bool filter_fn(AuCssValue *f)
{
    if (f->type == AU_CSSV_URL)
        return true;
    if (f->type != AU_CSSV_FUNCTION)
        return false;
    AuCssValue *a = f->u.items;
    switch (f->kw) {
    case AU_K_blur:
        return f->n == 0 || (f->n == 1 && norm_lp(&a[0], P_NONNEG));
    case AU_K_brightness: case AU_K_contrast: case AU_K_grayscale: case AU_K_invert: case AU_K_opacity:
    case AU_K_saturate: case AU_K_sepia:
        return f->n == 0 || (f->n == 1 && (is_number(&a[0], true) || a[0].type == AU_CSSV_PERCENT));
    case AU_K_hue_rotate:
        return f->n == 0 || (f->n == 1 && is_angle(&a[0]));
    case AU_K_drop_shadow: {
        if (f->n != 1)
            return false;
        uint32_t n;
        AuCssValue *p = (AuCssValue *)css_items(&a[0], &n);
        int nl = 0, nc = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (nl < 3 && norm_lp(&p[i], nl == 2 ? P_NONNEG : 0))
                nl++;
            else if (!nc && norm_color(&p[i]))
                nc++;
            else
                return false;
        }
        return nl >= 2;
    }
    default:
        return false;
    }
}

AuCssValue *pp_filter(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        if (!filter_fn(&p[i]))
            return NULL;
    return v;
}

/* ---- Generated content ---- */

static bool counter_fn(const AuCssValue *f)
{
    if (f->type != AU_CSSV_FUNCTION)
        return false;
    if (f->kw == AU_K_counter)
        return (f->n == 1 || f->n == 2) && is_custom_ident(&f->u.items[0]) &&
               (f->n == 1 || f->u.items[1].type == AU_CSSV_KEYWORD || f->u.items[1].type == AU_CSSV_IDENT);
    if (f->kw == AU_K_counters)
        return (f->n == 2 || f->n == 3) && is_custom_ident(&f->u.items[0]) && f->u.items[1].type == AU_CSSV_STRING &&
               (f->n == 2 || f->u.items[2].type == AU_CSSV_KEYWORD || f->u.items[2].type == AU_CSSV_IDENT);
    return false;
}

static bool attr_fn(const AuCssValue *f)
{
    if (f->type != AU_CSSV_FUNCTION || f->kw != AU_K_attr || f->n < 1 || f->n > 2)
        return false;
    uint32_t n;
    const AuCssValue *p = css_items(&f->u.items[0], &n);
    return n >= 1 && n <= 2 && (p[0].type == AU_CSSV_IDENT || p[0].type == AU_CSSV_KEYWORD);
}

AuCssValue *pp_content(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    if (css_is_kw(v, AU_K_normal) || css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    AuCssValue *p = (AuCssValue *)css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    bool alt = false;
    uint32_t main_items = 0;
    for (uint32_t i = 0; i < n; i++) {
        AuCssValue *it = &p[i];
        if (it->type == AU_CSSV_DELIM && it->unit == '/') {
            if (alt || main_items == 0)
                return NULL;
            alt = true;
            continue;
        }
        if (it->type == AU_CSSV_STRING || counter_fn(it) || attr_fn(it)) {
            if (!alt)
                main_items++;
            continue;
        }
        if (!alt && (KW_IN(it, AU_K_open_quote, AU_K_close_quote, AU_K_no_open_quote, AU_K_no_close_quote) ||
                     valid_image(cx, it))) {
            main_items++;
            continue;
        }
        return NULL;
    }
    return main_items ? v : NULL;
}

static AuCssValue *counter_list(CssParseCtx *cx, AuCssValue *v, bool reset)
{
    (void)cx;
    if (css_is_kw(v, AU_K_none))
        return v;
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    for (uint32_t i = 0; i < n; i++) {
        bool name = is_custom_ident(&p[i]) && !css_is_kw(&p[i], AU_K_none);
        if (!name && reset && p[i].type == AU_CSSV_FUNCTION && p[i].kw == AU_K_reversed && p[i].n == 1 &&
            is_custom_ident(&p[i].u.items[0]))
            name = true;
        if (!name)
            return NULL;
        if (i + 1 < n && is_integer(&p[i + 1]))
            i++;
    }
    return v;
}

AuCssValue *pp_counter_reset(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    return counter_list(cx, v, prop == AU_PROP_counter_reset);
}

AuCssValue *pp_counter_increment(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)prop;
    return counter_list(cx, v, false);
}

/* ---- Misc ---- */

static bool will_change_item(CssParseCtx *cx, uint16_t prop, AuCssValue *it)
{
    (void)cx;
    (void)prop;
    return is_custom_ident(it) && !css_is_kw(it, AU_K_auto_) && !css_is_kw(it, AU_K_all);
}

AuCssValue *pp_will_change(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    if (css_is_kw(v, AU_K_auto_))
        return v;
    return each_item(cx, prop, v, will_change_item);
}

AuCssValue *pp_contain(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (KW_IN(v, AU_K_none, AU_K_strict, AU_K_content))
        return v;
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    unsigned seen = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned bit;
        if (css_is_kw(&p[i], AU_K_size)) bit = 1;
        else if (css_is_kw(&p[i], AU_K_inline_size)) bit = 2;
        else if (css_is_kw(&p[i], AU_K_layout)) bit = 4;
        else if (css_is_kw(&p[i], AU_K_style)) bit = 8;
        else if (css_is_kw(&p[i], AU_K_paint)) bit = 16;
        else return NULL;
        if ((seen & bit) || ((seen | bit) & 3) == 3)
            return NULL;
        seen |= bit;
    }
    return v;
}

AuCssValue *pp_text_decoration_line(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (KW_IN(v, AU_K_none, AU_K_spelling_error, AU_K_grammar_error))
        return v;
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST)
        return NULL;
    unsigned seen = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned bit;
        if (css_is_kw(&p[i], AU_K_underline)) bit = AU_TDL_UNDERLINE;
        else if (css_is_kw(&p[i], AU_K_overline)) bit = AU_TDL_OVERLINE;
        else if (css_is_kw(&p[i], AU_K_line_through)) bit = AU_TDL_LINE_THROUGH;
        else if (css_is_kw(&p[i], AU_K_blink)) bit = AU_TDL_BLINK;
        else return NULL;
        if (seen & bit)
            return NULL;
        seen |= bit;
    }
    return v;
}

AuCssValue *pp_clip_path(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_none) || v->type == AU_CSSV_URL)
        return v;
    uint32_t n;
    const AuCssValue *p = css_items(v, &n);
    if (v->type == AU_CSSV_COMMA_LIST || n > 2)
        return NULL;
    int shapes = 0, boxes = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i].type == AU_CSSV_FUNCTION &&
            (p[i].kw == AU_K_inset || p[i].kw == AU_K_circle || p[i].kw == AU_K_ellipse ||
             p[i].kw == AU_K_polygon || p[i].kw == AU_K_path || p[i].kw == AU_K_rect || p[i].kw == AU_K_xywh))
            shapes++;
        else if (KW_IN(&p[i], AU_K_border_box, AU_K_padding_box, AU_K_content_box, AU_K_margin_box, AU_K_fill_box,
                       AU_K_stroke_box, AU_K_view_box))
            boxes++;
        else
            return NULL;
    }
    return (shapes <= 1 && boxes <= 1) ? v : NULL;
}

AuCssValue *pp_clip_rect(CssParseCtx *cx, uint16_t prop, AuCssValue *v)
{
    (void)cx;
    (void)prop;
    if (css_is_kw(v, AU_K_auto_))
        return v;
    if (v->type != AU_CSSV_FUNCTION || v->kw != AU_K_rect)
        return NULL;
    AuCssValue *args = v->u.items;
    uint32_t n = v->n;
    if (n == 1) {
        /* rect(a b c d) */
        uint32_t m;
        args = (AuCssValue *)css_items(&v->u.items[0], &m);
        n = m;
    }
    if (n != 4)
        return NULL;
    for (uint32_t i = 0; i < 4; i++)
        if (!css_is_kw(&args[i], AU_K_auto_) && !norm_lp(&args[i], 0))
            return NULL;
    return v;
}
