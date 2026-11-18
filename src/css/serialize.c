/* Serialization (CSSOM): value trees, colors, declaration blocks, and
 * computed values for getComputedStyle. */
#include "css_int.h"

#include <math.h>
#include <stdio.h>

/* ---- Primitives ---- */

void css_serialize_number(float f, AuBuf *out)
{
    double v = f;
    if (v != v) {
        au_buf_puts(out, "NaN");
        return;
    }
    if (isinf(v)) {
        au_buf_puts(out, v < 0 ? "-infinity" : "infinity");
        return;
    }
    if (fabs(v) < 5e-7)
        v = 0;
    /* six significant digits (floats carry about seven), no exponent */
    char buf[64];
    int digits = fabs(v) >= 1 ? (int)floor(log10(fabs(v))) + 1 : 1;
    int decimals = 6 - digits;
    if (decimals < 0)
        decimals = 0;
    if (fabs(v) >= 1e15)
        snprintf(buf, sizeof buf, "%.0f", v);
    else
        snprintf(buf, sizeof buf, "%.*f", decimals, v);
    /* trim trailing zeros and the point */
    size_t n = strlen(buf);
    if (strchr(buf, '.')) {
        while (n && buf[n - 1] == '0')
            n--;
        if (n && buf[n - 1] == '.')
            n--;
    }
    buf[n] = 0;
    if (strcmp(buf, "-0") == 0) {
        buf[0] = '0';
        buf[1] = 0;
        n = 1;
    }
    au_buf_append(out, buf, n);
}

static void put_hex_escape(AuBuf *out, unsigned c)
{
    au_buf_printf(out, "\\%x ", c);
}

void css_serialize_ident(const char *s, size_t n, AuBuf *out)
{
    if (n == 1 && s[0] == '-') {
        au_buf_puts(out, "\\-");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0) {
            au_buf_puts(out, "\xEF\xBF\xBD");
        } else if (c < 0x20 || c == 0x7F) {
            put_hex_escape(out, c);
        } else if (c >= '0' && c <= '9' && (i == 0 || (i == 1 && s[0] == '-'))) {
            put_hex_escape(out, c);
        } else if (c >= 0x80 || c == '-' || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z')) {
            au_buf_putc(out, (char)c);
        } else {
            au_buf_putc(out, '\\');
            au_buf_putc(out, (char)c);
        }
    }
}

void css_serialize_string(const char *s, size_t n, AuBuf *out)
{
    au_buf_putc(out, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0)
            au_buf_puts(out, "\xEF\xBF\xBD");
        else if (c < 0x20 || c == 0x7F)
            put_hex_escape(out, c);
        else if (c == '"' || c == '\\') {
            au_buf_putc(out, '\\');
            au_buf_putc(out, (char)c);
        } else
            au_buf_putc(out, (char)c);
    }
    au_buf_putc(out, '"');
}

void au_css_color_serialize(AuColor c, AuBuf *out)
{
    if (c == AU_COLOR_CURRENT) {
        au_buf_puts(out, "currentcolor");
        return;
    }
    unsigned r = c >> 24, g = (c >> 16) & 255, b = (c >> 8) & 255, a = c & 255;
    if (a == 255) {
        au_buf_printf(out, "rgb(%u, %u, %u)", r, g, b);
        return;
    }
    /* the shortest alpha that maps back to the same byte */
    double av = a / 255.0;
    double two = floor(av * 100 + 0.5) / 100;
    float alpha = (unsigned)floor(two * 255 + 0.5) == a ? (float)two : (float)(floor(av * 1000 + 0.5) / 1000);
    au_buf_printf(out, "rgba(%u, %u, %u, ", r, g, b);
    css_serialize_number(alpha, out);
    au_buf_putc(out, ')');
}

/* ---- Value trees ---- */

static void ser(const AuCssValue *v, AuBuf *out, int depth);

static const char *calc_fn_name(int op)
{
    switch (op) {
    case AU_CALC_MIN: return "min";
    case AU_CALC_MAX: return "max";
    case AU_CALC_CLAMP: return "clamp";
    case AU_CALC_ROUND_NEAREST: case AU_CALC_ROUND_UP: case AU_CALC_ROUND_DOWN: case AU_CALC_ROUND_TO_ZERO:
        return "round";
    case AU_CALC_MOD: return "mod";
    case AU_CALC_REM: return "rem";
    case AU_CALC_ABS: return "abs";
    case AU_CALC_SIGN: return "sign";
    case AU_CALC_SIN: return "sin";
    case AU_CALC_COS: return "cos";
    case AU_CALC_TAN: return "tan";
    case AU_CALC_ASIN: return "asin";
    case AU_CALC_ACOS: return "acos";
    case AU_CALC_ATAN: return "atan";
    case AU_CALC_ATAN2: return "atan2";
    case AU_CALC_POW: return "pow";
    case AU_CALC_SQRT: return "sqrt";
    case AU_CALC_HYPOT: return "hypot";
    case AU_CALC_LOG: return "log";
    case AU_CALC_EXP: return "exp";
    default: return NULL;
    }
}

