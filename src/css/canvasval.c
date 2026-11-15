/* CSS values outside a style sheet: a colour and a font shorthand given as
 * text, for the canvas context's fillStyle, strokeStyle, shadowColor and
 * font attributes. They go through the same tokenizer, value parser and
 * shorthand code as declarations, so every syntax a sheet accepts is
 * accepted here. */
#include "css_int.h"

#include <math.h>
#include <string.h>

static const AuCssDecl *find_decl(const AuCssDeclBlock *b, uint16_t prop)
{
    for (uint32_t i = 0; i < b->n; i++)
        if (b->d[i].prop == prop)
            return &b->d[i];
    return NULL;
}

static const AuCssValue *decl_value(const AuCssDecl *d)
{
    if (!d)
        return NULL;
    if (d->kind == DK_VALUE)
        return &d->u.v;
    if (d->kind == DK_BLOB && d->u.blob)
        return &d->u.blob->value;
    return NULL;
}

bool au_css_parse_color_text(const char *s, size_t len, AuColor *out, bool *current)
{
    CssTokens tk;
    memset(&tk, 0, sizeof tk);
    css_tokenize(s, len, &tk);
    AuCssDeclBlock b;
    css_decl_block_init(&b);
    bool ok = false, cur = false;
    if (css_parse_property(AU_PROP_color, &tk, 0, tk.n, false, &b, false, NULL)) {
        const AuCssValue *v = decl_value(find_decl(&b, AU_PROP_color));
        ok = v && css_value_to_color(v, out, &cur);
    }
    if (current)
        *current = cur;
    css_decl_block_clear(&b);
    css_tokens_free(&tk);
    return ok;
}

/* A font-size value in pixels; base is the size em and percentages refer
 * to. False for values that need a context this has not got (calc with
 * relative parts, viewport units). */
static bool font_size_px(const AuCssValue *v, float base, float *px)
{
    static const float kw_px[8] = {9, 10, 13, 16, 18, 24, 32, 48};
    if (!v)
        return false;
    switch (v->type) {
    case AU_CSSV_KEYWORD:
        switch (v->kw) {
        case AU_K_xx_small: *px = kw_px[0]; return true;
        case AU_K_x_small: *px = kw_px[1]; return true;
        case AU_K_small: *px = kw_px[2]; return true;
        case AU_K_medium: *px = kw_px[3]; return true;
        case AU_K_large: *px = kw_px[4]; return true;
        case AU_K_x_large: *px = kw_px[5]; return true;
        case AU_K_xx_large: *px = kw_px[6]; return true;
        case AU_K_xxx_large: *px = kw_px[7]; return true;
        case AU_K_larger: *px = base * 1.2f; return true;
        case AU_K_smaller: *px = base / 1.2f; return true;
        default: return false;
        }
    case AU_CSSV_PERCENT:
        *px = base * v->u.num / 100;
        return true;
    case AU_CSSV_DIMENSION: {
        float f;
        int canon;
        if (css_unit_canonical(v->unit, &f, &canon)) {
            if (canon != AU_UNIT_PX)
                return false;
            *px = v->u.num * f;
            return true;
        }
        if (v->unit == AU_UNIT_EM || v->unit == AU_UNIT_REM) {
            *px = v->u.num * base;
            return true;
        }
        if (v->unit == AU_UNIT_EX || v->unit == AU_UNIT_CH) {
            *px = v->u.num * base * 0.5f;
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool au_css_parse_font_text(const char *s, size_t len, const AuStyle *base, AuCssCanvasFont *out)
{
    CssTokens tk;
    memset(&tk, 0, sizeof tk);
    css_tokenize(s, len, &tk);
    AuCssDeclBlock b;
    css_decl_block_init(&b);
    bool ok = false;
    float base_size = base ? base->inh->font_size : 10;
    float base_weight = base ? base->inh->font_weight : 400;
    /* The CSS-wide keywords are not font values here. */
    uint32_t a = 0, e = tk.n;
    while (a < e && tk.t[a].type == TK_WS)
        a++;
    while (e > a && (tk.t[e - 1].type == TK_WS || tk.t[e - 1].type == TK_EOF))
        e--;
    bool wide = e - a == 1 && tk.t[a].type == TK_IDENT &&
                (tk.t[a].kw == AU_K_inherit || tk.t[a].kw == AU_K_initial || tk.t[a].kw == AU_K_unset ||
                 tk.t[a].kw == AU_K_revert || tk.t[a].kw == AU_K_revert_layer);
    if (!wide && !css_tokens_have_var(&tk, a, e) &&
        css_parse_property(AU_PROP_font, &tk, a, e, false, &b, false, NULL)) {
        const AuCssValue *size = decl_value(find_decl(&b, AU_PROP_font_size));
        const AuCssValue *fam = decl_value(find_decl(&b, AU_PROP_font_family));
        const AuCssValue *weight = decl_value(find_decl(&b, AU_PROP_font_weight));
        const AuCssValue *style = decl_value(find_decl(&b, AU_PROP_font_style));
        const AuCssValue *caps = decl_value(find_decl(&b, AU_PROP_font_variant_caps));
        float px = 0, w = 400;
        if (weight) {
            if (weight->type == AU_CSSV_KEYWORD) {
                float pw = base_weight;
                switch (weight->kw) {
                case AU_K_bold: w = 700; break;
                case AU_K_bolder: w = pw < 350 ? 400 : pw < 550 ? 700 : pw < 900 ? 900 : pw; break;
                case AU_K_lighter: w = pw < 100 ? pw : pw < 550 ? 100 : pw < 750 ? 400 : 700; break;
                default: w = 400; break;
                }
            } else if (weight->type == AU_CSSV_NUMBER) {
                w = weight->u.num;
            }
        }
        if (fam && font_size_px(size, base_size, &px) && px >= 0 && isfinite(px) && w >= 1 && w <= 1000) {
            ok = true;
            out->size = px;
            out->weight = (int)(w + 0.5f);
            out->italic = style && style->type == AU_CSSV_KEYWORD &&
                          (style->kw == AU_K_italic || style->kw == AU_K_oblique);
            out->small_caps = caps && css_is_kw(caps, AU_K_small_caps);
            au_buf_clear(&out->families);
            au_css_value_serialize(fam, &out->families);
            /* The serialized shorthand: style, variant, weight, size, family. */
            au_buf_clear(&out->text);
            if (out->italic)
                au_buf_puts(&out->text, style->kw == AU_K_oblique ? "oblique " : "italic ");
            if (out->small_caps)
                au_buf_puts(&out->text, "small-caps ");
            if (out->weight == 700)
                au_buf_puts(&out->text, "bold ");
            else if (out->weight != 400)
                au_buf_printf(&out->text, "%d ", out->weight);
            if (px == floorf(px))
                au_buf_printf(&out->text, "%dpx ", (int)px);
            else
                au_buf_printf(&out->text, "%.6gpx ", (double)px);
            au_buf_append(&out->text, out->families.data, out->families.len);
        }
    }
    css_decl_block_clear(&b);
    css_tokens_free(&tk);
    return ok;
}