static void leaf(const AuCssValue *v, AuBuf *out)
{
    switch (v->type) {
    case AU_CSSV_NUMBER:
        css_serialize_number(v->u.num, out);
        break;
    case AU_CSSV_PERCENT:
        css_serialize_number(v->u.num, out);
        au_buf_putc(out, '%');
        break;
    case AU_CSSV_DIMENSION:
        css_serialize_number(v->u.num, out);
        au_buf_puts(out, css_unit_name(v->unit));
        break;
    default:
        break;
    }
}

/* A calc operand: sums inside products get parentheses. */
static void calc_expr(const AuCssValue *v, AuBuf *out, int depth, bool in_product)
{
    if (depth > 40)
        return;
    if (v->type != AU_CSSV_CALC) {
        if (v->type == AU_CSSV_KEYWORD)
            au_buf_puts(out, au_css_keyword_name((AuCssKeyword)v->kw));
        else
            leaf(v, out);
        return;
    }
    const AuCssValue *it = v->u.items;
    switch (v->unit) {
    case AU_CALC_SUM:
        if (v->n == 1) {
            calc_expr(&it[0], out, depth + 1, in_product);
            return;
        }
        if (in_product)
            au_buf_putc(out, '(');
        for (uint32_t i = 0; i < v->n; i++) {
            const AuCssValue *x = &it[i];
            if (i) {
                bool neg = x->type == AU_CSSV_CALC && x->unit == AU_CALC_NEGATE;
                bool negleaf = x->type != AU_CSSV_CALC && x->type != AU_CSSV_KEYWORD && x->u.num < 0;
                if (neg) {
                    au_buf_puts(out, " - ");
                    calc_expr(&x->u.items[0], out, depth + 1, false);
                    continue;
                }
                if (negleaf) {
                    AuCssValue y = *x;
                    y.u.num = -y.u.num;
                    au_buf_puts(out, " - ");
                    leaf(&y, out);
                    continue;
                }
                au_buf_puts(out, " + ");
            }
            calc_expr(x, out, depth + 1, false);
        }
        if (in_product)
            au_buf_putc(out, ')');
        return;
    case AU_CALC_PRODUCT:
        for (uint32_t i = 0; i < v->n; i++) {
            const AuCssValue *x = &it[i];
            if (i) {
                if (x->type == AU_CSSV_CALC && x->unit == AU_CALC_INVERT) {
                    au_buf_puts(out, " / ");
                    calc_expr(&x->u.items[0], out, depth + 1, true);
                    continue;
                }
                au_buf_puts(out, " * ");
            }
            calc_expr(x, out, depth + 1, true);
        }
        return;
    case AU_CALC_NEGATE:
        au_buf_puts(out, "-1 * ");
        calc_expr(&it[0], out, depth + 1, true);
        return;
    case AU_CALC_INVERT:
        au_buf_puts(out, "1 / ");
        calc_expr(&it[0], out, depth + 1, true);
        return;
    default: {
        const char *fn = calc_fn_name(v->unit);
        au_buf_puts(out, fn ? fn : "calc");
        au_buf_putc(out, '(');
        if (v->unit == AU_CALC_ROUND_UP)
            au_buf_puts(out, "up, ");
        else if (v->unit == AU_CALC_ROUND_DOWN)
            au_buf_puts(out, "down, ");
        else if (v->unit == AU_CALC_ROUND_TO_ZERO)
            au_buf_puts(out, "to-zero, ");
        for (uint32_t i = 0; i < v->n; i++) {
            if (i)
                au_buf_puts(out, ", ");
            calc_expr(&it[i], out, depth + 1, false);
        }
        au_buf_putc(out, ')');
        return;
    }
    }
}

static void calc_top(const AuCssValue *v, AuBuf *out, int depth)
{
    if (v->type == AU_CSSV_CALC && v->kw == CSS_CALC_CLAMP_WRAPPER && v->n == 2)
        v = &v->u.items[0];
    if (v->type != AU_CSSV_CALC) {
        au_buf_puts(out, "calc(");
        calc_expr(v, out, depth, false);
        au_buf_putc(out, ')');
        return;
    }
    if (calc_fn_name(v->unit)) {
        calc_expr(v, out, depth, false);
        return;
    }
    au_buf_puts(out, "calc(");
    calc_expr(v, out, depth, false);
    au_buf_putc(out, ')');
}

/* Function names are keywords (lowercase); transform functions are
 * written in their canonical mixed case. */
static const char *function_name(uint16_t kw)
{
    static const char *const mixed[] = {"translateX", "translateY", "translateZ", "translate3d", "scaleX",
                                        "scaleY", "scaleZ", "scale3d", "rotateX", "rotateY", "rotateZ",
                                        "rotate3d", "skewX", "skewY", "matrix3d"};
    const char *name = au_css_keyword_name((AuCssKeyword)kw);
    for (size_t i = 0; i < CSS_ARRAY_LEN(mixed); i++) {
        const char *a = mixed[i], *b = name;
        while (*a && (*a | 0x20) == *b) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return mixed[i];
    }
    return name;
}

static void ser(const AuCssValue *v, AuBuf *out, int depth)
{
    if (!v || depth > 64)
        return;
    switch (v->type) {
    case AU_CSSV_KEYWORD:
        au_buf_puts(out, au_css_keyword_name((AuCssKeyword)v->kw));
        break;
    case AU_CSSV_IDENT: {
        /* a family name of several identifiers keeps its spaces */
        const char *s = v->u.str, *e = s + v->n;
        while (s < e) {
            const char *sp = memchr(s, ' ', (size_t)(e - s));
            const char *w = sp ? sp : e;
            css_serialize_ident(s, (size_t)(w - s), out);
            if (!sp)
                break;
            au_buf_putc(out, ' ');
            s = sp + 1;
        }
        break;
    }
    case AU_CSSV_STRING:
        css_serialize_string(v->u.str, v->n, out);
        break;
    case AU_CSSV_URL:
        au_buf_puts(out, "url(");
        css_serialize_string(v->u.str, v->n, out);
        au_buf_putc(out, ')');
        break;
    case AU_CSSV_NUMBER:
    case AU_CSSV_PERCENT:
    case AU_CSSV_DIMENSION:
        if (v->n == 1)
            calc_top(v, out, depth);
        else
            leaf(v, out);
        break;
    case AU_CSSV_COLOR:
        au_css_color_serialize(v->u.rgba, out);
        break;
    case AU_CSSV_FUNCTION:
        if (v->unit == '[') {
            au_buf_putc(out, '[');
            for (uint32_t i = 0; i < v->n; i++) {
                if (i)
                    au_buf_putc(out, ' ');
                ser(&v->u.items[i], out, depth + 1);
            }
            au_buf_putc(out, ']');
            break;
        }
        au_buf_puts(out, function_name(v->kw));
        au_buf_putc(out, '(');
        for (uint32_t i = 0; i < v->n; i++) {
            if (i)
                au_buf_puts(out, ", ");
            ser(&v->u.items[i], out, depth + 1);
        }
        au_buf_putc(out, ')');
        break;
    case AU_CSSV_CALC:
        calc_top(v, out, depth);
        break;
    case AU_CSSV_LIST:
        for (uint32_t i = 0; i < v->n; i++) {
            if (i)
                au_buf_putc(out, ' ');
            ser(&v->u.items[i], out, depth + 1);
        }
        break;
    case AU_CSSV_COMMA_LIST:
        for (uint32_t i = 0; i < v->n; i++) {
            if (i)
                au_buf_puts(out, ", ");
            ser(&v->u.items[i], out, depth + 1);
        }
        break;
    case AU_CSSV_DELIM:
        au_buf_putc(out, (char)v->unit);
        break;
    default:
        break;
    }
}

void au_css_value_serialize(const AuCssValue *v, AuBuf *out) { ser(v, out, 0); }

/* Shadows are stored as LIST(x y blur spread color inset|none) for
 * box-shadow and LIST(x y blur color) for text-shadow; they are written
 * color first, as browsers do. */
static void ser_shadows(const AuCssValue *v, bool box, AuBuf *out)
{
    if (v->type == AU_CSSV_KEYWORD) {
        ser(v, out, 0);
        return;
    }
    uint32_t n;
    const AuCssValue *sh = v->type == AU_CSSV_COMMA_LIST ? v->u.items : v;
    n = v->type == AU_CSSV_COMMA_LIST ? v->n : 1;
    for (uint32_t i = 0; i < n; i++) {
        const AuCssValue *s = &sh[i];
        if (i)
            au_buf_puts(out, ", ");
        uint32_t want = box ? 6 : 4;
        if (s->type != AU_CSSV_LIST || s->n != want) {
            ser(s, out, 0);
            continue;
        }
        const AuCssValue *it = s->u.items;
        uint32_t ci = box ? 4 : 3;
        ser(&it[ci], out, 0);
        for (uint32_t k = 0; k < ci; k++) {
            au_buf_putc(out, ' ');
            ser(&it[k], out, 0);
        }
        if (box && css_is_kw(&it[5], AU_K_inset))
            au_buf_puts(out, " inset");
    }
}

static void ser_prop(uint16_t prop, const AuCssValue *v, AuBuf *out)
{
    switch (prop) {
    case AU_PROP_box_shadow:
    case AU_PROP_text_shadow:
        ser_shadows(v, prop == AU_PROP_box_shadow, out);
        return;
    case AU_PROP_transform_origin:
        /* the z offset is written only when not zero */
        if (v->type == AU_CSSV_LIST && v->n == 3 && v->u.items[2].type == AU_CSSV_DIMENSION &&
            v->u.items[2].u.num == 0) {
            ser(&v->u.items[0], out, 0);
            au_buf_putc(out, ' ');
            ser(&v->u.items[1], out, 0);
            return;
        }
        break;
    default:
        break;
    }
    ser(v, out, 0);
}

/* ---- Declarations ---- */

void css_serialize_decl_value(const AuCssDecl *d, AuBuf *out)
{
    switch (d->kind) {
    case DK_VALUE:
        ser_prop(d->prop, &d->u.v, out);
        break;
    case DK_BLOB:
        ser_prop(d->prop, &d->u.blob->value, out);
        break;
    case DK_WIDE:
        au_buf_puts(out, au_css_keyword_name((AuCssKeyword)d->u.v.kw));
        break;
    case DK_VAR:
    case DK_CUSTOM:
        css_tokens_serialize(&d->u.toks->tk, 0, d->u.toks->tk.n, out);
        break;
    default:
        break;
    }
}

static const char *decl_name(const AuCssDecl *d)
{
    if (d->prop == AU_PROP_CUSTOM)
        return au_atom_str(d->aux);
    return css_prop_name(d->prop);
}

void au_css_decl_block_serialize(const AuCssDeclBlock *b, AuBuf *out)
{
    if (!b)
        return;
    bool first = true;
    for (uint32_t i = 0; i < b->n; i++) {
        const AuCssDecl *d = &b->d[i];
        if (d->flags & DF_QUIRKS_ONLY)
            continue;
        const char *name = decl_name(d);
        if (d->kind == DK_VAR && d->aux) {
            /* longhands of a shorthand with var(): write the shorthand once */
            bool seen = false;
            for (uint32_t k = 0; k < i; k++)
                if (b->d[k].kind == DK_VAR && b->d[k].aux == d->aux && b->d[k].u.toks == d->u.toks)
                    seen = true;
            if (seen)
                continue;
            name = css_prop_name((uint16_t)d->aux);
        }
        if (!first)
            au_buf_putc(out, ' ');
        first = false;
        au_buf_puts(out, name);
        au_buf_puts(out, ": ");
        css_serialize_decl_value(d, out);
        if (d->flags & DF_IMPORTANT)
            au_buf_puts(out, " !important");
        au_buf_putc(out, ';');
    }
}


static const AuCssDecl *find_decl(const AuCssDeclBlock *b, uint16_t prop)
{
    for (uint32_t i = b->n; i-- > 0;)
        if (b->d[i].prop == prop && !(b->d[i].flags & DF_QUIRKS_ONLY))
            return &b->d[i];
    return NULL;
}

static bool is_box_shorthand(uint16_t sh)
{
    switch (sh) {
    case AU_PROP_margin: case AU_PROP_padding: case AU_PROP_inset: case AU_PROP_border_width:
    case AU_PROP_border_style: case AU_PROP_border_color: case AU_PROP_scroll_padding: case AU_PROP_scroll_margin:
        return true;
    default:
        return false;
    }
}

/* Writes 1 to 4 values for a top/right/bottom/left box. */
static void box_compress(char *const *v, AuBuf *out)
{
    int n = 4;
    if (strcmp(v[3], v[1]) == 0) {
        n = 3;
        if (strcmp(v[2], v[0]) == 0) {
            n = 2;
            if (strcmp(v[1], v[0]) == 0)
                n = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        if (i)
            au_buf_putc(out, ' ');
        au_buf_puts(out, v[i]);
    }
}

/* Serializes a shorthand from per-longhand strings. */
static void shorthand_join(uint16_t sh, const uint16_t *lh, char **vals, uint32_t n, AuBuf *out)
{
    if (is_box_shorthand(sh) && n == 4) {
        box_compress(vals, out);
        return;
    }
    if (sh == AU_PROP_container && n == 2) {
        au_buf_puts(out, vals[0]);
        if (strcmp(vals[1], "normal") != 0) {
            au_buf_puts(out, " / ");
            au_buf_puts(out, vals[1]);
        }
        return;
    }
    bool all_same = true;
    for (uint32_t i = 1; i < n; i++)
        if (strcmp(vals[i], vals[0]) != 0)
            all_same = false;
    if (all_same && n == 2) {
        au_buf_puts(out, vals[0]);
        return;
    }
    (void)lh;
    for (uint32_t i = 0; i < n; i++) {
        if (i)
            au_buf_putc(out, ' ');
        au_buf_puts(out, vals[i]);
    }
}

bool au_css_decl_block_get(const AuCssDeclBlock *b, AuAtom name, AuBuf *out)
{
    if (!b || !name)
        return false;
    const char *s = au_atom_str(name);
    size_t sl = au_atom_len(name);
    if (css_ident_is_custom_prop(s, sl)) {
        for (uint32_t i = b->n; i-- > 0;)
            if (b->d[i].prop == AU_PROP_CUSTOM && b->d[i].aux == name) {
                css_serialize_decl_value(&b->d[i], out);
                return true;
            }
        return false;
    }
    uint16_t p = (uint16_t)au_css_property_id(name);
    if (!p)
        return false;
    if (p < AU_PROP_LOGICAL_END) {
        const AuCssDecl *d = find_decl(b, p);
        if (!d)
            return false;
        css_serialize_decl_value(d, out);
        return true;
    }
    const uint16_t *lh = css_shorthand_longhands(p);
    if (!lh || !lh[0])
        return false;
    uint32_t n = 0;
    while (lh[n])
        n++;
    const AuCssDecl **ds = au_alloc(n * sizeof *ds);
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) {
        ds[i] = find_decl(b, lh[i]);
        if (!ds[i])
            ok = false;
    }
    if (ok) {
        /* all from one var() shorthand, or one CSS-wide keyword */
        bool same_var = true, same_wide = true;
        for (uint32_t i = 0; i < n; i++) {
            if (ds[i]->kind != DK_VAR || ds[i]->u.toks != ds[0]->u.toks)
                same_var = false;
            if (ds[i]->kind != DK_WIDE || ds[i]->u.v.kw != ds[0]->u.v.kw)
                same_wide = false;
            if ((ds[i]->flags & DF_IMPORTANT) != (ds[0]->flags & DF_IMPORTANT))
                ok = false;
        }
        if (!ok) {
        } else if (same_var || same_wide) {
            css_serialize_decl_value(ds[0], out);
        } else {
            bool any_var = false;
            for (uint32_t i = 0; i < n; i++)
                if (ds[i]->kind == DK_VAR || ds[i]->kind == DK_WIDE)
                    any_var = true;
            if (any_var) {
                ok = false;
            } else {
                char **vals = au_alloc(n * sizeof *vals);
                for (uint32_t i = 0; i < n; i++) {
                    AuBuf t;
                    au_buf_init(&t);
                    css_serialize_decl_value(ds[i], &t);
                    vals[i] = au_buf_take(&t, NULL);
                    if (!vals[i])
                        vals[i] = au_strndup("", 0);
                }
                shorthand_join(p, lh, vals, n, out);
                for (uint32_t i = 0; i < n; i++)
                    au_free(vals[i]);
                au_free(vals);
            }
        }
    }
    au_free(ds);
    return ok;
}

/* ---- Computed values ---- */

static void ser_len(const AuLen *l, AuBuf *out)
{
    switch (l->kind) {
    case AU_LEN_PX:
        css_serialize_number(l->u.px, out);
        au_buf_puts(out, "px");
        break;
    case AU_LEN_PCT:
        css_serialize_number(l->u.pct, out);
        au_buf_putc(out, '%');
        break;
    case AU_LEN_CALC:
        calc_top(&l->u.calc->value, out, 0);
        break;
    case AU_LEN_NUMBER:
        css_serialize_number(l->u.num, out);
        break;
    case AU_LEN_AUTO: au_buf_puts(out, "auto"); break;
    case AU_LEN_NONE: au_buf_puts(out, "none"); break;
    case AU_LEN_NORMAL: au_buf_puts(out, "normal"); break;
    case AU_LEN_MIN_CONTENT: au_buf_puts(out, "min-content"); break;
    case AU_LEN_MAX_CONTENT: au_buf_puts(out, "max-content"); break;
    case AU_LEN_FIT_CONTENT: au_buf_puts(out, "fit-content"); break;
    case AU_LEN_FIT_CONTENT_PX:
        au_buf_puts(out, "fit-content(");
        css_serialize_number(l->u.px, out);
        au_buf_puts(out, "px)");
        break;
    case AU_LEN_STRETCH: au_buf_puts(out, "stretch"); break;
    case AU_LEN_CONTENT: au_buf_puts(out, "content"); break;
    case AU_LEN_FROM_FONT: au_buf_puts(out, "from-font"); break;
    default: break;
    }
}

static void ser_px(float v, AuBuf *out)
{
    css_serialize_number(v, out);
    au_buf_puts(out, "px");
}

static const char *const display_inner_names[] = {
    "flow", "flow-root", "table", "flex", "grid", "ruby", "-webkit-box", "table-row-group",
    "table-header-group", "table-footer-group", "table-row", "table-cell", "table-column-group",
    "table-column", "table-caption", "ruby-base", "ruby-text", "ruby-base-container", "ruby-text-container",
    "contents", "none"};

static void ser_display(uint16_t d, AuBuf *out)
{
    int outer = au_display_outer(d), inner = au_display_inner(d);
    bool li = au_display_list_item(d);
    if (inner >= AU_DISPLAY_INNER_TABLE_ROW_GROUP || (size_t)inner >= CSS_ARRAY_LEN(display_inner_names)) {
        if ((size_t)inner < CSS_ARRAY_LEN(display_inner_names))
            au_buf_puts(out, display_inner_names[inner]);
        return;
    }
    if (li) {
        if (outer == AU_DISPLAY_OUTER_INLINE)
            au_buf_puts(out, "inline ");
        if (inner == AU_DISPLAY_INNER_FLOW_ROOT)
            au_buf_puts(out, "flow-root ");
        au_buf_puts(out, "list-item");
        return;
    }
    if (outer == AU_DISPLAY_OUTER_BLOCK) {
        static const char *const b[] = {"block", "flow-root", "table", "flex", "grid", "block ruby", "-webkit-box"};
        au_buf_puts(out, b[inner]);
    } else if (outer == AU_DISPLAY_OUTER_INLINE) {
        static const char *const i[] = {"inline", "inline-block", "inline-table", "inline-flex", "inline-grid",
                                        "ruby", "-webkit-inline-box"};
        au_buf_puts(out, i[inner]);
    } else {
        au_buf_puts(out, "run-in");
        if (inner != AU_DISPLAY_INNER_FLOW) {
            au_buf_putc(out, ' ');
            au_buf_puts(out, display_inner_names[inner]);
        }
    }
}

static void ser_align(uint16_t a, AuBuf *out)
{
    static const char *const names[] = {"auto", "normal", "stretch", "baseline", "last baseline", "center",
                                        "start", "end", "flex-start", "flex-end", "self-start", "self-end",
                                        "left", "right", "space-between", "space-around", "space-evenly",
                                        "legacy"};
    if (a & AU_ALIGN_SAFE)
        au_buf_puts(out, "safe ");
    if (a & AU_ALIGN_UNSAFE)
        au_buf_puts(out, "unsafe ");
    if (a & AU_ALIGN_LEGACY)
        au_buf_puts(out, "legacy ");
    unsigned v = a & 0xFF;
    au_buf_puts(out, v < CSS_ARRAY_LEN(names) ? names[v] : "normal");
}

static void ser_bits(uint16_t prop, uint8_t b, AuBuf *out)
{
    static const struct {
        uint16_t prop;
        uint8_t bit;
        const char *name;
    } t[] = {
        {AU_PROP_text_decoration_line, AU_TDL_UNDERLINE, "underline"},
        {AU_PROP_text_decoration_line, AU_TDL_OVERLINE, "overline"},
        {AU_PROP_text_decoration_line, AU_TDL_LINE_THROUGH, "line-through"},
        {AU_PROP_text_decoration_line, AU_TDL_BLINK, "blink"},
        {AU_PROP_text_decoration_line, AU_TDL_SPELLING_ERROR, "spelling-error"},
        {AU_PROP_text_decoration_line, AU_TDL_GRAMMAR_ERROR, "grammar-error"},
        {AU_PROP_contain, AU_CONTAIN_SIZE, "size"},
        {AU_PROP_contain, AU_CONTAIN_INLINE_SIZE, "inline-size"},
        {AU_PROP_contain, AU_CONTAIN_LAYOUT, "layout"},
        {AU_PROP_contain, AU_CONTAIN_STYLE, "style"},
        {AU_PROP_contain, AU_CONTAIN_PAINT, "paint"},
    };
    if (prop == AU_PROP_grid_auto_flow) {
        au_buf_puts(out, (b & AU_GRID_FLOW_COLUMN) ? "column" : "row");
        if (b & AU_GRID_FLOW_DENSE)
            au_buf_puts(out, " dense");
        return;
    }
    if (prop == AU_PROP_contain) {
        if (b == (AU_CONTAIN_SIZE | AU_CONTAIN_LAYOUT | AU_CONTAIN_PAINT | AU_CONTAIN_STYLE)) {
            au_buf_puts(out, "strict");
            return;
        }
        if (b == (AU_CONTAIN_LAYOUT | AU_CONTAIN_PAINT | AU_CONTAIN_STYLE)) {
            au_buf_puts(out, "content");
            return;
        }
    }
    if (!b) {
        au_buf_puts(out, "none");
        return;
    }
    bool first = true;
    for (size_t i = 0; i < CSS_ARRAY_LEN(t); i++)
        if (t[i].prop == prop && (b & t[i].bit)) {
            if (!first)
                au_buf_putc(out, ' ');
            au_buf_puts(out, t[i].name);
            first = false;
        }
}

static void ser_special(const AuStyle *s, uint16_t prop, const void *f, AuBuf *out)
{
    switch (prop) {
    case AU_PROP_font_size:
    case AU_PROP_border_top_width:
    case AU_PROP_border_right_width:
    case AU_PROP_border_bottom_width:
    case AU_PROP_border_left_width:
    case AU_PROP_outline_width:
        ser_px(*(const float *)f, out);
        break;
    case AU_PROP_font_weight:
        css_serialize_number(*(const uint16_t *)f, out);
        break;
    case AU_PROP_font_style: {
        uint8_t v = *(const uint8_t *)f;
        au_buf_puts(out, v == AU_FONT_STYLE_ITALIC ? "italic" : v == AU_FONT_STYLE_OBLIQUE ? "oblique" : "normal");
        break;
    }
    case AU_PROP_font_stretch:
        css_serialize_number(*(const float *)f, out);
        au_buf_putc(out, '%');
        break;
    case AU_PROP_line_height:
        ser_len((const AuLen *)f, out);
        break;
    case AU_PROP_border_spacing: {
        const float *b = f;
        ser_px(b[0], out);
        if (b[1] != b[0]) {
            au_buf_putc(out, ' ');
            ser_px(b[1], out);
        }
        break;
    }
    case AU_PROP_cursor: {
        const AuCssBlob *imgs = s->inhr->cursor_images;
        if (imgs) {
            ser(&imgs->value, out, 0);
            au_buf_puts(out, ", ");
        }
        uint16_t k = css_prop_kw_at(prop, *(const uint8_t *)f);
        au_buf_puts(out, au_css_keyword_name((AuCssKeyword)(k ? k : AU_K_auto_)));
        break;
    }
    case AU_PROP_display:
        ser_display(*(const uint16_t *)f, out);
        break;
    case AU_PROP_vertical_align: {
        static const char *const names[] = {"baseline", "sub", "super", "text-top", "text-bottom", "middle",
                                            "top", "bottom"};
        uint8_t v = *(const uint8_t *)f;
        if (v == AU_VALIGN_LENGTH)
            ser_len(&s->box->vertical_align_len, out);
        else
            au_buf_puts(out, v < CSS_ARRAY_LEN(names) ? names[v] : "baseline");
        break;
    }
    case AU_PROP_aspect_ratio: {
        const AuStyleBox *b = s->box;
        if (b->aspect_ratio_auto)
            au_buf_puts(out, b->aspect_ratio > 0 ? "auto " : "auto");
        if (b->aspect_ratio > 0) {
            css_serialize_number(b->aspect_ratio, out);
            au_buf_puts(out, " / 1");
        }
        break;
    }
    case AU_PROP_border_top_left_radius:
    case AU_PROP_border_top_right_radius:
    case AU_PROP_border_bottom_right_radius:
    case AU_PROP_border_bottom_left_radius: {
        const AuLen *l = f;
        AuBuf a, b;
        au_buf_init(&a);
        au_buf_init(&b);
        ser_len(&l[0], &a);
        ser_len(&l[1], &b);
        au_buf_append(out, a.data, a.len);
        if (a.len != b.len || memcmp(a.data, b.data, a.len) != 0) {
            au_buf_putc(out, ' ');
            au_buf_append(out, b.data, b.len);
        }
        au_buf_free(&a);
        au_buf_free(&b);
        break;
    }
    case AU_PROP_justify_content:
    case AU_PROP_align_content:
    case AU_PROP_justify_items:
    case AU_PROP_align_items:
    case AU_PROP_justify_self:
    case AU_PROP_align_self:
        ser_align(*(const uint16_t *)f, out);
        break;
    case AU_PROP_grid_auto_flow:
    case AU_PROP_contain:
    case AU_PROP_text_decoration_line:
        ser_bits(prop, *(const uint8_t *)f, out);
        break;
    default:
        break;
    }
}

void css_serialize_computed(const AuStyle *s, uint16_t prop, AuBuf *out)
{
    if (prop >= AU_PROP_LONGHAND_END) {
        if (prop < AU_PROP_LOGICAL_END) {
            uint16_t ph = css_logical_to_physical(prop, s->inh->writing_mode, s->inh->direction);
            css_serialize_computed(s, ph, out);
        }
        return;
    }
    const CssPropInfo *pi = &css_props[prop];
    const char *f = (const char *)css_style_group(s, pi->group) + pi->offset;
    switch (pi->type) {
    case T_KW: {
        uint8_t i = *(const uint8_t *)f;
        au_buf_puts(out, au_css_keyword_name((AuCssKeyword)(i < pi->nkws ? pi->kws[i] : 0)));
        break;
    }
    case T_LEN:
        ser_len((const AuLen *)f, out);
        break;
    case T_PX:
        ser_px(*(const float *)f, out);
        break;
    case T_COLOR:
        au_css_color_serialize(au_style_color(s, *(const AuColor *)f), out);
        break;
    case T_NUM:
        css_serialize_number(*(const float *)f, out);
        break;
    case T_INT: {
        int32_t v = *(const int32_t *)f;
        if (v == INT32_MIN)
            au_buf_puts(out, (pi->extra & LK_AUTO) ? "auto" : "none");
        else
            au_buf_printf(out, "%ld", (long)v);
        break;
    }
    case T_BLOB: {
        const AuCssBlob *b = *(AuCssBlob *const *)f;
        if (!b)
            au_buf_puts(out, pi->initial);
        else
            ser_prop(prop, &b->value, out);
        break;
    }
    case T_SPECIAL:
        ser_special(s, prop, f, out);
        break;
    default:
        break;
    }
}

bool au_style_serialize_property(const AuStyle *s, AuAtom name, AuBuf *out)
{
    if (!s || !name)
        return false;
    const char *str = au_atom_str(name);
    size_t sl = au_atom_len(name);
    if (css_ident_is_custom_prop(str, sl)) {
        size_t len;
        const char *v = au_style_custom_property(s, name, &len);
        if (v)
            au_buf_append(out, v, len);
        return true;
    }
    uint16_t p = (uint16_t)au_css_property_id(name);
    if (!p)
        return false;
    if (p < AU_PROP_LOGICAL_END) {
        css_serialize_computed(s, p, out);
        return true;
    }
    const uint16_t *lh = css_shorthand_longhands(p);
    if (!lh || !lh[0] || p == AU_PROP_all)
        return true; /* known, serialized as "" */
    uint32_t n = 0;
    while (lh[n])
        n++;
    char **vals = au_alloc(n * sizeof *vals);
    for (uint32_t i = 0; i < n; i++) {
        AuBuf t;
        au_buf_init(&t);
        css_serialize_computed(s, lh[i], &t);
        vals[i] = au_buf_take(&t, NULL);
        if (!vals[i])
            vals[i] = au_strndup("", 0);
    }
    shorthand_join(p, lh, vals, n, out);
    for (uint32_t i = 0; i < n; i++)
        au_free(vals[i]);
    au_free(vals);
    return true;
}
